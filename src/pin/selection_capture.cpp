#include "pin/selection_capture.h"
#include <ole2.h>
#include <UIAutomation.h>
#include <oleauto.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <system_error>
#include <thread>

namespace lumashot::selection {
using Microsoft::WRL::ComPtr;
namespace {
bool Blank(wchar_t c) { return c == L' ' || c == L'\n' || c == L'\t' || c == 0x3000 || c == 0x00a0; }

std::wstring_view Trim(std::wstring_view text) {
    while (!text.empty() && Blank(text.front())) text.remove_prefix(1);
    while (!text.empty() && Blank(text.back())) text.remove_suffix(1);
    return text;
}

// "- item", "* item", "• item", "1. item", "2) item", "3、item"
bool ListItem(std::wstring_view line) {
    line = Trim(line);
    if (line.size() >= 2 && (line[0] == L'-' || line[0] == L'*' || line[0] == L'\x2022' || line[0] == L'\x00b7' || line[0] == L'\x2013') && line[1] == L' ') return true;
    size_t digits = 0;
    while (digits < line.size() && digits < 3 && line[digits] >= L'0' && line[digits] <= L'9') ++digits;
    if (!digits || digits >= line.size()) return false;
    const wchar_t mark = line[digits];
    if (mark == L'\x3001') return true;
    return (mark == L'.' || mark == L')') && digits + 1 < line.size() && line[digits + 1] == L' ';
}

// Waits while dispatching only messages sent from other threads: a clipboard
// owner (possibly this process) must still answer WM_RENDERFORMAT and
// WM_DESTROYCLIPBOARD, but posted input stays queued for the main loop.
template <class Done> bool WaitFor(Done done, DWORD milliseconds) {
    const ULONGLONG end = GetTickCount64() + milliseconds;
    for (;;) {
        if (done()) return true;
        const ULONGLONG now = GetTickCount64();
        if (now >= end) return false;
        MSG message{};
        PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
        MsgWaitForMultipleObjectsEx(0, nullptr, static_cast<DWORD>(std::min<ULONGLONG>(10, end - now)), QS_SENDMESSAGE, 0);
    }
}

struct OpenClipboardScope {
    bool ok{};
    explicit OpenClipboardScope(HWND owner) { ok = WaitFor([&] { return OpenClipboard(owner) != FALSE; }, 400); }
    ~OpenClipboardScope() { if (ok) CloseClipboard(); }
};

bool HandleFormat(UINT format) {
    switch (format) {
    case CF_BITMAP: case CF_METAFILEPICT: case CF_PALETTE: case CF_ENHMETAFILE:
    case CF_OWNERDISPLAY: case CF_DSPBITMAP: case CF_DSPMETAFILEPICT: case CF_DSPENHMETAFILE:
        return true;
    default:
        return format >= CF_PRIVATEFIRST && format <= CF_GDIOBJLAST;
    }
}

std::optional<std::wstring> ReadAutomation() {
    ComPtr<IUIAutomation> automation;
    if (FAILED(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation))) || !automation) return std::nullopt;
    ComPtr<IUIAutomationElement> focused;
    if (FAILED(automation->GetFocusedElement(&focused)) || !focused) return std::nullopt;
    int process{};
    if (SUCCEEDED(focused->get_CurrentProcessId(&process)) && static_cast<DWORD>(process) == GetCurrentProcessId()) return std::nullopt;
    // Never read or copy out of a password box: an empty result stops the fallback too.
    BOOL password = FALSE;
    if (SUCCEEDED(focused->get_CurrentIsPassword(&password)) && password) return std::wstring{};
    ComPtr<IUIAutomationTextPattern> pattern;
    if (FAILED(focused->GetCurrentPatternAs(UIA_TextPatternId, IID_PPV_ARGS(&pattern))) || !pattern) return std::nullopt;
    ComPtr<IUIAutomationTextRangeArray> ranges;
    if (FAILED(pattern->GetSelection(&ranges)) || !ranges) return std::nullopt;
    int count{};
    if (FAILED(ranges->get_Length(&count))) return std::nullopt;
    std::wstring text;
    for (int i = 0; i < count && i < 32 && text.size() < MaxChars * 2; ++i) {
        ComPtr<IUIAutomationTextRange> range;
        if (FAILED(ranges->GetElement(i, &range)) || !range) continue;
        BSTR value = nullptr;
        if (SUCCEEDED(range->GetText(static_cast<int>(MaxChars * 2), &value)) && value) {
            if (!text.empty()) text.push_back(L'\n');
            text.append(value, SysStringLen(value));
        }
        if (value) SysFreeString(value);
    }
    if (Normalize(text).empty()) return std::nullopt;
    return text;
}

