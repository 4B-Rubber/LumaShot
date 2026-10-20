#include "pin/selection_translator.h"
#include "pin/selection_capture.h"
#include "pin/translation_panel.h"
#include "translate/service.h"
#include <array>

namespace lumashot {
namespace {
constexpr UINT ReadyMessage = WM_APP + 1;
constexpr UINT_PTR WatchTimer = 1;
constexpr UINT WatchInterval = 40;
constexpr std::array<int, 5> MouseButtons{VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2};
}

struct SelectionTranslator::Impl {
    HWND owner{}, events{};
    Host host;
    std::unique_ptr<TranslationPanel> panel;
    std::unique_ptr<translate::Service> service;
    TranslationView view;
    uint64_t job{};
    bool pinned{}, escape{};
    std::array<bool, MouseButtons.size()> buttons{};

    static LRESULT CALLBACK Proc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self) {
            if (message == ReadyMessage) { self->Finished(static_cast<uint64_t>(wp)); return 0; }
            if (message == WM_TIMER && wp == WatchTimer) { self->Watch(); return 0; }
            if (message == WM_NCDESTROY) { SetWindowLongPtrW(window, GWLP_USERDATA, 0); self->events = nullptr; }
        }
        return DefWindowProcW(window, message, wp, lp);
    }

    void EnsureWindows() {
        if (!events) {
            WNDCLASSW wc{};
            wc.lpfnWndProc = Proc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = L"LumaShot.SelectionTranslate";
            RegisterClassW(&wc);
            events = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, this);
        }
        if (panel && panel->Window()) return;
        TranslationPanel::Callbacks callbacks;
        callbacks.retarget = [this](translate::Language language) {
            view.requested = language;
            if (view.state == TranslationView::State::Idle) Refresh();
            else Start();
        };
        callbacks.retry = [this] { Start(); };
        callbacks.settings = [this] { if (host.open_settings) host.open_settings(); };
        callbacks.closed = [this] { HideBubble(); };
        callbacks.pinned = [this](bool value) {
            pinned = value;
            Refresh();
            if (pinned) { if (events) KillTimer(events, WatchTimer); }
            else Arm();
        };
        panel = std::make_unique<TranslationPanel>(owner, std::move(callbacks));
    }

    void Refresh() {
        if (!panel) return;
        view.text_mode = true;
        view.pinned = pinned;
        view.dark = host.dark && host.dark();
        panel->Update(view);
    }

    void CancelJob() {
        if (job && service) service->Cancel(job);
        job = 0;
    }

    void Start() {
        using State = TranslationView::State;
        CancelJob();
        view.results.clear();
        view.error.clear();
        if (view.sources.empty()) { view.state = State::Idle; view.error = L"先选中文字，再按快捷键"; Refresh(); return; }
        const auto config = translate::LoadConfig(translate::DefaultConfigPath());
        std::wstring reason;
        auto credentials = translate::Resolve(config, &reason);
        if (!credentials) { view.state = State::NeedsSetup; view.error = reason; view.engine.clear(); Refresh(); return; }
        view.engine = std::wstring(credentials->preset->label);
        std::wstring all;
        for (const auto& text : view.sources) { all += text; all += L'\n'; }
        view.source = translate::Detect(all);
        view.target = translate::ResolveTarget(view.requested != translate::Language::Auto ? view.requested : config.target, view.source);
        translate::Job request;
        request.source = view.source;
        request.target = view.target;
        request.texts = view.sources;
        if (!service) service = std::make_unique<translate::Service>(events, ReadyMessage);
        job = service->Submit(std::move(*credentials), std::move(request));
        view.state = State::Running;
        Refresh();
    }

    void Finished(uint64_t id) {
        if (!service || !id) return;
        auto outcome = service->Take(id);
        if (!outcome || id != job) return;
        job = 0;
        using State = TranslationView::State;
        if (!outcome->ok || outcome->texts.size() != view.sources.size()) {
            view.state = State::Failed;
            view.error = outcome->error.empty() ? L"翻译服务没有返回结果" : outcome->error;
        } else {
            view.results = std::move(outcome->texts);
            view.state = State::Done;
        }
        Refresh();
    }

    // Present the bubble for a new request. A pinned bubble keeps its place.
    void Present(POINT cursor) {
        const bool keep = pinned && panel && panel->Visible();
        if (!keep) { pinned = false; panel->PlaceNear(cursor); }
    }
    void Reveal() {
        panel->Show();
        if (!pinned) Arm();
    }

    // Outside-click / Escape detection while the bubble is up. Polling keeps the
    // target application's input untouched (no hooks, Escape is not swallowed)
    // and only runs while an unpinned bubble is visible.
    void Arm() {
        if (!events) return;
        escape = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
        for (size_t i = 0; i < MouseButtons.size(); ++i) buttons[i] = (GetAsyncKeyState(MouseButtons[i]) & 0x8000) != 0;
        SetTimer(events, WatchTimer, WatchInterval, nullptr);
    }
    void Watch() {
        if (!panel || !panel->Visible() || pinned) { KillTimer(events, WatchTimer); return; }
        const SHORT esc = GetAsyncKeyState(VK_ESCAPE);
        const bool esc_down = (esc & 0x8000) != 0;
        if (esc_down && !escape) { HideBubble(); return; }
        escape = esc_down;
        POINT cursor{};
        GetCursorPos(&cursor);
        RECT bounds{};
        GetWindowRect(panel->Window(), &bounds);
        const bool outside = !PtInRect(&bounds, cursor);
        for (size_t i = 0; i < MouseButtons.size(); ++i) {
            const SHORT state = GetAsyncKeyState(MouseButtons[i]);
            const bool down = (state & 0x8000) != 0;
            // The low bit also catches a tap shorter than one polling interval.
            const bool pressed = (down && !buttons[i]) || (!down && !buttons[i] && (state & 1));
            buttons[i] = down;
            if (pressed && outside) { HideBubble(); return; }
        }
    }

    void HideBubble() {
        CancelJob();
        if (events) KillTimer(events, WatchTimer);
        if (panel) panel->Hide();
        pinned = false;
    }
};

