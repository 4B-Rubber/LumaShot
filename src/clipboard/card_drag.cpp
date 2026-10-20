#include "clipboard/card_drag.h"
#include "clipboard/native.h"
#include <shlobj.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <future>
#include <map>
#include <thread>

namespace lumashot::clipboard {
namespace {
using Microsoft::WRL::ComPtr;

HGLOBAL Copy(const void* data, size_t size) {
    const HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, size ? size : 1);
    if (!memory) return nullptr;
    if (void* target = GlobalLock(memory)) {
        if (size) std::memcpy(target, data, size);
        GlobalUnlock(memory);
        return memory;
    }
    GlobalFree(memory);
    return nullptr;
}
HGLOBAL DropFiles(const std::filesystem::path& path) {
    const auto text = path.wstring();
    const HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DROPFILES) + (text.size() + 2) * sizeof(wchar_t));
    if (!memory) return nullptr;
    auto* files = static_cast<DROPFILES*>(GlobalLock(memory));
    if (!files) { GlobalFree(memory); return nullptr; }
    files->pFiles = sizeof(DROPFILES);
    files->fWide = TRUE;
    std::copy(text.begin(), text.end(), reinterpret_cast<wchar_t*>(files + 1));
    GlobalUnlock(memory);
    return memory;
}
std::string Utf8(std::wstring_view s) {
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(std::max(n, 0)), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}
// ASCII-only URL: non-ASCII and controls percent-encoded as UTF-8.
std::string AsciiUrl(std::wstring_view url) {
    std::string out;
    for (unsigned char c : Utf8(url)) {
        if (c > 0x20 && c < 0x7f) out.push_back(static_cast<char>(c));
        else {
            char hex[4]{};
            std::snprintf(hex, sizeof(hex), "%%%02X", c);
            out += hex;
        }
    }
    return out;
}

