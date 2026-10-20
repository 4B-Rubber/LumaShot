#pragma once
// Dragging a clipboard card out of LumaShot (docs/clipboard-card-stack.md 3.5).
// System OLE drag and drop with the entry's own formats, offered verbatim.
// Images add CF_HDROP for a PNG written on a worker as the drag starts
// (%TEMP%\LumaShot-Clipboard\LumaShot-*.png, removed by the 24 h cleanup).
// LumaShot registers no drop targets; the caller decides at release time
// whether to intercept (desktop -> pin / text file, back onto the stack).
#include <windows.h>
#include <ole2.h>
#include <wrl/client.h>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "capture/frame.h"
#include "clipboard/history.h"

namespace lumashot::clipboard {

enum class DragRelease { Default, Intercept };  // Intercept: DRAGDROP_S_CANCEL, caller acts
enum class CardDragResult { Dropped, Intercepted, NoTarget, Canceled, Failed };

// Premultiplied BGRA, top-down. offset: cursor position inside the image.
struct DragImage {
    int width{}, height{};
    POINT offset{};
    std::vector<uint32_t> pixels;
};
struct CardDragPayload {
    std::vector<Format> formats;                    // from the loaded entry, offered as stored
    std::function<std::filesystem::path()> file;    // optional single CF_HDROP file, produced on a worker
    std::wstring url;                               // links: UniformResourceLocator(W) as well
};
struct CardDragOptions {
    std::function<DragRelease(POINT cursor, DWORD keys)> on_release;  // UI thread, button released
    std::function<void(POINT cursor, bool over_target, bool alt)> moved;  // pointer or feedback changed
    DragImage image;                                // shell drag image; empty when the caller draws its own ghost
};
// Modal drag loop on the calling STA thread; the left button must be held.
CardDragResult DragCard(CardDragPayload payload, CardDragOptions options);

// ---- Release rules (pure, tested).
enum class ReleaseAction { None, Insert, Pin, SaveText, MoveToTop, CopyFallback };
struct ReleaseContext {
    Kind kind{Kind::Text};
    bool desktop{}, alt{}, over_stack{}, pin_available{};
};
DragRelease DecideRelease(const ReleaseContext& context);
ReleaseAction ResolveRelease(const ReleaseContext& context, CardDragResult result);

// ---- Feedback while dragging (strip-concepts.html, concept D: .ghost.gcard / .cursorhint).
enum class DragZone { None, App, Desktop, Stack };
struct GhostPose {
    float rotation{-3}, scale{1.03f}, opacity{1};
    bool outline{};  // 2 px accent outline over the desktop
};
GhostPose ZonePose(DragZone zone);
struct HintContext {
    Kind kind{Kind::Text};
    bool link{}, alt{}, pin_available{};
    std::wstring app;  // title of the window under the pointer (App zone)
};
std::wstring ZoneHint(DragZone zone, const HintContext& context);

// ---- Helpers (tested).
Microsoft::WRL::ComPtr<IDataObject> MakeCardDataObject(CardDragPayload payload);
// Explorer's desktop: root class Progman / WorkerW that hosts SHELLDLL_DefView.
bool IsDesktopClass(std::wstring_view root_class, bool hosts_defview);
bool IsDesktopAt(POINT screen);
// File-system safe, readable name from a card title (no extension).
std::wstring SafeFileName(std::wstring_view title, size_t limit = 60);
std::filesystem::path UniqueFile(const std::filesystem::path& folder, const std::wstring& stem, const std::wstring& extension);
std::string UrlFileContents(std::wstring_view url);
// Full text of a loaded entry (CF_UNICODETEXT), falling back to the summary.
std::wstring EntryText(const Entry& loaded);
// Writes the entry text (UTF-8 with BOM) or a .url shortcut into `folder`.
std::optional<std::filesystem::path> SaveTextFile(const Entry& loaded, const std::filesystem::path& folder, const std::wstring& title, bool link);
// PNG bytes as stored, or the DIB re-encoded as PNG. `folder` empty: LumaShot-Clipboard.
std::optional<std::filesystem::path> WriteImageFile(const Entry& loaded, std::filesystem::path folder = {});
// Paths in the entry's CF_HDROP (DROPFILES), in order.
std::vector<std::filesystem::path> FilePaths(const Entry& loaded);
// Host of an http(s) link as a file title ("example.com"), else the text.
std::wstring LinkTitle(std::wstring_view url);
// Decoded picture (opaque BGRA) for a pin; nullptr when the entry has no image.
std::shared_ptr<const Frame> DecodeFrame(const Entry& loaded);

}  // namespace lumashot::clipboard
