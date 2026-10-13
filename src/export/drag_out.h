#pragma once
#include "capture/frame.h"
#include <functional>
#include <memory>
#include <windows.h>

namespace lumashot {
// Dragging an image out of LumaShot into another program (chat input boxes,
// browsers, Word, Explorer...) with system OLE drag and drop. The data object
// offers CF_HDROP (an image file in %TEMP%\LumaShot-Clipboard, cleaned up by
// CleanupClipboardFiles), CF_DIB and "PNG". The file and the DIB are encoded on a worker thread
// as soon as the drag starts; a target that asks earlier waits for them.
// LumaShot registers no drop targets, so its own windows (pins, the
// clothesline) never accept the drop: releasing over them, or over a window
// that refuses the data, reports NoTarget and the caller keeps its normal
// drop behavior at the release point.
enum class DragOutResult{Dropped,NoTarget,Canceled,Failed};
struct DragOutOptions {
    // Produces the exported picture (annotations already applied). Called
    // once, on a worker thread.
    std::function<std::shared_ptr<const Frame>()> image;
    // Clipboard file format setting (0 PNG, 1 JPEG, 2 BMP); < 0 means PNG.
    int file_format{-1};
    // Called on the UI thread whenever the cursor moves (screen pixels);
    // `over_target` is true while the window under the cursor accepts the drop.
    std::function<void(POINT cursor,bool over_target)> moved;
};
// Runs the modal drag loop on the calling (UI, STA) thread; the left button
// must be held. Initializes OLE on that thread on first use (kept for the
// lifetime of the process).
DragOutResult DragImageOut(DragOutOptions options);
}
