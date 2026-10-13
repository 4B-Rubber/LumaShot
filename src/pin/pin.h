#pragma once
#include "ocr/service.h"
#include "export/clipboard.h"
#include "ui/render.h"
#include "model/pin_style.h"
#include "pin/session.h"
#include "app/deferred_writer.h"
#include "pin/clothesline.h"
#include <future>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
namespace lumashot::translate{class Service;}
namespace lumashot {
constexpr UINT kOcrReady=WM_APP+20,kPinSaveReady=WM_APP+21,kTranslateReady=WM_APP+22,kPinSessionError=WM_APP+89;
class PinManager {
public:
    explicit PinManager(HWND host);
    ~PinManager();
    void Create(Frame image,Frame ocr_image,POINT position,std::optional<Frame> base=std::nullopt,Document annotations={},bool recognize=true,const PinSessionRecord* restored=nullptr);
    bool EnableSession(const std::filesystem::path& directory);
    bool FlushSession();
    void PreserveSession();
    void Ready();void Saved();
    void RefreshAppearance();
    void SetOcrModelDirectory(const std::filesystem::path& path){service_.SetModelDirectory(path);}
    // Starts the OCR worker while the user selects a region for translation.
    void PrewarmOcr(){service_.Prewarm();}
    void CompleteAnnotation(uint64_t id,std::optional<Frame> image,Document annotations={},std::optional<Frame> base=std::nullopt,bool recognize=true);
    std::function<bool(uint64_t,std::shared_ptr<const Frame>,Document,RECT)> annotate;
    std::function<bool()> dark_theme;
    std::function<int()> clipboard_format;
    std::function<PinStyle()> sticker_style;
    // Whether a finished translation replaces the pin image right away (default on).
    std::function<bool()> translate_auto_show;
    // Settings switch for the top-edge clothesline (default on). After it
    // changes call ClotheslineConfigChanged: off sends hung pins home.
    std::function<bool()> clothesline_enabled;
    void ClotheslineConfigChanged();
    // Screenshot translation: translate the most recently created pin (toolbar
    // button / translate hotkey), deliver finished jobs, and retry pins that were
    // waiting for an engine after the translation settings were saved.
    void TranslateLast();
    void TranslationReady(uint64_t job);
    void TranslationConfigChanged();
    std::function<void()> open_translation_settings;
private:
    friend struct PinTest;
    friend struct PinSessionLifecycleTest;
    friend struct ClotheslineLiveTest;
    std::vector<PinSessionRecord> SessionSnapshot()const;
    void SessionChanged();
    std::unique_ptr<PinSessionStore> session_store_;
    std::unique_ptr<DeferredWriter<std::vector<PinSessionRecord>>> session_writer_;
    bool restoring_session_{},preserving_session_{},preserve_ok_{true};
    std::atomic_bool session_warning_{};
    struct Pin;
    static LRESULT CALLBACK Proc(HWND,UINT,WPARAM,LPARAM);
    static LRESULT CALLBACK ToolsProc(HWND,UINT,WPARAM,LPARAM);
    void UpdateTools(Pin&);
    void ToolCommand(Pin&,int);
    void Annotate(Pin&);
    void ToggleLock(Pin&);
    void ApplyPaper(Pin&,Point);
    void Paint(Pin& pin);
    void Zoom(Pin&,int delta,POINT anchor);
    void Draw(Pin& pin,ID2D1RenderTarget* target,const D2D1_RECT_F* clip=nullptr);
    void PaintTools(Pin& pin);
    void Recognize(Pin& pin);
    void Menu(Pin& pin,POINT point);
    void Save(Pin& pin);
    void Copy(Pin& pin);
    void TranslatePin(Pin& pin);
    void TranslateKey(Pin& pin);
    void StartOrWait(Pin& pin);
    void StartTranslation(Pin& pin);
    void ResetTranslation(Pin& pin);
    void ShowTranslation(Pin& pin,bool show);
    void EnsurePanel(Pin& pin);
    void UpdatePanel(Pin& pin);
    void PlacePanel(Pin& pin);
    Pin* FindPin(uint64_t id);
    // Clothesline: hang a pin (hidden window + thumbnail on the rope) and take it back.
    // keep_home: a card pulled off the rope goes back up; its home stays where it was hung from.
    void HangPin(Pin&,bool keep_home=false);
    bool DropOnLine(Pin&);
    void EndLineDrag(Pin&);
    void RetrievePin(uint64_t id,RECT from);
    void DetachPin(uint64_t id,POINT cursor);
    void FlyStep(Pin&);
    // System drag and drop of the pin picture into another program: Ctrl+drag
    // on a pin, or (without Shift) a card pulled off the rope.
    void DragOut(Pin&);
    // Hover buttons on the pin (hang, close): fade in/out and the fading close.
    void UpdateHoverTools(Pin&);
    void HoverToolsStep(Pin&);
    void FadeClose(Pin&);
    struct SaveResult {uint64_t id{};std::wstring error;std::unique_ptr<ClipboardImage> clipboard_image;};
    HWND host_;ocr::Service service_;uint64_t next_id_{1};
    std::map<uint64_t,std::unique_ptr<Pin>> pins_;
    std::vector<std::future<SaveResult>> saves_;
    bool processing_saves_{};
    std::unique_ptr<Clothesline> line_;
    bool LineOn()const{return line_&&(!clothesline_enabled||clothesline_enabled());}
    std::unique_ptr<translate::Service> translator_;uint64_t last_created_{};bool settings_prompted_{};
};
}