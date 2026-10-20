#pragma once
// Edge card stack (docs/clipboard-card-stack.md section 3): the folded form of
// the clipboard. Two windows share one rectangle:
//  - visual: layered + click-through, Windows.UI.Composition tree with a
//    HostBackdrop glass per card, CSS-equivalent shadows, no region (a region
//    would clip the shadows);
//  - input: invisible (no content), region = the card shapes, owns all mouse
//    input and the one-shot hover/leave timers.
// Every animation runs in DWM. Idle cost: no timer, no loop, no wake-ups.
#include <windows.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "clipboard/stack_layout.h"
#include "clipboard/card_drag.h"

namespace lumashot::clipboard {

enum class StackTile : unsigned char { Image, Text, Code, Link, Color, File };

struct StackItem {
    uint64_t id{};
    StackTile tile{StackTile::Text};
    std::wstring title, subtitle;
    std::wstring glyph;   // tile text: "Aa", "</>", link initial, file extension
    uint32_t accent{};    // link avatar / colour swatch / file badge (0xRRGGBB)
    // Image tiles: premultiplied BGRA, any size; drawn cover-cropped.
    int thumb_width{}, thumb_height{};
    std::vector<uint32_t> thumb;
};

struct StackEvents {
    std::function<void(size_t index)> activate;          // click a card: paste it
    std::function<void()> open_panel;                     // click the grip
    std::function<void(RECT window)> moved;               // grip drag released
    std::function<void(POINT screen)> dragging;           // grip drag step (close-target tracking)
    std::function<void()> move_cancelled;                 // Esc: stack returned to its place
    std::function<bool(size_t index, POINT screen)> drag; // S2: card drag-out; false = not handled
    std::function<void(int steps)> scroll;                // wheel over the open stack: + older, - newer
    std::function<void()> folded;                         // the open stack folded (leave, grip, Collapse)
};

class StackWindow {
public:
    StackWindow(HWND owner, StackEvents events);
    ~StackWindow();
    StackWindow(const StackWindow&) = delete;
    StackWindow& operator=(const StackWindow&) = delete;

    // false when Windows.UI.Composition is unavailable; keep the classic strip.
    bool Create();
    // items: at most stack::MaxCards consecutive history entries, newest first; total = history
    // size; first = history index of items[0] (wheel paging: badges and the more line follow it).
    // incoming: the first item is new and slides in.
    void SetItems(std::vector<StackItem> items, size_t total, bool incoming, size_t first = 0);
    void SetTheme(bool dark);
    void SetGlass(bool glass);   // false: opaque token fill (transparency off, Windows 10)
    void SetPaused(bool paused);
    void SetMotion(bool enabled);  // SPI_GETCLIENTAREAANIMATION
    // Row badge text = prefix + index ("Alt+1" in the HTML). Empty: index only.
    void SetBadgePrefix(std::wstring prefix);
    // Row card as a drag image (premultiplied BGRA); empty when unavailable.
    DragImage RenderDragImage(size_t index);
    int DraggingIndex() const;  // -1 unless a card drag-out is running
    // Drag ghost: the dragged card plus the cursor hint, in click-through
    // windows that exist only between BeginGhost and EndGhost.
    bool BeginGhost(const StackItem& item, POINT cursor);
    void MoveGhost(POINT cursor, const GhostPose& pose, const std::wstring& hint);
    void EndGhost();
    struct GhostInfo {
        bool active{}, animating{};
        GhostPose pose, target;
        std::wstring hint;
        RECT card{}, hint_rect{};
    };
    GhostInfo InspectGhost() const;
    // Physical pixels; grip_top is the classic strip's stored top.
    void Place(stack::Side side, LONG grip_top, RECT work, float scale);
    void Show(bool visible);
    void Collapse();
    // Fold <-> expand continuity (docs section 4.3): the stack cards fly to / from the open
    // panel's card slots (physical pixels, card order). Runs in DWM; the visual window grows
    // to cover the path until the flight ends. false: nothing to fly (no motion, hidden, empty).
    bool Fly(const std::vector<RECT>& slots, bool toward_panel);
    bool Flying() const;

    bool Visible() const;
    bool Fanned() const;
    stack::Side Side() const;
    RECT WindowRect() const;
    HWND InputWindow() const;
    HWND VisualWindow() const;

    struct Diagnostics {
        bool glass{}, graphics{}, moving{};
        int cards{}, timers{}, hovered{-1};
        int thumbnail_cards{};  // cards whose tile shows real image pixels
        unsigned surface_draws{};
        HRESULT last_error{S_OK};  // last composition failure (glass dropped)
        int last_error_line{};
    };
    Diagnostics Inspect() const;

private:
    friend struct StackWindowTest;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace lumashot::clipboard