// Terminals treat Ctrl+C without a selection as an interrupt; never send it there.
bool Terminal(HWND window) {
    wchar_t name[128]{};
    if (!GetClassNameW(window, name, static_cast<int>(std::size(name)))) return false;
    for (const wchar_t* terminal : {L"ConsoleWindowClass", L"CASCADIA_HOSTING_WINDOW_CLASS", L"mintty", L"VirtualConsoleClass", L"PuTTY", L"KiTTY", L"org.wezfurlong.wezterm", L"Alacritty"})
        if (_wcsicmp(name, terminal) == 0) return true;
    return false;
}

bool ModifiersDown() {
    for (int key : {VK_CONTROL, VK_MENU, VK_SHIFT, VK_LWIN, VK_RWIN})
        if (GetAsyncKeyState(key) & 0x8000) return true;
    return false;
}

INPUT Key(WORD key, bool up) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = key;
    input.ki.wScan = static_cast<WORD>(MapVirtualKeyW(key, MAPVK_VK_TO_VSC));
    input.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
    return input;
}

// Modifiers of the hotkey that are still held would turn Ctrl+C into e.g.
// Ctrl+Alt+C. Give the user a moment, then release them synthetically; an
// unassigned key first keeps a lone Alt/Win release from opening menus.
void ReleaseModifiers() {
    if (WaitFor([] { return !ModifiersDown(); }, 700)) return;
    std::vector<INPUT> inputs{Key(0xE8, false), Key(0xE8, true)};
    for (WORD key : {WORD(VK_LMENU), WORD(VK_RMENU), WORD(VK_LSHIFT), WORD(VK_RSHIFT), WORD(VK_LWIN), WORD(VK_RWIN), WORD(VK_LCONTROL), WORD(VK_RCONTROL)})
        if (GetAsyncKeyState(key) & 0x8000) inputs.push_back(Key(key, true));
    SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
    WaitFor([] { return false; }, 30);
}

std::optional<std::wstring> ReadText(HWND owner) {
    OpenClipboardScope open(owner);
    if (!open.ok) return std::nullopt;
    const HANDLE handle = GetClipboardData(CF_UNICODETEXT);
    if (!handle) return std::nullopt;
    const SIZE_T bytes = GlobalSize(handle);
    const auto* data = static_cast<const wchar_t*>(GlobalLock(handle));
    if (!data) return std::nullopt;
    const size_t chars = std::min<size_t>(bytes / sizeof(wchar_t), MaxChars * 4);
    std::wstring text(data, std::find(data, data + chars, L'\0'));
    GlobalUnlock(handle);
    return text;
}
}

std::wstring Normalize(std::wstring_view text, size_t max_chars) {
    std::wstring out;
    out.reserve(std::min(text.size(), max_chars + 1));
    for (size_t i = 0; i < text.size() && out.size() <= max_chars + 1; ++i) {
        const wchar_t c = text[i];
        if (c == L'\r') { out.push_back(L'\n'); if (i + 1 < text.size() && text[i + 1] == L'\n') ++i; continue; }
        if (c == 0x2028 || c == 0x2029) { out.push_back(L'\n'); continue; }
        if (c == L'\t' || c == 0x00a0) { out.push_back(L' '); continue; }
        if (c == L'\n') { out.push_back(c); continue; }
        if (c < 0x20 || c == 0x7f || c == 0xfeff) continue;
        out.push_back(c);
    }
    auto view = Trim(out);
    if (view.size() > max_chars) {
        view = view.substr(0, max_chars);
        if (!view.empty() && view.back() >= 0xd800 && view.back() <= 0xdbff) view.remove_suffix(1);
        view = Trim(view);
    }
    return std::wstring(view);
}

std::vector<std::wstring> Paragraphs(std::wstring_view text) {
    const auto normalized = Normalize(text);
    std::vector<std::wstring> units;
    std::wstring current;
    const auto flush = [&] {
        const auto trimmed = Trim(current);
        if (!trimmed.empty()) units.emplace_back(trimmed);
        current.clear();
    };
    size_t begin = 0;
    while (begin <= normalized.size()) {
        size_t end = normalized.find(L'\n', begin);
        if (end == std::wstring::npos) end = normalized.size();
        const std::wstring_view line(normalized.data() + begin, end - begin);
        // Keep the unit count bounded: everything past the limit stays in the last unit.
        const bool room = units.size() + 1 < MaxParagraphs;
        if (Trim(line).empty()) { if (room) flush(); else if (!current.empty()) current.push_back(L'\n'); }
        else {
            if (room && ListItem(line)) flush();
            if (!current.empty()) current.push_back(L'\n');
            current.append(Trim(line));
        }
        begin = end + 1;
    }
    flush();
    return units;
}

