#pragma once
#include "translate/engine.h"
#include <windows.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// Side-by-side original/translation panel attached to a pin. It never takes
// focus (WS_EX_NOACTIVATE), follows the pin, and is destroyed with it.
namespace lumashot {
struct TranslationView {
    enum class State { Idle, NeedsSetup, WaitingOcr, Running, Done, Failed };
    State state{State::Idle};
    std::wstring engine, error;
    translate::Language source{translate::Language::Auto}, target{translate::Language::ChineseSimplified};
    translate::Language requested{translate::Language::Auto}; // user choice for this pin (Auto = rule)
    std::vector<std::wstring> sources, results;
    bool show_translation{true}, dark{};
    // Selection translation: no image to toggle, a pin (keep open) switch
    // instead, Idle shows `error` as the reason nothing was read, and the
    // height follows the content.
    bool text_mode{}, pinned{};
};

class TranslationPanel {
public:
    struct Callbacks {
        std::function<void(bool)> show_translation;
        std::function<void(translate::Language)> retarget;
        std::function<void()> retry, settings, closed;
        std::function<void(bool)> pinned;   // text mode only
    };
    TranslationPanel(HWND owner, Callbacks callbacks);
    ~TranslationPanel();
    TranslationPanel(const TranslationPanel&) = delete;
    TranslationPanel& operator=(const TranslationPanel&) = delete;
    void Update(const TranslationView& view);
    // anchor = the pin's visible paper in screen pixels.
    void Place(RECT anchor);
    // Text mode: below-right of a screen point (physical pixels), flipped to
    // stay inside the work area. Later height changes keep this anchor until
    // the user drags the panel by its header.
    void PlaceNear(POINT point);
    void Show();
    void Hide();
    bool Visible() const;
    HWND Window() const;
    // Test hooks: logical (DIP) hit ids and the text that a copy button yields.
    std::wstring CopyText(bool bilingual) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}