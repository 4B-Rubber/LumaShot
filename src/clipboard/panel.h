#pragma once
#include <windows.h>
#include <string>
#include <functional>
#include <memory>
namespace lumashot {
struct Frame;
// Image-card actions a host can provide to the clipboard panel.
enum class ClipboardImageAction { Ocr, Annotate };
class ClipboardPanel {
public:
    ClipboardPanel(HWND owner,std::function<void()> settings,std::function<bool()> hotkey_allowed = {});
    ~ClipboardPanel();
    ClipboardPanel(const ClipboardPanel&)=delete;
    ClipboardPanel& operator=(const ClipboardPanel&)=delete;
    bool SetShortcut(UINT modifiers,UINT key);
    bool Enable(bool enabled,bool dark);
    // false: history is session-only and previously saved history is deleted.
    bool SetPersistent(bool persistent);
    void Show();
    void SetHotkeysSuspended(bool suspended);
    // The folded edge strip can be hidden while history and the shortcut keep working.
    void SetStripVisible(bool visible);
    // Called after the user drags the strip onto the close target (strip now hidden).
    void SetStripDismissedHandler(std::function<void()> handler);
    // 最多保留: ordinary entries kept (favorites and grouped entries are extra); lowering trims now.
    void SetHistoryLimit(size_t entries);
    // Folded form: card stack (default) or the classic thin strip; switches immediately.
    void SetStripStyle(bool classic);
    // false: stack cards show only their type tile (no thumbnails or text).
    void SetCardPreview(bool show);
    // Card stack: an image card dropped on the desktop becomes a pin at `screen`.
    // Without a handler (or with Alt held) Explorer saves the PNG instead.
    void SetPinHandler(std::function<bool(std::shared_ptr<const Frame> image,POINT screen)> handler);
    // Image-card actions the host provides (OCR, annotate then pin); the buttons hide while unset.
    // Result of a ClipboardImageAction::Ocr request (empty: nothing recognised); shown in Quick Look.
    void SetImageOcrResult(std::wstring text);
    void SetImageActionHandler(std::function<bool(ClipboardImageAction action,std::shared_ptr<const Frame> image,POINT screen)> handler);
private:
    friend struct ClipboardPanelTest;
    struct Impl;std::unique_ptr<Impl> impl_;
};
}