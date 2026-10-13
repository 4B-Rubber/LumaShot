#pragma once
#include "capture/frame.h"
#include "pin/clothesline_model.h"
#include "ui/text_renderer.h"
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>

namespace lumashot {
class FramePacer;
// The hanger icon from the clothesline prototype: a 24 x 24 unit open path,
// meant to be stroked 1.9 units wide with round caps.
Microsoft::WRL::ComPtr<ID2D1PathGeometry> CreateHangerIcon(ID2D1Factory* factory);
// A rope that drops from the top edge of the screen; pins can be hung on it as
// swinging thumbnails and taken back. One layered, non-activating, topmost
// window per manager, created on first use. It animates only while something
// moves: frames are paced to the display's vertical blank by a worker thread
// that is parked whenever the rope is still. Settled cards are cached as
// sprites (shadow + paper + picture + clip) and only the rows that changed are
// pushed to the layered window. While pins are hung or open and the rope is hidden, a 100 ms cursor
// poll watches the top edge: resting on it fills a dwell bar that brings the
// rope down, and after a short rest a click-through glow window and the tab
// ("晾着 N 张") appear (16 ms frames, only while they are on screen). Merely
// passing near the edge (desktop icons, toolbars) shows nothing.
// When resting on the top edge brings the rope down. Pure state machine (the
// caller passes cursor, time and the foreground state), unit-tested.
// - Fullscreen foreground (games, videos, presentations, F11 browsers, or the
//   shell reporting busy / D3D fullscreen / presentation mode): no glow, no dwell.
//   Their menu bars and tabs live at the top edge.
// - The cursor must rest: moving more than kStillDip restarts the dwell, so
//   sliding along a menu bar or a tab strip never fills it.
// - Maximized foreground (tabs and menus reachable at y = 0): longer dwell.
// - Right after a mouse button is released (a click on a tab, a drag that
//   ended at the edge) the dwell waits kReleaseQuietMs.
// - Glow and tab: only after the cursor has rested still on the edge for
//   kRevealMs (the start of the dwell), never just for being near it, so
//   hovering icons or toolbars along the top of the screen stays quiet. Once
//   shown they stay while the cursor is within kKeepDip of the edge (to reach
//   the tab) and disappear beyond it until the next rest on the edge.
struct ClotheslineEdge {
    enum class Foreground{Normal,Maximized,Fullscreen};
    static constexpr ULONGLONG kDwellMs=350,kMaximizedDwellMs=600,kReleaseQuietMs=400,kRevealMs=150;
    static constexpr float kStillDip=4,kKeepDip=48;
    // `top` is the monitor's top edge in physical pixels (for the keep zone).
    struct Input{POINT cursor{};ULONGLONG now{};bool buttons{},at_edge{},on_tab{};Foreground foreground{Foreground::Normal};float scale{1};LONG top{};};
    struct Output{bool glow{};float dwell{};bool fire{};};
    Output Update(const Input& in);
    void Reset(){start_=0;revealed_=false;}
    // The rope just folded: a cursor still parked on the edge must leave it (or
    // slide away along it) before a new rest counts, or the rope would keep
    // dropping and folding under it.
    void Folded(){Reset();wait_leave_=true;leave_anchored_=false;}
    // Resting on the edge or showing the tab: the caller polls at 16 ms.
    bool Dwelling()const{return start_!=0||revealed_;}
    bool Revealed()const{return revealed_;}
    // Geometry and style of one top-level window on a monitor (no process or
    // shell filtering): covers the whole monitor without a caption, or is not
    // maximized while covering it = Fullscreen; maximized = Maximized.
    static Foreground Classify(HWND window,const RECT& monitor);
    // The foreground window on `monitor`, ignoring LumaShot's own windows, the
    // desktop and the taskbar; while it is there, SHQueryUserNotificationState
    // reporting busy / D3D fullscreen / presentation mode also counts.
    static Foreground Current(HMONITOR monitor,const RECT& monitor_rect);
private:
    ULONGLONG start_{},quiet_until_{};POINT anchor_{},leave_anchor_{};bool buttons_{},revealed_{},wait_leave_{},leave_anchored_{};
};

class Clothesline {
public:
    struct Callbacks {
        // Pointer released on a thumbnail without dragging: the pin flies back
        // from `from` (thumbnail picture in screen pixels) to where it was.
        std::function<void(uint64_t id,RECT from)> retrieve;
        // Thumbnail pulled off the rope: the pin continues the drag under `cursor`.
        std::function<void(uint64_t id,POINT cursor)> detach;
        // True while pins are open on screen: the top edge reacts even with
        // an empty rope, so pins can be hung by dragging them up.
        std::function<bool()> armed;
    };
    explicit Clothesline(Callbacks callbacks);
    ~Clothesline();
    Clothesline(const Clothesline&)=delete;
    Clothesline& operator=(const Clothesline&)=delete;

