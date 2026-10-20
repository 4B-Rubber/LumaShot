// Net memory of the folded card stack versus the classic strip (S1 part 2).
// Same synthetic fixtures and flow as clipboard_resource_audit; one style per
// process: lumashot_clipboard_stack_memprobe.exe classic|stack. Never touches
// the OS clipboard; the store lives under the working directory (build/).
#include "../src/clipboard/panel.cpp"
#include "export/png.h"
#include <psapi.h>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>

namespace lumashot {
namespace {
using Clock = std::chrono::steady_clock;
void Require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
double Ms(Clock::time_point start) { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); }
void Pump() {
    MSG m{};
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}
void Wait(double ms) {
    const auto start = Clock::now();
    while (Ms(start) < ms) { Pump(); Sleep(20); }
}
void Sample(const char* style, const char* stage) {
    PROCESS_MEMORY_COUNTERS_EX m{};
    m.cb = sizeof(m);
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&m), sizeof(m));
    std::cout << "MEMORY style=" << style << " stage=" << stage << " private_mib=" << m.PrivateUsage / 1048576.0
              << " working_mib=" << m.WorkingSetSize / 1048576.0 << std::endl;
}
uint64_t CpuTime() {
    FILETIME c{}, e{}, k{}, u{};
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    return (static_cast<uint64_t>(k.dwHighDateTime) << 32 | k.dwLowDateTime) + (static_cast<uint64_t>(u.dwHighDateTime) << 32 | u.dwLowDateTime);
}
clipboard::Entry Text(int i) {
    clipboard::Entry e;
    e.kind = clipboard::Kind::Text;
    e.text = std::wstring(128 * 1024, L'x') + std::to_wstring(i);
    const auto* b = reinterpret_cast<const unsigned char*>(e.text.c_str());
    e.formats.push_back({CF_UNICODETEXT, {b, b + (e.text.size() + 1) * sizeof(wchar_t)}});
    return e;
}
clipboard::Entry Dib(int i) {
    clipboard::Entry e;
    e.kind = clipboard::Kind::Image;
    e.text = L"synthetic DIB " + std::to_wstring(i);
    BITMAPINFOHEADER h{};
    h.biSize = sizeof(h);
    h.biWidth = 512;
    h.biHeight = -512;
    h.biPlanes = 1;
    h.biBitCount = 32;
    std::vector<unsigned char> bytes(sizeof(h) + 512 * 512 * 4);
    std::memcpy(bytes.data(), &h, sizeof(h));
    const uint32_t color = 0xff4080a0u + static_cast<uint32_t>(i);
    for (size_t j = sizeof(h); j < bytes.size(); j += 4) std::memcpy(bytes.data() + j, &color, 4);
    e.formats.push_back({CF_DIB, std::move(bytes)});
    return e;
}
clipboard::Entry Png(int i, const std::filesystem::path& path) {
    {
        auto frame = MakeFrame({0, 0, 3840, 2160});
        for (int y = 0; y < 2160; ++y)
            for (int x = 0; x < 3840; ++x)
                frame.pixels[static_cast<size_t>(y) * 3840 + x] =
                    0xff000000u | ((static_cast<uint32_t>(x / 16 + i) & 255) << 16) | ((static_cast<uint32_t>(y / 8) & 255) << 8) | 0x80u;
        SavePng(frame, path);
    }
    std::ifstream file(path, std::ios::binary);
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(file)), {});
    clipboard::Entry e;
    e.kind = clipboard::Kind::Image;
    e.text = L"synthetic 4K PNG " + std::to_wstring(i);
    e.formats.push_back({RegisterClipboardFormatW(L"PNG"), std::move(bytes)});
    return e;
}
}  // namespace

