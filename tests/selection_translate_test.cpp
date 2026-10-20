#include "pin/selection_capture.h"
#include "pin/selection_translator.h"
#include "pin/translation_panel.h"
#include "app/hotkeys.h"
#include "translate/engine.h"
#include "export/png.h"
#include <objbase.h>
#include <cmath>
#include <filesystem>
#include <iostream>

// Selection translation: text normalisation and paragraph split, the text-mode
// bubble geometry, and the bubble lifecycle without any engine or network.
// The real clipboard and foreground applications are never touched here.
using namespace lumashot;

static int failures = 0;
static void Expect(bool ok, const char* name) { std::cout << (ok ? "PASS " : "FAIL ") << name << std::endl; failures += !ok; }

static RECT WindowRect(HWND window) { RECT r{}; GetWindowRect(window, &r); return r; }
static RECT WorkArea(POINT point) {
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST), &monitor);
    return monitor.rcWork;
}
static bool Inside(RECT inner, RECT outer) { return inner.left >= outer.left && inner.top >= outer.top && inner.right <= outer.right && inner.bottom <= outer.bottom; }
// Real window pixels for a visual check (synthetic text only).
static void Snapshot(HWND window, const wchar_t* file) {
    RECT r{};
    GetWindowRect(window, &r);
    const int w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return;
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    BITMAPINFO info{};
    info.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(dc, bitmap);
    PrintWindow(window, dc, PW_RENDERFULLCONTENT);
    auto frame = MakeFrame({0, 0, w, h}, 0xffffffff);
    const auto* source = static_cast<const uint32_t*>(bits);
    for (size_t i = 0; i < frame.pixels.size(); ++i) frame.pixels[i] = source[i] | 0xff000000;
    SelectObject(dc, old);
    DeleteObject(bitmap);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    try { SavePng(frame, file); } catch (...) {}
}
static void Pump() { MSG message{}; while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); } }