    // While a pin is dragged: returns true when the cursor is in the hang zone
    // (the rope is shown with an insertion gap).
    bool DragUpdate(POINT cursor,SIZE image);
    // Pin dropped: hangs it at the previewed slot if the cursor is in the zone.
    bool DragDrop(uint64_t id,std::shared_ptr<const Frame> image,POINT cursor);
    void DragEnd();
    // Hang directly (keyboard); `from` is the pin picture in screen pixels.
    void Hang(uint64_t id,std::shared_ptr<const Frame> image,RECT from);
    // Put a pin back on the rope without showing it (session restore after a
    // restart): no flight, the rope stays up until the top edge brings it down.
    void Restore(uint64_t id,std::shared_ptr<const Frame> image);
    void Remove(uint64_t id);
    void RetrieveAll();
    bool Contains(uint64_t id)const{return model_.Find(id)>=0;}
    size_t Count()const{return model_.items.size();}
    // Pins were opened: start watching the top edge (stops by itself).
    void Arm();
    // Settings switch. Off: the top edge, drags and hangs are ignored, every
    // hung pin flies back home (through `retrieve`) and the rope folds away.
    void SetEnabled(bool enabled);
    bool Enabled()const{return enabled_;}
    void Show();
    void Hide();
    bool GlowVisible()const{return glow_shown_;}
    bool Visible()const{return model_.visible;}
    HWND Window()const{return window_;}
private:
    friend struct ClotheslineTest;
    // A pre-rendered card: `ox`,`oy` is the paper's top-left inside the bitmap,
    // `cw`,`ch` its size; drawn rotated with one bitmap blit per frame.
    struct Sprite{Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;float ox{},oy{},cw{},ch{},w{},h{};};
    struct Thumb{std::shared_ptr<const Frame> image;Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;int w{},h{};Sprite sprites[2];};
    struct Held{uint64_t id{};POINT start{};ULONGLONG time{};bool moved{};};
    static LRESULT CALLBACK Proc(HWND,UINT,WPARAM,LPARAM);
    LRESULT Handle(UINT,WPARAM,LPARAM);
    void EnsureWindow();
    void Place(POINT point);
    void ShowAt(POINT point);
    void Add(uint64_t id,std::shared_ptr<const Frame> image,ClotheslineRect from,int index,float ms);
    void Retrieve(uint64_t id);
    void Animate();
    void Tick();
    void Poll();
    void Render();
    void Draw(ID2D1RenderTarget* target);
    float DrawItem(ID2D1RenderTarget* target,const ClotheslineItem& item,bool hover);
    void PaintCard(ID2D1RenderTarget* target,const ClotheslineRect& r,ID2D1Bitmap* bitmap,float border,bool hover,uint64_t id,bool clip);
    const Sprite* CardSprite(ID2D1RenderTarget* target,const ClotheslineItem& item,bool hover);
    ID2D1Bitmap* Bake(ID2D1RenderTarget* target,int width,int height,const std::function<void(ID2D1RenderTarget*)>& paint);
    void ResetSprites();
    void DrawRope(ID2D1RenderTarget* target);
    void Label(ID2D1RenderTarget* target,const wchar_t* text,D2D1_RECT_F rect,D2D1_COLOR_F color);
    ID2D1Bitmap* ThumbBitmap(ID2D1RenderTarget* target,const ClotheslineItem& item);
    int BarHit(float x,float y)const;
    // While the rope is down: is the cursor (window coordinates) on or near
    // something drawn? The strip between the top edge and the rope, a card, the
    // bar or the empty-rope hint count; the transparent space under and beside
    // the cards does not.
    bool KeepOpen(float x,float y)const;
    void UpdatePoll();
    bool Armed()const;
    // Top-edge feedback while the rope is hidden.
    void UpdateGlow(POINT cursor,ULONGLONG now,bool allowed,const RECT& monitor);
    void RenderGlow();
    void HideGlow();
    // Idle sleep: once the cursor has rested away from the top edge for
    // kSleepAfterPolls polls, the poll timer stops and raw mouse input
    // (RIDEV_INPUTSINK, no hook) wakes it on the next movement. Input that
    // moves the cursor without a mouse report (SetCursorPos from another
    // app) is only seen after the next real movement.
    static constexpr int kSleepAfterPolls=10;
    void TrackStill(POINT cursor,bool near_edge);
    void StopSleeping();
    void WakeOnInput();
    float TextWidth(const wchar_t* text,IDWriteTextFormat* format)const;