struct ClipboardPanelTest {
    // late: the stack is created only after the history fill (isolates update cost).
    static int Run(bool use_stack, bool late) {
        const char* style = !use_stack ? "classic" : late ? "stacklate" : "stack";
        const auto root = std::filesystem::current_path() / (L"memprobe-" + std::to_wstring(GetCurrentProcessId()));
        std::filesystem::create_directories(root);
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code ec; std::filesystem::remove_all(path, ec); }
        } cleanup{root};
        ClipboardPanel panel(nullptr, [] {});
        auto& p = *panel.impl_;
        p.test_mode = true;
        p.stack_style = use_stack && !late;
        std::cout << std::fixed << std::setprecision(2);
        Sample(style, "cold");
        Require(p.Enable(true, false), "enable isolated panel");
        Require(!use_stack || late || p.stack, "stack created");
        Sample(style, "enabled");
        p.disk = std::make_unique<clipboard::SessionStore>(root / L"store");
        Require(p.disk->WaitIdle(), "worker initialization");
        const auto save = [&](clipboard::Entry e) { Require(p.disk->Save(std::move(e)), "queue fixture"); Require(p.disk->WaitIdle(), "save fixture"); p.DrainStore(); };
        for (int i = 0; i < 80; ++i) save(Text(i));
        for (int i = 0; i < 16; ++i) save(Dib(i));
        for (int i = 0; i < 4; ++i) save(Png(i, root / (L"fixture-" + std::to_wstring(i) + L".png")));
        Require(p.history.entries.size() == 100, "100 history entries");
        if (p.stack) std::cout << "STACK fill surface_draws=" << p.stack->Inspect().surface_draws << " pending_thumbs=" << p.thumbnail_pending.size() << std::endl;
        Wait(300);
        Sample(style, "history100");
        if (use_stack && late) { p.stack_style = true; p.CreateStack(); Require(p.stack != nullptr, "late stack created"); p.Place(); Wait(300); Sample(style, "stack_created"); }
        const auto settle = [&] { Require(p.disk->WaitIdle(), "worker settles"); p.DrainStore(); };
        {   // Realistic use: 50 more copies, one per message-loop turn, stack visible.
            if (p.stack) p.Place();
            for (int i = 0; i < 50; ++i) {
                Require(p.disk->Save(i % 10 == 9 ? Dib(100 + i) : Text(100 + i)), "queue live copy");
                Require(p.disk->WaitIdle(), "save live copy");
                p.DrainStore();
                Wait(40);
            }
            settle(); Wait(600);
            if (p.stack) std::cout << "STACK live surface_draws=" << p.stack->Inspect().surface_draws << std::endl;
            Sample(style, "live150");
        }
        p.expanded = true; p.scale = 1; p.Filter(); p.Place(); p.Present(); settle(); p.Present(); Pump();
        Sample(style, "panel_open");
        p.Fold(); settle(); Pump(); settle(); Pump();
        if (!use_stack) { ShowWindow(p.folded_window, SW_SHOWNOACTIVATE); p.Present(); Pump(); }
        Wait(400);
        if (use_stack) {
            Require(p.stack->Visible() && p.stack->Inspect().cards == 5, "five cards shown");
            int thumbs = 0;
            for (bool has : p.stack_has_thumb) thumbs += has ? 1 : 0;
            std::cout << "STACK thumbs=" << thumbs << " thumbnail_cache_bytes=" << p.thumbnail_cache.Bytes() << std::endl;
        }
        Sample(style, "folded_visible");
        Wait(11000);  // real 10 s trim timer
        Sample(style, "folded_trimmed");
        if (!use_stack) {
            Require(p.composition != nullptr, "classic strip keeps its composition until the idle handoff");
            p.ReleaseRenderer(); p.EnterIdleStrip(); Pump(); Wait(300);
            Sample(style, "folded_idle_layered");
        } else {
            Require(!p.composition && !p.parked_composition && !p.target, "stack mode released the panel renderer");
            Sample(style, "folded_idle");
        }
        {
            Pump();
            const auto before = CpuTime();
            Sleep(1000);
            Pump();
            std::cout << "IDLE style=" << style << " process_cpu_ms=" << (CpuTime() - before) / 10000.0 << std::endl;
        }
        p.Enable(false, false);
        return 0;
    }
};
}  // namespace lumashot

int main(int argc, char** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int result = 1;
    try {
        const std::string mode = argc > 1 ? argv[1] : "classic";
        result = lumashot::ClipboardPanelTest::Run(mode != "classic", mode == "stacklate");
    } catch (const std::exception& e) {
        std::cerr << "FAIL " << e.what() << std::endl;
    }
    CoUninitialize();
    return result;
}
