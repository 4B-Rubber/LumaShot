// Card drag-out (docs/clipboard-card-stack.md 3.5, stage S2).
// Synthetic entries only: no DoDragDrop, no system clipboard, no personal files.
// Files are written under the test's working directory in build/.
#include "clipboard/card_drag.h"
#include <shlobj.h>
#include <wincodec.h>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

using namespace lumashot;
using namespace lumashot::clipboard;
using Microsoft::WRL::ComPtr;

namespace {
int checks{}, failures{};
void Expect(bool ok, const char* message) {
    ++checks;
    if (!ok) {
        ++failures;
        std::cout << "FAIL " << message << '\n';
    }
}
Format Wide(UINT id, const std::wstring& text) {
    const auto* p = reinterpret_cast<const unsigned char*>(text.c_str());
    return {id, std::vector<unsigned char>(p, p + (text.size() + 1) * sizeof(wchar_t))};
}
Entry TextEntry(std::wstring text, Kind kind = Kind::Text) {
    Entry e;
    e.id = 7;
    e.kind = kind;
    e.text = text.substr(0, 20);
    e.formats.push_back(Wide(CF_UNICODETEXT, text));
    return e;
}
// 2x2 32bpp bottom-up DIB: red, green / blue, white (top row first visually).
Entry DibEntry() {
    Entry e;
    e.id = 9;
    e.kind = Kind::Image;
    BITMAPINFOHEADER h{};
    h.biSize = sizeof(h);
    h.biWidth = 2;
    h.biHeight = 2;
    h.biPlanes = 1;
    h.biBitCount = 32;
    h.biCompression = BI_RGB;
    const uint32_t pixels[4] = {0x000000ff, 0x00ffffff, 0x00ff0000, 0x0000ff00};  // bottom row: blue, white; top: red, green
    std::vector<unsigned char> bytes(sizeof(h) + sizeof(pixels));
    std::memcpy(bytes.data(), &h, sizeof(h));
    std::memcpy(bytes.data() + sizeof(h), pixels, sizeof(pixels));
    e.formats.push_back({CF_DIB, std::move(bytes)});
    return e;
}
std::vector<unsigned char> Get(IDataObject* data, UINT id) {
    FORMATETC f{static_cast<CLIPFORMAT>(id), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM m{};
    if (FAILED(data->GetData(&f, &m))) return {};
    std::vector<unsigned char> out;
    if (const void* p = GlobalLock(m.hGlobal)) {
        const auto* b = static_cast<const unsigned char*>(p);
        out.assign(b, b + GlobalSize(m.hGlobal));
        GlobalUnlock(m.hGlobal);
    }
    ReleaseStgMedium(&m);
    return out;
}
bool Offers(IDataObject* data, UINT id) {
    FORMATETC f{static_cast<CLIPFORMAT>(id), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    return data->QueryGetData(&f) == S_OK;
}
std::vector<UINT> Enumerated(IDataObject* data) {
    std::vector<UINT> out;
    ComPtr<IEnumFORMATETC> e;
    if (FAILED(data->EnumFormatEtc(DATADIR_GET, &e))) return out;
    FORMATETC f{};
    while (e->Next(1, &f, nullptr) == S_OK) out.push_back(f.cfFormat);
    return out;
}
std::string ReadAll(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

void DataObjects(const std::filesystem::path& work) {
    const UINT html = RegisterClipboardFormatW(L"HTML Format");
    {
        auto e = TextEntry(L"示例文本\r\n第二行");
        const std::string fragment = "Version:0.9\r\n<b>x</b>";
        e.formats.push_back({html, std::vector<unsigned char>(fragment.begin(), fragment.end())});
        CardDragPayload payload;
        payload.formats = e.formats;
        auto data = MakeCardDataObject(payload);
        Expect(Get(data.Get(), CF_UNICODETEXT) == e.formats[0].bytes, "text bytes offered verbatim");
        Expect(Get(data.Get(), html) == e.formats[1].bytes, "HTML Format bytes offered verbatim");
        const auto list = Enumerated(data.Get());
        Expect(list.size() == 2 && list[0] == CF_UNICODETEXT && list[1] == html, "EnumFormatEtc keeps the entry's order");
        Expect(!Offers(data.Get(), CF_HDROP) && Get(data.Get(), CF_HDROP).empty(), "text offers no file");
        FORMATETC stream{CF_UNICODETEXT, nullptr, DVASPECT_CONTENT, -1, TYMED_ISTREAM};
        Expect(data->QueryGetData(&stream) != S_OK, "only HGLOBAL media");
        // Shell helpers attach private blobs through SetData.
        const UINT shell = RegisterClipboardFormatW(L"DragImageBits");
        FORMATETC f{static_cast<CLIPFORMAT>(shell), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        STGMEDIUM m{};
        m.tymed = TYMED_HGLOBAL;
        m.hGlobal = GlobalAlloc(GMEM_MOVEABLE, 4);
        if (auto* p = static_cast<uint32_t*>(GlobalLock(m.hGlobal))) { *p = 0x12345678; GlobalUnlock(m.hGlobal); }
        Expect(data->SetData(&f, &m, TRUE) == S_OK && Offers(data.Get(), shell), "SetData stores shell blobs");
        const auto back = Get(data.Get(), shell);
        Expect(back.size() >= 4 && *reinterpret_cast<const uint32_t*>(back.data()) == 0x12345678, "SetData blob round-trips");
        STGMEDIUM over{};
        over.tymed = TYMED_HGLOBAL;
        over.hGlobal = GlobalAlloc(GMEM_MOVEABLE, 4);
        FORMATETC own{CF_UNICODETEXT, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        Expect(data->SetData(&own, &over, FALSE) == E_NOTIMPL && Get(data.Get(), CF_UNICODETEXT) == e.formats[0].bytes, "entry formats cannot be overwritten");
        GlobalFree(over.hGlobal);
    }
    {
        CardDragPayload payload;
        payload.formats = TextEntry(L"https://例子.example/a b").formats;
        payload.url = L"https://例子.example/a b";
        auto data = MakeCardDataObject(payload);
        const auto wide = Get(data.Get(), RegisterClipboardFormatW(L"UniformResourceLocatorW"));
        Expect(wide.size() >= sizeof(wchar_t) && std::wstring(reinterpret_cast<const wchar_t*>(wide.data())) == payload.url, "links offer UniformResourceLocatorW");
        const auto ascii = Get(data.Get(), RegisterClipboardFormatW(L"UniformResourceLocator"));
        Expect(!ascii.empty() && std::string(reinterpret_cast<const char*>(ascii.data())) == "https://%E4%BE%8B%E5%AD%90.example/a%20b", "ANSI URL is percent-encoded");
        Expect(Enumerated(data.Get()).size() == 3, "text + two URL formats");
    }
    {
        // Image: CF_HDROP comes from the worker; the stored CF_HDROP of the entry is not duplicated.
        const auto file = work / L"LumaShot-test.png";
        { std::ofstream(file, std::ios::binary) << "png"; }
        CardDragPayload payload;
        payload.formats = DibEntry().formats;
        payload.file = [file] { Sleep(30); return file; };
        auto data = MakeCardDataObject(payload);
        Expect(Offers(data.Get(), CF_HDROP) && Offers(data.Get(), CF_DIB), "image offers CF_DIB and CF_HDROP");
        Entry dropped;
        dropped.formats.push_back({CF_HDROP, Get(data.Get(), CF_HDROP)});
        const auto paths = FilePaths(dropped);
        Expect(paths.size() == 1 && paths[0] == file, "CF_HDROP waits for the worker and names the PNG");
        CardDragPayload failed;
        failed.file = [] { return std::filesystem::path{}; };
        auto none = MakeCardDataObject(failed);
        Expect(Get(none.Get(), CF_HDROP).empty(), "a failed encode offers no file instead of a bad path");
    }
}

void Rules() {
    ReleaseContext c;
    c.kind = Kind::Image;
    c.desktop = true;
    c.pin_available = true;
    Expect(DecideRelease(c) == DragRelease::Intercept && ResolveRelease(c, CardDragResult::Intercepted) == ReleaseAction::Pin, "image on desktop: pin");
    c.alt = true;
    Expect(DecideRelease(c) == DragRelease::Default && ResolveRelease(c, CardDragResult::Dropped) == ReleaseAction::Insert, "Alt: Explorer saves the PNG");
    c.alt = false;
    c.pin_available = false;
    Expect(DecideRelease(c) == DragRelease::Default, "no pin handler: Explorer saves the PNG");
    c = {};
    c.kind = Kind::Text;
    c.desktop = true;
    Expect(DecideRelease(c) == DragRelease::Intercept && ResolveRelease(c, CardDragResult::Intercepted) == ReleaseAction::SaveText, "text on desktop: .txt");
    c.kind = Kind::Text;
    Expect(DecideRelease(c) == DragRelease::Intercept && ResolveRelease(c, CardDragResult::Intercepted) == ReleaseAction::SaveText, "link on desktop: .url");
    c.kind = Kind::Files;
    Expect(DecideRelease(c) == DragRelease::Default, "files on desktop: Explorer copies them");
    c = {};
    c.kind = Kind::Text;
    Expect(DecideRelease(c) == DragRelease::Default && ResolveRelease(c, CardDragResult::Dropped) == ReleaseAction::Insert, "into an app: the app inserts");
    Expect(ResolveRelease(c, CardDragResult::NoTarget) == ReleaseAction::CopyFallback, "refused elsewhere: copy");
    Expect(ResolveRelease(c, CardDragResult::Canceled) == ReleaseAction::None && ResolveRelease(c, CardDragResult::Failed) == ReleaseAction::None, "Esc / failure: nothing");
    c.over_stack = true;
    c.desktop = false;
    Expect(DecideRelease(c) == DragRelease::Intercept && ResolveRelease(c, CardDragResult::Intercepted) == ReleaseAction::MoveToTop, "back on the stack: move to top");
    Expect(ResolveRelease(c, CardDragResult::NoTarget) == ReleaseAction::MoveToTop, "no target over the stack: move to top");
    // Ghost poses and cursor hints (strip-concepts.html concept D).
    const auto rest = ZonePose(DragZone::None), stack = ZonePose(DragZone::Stack), desk = ZonePose(DragZone::Desktop);
    Expect(rest.rotation == -3 && rest.scale == 1.03f && rest.opacity == 1 && !rest.outline, "ghost rests at rotate(-3deg) scale(1.03)");
    Expect(stack.rotation == 0 && stack.scale == .92f && stack.opacity == .8f && !stack.outline, "near the stack: rotate(0) scale(.92) opacity .8");
    Expect(desk.rotation == -3 && desk.scale == 1.03f && desk.outline, "desktop: accent outline");
    Expect(ZonePose(DragZone::App).scale == 1.03f && !ZonePose(DragZone::App).outline, "apps keep the resting pose");
    HintContext h;
    h.kind = Kind::Image;
    h.pin_available = true;
    Expect(ZoneHint(DragZone::Desktop, h) == L"松手：钉到桌面（按住 Alt 存为文件）", "image hint offers the pin");
    h.alt = true;
    Expect(ZoneHint(DragZone::Desktop, h) == L"松手：在桌面存为 .png", "Alt switches the hint to a file");
    h = {};
    Expect(ZoneHint(DragZone::Desktop, h) == L"松手：在桌面存为 .txt", "text hint");
    h.link = true;
    Expect(ZoneHint(DragZone::Desktop, h) == L"松手：在桌面存为 .url", "link hint");
    h.kind = Kind::Files;
    Expect(ZoneHint(DragZone::Desktop, h) == L"松手：复制到桌面", "files hint");
    Expect(ZoneHint(DragZone::Stack, h) == L"松手：放回并置顶" && ZoneHint(DragZone::None, h) == L"拖到程序或桌面", "stack and empty hints match the HTML");
    h.app = L"项目笔记.txt - 记事本";
    Expect(ZoneHint(DragZone::App, h) == L"松手：插入到「项目笔记.txt - 记事本」", "app hint names the window");
    h.app = std::wstring(40, L'长');
    Expect(ZoneHint(DragZone::App, h).size() == std::wstring(L"松手：插入到「」").size() + 24, "long titles are shortened");
    h.app.clear();
    Expect(ZoneHint(DragZone::App, h) == L"松手：插入", "untitled windows");
    Expect(IsDesktopClass(L"Progman", true) && IsDesktopClass(L"WorkerW", true), "Progman / WorkerW with DefView are the desktop");
    Expect(!IsDesktopClass(L"WorkerW", false) && !IsDesktopClass(L"CabinetWClass", true) && !IsDesktopClass(L"Shell_TrayWnd", false), "folders, taskbar and empty WorkerW are not");
}

void Files(const std::filesystem::path& work) {
    Expect(SafeFileName(L"  会议 纪要: Q3/Q4?  \r\n第二行") == L"会议 纪要 Q3 Q4", "names: first line, invalid characters removed");
    Expect(SafeFileName(L"con") == L"_con" && SafeFileName(L"LPT1.txt") == L"_LPT1.txt", "reserved device names are prefixed");
    Expect(SafeFileName(L"...") == L"剪贴板文本" && SafeFileName(L"") == L"剪贴板文本", "empty names get a default");
    Expect(SafeFileName(std::wstring(200, L'字')).size() == 60 && SafeFileName(L"end. ") == L"end", "length capped, trailing dots trimmed");
    Expect(LinkTitle(L" https://user@www.Example.org:8080/path?q=1 ") == L"Example.org:8080" && LinkTitle(L"mailto:a@b.c") == L"mailto:a@b.c", "link titles use the host");
    Expect(UrlFileContents(L"https://a.example/x") == "[InternetShortcut]\r\nURL=https://a.example/x\r\n", ".url contents");
    const auto folder = work / L"desktop";
    std::error_code ec;
    std::filesystem::remove_all(folder, ec);
    std::filesystem::create_directories(folder, ec);
    const auto text = TextEntry(L"全部内容\r\n第二行 full text beyond the summary");
    const auto a = SaveTextFile(text, folder, L"会议纪要", false);
    const auto b = SaveTextFile(text, folder, L"会议纪要", false);
    Expect(a && a->filename() == L"会议纪要.txt" && b && b->filename() == L"会议纪要 (2).txt", "existing files are never overwritten");
    Expect(a && ReadAll(*a) == "\xEF\xBB\xBF\xE5\x85\xA8\xE9\x83\xA8\xE5\x86\x85\xE5\xAE\xB9\r\n\xE7\xAC\xAC\xE4\xBA\x8C\xE8\xA1\x8C full text beyond the summary",
           ".txt is UTF-8 with BOM and the full text");
    const auto link = SaveTextFile(TextEntry(L"  https://docs.example.com/guide \r\n", Kind::Text), folder, LinkTitle(L"https://docs.example.com/guide"), true);
    Expect(link && link->filename() == L"docs.example.com.url" && ReadAll(*link) == "[InternetShortcut]\r\nURL=https://docs.example.com/guide\r\n", "link becomes a .url shortcut");
    Expect(!SaveTextFile(text, folder / L"missing", L"x", false), "missing folder fails cleanly");
    Entry fallback;
    fallback.text = L"摘要";
    Expect(EntryText(fallback) == L"摘要", "entries without CF_UNICODETEXT use their text");

    // DROPFILES parsing (wide, two paths).
    Entry files;
    files.kind = Kind::Files;
    const std::wstring list = std::wstring(L"C:\\a\\一.txt") + L'\0' + L"D:\\b" + L'\0' + L'\0';
    std::vector<unsigned char> drop(sizeof(DROPFILES) + list.size() * sizeof(wchar_t));
    DROPFILES header{};
    header.pFiles = sizeof(DROPFILES);
    header.fWide = TRUE;
    std::memcpy(drop.data(), &header, sizeof(header));
    std::memcpy(drop.data() + sizeof(header), list.data(), list.size() * sizeof(wchar_t));
    files.formats.push_back({CF_HDROP, drop});
    const auto paths = FilePaths(files);
    Expect(paths.size() == 2 && paths[0] == L"C:\\a\\一.txt" && paths[1] == L"D:\\b", "CF_HDROP paths parsed");
    files.formats[0].bytes.resize(sizeof(DROPFILES) - 1);
    Expect(FilePaths(files).empty(), "truncated DROPFILES is rejected");
}

void Images(const std::filesystem::path& work) {
    const auto folder = work / L"images";
    std::error_code ec;
    std::filesystem::remove_all(folder, ec);
    const auto dib = DibEntry();
    const auto png = WriteImageFile(dib, folder);
    Expect(png && png->extension() == L".png" && png->filename().wstring().rfind(L"LumaShot-", 0) == 0, "DIB is encoded to LumaShot-*.png (24 h cleanup pattern)");
    if (png) {
        ComPtr<IWICImagingFactory> wic;
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        UINT w{}, h{};
        GUID container{};
        const bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
                        SUCCEEDED(wic->CreateDecoderFromFilename(png->c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)) &&
                        SUCCEEDED(decoder->GetContainerFormat(&container)) && SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&w, &h));
        Expect(ok && container == GUID_ContainerFormatPng && w == 2 && h == 2, "written PNG decodes at 2x2");
        // Stored PNG bytes are written as-is.
        Entry stored;
        stored.kind = Kind::Image;
        const std::string bytes = ReadAll(*png);
        stored.formats.push_back({RegisterClipboardFormatW(L"PNG"), std::vector<unsigned char>(bytes.begin(), bytes.end())});
        const auto copy = WriteImageFile(stored, folder);
        Expect(copy && *copy != *png && ReadAll(*copy) == bytes, "stored PNG is written byte for byte, never over another file");
    }
    const auto frame = DecodeFrame(dib);
    Expect(frame && frame->Width() == 2 && frame->Height() == 2, "pin frame keeps the full size");
    if (frame && frame->pixels.size() == 4) {
        Expect((frame->pixels[0] & 0xffffff) == 0xff0000 && (frame->pixels[1] & 0xffffff) == 0x00ff00 && (frame->pixels[2] & 0xffffff) == 0x0000ff &&
                   (frame->pixels[3] & 0xffffff) == 0xffffff,
               "pin frame is top-down BGRA");
        Expect((frame->pixels[0] >> 24) == 0xff && (frame->pixels[3] >> 24) == 0xff, "pin frame is opaque");
    }
    Expect(!DecodeFrame(TextEntry(L"x")), "text entries decode to no frame");
}
}  // namespace

int main() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const auto work = std::filesystem::current_path() / L"card-drag";
    std::error_code ec;
    std::filesystem::create_directories(work, ec);
    DataObjects(work);
    Rules();
    Files(work);
    Images(work);
    std::filesystem::remove_all(work, ec);
    CoUninitialize();
    std::cout << "CARD_DRAG checks=" << checks << " failures=" << failures << '\n';
    return failures ? 1 : 0;
}
