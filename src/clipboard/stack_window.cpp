#include "clipboard/stack_window.h"
#include "ui/backdrop_compositor.h"
#include "ui/luma_glass.h"
#include "ui/text_renderer.h"
#include <d2d1_1helper.h>
#include <wincodec.h>
#include <dwmapi.h>
#include <windowsx.h>
#include <windows.ui.composition.interop.h>
#include <wrl/wrappers/corewrappers.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#pragma comment(lib, "dwmapi.lib")

namespace lumashot::clipboard {
namespace {
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Wrappers::HStringReference;
namespace wuc = ABI::Windows::UI::Composition;
using ABI::Windows::Foundation::Numerics::Vector2;
using ABI::Windows::Foundation::Numerics::Vector3;
using stack::Pose;

constexpr UINT_PTR HoverTimer = 1, LeaveTimer = 2, TrimTimer = 3, FlightTimer = 4;
constexpr unsigned FlightMs = 350;                                // = the panel LiquidTween
constexpr float FlightEase[4] = {.25f, 1.2f, .4f, 1.f};          // full.html --ease-spring
constexpr float FlightRadius = 10.f;                              // .item border-radius
// One-shot after a draw batch. WARP executes the batch when the compositor
// commits (+5 MiB transient in the 2026-10-17 measurement); trimming after that
// returns it (steady state about +1 MiB). A posted message would run first.
constexpr UINT TrimDelayMs = 250;
constexpr wchar_t VisualClass[] = L"LumaShotCardStackVisual";
constexpr wchar_t InputClass[] = L"LumaShotCardStackInput";
constexpr wchar_t GhostClass[] = L"LumaShotCardDragGhost";
// CSS blur radius -> Composition DropShadow.BlurRadius. Tuned by screenshot
// comparison with strip-concepts.html (stack appearance test).
constexpr float ShadowBlurScale = 1.f;

struct CompositionError {
    HRESULT hr;
    int line;
};
#define Must(expr) MustAt((expr), __LINE__)
void MustAt(HRESULT hr, int line) {
    if (FAILED(hr)) throw CompositionError{hr, line};
}
ABI::Windows::UI::Color Rgba(uint32_t rgb, float alpha) {
    ABI::Windows::UI::Color c{};
    c.A = static_cast<BYTE>(std::lround(std::clamp(alpha, 0.f, 1.f) * 255.f));
    c.R = static_cast<BYTE>((rgb >> 16) & 255);
    c.G = static_cast<BYTE>((rgb >> 8) & 255);
    c.B = static_cast<BYTE>(rgb & 255);
    return c;
}
template <class T, class U> ComPtr<T> As(const ComPtr<U>& from) {
    ComPtr<T> to;
    Must(from.As(&to));
    return to;
}

// ---- animation helpers: every property change is a DWM animation (1 ms when instant)
struct Motion {
    wuc::ICompositor* compositor{};
    ComPtr<wuc::ICompositionEasingFunction> ease;
    unsigned ms{};
};
void Start(IUnknown* target, const wchar_t* property, IUnknown* animation, unsigned ms) {
    ComPtr<wuc::IKeyFrameAnimation> key;
    Must(animation->QueryInterface(IID_PPV_ARGS(&key)));
    Must(key->put_Duration(ABI::Windows::Foundation::TimeSpan{static_cast<INT64>(std::max(1u, ms)) * 10000}));
    ComPtr<wuc::ICompositionAnimation> generic;
    Must(animation->QueryInterface(IID_PPV_ARGS(&generic)));
    ComPtr<wuc::ICompositionObject> object;
    Must(target->QueryInterface(IID_PPV_ARGS(&object)));
    Must(object->StartAnimation(HStringReference(property).Get(), generic.Get()));
}
// Without `from` the animation starts at the current value; instant moves pin
// both ends so no stale value is ever interpolated.
void AnimateScalar(IUnknown* target, const wchar_t* property, float value, const Motion& m, const float* from = nullptr) {
    ComPtr<wuc::IScalarKeyFrameAnimation> a;
    Must(m.compositor->CreateScalarKeyFrameAnimation(&a));
    if (from) Must(a->InsertKeyFrame(0.f, *from));
    else if (m.ms <= 1) Must(a->InsertKeyFrame(0.f, value));
    Must(a->InsertKeyFrameWithEasingFunction(1.f, value, m.ease.Get()));
    Start(target, property, a.Get(), m.ms);
}
// Without `from` the animation starts at the current value; instant moves pin
// both ends so no stale value is ever interpolated.
void AnimateVector2(IUnknown* target, const wchar_t* property, Vector2 value, const Motion& m, const Vector2* from = nullptr) {
    ComPtr<wuc::IVector2KeyFrameAnimation> a;
    Must(m.compositor->CreateVector2KeyFrameAnimation(&a));
    if (from) Must(a->InsertKeyFrame(0.f, *from));
    else if (m.ms <= 1) Must(a->InsertKeyFrame(0.f, value));
    Must(a->InsertKeyFrameWithEasingFunction(1.f, value, m.ease.Get()));
    Start(target, property, a.Get(), m.ms);
}
// Without `from` the animation starts at the current value; instant moves pin
// both ends so no stale value is ever interpolated.
void AnimateVector3(IUnknown* target, const wchar_t* property, Vector3 value, const Motion& m, const Vector3* from = nullptr) {
    ComPtr<wuc::IVector3KeyFrameAnimation> a;
    Must(m.compositor->CreateVector3KeyFrameAnimation(&a));
    if (from) Must(a->InsertKeyFrame(0.f, *from));
    else if (m.ms <= 1) Must(a->InsertKeyFrame(0.f, value));
    Must(a->InsertKeyFrameWithEasingFunction(1.f, value, m.ease.Get()));
    Start(target, property, a.Get(), m.ms);
}
// Without `from` the animation starts at the current value; instant moves pin
// both ends so no stale value is ever interpolated.
void AnimateColor(IUnknown* target, const wchar_t* property, ABI::Windows::UI::Color value, const Motion& m, const ABI::Windows::UI::Color* from = nullptr) {
    ComPtr<wuc::IColorKeyFrameAnimation> a;
    Must(m.compositor->CreateColorKeyFrameAnimation(&a));
    if (from) Must(a->InsertKeyFrame(0.f, *from));
    else if (m.ms <= 1) Must(a->InsertKeyFrame(0.f, value));
    Must(a->InsertKeyFrameWithEasingFunction(1.f, value, m.ease.Get()));
    Start(target, property, a.Get(), m.ms);
}

// ---- D2D drawing into a composition surface (DIP coordinates, DPI-aware).
class SurfaceDraw {
public:
    SurfaceDraw(wuc::ICompositionDrawingSurface* surface, float scale, float width, float height) {
        Must(surface->QueryInterface(IID_PPV_ARGS(&interop_)));
        POINT offset{};
        Must(interop_->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext), reinterpret_cast<void**>(dc_.GetAddressOf()), &offset));
        dc_->SetDpi(96.f * scale, 96.f * scale);
        dc_->SetTransform(D2D1::Matrix3x2F::Translation(static_cast<float>(offset.x) / scale, static_cast<float>(offset.y) / scale));
        // The surface may live in an atlas: clear only our own rectangle.
        dc_->PushAxisAlignedClip(D2D1::RectF(0, 0, width, height), D2D1_ANTIALIAS_MODE_ALIASED);
        dc_->Clear(D2D1::ColorF(0, 0.f));
        dc_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    }
    ~SurfaceDraw() {
        if (!dc_) return;
        dc_->PopAxisAlignedClip();
        interop_->EndDraw();
    }
    SurfaceDraw(const SurfaceDraw&) = delete;
    SurfaceDraw& operator=(const SurfaceDraw&) = delete;
    ID2D1DeviceContext* operator->() const { return dc_.Get(); }
    ID2D1DeviceContext* Get() const { return dc_.Get(); }
    ComPtr<ID2D1SolidColorBrush> Brush(uint32_t rgb, float alpha = 1) const {
        ComPtr<ID2D1SolidColorBrush> b;
        Must(dc_->CreateSolidColorBrush(D2D1::ColorF(rgb, alpha), &b));
        return b;
    }

private:
    ComPtr<ABI::Windows::UI::Composition::ICompositionDrawingSurfaceInterop> interop_;
    ComPtr<ID2D1DeviceContext> dc_;
};

struct CardVisual {
    uint64_t id{};
    ComPtr<wuc::IContainerVisual> root;  // pose: offset, size, rotation, scale, opacity
    ComPtr<wuc::ISpriteVisual> shadow_far, shadow_near, ring;
    ComPtr<wuc::IDropShadow> far_shadow, near_shadow;
    ComPtr<wuc::IContainerVisual> body;  // rounded clip
    ComPtr<wuc::ICompositionRoundedRectangleGeometry> body_shape;
    ComPtr<wuc::ISpriteVisual> backdrop, tint, thumb, label;
    ComPtr<wuc::ICompositionRoundedRectangleGeometry> thumb_shape;
    ComPtr<wuc::ICompositionColorBrush> tint_brush;
    ComPtr<wuc::ICompositionDrawingSurface> thumb_surface, label_surface;
    bool thumb_drawn{};
    bool thumb_image{};     // the tile shows real image pixels (not the placeholder)
    uint64_t thumb_key{};   // what the tile was drawn from; Sync redraws reused cards when it changes
};

}  // namespace

struct StackWindow::Impl {
    HWND owner{}, visual{}, input{};
    StackEvents events;
    std::shared_ptr<ui::BackdropCompositor> compositor;
    ComPtr<ui::BackdropCompositor::WindowTarget> target;
    ComPtr<wuc::IContainerVisual> root;
    ComPtr<wuc::ICompositionBrush> backdrop_brush, transparent_brush;
    ComPtr<wuc::ICompositionBrush> ring_brush, mask_mini, mask_row, mask_grip, grip_border_brush;
    ComPtr<wuc::ICompositionDrawingSurface> ring_surface, mini_mask_surface, row_mask_surface, grip_mask_surface, grip_border_surface;
    // Grip
    ComPtr<wuc::IContainerVisual> grip;
    ComPtr<wuc::ISpriteVisual> grip_shadow_far, grip_shadow_near, grip_backdrop, grip_tint, grip_solid, grip_border, grip_text;
    ComPtr<wuc::ICompositionColorBrush> grip_tint_brush, grip_solid_brush;
    ComPtr<wuc::ICompositionDrawingSurface> grip_text_surface;
    // "还有 N 条"
    ComPtr<wuc::ISpriteVisual> more;
    ComPtr<wuc::ICompositionDrawingSurface> more_surface;
    std::vector<CardVisual> cards;

    std::vector<StackItem> items;
    size_t total{};
    size_t first{};    // history index of items[0] (wheel paging)
    int wheel_rest{};  // partial wheel notches (precision touchpads)
    std::wstring badge_prefix{L"Alt+"};
    bool dark{}, glassy{true}, paused{}, motion{true}, fanned{}, shown{}, created{}, flying{};
    stack::Side side{stack::Side::Right};
    float scale{1};
    RECT window{}, work{};
    int hovered{-1};
    unsigned timers{}, draws{};
    TextRenderer text;
    ComPtr<IDWriteTextFormat> f_title, f_sub, f_idx, f_count, f_more, f_tile, f_tile_text, f_tile_code, f_tile_link;
    // Pointer
    bool tracking{}, pressed{}, moving{};
    int dragging{-1};  // card index during a drag-out
    stack::Side move_side{};  // placement before a grip drag, for Esc
    LONG move_top{};
    RECT move_work{};
    stack::Hit press_hit{};
    POINT press_point{};
    LONG grab_offset{};