class DataObject final : public IDataObject {
public:
    explicit DataObject(CardDragPayload payload) {
        const auto add = [&](UINT id, std::vector<unsigned char> bytes) {
            if (!id || data_.contains(static_cast<CLIPFORMAT>(id))) return;
            data_.emplace(static_cast<CLIPFORMAT>(id), std::move(bytes));
            formats_.push_back({static_cast<CLIPFORMAT>(id), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL});
        };
        if (payload.file) {
            // Encode while the user is still moving; a canceled drag never waits for it.
            std::promise<std::filesystem::path> promise;
            file_ = promise.get_future().share();
            try {
                std::thread([promise = std::move(promise), produce = std::move(payload.file)]() mutable {
                    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                    std::filesystem::path path;
                    try { path = produce(); } catch (...) {}
                    try { promise.set_value(std::move(path)); } catch (...) {}
                    if (SUCCEEDED(com)) CoUninitialize();
                }).detach();
                formats_.push_back({CLIPFORMAT(CF_HDROP), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL});
            } catch (...) {
                file_ = {};
            }
        }
        for (auto& f : payload.formats) {
            if (payload.file && f.id == CF_HDROP) continue;
            add(f.id, std::move(f.bytes));
        }
        if (!payload.url.empty()) {
            std::vector<unsigned char> wide((payload.url.size() + 1) * sizeof(wchar_t));
            std::memcpy(wide.data(), payload.url.c_str(), wide.size());
            add(RegisterClipboardFormatW(L"UniformResourceLocatorW"), std::move(wide));
            const std::string ascii = AsciiUrl(payload.url);
            add(RegisterClipboardFormatW(L"UniformResourceLocator"), std::vector<unsigned char>(ascii.c_str(), ascii.c_str() + ascii.size() + 1));
        }
    }
    ~DataObject() {
        for (auto& entry : extra_) GlobalFree(entry.second);
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if (!out) return E_POINTER;
        if (id == IID_IUnknown || id == IID_IDataObject) {
            *out = static_cast<IDataObject*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = --refs_;
        if (!left) delete this;
        return left;
    }
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* format, STGMEDIUM* medium) override {
        if (!format || !medium) return E_INVALIDARG;
        *medium = {};
        if (!(format->tymed & TYMED_HGLOBAL) || format->dwAspect != DVASPECT_CONTENT) return DV_E_FORMATETC;
        HGLOBAL memory{};
        if (const auto it = extra_.find(format->cfFormat); it != extra_.end()) {
            memory = Copy(GlobalLock(it->second), GlobalSize(it->second));
            GlobalUnlock(it->second);
        } else if (format->cfFormat == CF_HDROP && file_.valid()) {
            std::filesystem::path path;
            try {
                if (file_.wait_for(std::chrono::seconds(10)) != std::future_status::ready) return DV_E_FORMATETC;
                path = file_.get();
            } catch (...) { return DV_E_FORMATETC; }
            if (path.empty()) return DV_E_FORMATETC;
            memory = DropFiles(path);
        } else if (const auto data = data_.find(format->cfFormat); data != data_.end()) {
            memory = Copy(data->second.data(), data->second.size());
        } else {
            return DV_E_FORMATETC;
        }
        if (!memory) return E_OUTOFMEMORY;
        medium->tymed = TYMED_HGLOBAL;
        medium->hGlobal = memory;
        medium->pUnkForRelease = nullptr;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* format) override {
        if (!format) return E_INVALIDARG;
        if (!(format->tymed & TYMED_HGLOBAL) || format->dwAspect != DVASPECT_CONTENT) return DV_E_FORMATETC;
        return Offers(format->cfFormat) || extra_.contains(format->cfFormat) ? S_OK : DV_E_FORMATETC;
    }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* out) override {
        if (out) out->ptd = nullptr;
        return DATA_S_SAMEFORMATETC;
    }
    // The shell and drop targets attach small blobs (drag image, drop description).
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC* format, STGMEDIUM* medium, BOOL release) override {
        if (!format || !medium) return E_INVALIDARG;
        if (medium->tymed != TYMED_HGLOBAL || !medium->hGlobal || Offers(format->cfFormat)) return E_NOTIMPL;
        const HGLOBAL memory = Copy(GlobalLock(medium->hGlobal), GlobalSize(medium->hGlobal));
        GlobalUnlock(medium->hGlobal);
        if (!memory) return E_OUTOFMEMORY;
        if (auto it = extra_.find(format->cfFormat); it != extra_.end()) {
            GlobalFree(it->second);
            it->second = memory;
        } else {
            extra_.emplace(format->cfFormat, memory);
        }
        if (release) ReleaseStgMedium(medium);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC** out) override {
        if (!out) return E_POINTER;
        if (direction != DATADIR_GET) { *out = nullptr; return E_NOTIMPL; }
        return SHCreateStdEnumFmtEtc(static_cast<UINT>(formats_.size()), formats_.data(), out);
    }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }

private:
    bool Offers(CLIPFORMAT format) const {
        for (const auto& f : formats_)
            if (f.cfFormat == format) return true;
        return false;
    }
    std::atomic<ULONG> refs_{1};
    std::map<CLIPFORMAT, std::vector<unsigned char>> data_;
    std::shared_future<std::filesystem::path> file_;
    std::vector<FORMATETC> formats_;
    std::map<CLIPFORMAT, HGLOBAL> extra_;
};