int main() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    // Synthetic, unconfigured engine file: never the user's translation.ini.
    const auto config = std::filesystem::current_path() / L"selection-translate-test.ini";
    { translate::Config empty; translate::SaveConfig(config, empty); }
    SetEnvironmentVariableW(L"LUMASHOT_TRANSLATION_CONFIG", config.c_str());

    using selection::Normalize;
    using selection::Paragraphs;
    Expect(Normalize(L"  \r\n Hello\r\nworld \t\r\n") == L"Hello\nworld", "CRLF folded, outer blank space trimmed");
    Expect(Normalize(L"a\rb\x2029" L"c\x0007\xfeff") == L"a\nb\nc", "CR / paragraph separator become LF, control characters dropped");
    Expect(Normalize(L"\x3000\x00a0 中文 \x3000") == L"中文", "ideographic and no-break spaces trimmed");
    Expect(Normalize(std::wstring(6000, L'x')).size() == selection::MaxChars, "long selections capped");
    {
        std::wstring pair = std::wstring(9, L'a') + L"\xd83d\xde00";
        const auto capped = Normalize(pair, 10);
        Expect(capped == std::wstring(9, L'a'), "cap never splits a surrogate pair");
    }
    Expect(Paragraphs(L"").empty() && Paragraphs(L" \r\n \n").empty(), "blank selection has no units");
    Expect(Paragraphs(L"one line") == std::vector<std::wstring>{L"one line"}, "single line is one unit");
    Expect(Paragraphs(L"first line\nwraps here\n\nsecond paragraph") == std::vector<std::wstring>{L"first line\nwraps here", L"second paragraph"}, "blank lines separate paragraphs");
    Expect(Paragraphs(L"Steps:\n- open\n- close\n1. save\n2) quit\n3、退出") == std::vector<std::wstring>{L"Steps:", L"- open", L"- close", L"1. save", L"2) quit", L"3、退出"}, "list items become separate units");
    {
        std::wstring many;
        for (int i = 0; i < 100; ++i) many += L"p" + std::to_wstring(i) + L"\n\n";
        const auto units = Paragraphs(many);
        Expect(units.size() == selection::MaxParagraphs && units.back().find(L"p99") != std::wstring::npos, "unit count bounded, the tail stays in the last unit");
    }

    Preferences defaults;
    Expect(defaults.selection_key == 'T' && defaults.selection_modifiers == (MOD_CONTROL | MOD_ALT), "selection translation defaults to Ctrl+Alt+T");
    Expect(UniqueShortcuts(defaults) && Shortcuts(defaults)[6] == Shortcut{MOD_CONTROL | MOD_ALT, 'T'}, "default chord unique and registered as host id 7");
    {
        const auto path = std::filesystem::current_path() / L"selection-translate-prefs.ini";
        Preferences clash;
        clash.gif_key = 'T';
        clash.SaveTo(path);
        WritePrivateProfileStringW(L"General", L"SelectionTranslateHotkey", nullptr, path.c_str());
        WritePrivateProfileStringW(L"General", L"SelectionTranslateModifiers", nullptr, path.c_str());
        const auto loaded = Preferences::LoadFrom(path);
        Expect(loaded.gif_key == 'T' && loaded.selection_key == 0, "new default yields to an existing identical chord");
        Preferences custom;
        custom.selection_key = VK_F16;
        custom.selection_modifiers = MOD_CONTROL | MOD_SHIFT;
        custom.SaveTo(path);
        const auto reloaded = Preferences::LoadFrom(path);
        Expect(reloaded.selection_key == VK_F16 && reloaded.selection_modifiers == (MOD_CONTROL | MOD_SHIFT), "custom selection chord persists");
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }

    HWND owner = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"Synthetic selection owner", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Expect(owner != nullptr, "owner window created");
    const POINT origin{0, 0};
    const RECT work = WorkArea(origin);
    {
        TranslationPanel panel(owner, {});
        const float scale = GetDpiForWindow(panel.Window()) / 96.f;
        TranslationView view;
        view.text_mode = true;
        view.state = TranslationView::State::Done;
        view.sources = {L"Hello"};
        view.results = {L"你好"};
        panel.PlaceNear({work.left + 100, work.top + 100});
        panel.Update(view);
        panel.Show();
        Pump();
        Snapshot(panel.Window(), L"selection-bubble-preview.png");
        view.dark = true;
        view.pinned = true;
        view.sources = {L"The quick brown fox jumps over the lazy dog.", L"Selection translation keeps the original text visible."};
        view.results = {L"敏捷的棕色狐狸跳过了那只懒狗。", L"划词翻译会同时显示原文。"};
        panel.Update(view);
        Pump();
        Snapshot(panel.Window(), L"selection-bubble-dark-preview.png");
        view.dark = false;
        view.pinned = false;
        view.sources = {L"Hello"};
        view.results = {L"你好"};
        panel.Update(view);
        RECT r = WindowRect(panel.Window());
        Expect(r.left > work.left + 100 && r.top > work.top + 100, "bubble opens below-right of the cursor");
        Expect(r.bottom - r.top >= int(250 * scale) - 1 && r.bottom - r.top < int(420 * scale), "short result gets a compact bubble");
        for (int i = 0; i < 40; ++i) { view.sources.push_back(L"A longer paragraph that wraps over a few lines in the bubble " + std::to_wstring(i)); view.results.push_back(L"会在气泡中换行的较长段落"); }
        panel.Update(view);
        r = WindowRect(panel.Window());
        Expect(r.bottom - r.top <= int(std::lround(560 * scale)) && Inside(r, work), "long result capped and kept on screen");
        panel.PlaceNear({work.right - 4, work.bottom - 4});
        r = WindowRect(panel.Window());
        Expect(Inside(r, work) && r.right <= work.right - 4 && r.bottom <= work.bottom - 4, "near the corner the bubble flips above-left");
        view.state = TranslationView::State::Failed;
        view.error = L"synthetic";
        panel.Update(view);
        r = WindowRect(panel.Window());
        Expect(r.bottom - r.top == int(std::lround(420 * scale)) || r.bottom - r.top == work.bottom - work.top - 2 * int(std::lround(8 * scale)), "failure card keeps room for its buttons");
    }
    {
        SelectionTranslator translator(owner, {});
        Expect(!translator.Visible() && !translator.Window(), "nothing is created before the first request");
        translator.Explain(L"先选中文字，再按快捷键", {work.left + 200, work.top + 200});
        Pump();
        Expect(translator.Visible() && translator.View().state == TranslationView::State::Idle, "explanation bubble shown");
        Expect((GetWindowLongPtrW(translator.Window(), GWL_EXSTYLE) & WS_EX_NOACTIVATE) != 0 && GetForegroundWindow() != translator.Window(), "bubble never takes focus");
        translator.Translate(L"Hello world", {work.left + 200, work.top + 200});
        Pump();
        const auto& view = translator.View();
        Expect(view.state == TranslationView::State::NeedsSetup && view.sources == std::vector<std::wstring>{L"Hello world"} && view.text_mode, "unconfigured engine asks for setup without sending anything");
        translator.ConfigChanged();
        Expect(view.state == TranslationView::State::NeedsSetup, "config change retries the waiting bubble");
        translator.Translate(L"   ", {work.left + 200, work.top + 200});
        Expect(view.state == TranslationView::State::Idle && !view.error.empty(), "blank text explains instead of translating");
        translator.Hide();
        Expect(!translator.Visible() && !translator.Pinned(), "hide closes and unpins");
    }
    DestroyWindow(owner);
    std::error_code ec;
    std::filesystem::remove(config, ec);
    CoUninitialize();
    return failures ? 1 : 0;
}
