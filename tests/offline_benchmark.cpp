#include "translate/engine.h"
#include "translate/offline.h"
#include <windows.h>
#include <chrono>
#include <iostream>

// Explicit opt-in benchmark of the application translation path (translate::Run)
// with the built-in offline engine: cold start (server launch + model load),
// then warm jobs with fresh text, then an exact repeat (memory cache).
// Usage: lumashot_offline_benchmark [offline-root]   (default: the real install)
// Synthetic UI text only; never reads or writes application preferences.
int wmain(int argc, wchar_t** argv) {
    using namespace lumashot::translate;
    if (argc > 2) { std::cerr << "Usage: lumashot_offline_benchmark [offline-root]\n"; return 2; }
    if (argc == 2) SetEnvironmentVariableW(L"LUMASHOT_OFFLINE_ROOT", argv[1]);
    const auto* model = offline::FindModel(offline::DefaultModel);
    if (!offline::RuntimeInstalled() || !offline::ModelInstalled(*model)) { std::cerr << "offline model/runtime not installed\n"; return 1; }
    Credentials credentials; credentials.preset = FindPreset("offline"); credentials.model = std::string(offline::DefaultModel);
    const Job a{{L"File  Edit  View  Window  Help", L"Your changes have been saved to the cloud. Other people with access can see the latest version.",
        L"Search settings", L"Automatically check for updates and install them when the computer is idle.",
        L"Storage is almost full. Free up space by removing large files you no longer need, or move them to an external drive.",
        L"Learn more", L"Remind me later", L"Clean up now", L"Privacy and security",
        L"Apps can request access to your camera, microphone and location. You can change these permissions at any time.",
        L"Manage devices connected to this account"}, Language::English, Language::ChineseSimplified};
    const Job b{{L"Download complete. The installer will close running applications before continuing.", L"Restart now", L"Restart later",
        L"Release notes", L"Two-factor authentication adds an extra layer of protection to your account by requiring a code from your phone.",
        L"Set up", L"Skip for now", L"Battery saver turns on automatically at 20 percent to extend battery life.", L"Brightness",
        L"Night light schedule"}, Language::English, Language::ChineseSimplified};
    const Job c{{L"Keyboard shortcuts", L"Press Ctrl+Shift+S to capture a region of the screen and pin it on top of other windows.",
        L"Export as PNG", L"The selected file could not be opened because it is being used by another program.", L"Try again",
        L"Open folder", L"Sign out of all devices", L"Your subscription renews on the first day of every month unless you cancel it before then."},
        Language::English, Language::ChineseSimplified};
    offline::Shutdown();
    int failures = 0;
    auto run = [&](const char* name, const Job& job) {
        const auto start = std::chrono::steady_clock::now();
        const auto result = Run(credentials, job, {});
        const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        const bool ok = result.ok && result.texts.size() == job.texts.size();
        failures += !ok;
        std::cout << name << " ms=" << static_cast<long long>(ms) << " ok=" << ok << " items=" << job.texts.size();
        if (!ok) { std::wcout << L" error=" << result.error; }
        std::cout << std::endl;
        if (ok) { std::string sample; for (size_t i = 0; i < 2 && i < result.texts.size(); ++i) { const auto& w = result.texts[i]; int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr); std::string u(size_t(n), '\0'); WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), u.data(), n, nullptr, nullptr); sample += "  | " + u; } std::cout << "  sample" << sample << std::endl; }
    };
    run("cold(start+job A)", a);
    if (const auto free = offline::FreeVideoMemory()) std::cout << "free_vram_mib=" << (*free >> 20) << " "; else std::cout << "free_vram_mib=n/a ";
    std::cout << "server_cpu_only=" << offline::ServerCpuOnly() << std::endl;
    run("warm(job B)", b);
    run("warm(job C)", c);
    run("repeat(job A, cache)", a);
    offline::Shutdown();
    std::cout << (failures ? "FAILED" : "OK") << std::endl;
    return failures ? 1 : 0;
}