class DropSource final : public IDropSource {
public:
    DropSource(std::function<DragRelease(POINT, DWORD)> release, std::function<void(POINT, bool, bool)> moved)
        : release_(std::move(release)), moved_(std::move(moved)) {}
    bool Escaped() const { return escaped_; }
    bool Intercepted() const { return intercepted_; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if (!out) return E_POINTER;
        if (id == IID_IUnknown || id == IID_IDropSource) {
            *out = static_cast<IDropSource*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = --refs_;
        if (!left) delete this;
        return left;
    }
    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escape, DWORD keys) override {
        if (escape) { escaped_ = true; return DRAGDROP_S_CANCEL; }
        if (!(keys & MK_LBUTTON)) {
            POINT cursor{};
            GetCursorPos(&cursor);
            if (release_ && release_(cursor, keys) == DragRelease::Intercept) { intercepted_ = true; return DRAGDROP_S_CANCEL; }
            return DRAGDROP_S_DROP;
        }
        Follow();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD effect) override {
        over_target_ = effect != DROPEFFECT_NONE;
        Follow();
        if (over_target_) return DRAGDROP_S_USEDEFAULTCURSORS;
        // Every release does something (pin, file, back to top, copy): no "forbidden" cursor.
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        return S_OK;
    }

private:
    // Reported only on change: GiveFeedback and QueryContinueDrag both arrive per mouse move.
    void Follow() {
        POINT cursor{};
        GetCursorPos(&cursor);
        const bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
        if (cursor.x == last_.x && cursor.y == last_.y && reported_ == over_target_ && alt == alt_) return;
        last_ = cursor;
        reported_ = over_target_;
        alt_ = alt;
        if (moved_) moved_(cursor, over_target_, alt);
    }
    std::atomic<ULONG> refs_{1};
    std::function<DragRelease(POINT, DWORD)> release_;
    std::function<void(POINT, bool, bool)> moved_;
    POINT last_{LONG_MIN, LONG_MIN};
    bool over_target_{}, reported_{}, alt_{}, escaped_{}, intercepted_{};
};

void AttachImage(IDataObject* data, const DragImage& image) {
    if (image.width <= 0 || image.height <= 0 || image.pixels.size() < static_cast<size_t>(image.width) * static_cast<size_t>(image.height)) return;
    ComPtr<IDragSourceHelper> helper;
    if (FAILED(CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&helper)))) return;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = image.width;
    info.bmiHeader.biHeight = -image.height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void* bits{};
    const HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap || !bits) { if (bitmap) DeleteObject(bitmap); return; }
    std::memcpy(bits, image.pixels.data(), static_cast<size_t>(image.width) * static_cast<size_t>(image.height) * 4);
    SHDRAGIMAGE shell{};
    shell.sizeDragImage = {image.width, image.height};
    shell.ptOffset = image.offset;
    shell.hbmpDragImage = bitmap;
    shell.crColorKey = CLR_NONE;
    if (FAILED(helper->InitializeFromBitmap(&shell, data))) DeleteObject(bitmap);  // on success the helper owns it
}

std::wstring Stamp() {
    SYSTEMTIME t{};
    GetLocalTime(&t);
    wchar_t stamp[48]{};
    swprintf_s(stamp, L"LumaShot-%04u%02u%02u-%02u%02u%02u-%03u", unsigned(t.wYear), unsigned(t.wMonth), unsigned(t.wDay), unsigned(t.wHour),
               unsigned(t.wMinute), unsigned(t.wSecond), unsigned(t.wMilliseconds));
    return stamp;
}
const Format* Find(const Entry& e, UINT id) {
    for (const auto& f : e.formats)
        if (f.id == id) return &f;
    return nullptr;
}
}  // namespace

CardDragResult DragCard(CardDragPayload payload, CardDragOptions options) {
    // DoDragDrop needs OLE on this thread (main only initializes COM).
    static const HRESULT ole = OleInitialize(nullptr);
    if (FAILED(ole)) return CardDragResult::Failed;
    ComPtr<IDataObject> data;
    try { data = MakeCardDataObject(std::move(payload)); } catch (...) { return CardDragResult::Failed; }
    AttachImage(data.Get(), options.image);
    ComPtr<DropSource> source;
    source.Attach(new DropSource(std::move(options.on_release), std::move(options.moved)));
    DWORD effect = DROPEFFECT_NONE;
    const HRESULT result = DoDragDrop(data.Get(), source.Get(), DROPEFFECT_COPY, &effect);
    if (result == DRAGDROP_S_DROP && effect != DROPEFFECT_NONE) return CardDragResult::Dropped;
    if (FAILED(result)) return CardDragResult::Failed;
    if (source->Intercepted()) return CardDragResult::Intercepted;
    return source->Escaped() ? CardDragResult::Canceled : CardDragResult::NoTarget;
}