    Callbacks callbacks_;
    ClotheslineModel model_;
    HWND window_{};RECT monitor_{};bool placed_{},animating_{},polling_{},dragging_{},enabled_{true};
    std::unique_ptr<FramePacer> pacer_;int64_t last_qpc_{};bool defer_thumbs_{};
    // Cost of the last frame in milliseconds (read by the performance test).
    struct FrameCost{double clear{},rope{},cards{},blit{};int rope_bakes{},sprite_bakes{},thumb_builds{};} cost_;
    // Rows [0, drawn_bottom_) hold last frame's pixels; everything below is transparent.
    int drawn_bottom_{};bool full_update_{true};float content_bottom_{};std::vector<float> rope_xs_,rope_ys_;
    // While only the cards swing the rope keeps its shape: it is baked once and
    // blitted. `rope_cache_xs_/ys_` is the polyline the bitmap was made from.
    Microsoft::WRL::ComPtr<ID2D1Bitmap> rope_cache_;std::vector<float> rope_cache_xs_,rope_cache_ys_,rope_last_ys_;float rope_cache_top_{};int rope_still_{};
    Microsoft::WRL::ComPtr<ID2D1Bitmap> bar_sprite_,hint_sprite_;size_t bar_sprite_count_{};int bar_sprite_hover_{-2};float hint_w_{};
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> hanger_;
    ULONGLONG hold_until_{},hide_at_{},next_retrieve_{};UINT poll_interval_{};
    bool sleeping_{};POINT still_cursor_{};int still_polls_{};
    ClotheslineEdge edge_;
    // Foreground state is looked up only near the top edge, at most every 250 ms.
    ClotheslineEdge::Foreground foreground_{ClotheslineEdge::Foreground::Normal};ULONGLONG foreground_tick_{};HMONITOR foreground_monitor_{};
    std::map<uint64_t,Thumb> thumbs_;
    std::deque<uint64_t> retrieve_queue_;
    std::optional<Held> held_;
    int hover_{-1},bar_hover_{-1};POINT last_mouse_{};bool tracking_{};
    ClotheslineRect bar_{},bar_all_{},bar_fold_{},hint_rect_{};
    std::unique_ptr<DibSurface> surface_;int surface_width_{},surface_height_{};
    Microsoft::WRL::ComPtr<ID2D1Factory> factory_;
    Microsoft::WRL::ComPtr<ID2D1RenderTarget> target_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_;
    Microsoft::WRL::ComPtr<ID2D1StrokeStyle> round_,twist_,dashed_;
    Microsoft::WRL::ComPtr<IDWriteFactory> text_factory_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> text_format_;float text_scale_{};
    TextRenderer text_renderer_;
    // Glow window: layered, click-through (WS_EX_TRANSPARENT), hidden when idle.
    HWND glow_window_{};RECT glow_monitor_{};float glow_scale_{1};bool glow_shown_{};
    float glow_{},glow_goal_{},tab_{},dwell_{};ULONGLONG glow_tick_{};RECT tab_screen_{};
    std::unique_ptr<DibSurface> glow_surface_;int glow_width_{},glow_height_{};
    Microsoft::WRL::ComPtr<ID2D1RenderTarget> glow_target_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> glow_brush_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> glow_format_,hint_format_;float glow_format_scale_{},hint_format_scale_{};
};
}