std::optional<ClipboardSnapshot> ClipboardSnapshot::Take(HWND owner, size_t max_bytes) {
    OpenClipboardScope open(owner);
    if (!open.ok) return std::nullopt;
    ClipboardSnapshot snapshot;
    size_t total = 0;
    for (UINT format = EnumClipboardFormats(0); format; format = EnumClipboardFormats(format)) {
        if (HandleFormat(format)) continue;
        const HANDLE handle = GetClipboardData(format);
        if (!handle) continue;
        const SIZE_T size = GlobalSize(handle);
        if (!size) continue;
        total += size;
        if (total > max_bytes) return std::nullopt;
        const auto* data = static_cast<const unsigned char*>(GlobalLock(handle));
        if (!data) continue;
        snapshot.items_.push_back({format, std::vector<unsigned char>(data, data + size)});
        GlobalUnlock(handle);
    }
    return snapshot;
}

bool ClipboardSnapshot::Restore(HWND owner) const {
    OpenClipboardScope open(owner);
    if (!open.ok || !EmptyClipboard()) return false;
    bool ok = true;
    const auto put = [&](UINT format, const void* data, size_t size) {
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, std::max<size_t>(size, 1));
        void* target = memory ? GlobalLock(memory) : nullptr;
        if (!target) { if (memory) GlobalFree(memory); ok = false; return; }
        if (size) std::memcpy(target, data, size);
        GlobalUnlock(memory);
        if (!SetClipboardData(format, memory)) { GlobalFree(memory); ok = false; }
    };
    for (const auto& item : items_) put(item.format, item.bytes.data(), item.bytes.size());
    const DWORD exclude = 0;
    put(RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing"), &exclude, sizeof(exclude));
    return ok;
}

std::optional<std::wstring> FromAutomation(std::chrono::milliseconds timeout) {
    struct Shared {
        std::mutex mutex;
        std::condition_variable ready;
        bool done{};
        std::optional<std::wstring> text;
    };
    auto shared = std::make_shared<Shared>();
    try {
        // A hung target can block UI Automation for seconds; the worker is
        // detached so a late answer is simply dropped.
        std::thread([shared] {
            std::optional<std::wstring> text;
            const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            if (SUCCEEDED(com)) {
                try { text = ReadAutomation(); } catch (...) {}
                CoUninitialize();
            }
            std::lock_guard lock(shared->mutex);
            shared->text = std::move(text);
            shared->done = true;
            shared->ready.notify_all();
        }).detach();
    } catch (const std::system_error&) {
        return std::nullopt;
    }
    std::unique_lock lock(shared->mutex);
    if (!shared->ready.wait_for(lock, timeout, [&] { return shared->done; })) return std::nullopt;
    return std::move(shared->text);
}

std::optional<std::wstring> FromCopy(HWND owner) {
    const HWND foreground = GetForegroundWindow();
    DWORD process{};
    if (!foreground || !GetWindowThreadProcessId(foreground, &process) || process == GetCurrentProcessId() || Terminal(foreground)) return std::nullopt;
    ReleaseModifiers();
    if (GetForegroundWindow() != foreground) return std::nullopt;
    auto snapshot = ClipboardSnapshot::Take(owner);
    if (!snapshot) return std::nullopt;
    const DWORD before = GetClipboardSequenceNumber();
    const std::array<INPUT, 4> keys{Key(VK_CONTROL, false), Key('C', false), Key('C', true), Key(VK_CONTROL, true)};
    if (SendInput(static_cast<UINT>(keys.size()), const_cast<INPUT*>(keys.data()), sizeof(INPUT)) != keys.size()) return std::nullopt;
    // Nothing selected: most applications leave the clipboard alone.
    if (!WaitFor([&] { return GetClipboardSequenceNumber() != before; }, 600)) return std::nullopt;
    WaitFor([] { return false; }, 40);
    auto text = ReadText(owner);
    snapshot->Restore(owner);
    return text;
}

Captured Capture(HWND owner) {
    if (auto automation = FromAutomation(std::chrono::milliseconds(400))) {
        if (automation->empty()) return {};
        if (auto text = Normalize(*automation); !text.empty()) return {std::move(text), Source::Automation};
    }
    if (auto copied = FromCopy(owner)) {
        if (auto text = Normalize(*copied); !text.empty()) return {std::move(text), Source::Copy};
    }
    return {};
}
}