DragRelease DecideRelease(const ReleaseContext& c) {
    if (c.over_stack) return DragRelease::Intercept;  // back to the top of the history
    if (!c.desktop) return DragRelease::Default;
    if (c.kind == Kind::Image) return c.pin_available && !c.alt ? DragRelease::Intercept : DragRelease::Default;  // Alt: Explorer saves the PNG
    if (c.kind == Kind::Files) return DragRelease::Default;  // Explorer copies the files
    return DragRelease::Intercept;                           // text: we write a .txt / .url
}
ReleaseAction ResolveRelease(const ReleaseContext& c, CardDragResult result) {
    switch (result) {
    case CardDragResult::Dropped: return ReleaseAction::Insert;
    case CardDragResult::Intercepted:
        if (c.over_stack) return ReleaseAction::MoveToTop;
        if (!c.desktop) return ReleaseAction::None;
        return c.kind == Kind::Image ? ReleaseAction::Pin : c.kind == Kind::Files ? ReleaseAction::None : ReleaseAction::SaveText;
    case CardDragResult::NoTarget: return c.over_stack ? ReleaseAction::MoveToTop : ReleaseAction::CopyFallback;
    case CardDragResult::Canceled:
    case CardDragResult::Failed:
    default: return ReleaseAction::None;
    }
}

GhostPose ZonePose(DragZone zone) {
    GhostPose p;  // .ghost.gcard: rotate(-3deg) scale(1.03)
    if (zone == DragZone::Stack) { p.rotation = 0; p.scale = .92f; p.opacity = .8f; }  // [data-z=stack]
    if (zone == DragZone::Desktop) p.outline = true;                                  // [data-z=desk]
    return p;
}
std::wstring ZoneHint(DragZone zone, const HintContext& c) {
    switch (zone) {
    case DragZone::Stack: return L"松手：放回并置顶";
    case DragZone::App: {
        std::wstring title = c.app;
        if (title.size() > 24) title = title.substr(0, 23) + L"…";
        return title.empty() ? L"松手：插入" : L"松手：插入到「" + title + L"」";
    }
    case DragZone::Desktop:
        if (c.kind == Kind::Image) return c.pin_available && !c.alt ? L"松手：钉到桌面（按住 Alt 存为文件）" : L"松手：在桌面存为 .png";
        if (c.kind == Kind::Files) return L"松手：复制到桌面";
        return c.link ? L"松手：在桌面存为 .url" : L"松手：在桌面存为 .txt";
    case DragZone::None:
    default: return L"拖到程序或桌面";
    }
}

ComPtr<IDataObject> MakeCardDataObject(CardDragPayload payload) {
    ComPtr<IDataObject> data;
    data.Attach(new DataObject(std::move(payload)));
    return data;
}

bool IsDesktopClass(std::wstring_view root_class, bool hosts_defview) {
    return hosts_defview && (root_class == L"Progman" || root_class == L"WorkerW");
}
bool IsDesktopAt(POINT screen) {
    const HWND hit = WindowFromPoint(screen);
    const HWND root = hit ? GetAncestor(hit, GA_ROOT) : nullptr;
    if (!root) return false;
    wchar_t name[64]{};
    if (!GetClassNameW(root, name, 64)) return false;
    return IsDesktopClass(name, FindWindowExW(root, nullptr, L"SHELLDLL_DefView", nullptr) != nullptr);
}