    // ---------------------------------------------------------------- setup
    wuc::ICompositor* C() const { return compositor->Get(); }
    Motion Ease(const float (&bezier)[4], unsigned ms) const {
        Motion m{C(), {}, motion ? ms : 1u};
        ComPtr<wuc::ICubicBezierEasingFunction> e;
        Must(C()->CreateCubicBezierEasingFunction(Vector2{bezier[0], bezier[1]}, Vector2{bezier[2], bezier[3]}, &e));
        Must(e.As(&m.ease));
        return m;
    }
    Motion Instant() const { return Ease(stack::MoveEase, 1); }
    Motion Linear(unsigned ms) const {
        Motion m{C(), {}, motion ? ms : 1u};
        ComPtr<wuc::ILinearEasingFunction> e;
        Must(C()->CreateLinearEasingFunction(&e));
        Must(e.As(&m.ease));
        return m;
    }

    ComPtr<wuc::ISpriteVisual> Sprite(wuc::ICompositionBrush* brush, bool fill_parent) const {
        ComPtr<wuc::ISpriteVisual> s;
        Must(C()->CreateSpriteVisual(&s));
        if (brush) Must(s->put_Brush(brush));
        if (fill_parent) Must(As<wuc::IVisual2>(s)->put_RelativeSizeAdjustment(Vector2{1, 1}));
        return s;
    }
    static void Add(wuc::IContainerVisual* parent, IUnknown* child) {
        ComPtr<wuc::IVisualCollection> kids;
        Must(parent->get_Children(&kids));
        ComPtr<wuc::IVisual> v;
        Must(child->QueryInterface(IID_PPV_ARGS(&v)));
        Must(kids->InsertAtTop(v.Get()));
    }
    ComPtr<wuc::ICompositionColorBrush> ColorBrush(uint32_t rgb, float alpha) const {
        ComPtr<wuc::ICompositionColorBrush> b;
        Must(C()->CreateColorBrushWithColor(Rgba(rgb, alpha), &b));
        return b;
    }
    ComPtr<wuc::ICompositionDrawingSurface> Surface(float w, float h) const {
        ComPtr<wuc::ICompositionDrawingSurface> s;
        auto* graphics = compositor->Graphics();
        if (!graphics) throw CompositionError{E_FAIL, __LINE__};
        Must(graphics->CreateDrawingSurface(
            ABI::Windows::Foundation::Size{std::ceil(w * scale), std::ceil(h * scale)},
            ABI::Windows::Graphics::DirectX::DirectXPixelFormat_B8G8R8A8UIntNormalized,
            ABI::Windows::Graphics::DirectX::DirectXAlphaMode_Premultiplied, &s));
        return s;
    }
    ComPtr<wuc::ICompositionSurfaceBrush> SurfaceBrush(wuc::ICompositionDrawingSurface* surface, bool cover) const {
        ComPtr<wuc::ICompositionSurfaceBrush> b;
        ComPtr<wuc::ICompositionSurface> s;
        Must(surface->QueryInterface(IID_PPV_ARGS(&s)));
        Must(C()->CreateSurfaceBrushWithSurface(s.Get(), &b));
        Must(b->put_Stretch(cover ? wuc::CompositionStretch_UniformToFill : wuc::CompositionStretch_Fill));
        Must(b->put_HorizontalAlignmentRatio(.5f));
        Must(b->put_VerticalAlignmentRatio(.5f));
        return b;
    }
    ComPtr<wuc::ICompositionBrush> NineGrid(wuc::ICompositionDrawingSurface* surface, float inset, bool hollow) const {
        ComPtr<wuc::ICompositionNineGridBrush> grid;
        Must(As<wuc::ICompositor2>(ComPtr<wuc::ICompositor>(C()))->CreateNineGridBrush(&grid));
        Must(grid->put_Source(As<wuc::ICompositionBrush>(SurfaceBrush(surface, false)).Get()));
        Must(grid->SetInsets(inset * scale));
        Must(grid->put_IsCenterHollow(hollow));
        return As<wuc::ICompositionBrush>(grid);
    }
    ComPtr<wuc::ICompositionRoundedRectangleGeometry> RoundRect(float radius) const {
        ComPtr<wuc::ICompositionRoundedRectangleGeometry> g;
        Must(As<wuc::ICompositor5>(ComPtr<wuc::ICompositor>(C()))->CreateRoundedRectangleGeometry(&g));
        Must(g->put_CornerRadius(Vector2{radius * scale, radius * scale}));
        return g;
    }
    void Clip(wuc::IContainerVisual* clipped, wuc::ICompositionRoundedRectangleGeometry* shape) const {
        ComPtr<wuc::ICompositionGeometry> geometry;
        Must(shape->QueryInterface(IID_PPV_ARGS(&geometry)));
        ComPtr<wuc::ICompositionGeometricClip> clip;
        Must(As<wuc::ICompositor6>(ComPtr<wuc::ICompositor>(C()))->CreateGeometricClipWithGeometry(geometry.Get(), &clip));
        ComPtr<wuc::IVisual> v;
        Must(clipped->QueryInterface(IID_PPV_ARGS(&v)));
        Must(v->put_Clip(As<wuc::ICompositionClip>(clip).Get()));
    }
    void Clip(wuc::ISpriteVisual* clipped, wuc::ICompositionRoundedRectangleGeometry* shape) const {
        ComPtr<wuc::IContainerVisual> c;
        Must(clipped->QueryInterface(IID_PPV_ARGS(&c)));
        Clip(c.Get(), shape);
    }
    // Shadow caster: transparent sprite the size of its parent, rounded mask.
    ComPtr<wuc::ISpriteVisual> ShadowSprite(wuc::ICompositionBrush* mask, ComPtr<wuc::IDropShadow>& shadow) const {
        auto sprite = Sprite(transparent_brush.Get(), true);
        Must(As<wuc::ICompositor2>(ComPtr<wuc::ICompositor>(C()))->CreateDropShadow(&shadow));
        Must(shadow->put_Mask(mask));
        Must(As<wuc::ISpriteVisual2>(sprite)->put_Shadow(As<wuc::ICompositionShadow>(shadow).Get()));
        return sprite;
    }