SelectionTranslator::SelectionTranslator(HWND owner, Host host) : impl_(std::make_unique<Impl>()) {
    impl_->owner = owner;
    impl_->host = std::move(host);
}

SelectionTranslator::~SelectionTranslator() {
    if (impl_->events) KillTimer(impl_->events, WatchTimer);
    impl_->service.reset();
    impl_->panel.reset();
    if (impl_->events) DestroyWindow(impl_->events);
}

void SelectionTranslator::Translate(std::wstring text, POINT cursor) {
    impl_->EnsureWindows();
    if (!impl_->panel->Window()) return;
    impl_->Present(cursor);
    impl_->view.sources = selection::Paragraphs(text);
    impl_->Start();
    impl_->Reveal();
}

void SelectionTranslator::Explain(std::wstring reason, POINT cursor) {
    impl_->EnsureWindows();
    if (!impl_->panel->Window()) return;
    impl_->Present(cursor);
    impl_->CancelJob();
    auto& view = impl_->view;
    view.state = TranslationView::State::Idle;
    view.sources.clear();
    view.results.clear();
    view.engine.clear();
    view.error = std::move(reason);
    impl_->Refresh();
    impl_->Reveal();
}

void SelectionTranslator::ConfigChanged() {
    using State = TranslationView::State;
    if (Visible() && (impl_->view.state == State::NeedsSetup || impl_->view.state == State::Failed)) impl_->Start();
}

void SelectionTranslator::Hide() { impl_->HideBubble(); }
bool SelectionTranslator::Visible() const { return impl_->panel && impl_->panel->Visible(); }
HWND SelectionTranslator::Window() const { return impl_->panel ? impl_->panel->Window() : nullptr; }
bool SelectionTranslator::Pinned() const { return impl_->pinned; }
const TranslationView& SelectionTranslator::View() const { return impl_->view; }
}