std::wstring SafeFileName(std::wstring_view title, size_t limit) {
    std::wstring out;
    for (wchar_t c : title) {
        if (c == L'\r' || c == L'\n') break;
        if (c < 0x20 || std::wstring_view(L"<>:\"/\\|?*").find(c) != std::wstring_view::npos) c = L' ';
        if (c == L' ' && (out.empty() || out.back() == L' ')) continue;
        out.push_back(c);
    }
    if (out.size() > limit) {
        out.resize(limit);
        if (!out.empty() && out.back() >= 0xd800 && out.back() <= 0xdbff) out.pop_back();
    }
    while (!out.empty() && (out.back() == L' ' || out.back() == L'.')) out.pop_back();
    std::wstring upper = out.substr(0, out.find(L'.'));
    for (auto& c : upper) c = static_cast<wchar_t>(towupper(c));
    static constexpr std::wstring_view reserved[] = {L"CON", L"PRN", L"AUX", L"NUL", L"COM1", L"COM2", L"COM3", L"COM4", L"COM5", L"COM6", L"COM7",
                                                     L"COM8", L"COM9", L"LPT1", L"LPT2", L"LPT3", L"LPT4", L"LPT5", L"LPT6", L"LPT7", L"LPT8", L"LPT9"};
    for (auto r : reserved)
        if (upper == r) { out = L"_" + out; break; }
    return out.empty() ? L"剪贴板文本" : out;
}
std::filesystem::path UniqueFile(const std::filesystem::path& folder, const std::wstring& stem, const std::wstring& extension) {
    std::error_code ec;
    for (int n = 1; n < 1000; ++n) {
        auto path = folder / (stem + (n > 1 ? L" (" + std::to_wstring(n) + L")" : L"") + extension);
        if (!std::filesystem::exists(path, ec)) return path;
    }
    return folder / (stem + L" " + Stamp() + extension);
}
std::string UrlFileContents(std::wstring_view url) { return "[InternetShortcut]\r\nURL=" + AsciiUrl(url) + "\r\n"; }

std::wstring EntryText(const Entry& loaded) {
    if (const auto* f = Find(loaded, CF_UNICODETEXT); f && f->bytes.size() >= sizeof(wchar_t)) {
        std::wstring text(reinterpret_cast<const wchar_t*>(f->bytes.data()), f->bytes.size() / sizeof(wchar_t));
        if (const auto end = text.find(L'\0'); end != std::wstring::npos) text.resize(end);
        return text;
    }
    return loaded.text;
}
std::optional<std::filesystem::path> SaveTextFile(const Entry& loaded, const std::filesystem::path& folder, const std::wstring& title, bool link) {
    std::error_code ec;
    if (folder.empty() || !std::filesystem::is_directory(folder, ec)) return {};
    const std::wstring text = EntryText(loaded);
    const auto path = UniqueFile(folder, SafeFileName(title), link ? L".url" : L".txt");
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return {};
    if (link) {
        const auto first = text.find_first_not_of(L" \t\r\n"), last = text.find_last_not_of(L" \t\r\n");
        out << UrlFileContents(first == std::wstring::npos ? std::wstring_view{} : std::wstring_view(text).substr(first, last - first + 1));
    } else {
        out << "\xEF\xBB\xBF" << Utf8(text);
    }
    out.close();
    if (!out) { std::filesystem::remove(path, ec); return {}; }
    return path;
}