    // Static artwork: ring (box-shadow 0 0 0 1.5px white .9), rounded masks, grip border.
    void DrawArtwork() {
        const auto mask = [&](ComPtr<wuc::ICompositionDrawingSurface>& surface, float radius) {
            const float size = 2 * radius + 2;
            surface = Surface(size, size);
            SurfaceDraw d(surface.Get(), scale, size, size);
            d->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0, 0, size, size), radius, radius), d.Brush(0xffffff).Get());
            ++draws;
        };
        mask(mini_mask_surface, stack::MiniRadius);
        mask(row_mask_surface, stack::RowRadius);
        mask(grip_mask_surface, stack::GripRadius);
        mask_mini = NineGrid(mini_mask_surface.Get(), stack::MiniRadius, false);
        mask_row = NineGrid(row_mask_surface.Get(), stack::RowRadius, false);
        mask_grip = NineGrid(grip_mask_surface.Get(), stack::GripRadius, false);
        {
            constexpr float spread = 1.5f, r = stack::MiniRadius + spread, size = 2 * r + 2;
            ring_surface = Surface(size, size);
            SurfaceDraw d(ring_surface.Get(), scale, size, size);
            d->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(spread / 2, spread / 2, size - spread / 2, size - spread / 2), r - spread / 2, r - spread / 2),
                                    d.Brush(0xffffff, .9f).Get(), spread);
            ++draws;
            ring_brush = NineGrid(ring_surface.Get(), r, true);
        }
        DrawGripBorder();
    }
    void DrawGripBorder() {
        // 1px var(--gborder); no border on the edge side (drawn beyond the window edge).
        constexpr float r = stack::GripRadius, size = 2 * r + 2;
        grip_border_surface = Surface(size, size);
        {
            SurfaceDraw d(grip_border_surface.Get(), scale, size, size);
            const uint32_t c = glass::Pick(glass::GlassBorder, dark);
            const float a = dark ? glass::GlassBorderAlphaDark : glass::GlassBorderAlphaLight;
            d->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(.5f, .5f, size - .5f, size - .5f), r - .5f, r - .5f), d.Brush(c, a).Get(), 1.f);
            ++draws;
        }
        grip_border_brush = NineGrid(grip_border_surface.Get(), r, true);
        if (grip_border) Must(grip_border->put_Brush(grip_border_brush.Get()));
    }

    IDWriteTextFormat* MakeFormat(ComPtr<IDWriteTextFormat>& f, const wchar_t* family, float size, DWRITE_FONT_WEIGHT weight) {
        if (!f) {
            Must(compositor->Writer()->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                                        size, L"zh-CN", &f));
            f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        return f.Get();
    }
    void Fonts() {
        if (!compositor->Writer()) throw CompositionError{E_FAIL, __LINE__};
        constexpr wchar_t ui[] = L"Microsoft YaHei UI";
        MakeFormat(f_title, ui, 12.f, DWRITE_FONT_WEIGHT_BOLD);
        MakeFormat(f_sub, ui, 10.5f, DWRITE_FONT_WEIGHT_NORMAL);
        MakeFormat(f_idx, ui, 10.f, DWRITE_FONT_WEIGHT_BOLD);
        MakeFormat(f_count, ui, 11.f, DWRITE_FONT_WEIGHT_BOLD);
        MakeFormat(f_more, ui, 11.f, DWRITE_FONT_WEIGHT_NORMAL);
        MakeFormat(f_tile, ui, 11.f, DWRITE_FONT_WEIGHT_EXTRA_BOLD);
        MakeFormat(f_tile_text, L"Georgia", 13.f, DWRITE_FONT_WEIGHT_NORMAL);
        MakeFormat(f_tile_code, L"Consolas", 11.f, DWRITE_FONT_WEIGHT_EXTRA_BOLD);
        MakeFormat(f_tile_link, ui, 15.f, DWRITE_FONT_WEIGHT_BOLD);
    }
    // One line, ellipsis, through TextRenderer (LumaText) as AGENTS.md requires.
    float Line(ID2D1DeviceContext* dc, const std::wstring& s, IDWriteTextFormat* format, D2D1_RECT_F r, uint32_t rgb, float alpha,
               DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING) {
        if (s.empty() || r.right <= r.left) return 0;
        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(compositor->Writer()->CreateTextLayout(s.data(), static_cast<UINT32>(s.size()), format, r.right - r.left, r.bottom - r.top, &layout)))
            return 0;
        layout->SetTextAlignment(align);
        ComPtr<IDWriteInlineObject> ellipsis;
        if (SUCCEEDED(compositor->Writer()->CreateEllipsisTrimmingSign(format, &ellipsis))) {
            const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            layout->SetTrimming(&trimming, ellipsis.Get());
        }
        DWRITE_TEXT_METRICS metrics{};
        layout->GetMetrics(&metrics);
        text.DrawLayout(dc, compositor->Writer(), layout.Get(), D2D1::Point2F(r.left, r.top), r, D2D1::ColorF(rgb, alpha));
        return metrics.widthIncludingTrailingWhitespace;
    }
    float Measure(const std::wstring& s, IDWriteTextFormat* format) {
        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(compositor->Writer()->CreateTextLayout(s.data(), static_cast<UINT32>(s.size()), format, 1000.f, 100.f, &layout))) return 0;
        DWRITE_TEXT_METRICS metrics{};
        layout->GetMetrics(&metrics);
        return metrics.widthIncludingTrailingWhitespace;
    }

    static bool HasThumbPixels(const StackItem& item) {
        return item.tile == StackTile::Image && !item.thumb.empty() && item.thumb_width > 0 && item.thumb_height > 0 &&
            item.thumb.size() >= static_cast<size_t>(item.thumb_width) * static_cast<size_t>(item.thumb_height);
    }
    // Everything PaintThumb reads. Image thumbnails load asynchronously after their card exists,
    // and 卡片显示内容预览 can conceal/reveal them, so the same id can need a different tile.
    static uint64_t ThumbKey(const StackItem& item) {
        uint64_t h = 1469598103934665603ull;
        const auto mix = [&](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
        mix(static_cast<uint64_t>(item.tile)); mix(item.accent);
        for (wchar_t c : item.glyph) mix(static_cast<uint64_t>(c));
        mix(static_cast<uint64_t>(static_cast<uint32_t>(item.thumb_width))); mix(static_cast<uint64_t>(static_cast<uint32_t>(item.thumb_height)));
        if (HasThumbPixels(item)) { const size_t step = std::max<size_t>(1, item.thumb.size() / 64); for (size_t i = 0; i < item.thumb.size(); i += step) mix(item.thumb[i]); }
        return h;
    }
    // Tile: .thumb + .th-* rules, drawn at mini size (52x40) and cover-cropped in rows.
    void DrawThumb(CardVisual& card, const StackItem& item) {
        if (!card.thumb_surface) card.thumb_surface = Surface(stack::MiniW, stack::MiniH);
        SurfaceDraw d(card.thumb_surface.Get(), scale, stack::MiniW, stack::MiniH);
        PaintThumb(d.Get(), item);
        ++draws;
        card.thumb_drawn = true;
        card.thumb_image = HasThumbPixels(item);
        card.thumb_key = ThumbKey(item);
    }
    static ComPtr<ID2D1SolidColorBrush> SolidBrush(ID2D1RenderTarget* rt, uint32_t rgb, float alpha = 1) {
        ComPtr<ID2D1SolidColorBrush> b;
        Must(rt->CreateSolidColorBrush(D2D1::ColorF(rgb, alpha), &b));
        return b;
    }
    // Tile in mini-card coordinates (52x40); used by surfaces and the drag image.
    void PaintThumb(ID2D1DeviceContext* dc, const StackItem& item) {
        constexpr float w = stack::MiniW, h = stack::MiniH;
        const D2D1_RECT_F r = D2D1::RectF(0, 0, w, h);
        ID2D1RenderTarget* rt = dc;  // classic overloads (no ID2D1DeviceContext hiding)
        switch (item.tile) {
        case StackTile::Image:
            if (!item.thumb.empty() && item.thumb_width > 0 && item.thumb_height > 0 &&
                item.thumb.size() >= static_cast<size_t>(item.thumb_width) * static_cast<size_t>(item.thumb_height)) {
                ComPtr<ID2D1Bitmap> bitmap;
                const D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
                if (SUCCEEDED(rt->CreateBitmap(D2D1::SizeU(static_cast<UINT32>(item.thumb_width), static_cast<UINT32>(item.thumb_height)), item.thumb.data(),
                                               static_cast<UINT32>(item.thumb_width) * 4, props, &bitmap))) {
                    // object-fit: cover
                    const float sw = static_cast<float>(item.thumb_width), sh = static_cast<float>(item.thumb_height);
                    const float k = std::max(w / sw, h / sh), cw = w / k, ch = h / k;
                    const D2D1_RECT_F src = D2D1::RectF((sw - cw) / 2, (sh - ch) / 2, (sw + cw) / 2, (sh + ch) / 2);
                    rt->DrawBitmap(bitmap.Get(), r, 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, src);
                    break;
                }
            }
            {  // .th-image placeholder gradient
                const D2D1_GRADIENT_STOP stops[] = {{0.f, D2D1::ColorF(0xffb37a)}, {.45f, D2D1::ColorF(0xff8a7a)}, {.70f, D2D1::ColorF(0x5b5fa8)}, {1.f, D2D1::ColorF(0x26365f)}};
                ComPtr<ID2D1GradientStopCollection> collection;
                ComPtr<ID2D1LinearGradientBrush> brush;
                if (SUCCEEDED(rt->CreateGradientStopCollection(stops, 4, &collection)) &&
                    SUCCEEDED(rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(0, h)), collection.Get(), &brush)))
                    dc->FillRectangle(r, brush.Get());
            }
            break;
        case StackTile::Text:
            dc->FillRectangle(r, SolidBrush(dc, 0xffffff).Get());
            Line(dc, item.glyph.empty() ? L"Aa" : item.glyph, f_tile_text.Get(), r, 0x1b2430, 1, DWRITE_TEXT_ALIGNMENT_CENTER);
            break;
        case StackTile::Code:
            dc->FillRectangle(r, SolidBrush(dc, 0x1b2130).Get());
            Line(dc, item.glyph.empty() ? L"</>" : item.glyph, f_tile_code.Get(), r, 0x7cb4ff, 1, DWRITE_TEXT_ALIGNMENT_CENTER);
            break;
        case StackTile::Link:
            dc->FillRectangle(r, SolidBrush(dc, item.accent ? item.accent : glass::Accent).Get());
            Line(dc, item.glyph, f_tile_link.Get(), r, 0xffffff, 1, DWRITE_TEXT_ALIGNMENT_CENTER);
            break;
        case StackTile::Color:
            dc->FillRectangle(r, SolidBrush(dc, item.accent).Get());
            break;
        case StackTile::File:
            dc->FillRectangle(r, SolidBrush(dc, dark ? 0x2a3342 : 0xeef2f8).Get());
            Line(dc, item.glyph, f_tile.Get(), r, item.accent ? item.accent : glass::Accent, 1, DWRITE_TEXT_ALIGNMENT_CENTER);
            break;
        }
        // box-shadow: inset 0 0 0 1px rgba(0,0,0,.08)
        dc->DrawRectangle(D2D1::RectF(.5f, .5f, w - .5f, h - .5f), SolidBrush(dc, 0x000000, .08f).Get(), 1.f);
    }
    // .lab (title 12px bold ink, subtitle 10.5px ink3) + .idx "Alt+N" badge.
    void DrawLabel(CardVisual& card, const StackItem& item, size_t index) {
        const Pose p = stack::LabelPose();
        if (!card.label_surface) card.label_surface = Surface(p.w, p.h);
        SurfaceDraw d(card.label_surface.Get(), scale, p.w, p.h);
        PaintLabel(d.Get(), item, index);
        ++draws;
    }
    void PaintLabel(ID2D1DeviceContext* dc, const StackItem& item, size_t index) {
        const Pose p = stack::LabelPose();
        // Alt+N names the newest entries; paged rows show their plain place in history.
        const std::wstring idx = (first == 0 ? badge_prefix : std::wstring{}) + std::to_wstring(first + index + 1);
        const float idx_w = std::ceil(Measure(idx, f_idx.Get())) + 8 + 2;  // padding 0 4px + 1px border
        const float idx_right = p.w - 8, idx_left = idx_right - idx_w;
        const uint32_t ink = glass::Pick(glass::StackInk, dark), ink3 = glass::Pick(glass::StackInk3, dark);
        const D2D1_RECT_F badge = D2D1::RectF(idx_left + .5f, p.h / 2 - 7.5f, idx_right - .5f, p.h / 2 + 7.5f);
        dc->DrawRoundedRectangle(D2D1::RoundedRect(badge, 4, 4), SolidBrush(dc, glass::Pick(glass::Edge, dark), dark ? glass::EdgeAlphaDark : glass::EdgeAlphaLight).Get(), 1.f);
        Line(dc, idx, f_idx.Get(), D2D1::RectF(idx_left, badge.top, idx_right, badge.bottom), ink3, 1, DWRITE_TEXT_ALIGNMENT_CENTER);
        const float text_right = idx_left - stack::LabelGap;
        Line(dc, item.title, f_title.Get(), D2D1::RectF(0, 4, text_right, 21), ink, 1);
        Line(dc, item.subtitle, f_sub.Get(), D2D1::RectF(0, 21, text_right, 36), ink3, 1);
    }
    void DrawGripText() {
        constexpr float w = stack::GripW, h = stack::GripH;
        if (!grip_text_surface) grip_text_surface = Surface(w, h);
        SurfaceDraw d(grip_text_surface.Get(), scale, w, h);
        auto* dc = d.Get();
        const std::wstring count = std::to_wstring(total);
        if (paused) {
            // Pause badge: two bars before the count, muted ink (no glyph dependency).
            const uint32_t c = glass::Pick(glass::StackInk3, dark);
            const float tw = Measure(count, f_count.Get()), x = (w - tw - 6) / 2;
            dc->FillRectangle(D2D1::RectF(x, 8, x + 1.5f, 14), d.Brush(c).Get());
            dc->FillRectangle(D2D1::RectF(x + 3, 8, x + 4.5f, 14), d.Brush(c).Get());
            Line(dc, count, f_count.Get(), D2D1::RectF(x + 6, 0, w, h), c, 1);
        } else {
            Line(dc, count, f_count.Get(), D2D1::RectF(0, 0, w, h), glass::Accent, 1, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        ++draws;
    }
    // Under the open stack: how many entries are left, or which slice the wheel shows.
    std::wstring MoreText() const {
        const size_t visible_count = std::min(items.size(), static_cast<size_t>(stack::MaxCards));
        if (first > 0 && visible_count > 0)
            return L"第 " + std::to_wstring(first + 1) + L"–" + std::to_wstring(first + visible_count) + L" 条，共 " + std::to_wstring(total) + L" 条";
        const size_t rest = total > visible_count ? total - visible_count : 0;
        return rest > 0 ? L"还有 " + std::to_wstring(rest) + L" 条 · 滚轮翻看 · 点数字打开" : std::wstring{};
    }
    // .more: 11px white, text-shadow 0 1px 2px #000a, aligned to the edge side.
    void DrawMore() {
        constexpr float w = stack::MoreW, h = stack::MoreH;
        if (!more_surface) more_surface = Surface(w, h);
        SurfaceDraw d(more_surface.Get(), scale, w, h);
        auto* dc = d.Get();
        const std::wstring s = MoreText();
        if (!s.empty()) {
            const auto align = side == stack::Side::Right ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING;
            // Soft shadow approximated by a few offset passes (2px blur, 1px down).
            constexpr float offsets[][2] = {{0, 1}, {-.5f, 1}, {.5f, 1}, {0, 1.5f}, {0, .5f}};
            for (const auto& o : offsets) Line(dc, s, f_more.Get(), D2D1::RectF(o[0], o[1], w + o[0], h + o[1]), 0x000000, .67f / 2.5f, align);
            Line(dc, s, f_more.Get(), D2D1::RectF(0, 0, w, h), 0xffffff, 1, align);
        }
        ++draws;
    }

    // ---- Drag ghost (card drag-out, strip-concepts.html concept D).
    // .ghost.gcard follows the pointer at (26, 22) inside the card, rotate(-3deg)
    // scale(1.03); [data-z=stack] rotate(0) scale(.92) opacity .8; [data-z=desk]
    // 2 px accent outline; transform transition .15s ease. .cursorhint is a
    // second click-through window at (+16, +18), flipped left at the screen edge.
    // Both exist only while a drag runs; the 15 ms timer only during a transition.
    static constexpr float GhostW = 252, GhostH = 92, GhostGrabX = 26, GhostGrabY = 22, HintH = 19;
    static constexpr ULONGLONG GhostMoveMs = 150, HintFadeMs = 200;
    struct Canvas {
        ComPtr<IWICBitmap> bitmap;
        ComPtr<ID2D1DeviceContext> dc;
        UINT w{}, h{};
        HDC mem{};
        HBITMAP dib{};
        HGDIOBJ old{};
        void* bits{};
        void Release() {
            dc.Reset();
            bitmap.Reset();
            if (mem) {
                if (old) SelectObject(mem, old);
                DeleteDC(mem);
            }
            if (dib) DeleteObject(dib);
            mem = nullptr;
            dib = nullptr;
            old = nullptr;
            bits = nullptr;
            w = h = 0;
        }
    };
    ComPtr<IWICImagingFactory> ghost_wic;
    ComPtr<ID2D1Factory1> ghost_factory;
    ComPtr<IDWriteTextFormat> f_hint;
    Canvas ghost_card, ghost_hint;
    HWND ghost_window{}, hint_window{};
    StackItem ghost_item;
    GhostPose ghost_from, ghost_to, ghost_now;
    ULONGLONG ghost_start{}, hint_start{};
    POINT ghost_cursor{};
    std::wstring ghost_text;
    bool ghost_animating{}, hint_fading{};
    float ghost_scale{1};

    static float Ease(float x) {  // CSS "ease" = cubic-bezier(.25, .1, .25, 1)
        x = std::clamp(x, 0.f, 1.f);
        constexpr float x1 = .25f, y1 = .1f, x2 = .25f, y2 = 1.f;
        const auto bx = [&](float t) { const float u = 1 - t; return 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t; };
        const auto by = [&](float t) { const float u = 1 - t; return 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t; };
        float lo = 0, hi = 1, t = x;
        for (int i = 0; i < 24; ++i) {
            t = (lo + hi) / 2;
            (bx(t) < x ? lo : hi) = t;
        }
        return by(t);
    }
    bool GhostFactories() {
        if (!ghost_wic && FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&ghost_wic)))) return false;
        if (!ghost_factory && FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, ghost_factory.GetAddressOf()))) return false;
        return compositor && compositor->Writer() && f_title;
    }
    bool MakeCanvas(Canvas& c, float w_dip, float h_dip) {
        const UINT w = static_cast<UINT>(std::ceil(w_dip * ghost_scale)), h = static_cast<UINT>(std::ceil(h_dip * ghost_scale));
        if (c.dc && c.w == w && c.h == h) return true;
        c.Release();
        if (!GhostFactories() || !w || !h) return false;
        ComPtr<ID2D1RenderTarget> rt;
        const auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                                                        96.f * ghost_scale, 96.f * ghost_scale);
        if (FAILED(ghost_wic->CreateBitmap(w, h, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &c.bitmap)) ||
            FAILED(ghost_factory->CreateWicBitmapRenderTarget(c.bitmap.Get(), props, &rt)) || FAILED(rt.As(&c.dc))) {
            c.Release();
            return false;
        }
        c.dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        c.w = w;
        c.h = h;
        return true;
    }
    // Copies the rendered pixels into a DIB for UpdateLayeredWindow (created on first use).
    bool CanvasPixels(Canvas& c, uint32_t* out) {
        return SUCCEEDED(c.bitmap->CopyPixels(nullptr, c.w * 4, c.w * c.h * 4, reinterpret_cast<BYTE*>(out)));
    }
    bool PresentCanvas(Canvas& c, HWND layered, POINT at, float opacity) {
        if (!c.dib) {
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(info.bmiHeader);
            info.bmiHeader.biWidth = static_cast<LONG>(c.w);
            info.bmiHeader.biHeight = -static_cast<LONG>(c.h);
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            c.dib = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &c.bits, nullptr, 0);
            c.mem = CreateCompatibleDC(nullptr);
            if (!c.dib || !c.mem || !c.bits) return false;
            c.old = SelectObject(c.mem, c.dib);
        }
        if (!CanvasPixels(c, static_cast<uint32_t*>(c.bits))) return false;
        GdiFlush();
        SIZE size{static_cast<LONG>(c.w), static_cast<LONG>(c.h)};
        POINT source{};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, static_cast<BYTE>(std::lround(std::clamp(opacity, 0.f, 1.f) * 255)), AC_SRC_ALPHA};
        return UpdateLayeredWindow(layered, nullptr, &at, &size, c.mem, &source, 0, &blend, ULW_ALPHA) != FALSE;
    }
    // Ghost label: title + kind only (".gl b" / ".gl span"), no index badge.
    void PaintGhostLabel(ID2D1DeviceContext* dc, const StackItem& item) {
        const Pose p = stack::LabelPose();
        const uint32_t ink = glass::Pick(glass::StackInk, dark), ink3 = glass::Pick(glass::StackInk3, dark);
        const std::wstring kind = item.subtitle.substr(0, item.subtitle.find(L" · "));
        Line(dc, item.title, f_title.Get(), D2D1::RectF(0, 4, p.w - 6, 21), ink, 1);
        Line(dc, kind, f_sub.Get(), D2D1::RectF(0, 21, p.w - 6, 36), ink3, 1);
    }
    void PaintGhostCard(Canvas& c, const StackItem& item, const GhostPose& pose) {
        auto* dc = c.dc.Get();
        const D2D1_RECT_F card = D2D1::RectF(0, 0, stack::RowW, stack::RowH);
        const auto base = D2D1::Matrix3x2F::Translation(-stack::RowW / 2, -stack::RowH / 2) * D2D1::Matrix3x2F::Scale(pose.scale, pose.scale) *
                          D2D1::Matrix3x2F::Rotation(pose.rotation) * D2D1::Matrix3x2F::Translation(GhostW / 2, GhostH / 2);
        dc->BeginDraw();
        try {
            dc->SetTransform(D2D1::Matrix3x2F::Identity());
            dc->Clear(D2D1::ColorF(0, 0.f));
            dc->SetTransform(base);
            // var(--shadow) approximated with widening translucent rounded rects.
            for (int k = 6; k >= 1; --k) {
                const float g = static_cast<float>(k) * 1.6f;
                dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(-g, -g + 5, stack::RowW + g, stack::RowH + g + 5), stack::RowRadius + g, stack::RowRadius + g),
                                         SolidBrush(dc, glass::Pick(glass::StackShadow, dark), .035f).Get());
            }
            dc->FillRoundedRectangle(D2D1::RoundedRect(card, stack::RowRadius, stack::RowRadius), SolidBrush(dc, glass::Pick(glass::StackSolid, dark), .97f).Get());
            dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(.5f, .5f, stack::RowW - .5f, stack::RowH - .5f), stack::RowRadius, stack::RowRadius),
                                     SolidBrush(dc, glass::Pick(glass::Edge, dark), dark ? glass::EdgeAlphaDark : glass::EdgeAlphaLight).Get(), 1.f);
            // Tile: the 52x40 mini artwork, centre-cropped into the 46x40 row tile (object-fit: cover).
            const Pose t = stack::ThumbPose(true);
            ComPtr<ID2D1RoundedRectangleGeometry> mask;
            if (SUCCEEDED(ghost_factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(D2D1::RectF(t.x, t.y, t.x + t.w, t.y + t.h), t.radius, t.radius), &mask))) {
                dc->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), mask.Get()), nullptr);
                dc->SetTransform(D2D1::Matrix3x2F::Translation(t.x - (stack::MiniW - t.w) / 2, t.y) * base);
                PaintThumb(dc, item);
                dc->SetTransform(base);
                dc->PopLayer();
            }
            const Pose l = stack::LabelPose();
            dc->SetTransform(D2D1::Matrix3x2F::Translation(l.x, l.y) * base);
            PaintGhostLabel(dc, item);
            dc->SetTransform(base);
            if (pose.outline)
                dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(-1, -1, stack::RowW + 1, stack::RowH + 1), stack::RowRadius + 1, stack::RowRadius + 1),
                                         SolidBrush(dc, glass::Accent).Get(), 2.f);
        } catch (...) {
            dc->EndDraw();
            throw;
        }
        dc->EndDraw();
    }
    void PaintHint(Canvas& c, const std::wstring& label, float w) {
        auto* dc = c.dc.Get();
        dc->BeginDraw();
        try {
            dc->SetTransform(D2D1::Matrix3x2F::Identity());
            dc->Clear(D2D1::ColorF(0, 0.f));
            dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0, 0, w, HintH), 6, 6), SolidBrush(dc, 0x0a1428, .6f).Get());
            Line(dc, label, f_hint.Get(), D2D1::RectF(8, 0, w - 8, HintH), 0xffffff, 1);
        } catch (...) {
            dc->EndDraw();
            throw;
        }
        dc->EndDraw();
    }
    POINT GhostOrigin() const {
        return {ghost_cursor.x - static_cast<LONG>(std::lround((GhostW / 2 - (stack::RowW / 2 - GhostGrabX)) * ghost_scale)),
                ghost_cursor.y - static_cast<LONG>(std::lround((GhostH / 2 - (stack::RowH / 2 - GhostGrabY)) * ghost_scale))};
    }
    POINT HintOrigin() const {
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromPoint(ghost_cursor, MONITOR_DEFAULTTONEAREST), &monitor);
        const LONG w = static_cast<LONG>(ghost_hint.w), gap = static_cast<LONG>(std::lround(16 * ghost_scale));
        LONG x = ghost_cursor.x + gap;
        if (x + w > monitor.rcWork.right - static_cast<LONG>(std::lround(6 * ghost_scale))) x = ghost_cursor.x - gap - w;
        return {x, ghost_cursor.y + static_cast<LONG>(std::lround(18 * ghost_scale))};
    }
    float HintOpacity() const {
        return hint_fading ? std::min(1.f, static_cast<float>(GetTickCount64() - hint_start) / HintFadeMs) : 1.f;
    }
    void RenderGhost() {
        if (!MakeCanvas(ghost_card, GhostW, GhostH)) return;
        PaintGhostCard(ghost_card, ghost_item, ghost_now);
        PresentCanvas(ghost_card, ghost_window, GhostOrigin(), ghost_now.opacity);
    }
    void RenderHint() {
        if (ghost_text.empty()) { ShowWindow(hint_window, SW_HIDE); return; }
        const float w = std::ceil(Measure(ghost_text, f_hint.Get())) + 16;
        if (!MakeCanvas(ghost_hint, w, HintH)) return;
        PaintHint(ghost_hint, ghost_text, w);
        PresentCanvas(ghost_hint, hint_window, HintOrigin(), HintOpacity());
        if (!IsWindowVisible(hint_window)) ShowWindow(hint_window, SW_SHOWNOACTIVATE);
    }
    void GhostTick() {
        const ULONGLONG now = GetTickCount64();
        if (ghost_animating) {
            const float k = std::min(1.f, static_cast<float>(now - ghost_start) / GhostMoveMs), e = Ease(k);
            ghost_now.rotation = ghost_from.rotation + (ghost_to.rotation - ghost_from.rotation) * e;
            ghost_now.scale = ghost_from.scale + (ghost_to.scale - ghost_from.scale) * e;
            if (k >= 1) ghost_animating = false;
            Safe([&] { RenderGhost(); });
        }
        if (hint_fading) {
            if (now - hint_start >= HintFadeMs) hint_fading = false;
            if (ghost_hint.dib) {
                BLENDFUNCTION blend{AC_SRC_OVER, 0, static_cast<BYTE>(std::lround(HintOpacity() * 255)), AC_SRC_ALPHA};
                UpdateLayeredWindow(hint_window, nullptr, nullptr, nullptr, nullptr, nullptr, 0, &blend, ULW_ALPHA);
            }
        }
        if (!ghost_animating && !hint_fading && ghost_window) KillTimer(ghost_window, 1);
    }
    static LRESULT CALLBACK GhostProc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
        if (m == WM_NCCREATE) SetWindowLongPtrW(w, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
        if (m == WM_NCHITTEST) return HTTRANSPARENT;
        if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
        if (m == WM_TIMER && wp == 1) {
            if (auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(w, GWLP_USERDATA))) self->GhostTick();
            return 0;
        }
        return DefWindowProcW(w, m, wp, lp);
    }
    bool BeginGhost(const StackItem& item, POINT cursor) {
        EndGhost();
        if (!created || !GhostFactories()) return false;
        ghost_scale = scale;
        MakeFormat(f_hint, L"Microsoft YaHei UI", 11.f, DWRITE_FONT_WEIGHT_NORMAL);
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW wc{sizeof(wc)};
        wc.hInstance = instance;
        wc.lpfnWndProc = GhostProc;
        wc.lpszClassName = GhostClass;
        RegisterClassExW(&wc);
        const DWORD ex = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE;
        ghost_window = CreateWindowExW(ex, GhostClass, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance, this);
        hint_window = CreateWindowExW(ex, GhostClass, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance, this);
        if (!ghost_window || !hint_window) { EndGhost(); return false; }
        Quiet(ghost_window);
        Quiet(hint_window);
        ghost_item = item;
        ghost_cursor = cursor;
        ghost_from = ghost_to = ghost_now = ZonePose(DragZone::None);
        ghost_text.clear();
        ghost_animating = hint_fading = false;
        bool ok = false;
        Safe([&] { RenderGhost(); ok = ghost_card.dib != nullptr; });
        if (!ok) { EndGhost(); return false; }
        ShowWindow(ghost_window, SW_SHOWNOACTIVATE);
        return true;
    }
    void MoveGhost(POINT cursor, const GhostPose& pose, const std::wstring& hint) {
        if (!ghost_window) return;
        const bool moved = cursor.x != ghost_cursor.x || cursor.y != ghost_cursor.y;
        ghost_cursor = cursor;
        bool redraw = false;
        if (pose.rotation != ghost_to.rotation || pose.scale != ghost_to.scale) {
            ghost_from = ghost_now;
            ghost_to = pose;
            ghost_start = GetTickCount64();
            ghost_animating = true;
            redraw = true;
        }
        if (pose.opacity != ghost_now.opacity || pose.outline != ghost_now.outline) {
            ghost_now.opacity = ghost_to.opacity = pose.opacity;  // not transitioned in the HTML
            ghost_now.outline = ghost_to.outline = pose.outline;
            redraw = true;
        }
        if (redraw) Safe([&] { RenderGhost(); });
        else if (moved) {
            const POINT at = GhostOrigin();
            SetWindowPos(ghost_window, HWND_TOPMOST, at.x, at.y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        }
        if (hint != ghost_text) {
            if (ghost_text.empty() && !hint.empty()) { hint_start = GetTickCount64(); hint_fading = true; }  // .cursorhint opacity .2s
            ghost_text = hint;
            Safe([&] { RenderHint(); });
        } else if (moved && !ghost_text.empty()) {
            const POINT at = HintOrigin();
            SetWindowPos(hint_window, HWND_TOPMOST, at.x, at.y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        }
        if (ghost_animating || hint_fading) SetTimer(ghost_window, 1, 15, nullptr);
    }
    void EndGhost() {
        if (ghost_window) { KillTimer(ghost_window, 1); DestroyWindow(ghost_window); ghost_window = nullptr; }
        if (hint_window) { DestroyWindow(hint_window); hint_window = nullptr; }
        ghost_card.Release();
        ghost_hint.Release();
        ghost_wic.Reset();
        ghost_factory.Reset();
        ghost_animating = hint_fading = false;
        ghost_text.clear();
        ghost_item = {};
    }
    // Stand-alone ghost image at the resting pose (tests, shell fallback).
    DragImage RenderDragImage(size_t index) {
        DragImage out;
        if (index >= items.size() || !created) return out;
        const bool had = ghost_factory != nullptr;
        ghost_scale = scale;
        Canvas c;
        Safe([&] {
            if (!MakeCanvas(c, GhostW, GhostH)) return;
            PaintGhostCard(c, items[index], ZonePose(DragZone::None));
            out.width = static_cast<int>(c.w);
            out.height = static_cast<int>(c.h);
            out.offset = {static_cast<LONG>(std::lround((GhostW / 2 - (stack::RowW / 2 - GhostGrabX)) * scale)),
                          static_cast<LONG>(std::lround((GhostH / 2 - (stack::RowH / 2 - GhostGrabY)) * scale))};
            out.pixels.resize(static_cast<size_t>(c.w) * c.h);
            if (!CanvasPixels(c, out.pixels.data())) out = {};
        });
        c.Release();
        if (!had && !ghost_window) { ghost_wic.Reset(); ghost_factory.Reset(); }
        return out;
    }
    void BuildTree() {
        // One DesktopWindowTarget per window for its lifetime; rebuilds reuse it.
        if (!target) target = compositor->CreateTarget(visual);
        if (!target) throw CompositionError{E_FAIL, __LINE__};
        Must(C()->CreateContainerVisual(&root));
        Must(As<wuc::IVisual>(root)->put_Size(Vector2{stack::WindowW * scale, stack::WindowH * scale}));
        Must(As<wuc::ICompositionTarget>(target)->put_Root(As<wuc::IVisual>(root).Get()));
        Must(As<wuc::ICompositionBrush>(ColorBrush(0, 0)).As(&transparent_brush));
        ComPtr<wuc::ICompositionBackdropBrush> backdrop;
        Must(As<wuc::ICompositor3>(ComPtr<wuc::ICompositor>(C()))->CreateHostBackdropBrush(&backdrop));
        Must(backdrop.As(&backdrop_brush));
        Fonts();
        DrawArtwork();
        BuildGrip();
        more = Sprite(nullptr, false);
        DrawMore();
        Must(more->put_Brush(As<wuc::ICompositionBrush>(SurfaceBrush(more_surface.Get(), false)).Get()));
        Add(root.Get(), more.Get());
        ScheduleTrim();
    }
    void BuildGrip() {
        Must(C()->CreateContainerVisual(&grip));
        ComPtr<wuc::IDropShadow> far_shadow, near_shadow;
        grip_shadow_far = ShadowSprite(mask_grip.Get(), far_shadow);
        grip_shadow_near = ShadowSprite(mask_grip.Get(), near_shadow);
        StyleRowShadows(far_shadow.Get(), near_shadow.Get());
        // Body extends GripRadius beyond the edge so only the inner corners are round.
        ComPtr<wuc::IContainerVisual> body;
        Must(C()->CreateContainerVisual(&body));
        grip_backdrop = Sprite(backdrop_brush.Get(), true);
        grip_tint_brush = ColorBrush(glass::Pick(glass::CardGlass, dark), glass::CardGlassAlpha(dark));
        grip_tint = Sprite(As<wuc::ICompositionBrush>(grip_tint_brush).Get(), true);
        grip_solid_brush = ColorBrush(glass::Pick(glass::StackSolid, dark), 1);
        grip_solid = Sprite(As<wuc::ICompositionBrush>(grip_solid_brush).Get(), true);
        Must(As<wuc::IVisual>(grip_solid)->put_Opacity(0));
        if (!glassy) {
            Must(As<wuc::IVisual>(grip_backdrop)->put_Opacity(0));
            Must(grip_tint_brush->put_Color(Rgba(glass::Pick(glass::CardGlass, dark), 1)));
        }
        grip_border = Sprite(grip_border_brush.Get(), true);
        Add(body.Get(), grip_backdrop.Get());
        Add(body.Get(), grip_tint.Get());
        Add(body.Get(), grip_solid.Get());
        Add(body.Get(), grip_border.Get());
        auto shape = RoundRect(stack::GripRadius);
        Must(shape->put_Size(Vector2{(stack::GripW + stack::GripRadius) * scale, stack::GripH * scale}));
        Clip(body.Get(), shape.Get());
        GripBodyLayout(body.Get(), grip_shadow_far.Get(), grip_shadow_near.Get());
        DrawGripText();
        grip_text = Sprite(As<wuc::ICompositionBrush>(SurfaceBrush(grip_text_surface.Get(), false)).Get(), true);
        Add(grip.Get(), grip_shadow_far.Get());
        Add(grip.Get(), grip_shadow_near.Get());
        Add(grip.Get(), body.Get());
        Add(grip.Get(), grip_text.Get());
        grip_body = body;
        Add(root.Get(), grip.Get());
    }
    ComPtr<wuc::IContainerVisual> grip_body;
    // Body and shadows of the grip are GripRadius wider and run past the screen
    // edge, so only the inner corners are round and the edge-side border is cut.
    void GripBodyLayout(wuc::IContainerVisual* body, wuc::ISpriteVisual* shadow_a, wuc::ISpriteVisual* shadow_b) const {
        const float extra = stack::GripRadius * scale, dx = side == stack::Side::Right ? 0.f : -extra;
        for (IUnknown* v : {static_cast<IUnknown*>(body), static_cast<IUnknown*>(shadow_a), static_cast<IUnknown*>(shadow_b)}) {
            ComPtr<wuc::IVisual> part;
            Must(v->QueryInterface(IID_PPV_ARGS(&part)));
            Must(part->put_Offset(Vector3{dx, 0, 0}));
            Must(As<wuc::IVisual2>(part)->put_RelativeSizeAdjustment(Vector2{1, 1}));
            Must(part->put_Size(Vector2{extra, 0}));
        }
    }
    void StyleRowShadows(wuc::IDropShadow* far_shadow, wuc::IDropShadow* near_shadow) const {
        const uint32_t c = glass::Pick(glass::StackShadow, dark);
        Must(far_shadow->put_Color(Rgba(c, dark ? glass::StackShadowAlphaDark : glass::StackShadowAlphaLight)));
        Must(far_shadow->put_BlurRadius(48 * ShadowBlurScale * scale));
        Must(far_shadow->put_Offset(Vector3{0, 18 * scale, 0}));
        Must(near_shadow->put_Color(Rgba(c, dark ? glass::StackShadowNearAlphaDark : glass::StackShadowNearAlphaLight)));
        Must(near_shadow->put_BlurRadius(6 * ShadowBlurScale * scale));
        Must(near_shadow->put_Offset(Vector3{0, 2 * scale, 0}));
    }

    CardVisual MakeCard(uint64_t id) {
        CardVisual c;
        c.id = id;
        Must(C()->CreateContainerVisual(&c.root));
        c.shadow_far = ShadowSprite(mask_row.Get(), c.far_shadow);
        c.shadow_near = ShadowSprite(mask_row.Get(), c.near_shadow);
        Must(C()->CreateContainerVisual(&c.body));
        Must(As<wuc::IVisual2>(c.body)->put_RelativeSizeAdjustment(Vector2{1, 1}));
        c.body_shape = RoundRect(stack::MiniRadius);
        Clip(c.body.Get(), c.body_shape.Get());
        c.backdrop = Sprite(backdrop_brush.Get(), true);
        c.tint_brush = ColorBrush(glass::Pick(glass::CardGlass, dark), glassy ? glass::CardGlassAlpha(dark) : 1.f);
        c.tint = Sprite(As<wuc::ICompositionBrush>(c.tint_brush).Get(), true);
        c.thumb = Sprite(nullptr, false);
        c.thumb_shape = RoundRect(0);
        Clip(c.thumb.Get(), c.thumb_shape.Get());
        c.label = Sprite(nullptr, false);
        c.ring = Sprite(ring_brush.Get(), true);
        const float spread = 1.5f * scale;
        Must(As<wuc::IVisual>(c.ring)->put_Offset(Vector3{-spread, -spread, 0}));
        Must(As<wuc::IVisual>(c.ring)->put_Size(Vector2{2 * spread, 2 * spread}));
        Add(c.body.Get(), c.backdrop.Get());
        Add(c.body.Get(), c.tint.Get());
        Add(c.body.Get(), c.thumb.Get());
        Add(c.body.Get(), c.label.Get());
        Add(c.root.Get(), c.shadow_far.Get());
        Add(c.root.Get(), c.shadow_near.Get());
        Add(c.root.Get(), c.body.Get());
        Add(c.root.Get(), c.ring.Get());
        if (!glassy) Must(As<wuc::IVisual>(c.backdrop)->put_Opacity(0));
        return c;
    }

    // Applies the HTML pose of card `i` (open or collapsed) with motion `m`.
    void PoseCard(CardVisual& c, int i, bool open, const Motion& m, const Motion& instant) {
        const Pose p = stack::CardPose(i, open, side, open && i == hovered);
        auto v = As<wuc::IVisual>(c.root);
        const float s = scale;
        AnimateVector3(v.Get(), L"Offset", Vector3{p.x * s, p.y * s, 0}, m);
        AnimateVector2(v.Get(), L"Size", Vector2{p.w * s, p.h * s}, m);
        AnimateVector3(v.Get(), L"CenterPoint", Vector3{p.w * s / 2, p.h * s / 2, 0}, m);
        AnimateScalar(v.Get(), L"RotationAngleInDegrees", p.rotation, m);
        AnimateVector3(v.Get(), L"Scale", Vector3{p.scale, p.scale, 1}, m);
        AnimateScalar(v.Get(), L"Opacity", p.opacity, m);
        AnimateVector2(c.body_shape.Get(), L"Size", Vector2{p.w * s, p.h * s}, m);
        AnimateVector2(c.body_shape.Get(), L"CornerRadius", Vector2{p.radius * s, p.radius * s}, m);
        // Glass background fades in with the move (transparent -> var(--glass)).
        const float glass_opacity = open ? 1.f : 0.f;
        AnimateScalar(c.backdrop.Get(), L"Opacity", glassy ? glass_opacity : 0.f, m);
        AnimateScalar(c.tint.Get(), L"Opacity", glass_opacity, m);
        // Thumbnail size / radius switch instantly (no transition on .thumb); its
        // position follows the animated 3 px padding. Label shows instantly.
        const Pose t = stack::ThumbPose(open);
        AnimateVector2(c.thumb.Get(), L"Size", Vector2{t.w * s, t.h * s}, instant);
        AnimateVector2(c.thumb_shape.Get(), L"Size", Vector2{t.w * s, t.h * s}, instant);
        AnimateVector2(c.thumb_shape.Get(), L"CornerRadius", Vector2{t.radius * s, t.radius * s}, instant);
        AnimateVector3(c.thumb.Get(), L"Offset", Vector3{t.x * s, t.y * s, 0}, m);
        const Pose l = stack::LabelPose();
        const float pad = open ? stack::RowPad : 0.f;
        AnimateVector2(c.label.Get(), L"Size", Vector2{l.w * s, l.h * s}, instant);
        AnimateVector3(c.label.Get(), L"Offset", Vector3{(pad + stack::ThumbRowW + stack::LabelGap) * s, pad * s, 0}, m);
        AnimateScalar(c.label.Get(), L"Opacity", open ? 1.f : 0.f, instant);
        // box-shadow: [0 6 16 rgba(0,0,0,.25), ring 1.5px white .9] -> var(--shadow)
        const uint32_t sc = glass::Pick(glass::StackShadow, dark);
        AnimateScalar(c.far_shadow.Get(), L"BlurRadius", (open ? 48.f : 16.f) * ShadowBlurScale * s, m);
        AnimateVector3(c.far_shadow.Get(), L"Offset", Vector3{0, (open ? 18.f : 6.f) * s, 0}, m);
        AnimateColor(c.far_shadow.Get(), L"Color", open ? Rgba(sc, dark ? glass::StackShadowAlphaDark : glass::StackShadowAlphaLight) : Rgba(0, .25f), m);
        Must(c.near_shadow->put_Color(Rgba(sc, dark ? glass::StackShadowNearAlphaDark : glass::StackShadowNearAlphaLight)));
        Must(c.near_shadow->put_BlurRadius(6 * ShadowBlurScale * s));
        Must(c.near_shadow->put_Offset(Vector3{0, 2 * s, 0}));
        AnimateScalar(c.near_shadow.Get(), L"Opacity", open ? 1.f : 0.f, m);
        AnimateScalar(c.ring.Get(), L"Opacity", open ? 0.f : 1.f, m);
    }
    void PoseGrip(bool open, const Motion& m) {
        const Pose g = stack::GripPose(open, side);
        auto v = As<wuc::IVisual>(grip);
        AnimateVector3(v.Get(), L"Offset", Vector3{g.x * scale, g.y * scale, 0}, m);
        Must(v->put_Size(Vector2{g.w * scale, g.h * scale}));
    }
    void PoseMore(bool open, const Motion& m) {
        const Pose p = stack::MorePose(side);
        auto v = As<wuc::IVisual>(more);
        Must(v->put_Offset(Vector3{p.x * scale, p.y * scale, 0}));
        Must(v->put_Size(Vector2{p.w * scale, p.h * scale}));
        AnimateScalar(v.Get(), L"Opacity", open ? 1.f : 0.f, m);
    }
    void Layout(bool animate, int incoming = -1) {
        if (!root) return;
        const Motion move = animate ? Ease(stack::MoveEase, stack::MoveMs) : Instant();
        const Motion instant = Instant();
        for (size_t i = 0; i < cards.size(); ++i) {
            const int index = static_cast<int>(i);
            if (index == incoming && motion) {
                // Start at the inslide keyframe, then run 500 ms to the slot.
                const Pose from = stack::IncomingPose(fanned, side), to = stack::CardPose(index, fanned, side);
                auto v = As<wuc::IVisual>(cards[i].root);
                PoseCard(cards[i], index, fanned, instant, instant);
                const Motion in = Ease(stack::IncomingEase, stack::IncomingMs);
                const Vector3 offset_from{from.x * scale, from.y * scale, 0};
                const float zero = 0;
                AnimateVector3(v.Get(), L"Offset", Vector3{to.x * scale, to.y * scale, 0}, in, &offset_from);
                AnimateScalar(v.Get(), L"RotationAngleInDegrees", to.rotation, in, &from.rotation);
                AnimateScalar(v.Get(), L"Opacity", to.opacity, in, &zero);
                continue;
            }
            PoseCard(cards[i], index, fanned, move, instant);
        }
        PoseGrip(fanned, animate ? Ease(stack::GripEase, stack::GripMs) : Instant());
        PoseMore(fanned, animate ? Linear(200) : Instant());
    }
    void Restack() {
        // Grip at the bottom, cards last..0 (0 on top), the "more" line last.
        ComPtr<wuc::IVisualCollection> kids;
        Must(root->get_Children(&kids));
        Must(kids->RemoveAll());
        Add(root.Get(), grip.Get());
        for (size_t i = cards.size(); i-- > 0;) Add(root.Get(), cards[i].root.Get());
        Add(root.Get(), more.Get());
    }

    void ScheduleTrim() {
        if (input) SetTimerBit(TrimTimer, TrimDelayMs);
        else if (compositor) compositor->Trim();
    }

    // ---------------------------------------------------------------- state
    void UpdateRegion() {
        if (!input) return;
        HRGN region = CreateRectRgn(0, 0, 0, 0);
        for (const auto& polygon : stack::InputPolygons(static_cast<int>(cards.size()), fanned, side, scale)) {
            HRGN one = CreatePolygonRgn(polygon.data(), 4, WINDING);
            CombineRgn(region, region, one, RGN_OR);
            DeleteObject(one);
        }
        if (!SetWindowRgn(input, region, TRUE)) DeleteObject(region);
    }
    void SetTimerBit(UINT_PTR id, UINT ms) {
        SetTimer(input, id, ms, nullptr);
        timers |= 1u << id;
    }
    void KillTimerBit(UINT_PTR id) {
        if (timers & (1u << id)) KillTimer(input, id);
        timers &= ~(1u << id);
    }
    // ------------------------------------------------------------- flight
    void Fade(IUnknown* animated, float from, float middle, float at, float to, unsigned ms) {
        ComPtr<wuc::IScalarKeyFrameAnimation> a;
        Must(C()->CreateScalarKeyFrameAnimation(&a));
        Must(a->InsertKeyFrame(0.f, from));
        Must(a->InsertKeyFrame(at, middle));
        Must(a->InsertKeyFrame(1.f, to));
        Start(animated, L"Opacity", a.Get(), ms);
    }
    bool Fly(const std::vector<RECT>& slots, bool out) {
        if (!root || !motion || !shown || cards.empty() || slots.empty() || !visual || !IsWindowVisible(visual)) return false;
        if (flying) EndFlight();
        RECT area = window;
        for (const auto& slot : slots) if (!IsRectEmpty(&slot)) UnionRect(&area, &area, &slot);
        const LONG pad = static_cast<LONG>(std::lround(56 * scale));  // room for the card shadows
        InflateRect(&area, pad, pad);
        flying = true;
        KillTimerBit(HoverTimer); KillTimerBit(LeaveTimer);
        SetWindowPos(visual, HWND_TOPMOST, area.left, area.top, area.right - area.left, area.bottom - area.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        bool ok = false;
        Safe([&] {
            Must(As<wuc::IVisual>(root)->put_Offset(Vector3{static_cast<float>(window.left - area.left), static_cast<float>(window.top - area.top), 0}));
            const Motion fly = Ease(FlightEase, FlightMs), instant = Instant();
            const float s = scale, radius = FlightRadius * s;
            for (size_t i = 0; i < cards.size(); ++i) {
                auto& c = cards[i];
                auto v = As<wuc::IVisual>(c.root);
                const int index = static_cast<int>(i);
                const bool has_slot = i < slots.size() && !IsRectEmpty(&slots[i]);
                if (!has_slot) {  // no visible slot: just fade with the flight
                    const float home = stack::CardPose(index, false, side).opacity;
                    Fade(v.Get(), out ? home : 0.f, home * .5f, .5f, out ? 0.f : home, FlightMs);
                    continue;
                }
                const RECT& slot = slots[i];
                const Vector3 at{static_cast<float>(slot.left - window.left), static_cast<float>(slot.top - window.top), 0};
                const Vector2 size{static_cast<float>(slot.right - slot.left), static_cast<float>(slot.bottom - slot.top)};
                const Vector3 center{size.X / 2, size.Y / 2, 0};
                if (out) {
                    // Become the row card (glass, thumb, label), then take the panel slot.
                    PoseCard(c, index, true, fly, instant);
                    AnimateVector3(v.Get(), L"Offset", at, fly);
                    AnimateVector2(v.Get(), L"Size", size, fly);
                    AnimateVector3(v.Get(), L"CenterPoint", center, fly);
                    AnimateScalar(v.Get(), L"RotationAngleInDegrees", 0.f, fly);
                    AnimateVector3(v.Get(), L"Scale", Vector3{1, 1, 1}, fly);
                    AnimateVector2(c.body_shape.Get(), L"Size", size, fly);
                    AnimateVector2(c.body_shape.Get(), L"CornerRadius", Vector2{radius, radius}, fly);
                    Fade(v.Get(), 1.f, 1.f, .72f, 0.f, FlightMs);  // dissolve into the panel's own card
                } else {
                    // Start on the panel slot, land back in the folded stack.
                    const Pose home = stack::CardPose(index, false, side);
                    PoseCard(c, index, false, fly, instant);
                    const Vector3 home_at{home.x * s, home.y * s, 0};
                    const Vector2 home_size{home.w * s, home.h * s};
                    const float zero = 0.f;
                    const Vector3 one{1, 1, 1};
                    AnimateVector3(v.Get(), L"Offset", home_at, fly, &at);
                    AnimateVector2(v.Get(), L"Size", home_size, fly, &size);
                    AnimateVector3(v.Get(), L"CenterPoint", Vector3{home_size.X / 2, home_size.Y / 2, 0}, fly, &center);
                    AnimateScalar(v.Get(), L"RotationAngleInDegrees", home.rotation, fly, &zero);
                    AnimateVector3(v.Get(), L"Scale", Vector3{home.scale, home.scale, 1}, fly, &one);
                    AnimateVector2(c.body_shape.Get(), L"Size", home_size, fly, &size);
                    const Vector2 from_radius{radius, radius};
                    AnimateVector2(c.body_shape.Get(), L"CornerRadius", Vector2{home.radius * s, home.radius * s}, fly, &from_radius);
                    Fade(v.Get(), 0.f, 1.f, .25f, home.opacity, FlightMs);
                }
            }
            ok = true;
        });
        if (!ok) { EndFlight(); return false; }
        SetTimerBit(FlightTimer, FlightMs + 40);
        return true;
    }
    void EndFlight() {
        if (!flying) return;
        flying = false;
        KillTimerBit(FlightTimer);
        Safe([&] { Must(As<wuc::IVisual>(root)->put_Offset(Vector3{0, 0, 0})); Layout(false); });
        if (!shown) ShowWindow(visual, SW_HIDE);
        SetWindowPos(visual, HWND_TOPMOST, window.left, window.top, window.right - window.left, window.bottom - window.top, SWP_NOACTIVATE);
        UpdateRegion();
    }
    void Fan(bool open) {
        KillTimerBit(HoverTimer);
        KillTimerBit(LeaveTimer);
        if (fanned == open) return;
        fanned = open;
        wheel_rest = 0;
        if (!open) hovered = -1;
        Safe([&] { Layout(true); });
        UpdateRegion();
        if (!open && events.folded) events.folded();
    }
    void SetHovered(int index) {
        if (index == hovered) return;
        const int old = hovered;
        hovered = index;
        if (!fanned) return;
        Safe([&] {
            const Motion move = Ease(stack::MoveEase, stack::MoveMs), instant = Instant();
            for (int i : {old, index})
                if (i >= 0 && i < static_cast<int>(cards.size())) PoseCard(cards[static_cast<size_t>(i)], i, true, move, instant);
        });
    }
    HRESULT last_error{S_OK};
    int last_error_line{};
    template <class F> void Safe(F&& f) {
        try {
            f();
        } catch (const CompositionError& e) {
            // Composition failure (device lost, OOM): keep the windows, drop glass.
            last_error = e.hr;
            last_error_line = e.line;
            glassy = false;
        } catch (...) {
            last_error = E_UNEXPECTED;
            glassy = false;
        }
    }
    void SetSolid(bool solid) {
        if (!grip_solid) return;
        Safe([&] { AnimateScalar(grip_solid.Get(), L"Opacity", solid ? 1.f : 0.f, solid ? Instant() : Linear(stack::SolidFadeMs)); });
    }

    // ---------------------------------------------------------------- input
    stack::Point Local(LPARAM lp) const {
        return {static_cast<float>(GET_X_LPARAM(lp)) / scale, static_cast<float>(GET_Y_LPARAM(lp)) / scale};
    }
    LRESULT InputProc(UINT m, WPARAM wp, LPARAM lp) {
        switch (m) {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_SETCURSOR: SetCursor(LoadCursorW(nullptr, IDC_HAND)); return TRUE;
        case WM_MOUSEMOVE: {
            if (!tracking) {
                TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, input, 0};
                tracking = TrackMouseEvent(&t) != FALSE;
            }
            KillTimerBit(LeaveTimer);
            if (pressed) { PressedMove(); return 0; }
            const auto hit = stack::HitTest(Local(lp), static_cast<int>(cards.size()), fanned, side, hovered);
            if (!fanned) {
                if (!(timers & (1u << HoverTimer))) SetTimerBit(HoverTimer, stack::HoverDelayMs);
            } else {
                SetHovered(hit.kind == stack::HitKind::Card ? hit.index : -1);
            }
            return 0;
        }
        case WM_MOUSEWHEEL: {
            // Folded: the first notch fans the stack. Open: one history entry per notch.
            if (pressed || dragging >= 0 || flying) return 0;
            if (!tracking) {
                TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, input, 0};
                tracking = TrackMouseEvent(&t) != FALSE;
            }
            KillTimerBit(LeaveTimer);
            wheel_rest += GET_WHEEL_DELTA_WPARAM(wp);
            const int notches = wheel_rest / WHEEL_DELTA;
            wheel_rest %= WHEEL_DELTA;
            if (notches == 0) return 0;
            if (!fanned) { Fan(true); return 0; }
            if (events.scroll) events.scroll(-notches);
            return 0;
        }
        case WM_MOUSELEAVE:
            tracking = false;
            if (pressed) return 0;
            KillTimerBit(HoverTimer);
            if (fanned) SetTimerBit(LeaveTimer, stack::LeaveGraceMs);
            return 0;
        case WM_TIMER:
            if (wp == HoverTimer) { KillTimerBit(HoverTimer); Fan(true); }
            else if (wp == LeaveTimer) { KillTimerBit(LeaveTimer); Fan(false); }
            else if (wp == TrimTimer) { KillTimerBit(TrimTimer); if (compositor) compositor->Trim(); }
            else if (wp == FlightTimer) EndFlight();
            return 0;
        case WM_LBUTTONDOWN: {
            press_hit = stack::HitTest(Local(lp), static_cast<int>(cards.size()), fanned, side, hovered);
            GetCursorPos(&press_point);
            pressed = press_hit.kind == stack::HitKind::Card || press_hit.kind == stack::HitKind::Grip;
            if (pressed) {
                grab_offset = press_point.y - stack::GripTop(window, scale);
                SetCapture(input);
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            if (!pressed) return 0;
            const bool was_moving = moving;
            const auto hit = press_hit;
            EndPress();
            if (was_moving) {
                SetSolid(false);
                if (events.moved) events.moved(window);
            } else if (hit.kind == stack::HitKind::Card && events.activate) {
                events.activate(static_cast<size_t>(hit.index));
            } else if (hit.kind == stack::HitKind::Grip && events.open_panel) {
                Fan(false);
                events.open_panel();
            }
            return 0;
        }
        case WM_CAPTURECHANGED:
            if (pressed && reinterpret_cast<HWND>(lp) != input) {
                const bool was_moving = moving;
                EndPress();
                if (was_moving) { SetSolid(false); if (events.moved) events.moved(window); }
            }
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE && pressed) {
                const bool was_moving = moving;
                EndPress();
                if (was_moving) {
                    Place(move_side, move_top, move_work, scale);
                    SetSolid(false);
                    if (events.move_cancelled) events.move_cancelled();
                }
            }
            return 0;
        }
        return DefWindowProcW(input, m, wp, lp);
    }
    void EndPress() {
        pressed = moving = false;
        if (GetCapture() == input) ReleaseCapture();
    }
    void PressedMove() {
        POINT now{};
        GetCursorPos(&now);
        const int cx = GetSystemMetrics(SM_CXDRAG), cy = GetSystemMetrics(SM_CYDRAG);
        if (!moving && std::abs(now.x - press_point.x) <= cx && std::abs(now.y - press_point.y) <= cy) return;
        if (press_hit.kind == stack::HitKind::Card) {
            // S2: OLE drag-out of the card. Without a handler a moved press is a cancel.
            const size_t index = static_cast<size_t>(press_hit.index);
            EndPress();
            KillTimerBit(LeaveTimer);
            if (!events.drag) return;
            // The source card stays as a 25% placeholder while it is dragged.
            if (index < cards.size()) Safe([&] { AnimateScalar(As<wuc::IVisual>(cards[index].root).Get(), L"Opacity", .25f, Instant()); });
            dragging = static_cast<int>(index);
            events.drag(index, now);
            dragging = -1;
            tracking = false;  // the OLE loop consumed the leave tracking
            Safe([&] { Layout(true); });
            POINT after{};
            GetCursorPos(&after);
            if (fanned && !PtInRect(&window, after)) SetTimerBit(LeaveTimer, stack::LeaveGraceMs);
            return;
        }
        if (!moving) {
            moving = true;
            move_side = side;
            move_top = stack::GripTop(window, scale);
            move_work = work;
            Fan(false);
            SetSolid(true);
        }
        // MoveStack: follow the pointer along the edge; dock to the nearer edge.
        const HMONITOR monitor = MonitorFromPoint(now, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info{sizeof(info)};
        if (!GetMonitorInfoW(monitor, &info)) return;
        const stack::Side next = stack::NearestSide(now.x, info.rcWork);
        const UINT dpi = GetDpiForWindow(input);
        Place(next, now.y - grab_offset, info.rcWork, dpi ? static_cast<float>(dpi) / 96.f : scale);
        if (events.dragging) events.dragging(now);
    }

    // ---------------------------------------------------------------- placement
    void Place(stack::Side next, LONG grip_top, RECT area, float next_scale) {
        EndFlight();
        const bool rescale = std::abs(next_scale - scale) > .001f;
        const bool reside = next != side;
        side = next;
        scale = next_scale;
        work = area;
        window = stack::WindowRect(side, grip_top, work, scale);
        const int w = window.right - window.left, h = window.bottom - window.top;
        SetWindowPos(visual, HWND_TOPMOST, window.left, window.top, w, h, SWP_NOACTIVATE);
        SetWindowPos(input, HWND_TOPMOST, window.left, window.top, w, h, SWP_NOACTIVATE);
        if (!root) return;
        Safe([&] {
            if (rescale) Rebuild();
            else if (reside) {
                GripBodyLayout(grip_body.Get(), grip_shadow_far.Get(), grip_shadow_near.Get());
                DrawMore();
                ScheduleTrim();
                Layout(false);
            }
        });
        UpdateRegion();
    }
    void Rebuild() {
        // DPI change: drop every surface and rebuild the tree at the new scale.
        cards.clear();
        grip.Reset(); grip_body.Reset(); more.Reset();
        grip_text_surface.Reset(); more_surface.Reset();
        f_title.Reset(); f_sub.Reset(); f_idx.Reset(); f_count.Reset(); f_more.Reset();
        f_tile.Reset(); f_tile_text.Reset(); f_tile_code.Reset(); f_tile_link.Reset();
        if (target) Must(As<wuc::ICompositionTarget>(target)->put_Root(nullptr));
        root.Reset();
        ring_brush.Reset(); mask_mini.Reset(); mask_row.Reset(); mask_grip.Reset(); grip_border_brush.Reset();
        ring_surface.Reset(); mini_mask_surface.Reset(); row_mask_surface.Reset(); grip_mask_surface.Reset(); grip_border_surface.Reset();
        grip_border.Reset();
        BuildTree();
        Sync(-1);
    }
    // Rebuild card visuals from `items` (by id), redraw labels, then pose.
    void Sync(int incoming) {
        std::vector<CardVisual> next;
        const size_t n = std::min(items.size(), static_cast<size_t>(stack::MaxCards));
        next.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            auto found = std::find_if(cards.begin(), cards.end(), [&](const CardVisual& c) { return c.id == items[i].id && c.id; });
            if (found != cards.end()) {
                next.push_back(std::move(*found));
                // The thumbnail usually arrives after the card was built with the placeholder.
                if (next.back().thumb_key != ThumbKey(items[i])) DrawThumb(next.back(), items[i]);
                DrawLabel(next.back(), items[i], i);  // "Alt+N" follows the index
                continue;
            }
            next.push_back(MakeCard(items[i].id));
            CardVisual& c = next.back();
            DrawThumb(c, items[i]);
            DrawLabel(c, items[i], i);
            Must(c.thumb->put_Brush(As<wuc::ICompositionBrush>(SurfaceBrush(c.thumb_surface.Get(), true)).Get()));
            Must(c.label->put_Brush(As<wuc::ICompositionBrush>(SurfaceBrush(c.label_surface.Get(), false)).Get()));
        }
        cards = std::move(next);
        DrawGripText();
        DrawMore();
        ScheduleTrim();
        Restack();
        Layout(false, incoming);
        UpdateRegion();
    }

    // ---------------------------------------------------------------- windows
    static LRESULT CALLBACK VisualProc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
        if (m == WM_NCHITTEST) return HTTRANSPARENT;
        return DefWindowProcW(w, m, wp, lp);
    }
    static LRESULT CALLBACK StaticInputProc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
        if (m == WM_NCCREATE) SetWindowLongPtrW(w, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(w, GWLP_USERDATA));
        if (!self || !self->input) return DefWindowProcW(w, m, wp, lp);
        return self->InputProc(m, wp, lp);
    }
    static void Quiet(HWND w) {
        const BOOL yes = TRUE;
        DwmSetWindowAttribute(w, DWMWA_TRANSITIONS_FORCEDISABLED, &yes, sizeof(yes));
        const DWORD square = 1;  // DWMWCP_DONOTROUND
        DwmSetWindowAttribute(w, 33, &square, sizeof(square));
        const DWORD none = 0xFFFFFFFE;  // DWMWA_COLOR_NONE
        DwmSetWindowAttribute(w, 34, &none, sizeof(none));
    }
    bool Create() {
        if (created) return true;
        compositor = ui::BackdropCompositor::Acquire();
        if (!compositor) return false;
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW wc{sizeof(wc)};
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_HAND);
        wc.lpfnWndProc = VisualProc;
        wc.lpszClassName = VisualClass;
        RegisterClassExW(&wc);
        wc.lpfnWndProc = StaticInputProc;
        wc.lpszClassName = InputClass;
        RegisterClassExW(&wc);
        const DWORD common = WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP;
        visual = CreateWindowExW(common | WS_EX_LAYERED | WS_EX_TRANSPARENT, VisualClass, L"LumaShot 剪贴板卡片", WS_POPUP, 0, 0, 1, 1, owner, nullptr, instance, nullptr);
        if (!visual) return false;
        SetLayeredWindowAttributes(visual, 0, 255, LWA_ALPHA);
        const BOOL yes = TRUE;
        DwmSetWindowAttribute(visual, 17 /*DWMWA_USE_HOSTBACKDROPBRUSH*/, &yes, sizeof(yes));
        Quiet(visual);
        // Owned by the visual window: always directly above it.
        input = CreateWindowExW(common, InputClass, L"LumaShot 剪贴板卡片", WS_POPUP, 0, 0, 1, 1, visual, nullptr, instance, this);
        if (!input) { DestroyWindow(visual); visual = nullptr; return false; }
        Quiet(input);
        glassy = glassy && ui::BackdropCompositor::GlassAvailable();
        const UINT dpi = GetDpiForWindow(visual);
        scale = dpi ? static_cast<float>(dpi) / 96.f : 1.f;
        try {
            BuildTree();
            if (!items.empty()) Sync(-1);
        } catch (const CompositionError& e) {
            last_error = e.hr;
            last_error_line = e.line;
            Destroy();
            return false;
        } catch (...) {
            Destroy();
            return false;
        }
        created = true;
        return true;
    }
    void Destroy() {
        EndGhost();
        cards.clear();
        grip.Reset(); grip_body.Reset(); more.Reset();
        ComPtr<wuc::ICompositionTarget> composition_target;
        if (target && SUCCEEDED(target.As(&composition_target))) composition_target->put_Root(nullptr);
        root.Reset(); target.Reset();
        if (input) { KillTimerBit(HoverTimer); KillTimerBit(LeaveTimer); KillTimerBit(TrimTimer); DestroyWindow(input); input = nullptr; }
        if (visual) { DestroyWindow(visual); visual = nullptr; }
        compositor.reset();
        created = false;
    }
};

