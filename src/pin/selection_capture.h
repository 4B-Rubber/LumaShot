#pragma once
#include <windows.h>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Selection translation: reads the text the user has selected in another
// application. UI Automation (TextPattern) is asked first because it leaves the
// clipboard untouched; applications without it get a synthetic Ctrl+C whose
// clipboard change is undone right after the text has been read.
namespace lumashot::selection {
inline constexpr size_t MaxChars = 5000;
inline constexpr size_t MaxParagraphs = 64;

// CRLF/CR → LF, NUL and other control characters removed, outer blank space
// trimmed, at most `max_chars` characters (never splitting a surrogate pair).
std::wstring Normalize(std::wstring_view text, size_t max_chars = MaxChars);
// Translation units: blank lines separate paragraphs, list items ("- ", "• ",
// "1. ") start their own unit. Lines inside a paragraph are joined later by
// the engine. Empty input → no units.
std::vector<std::wstring> Paragraphs(std::wstring_view text);

// All global-memory clipboard formats, for undoing a synthetic copy. GDI handle
// formats (CF_BITMAP, metafiles, palettes) are skipped; Windows re-synthesises
// them from CF_DIB / CF_UNICODETEXT.
class ClipboardSnapshot {
public:
    struct Item { UINT format{}; std::vector<unsigned char> bytes; };
    // nullopt: the clipboard could not be opened or holds more than max_bytes.
    static std::optional<ClipboardSnapshot> Take(HWND owner, size_t max_bytes = size_t(256) << 20);
    // Restores the formats and tags them "ExcludeClipboardContentFromMonitorProcessing"
    // so clipboard histories (including LumaShot's) do not record the restore.
    bool Restore(HWND owner) const;
    const std::vector<Item>& Items() const { return items_; }
private:
    std::vector<Item> items_;
};

enum class Source { None, Automation, Copy };
struct Captured { std::wstring text; Source source{Source::None}; };

// Focused element of another process via UI Automation, on a worker thread;
// nullopt when unsupported, nothing selected, or `timeout` elapsed.
std::optional<std::wstring> FromAutomation(std::chrono::milliseconds timeout);
// Synthetic Ctrl+C into the foreground window of another process. Waits for
// the hotkey's modifiers to be released, reads CF_UNICODETEXT and restores
// the previous clipboard. Only sent messages are dispatched while waiting.
std::optional<std::wstring> FromCopy(HWND owner);
// UI Automation, then the copy fallback. Text is normalised.
Captured Capture(HWND owner);
}