std::vector<std::filesystem::path> FilePaths(const Entry& loaded) {
    std::vector<std::filesystem::path> out;
    const auto* f = Find(loaded, CF_HDROP);
    if (!f || f->bytes.size() < sizeof(DROPFILES)) return out;
    DROPFILES header{};
    std::memcpy(&header, f->bytes.data(), sizeof(header));
    if (header.pFiles < sizeof(DROPFILES) || header.pFiles >= f->bytes.size()) return out;
    const unsigned char* p = f->bytes.data() + header.pFiles;
    const size_t size = f->bytes.size() - header.pFiles;
    if (header.fWide) {
        std::wstring all(size / sizeof(wchar_t), L'\0');
        std::memcpy(all.data(), p, all.size() * sizeof(wchar_t));
        for (size_t at = 0; at < all.size() && all[at];) {
            const size_t end = std::min(all.find(L'\0', at), all.size());
            out.emplace_back(all.substr(at, end - at));
            at = end + 1;
        }
    } else {
        const std::string all(reinterpret_cast<const char*>(p), size);
        for (size_t at = 0; at < all.size() && all[at];) {
            const size_t end = std::min(all.find('\0', at), all.size());
            const std::string one = all.substr(at, end - at);
            const int n = MultiByteToWideChar(CP_ACP, 0, one.data(), static_cast<int>(one.size()), nullptr, 0);
            std::wstring wide(static_cast<size_t>(std::max(n, 0)), L'\0');
            if (n > 0) MultiByteToWideChar(CP_ACP, 0, one.data(), static_cast<int>(one.size()), wide.data(), n);
            out.emplace_back(std::move(wide));
            at = end + 1;
        }
    }
    return out;
}
std::wstring LinkTitle(std::wstring_view url) {
    const auto first = url.find_first_not_of(L" \t\r\n");
    if (first == std::wstring_view::npos) return {};
    url.remove_prefix(first);
    const auto scheme = url.find(L"://");
    if (scheme == std::wstring_view::npos || scheme > 10) return std::wstring(url);
    auto host = url.substr(scheme + 3);
    host = host.substr(0, host.find_first_of(L"/?#"));
    if (const auto at = host.rfind(L'@'); at != std::wstring_view::npos) host.remove_prefix(at + 1);
    if (host.substr(0, 4) == L"www.") host.remove_prefix(4);
    return host.empty() ? std::wstring(url) : std::wstring(host);
}
std::optional<std::filesystem::path> WriteImageFile(const Entry& loaded, std::filesystem::path folder) {
    std::error_code ec;
    if (folder.empty()) {
        folder = std::filesystem::temp_directory_path(ec) / L"LumaShot-Clipboard";
        if (ec) return {};
    }
    std::filesystem::create_directories(folder, ec);
    const auto path = UniqueFile(folder, Stamp(), L".png");
    if (const auto* png = Find(loaded, RegisterClipboardFormatW(L"PNG")); png && png->bytes.size() > 8) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(png->bytes.data()), static_cast<std::streamsize>(png->bytes.size()));
        out.close();
        if (out) return path;
        std::filesystem::remove(path, ec);
        return {};
    }
    const auto source = Thumbnail(loaded, 16384, 268435456ull);
    if (!source) return {};
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
              SUCCEEDED(wic->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
              SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) && SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
              SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
              SUCCEEDED(frame->WriteSource(source.Get(), nullptr)) && SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
    frame.Reset();
    encoder.Reset();
    stream.Reset();
    if (!ok) { std::filesystem::remove(path, ec); return {}; }
    return path;
}

std::shared_ptr<const Frame> DecodeFrame(const Entry& loaded) {
    if (loaded.kind != Kind::Image) return nullptr;
    const auto source = Thumbnail(loaded, 16384, 268435456ull);
    if (!source) return nullptr;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICFormatConverter> converter;
    UINT w{}, h{};
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) || FAILED(wic->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(source.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) ||
        FAILED(converter->GetSize(&w, &h)) || !w || !h)
        return nullptr;
    auto frame = std::make_shared<Frame>();
    frame->bounds = {0, 0, static_cast<LONG>(w), static_cast<LONG>(h)};
    frame->pixels.resize(static_cast<size_t>(w) * h);
    if (FAILED(converter->CopyPixels(nullptr, w * 4, w * h * 4, reinterpret_cast<BYTE*>(frame->pixels.data())))) return nullptr;
    // Pins show opaque pictures: flatten transparency over white.
    for (auto& p : frame->pixels) {
        const uint32_t a = p >> 24;
        if (a == 255) continue;
        const uint32_t inv = 255 - a;
        const uint32_t b = (p & 255) + inv, g = ((p >> 8) & 255) + inv, r = ((p >> 16) & 255) + inv;
        p = 0xff000000u | (std::min(r, 255u) << 16) | (std::min(g, 255u) << 8) | std::min(b, 255u);
    }
    return frame;
}

}  // namespace lumashot::clipboard
