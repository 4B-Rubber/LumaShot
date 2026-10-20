#pragma once
#include <windows.h>
#include <functional>
#include <memory>
#include <string>

// Selection translation bubble: translates text the user selected (in another
// application or in a pin's OCR text) and shows it in a TranslationPanel in
// text mode next to the cursor. Unless pinned, the bubble closes on Escape or
// a click outside it; it never takes focus from the application underneath.
namespace lumashot {
struct TranslationView;
class SelectionTranslator {
public:
    struct Host {
        std::function<bool()> dark;
        std::function<void()> open_settings;
    };
    SelectionTranslator(HWND owner, Host host);
    ~SelectionTranslator();
    SelectionTranslator(const SelectionTranslator&) = delete;
    SelectionTranslator& operator=(const SelectionTranslator&) = delete;
    // `cursor` = mouse position in physical screen pixels.
    void Translate(std::wstring text, POINT cursor);
    // Nothing could be read: the bubble shows `reason` instead of a result.
    void Explain(std::wstring reason, POINT cursor);
    // Translation settings were saved: retry a bubble waiting for an engine.
    void ConfigChanged();
    void Hide();
    bool Visible() const;
    HWND Window() const;
    bool Pinned() const;
    // Test hook: what the bubble currently shows.
    const TranslationView& View() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