StackWindow::StackWindow(HWND owner, StackEvents events) : impl_(std::make_unique<Impl>()) {
    impl_->owner = owner;
    impl_->events = std::move(events);
}
StackWindow::~StackWindow() { impl_->Destroy(); }
bool StackWindow::Create() { return impl_->Create(); }
void StackWindow::SetItems(std::vector<StackItem> items, size_t total, bool incoming, size_t first) {
    auto& d = *impl_;
    d.first = first;
    const bool fresh = incoming && !items.empty() && (d.items.empty() || d.items.front().id != items.front().id);
    d.items = std::move(items);
    d.total = std::max(total, first + d.items.size());
    if (!d.root) return;
    d.Safe([&] { d.Sync(fresh ? 0 : -1); });
}
void StackWindow::SetTheme(bool dark) {
    auto& d = *impl_;
    if (d.dark == dark && d.root) return;
    d.dark = dark;
    if (!d.root) return;
    d.Safe([&] {
        Must(d.grip_tint_brush->put_Color(Rgba(glass::Pick(glass::CardGlass, dark), d.glassy ? glass::CardGlassAlpha(dark) : 1.f)));
        Must(d.grip_solid_brush->put_Color(Rgba(glass::Pick(glass::StackSolid, dark), 1)));
        for (auto& c : d.cards) {
            Must(c.tint_brush->put_Color(Rgba(glass::Pick(glass::CardGlass, dark), d.glassy ? glass::CardGlassAlpha(dark) : 1.f)));
            c.thumb_drawn = false;  // file tiles follow the theme
        }
        d.DrawGripBorder();
        for (size_t i = 0; i < d.cards.size() && i < d.items.size(); ++i) d.DrawThumb(d.cards[i], d.items[i]);
        d.Sync(-1);
    });
}
void StackWindow::SetGlass(bool enabled) {
    auto& d = *impl_;
    d.glassy = enabled && ui::BackdropCompositor::GlassAvailable();
    if (!d.root) return;
    d.Safe([&] {
        const float alpha = d.glassy ? glass::CardGlassAlpha(d.dark) : 1.f;
        Must(As<wuc::IVisual>(d.grip_backdrop)->put_Opacity(d.glassy ? 1.f : 0.f));
        Must(d.grip_tint_brush->put_Color(Rgba(glass::Pick(glass::CardGlass, d.dark), alpha)));
        for (auto& c : d.cards) Must(c.tint_brush->put_Color(Rgba(glass::Pick(glass::CardGlass, d.dark), alpha)));
        d.Layout(false);
    });
}
void StackWindow::SetPaused(bool paused) {
    auto& d = *impl_;
    if (d.paused == paused) return;
    d.paused = paused;
    if (!d.root) return;
    d.Safe([&] { d.DrawGripText(); d.ScheduleTrim(); });
}
void StackWindow::SetMotion(bool enabled) { impl_->motion = enabled; }
DragImage StackWindow::RenderDragImage(size_t index) {
    auto& d = *impl_;
    DragImage out;
    d.Safe([&] { out = d.RenderDragImage(index); });
    return out;
}
int StackWindow::DraggingIndex() const { return impl_->dragging; }
bool StackWindow::BeginGhost(const StackItem& item, POINT cursor) { return impl_->BeginGhost(item, cursor); }
void StackWindow::MoveGhost(POINT cursor, const GhostPose& pose, const std::wstring& hint) { impl_->MoveGhost(cursor, pose, hint); }
void StackWindow::EndGhost() { impl_->EndGhost(); }
StackWindow::GhostInfo StackWindow::InspectGhost() const {
    const auto& d = *impl_;
    GhostInfo info;
    info.active = d.ghost_window != nullptr;
    info.animating = d.ghost_animating;
    info.pose = d.ghost_now;
    info.target = d.ghost_to;
    info.hint = d.ghost_text;
    if (d.ghost_window) GetWindowRect(d.ghost_window, &info.card);
    if (d.hint_window && IsWindowVisible(d.hint_window)) GetWindowRect(d.hint_window, &info.hint_rect);
    return info;
}
void StackWindow::SetBadgePrefix(std::wstring prefix) {
    auto& d = *impl_;
    if (d.badge_prefix == prefix) return;
    d.badge_prefix = std::move(prefix);
    if (d.root) d.Safe([&] { d.Sync(-1); });
}
void StackWindow::Place(stack::Side side, LONG grip_top, RECT work, float scale) {
    if (impl_->created) impl_->Place(side, grip_top, work, scale);
}
void StackWindow::Show(bool visible) {
    auto& d = *impl_;
    if (!d.created) return;
    d.shown = visible;
    if (!visible && d.flying) {  // an outward flight keeps the visual until it lands
        d.EndPress();
        d.fanned = false;
        d.hovered = -1;
        ShowWindow(d.input, SW_HIDE);
        return;
    }
    if (visible) d.EndFlight();
    if (!visible) {
        d.EndPress();
        d.Fan(false);
        ShowWindow(d.input, SW_HIDE);
        ShowWindow(d.visual, SW_HIDE);
        return;
    }
    ShowWindow(d.visual, SW_SHOWNOACTIVATE);
    ShowWindow(d.input, SW_SHOWNOACTIVATE);
}
void StackWindow::Collapse() { impl_->Fan(false); }
bool StackWindow::Fly(const std::vector<RECT>& slots, bool toward_panel) { return impl_->created && impl_->Fly(slots, toward_panel); }
bool StackWindow::Flying() const { return impl_->flying; }
bool StackWindow::Visible() const { return impl_->visual && IsWindowVisible(impl_->visual); }
bool StackWindow::Fanned() const { return impl_->fanned; }
stack::Side StackWindow::Side() const { return impl_->side; }
RECT StackWindow::WindowRect() const { return impl_->window; }
HWND StackWindow::InputWindow() const { return impl_->input; }
HWND StackWindow::VisualWindow() const { return impl_->visual; }
StackWindow::Diagnostics StackWindow::Inspect() const {
    const auto& d = *impl_;
    Diagnostics out;
    out.glass = d.glassy;
    out.graphics = d.compositor && d.compositor->HasGraphics();
    out.moving = d.moving;
    out.cards = static_cast<int>(d.cards.size());
    for (const auto& card : d.cards) if (card.thumb_image) ++out.thumbnail_cards;
    for (unsigned bits = d.timers; bits; bits &= bits - 1) ++out.timers;
    out.hovered = d.hovered;
    out.surface_draws = d.draws;
    out.last_error = d.last_error;
    out.last_error_line = d.last_error_line;
    return out;
}

}  // namespace lumashot::clipboard