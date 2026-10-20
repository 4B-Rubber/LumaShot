#include "clipboard/panel.h"
#include "clipboard/win_v_shortcut.h"
#include "clipboard/quick_window.h"
#include "clipboard/quick_input.h"
#include "clipboard/liquid_surface.h"
#include "clipboard/strip_dismiss.h"
#include "clipboard/stack_items.h"
#include "clipboard/panel_cards.h"
#include "clipboard/syntax_highlight.h"
#include "ui/luma_glass.h"
#include "clipboard/stack_window.h"
#include "clipboard/card_drag.h"
#include "clipboard/input_focus.h"
#include "ui/shortcut_label.h"
#include "clipboard/composition.h"
#include "clipboard/native.h"
#include "clipboard/file_icons.h"
#include "clipboard/session_store.h"
#include "clipboard/layout.h"
#include "clipboard/preview_window.h"
#include "clipboard/preview_cache.h"
#include "ui/acrylic.h"
#include "ui/brand_icon.h"
#include "ui/memory_target.h"
#include "ui/text_renderer.h"
#include <shlobj.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <wincodec.h>
#include "capture/frame.h"
#include <commctrl.h>
#include <d2d1.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <windowsx.h>
#include <imm.h>
#include <cstring>
#include <map>
#include <set>
#include <cmath>

namespace lumashot {
using Microsoft::WRL::ComPtr;
struct ClipboardPanel::Impl {
    HWND window{},native_window{},folded_window{},search{},owner{},previous{};HFONT font{};HBRUSH edit_brush{};
    std::function<void()> settings;
    std::function<bool()> hotkey_allowed;bool hotkeys_suspended{};clipboard::WinVShortcut win_v_shortcut;UINT shortcut_modifiers{MOD_WIN},shortcut_key{'V'};
    clipboard::History history;std::vector<uint64_t> visible;
    std::unique_ptr<clipboard::SessionStore> disk;bool persist_history{};
    uint64_t copy_token{},image_token{1},search_token{},preview_token{},last_read_id{};bool copy_pending{},copy_paste{},paste_inflight{};
    HWND copy_target{},paste_target{},previous_focus{},copy_focus{},paste_focus{};DWORD copy_process{},paste_process{},paste_sequence{};
    bool copy_remove_whitespace{},copy_plain{},copy_keyboard{},paste_keyboard{},paste_activated{},paste_returning{};ULONGLONG paste_return_at{};
    std::unique_ptr<clipboard::QuickWindow> quick;clipboard::QuickInput quick_input;
    std::vector<uint64_t> quick_ids;size_t quick_selection{};int quick_tab{};
    HWND quick_target{},quick_focus{};DWORD quick_process{};
    bool quick_active{},quick_continuous{},copy_quick{},paste_quick{};
    clipboard::InputFocusProbe input_focus_probe;clipboard::InputFocusSnapshot shortcut_focus;
    bool shortcut_pending{};ULONGLONG shortcut_deadline{};
    DWORD previous_process{};HWINEVENTHOOK foreground_hook{},ime_hook{};std::set<HWND> ime_windows;
    inline static std::map<HWINEVENTHOOK,Impl*> foreground_observers;
    static void CALLBACK ForegroundChanged(HWINEVENTHOOK hook,DWORD,HWND candidate,LONG,LONG,DWORD,DWORD){
        const auto it=foreground_observers.find(hook);
        if(it!=foreground_observers.end()&&candidate==GetForegroundWindow()){
            auto* self=it->second;if(self->quick_active&&candidate!=self->quick_target)self->CloseQuick();
            if(self->shortcut_pending&&candidate!=self->shortcut_focus.foreground)self->CancelShortcutProbe();
            self->RememberTarget(candidate);
        }
    }
    static void CALLBACK ImeChanged(HWINEVENTHOOK hook,DWORD event,HWND candidate,LONG,LONG,DWORD,DWORD){
        const auto it=foreground_observers.find(hook);if(it==foreground_observers.end())return;
        auto* self=it->second;
        if(event==EVENT_OBJECT_IME_HIDE)self->ime_windows.erase(candidate);
        else if(candidate){self->ime_windows.insert(candidate);self->CancelShortcutProbe();if(self->quick_active)self->CloseQuick();}
    }
    void TrackForeground(){
        if(!foreground_hook){foreground_hook=SetWinEventHook(EVENT_SYSTEM_FOREGROUND,EVENT_SYSTEM_FOREGROUND,nullptr,ForegroundChanged,0,0,WINEVENT_OUTOFCONTEXT|WINEVENT_SKIPOWNPROCESS);if(foreground_hook)foreground_observers.emplace(foreground_hook,this);}
        if(!ime_hook){ime_hook=SetWinEventHook(EVENT_OBJECT_IME_SHOW,EVENT_OBJECT_IME_HIDE,nullptr,ImeChanged,0,0,WINEVENT_OUTOFCONTEXT|WINEVENT_SKIPOWNPROCESS);if(ime_hook)foreground_observers.emplace(ime_hook,this);}
        RememberTarget(GetForegroundWindow());
    }
    void UpdateActivationPolicy(){
        const auto style=GetWindowLongPtrW(window,GWL_EXSTYLE);
        SetWindowLongPtrW(window,GWL_EXSTYLE,(pinned||window==folded_window)?(style|WS_EX_NOACTIVATE):(style&~WS_EX_NOACTIVATE));
    }
    std::vector<uint64_t> search_matches;bool search_complete{};std::wstring completed_query;
    bool enabled{},expanded{},dark{},acrylic{},custom_outline{},caret_on{},pinned{},oldest{},hotkey{},listening{},test_mode{};
    bool ime_active{};std::wstring ime_original,ime_text;DWORD ime_first{},ime_last{},ime_cursor{};
    bool suppress_search_space{},scroll_drag{};float scroll_grab{};
    bool menu_open{},menu_confirm{};int menu_hover{};
    // Custom groups: in-panel popup (group list / group actions), inline naming via the search box, scrollable tab strip.
    struct PopupItem{std::wstring label,hint;UINT32 dot{};int command{};uint32_t arg{};bool enabled{true},checked{};};
    bool popup_open{};int popup_hover{-1};uint64_t popup_entry{};uint32_t popup_group{};D2D1_RECT_F popup_rect{};std::vector<PopupItem> popup_items;std::vector<D2D1_RECT_F> popup_rows;
    int naming{};uint32_t naming_group{};uint64_t naming_entry{};std::wstring naming_query;
    float tab_scroll{},tab_scroll_max{};bool reveal_tab{};
    int tab{},scroll{},retry{},paste_wait{};uint64_t selected{};DWORD sequence{};
    std::filesystem::path layout_path;
    int snap_x{},snap_y{};HMONITOR drag_monitor{};
    std::optional<POINT> summon_cursor;POINT anchor{},drag_start{};RECT drag_rect{};bool placed{},drag_pending{},dragged{};
    float scale{1};std::wstring query,status;int hover{-1};
    TextRenderer text_renderer;uint64_t text_glyphs{};
    ComPtr<ID2D1Factory> factory;ComPtr<IDWriteFactory> writer;ComPtr<ID2D1RenderTarget> target;ComPtr<ID2D1SolidColorBrush> brush;
    BrandIcon app_icon;
    std::map<uint64_t,ComPtr<ID2D1Bitmap>> images;
    std::set<uint64_t> thumbnail_pending,thumbnail_failed;
    std::unique_ptr<clipboard::FileIconCache> file_icons;
    std::map<std::pair<std::wstring,int>,ComPtr<ID2D1Bitmap>> file_bitmaps;
    std::map<std::pair<int,bool>,ComPtr<IDWriteTextFormat>> fonts;
    struct Hit {D2D1_RECT_F rect;int action;uint64_t id;};std::vector<Hit> hits;std::optional<Hit> paste_click;
    std::unique_ptr<DibSurface> surface;int width{},height{};uint64_t search_text_glyphs{};
    std::unique_ptr<ClipboardComposition> composition,parked_composition;
    HWND idle_strip{};bool idle_layered{};
    // Hidden strip: history, the shortcut and the tray entry keep working; only the
    // edge tab is gone. Dragging the strip onto the close target hides it.
    bool strip_hidden{};std::function<void()> strip_dismissed;
    // Card stack (docs/clipboard-card-stack.md) replaces the folded strip unless
    // [Strip] Style=Classic. Test mode keeps the strip; stack tests opt in.
    std::unique_ptr<clipboard::StackWindow> stack;bool stack_style{},stack_moving{};
    // Settings: 侧边条样式 (unset: clipboard-layout.ini decides) and 卡片显示内容预览.
    std::optional<bool> classic_override;bool card_preview{true};
    std::vector<uint64_t> stack_ids;size_t stack_total{};bool stack_dark{};std::vector<bool> stack_has_thumb;
    std::map<uint64_t,std::shared_ptr<const Frame>> stack_thumbs;// downscaled; survive Fold()'s CancelReads
    bool copy_stack{},paste_stack{};
    clipboard::DismissTracker dismiss_tracker;std::unique_ptr<clipboard::DismissTarget> dismiss_target;POINT drag_anchor{};
    bool UpdateDismiss(POINT point,RECT work,HWND relative=nullptr){
        const HWND w=relative?relative:window;
        const bool armed=dismiss_tracker.Update(point,work,GetDpiForWindow(w)/96.f);
        if(dismiss_tracker.Active()&&!test_mode){
            if(!dismiss_target)dismiss_target=std::make_unique<clipboard::DismissTarget>(owner);
            dismiss_target->Show(dismiss_tracker.Zone(),dark,w);
            dismiss_target->SetState(armed,dismiss_tracker.Proximity());
        }
        return armed;
    }
    // An armed strip sits inside the target; cancelling returns it to its place.
    void CancelDismiss(){
        const bool armed=dismiss_tracker.Armed();dismiss_tracker.Reset();
        if(dismiss_target)dismiss_target->Hide();
        if(armed&&enabled&&window&&!expanded)Place();
    }
    // Nothing of the folded strip stays on screen or in the GPU while it is hidden.
    void ParkHiddenStrip(){
        if(stack)stack->Show(false);
        StopOpening(false);StopLiquidDrag(false);
        if(!window)return;
        SelectWindow();reveal_window=false;drag_pending=false;
        if(GetCapture()==window)ReleaseCapture();
        ShowWindow(window,SW_HIDE);HideIdleStrip();if(search)ShowWindow(search,SW_HIDE);
        const HWND events=EventWindow();KillTimer(events,4);KillTimer(events,9);KillTimer(events,11);
        composition.reset();parked_composition.reset();compact_positioned=false;ReleaseRenderer();ClipboardComposition::DropPrewarm();
    }
    void DismissStrip(){
        RECT shown{};const bool on_screen=liquid_surface&&window==folded_window&&IsWindowVisible(folded_window)&&GetWindowRect(folded_window,&shown);
        if(dismiss_target)dismiss_target->Dismiss(on_screen?liquid_surface->Pixels():nullptr,on_screen?liquid_surface->Width():0,on_screen?liquid_surface->Height():0,shown);
        dismiss_tracker.Reset();
        // The strip returns where it was before this drag, not inside the target.
        anchor=drag_anchor;placed=true;
        if(!SaveLayout())status=L"位置保存失败，下次启动可能无法恢复";
        strip_hidden=true;Place();
        if(strip_dismissed)strip_dismissed();
    }
    void SetStripVisible(bool show){
        if(strip_hidden==!show)return;
        strip_hidden=!show;
        if(show&&dismiss_target)dismiss_target->Reset();
        if(!enabled||!window||expanded)return;
        if(!show){CancelDismiss();Place();return;}
        Place();SetTimer(EventWindow(),4,10000,nullptr);
    }
    // After the 10 s renderer trim, a folded strip left alone this long hands off to the
    // layered copy and releases the ~28 MB D3D11 device (recreated in ~75 ms on use).
    static constexpr UINT IdleStripDelayMs=60000;
    bool switching_window{},reveal_window{};
    bool opening{};
    clipboard::LiquidTween opening_tween;
    RECT opening_work{},opening_viewport{};
    void StopOpening(bool reveal){
        if(!opening)return;
        opening=false;KillTimer(EventWindow(),10);
        ShowWindow(folded_window,SW_HIDE);
        if(liquid_surface)liquid_surface->ClearExpanded();
        if(reveal&&enabled&&expanded){ShowWindow(native_window,SW_SHOWNOACTIVATE);SetForegroundWindow(native_window);SetFocus(native_window);Invalidate();}
    }
    void OpeningTick(ULONGLONG now){
        if(!opening)return;
        if(opening_tween.Done(now)){StopOpening(true);return;}
        try{
            clipboard::LiquidFrame frame;frame.pose=opening_tween.Sample(now);
            frame.pose.bounds=clipboard::ConstrainLiquidBounds(frame.pose.bounds,opening_work);
            frame.viewport=opening_viewport;frame.work=opening_work;frame.scale=scale;frame.dark=dark;
            // Match DWMWCP_ROUND, not the 22-DIP software fallback. Otherwise
            // the last liquid frame visibly tightens when the native HWND appears.
            frame.expanded_radius_pixels=custom_outline?8.f*GetDpiForWindow(native_window)/96.f:22.f*scale;
            frame.detail=1-clipboard::LiquidSmooth((frame.pose.open-.65f)/.35f);
            liquid_surface->Render(frame);
            if(!parked_composition)parked_composition=std::make_unique<ClipboardComposition>();
            parked_composition->Present(folded_window,static_cast<UINT>(liquid_surface->Width()),static_cast<UINT>(liquid_surface->Height()),liquid_surface->Pixels());
            const HRGN region=liquid_surface->CreateInputRegion();CheckWin32(region!=nullptr,"Opening input mask");
            if(!SetWindowRgn(folded_window,region,FALSE)){DeleteObject(region);CheckWin32(false,"Opening input mask");}
        }catch(...){StopOpening(true);}
    }
    bool StartOpening(RECT from,RECT to,ULONGLONG now){
        if(!liquid_surface||!clipboard::LiquidBudget(from,to,true))return false;
        opening_work=WorkArea(to);RECT bounds{};UnionRect(&bounds,&from,&to);
        opening_viewport=clipboard::LiquidViewport(clipboard::LiquidRect(bounds),opening_work,scale);
        try{Render(to.right-to.left,to.bottom-to.top,true);liquid_surface->Cache(true,width,height,surface->Pixels());}
        catch(...){return false;}
        opening_tween.Reset(from,0,now);opening_tween.Target(to,1,now,350);opening=true;
        SetWindowPos(folded_window,HWND_TOPMOST,opening_viewport.left,opening_viewport.top,
            opening_viewport.right-opening_viewport.left,opening_viewport.bottom-opening_viewport.top,SWP_NOACTIVATE);
        OpeningTick(now);
        if(!opening)return false;
        ShowWindow(folded_window,SW_SHOWNOACTIVATE);
        if(!SetTimer(EventWindow(),10,16,nullptr)){StopOpening(true);return false;}
        return true;
    }
    HWND EventWindow()const{return native_window?native_window:window;}
    clipboard::PreviewWindow preview;
    clipboard::PreviewCache preview_cache;
    clipboard::PreviewCache thumbnail_cache{384 * 1024};
    uint64_t preview_pending{};
    float W=400,H=744,ListBottom=744-clipboard::cards::FooterHeight;bool resizing{};
    // Pixel scrolling: `scroll` stays the index of the block at the list top;
    // `scroll_offset` is the pixel offset into it and applies only while
    // offset_index==scroll, so plain index assignments keep their meaning.
    float scroll_offset{};int offset_index{-1};float scroll_target{-1};ULONGLONG scroll_tick{};
    std::vector<float> block_tops;std::vector<char> block_header;std::optional<D2D1_RECT_F> hit_clip;
    clipboard::LiquidTween liquid_drag_tween;clipboard::LiquidPull liquid_pull;
    clipboard::LiquidEdge liquid_drag_edge{clipboard::LiquidEdge::None};
    std::unique_ptr<clipboard::LiquidSurface> liquid_surface;
    bool liquid_backdrop{},liquid_failed{},liquid_drag_active{},liquid_drag_timer{},liquid_placing{},compact_positioned{};
    RECT compact_body{},compact_viewport{},liquid_drag_goal{},liquid_drag_work{};
    POINT liquid_pointer{};ULONGLONG liquid_pointer_time{};
    static bool MotionEnabled(){BOOL effects=TRUE;return SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION,0,&effects,0)&&effects;}
    RECT VisualBodyBounds()const{
        RECT actual{};GetWindowRect(window,&actual);
        return compact_positioned&&EqualRect(&actual,&compact_viewport)?compact_body:actual;
    }
    static RECT WorkArea(RECT body){
        MONITORINFO monitor{sizeof(monitor)};
        if(GetMonitorInfoW(MonitorFromRect(&body,MONITOR_DEFAULTTONEAREST),&monitor))return monitor.rcWork;
        return body;
    }
    void LiquidBackdrop(bool enabled_shape){
        if(native_window&&folded_window)return; // Fixed roles after both windows are configured.
        if(!window||liquid_backdrop==enabled_shape)return;
        liquid_backdrop=enabled_shape;
        const auto style=GetWindowLongPtrW(window,GWL_STYLE);
        SetWindowLongPtrW(window,GWL_STYLE,enabled_shape?(style&~WS_THICKFRAME):(style|WS_THICKFRAME));
        const DWMNCRENDERINGPOLICY nc=enabled_shape?DWMNCRP_DISABLED:DWMNCRP_USEWINDOWSTYLE;
        DwmSetWindowAttribute(window,DWMWA_NCRENDERING_POLICY,&nc,sizeof(nc));
        const MARGINS margins{enabled_shape?0:-1};DwmExtendFrameIntoClientArea(window,&margins);
        if(!enabled_shape)SetWindowRgn(window,nullptr,FALSE);
        SetWindowPos(window,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED);
        const DWORD corner=enabled_shape?1:2,backdrop=enabled_shape?1:3;
        DwmSetWindowAttribute(window,33,&corner,sizeof(corner));
        const bool supported=SUCCEEDED(DwmSetWindowAttribute(window,38,&backdrop,sizeof(backdrop)));
        if(!enabled_shape)acrylic=supported;
    }
    // Idle folded strip: the same premultiplied pixels shown by a click-through
    // layered window directly below folded_window, so the D3D11/DComp device can be
    // released while the strip just sits at the screen edge. folded_window stays
    // visible and keeps its input region; without a composition it draws nothing.
    bool ShowIdleStrip(){
        if(!liquid_surface||!folded_window||window!=folded_window||!IsWindowVisible(folded_window))return false;
        RECT bounds{};GetWindowRect(folded_window,&bounds);
        const int w=liquid_surface->Width(),h=liquid_surface->Height();
        if(w<=0||h<=0||bounds.right-bounds.left!=w||bounds.bottom-bounds.top!=h||!liquid_surface->Pixels())return false;
        if(!idle_strip){
            WNDCLASSEXW wc{sizeof(wc)};wc.lpfnWndProc=DefWindowProcW;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"LumaShotClipboardIdleStrip";
            if(!RegisterClassExW(&wc)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)return false;
            idle_strip=CreateWindowExW(WS_EX_LAYERED|WS_EX_TRANSPARENT|WS_EX_TOOLWINDOW|WS_EX_TOPMOST|WS_EX_NOACTIVATE,wc.lpszClassName,L"LumaShot 剪贴板侧边条",WS_POPUP,
                bounds.left,bounds.top,w,h,owner,nullptr,wc.hInstance,nullptr);
            if(!idle_strip)return false;
            const BOOL yes=TRUE;DwmSetWindowAttribute(idle_strip,DWMWA_TRANSITIONS_FORCEDISABLED,&yes,sizeof(yes));
        }
        BITMAPINFO info{};info.bmiHeader={sizeof(BITMAPINFOHEADER),w,-h,1,32,BI_RGB};
        const HDC screen=GetDC(nullptr);if(!screen)return false;
        const HDC memory=CreateCompatibleDC(screen);void* bits=nullptr;
        const HBITMAP bitmap=memory?CreateDIBSection(memory,&info,DIB_RGB_COLORS,&bits,nullptr,0):nullptr;
        BOOL shown=FALSE;
        if(bitmap&&bits){
            std::memcpy(bits,liquid_surface->Pixels(),static_cast<size_t>(w)*static_cast<size_t>(h)*4);
            const auto old=SelectObject(memory,bitmap);
            POINT destination{bounds.left,bounds.top},source{};SIZE size{w,h};BLENDFUNCTION blend{AC_SRC_OVER,0,255,AC_SRC_ALPHA};
            shown=UpdateLayeredWindow(idle_strip,screen,&destination,&size,memory,&source,0,&blend,ULW_ALPHA);
            SelectObject(memory,old);
        }
        if(bitmap)DeleteObject(bitmap);if(memory)DeleteDC(memory);ReleaseDC(nullptr,screen);
        if(!shown)return false;
        SetWindowPos(idle_strip,folded_window,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_SHOWWINDOW);
        return true;
    }
    void HideIdleStrip(){idle_layered=false;if(idle_strip&&IsWindowVisible(idle_strip))ShowWindow(idle_strip,SW_HIDE);}
    // Called by the folded idle timer. The layered copy is on screen before the
    // composition detaches, so the strip never blinks.
    void EnterIdleStrip(){
        if(idle_layered||expanded||opening||liquid_drag_active||liquid_failed||!composition)return;
        if(!ShowIdleStrip()){if(idle_strip)ShowWindow(idle_strip,SW_HIDE);return;}
        DwmFlush();parked_composition.reset();composition.reset();idle_layered=true;
    }
    void PaintLiquid(const clipboard::LiquidFrame& frame){
        liquid_surface->Render(frame);
        liquid_placing=true;RECT current{};GetWindowRect(window,&current);
        if(!EqualRect(&current,&frame.viewport))SetWindowPos(window,nullptr,frame.viewport.left,frame.viewport.top,frame.viewport.right-frame.viewport.left,
            frame.viewport.bottom-frame.viewport.top,SWP_NOZORDER|SWP_NOACTIVATE);
        liquid_placing=false;
        if(liquid_backdrop){const HRGN region=liquid_surface->CreateInputRegion();CheckWin32(region!=nullptr,"Liquid input mask");
            if(!SetWindowRgn(window,region,FALSE)){DeleteObject(region);CheckWin32(false,"Liquid input mask");}}
        // An idle folded strip stays on the layered copy; anything else presents through composition.
        const bool layered=idle_layered&&!expanded&&!liquid_drag_active&&ShowIdleStrip();
        if(!layered){
            const bool handoff=idle_strip&&IsWindowVisible(idle_strip);
            if(!composition)composition=std::make_unique<ClipboardComposition>();
            composition->Transition(0,1);
            composition->Present(window,static_cast<UINT>(liquid_surface->Width()),static_cast<UINT>(liquid_surface->Height()),liquid_surface->Pixels());
            if(handoff)DwmFlush();
            HideIdleStrip();
        }
        if(!expanded){compact_body=clipboard::LiquidPixels(frame.pose.bounds);compact_viewport=frame.viewport;compact_positioned=true;}
    }
    void PresentFolded(){
        const RECT body=VisualBodyBounds(),work=WorkArea(body);
        if(!liquid_surface)liquid_surface=std::make_unique<clipboard::LiquidSurface>();
        Render(static_cast<int>(CW*scale),static_cast<int>(CH*scale),true);
        liquid_surface->Cache(false,width,height,surface->Pixels());liquid_surface->ClearExpanded();
        clipboard::LiquidFrame frame;frame.pose={clipboard::ConstrainLiquidBounds(clipboard::LiquidRect(body),work),0};
        frame.viewport=clipboard::LiquidViewport(frame.pose.bounds,work,scale);frame.work=work;frame.scale=scale;frame.dark=dark;
        LiquidBackdrop(true);PaintLiquid(frame);
        // The folded strip repaints rarely; drop the software D2D factory caches once it settles.
        SetTimer(EventWindow(),4,10000,nullptr);
    }
    void StopLiquidDrag(bool finish){
        if(window)KillTimer(EventWindow(),9);liquid_drag_timer=false;
        liquid_drag_edge=clipboard::LiquidEdge::None;
        if(!liquid_drag_active)return;
        const auto pose=liquid_drag_tween.Sample(GetTickCount64());
        compact_body=finish?liquid_drag_goal:clipboard::LiquidPixels(clipboard::ConstrainLiquidBounds(pose.bounds,liquid_drag_work));
        GetWindowRect(window,&compact_viewport);compact_positioned=true;liquid_drag_active=false;
    }
    // The two HWNDs keep their material for their entire lifetime. Only the
    // folded HWND owns a liquid region; the native HWND always owns Acrylic.
    void SelectWindow(){
        const HWND next=expanded?native_window:folded_window;
        if(!next||window==next)return;
        switching_window=true;
        StopLiquidDrag(true);
        if(GetCapture()==window)ReleaseCapture();
        drag_pending=scroll_drag=false;
        RECT bounds{};GetWindowRect(window,&bounds);
        reveal_window=IsWindowVisible(window)!=FALSE;
        ShowWindow(window,SW_HIDE);HideIdleStrip();
        std::swap(composition,parked_composition);
        window=next;liquid_backdrop=window==folded_window;
        compact_positioned=false;
        SetWindowPos(window,HWND_TOPMOST,bounds.left,bounds.top,bounds.right-bounds.left,bounds.bottom-bounds.top,SWP_NOACTIVATE);
        UpdateActivationPolicy();
        switching_window=false;
    }
    bool PresentAt(RECT destination,bool paint=true){
        SelectWindow();StopLiquidDrag(true);compact_positioned=false;
        SetWindowPos(window,HWND_TOPMOST,destination.left,destination.top,destination.right-destination.left,destination.bottom-destination.top,SWP_NOACTIVATE);
        LayoutSearch();ShowWindow(search,expanded?SW_SHOW:SW_HIDE);if(paint)Present();Invalidate();
        return true;
    }
    void LiquidDragTick(ULONGLONG now){
        if(!liquid_drag_active||expanded||!enabled){StopLiquidDrag(false);return;}
        clipboard::LiquidFrame frame;frame.pose=liquid_drag_tween.Sample(now);
        frame.pose.bounds=clipboard::ConstrainLiquidBounds(frame.pose.bounds,liquid_drag_work);
        const auto edge=clipboard::FindLiquidDock(frame.pose.bounds,liquid_drag_work,scale).edge;
        if(liquid_drag_edge!=clipboard::LiquidEdge::None&&edge==clipboard::LiquidEdge::None){
            const float vx=liquid_drag_edge==clipboard::LiquidEdge::Right?180.f:liquid_drag_edge==clipboard::LiquidEdge::Left?-180.f:0.f;
            const float vy=liquid_drag_edge==clipboard::LiquidEdge::Bottom?180.f:liquid_drag_edge==clipboard::LiquidEdge::Top?-180.f:0.f;
            liquid_pull.Recoil(vx,vy,now);
        }
        liquid_drag_edge=edge;
        frame.work=liquid_drag_work;frame.scale=scale;frame.dark=dark;frame.pull=liquid_pull.Sample(now);
        frame.viewport=clipboard::LiquidViewport(frame.pose.bounds,frame.work,scale);
        LiquidBackdrop(true);PaintLiquid(frame);
        if(liquid_drag_tween.Done(now)&&liquid_pull.Settled()){
            KillTimer(EventWindow(),9);liquid_drag_timer=false;
            if(!drag_pending){StopLiquidDrag(true);Present();}
        }
    }
    void MoveLiquidDrag(RECT goal,POINT pointer,RECT work,ULONGLONG now){
        if(!liquid_drag_active){
            const RECT body=VisualBodyBounds();PresentFolded();
            liquid_drag_tween.Reset(body,0,now);liquid_pull.Reset(now);liquid_pointer=drag_start;liquid_pointer_time=now>16?now-16:0;
            liquid_drag_edge=clipboard::FindLiquidDock(clipboard::LiquidRect(body),work,scale).edge;
            liquid_drag_work=work;
            liquid_drag_active=true;
        }
        if(!EqualRect(&work,&liquid_drag_work))liquid_drag_edge=clipboard::LiquidEdge::None;
        liquid_drag_goal=goal;liquid_drag_work=work;liquid_drag_tween.Target(goal,0,now,280,true);
        const float dt=std::max(.008f,float(now-liquid_pointer_time)/1000.f);
        liquid_pull.Impulse((pointer.x-liquid_pointer.x)/(dt*scale),(pointer.y-liquid_pointer.y)/(dt*scale),now);
        liquid_pointer=pointer;liquid_pointer_time=now;
        if(!liquid_drag_timer){liquid_drag_timer=SetTimer(EventWindow(),9,15,nullptr)!=0;if(!liquid_drag_timer){StopLiquidDrag(true);Present();}}
    }
    void ReleaseLiquidDrag(){
        if(!liquid_drag_active)return;
        liquid_pull.Release(GetTickCount64());liquid_drag_timer=SetTimer(EventWindow(),9,15,nullptr)!=0;
        if(!liquid_drag_timer){StopLiquidDrag(true);Present();}
    }
    void ReflowLiquid(bool dpi_change=false){
        const bool resume=dpi_change&&drag_pending&&!expanded&&GetCapture()==window;
        POINT pointer{};GetCursorPos(&pointer);CloseQuick();StopLiquidDrag(true);
        if(!resume){CancelDismiss();drag_pending=false;if(GetCapture()==window)ReleaseCapture();}
        if(enabled)Place();
        if(resume){drag_start=pointer;drag_rect=VisualBodyBounds();dragged=true;snap_x=snap_y=0;drag_monitor=nullptr;}
    }
    static constexpr float CW=28,CH=96,HeaderBottom=clipboard::cards::HeaderBottom,BulkHeight=32,BulkGap=8;
    float ListTop=clipboard::cards::ListTop;   // + bulk bar while 2+ cards are selected
    // Cards are variable height (panel_cards.h): a page is what fits below `scroll`.
    int PageRows(){return FitCount(scroll);}
    int ResizeHit(POINT point)const{
        if(!expanded){
            RECT actual{};GetWindowRect(window,&actual);
            if(liquid_surface&&(compact_positioned&&EqualRect(&actual,&compact_viewport))){
                POINT local=point;ScreenToClient(window,&local);if(!liquid_surface->Contains(local))return HTTRANSPARENT;
            }
            return HTCLIENT;
        }
        RECT bounds{};GetWindowRect(window,&bounds);const int edge=std::max(4,static_cast<int>(5*scale));
        const bool left=point.x<bounds.left+edge,right=point.x>=bounds.right-edge,top=point.y<bounds.top+edge,bottom=point.y>=bounds.bottom-edge;
        if(top)return left?HTTOPLEFT:right?HTTOPRIGHT:HTTOP;
        if(bottom)return left?HTBOTTOMLEFT:right?HTBOTTOMRIGHT:HTBOTTOM;
        return left?HTLEFT:right?HTRIGHT:HTCLIENT;
    }
    bool HeaderDragHit(float x,float y)const{
        if(x<0||x>=W||y<0||y>=HeaderBottom)return false;
        // Keep the full click targets of header controls out of the drag area.
        return std::none_of(hits.begin(),hits.end(),[x,y](const Hit& hit){
            const auto r=hit.rect;
            return x>=r.left&&x<r.right&&y>=r.top&&y<r.bottom;
        });
    }
    void ResizeClient(int pixels_w,int pixels_h){
        if(!expanded||!resizing||pixels_w<=0||pixels_h<=0)return;
        W=pixels_w/scale;H=pixels_h/scale;ListBottom=H-clipboard::cards::FooterHeight;
        ClampScroll();
        LayoutSearch();Invalidate();
    }
    explicit Impl(HWND host,std::function<void()> callback,std::function<bool()> allowed = {}):owner(host),settings(std::move(callback)),hotkey_allowed(std::move(allowed)){}
    ~Impl(){if(!pending_delete.empty()){for(const auto id:pending_delete)history.Erase(id);pending_delete.clear();if(disk)disk->Commit(history.entries,history.groups);}ReleaseDisabledResources();}
    void Invalidate(){if(window)InvalidateRect(window,nullptr,FALSE);}
    void ClearFileIcons(){if(file_icons)file_icons->Clear();file_bitmaps.clear();}
    void ReleaseDisabledResources(){
        StopOpening(false);
        CloseQuick();quick.reset();quick_ids.clear();
        StopLiquidDrag(false);liquid_surface.reset();compact_positioned=false;liquid_backdrop=liquid_failed=false;
        dismiss_tracker.Reset();dismiss_target.reset();
        win_v_shortcut.Reset();
        if(foreground_hook){foreground_observers.erase(foreground_hook);UnhookWinEvent(foreground_hook);foreground_hook=nullptr;}
        if(ime_hook){foreground_observers.erase(ime_hook);UnhookWinEvent(ime_hook);ime_hook=nullptr;}ime_windows.clear();
        previous_process=0;
        // Disable callbacks first, join producers before destroying their target HWND.
        if(disk&&!test_mode){disk->WaitIdle();DrainStore();disk->Commit(history.entries,history.groups);disk->WaitIdle();}
        enabled=expanded=false;menu_open=menu_confirm=false;drag_pending=dragged=scroll_drag=false;
        ++copy_token;++image_token;++search_token;copy_pending=false;
        if(window){RemoveClipboardFormatListener(EventWindow());UnregisterShortcut();KillTimer(EventWindow(),1);KillTimer(EventWindow(),2);KillTimer(EventWindow(),3);KillTimer(EventWindow(),4);KillTimer(EventWindow(),6);KillTimer(EventWindow(),11);if(GetCapture()==window)ReleaseCapture();}
        ClosePreview();listening=hotkey=paste_inflight=false;disk.reset();file_icons.reset();
        composition.reset();parked_composition.reset();
        stack.reset();stack_ids.clear();stack_has_thumb.clear();stack_thumbs.clear();stack_total=0;stack_moving=false;stack_first=0;
        idle_layered=false;if(idle_strip){DestroyWindow(idle_strip);idle_strip=nullptr;}ClipboardComposition::DropPrewarm();
        switching_window=true;
        if(folded_window)DestroyWindow(folded_window);
        if(native_window)DestroyWindow(native_window);
        else if(window)DestroyWindow(window);
        window=native_window=folded_window=nullptr;search=nullptr;switching_window=reveal_window=false;
        // Release bitmap references before render target, and target before bound DC.
        images.clear();thumbnail_pending.clear();thumbnail_failed.clear();thumbnail_cache.Clear();file_bitmaps.clear();app_icon.Reset();brush.Reset();checker.Reset();target.Reset();surface.reset();search_text_glyphs=0;ime_active=false;ime_original.clear();ime_text.clear();
        fonts.clear();writer.Reset();factory.Reset();
        if(font){DeleteObject(font);font=nullptr;}if(edit_brush){DeleteObject(edit_brush);edit_brush=nullptr;}
        history=clipboard::History{};std::vector<uint64_t>().swap(visible);std::vector<uint64_t>().swap(search_matches);std::vector<Hit>().swap(hits);
        std::wstring().swap(completed_query);std::wstring().swap(query);std::wstring().swap(status);selected=last_read_id=0;copy_target=previous=paste_target=previous_focus=copy_focus=paste_focus=nullptr;copy_process=paste_process=paste_sequence=0;
        copy_remove_whitespace=copy_plain=copy_keyboard=paste_keyboard=paste_activated=paste_returning=copy_quick=paste_quick=copy_stack=paste_stack=false;paste_return_at=0;
        suppress_search_space=false;search_complete=false;tab=scroll=retry=paste_wait=0;scroll_offset=0;offset_index=-1;scroll_target=-1;hover=-1;width=height=0;
        // Keep only small user placement/policy fields; recreate resources on demand.
        // The history and decoded images just freed leave tens of MB of free pages that
        // the process heap would otherwise keep committed until later churn reuses them.
        HeapCompact(GetProcessHeap(),0);
    }
    void SelectVisible(){ClampScroll();if(std::find(visible.begin(),visible.end(),selected)==visible.end())selected=visible.empty()?0:visible.front();if(preview.IsOpen())ShowPreview();else PrefetchPreview();Invalidate();}
    void Filter(bool cancel_copy=true){
        if(tab<0||tab>=TabCount())tab=0;
        if(cancel_copy){++copy_token;copy_pending=false;}
        const auto summaries=history.Filter(tab,query,oldest,flavor);
        if(search_complete&&!query.empty()&&completed_query==query){
            visible=history.Filter(tab,L"",oldest,flavor);
            std::erase_if(visible,[&](uint64_t id){return std::find(search_matches.begin(),search_matches.end(),id)==search_matches.end()&&std::find(summaries.begin(),summaries.end(),id)==summaries.end();});
        }else{search_complete=false;visible=summaries;}
        if(!pending_delete.empty())std::erase_if(visible,[&](uint64_t id){return std::find(pending_delete.begin(),pending_delete.end(),id)!=pending_delete.end();});
        if(!msel.empty())std::erase_if(msel,[&](uint64_t id){return std::find(visible.begin(),visible.end(),id)==visible.end();});
        SelectVisible();
        if(disk&&!query.empty())disk->Search(history.entries,query,++search_token);else ++search_token;
    }
    void ClosePreview(){++preview_token;preview_pending=0;if(disk)disk->CancelPreview();preview.Close();preview_cache.Clear();}
    void CancelReads(){ClosePreview();++copy_token;++image_token;++search_token;copy_pending=false;if(disk)disk->CancelReads();images.clear();thumbnail_pending.clear();thumbnail_failed.clear();thumbnail_cache.Clear();}
    bool ImageNeeded(uint64_t id){
        if(!expanded)return false;
        const auto [first,end]=DrawnRange();for(int i=first;i<end;++i)if(visible[static_cast<size_t>(i)]==id)return true;return false;
    }
    void DrainStore(){
        if(!disk)return;bool changed=false;
        for(auto& result:disk->Take()){
            switch(result.kind){
            case clipboard::StoreResultKind::Restored:
                history.SetGroups(std::move(result.groups));for(auto it=result.entries.rbegin();it!=result.entries.rend();++it)history.Add(std::move(*it));if(!result.message.empty())status=result.message;changed=true;break;
            case clipboard::StoreResultKind::Saved:
                if(!history.Add(std::move(result.entry)))status=L"历史容量已满，未记录该条";else if(!history.entries.empty())MarkNew(history.entries.front().id);changed=true;break;
            case clipboard::StoreResultKind::Loaded:
                if(copy_pending&&result.token==copy_token){copy_pending=false;FinishCopy(result.entry,copy_paste,copy_target,copy_process);}
                else if(drag_waiting&&result.token==drag_token)drag_loaded=std::make_unique<clipboard::Entry>(std::move(result.entry));break;
            case clipboard::StoreResultKind::Thumbnail:
                if(result.token==image_token&&history.Find(result.entry.id)){
                    thumbnail_pending.erase(result.entry.id);
                    if(result.image){thumbnail_cache.Put(result.entry.id,result.image);thumbnail_failed.erase(result.entry.id);images.erase(result.entry.id);}
                    else thumbnail_failed.insert(result.entry.id);
                }break;
            case clipboard::StoreResultKind::Preview:
                if(expanded&&result.token==preview_token&&history.Find(result.entry.id)) {
                    preview_pending=0;
                    preview_cache.Put(result.entry.id,result.preview.image);
                    const bool ready=result.preview.image!=nullptr;
                    if(preview.IsOpen()&&result.entry.id==selected)preview.SetContent(selected,std::move(result.preview));
                    if(ready)PrefetchPreview();
                }
                break;
            case clipboard::StoreResultKind::Search:
                if(result.token==search_token&&!query.empty()){
                    search_matches=std::move(result.matches);search_complete=true;completed_query=query;if(!result.message.empty())status=result.message;visible=history.Filter(tab,L"",oldest,flavor);
                    std::erase_if(visible,[&](uint64_t id){return std::find(search_matches.begin(),search_matches.end(),id)==search_matches.end();});SelectVisible();
                }break;
            case clipboard::StoreResultKind::Error:
                if(result.thumbnail_read&&result.token==image_token){thumbnail_pending.erase(result.entry.id);thumbnail_failed.insert(result.entry.id);}
                if(result.preview_read&&result.token==preview_token)preview_pending=0;
                if(result.preview_read&&expanded&&preview.IsOpen()&&result.token==preview_token&&result.entry.id==selected){
                    clipboard::PreviewData data;data.error=result.message;preview.SetContent(selected,std::move(data));
                }
                if(drag_waiting&&result.token==drag_token)drag_failed=true;
                if(result.token==0||(result.foreground_read&&copy_pending&&result.token==copy_token)){status=result.message;if(result.token)copy_pending=false;}break;
            }
        }
        if(changed){disk->Commit(history.entries,history.groups);Filter(false);if(quick_active)RefreshQuick();}else if(quick_active)UpdateQuick();Invalidate();
        QueueStackRefresh();
    }
    bool Create(){
        if(window)return true;WNDCLASSEXW wc{sizeof(wc)};wc.lpfnWndProc=Proc;wc.hInstance=GetModuleHandleW(nullptr);wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.lpszClassName=L"LumaShot.Clipboard";
        if(!RegisterClassExW(&wc)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)return false;
        window=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_TOPMOST|WS_EX_NOREDIRECTIONBITMAP,wc.lpszClassName,L"LumaShot 剪贴板",WS_POPUP|WS_THICKFRAME|WS_CLIPCHILDREN,0,0,36,132,owner,nullptr,wc.hInstance,this);if(!window)return false;
        const DWORD corner=2,border_color=0xfffffffe;
        custom_outline=SUCCEEDED(DwmSetWindowAttribute(window,33,&corner,sizeof(corner)));
        DwmSetWindowAttribute(window,34,&border_color,sizeof(border_color));
        const MARGINS margins{-1};DwmExtendFrameIntoClientArea(window,&margins);
        acrylic=ApplyBackdrop();
        const BOOL yes=TRUE;DwmSetWindowAttribute(window,DWMWA_TRANSITIONS_FORCEDISABLED,&yes,sizeof(yes));
        // Clipboard UI is intentionally capturable, including the folded tab. Rely on
        // the WDA_NONE default: calling SetWindowDisplayAffinity on a never-shown
        // Acrylic HWND makes DWM composite its backdrop at the hidden position.
        search=CreateWindowExW(0,L"EDIT",L"",WS_CHILD|WS_TABSTOP|ES_AUTOHSCROLL,0,0,1,1,window,reinterpret_cast<HMENU>(201),wc.hInstance,nullptr);
        SendMessageW(search,EM_SETCUEBANNER,TRUE,reinterpret_cast<LPARAM>(L"搜索剪贴内容"));SendMessageW(search,EM_SETLIMITTEXT,512,0);
        SetWindowSubclass(search,SearchProc,1,reinterpret_cast<DWORD_PTR>(this));
        native_window=window;
        const HWND folded=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_TOPMOST|WS_EX_NOACTIVATE|WS_EX_NOREDIRECTIONBITMAP,wc.lpszClassName,L"LumaShot 剪贴板侧边条",WS_POPUP,0,0,28,96,owner,nullptr,wc.hInstance,this);
        if(!folded)return false;
        window=folded;LiquidBackdrop(true);folded_window=folded;
        DwmSetWindowAttribute(folded,DWMWA_TRANSITIONS_FORCEDISABLED,&yes,sizeof(yes));
        window=native_window;liquid_backdrop=false;
        UpdateActivationPolicy();TrackForeground();return search!=nullptr;
    }
    void LoadLayout(){
        if(layout_path.empty())return;
        if(const auto dimensions=clipboard::LoadPanelSize(layout_path)){W=static_cast<float>(dimensions->cx);H=static_cast<float>(dimensions->cy);ListBottom=H-clipboard::cards::FooterHeight;}
        if(!placed){if(const auto saved=clipboard::LoadPosition(layout_path)){anchor=*saved;placed=true;}}
        paused=clipboard::LoadPaused(layout_path);
    }
    bool SaveLayout()const{
        // Tests only persist when given an explicit isolated path; never resolve personal settings.
        if(test_mode&&layout_path.empty())return true;
        return clipboard::SavePosition(layout_path,anchor,SIZE{static_cast<LONG>(std::lround(W)),static_cast<LONG>(std::lround(H))});
    }
    bool Enable(bool value,bool night){
        if(quick_active)CloseQuick();
        dark=night;if(!value){ReleaseDisabledResources();return true;}if(edit_brush)DeleteObject(edit_brush);edit_brush=CreateSolidBrush(dark?RGB(31,40,55):RGB(255,255,255));
        if(!file_icons)file_icons=std::make_unique<clipboard::FileIconCache>();
        if(!test_mode&&layout_path.empty()){
            PWSTR folder{};if(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&folder))){layout_path=std::filesystem::path(folder)/L"LumaShot"/L"clipboard-layout.ini";CoTaskMemFree(folder);}
        }
        if(!window)LoadLayout();
        if(!Create()){ReleaseDisabledResources();return false;}acrylic=ApplyBackdrop();CreateStack();
        if(!test_mode&&!disk){if(layout_path.empty()){ReleaseDisabledResources();return false;}disk=std::make_unique<clipboard::SessionStore>(layout_path.parent_path()/L"clipboard-history",EventWindow(),persist_history);}
        if(!enabled){sequence=GetClipboardSequenceNumber();listening=test_mode||AddClipboardFormatListener(EventWindow())!=FALSE;if(!listening){ReleaseDisabledResources();return false;}enabled=true;expanded=false;
            hotkey=shortcut_key&&!test_mode&&!hotkeys_suspended&&RegisterShortcut(shortcut_modifiers,shortcut_key);
            status=(!shortcut_key||hotkeys_suspended)?L"快捷键已禁用，可点击侧边栏打开":hotkey?L"":L"快捷键被占用，可点击侧边栏打开";
        }
        Place();Invalidate();return true;
    }
    void ResolveLayoutPath(){
        if(test_mode||!layout_path.empty())return;
        PWSTR folder{};if(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&folder))){layout_path=std::filesystem::path(folder)/L"LumaShot"/L"clipboard-layout.ini";CoTaskMemFree(folder);}
    }
    bool SetPersistent(bool value){
        const bool changed=persist_history!=value;persist_history=value;
        if(test_mode)return true;
        // A live store only exists while enabled; recreate it under the new policy.
        if(disk){if(!changed)return true;const bool night=dark;ReleaseDisabledResources();return Enable(true,night);}
        if(!value){ResolveLayoutPath();if(!layout_path.empty())return clipboard::SessionStore::PurgePersistent(layout_path.parent_path()/L"clipboard-history");}
        return true;
    }
    void UnregisterShortcut(){win_v_shortcut.Reset();if(window)UnregisterHotKey(EventWindow(),1);}
    bool RegisterShortcut(UINT modifiers,UINT key){
        if(modifiers==MOD_WIN&&key=='V')return win_v_shortcut.Install(EventWindow());// Hook runs on its own thread; WM_HOTKEY re-checks the live guard on the UI thread.
        return RegisterHotKey(EventWindow(),1,modifiers|MOD_NOREPEAT,key)!=FALSE;
    }
    bool SetShortcut(UINT modifiers,UINT key){
        if(modifiers==shortcut_modifiers&&key==shortcut_key)return true;
        const bool active=window&&enabled&&!hotkeys_suspended&&!test_mode;
        const bool was_registered=hotkey;
        if(active&&hotkey)UnregisterShortcut();
        if(active&&key&&!RegisterShortcut(modifiers,key)){
            hotkey=was_registered&&RegisterShortcut(shortcut_modifiers,shortcut_key);
            return false;
        }
        shortcut_modifiers=modifiers;shortcut_key=key;hotkey=active&&key;
        status=(!key||hotkeys_suspended)?L"快捷键已禁用，可点击侧边栏打开":L"";
        if(window)Invalidate();return true;
    }
    void SetHotkeysSuspended(bool suspended){
        if(hotkeys_suspended==suspended)return;
        hotkeys_suspended=suspended;
        if(suspended)CloseQuick();
        if(!window||!enabled)return;
        if(hotkey)UnregisterShortcut();
        hotkey=shortcut_key&&!suspended&&!test_mode&&RegisterShortcut(shortcut_modifiers,shortcut_key);
        status=(!shortcut_key||suspended)?L"快捷键已禁用，可点击侧边栏打开":hotkey?L"":L"快捷键被占用，可点击侧边栏打开";
        Invalidate();
    }
    void Place(bool animate=false){
        // From the idle strip the D3D11 device is gone; create it on a worker while this thread renders.
        if(expanded)ClipboardComposition::Prewarm();
        if(!expanded&&strip_hidden){ParkHiddenStrip();return;}
        // Fold -> expand continuity: the stack cards fly into the panel's card slots.
        const bool fly_out=stack&&expanded&&stack->Visible()&&!stack->Flying()&&MotionEnabled()&&!test_mode&&enabled;
        if(stack){if(!expanded){PlaceStack();return;}if(!fly_out)stack->Show(false);}
        StopOpening(false);
        const RECT from=VisualBodyBounds();
        animate=animate&&expanded&&window==folded_window&&IsWindowVisible(window)&&MotionEnabled()&&!liquid_failed;
        StopLiquidDrag(true);SelectWindow();
        if(animate)reveal_window=false;
        MONITORINFO monitor{sizeof(monitor)};
        if(!placed){POINT cursor{};GetCursorPos(&cursor);GetMonitorInfoW(MonitorFromPoint(cursor,MONITOR_DEFAULTTONEAREST),&monitor);anchor={monitor.rcWork.right-6,monitor.rcWork.top+70};placed=true;}
        const POINT location=expanded&&!pinned&&summon_cursor?*summon_cursor:POINT{anchor.x-1,anchor.y};
        GetMonitorInfoW(MonitorFromPoint(location,MONITOR_DEFAULTTONEAREST),&monitor);
        const auto r=monitor.rcWork;scale=GetDpiForWindow(window)/96.f;
        if(expanded)scale=std::min({scale,(r.right-r.left-16)/W,(r.bottom-r.top-16)/H});scale=std::max(.25f,scale);
        const int w=static_cast<int>((expanded?W:CW)*scale),h=static_cast<int>((expanded?H:CH)*scale);
        const LONG margin=expanded?4:0;
        const LONG x=std::clamp(anchor.x-w,r.left+margin,std::max(r.left+margin,r.right-w-margin)),y=std::clamp(anchor.y,r.top+margin,std::max(r.top+margin,r.bottom-h-margin));
        const RECT placement=expanded&&!pinned&&summon_cursor?clipboard::PanelNearCursor(*summon_cursor,r,{w,h},scale):RECT{x,y,x+w,y+h};
        PresentAt(placement,enabled&&(!test_mode||reveal_window));
        if(animate&&StartOpening(from,placement,GetTickCount64())){if(fly_out)stack->Show(false);return;}
        if(enabled&&!test_mode)ShowWindow(window,SW_SHOWNOACTIVATE);
        if(fly_out){stack->Fly(StackSlots(placement),true);stack->Show(false);}
    }
    // Screen rects (physical px) of the panel cards showing the stack's items, in stack order;
    // empty where the card is not on screen.
    std::vector<RECT> StackSlots(RECT panel){
        std::vector<RECT> out;if(!stack)return out;
        BuildBlocks();const float px=ScrollPx();
        for(size_t k=0;k<stack_ids.size()&&k<static_cast<size_t>(clipboard::stack::MaxCards);++k){
            RECT slot{};const auto it=std::find(visible.begin(),visible.end(),stack_ids[k]);
            if(it!=visible.end()){
                const size_t i=static_cast<size_t>(it-visible.begin());
                if(i<block_tops.size())if(const auto* e=history.Find(*it)){
                    const float top=ListTop+block_tops[i]-px+(block_header[i]?clipboard::cards::GroupHeaderHeight:0.f),bottom=top+CardHeightFor(*e);
                    if(bottom>ListTop&&top<ListBottom)slot={panel.left+std::lround(clipboard::cards::ListInset*scale),panel.top+std::lround(std::max(top,ListTop)*scale),
                        panel.left+std::lround((W-clipboard::cards::ListInset)*scale),panel.top+std::lround(std::min(bottom,ListBottom)*scale)};
                }
            }
            out.push_back(slot);
        }
        return out;
    }
    // ---- Card stack ---------------------------------------------------------
    // 最多保留 (settings): lowering it drops the oldest ordinary entries right away.
    void SetHistoryLimit(size_t value){
        history.SetLimit(value);const size_t before=history.entries.size();history.TrimOrdinary();
        if(history.entries.size()==before)return;
        if(disk)disk->Commit(history.entries,history.groups);
        Filter();QueueStackRefresh();Invalidate();
    }
    // 侧边条样式 (settings). Mirrored into clipboard-layout.ini so both stay in step;
    // while enabled the folded form is rebuilt in place.
    void SetStripStyle(bool classic){
        classic_override=classic;ResolveLayoutPath();
        if(test_mode)stack_style=!classic;
        else if(!layout_path.empty()&&(clipboard::LoadStripStyle(layout_path)==clipboard::StripStyle::Classic)!=classic)
            clipboard::SaveStripStyle(layout_path,classic?clipboard::StripStyle::Classic:clipboard::StripStyle::Stack);
        if(!enabled)return;
        if(classic&&stack){stack.reset();stack_ids.clear();stack_has_thumb.clear();stack_thumbs.clear();stack_total=0;stack_moving=false;stack_first=0;if(!expanded)Place();}
        else if(!classic&&!stack){CreateStack();if(stack&&!expanded)Place();}
    }
    void SetCardPreview(bool show){
        if(card_preview==show)return;
        card_preview=show;if(!show)stack_thumbs.clear();
        stack_ids.clear();RefreshStack();
    }
    void CreateStack(){
        if(!test_mode)stack_style=classic_override?!*classic_override:clipboard::LoadStripStyle(layout_path)==clipboard::StripStyle::Stack;
        if(stack||!stack_style)return;
        clipboard::StackEvents events;
        events.activate=[this](size_t index){StackPaste(index);};
        events.open_panel=[this]{Show(false);};
        events.moved=[this](RECT){StackMoved();};
        events.dragging=[this](POINT at){StackDragging(at);};
        events.move_cancelled=[this]{stack_moving=false;CancelDismiss();};
        events.drag=[this](size_t index,POINT at){return StackDrag(index,at);};
        events.scroll=[this](int steps){StackScroll(steps);};
        events.folded=[this]{StackFolded();};
        stack=std::make_unique<clipboard::StackWindow>(owner,std::move(events));
        // Without Windows.UI.Composition the classic strip stays.
        if(!stack->Create()){stack.reset();return;}
        stack->SetPaused(paused);
        stack->SetBadgePrefix(L"");stack_ids.clear();stack_has_thumb.clear();stack_total=0;stack_first=0;
    }
    void PlaceStack(){
        // The liquid strip and its renderer are never shown in stack mode.
        StopOpening(false);StopLiquidDrag(false);
        if(window){SelectWindow();reveal_window=false;if(GetCapture()==window)ReleaseCapture();ShowWindow(window,SW_HIDE);}
        HideIdleStrip();if(search)ShowWindow(search,SW_HIDE);
        MONITORINFO monitor{sizeof(monitor)};
        if(!placed){POINT cursor{};GetCursorPos(&cursor);GetMonitorInfoW(MonitorFromPoint(cursor,MONITOR_DEFAULTTONEAREST),&monitor);anchor={monitor.rcWork.right-6,monitor.rcWork.top+70};placed=true;}
        GetMonitorInfoW(MonitorFromPoint(POINT{anchor.x-1,anchor.y},MONITOR_DEFAULTTONEAREST),&monitor);
        const auto side=clipboard::stack::NearestSide(anchor.x-1,monitor.rcWork);
        const auto dpi=[&]{const UINT value=GetDpiForWindow(stack->VisualWindow());return value?value/96.f:1.f;};
        const float first=dpi();stack->Place(side,anchor.y,monitor.rcWork,first);
        // Moving onto another monitor updates the window DPI; place once more at it.
        if(const float actual=dpi();std::abs(actual-first)>.001f)stack->Place(side,anchor.y,monitor.rcWork,actual);
        stack->SetMotion(MotionEnabled());
        RefreshStack();
        stack->Show(enabled);
    }
    static std::shared_ptr<const Frame> StackThumbnail(const Frame& source){
        // Area-average down to cover the 46x40 DIP tile at up to ~2x scale.
        const int w=source.Width(),h=source.Height();
        if(w<=0||h<=0||source.pixels.size()<static_cast<size_t>(w)*static_cast<size_t>(h))return nullptr;
        const float s=std::min(1.f,std::max(96.f/w,84.f/h));
        const int dw=std::max(1,static_cast<int>(std::lround(w*s))),dh=std::max(1,static_cast<int>(std::lround(h*s)));
        auto out=std::make_shared<Frame>();out->bounds={0,0,dw,dh};out->pixels.resize(static_cast<size_t>(dw)*static_cast<size_t>(dh));
        for(int y=0;y<dh;++y){
            const int y0=y*h/dh,y1=std::max(y0+1,(y+1)*h/dh);
            for(int x=0;x<dw;++x){
                const int x0=x*w/dw,x1=std::max(x0+1,(x+1)*w/dw);uint64_t sum[4]{};
                for(int sy=y0;sy<y1;++sy)for(int sx=x0;sx<x1;++sx){const uint32_t c=source.pixels[static_cast<size_t>(sy)*w+sx];for(int k=0;k<4;++k)sum[k]+=(c>>(8*k))&255u;}
                const uint64_t n=static_cast<uint64_t>(x1-x0)*static_cast<uint64_t>(y1-y0);uint32_t c=0;
                for(int k=0;k<4;++k)c|=static_cast<uint32_t>((sum[k]+n/2)/n)<<(8*k);
                out->pixels[static_cast<size_t>(y)*dw+x]=c;
            }
        }
        return out;
    }
    // A burst of copies must cost one redraw: composition only commits (and WARP
    // only frees replaced surfaces) between message-loop turns.
    static constexpr UINT StackRefreshMessage=WM_APP+185;
    bool stack_refresh_queued{};
    size_t stack_first{};  // wheel paging: history index of the first card (0 = newest)
    void QueueStackRefresh(){
        if(!stack||expanded||stack_refresh_queued)return;
        stack_refresh_queued=PostMessageW(EventWindow(),StackRefreshMessage,0,0)!=FALSE;
        if(!stack_refresh_queued)RefreshStack();
    }
    // Cheap when nothing changed: ids, total, theme and thumbnail presence are compared first.
    void RefreshStack(){
        if(!stack||expanded)return;
        std::vector<const clipboard::Entry*> top;
        const size_t entry_count=history.entries.size(),stack_cap=static_cast<size_t>(clipboard::stack::MaxCards);
        stack_first=std::min(stack_first,entry_count>stack_cap?entry_count-stack_cap:0);
        size_t stack_skip=stack_first;
        for(const auto& e:history.entries){if(top.size()==stack_cap)break;if(stack_skip){--stack_skip;continue;}top.push_back(&e);}
        std::vector<uint64_t> ids;std::vector<bool> has;
        for(const auto* e:top){
            ids.push_back(e->id);
            if(card_preview&&e->kind==clipboard::Kind::Image&&!stack_thumbs.contains(e->id)){
                if(auto frame=thumbnail_cache.Find(e->id)){if(auto reduced=StackThumbnail(*frame))stack_thumbs[e->id]=std::move(reduced);}
                else if(e->payload&&disk&&!thumbnail_pending.contains(e->id)&&!thumbnail_failed.contains(e->id)&&disk->Load(*e,image_token,true))thumbnail_pending.insert(e->id);
            }
            has.push_back(stack_thumbs.contains(e->id));
        }
        std::erase_if(stack_thumbs,[&](const auto& item){return std::find(ids.begin(),ids.end(),item.first)==ids.end();});
        // Folded, only the stack asked for these; it keeps its own reduced copies.
        if(thumbnail_cache.Bytes())thumbnail_cache.Clear();
        const size_t total=history.entries.size();
        if(ids==stack_ids&&has==stack_has_thumb&&total==stack_total&&dark==stack_dark)return;
        // Ids only grow: a larger unseen first id is a new copy, also once history is full.
        const bool incoming=stack_first==0&&stack->Visible()&&!ids.empty()&&!stack_ids.empty()&&ids.front()>stack_ids.front()&&
            std::find(stack_ids.begin(),stack_ids.end(),ids.front())==stack_ids.end();
        std::vector<clipboard::StackItem> items;items.reserve(top.size());
        for(const auto* e:top){
            const auto thumb=stack_thumbs.find(e->id);
            auto item=clipboard::MakeStackItem(*e,thumb==stack_thumbs.end()?nullptr:thumb->second.get(),e->file_names[0],e->file_count);
            items.push_back(card_preview?std::move(item):clipboard::Concealed(std::move(item)));
        }
        stack->SetTheme(dark);stack->SetItems(std::move(items),total,incoming,stack_first);
        stack_ids=std::move(ids);stack_has_thumb=std::move(has);stack_total=total;stack_dark=dark;
    }
    // Wheel over the open stack: one history entry per notch (+ older), clamped to the oldest five.
    void StackScroll(int steps){
        if(!stack||expanded||steps==0)return;
        const size_t entry_count=history.entries.size(),stack_cap=static_cast<size_t>(clipboard::stack::MaxCards);
        const size_t oldest_first=entry_count>stack_cap?entry_count-stack_cap:0,notches=static_cast<size_t>(steps<0?-steps:steps);
        const size_t goal=steps<0?(stack_first>notches?stack_first-notches:0):std::min(oldest_first,stack_first+notches);
        if(goal==stack_first)return;
        stack_first=goal;RefreshStack();
    }
    // Folded again: the minis always show the newest entries.
    void StackFolded(){if(stack_first){stack_first=0;RefreshStack();}}
    // Card click: copy and paste into the window that still has focus (the stack never activates).
    void StackPaste(size_t index){
        if(index>=stack_ids.size()||copy_pending||paste_inflight)return;
        const auto* e=history.Find(stack_ids[index]);if(!e)return;
        const HWND foreground=GetForegroundWindow();RememberTarget(foreground);
        const bool paste=previous&&foreground&&GetAncestor(foreground,GA_ROOT)==previous;
        copy_target=paste?previous:nullptr;copy_process=paste?previous_process:0;copy_focus=paste?previous_focus:nullptr;
        copy_remove_whitespace=copy_plain=copy_keyboard=copy_quick=false;copy_stack=true;
        if(e->payload&&disk){copy_paste=paste;copy_pending=true;const auto token=++copy_token;
            if(!disk->Load(*e,token)){copy_pending=false;copy_stack=false;}return;}
        FinishCopy(*e,paste,copy_target,copy_process);
    }
    void StackDragging(POINT at){
        if(!stack_moving){stack_moving=true;drag_anchor=anchor;dismiss_tracker.Begin(at);}
        MONITORINFO monitor{sizeof(monitor)};GetMonitorInfoW(MonitorFromPoint(at,MONITOR_DEFAULTTONEAREST),&monitor);
        UpdateDismiss(at,monitor.rcWork,stack->VisualWindow());
    }
    // ---- Card drag-out (docs/clipboard-card-stack.md 3.5) --------------------
    // Formats are read before DoDragDrop: OLE calls GetData on this thread, so it
    // can never wait for a store result that DrainStore would deliver.
    uint64_t drag_token{1ull<<62},drag_id{};bool drag_waiting{},drag_failed{},drag_running{};
    struct RowPress{uint64_t id{};POINT at{};};std::optional<RowPress> row_press;  // panel card pressed, drag not yet started
    std::unique_ptr<clipboard::Entry> drag_loaded;
    std::function<bool(std::shared_ptr<const Frame>,POINT)> pin_request;
    // Optional host hooks for image cards (OCR, annotate-then-pin); buttons hide without them.
    std::function<bool(ClipboardImageAction,std::shared_ptr<const Frame>,POINT)> image_action;
    std::wstring last_copied_text,last_shell_target;   // test-mode observations; never written to the system
    // .item / .acts transitions (.12s): values move only while a transition runs (timer 13).
    struct CardFx{float hover{},active{},acts{},hover_to{},active_to{},acts_to{};};std::map<uint64_t,CardFx> card_fx;
    bool fx_running{},fx_enabled{true};ULONGLONG fx_tick{};
    uint64_t new_id{};ULONGLONG new_tick{};   // .item.new: slidein .45s on the card that just arrived
    // Multi-select (full.html msel): Ctrl toggles, Shift adds a range, the bulk bar shows at 2+.
    std::set<uint64_t> msel;std::optional<size_t> last_click;WPARAM click_mods{};
    // Bulk delete is undoable: rows hide at once and are erased after 6 s, on fold, or before the next bulk delete.
    std::vector<uint64_t> pending_delete;
    // OCR text of image cards recognised this session (bounded), shown in Quick Look's OCR area.
    std::map<uint64_t,std::wstring> ocr_texts;uint64_t ocr_pending_id{};bool swallow_char{};
    std::filesystem::path drag_save_folder;  // tests: never the real desktop
    std::function<clipboard::CardDragResult(clipboard::CardDragPayload,clipboard::CardDragOptions)> drag_runner;  // tests: no DoDragDrop
    std::unique_ptr<clipboard::Entry> LoadForDrag(const clipboard::Entry& e){
        if(!e.payload||!disk)return std::make_unique<clipboard::Entry>(e);
        drag_loaded.reset();drag_failed=false;drag_waiting=true;const auto token=++drag_token;
        if(!disk->Load(e,token)){drag_waiting=false;return nullptr;}
        // Store reads of one entry take a few ms; bounded so a stuck disk cannot freeze the drag.
        for(int i=0;i<8&&!drag_loaded&&!drag_failed;++i){disk->WaitIdle(100);DrainStore();}
        drag_waiting=false;
        return std::move(drag_loaded);
    }
    bool OverStack(POINT at)const{
        if(!stack||!stack->Visible())return false;
        const RECT r=stack->WindowRect();return PtInRect(&r,at)!=FALSE;
    }
    bool StackDrag(size_t index,POINT){
        if(index>=stack_ids.size())return false;
        return DragEntry(stack_ids[index],false);
    }
    // Over the place the card came from: the stack, or the open panel.
    bool OverSource(POINT at,bool from_panel)const{
        if(from_panel){RECT r{};return expanded&&window&&GetWindowRect(window,&r)&&PtInRect(&r,at);}
        return OverStack(at);
    }
    static std::wstring WindowTitle(HWND root){
        // InternalGetWindowText never sends WM_GETTEXT, so a hung target cannot stall the drag.
        wchar_t title[128]{};if(root)InternalGetWindowText(root,title,128);return title;
    }
    bool DragEntry(uint64_t id,bool from_panel){
        if(drag_running||copy_pending||paste_inflight)return false;
        const auto* e=history.Find(id);if(!e)return false;
        auto loaded=LoadForDrag(*e);
        if(!loaded){status=L"读取记录失败，请重试";Invalidate();return false;}
        e=history.Find(id);if(!e)return false;  // draining the store may have reallocated the history
        if(from_panel&&msel.size()>1&&msel.count(id))if(auto merged=MergedSelection(id)){loaded=std::make_unique<clipboard::Entry>(std::move(*merged));e=history.Find(id);if(!e)return false;}
        if(loaded->kind==clipboard::Kind::Files){
            std::error_code ec;bool any=false;
            for(const auto& path:clipboard::FilePaths(*loaded))if(std::filesystem::exists(path,ec)){any=true;break;}
            if(!any){status=L"文件已不存在";Invalidate();return false;}
        }
        clipboard::CardDragPayload payload;payload.formats=loaded->formats;
        if(loaded->kind==clipboard::Kind::Image){
            auto copy=std::make_shared<const clipboard::Entry>(*loaded);
            payload.file=[copy]{auto path=clipboard::WriteImageFile(*copy);return path?*path:std::filesystem::path{};};
        }
        clipboard::HintContext hint;hint.kind=loaded->kind;hint.pin_available=static_cast<bool>(pin_request);
        if(loaded->kind==clipboard::Kind::Text){
            // Same rule as the stack's link tile: one http(s) URL and nothing else.
            const auto text=clipboard::EntryText(*loaded);
            if(clipboard::LooksLikeLink(text)){payload.url=clipboard::Trimmed(text);hint.link=true;}
        }
        clipboard::ReleaseContext context;context.kind=loaded->kind;context.pin_available=hint.pin_available;
        POINT released{},start{};GetCursorPos(&start);
        // Ghost: drawn by the stack renderer (also while the stack is hidden behind the open panel).
        bool ghost=false;
        if(stack){
            const auto thumb=stack_thumbs.find(id);
            std::shared_ptr<const Frame> reduced;
            if(thumb!=stack_thumbs.end())reduced=thumb->second;
            else if(auto frame=thumbnail_cache.Find(id))reduced=StackThumbnail(*frame);
            ghost=stack->BeginGhost(clipboard::MakeStackItem(*e,reduced.get(),e->file_names[0],e->file_count),start);
        }
        clipboard::CardDragOptions options;
        HWND hint_root{};
        options.moved=[&](POINT at,bool over,bool alt){
            if(!ghost)return;
            clipboard::DragZone zone=clipboard::DragZone::None;
            if(OverSource(at,from_panel))zone=clipboard::DragZone::Stack;
            else if(clipboard::IsDesktopAt(at))zone=clipboard::DragZone::Desktop;
            else if(over){
                zone=clipboard::DragZone::App;
                const HWND hit=WindowFromPoint(at),root=hit?GetAncestor(hit,GA_ROOT):nullptr;
                if(root!=hint_root){hint_root=root;hint.app=WindowTitle(root);}
            }
            hint.alt=alt;
            stack->MoveGhost(at,clipboard::ZonePose(zone),clipboard::ZoneHint(zone,hint));
        };
        options.on_release=[&](POINT at,DWORD keys){
            released=at;context.over_stack=OverSource(at,from_panel);context.desktop=!context.over_stack&&clipboard::IsDesktopAt(at);context.alt=(keys&MK_ALT)!=0;
            return clipboard::DecideRelease(context);
        };
        drag_running=true;drag_id=from_panel?id:0;if(from_panel)Invalidate();
        const auto result=drag_runner?drag_runner(std::move(payload),std::move(options)):clipboard::DragCard(std::move(payload),std::move(options));
        drag_running=false;drag_id=0;
        if(ghost)stack->EndGhost();
        ApplyRelease(clipboard::ResolveRelease(context,result),*loaded,id,released);
        return true;
    }
    // Whole-card drag from the open panel: starts past the system drag threshold.
    // No mouse capture here: releasing it would send WM_CAPTURECHANGED, which
    // cancels the pinned panel's pending click-to-paste.
    bool RowDragMove(){
        if(!(GetKeyState(VK_LBUTTON)&0x8000)){row_press.reset();return false;}
        POINT at{};GetCursorPos(&at);
        if(std::abs(at.x-row_press->at.x)<GetSystemMetrics(SM_CXDRAG)&&std::abs(at.y-row_press->at.y)<GetSystemMetrics(SM_CYDRAG))return true;
        const uint64_t id=row_press->id;row_press.reset();paste_click.reset();
        DragEntry(id,true);Invalidate();return true;
    }
    std::filesystem::path DragSaveFolder()const{
        if(!drag_save_folder.empty())return drag_save_folder;
        PWSTR path{};std::filesystem::path out;
        if(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop,0,nullptr,&path)))out=path;
        CoTaskMemFree(path);return out;
    }
    void ApplyRelease(clipboard::ReleaseAction action,const clipboard::Entry& loaded,uint64_t id,POINT at){
        switch(action){
        case clipboard::ReleaseAction::Pin:{
            const auto frame=clipboard::DecodeFrame(loaded);
            if(!frame){status=L"图片无法读取，未能钉到桌面";break;}
            if(!pin_request||!pin_request(frame,at))status=L"钉图失败";
            else status=L"已钉到桌面";
            break;}
        case clipboard::ReleaseAction::SaveText:{
            const auto text=clipboard::EntryText(loaded);const bool link=loaded.kind==clipboard::Kind::Text&&clipboard::LooksLikeLink(text);
            const auto saved=clipboard::SaveTextFile(loaded,DragSaveFolder(),link?clipboard::LinkTitle(text):text,link);
            status=saved?L"已保存到桌面："+saved->filename().wstring():L"无法保存到桌面";
            break;}
        case clipboard::ReleaseAction::MoveToTop:MoveToTop(id);break;
        case clipboard::ReleaseAction::CopyFallback:
            if(test_mode){last_read_id=loaded.id;status=L"目标不接受拖放，已复制，可按 Ctrl+V";break;}
            if(!clipboard::Write(window,loaded,false,false)){status=L"剪贴板正忙或内容不可用，请重试";break;}
            sequence=GetClipboardSequenceNumber();status=L"目标不接受拖放，已复制，可按 Ctrl+V";
            break;
        case clipboard::ReleaseAction::Insert:case clipboard::ReleaseAction::None:default:break;
        }
        Invalidate();
    }
    bool MoveToTop(uint64_t id){
        auto it=std::find_if(history.entries.begin(),history.entries.end(),[&](const clipboard::Entry& e){return e.id==id;});
        if(it==history.entries.end())return false;
        if(it!=history.entries.begin())std::rotate(history.entries.begin(),it,std::next(it));
        CommitHistory();Filter(false);status=L"已移到最前";RefreshStack();return true;
    }
    void StackMoved(){
        const bool was_moving=stack_moving;stack_moving=false;
        if(was_moving&&dismiss_tracker.Armed()){DismissStrip();return;}
        dismiss_tracker.Reset();if(dismiss_target)dismiss_target->Hide();
        const RECT r=stack->WindowRect();MONITORINFO monitor{sizeof(monitor)};GetMonitorInfoW(MonitorFromRect(&r,MONITOR_DEFAULTTONEAREST),&monitor);
        const UINT dpi=GetDpiForWindow(stack->VisualWindow());const float s=dpi?dpi/96.f:1.f;
        // Same anchor as the classic strip (right edge x, top y), so both styles share it.
        anchor={stack->Side()==clipboard::stack::Side::Right?monitor.rcWork.right-6:monitor.rcWork.left+static_cast<LONG>(CW*s),clipboard::stack::GripTop(r,s)};
        placed=true;if(!SaveLayout())status=L"位置保存失败，下次启动可能无法恢复";
    }
    void LayoutSearch(){
        SelectWindow();
        const HFONT next=CreateFontW(-static_cast<int>(13*scale),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
        if(!next)return;SendMessageW(search,WM_SETFONT,reinterpret_cast<WPARAM>(next),FALSE);if(font)DeleteObject(font);font=next;
        HDC dc=GetDC(search);if(!dc)return;const auto old=SelectObject(dc,font);TEXTMETRICW metrics{};GetTextMetricsW(dc,&metrics);SelectObject(dc,old);ReleaseDC(search,dc);
        const int x=static_cast<int>(44*scale),w=static_cast<int>((W-124)*scale),h=metrics.tmHeight+4;
        SendMessageW(search,EM_SETMARGINS,EC_LEFTMARGIN|EC_RIGHTMARGIN,0);
        SetWindowPos(search,nullptr,x,0,w,h,SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOREDRAW);
        RECT format{};SendMessageW(search,EM_GETRECT,0,reinterpret_cast<LPARAM>(&format));
        // A borderless single-line EDIT paints at its formatting-rect top, not
        // vertically centered in an arbitrarily tall child window.
        const int y=static_cast<int>(std::lround((clipboard::cards::SearchTop+clipboard::cards::SearchBottom)/2.f*scale-format.top-metrics.tmHeight/2.f));
        SetWindowPos(search,nullptr,x,y,w,h,SWP_NOZORDER|SWP_NOACTIVATE);InvalidateRect(search,nullptr,TRUE);
    }
    float ScrollThumb(){const float track=ViewHeight(),content=ContentHeight();return content<=track?track:std::max(24.f,track*track/content);}
    void ScrollAt(float y){
        StopScrollAnim();const float travel=ViewHeight()-ScrollThumb();if(travel<=0)return;
        SetScrollPx((y-ListTop-scroll_grab)*MaxScrollPx()/travel);Invalidate();
    }
    void BeginScroll(float y){
        const float thumb=ScrollThumb(),limit_px=MaxScrollPx(),top=ListTop+(ViewHeight()-thumb)*(limit_px>0?ScrollPx()/limit_px:0.f);
        scroll_grab=y>=top&&y<=top+thumb?y-top:thumb/2;scroll_drag=true;SetCapture(window);ScrollAt(y);
    }
    void BeginDrag(POINT point){if(expanded&&summon_cursor){const RECT body=VisualBodyBounds();anchor={body.right,body.top};summon_cursor.reset();}StopLiquidDrag(false);drag_anchor=anchor;if(expanded)dismiss_tracker.Reset();else dismiss_tracker.Begin(point);snap_x=snap_y=0;drag_monitor=nullptr;drag_start=point;drag_rect=VisualBodyBounds();drag_pending=true;dragged=false;SetCapture(window);}
    void Drag(POINT point){
        if(!drag_pending)return;const LONG dx=point.x-drag_start.x,dy=point.y-drag_start.y;
        if(!dragged&&std::abs(dx)<GetSystemMetrics(SM_CXDRAG)&&std::abs(dy)<GetSystemMetrics(SM_CYDRAG))return;
        dragged=true;MONITORINFO monitor{sizeof(monitor)};const auto current_monitor=MonitorFromPoint(point,MONITOR_DEFAULTTONEAREST);GetMonitorInfoW(current_monitor,&monitor);if(current_monitor!=drag_monitor){snap_x=snap_y=0;drag_monitor=current_monitor;}
        const LONG w=drag_rect.right-drag_rect.left,h=drag_rect.bottom-drag_rect.top;const auto r=monitor.rcWork;
        LONG x=std::clamp(drag_rect.left+dx,r.left,std::max(r.left,r.right-w)),y=std::clamp(drag_rect.top+dy,r.top,std::max(r.top,r.bottom-h));
        if(!expanded){const float dpi=GetDpiForWindow(window)/96.f;
            x=clipboard::SnapAxis(x,r.left,std::max(r.left,r.right-w),dpi,snap_x);
            y=clipboard::SnapAxis(y,r.top,std::max(r.top,r.bottom-h),dpi,snap_y);
        }
        anchor={x+w,y};placed=true;
        if(!expanded&&UpdateDismiss(point,r)){
            // Armed: the strip is pulled into the close target.
            anchor=drag_anchor;const RECT goal=dismiss_tracker.Magnet(w,h);
            if(!test_mode&&!liquid_failed&&MotionEnabled()){MoveLiquidDrag(goal,point,r,GetTickCount64());return;}
            compact_positioned=false;SetWindowPos(window,nullptr,goal.left,goal.top,w,h,SWP_NOZORDER|SWP_NOACTIVATE);Invalidate();return;
        }
        if(!expanded&&!test_mode&&!liquid_failed&&MotionEnabled()){MoveLiquidDrag({x,y,x+w,y+h},point,r,GetTickCount64());return;}
        compact_positioned=false;SetWindowPos(window,nullptr,x,y,w,h,SWP_NOZORDER|SWP_NOACTIVATE);Invalidate();
    }
    void EndDrag(){const bool click=drag_pending&&!dragged&&!expanded;const bool save=drag_pending&&dragged;const bool dismiss=save&&!expanded&&dismiss_tracker.Armed();drag_pending=false;if(dismiss)dismiss_tracker.Reset();ReleaseCapture();if(dismiss){DismissStrip();return;}ReleaseLiquidDrag();CancelDismiss();if(save&&!SaveLayout()){status=L"位置保存失败，下次启动可能无法恢复";Invalidate();}if(click)Show(false);}
    void RememberTarget(HWND candidate){
        if(!IsWindow(candidate)||candidate==window||candidate==native_window||candidate==folded_window||(idle_strip&&candidate==idle_strip)||candidate==owner||(quick&&candidate==quick->Window())||IsChild(native_window?native_window:window,candidate)||preview.OwnsWindow(candidate)||candidate==GetDesktopWindow()||candidate==GetShellWindow())return;
        if(owner&&GetAncestor(candidate,GA_ROOTOWNER)==owner)return;
        const HWND root=GetAncestor(candidate,GA_ROOT);DWORD process{};
        if(root&&GetWindowThreadProcessId(root,&process)){
            GUITHREADINFO info{sizeof(info)};const DWORD thread=GetWindowThreadProcessId(root,nullptr);
            const HWND focused=GetGUIThreadInfo(thread,&info)?info.hwndFocus:nullptr;
            if(root!=previous||process!=previous_process)previous_focus=nullptr;
            if(focused&&(focused==root||IsChild(root,focused)))previous_focus=focused;
            previous=root;previous_process=process;
        }
    }
    bool QuickDestinationValid()const{
        if(!quick_active||!IsWindow(quick_target)||GetForegroundWindow()!=quick_target)return false;
        DWORD process{};const DWORD thread=GetWindowThreadProcessId(quick_target,&process);
        if(!thread||process!=quick_process)return false;
        GUITHREADINFO info{sizeof(info)};
        return GetGUIThreadInfo(thread,&info)&&(!quick_focus||info.hwndFocus==quick_focus);
    }
    void CloseQuick(){
        CancelShortcutProbe();quick_active=false;quick_input.Stop();if(window)KillTimer(EventWindow(),7);
        if(quick)quick->Hide();
        if(copy_quick){++copy_token;copy_pending=false;copy_quick=false;}
        if(paste_quick){KillTimer(EventWindow(),2);paste_inflight=paste_returning=paste_quick=false;}
        quick_target=quick_focus=nullptr;quick_process=0;
    }
    void UpdateQuick(){
        if(!quick_active||!quick)return;
        std::vector<clipboard::QuickRow> rows;rows.reserve(quick_ids.size());
        for(auto id:quick_ids)if(const auto* entry=history.Find(id))rows.push_back({id,entry->kind,entry->text,entry->favorite});
        quick->Update(std::move(rows),quick_selection,quick_tab,quick_continuous,copy_pending||paste_inflight,status);
        quick_input.SetBounds(quick->Bounds());
    }
    void RefreshQuick(bool first=false){
        const auto id=quick_selection<quick_ids.size()?quick_ids[quick_selection]:0;
        constexpr int categories[]={0,4,1,2,3};quick_ids=history.Filter(categories[quick_tab],L"");
        const auto found=std::find(quick_ids.begin(),quick_ids.end(),id);
        quick_selection=!first&&found!=quick_ids.end()?static_cast<size_t>(found-quick_ids.begin()):0;
        UpdateQuick();
    }
    void QuickSelect(size_t index){
        if(!quick_active||copy_pending||paste_inflight||index>=quick_ids.size())return;
        quick_selection=index;status.clear();UpdateQuick();
    }
    void QuickCategory(int category){
        if(!quick_active||copy_pending||paste_inflight)return;
        quick_tab=(category+5)%5;status.clear();RefreshQuick(true);
    }
    void QuickPaste(bool plain){
        if(!QuickDestinationValid()){CloseQuick();return;}
        if(copy_pending||paste_inflight||quick_selection>=quick_ids.size())return;
        const auto* entry=history.Find(quick_ids[quick_selection]);if(!entry)return;
        if(plain&&entry->kind!=clipboard::Kind::Text){status=L"纯文本仅支持文本记录";UpdateQuick();return;}
        copy_quick=true;copy_stack=false;copy_remove_whitespace=false;copy_plain=plain;copy_keyboard=false;copy_paste=true;
        copy_target=quick_target;copy_focus=quick_focus;copy_process=quick_process;
        if(entry->payload&&disk){
            copy_pending=true;const auto token=++copy_token;
            if(!disk->Load(*entry,token)){copy_pending=false;status=L"读取队列已满，请重试";}
            else status=L"正在准备粘贴…  Esc 取消";
        }else FinishCopy(*entry,true,copy_target,copy_process);
        UpdateQuick();
    }
    void QuickKey(WPARAM key,bool shift=false){
        if(!QuickDestinationValid()){CloseQuick();return;}
        if(key==VK_ESCAPE){CloseQuick();return;}
        if(copy_pending||paste_inflight)return;
        if(key==VK_F2){quick_continuous=!quick_continuous;status.clear();UpdateQuick();return;}
        if(key==VK_RETURN){QuickPaste(shift);return;}
        if(key==VK_LEFT||key==VK_RIGHT){QuickCategory(quick_tab+(key==VK_RIGHT?1:-1));return;}
        if(quick_ids.empty())return;
        const int last=static_cast<int>(quick_ids.size())-1;
        const int index=key==VK_HOME?0:key==VK_END?last:static_cast<int>(quick_selection)+(key==VK_UP?-1:key==VK_DOWN?1:key==VK_PRIOR?-6:key==VK_NEXT?6:0);
        QuickSelect(static_cast<size_t>(std::clamp(index,0,last)));
    }
    void CancelShortcutProbe(){shortcut_pending=false;if(window)KillTimer(EventWindow(),8);}
    void ApplyShortcutFocus(bool editable){
        CancelShortcutProbe();
        if(!shortcut_focus.Current(GetForegroundWindow()))return;
        if(editable)ShowQuick();else Show();
    }
    void ShortcutFocusTick(){
        if(!shortcut_pending)return;
        if(!enabled||!shortcut_key||hotkeys_suspended||(hotkey_allowed&&!hotkey_allowed())||
            !shortcut_focus.Current(GetForegroundWindow())||!ime_windows.empty()){CancelShortcutProbe();return;}
        const auto editable=input_focus_probe.Result();
        if(editable.has_value())ApplyShortcutFocus(*editable);
        else if(GetTickCount64()>=shortcut_deadline)ApplyShortcutFocus(false);
    }
    void ShowFromShortcut(){
        CancelShortcutProbe();
        if(!enabled||hotkeys_suspended)return;
        if(quick_active&&!QuickDestinationValid())CloseQuick();
        if((copy_pending||paste_inflight)&&!quick_active)return;
        std::erase_if(ime_windows,[](HWND h){return !IsWindow(h)||!IsWindowVisible(h);});
        if(!ime_windows.empty())return;
        const HWND foreground=GetForegroundWindow();RememberTarget(foreground);
        if(!foreground||previous!=foreground){Show();return;}
        // Snapshot current focus rather than the remembered paste destination.
        shortcut_focus=clipboard::InputFocusSnapshot::Capture(foreground);
        if(const auto editable=clipboard::NativeEditableInput(shortcut_focus);editable.has_value()){
            ApplyShortcutFocus(*editable);return;
        }
        // Browser/custom controls expose editability through UI Automation. Never
        // query third-party providers in the keyboard hook or block the UI thread.
        if(!input_focus_probe.Begin(shortcut_focus)){ApplyShortcutFocus(false);return;}
        shortcut_pending=true;shortcut_deadline=GetTickCount64()+300;
        if(!SetTimer(EventWindow(),8,15,nullptr))ApplyShortcutFocus(false);
    }
    void ShowQuick(){
        if(quick_active){CloseQuick();return;}
        MSG stale{};while(PeekMessageW(&stale,EventWindow(),clipboard::QuickKeyMessage,clipboard::QuickWheelMessage,PM_REMOVE)){}
        if(!enabled||hotkeys_suspended||copy_pending||paste_inflight)return;
        std::erase_if(ime_windows,[](HWND h){return !IsWindow(h)||!IsWindowVisible(h);});
        if(!ime_windows.empty())return; // Candidate keys belong to the active IME.
        const HWND foreground=GetForegroundWindow();RememberTarget(foreground);
        if(!IsWindow(previous)||previous!=foreground)return;
        if(expanded)Fold();else ClosePreview();
        quick_target=previous;quick_focus=previous_focus;quick_process=previous_process;quick_active=true;
        if(!QuickDestinationValid()){CloseQuick();return;}
        if(!quick){clipboard::QuickCallbacks callbacks;
            callbacks.select=[this](size_t index){QuickSelect(index);};
            callbacks.paste=[this](size_t index){QuickSelect(index);QuickPaste(false);};
            callbacks.category=[this](int category){QuickCategory(category);};
            callbacks.move=[this](int delta){if(QuickDestinationValid()&&!quick_ids.empty())QuickSelect(static_cast<size_t>(std::clamp(static_cast<int>(quick_selection)+delta,0,static_cast<int>(quick_ids.size())-1)));};
            callbacks.close=[this]{CloseQuick();};
            quick=std::make_unique<clipboard::QuickWindow>(std::move(callbacks));
        }
        POINT point{};GetCursorPos(&point);GUITHREADINFO info{sizeof(info)};
        if(GetGUIThreadInfo(GetWindowThreadProcessId(quick_target,nullptr),&info)&&info.hwndCaret&&info.rcCaret.bottom>info.rcCaret.top){point={info.rcCaret.left,info.rcCaret.top};ClientToScreen(info.hwndCaret,&point);}
        status.clear();RefreshQuick(true);
        const float initial_scale=GetDpiForWindow(quick_target)/96.f;
        quick->Show(window,point,initial_scale,dark);
        // A DPI-unaware target can report 96 even on a scaled monitor.
        // Once placed, our own per-monitor window supplies the actual DPI.
        const float popup_scale=GetDpiForWindow(quick->Window())/96.f;
        if(popup_scale>0&&popup_scale!=initial_scale)quick->Show(window,point,popup_scale,dark);
        if(!quick_input.Start(EventWindow(),quick_target,quick->Bounds(),[this]{return quick_active&&!hotkeys_suspended&&QuickDestinationValid();})){
            CloseQuick();status=L"快捷取用暂不可用，可点击侧边栏";Invalidate();return;
        }
        SetTimer(EventWindow(),7,100,nullptr);
    }
    void Show(bool near_cursor=true){CloseQuick();if(!enabled)return;if(near_cursor&&!pinned){POINT cursor{};if(GetCursorPos(&cursor))summon_cursor=cursor;}KillTimer(EventWindow(),4);KillTimer(EventWindow(),11);ClosePreview();const HWND foreground=GetForegroundWindow();RememberTarget(foreground);expanded=true;SetWindowTextW(search,L"");query.clear();scroll=0;scroll_offset=0;Filter();selected=visible.empty()?0:visible.front();Place(true);if(opening){SetForegroundWindow(folded_window);SetFocus(folded_window);}else{SetForegroundWindow(window);SetFocus(window);}}
    void Fold(){
        // Expand -> fold continuity: remember where the cards are before the panel goes away.
        std::vector<RECT> fly_back;
        if(stack&&expanded&&enabled&&!test_mode&&MotionEnabled()&&window&&IsWindowVisible(window)){RECT panel{};GetWindowRect(window,&panel);fly_back=StackSlots(panel);}
        FoldNow();
        if(stack&&!fly_back.empty()&&!expanded)stack->Fly(fly_back,false);
    }
    void FoldNow(){summon_cursor.reset();StopFx();StopScrollAnim();CommitPendingDelete();msel.clear();last_click.reset();StopOpening(false);paste_click.reset();paste_inflight=false;scroll_drag=false;if(GetCapture()==window)ReleaseCapture();CloseMore();ClosePopup();CancelNaming();CancelReads();ClearFileIcons();app_icon.Reset();brush.Reset();checker.Reset();target.Reset();surface.reset();search_text_glyphs=0;ime_active=false;ime_original.clear();ime_text.clear();expanded=false;KillTimer(EventWindow(),2);KillTimer(EventWindow(),3);Place();SetTimer(EventWindow(),4,10000,nullptr);}
    // ---- Custom groups ----------------------------------------------------
    int TabCount()const{return clipboard::History::GroupTabBase+static_cast<int>(history.groups.size());}
    uint32_t TabGroup()const{return tab>=clipboard::History::GroupTabBase&&tab<TabCount()?history.groups[static_cast<size_t>(tab-clipboard::History::GroupTabBase)].id:0;}
    std::wstring GroupName(uint32_t id)const{const auto* g=history.FindGroup(id);return g?g->name:std::wstring{};}
    // The software D2D factory accumulates rasterizer caches (tens of MB) with use.
    // Render() recreates it on demand, so the folded idle state never keeps them.
    void ReleaseRenderer(){images.clear();file_bitmaps.clear();app_icon.Reset();brush.Reset();checker.Reset();target.Reset();factory.Reset();}
    void CommitHistory(){if(disk)disk->Commit(history.entries,history.groups);}
    void SelectTab(int value){flavor.reset();tab=std::clamp(value,0,std::max(0,TabCount()-1));scroll=0;scroll_offset=0;reveal_tab=true;Filter();RevealSelection();Invalidate();}
    float Measure(const std::wstring& s,int size){
        ComPtr<IDWriteTextLayout> layout;DWRITE_TEXT_METRICS metrics{};
        if(!writer||FAILED(writer->CreateTextLayout(s.data(),static_cast<UINT32>(s.size()),Format(size),4096,64,&layout))||FAILED(layout->GetMetrics(&metrics)))return static_cast<float>(s.size()*static_cast<size_t>(size));
        return metrics.widthIncludingTrailingWhitespace;
    }
    // Moves an entry into a group; choosing its current group again removes it (0 always removes).
    void AssignGroup(uint64_t entry,uint32_t group){
        auto* e=history.Find(entry);if(!e){status=L"请先选择一条记录";Invalidate();return;}
        if(group&&e->group==group)group=0;
        const uint32_t before=e->group;
        if(!group&&!before){status=L"这条记录不在分组中";Invalidate();return;}
        if(!history.SetGroup(entry,group)){status=L"收藏和分组最多 200 条，请先移出一些再试";Invalidate();return;}
        selected=entry;status=group?L"已移入“"+GroupName(group)+L"” · Ctrl+0 移出":L"已移出“"+GroupName(before)+L"”";
        CommitHistory();Filter();RevealSelection();Invalidate();
    }
    void DeleteGroup(uint32_t group){
        const int index=history.GroupIndex(group);if(index<0)return;const auto name=GroupName(group);
        const int group_tab=clipboard::History::GroupTabBase+index;
        history.RemoveGroup(group);if(tab==group_tab)tab=0;else if(tab>group_tab)--tab;
        CommitHistory();scroll=0;scroll_offset=0;reveal_tab=true;Filter();status=L"已删除分组“"+name+L"”，记录仍保留在“全部”";Invalidate();
    }
    bool PopupSelectable(int i)const{return i>=0&&i<static_cast<int>(popup_items.size())&&popup_items[static_cast<size_t>(i)].command&&popup_items[static_cast<size_t>(i)].enabled;}
    int NextPopupRow(int from,int step)const{
        const int n=static_cast<int>(popup_items.size());int i=from;
        for(int k=0;k<n;++k){i=from<0&&k==0?(step>0?0:n-1):((i+step)%n+n)%n;if(PopupSelectable(i))return i;}
        return -1;
    }
    // Lays out and opens the popup at (x,y); when it does not fit below, it opens above `above`.
    void OpenPopup(std::vector<PopupItem> items,float x,float y,bool keyboard,float above=-1){
        CloseMore();if(!expanded||items.empty())return;
        popup_items=std::move(items);popup_rows.clear();
        int rows=0;float fixed=12;for(const auto& item:popup_items){if(item.label.empty())fixed+=9;else if(item.command)++rows;else fixed+=24;}
        const float top_limit=128,bottom_limit=H-12.f;
        const float row_height=rows?std::clamp((bottom_limit-top_limit-fixed)/static_cast<float>(rows),22.f,32.f):32.f;
        const float popup_width=std::min(W-24.f,236.f),popup_height=fixed+row_height*static_cast<float>(rows);
        const float left=std::clamp(x,12.f,std::max(12.f,W-12.f-popup_width));
        float top=y;if(top+popup_height>bottom_limit)top=(above>=0?above:y)-popup_height;
        top=std::clamp(top,top_limit,std::max(top_limit,bottom_limit-popup_height));
        popup_rect={left,top,left+popup_width,top+popup_height};
        float cursor=top+6;for(const auto& item:popup_items){const float h=item.label.empty()?9.f:item.command?row_height:24.f;popup_rows.push_back({left+6,cursor,left+popup_width-6,cursor+h});cursor+=h;}
        popup_hover=keyboard?NextPopupRow(-1,1):-1;popup_open=true;
        SetFocus(window);SetCapture(window);Invalidate();
    }
    void ClosePopup(){
        if(!popup_open)return;popup_open=false;popup_hover=-1;popup_items.clear();popup_rows.clear();
        if(GetCapture()==window)ReleaseCapture();Invalidate();
    }
    bool InsidePopup(float x,float y)const{return popup_open&&x>=popup_rect.left&&x<popup_rect.right&&y>=popup_rect.top&&y<popup_rect.bottom;}
    int PopupHit(float x,float y)const{
        for(size_t i=0;i<popup_rows.size();++i){const auto& r=popup_rows[i];if(x>=r.left&&x<r.right&&y>=r.top&&y<r.bottom)return PopupSelectable(static_cast<int>(i))?static_cast<int>(i):-1;}
        return -1;
    }
    void ChoosePopup(int i){
        if(!PopupSelectable(i))return;const auto item=popup_items[static_cast<size_t>(i)];const auto entry=popup_entry;const auto group=popup_group;
        if(item.command==4){history.CycleGroupColor(group);if(const auto* g=history.FindGroup(group))popup_items[static_cast<size_t>(i)].dot=g->color;CommitHistory();Invalidate();return;}
        ClosePopup();
        switch(item.command){
        case 1:AssignGroup(entry,item.arg);break;
        case 2:BeginNaming(1,0,entry);break;
        case 3:BeginNaming(2,group,0);break;
        case 5:DeleteGroup(group);break;
        case 6:BeginNaming(1,0,0);break;
        default:break;
        }
    }
    // Returns true when the key was consumed; other keys close the popup and fall through.
    bool PopupKey(WPARAM key){
        if(key==VK_ESCAPE){ClosePopup();return true;}
        if(key==VK_UP||key==VK_DOWN||key==VK_TAB){popup_hover=NextPopupRow(popup_hover,key==VK_UP?-1:1);Invalidate();return true;}
        if(key==VK_RETURN||key==VK_SPACE){ChoosePopup(popup_hover);return true;}
        ClosePopup();return false;
    }
    void OpenEntryPopup(uint64_t entry,float x,float y,bool keyboard,float above=-1){
        const auto* e=history.Find(entry);if(!e)return;selected=entry;
        std::vector<PopupItem> items;items.push_back({L"移到分组"});
        for(size_t i=0;i<history.groups.size();++i){const auto& g=history.groups[i];items.push_back({g.name,i<9?L"Ctrl+"+std::to_wstring(i+1):std::wstring{},g.color,1,g.id,true,e->group==g.id});}
        if(history.groups.empty())items.push_back({L"还没有分组",L"",0,1,0,false});
        items.push_back({L"移出分组",L"Ctrl+0",0,1,0,e->group!=0});
        items.push_back({});
        items.push_back({L"新建分组并移入…",L"Ctrl+Shift+N",0,2,0,history.groups.size()<clipboard::History::MaxGroups});
        popup_entry=entry;popup_group=0;OpenPopup(std::move(items),x,y,keyboard,above);
    }
    // Opens the group list under the card's folder button (mouse or Ctrl+G).
    void OpenEntryPopupAtButton(uint64_t entry,bool keyboard){
        for(const auto& hit:hits)if(hit.action==35&&hit.id==entry){OpenEntryPopup(entry,hit.rect.right-236,hit.rect.bottom+2,keyboard,hit.rect.top-2);return;}
        OpenEntryPopup(entry,W/2.f-118,ListTop+24,keyboard);
    }
    void OpenGroupPopup(uint32_t group,float x,float y){
        const auto* g=history.FindGroup(group);if(!g)return;
        std::vector<PopupItem> items;items.push_back({L"分组 · "+g->name});
        items.push_back({L"重命名…",L"",0,3});
        items.push_back({L"更换颜色",L"",g->color,4});
        items.push_back({L"删除分组",L"记录保留",0,5});
        items.push_back({});
        items.push_back({L"新建分组…",L"",0,6,0,history.groups.size()<clipboard::History::MaxGroups});
        popup_entry=0;popup_group=group;OpenPopup(std::move(items),x,y,false);
    }
    void DrawPopup(){
        const UINT32 ink=dark?0xe7effb:0x35445b,muted=dark?0x91a4bf:0x95a3b7;const auto r=popup_rect;
        for(int i=5;i>=1;--i){const float inset=static_cast<float>(i);brush->SetColor(D2D1::ColorF(0x274568,dark?.025f:.012f));target->FillRoundedRectangle(D2D1::RoundedRect({r.left-inset,r.top-inset/2,r.right+inset,r.bottom+inset},12+inset,12+inset),brush.Get());}
        Box(r,dark?0x222f42:0xfcfdff,dark?0x3a4c66:0xe1e8f2,12);
        for(size_t i=0;i<popup_items.size()&&i<popup_rows.size();++i){
            const auto& item=popup_items[i];const auto row=popup_rows[i];
            if(item.label.empty()){Color(dark?0x3a4a60:0xe9eef5);target->DrawLine({row.left+10,row.top+4.5f},{row.right-10,row.top+4.5f},brush.Get());continue;}
            if(!item.command){TextLine(item.label,{row.left+10,row.top+2,row.right-8,row.bottom},10,muted);continue;}
            const bool danger=item.command==5,hot=static_cast<int>(i)==popup_hover&&item.enabled;
            if(hot)Box(row,danger?(dark?0x4a303d:0xffeeee):(dark?0x2d4262:0xeaf2ff),danger?(dark?0x4a303d:0xffeeee):(dark?0x2d4262:0xeaf2ff),7);
            const float mid=(row.top+row.bottom)/2;float label_left=row.left+12;
            if(item.dot){Color(item.dot);target->FillEllipse(D2D1::Ellipse({row.left+16,mid},4.5f,4.5f),brush.Get());label_left=row.left+28;}
            else if(item.command!=1||!item.arg){const wchar_t glyph=item.command==1?L'\xE894':item.command==2||item.command==6?L'\xE710':item.command==3?L'\xE8AC':L'\xE74D';if(item.enabled)Center(std::wstring(1,glyph),{row.left+6,row.top,row.left+26,row.bottom},11,danger?0xd46a72:0x8092ac,true);label_left=row.left+28;}
            float right=row.right-8;
            if(!item.hint.empty()){const float w=Measure(item.hint,10);TextLine(item.hint,{right-w-2,row.top,right,row.bottom},10,muted);right-=w+8;}
            if(item.checked){Center(L"\xE73E",{right-18,row.top,right,row.bottom},11,0x3975ef,true);right-=20;}
            TextLine(item.label,{label_left,row.top,right,row.bottom},12,!item.enabled?muted:danger?0xd15c66:ink);
        }
    }
    // Inline naming reuses the self-painted search edit (IME + LumaText painting); the query is restored afterwards.
    void BeginNaming(int mode,uint32_t group,uint64_t entry){
        if(!expanded||!search)return;
        if(mode==1&&history.groups.size()>=clipboard::History::MaxGroups){status=L"最多 20 个分组";Invalidate();return;}
        if(mode==2&&!history.FindGroup(group))return;
        ClosePopup();CloseMore();
        if(!naming)naming_query=query;
        naming=mode;naming_group=group;naming_entry=entry;
        SendMessageW(search,EM_SETLIMITTEXT,clipboard::History::MaxGroupName,0);
        SetWindowTextW(search,mode==2?GroupName(group).c_str():L"");
        ShowWindow(search,SW_SHOW);SetFocus(search);SendMessageW(search,EM_SETSEL,0,-1);
        status=mode==2?L"重命名分组 · Enter 确定 · Esc 取消":entry?L"新建分组并移入所选记录 · Enter 确定 · Esc 取消":L"输入新分组名称 · Enter 确定 · Esc 取消";Invalidate();
    }
    void EndNaming(){
        if(!naming)return;
        SendMessageW(search,EM_SETLIMITTEXT,512,0);SetWindowTextW(search,naming_query.c_str());  // EN_CHANGE is ignored while naming
        query=naming_query;naming=0;naming_group=0;naming_entry=0;naming_query.clear();
        if(GetFocus()==search)SetFocus(window);
        Filter();Invalidate();
    }
    void CancelNaming(){if(!naming)return;EndNaming();status.clear();Invalidate();}
    void CommitNaming(){
        if(!naming)return;wchar_t value[64]{};GetWindowTextW(search,value,64);
        const auto name=clipboard::History::CleanGroupName(value);
        if(name.empty()){status=L"分组名称不能为空 · Esc 取消";Invalidate();return;}
        if(history.GroupNameTaken(name,naming==2?naming_group:0)){status=L"已有同名分组，请换一个名称";Invalidate();return;}
        const int mode=naming;const auto group=naming_group;const auto entry=naming_entry;EndNaming();
        if(mode==2){history.RenameGroup(group,name);CommitHistory();reveal_tab=true;status=L"已重命名为“"+name+L"”";Invalidate();return;}
        const auto id=history.AddGroup(name);if(!id){status=L"最多 20 个分组";Invalidate();return;}
        CommitHistory();reveal_tab=true;
        if(entry&&history.Find(entry)){AssignGroup(entry,id);if(const auto* e=history.Find(entry);e&&e->group==id)status=L"已新建“"+name+L"”并移入所选记录";Invalidate();return;}
        const int index=history.GroupIndex(id);SelectTab(clipboard::History::GroupTabBase+index);
        status=index<9?L"已新建“"+name+L"” · 在其他标签选中记录按 Ctrl+"+std::to_wstring(index+1)+L" 移入":L"已新建“"+name+L"” · 右键记录可移入";Invalidate();
    }
    int MenuCount()const{return menu_confirm?2:3;}
    bool MenuEnabled(int row)const{if(row==0||menu_confirm)return true;if(row==1)return std::any_of(history.entries.begin(),history.entries.end(),[](const clipboard::Entry& e){return !clipboard::History::Protected(e);});return !history.entries.empty();}
    int MenuHit(float x,float y)const{
        if(x<W-256||x>W-24)return -1;
        const float first=menu_confirm?112.f:76.f;
        for(int i=0;i<MenuCount();++i)if(y>=first+i*38&&y<first+i*38+34)return i;
        return -1;
    }
    void OpenMore(){
        ClosePopup();menu_open=true;menu_confirm=false;menu_hover=0;ShowWindow(search,SW_HIDE);SetFocus(window);SetCapture(window);Invalidate();
    }
    void CloseMore(){
        if(!menu_open)return;menu_open=false;menu_confirm=false;
        if(GetCapture()==window)ReleaseCapture();if(expanded)ShowWindow(search,SW_SHOW);Invalidate();
    }
    void ChooseMore(int row){
        if(row<0||row>=MenuCount()||!MenuEnabled(row))return;
        if(menu_confirm){if(row==1){CancelReads();if(disk)disk->CancelSaves();history.Clear(false);if(disk)disk->Commit(history.entries,history.groups);images.clear();ClearFileIcons();Filter();}CloseMore();return;}
        if(row==0){CloseMore();Fold();return;}
        if(row==1){CancelReads();if(disk)disk->CancelSaves();history.Clear(true);if(disk)disk->Commit(history.entries,history.groups);images.clear();ClearFileIcons();Filter();CloseMore();return;}
        menu_confirm=true;menu_hover=0;Invalidate();
    }
    void MenuKey(WPARAM key){
        if(key==VK_ESCAPE){CloseMore();return;}
        if(key==VK_RETURN||key==VK_SPACE){ChooseMore(menu_hover);return;}
        if(key==VK_UP||key==VK_DOWN||key==VK_TAB){const int n=MenuCount(),direction=key==VK_UP?-1:1;for(int i=0;i<n;++i){menu_hover=(menu_hover+direction+n)%n;if(MenuEnabled(menu_hover))break;}Invalidate();}
    }
    void Read(){
        if(!enabled||test_mode)return;const DWORD current=GetClipboardSequenceNumber();if(current==sequence)return;
        // Paused: existing history stays searchable and pasteable; nothing new is recorded.
        if(paused){sequence=current;status=L"记录已暂停，本次复制没有加入历史";Invalidate();return;}
        bool busy=false;auto entry=clipboard::Read(window,busy);
        if(busy&&retry++<5){SetTimer(EventWindow(),1,50,nullptr);return;}
        KillTimer(EventWindow(),1);retry=0;sequence=current;
        if(entry){if(!disk||!disk->Save(std::move(*entry)))status=L"保存队列或单条容量已满，未记录该条";Invalidate();}
    }
    IDWriteTextFormat* Format(int size,bool icon=false){auto& f=fonts[{size,icon}];if(!f){CheckWin32(SUCCEEDED(writer->CreateTextFormat(icon?L"Segoe MDL2 Assets":L"Microsoft YaHei UI",nullptr,(size==22||size==24)?DWRITE_FONT_WEIGHT_SEMI_BOLD:DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,static_cast<float>(size),L"zh-CN",&f)),"Clipboard font");f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);f->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);}return f.Get();}
    void Color(UINT32 c){brush->SetColor(D2D1::ColorF(c));}
    void Text(const std::wstring& s,D2D1_RECT_F r,int size,UINT32 c,bool icon=false){text_glyphs+=text_renderer.Draw(target.Get(),writer.Get(),s,Format(size,icon),r,D2D1::ColorF(c)).freetype_glyphs;}
    void Center(const std::wstring& s,D2D1_RECT_F r,int size,UINT32 c,bool icon=false){auto* f=Format(size,icon);f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);Text(s,r,size,c,icon);f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);}
    void TextLine(const std::wstring& text,D2D1_RECT_F rect,int size,UINT32 color){
        if(rect.right<=rect.left||rect.bottom<=rect.top)return;
        ComPtr<IDWriteTextLayout> layout;ComPtr<IDWriteInlineObject> ellipsis;
        if(FAILED(writer->CreateTextLayout(text.data(),static_cast<UINT32>(text.size()),Format(size),rect.right-rect.left,rect.bottom-rect.top,&layout)))return;
        layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        if(SUCCEEDED(writer->CreateEllipsisTrimmingSign(Format(size),&ellipsis))){const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER,0,0};layout->SetTrimming(&trimming,ellipsis.Get());}
        text_glyphs+=text_renderer.DrawLayout(target.Get(),writer.Get(),layout.Get(),{rect.left,rect.top},rect,D2D1::ColorF(color)).freetype_glyphs;
    }
    void TextBlock(const std::wstring& text,D2D1_RECT_F rect,int size,UINT32 color){
        const float line=std::ceil(size*1.35f),text_height=std::floor((rect.bottom-rect.top)/line)*line;
        if(text_height<=0||rect.right<=rect.left)return;
        ComPtr<IDWriteTextLayout> layout;ComPtr<IDWriteInlineObject> ellipsis;
        if(FAILED(writer->CreateTextLayout(text.data(),static_cast<UINT32>(text.size()),Format(size),rect.right-rect.left,text_height,&layout)))return;
        layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        layout->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM,line,std::ceil(size*1.05f));
        if(SUCCEEDED(writer->CreateEllipsisTrimmingSign(Format(size),&ellipsis))){const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER,0,0};layout->SetTrimming(&trimming,ellipsis.Get());}
        UINT32 count{};layout->GetLineMetrics(nullptr,0,&count);const auto visible_lines=static_cast<UINT32>(text_height/line);
        if(count>visible_lines){
            std::vector<DWRITE_LINE_METRICS> lines(count);if(SUCCEEDED(layout->GetLineMetrics(lines.data(),count,&count))){
                std::wstring shown;size_t offset=0;for(UINT32 i=0;i<visible_lines;++i){if(i)shown+=L'\n';shown+=text.substr(offset,lines[i].length-lines[i].newlineLength);offset+=lines[i].length;}shown+=L"…";
                layout.Reset();if(FAILED(writer->CreateTextLayout(shown.data(),static_cast<UINT32>(shown.size()),Format(size),rect.right-rect.left,text_height,&layout)))return;
                layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);layout->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM,line,std::ceil(size*1.05f));
                if(ellipsis){const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER,0,0};layout->SetTrimming(&trimming,ellipsis.Get());}
            }
        }
        text_glyphs+=text_renderer.DrawLayout(target.Get(),writer.Get(),layout.Get(),{rect.left,rect.top},rect,D2D1::ColorF(color)).freetype_glyphs;
    }
    void Card(D2D1_RECT_F r,UINT32 fill,UINT32 border,bool highlighted=false){
        brush->SetColor(D2D1::ColorF(fill,highlighted?.88f:.57f));target->FillRoundedRectangle(D2D1::RoundedRect(r,13,13),brush.Get());
        brush->SetColor(D2D1::ColorF(border,highlighted?.65f:.79f));target->DrawRoundedRectangle(D2D1::RoundedRect(r,13,13),brush.Get());
        if(highlighted){Color(0x689af0);target->FillRoundedRectangle(D2D1::RoundedRect({r.left,r.top+23,r.left+3,r.bottom-23},1.5f,1.5f),brush.Get());}
    }
    void Box(D2D1_RECT_F r,UINT32 fill,UINT32 border,float radius=10){Color(fill);target->FillRoundedRectangle(D2D1::RoundedRect(r,radius,radius),brush.Get());Color(border);target->DrawRoundedRectangle(D2D1::RoundedRect(r,radius,radius),brush.Get());}
    void ClipboardIcon(float x,float y,float size){app_icon.Draw(target.Get(),{x,y,x+size,y+size});}
    // `box` < 32 draws the same 32 DIP icon scaled down (header 30, card action bar 28).
    void Icon(wchar_t glyph,float x,float y,int action,uint64_t id=0,bool active=false,bool on_accent=false,float box=32){
        if(action)PushHit({x,y,x+box,y+box},action,id);
        D2D1_MATRIX_3X2_F outer_transform{};target->GetTransform(&outer_transform);
        const D2D1_MATRIX_3X2_F previous_transform=box==32?outer_transform:D2D1::Matrix3x2F::Scale(box/32,box/32,{x,y})*outer_transform;target->SetTransform(previous_transform);
        const D2D1_RECT_F r{x,y,x+32,y+32};
        if(hover>=0&&hover<static_cast<int>(hits.size())&&action&&hits[static_cast<size_t>(hover)].action==action&&hits[static_cast<size_t>(hover)].id==id)Box(r,on_accent?0x1769d9:dark?0x34435a:0xe6efff,on_accent?0x1769d9:dark?0x34435a:0xe6efff,6);
        const float icon_scale=action==14?.65f:(action==31||action==34)?.72f:.80f;
        target->SetTransform(D2D1::Matrix3x2F::Scale(icon_scale,icon_scale,{x+16,y+16})*previous_transform);
        if(glyph==L'\xE8C8'||glyph==L'\xE77F'||glyph==L'\xE74D'){
            Color(on_accent?0xffffff:dark?0xaabbd1:0x748198);const float left=x+9,top=y+8;
            if(glyph==L'\xE8C8'){
                target->DrawRoundedRectangle(D2D1::RoundedRect({left+3,top,left+15,top+15},2,2),brush.Get(),1.4f);
                target->DrawLine({left,top+4},{left,top+18},brush.Get(),1.4f);target->DrawLine({left,top+18},{left+11,top+18},brush.Get(),1.4f);
            }else if(glyph==L'\xE77F'){
                target->DrawRoundedRectangle(D2D1::RoundedRect({left,top+3,left+14,top+18},2,2),brush.Get(),1.4f);
                target->DrawRoundedRectangle(D2D1::RoundedRect({left+3,top,left+11,top+5},1,1),brush.Get(),1.4f);
            }else{
                target->DrawRoundedRectangle(D2D1::RoundedRect({left+1,top+4,left+13,top+18},1.5f,1.5f),brush.Get(),1.4f);
                target->DrawLine({left-1,top+3},{left+15,top+3},brush.Get(),1.4f);target->DrawLine({left+4,top},{left+10,top},brush.Get(),1.4f);
                for(float xx:{left+5,left+9})target->DrawLine({xx,top+7},{xx,top+14},brush.Get(),1.3f);
            }
        }else if(glyph==L'\xE734'){
            Color(active?(dark?0xffce60:0xc98a0a):(dark?0xaabbd1:0x748198));
            ComPtr<ID2D1PathGeometry> star;ComPtr<ID2D1GeometrySink> sink;
            CheckWin32(SUCCEEDED(factory->CreatePathGeometry(&star)),"Favorite star");
            CheckWin32(SUCCEEDED(star->Open(&sink)),"Favorite star path");
            for(int i=0;i<10;++i){const float angle=-1.57079633f+i*.62831853f,radius=i%2?4.5f:10.f;const D2D1_POINT_2F point{x+16+radius*std::cos(angle),y+16+radius*std::sin(angle)};if(i==0)sink->BeginFigure(point,D2D1_FIGURE_BEGIN_FILLED);else sink->AddLine(point);}
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);CheckWin32(SUCCEEDED(sink->Close()),"Favorite star close");
            if(active)target->FillGeometry(star.Get(),brush.Get());target->DrawGeometry(star.Get(),brush.Get(),1.4f);
        }else if(glyph==L'\xE718'){
            Color(on_accent?0xffffff:active?0x2580ff:dark?0xaabbd1:0x748198);const float weight=active?1.9f:1.4f;
            target->DrawLine({x+9,y+25},{x+15,y+18},brush.Get(),weight);target->DrawLine({x+11,y+13},{x+21,y+23},brush.Get(),weight);
            target->DrawLine({x+11,y+13},{x+17,y+11},brush.Get(),weight);target->DrawLine({x+21,y+23},{x+23,y+17},brush.Get(),weight);
            target->DrawLine({x+17,y+11},{x+20,y+7},brush.Get(),weight);target->DrawLine({x+23,y+17},{x+27,y+14},brush.Get(),weight);target->DrawLine({x+20,y+7},{x+27,y+14},brush.Get(),weight);
        }else Center(std::wstring(1,glyph),{x,y,x+32,y+32},16,on_accent?0xffffff:active?0x2580ff:dark?0xaabbd1:0x748198,true);
        target->SetTransform(outer_transform);
    }
    void Image(const clipboard::Entry& e,D2D1_RECT_F r){
        auto found=images.find(e.id);
        if(found==images.end()){
            if(auto frame=thumbnail_cache.Find(e.id)){
                ComPtr<ID2D1Bitmap> cached;const auto props=D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),96,96);
                if(SUCCEEDED(target->CreateBitmap(D2D1::SizeU(frame->Width(),frame->Height()),frame->pixels.data(),frame->Width()*4,props,&cached)))images[e.id]=std::move(cached);
            }else if(!thumbnail_pending.contains(e.id)&&!thumbnail_failed.contains(e.id)){
                if(e.payload&&disk){if(disk->Load(e,image_token,true))thumbnail_pending.insert(e.id);}
                else if(test_mode){auto source=clipboard::Thumbnail(e);ComPtr<ID2D1Bitmap> local;if(source&&SUCCEEDED(target->CreateBitmapFromWicBitmap(source.Get(),nullptr,&local)))images[e.id]=std::move(local);else thumbnail_failed.insert(e.id);}
            }
            found=images.find(e.id);
        }
        ComPtr<ID2D1Bitmap> bitmap;if(found!=images.end())bitmap=found->second;
        if(!bitmap){Center(thumbnail_failed.contains(e.id)?L"点击重试":L"加载中…",r,10,0x6489b2);return;}
        const auto s=bitmap->GetSize();const float factor=std::min((r.right-r.left)/s.width,(r.bottom-r.top)/s.height),w=s.width*factor,h=s.height*factor;
        r.left+=(r.right-r.left-w)/2;r.top+=(r.bottom-r.top-h)/2;r.right=r.left+w;r.bottom=r.top+h;target->DrawBitmap(bitmap.Get(),r);
    }
    static std::vector<std::wstring> FileNames(const clipboard::Entry& entry){
        std::vector<std::wstring> names;for(const auto& name:entry.file_names)if(!name.empty())names.push_back(name);
        if(!names.empty())return names;
        size_t start=0;while(start<entry.text.size()&&names.size()<3){const auto end=entry.text.find(L'\n',start);const auto path=entry.text.substr(start,end==std::wstring::npos?end:end-start);const auto slash=path.find_last_of(L"\\/");names.push_back(path.substr(slash==std::wstring::npos?0:slash+1));if(end==std::wstring::npos)break;start=end+1;}return names;
    }
    static uint32_t FileCount(const clipboard::Entry& entry){return entry.file_count?entry.file_count:entry.text.empty()?0:1+static_cast<uint32_t>(std::count(entry.text.begin(),entry.text.end(),L'\n'));}
    void FileImage(const std::wstring& path,D2D1_RECT_F rect,UINT32 muted){
        const auto key=std::pair{path,clipboard::FileIconResolution(int(std::ceil(32*scale)))};
        auto found=file_bitmaps.find(key);
        if(found==file_bitmaps.end()){
            const auto source=file_icons->Get(key.first,key.second,window);
            if(source){ComPtr<ID2D1Bitmap> bitmap;const auto properties=D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),96,96);
                if(SUCCEEDED(target->CreateBitmap(D2D1::SizeU(source->Width(),source->Height()),source->pixels.data(),source->Width()*4,properties,&bitmap))){if(file_bitmaps.size()>=clipboard::FileIconCache::MaxEntries)file_bitmaps.clear();found=file_bitmaps.emplace(key,std::move(bitmap)).first;}
            }
        }
        if(found==file_bitmaps.end()){Center(L"\xE8A5",rect,22,muted,true);return;}
        const auto size=found->second->GetSize();const float factor=std::min((rect.right-rect.left)/size.width,(rect.bottom-rect.top)/size.height),w=size.width*factor,h=size.height*factor;
        rect.left+=(rect.right-rect.left-w)/2;rect.top+=(rect.bottom-rect.top-h)/2;rect.right=rect.left+w;rect.bottom=rect.top+h;target->DrawBitmap(found->second.Get(),rect,1,D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }
    void DrawMore(){
        const float left=W-264;const UINT32 ink=dark?0xe7effb:0x35445b,muted=dark?0x91a4bf:0x95a3b7;
        for(int i=5;i>=1;--i){const float inset=static_cast<float>(i);brush->SetColor(D2D1::ColorF(0x274568,dark?.025f:.012f));target->FillRoundedRectangle(D2D1::RoundedRect({left-inset,64-inset/2,W-16+inset,252+inset},12+inset,12+inset),brush.Get());}
        Box({left,64,W-16,252},dark?0x222f42:0xfcfdff,dark?0x3a4c66:0xe1e8f2,12);
        const wchar_t* normal[]={L"折叠到屏幕边缘",L"清空历史（保留收藏和分组）",L"清空全部记录…"};
        const wchar_t* confirm[]={L"取消",L"确认清空全部记录"};
        if(menu_confirm)Text(L"清空所有记录及收藏？",{left+18,73,W-32,103},13,ink);
        const float first=menu_confirm?112.f:76.f;
        for(int i=0;i<MenuCount();++i){const float y=first+i*38;const bool available=MenuEnabled(i),danger=menu_confirm?i==1:i==2;
            if(i==menu_hover&&available)Box({left+8,y,W-24,y+34},danger?(dark?0x4a303d:0xffeeee):(dark?0x2d4262:0xeaf2ff),danger?(dark?0x4a303d:0xffeeee):(dark?0x2d4262:0xeaf2ff),7);
            const wchar_t glyph=menu_confirm?(i==0?L'\xE711':L'\xE74D'):(i==0?L'\xE76C':i==1?L'\xE894':L'\xE74D');
            Center(std::wstring(1,glyph),{left+17,y,left+37,y+34},13,!available?muted:danger?0xd46a72:0x8092ac,true);
            Text(menu_confirm?confirm[i]:normal[i],{left+48,y,W-32,y+34},13,!available?muted:danger?0xd15c66:ink);
        }
        Color(dark?0x3a4a60:0xe9eef5);target->DrawLine({left+16,204},{W-32,204},brush.Get());
        Text(menu_confirm?L"系统剪贴板不受影响":L"普通 100 · 收藏/分组 200 · 128 MB",{left+17,210,W-28,241},11,muted);
    }
    void PaintSearch(){
        if(!expanded||menu_open||!IsWindowVisible(search)||!surface)return;
        RECT r{};GetWindowRect(search,&r);MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&r),2);
        const D2D1_RECT_F bounds{r.left/scale,r.top/scale,r.right/scale,r.bottom/scale};
        wchar_t buffer[513]{};GetWindowTextW(search,buffer,513);std::wstring value=buffer;if(ime_active&&!ime_text.empty())value=ime_original.substr(0,ime_first)+ime_text+ime_original.substr(ime_last);
        if(value.empty()){Text(naming?L"输入分组名称（最多 12 字）":L"搜索文本、OCR 文字、文件名…",bounds,13,dark?0x95a7bf:0x7e899a);if(GetFocus()==search&&caret_on){Color(dark?0xe7effb:0x223248);target->FillRectangle({bounds.left,bounds.top+2,bounds.left+1,bounds.bottom-2},brush.Get());}return;}
        ComPtr<IDWriteTextLayout> layout;if(FAILED(writer->CreateTextLayout(value.data(),static_cast<UINT32>(value.size()),Format(13),100000,bounds.bottom-bounds.top,&layout)))return;
        layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        const auto first_visible=std::min(static_cast<UINT32>(LOWORD(SendMessageW(search,EM_CHARFROMPOS,0,0))),static_cast<UINT32>(value.size()));
        FLOAT offset{},vertical{};DWRITE_HIT_TEST_METRICS hit{};layout->HitTestTextPosition(first_visible,FALSE,&offset,&vertical,&hit);
        const auto native_position=SendMessageW(search,EM_POSFROMCHAR,first_visible,0);
        const D2D1_POINT_2F origin{bounds.left-offset+(native_position==-1?0.f:GET_X_LPARAM(native_position)/scale),bounds.top};DWORD first{},last{};SendMessageW(search,EM_GETSEL,reinterpret_cast<WPARAM>(&first),reinterpret_cast<LPARAM>(&last));
        if(ime_active&&!ime_text.empty())first=last=ime_first+std::min(ime_cursor,static_cast<DWORD>(ime_text.size()));
        target->PushAxisAlignedClip(bounds,D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        std::vector<DWRITE_HIT_TEST_METRICS> selection;
        if(first!=last&&GetFocus()==search){UINT32 count{};layout->HitTestTextRange(first,last-first,origin.x,origin.y,nullptr,0,&count);selection.resize(count);if(count)layout->HitTestTextRange(first,last-first,origin.x,origin.y,selection.data(),count,&count);
            Color(0x267aff);for(const auto& part:selection)target->FillRectangle({part.left,part.top,part.left+part.width,part.top+part.height},brush.Get());}
        search_text_glyphs+=text_renderer.DrawLayout(target.Get(),writer.Get(),layout.Get(),origin,bounds,D2D1::ColorF(dark?0xe7effb:0x223248)).freetype_glyphs;
        for(const auto& part:selection)text_renderer.DrawLayout(target.Get(),writer.Get(),layout.Get(),origin,{part.left,part.top,part.left+part.width,part.top+part.height},D2D1::ColorF(0xffffff));
        if(first==last&&GetFocus()==search&&caret_on){FLOAT x{},y{};layout->HitTestTextPosition(last,FALSE,&x,&y,&hit);Color(dark?0xe7effb:0x223248);target->FillRectangle({origin.x+x,origin.y+y,origin.x+x+1,origin.y+y+hit.height},brush.Get());}
        if(ime_active&&!ime_text.empty()){UINT32 count{};layout->HitTestTextRange(ime_first,static_cast<UINT32>(ime_text.size()),origin.x,origin.y,nullptr,0,&count);std::vector<DWRITE_HIT_TEST_METRICS> parts(count);if(count)layout->HitTestTextRange(ime_first,static_cast<UINT32>(ime_text.size()),origin.x,origin.y,parts.data(),count,&count);Color(0x3975ef);for(const auto& part:parts)target->DrawLine({part.left,part.top+part.height-1},{part.left+part.width,part.top+part.height-1},brush.Get());}
        target->PopAxisAlignedClip();
    }
    bool ApplyBackdrop(){
        const HWND native=EventWindow();const BOOL night=dark;
        DwmSetWindowAttribute(native,20,&night,sizeof(night));
        const DWORD backdrop=3;return SUCCEEDED(DwmSetWindowAttribute(native,38,&backdrop,sizeof(backdrop)));
    }
    void Present(){
        if(opening||liquid_drag_active||!enabled)return;
        if(!expanded&&(strip_hidden||stack))return;
        SelectWindow();
        struct Reveal {Impl& p;int exceptions=std::uncaught_exceptions();~Reveal(){if(p.reveal_window&&std::uncaught_exceptions()==exceptions){p.reveal_window=false;ShowWindow(p.window,SW_SHOWNOACTIVATE);}}} reveal{*this};
        if(!expanded&&!liquid_failed){const RECT body=VisualBodyBounds();try{PresentFolded();return;}catch(...){
            liquid_surface.reset();liquid_failed=true;compact_positioned=false;LiquidBackdrop(false);
            SetWindowPos(window,nullptr,body.left,body.top,body.right-body.left,body.bottom-body.top,SWP_NOZORDER|SWP_NOACTIVATE);
        }}
        LiquidBackdrop(false);Render();if(!surface)return;
        if(!composition)composition=std::make_unique<ClipboardComposition>();
        composition->Present(window,static_cast<UINT>(width),static_cast<UINT>(height),surface->Pixels());
        HideIdleStrip();
    }
    // ---- Expanded panel, S3 (full.html #panel; geometry in panel_cards.h) ----
    std::optional<clipboard::Flavor> flavor;bool paused{};uint64_t hover_card{};
    // Per-entry caches carry a content fingerprint: ids can be reused after the
    // history is cleared, and a stale height/flavour/snippet must never survive that.
    struct CardMetric{float width{},height{};uint64_t fingerprint{};};std::map<uint64_t,CardMetric> card_metrics;
    struct FlavorEntry{clipboard::Flavor flavor{};uint64_t fingerprint{};};std::map<uint64_t,FlavorEntry> flavor_cache;
    struct CodeSnippet{std::wstring text;clipboard::HighlightResult highlight;uint64_t fingerprint{};};std::map<uint64_t,CodeSnippet> code_cache;
    // Cheap (bounded) FNV-1a over kind, sizes and up to 80 characters; never the whole 16 MB text.
    static uint64_t Fingerprint(const clipboard::Entry& e){
        uint64_t h=1469598103934665603ull;const auto mix=[&](uint64_t v){h^=v;h*=1099511628211ull;};
        mix(static_cast<uint64_t>(e.kind));mix(e.text.size());mix(e.formats.size());
        const size_t n=e.text.size();for(size_t i=0;i<std::min<size_t>(n,64);++i)mix(e.text[i]);for(size_t i=n>80?n-16:std::min<size_t>(n,64);i<n;++i)mix(e.text[i]);
        return h;
    }
    std::map<std::pair<int,int>,ComPtr<IDWriteTextFormat>> weighted_fonts;const wchar_t* mono_family{};
    ComPtr<ID2D1BitmapBrush> checker;
    void EnsureWriter(){if(!writer)CheckWin32(SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(writer.GetAddressOf()))),"Clipboard text");}
    // Flavour of an entry; text entries are classified once (history text can be 16 MB).
    clipboard::Flavor FlavorFor(const clipboard::Entry& e){
        if(e.kind!=clipboard::Kind::Text)return clipboard::FlavorOf(e);
        if(flavor_cache.size()>2*clipboard::History::MaxTotal)flavor_cache.clear();
        const uint64_t fingerprint=Fingerprint(e);auto [it,inserted]=flavor_cache.try_emplace(e.id);
        if(inserted||it->second.fingerprint!=fingerprint)it->second={clipboard::TextFlavor(e.text),fingerprint};return it->second.flavor;
    }
    IDWriteTextFormat* Weighted(int size,DWRITE_FONT_WEIGHT weight,bool mono=false){
        EnsureWriter();
        if(mono&&!mono_family){mono_family=L"Consolas";ComPtr<IDWriteFontCollection> system;UINT32 index{};BOOL exists{};
            if(SUCCEEDED(writer->GetSystemFontCollection(&system,FALSE))&&SUCCEEDED(system->FindFamilyName(L"Cascadia Code",&index,&exists))&&exists)mono_family=L"Cascadia Code";}
        auto& f=weighted_fonts[{size*2+(mono?1:0),static_cast<int>(weight)}];
        if(!f){CheckWin32(SUCCEEDED(writer->CreateTextFormat(mono?mono_family:L"Microsoft YaHei UI",nullptr,weight,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,static_cast<float>(size),L"zh-CN",&f)),"Clipboard font");
            f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);}
        return f.Get();
    }
    float MeasureWith(const std::wstring& s,IDWriteTextFormat* f){
        ComPtr<IDWriteTextLayout> layout;DWRITE_TEXT_METRICS metrics{};
        if(!writer||!f||FAILED(writer->CreateTextLayout(s.data(),static_cast<UINT32>(s.size()),f,4096,64,&layout))||FAILED(layout->GetMetrics(&metrics)))return static_cast<float>(s.size()*8);
        return metrics.widthIncludingTrailingWhitespace;
    }
    // One line through LumaText; trailing ellipsis unless `clip_only`.
    void Label(const std::wstring& text,D2D1_RECT_F rect,IDWriteTextFormat* f,UINT32 color,float alpha=1.f,DWRITE_TEXT_ALIGNMENT align=DWRITE_TEXT_ALIGNMENT_LEADING,bool clip_only=false){
        if(text.empty()||!f||rect.right<=rect.left||rect.bottom<=rect.top)return;
        ComPtr<IDWriteTextLayout> layout;if(FAILED(writer->CreateTextLayout(text.data(),static_cast<UINT32>(text.size()),f,rect.right-rect.left,rect.bottom-rect.top,&layout)))return;
        layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);layout->SetTextAlignment(align);layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        ComPtr<IDWriteInlineObject> sign;if(!clip_only&&SUCCEEDED(writer->CreateEllipsisTrimmingSign(f,&sign))){const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER,0,0};layout->SetTrimming(&trimming,sign.Get());}
        text_glyphs+=text_renderer.DrawLayout(target.Get(),writer.Get(),layout.Get(),{rect.left,rect.top},rect,D2D1::ColorF(color,alpha)).freetype_glyphs;
    }
    void Fill(D2D1_RECT_F r,UINT32 c,float a,float radius){brush->SetColor(D2D1::ColorF(c,a));target->FillRoundedRectangle(D2D1::RoundedRect(r,radius,radius),brush.Get());}
    void Stroke(D2D1_RECT_F r,UINT32 c,float a,float radius,float weight=1.f){brush->SetColor(D2D1::ColorF(c,a));const float h=weight/2;target->DrawRoundedRectangle(D2D1::RoundedRect({r.left+h,r.top+h,r.right-h,r.bottom-h},radius,radius),brush.Get(),weight);}
    bool PushHit(D2D1_RECT_F r,int action,uint64_t id){
        // Partially scrolled cards must not claim clicks above or below the list.
        if(hit_clip){r.left=std::max(r.left,hit_clip->left);r.top=std::max(r.top,hit_clip->top);r.right=std::min(r.right,hit_clip->right);r.bottom=std::min(r.bottom,hit_clip->bottom);if(r.right<=r.left||r.bottom<=r.top)return false;}
        hits.push_back({r,action,id});return hover==static_cast<int>(hits.size())-1;}
    // Hits that belong to one card: the card itself and every button of its action bar
    // (31-35 common, 36-38 image pin / OCR / annotate, 39 link, 42-45 file / colour / plain).
    static constexpr bool IsCardAction(int action){return (action>=30&&action<=39)||(action>=42&&action<=45);}
    // The bar is only hit-testable on the hovered card, so a bar button must keep its card
    // hovered; otherwise the next paint drops the bar and the stale index lands on the next card.
    uint64_t HoverCard()const{if(hover<0||hover>=static_cast<int>(hits.size()))return 0;const auto& h=hits[static_cast<size_t>(hover)];return IsCardAction(h.action)?h.id:0;}
    // .txt: whitespace collapsed like HTML white-space:normal, bounded for layout cost.
    static std::wstring CardText(const std::wstring& text,size_t limit=400){
        std::wstring out;out.reserve(std::min(text.size(),limit));bool gap=false;
        for(wchar_t c:text){if(out.size()>=limit)break;if(c==L' '||c==L'\t'||c==L'\r'||c==L'\n'||c==L'\x3000'){gap=!out.empty();continue;}if(gap){out+=L' ';gap=false;}out+=c;}
        return out;
    }
    // -webkit-line-clamp: wraps at `width`, keeps `max_lines` and ends the last with an ellipsis.
    ComPtr<IDWriteTextLayout> ClampLayout(const std::wstring& text,float wrap_width,IDWriteTextFormat* f,float line,int max_lines,int* lines_out){
        ComPtr<IDWriteTextLayout> layout;if(lines_out)*lines_out=1;
        if(!writer||text.empty()||FAILED(writer->CreateTextLayout(text.data(),static_cast<UINT32>(text.size()),f,wrap_width,line*static_cast<float>(max_lines+1),&layout)))return nullptr;
        layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        layout->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM,line,line*.78f);
        UINT32 count{};layout->GetLineMetrics(nullptr,0,&count);
        if(lines_out)*lines_out=static_cast<int>(std::clamp<UINT32>(count,1,static_cast<UINT32>(max_lines)));
        if(count<=static_cast<UINT32>(max_lines))return layout;
        std::vector<DWRITE_LINE_METRICS> metrics(count);if(FAILED(layout->GetLineMetrics(metrics.data(),count,&count)))return layout;
        std::wstring shown;size_t offset=0;
        for(int i=0;i<max_lines;++i){if(i)shown+=L'\n';shown+=text.substr(offset,metrics[static_cast<size_t>(i)].length-metrics[static_cast<size_t>(i)].newlineLength);offset+=metrics[static_cast<size_t>(i)].length;}
        while(!shown.empty()&&shown.back()==L' ')shown.pop_back();shown+=L"…";
        // Half a line of slack: with trimming on, DirectWrite drops a last line that
        // only fails to fit by float rounding (3 × 19.5 showed 2 lines).
        ComPtr<IDWriteTextLayout> clamped;if(FAILED(writer->CreateTextLayout(shown.data(),static_cast<UINT32>(shown.size()),f,wrap_width,line*(static_cast<float>(max_lines)+.5f),&clamped)))return layout;
        clamped->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);clamped->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);clamped->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM,line,line*.78f);
        ComPtr<IDWriteInlineObject> sign;if(SUCCEEDED(writer->CreateEllipsisTrimmingSign(f,&sign))){const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER,0,0};clamped->SetTrimming(&trimming,sign.Get());}
        return clamped;
    }
    float CardWidth()const{return W-2*clipboard::cards::ListInset;}
    std::pair<float,float> NaturalImageSize(const clipboard::Entry& e){
        auto size=clipboard::cards::ImageSize(e);
        if(size.first<=0)if(auto frame=thumbnail_cache.Find(e.id))size={static_cast<float>(frame->Width()),static_cast<float>(frame->Height())};
        if(size.first<=0){const auto found=images.find(e.id);if(found!=images.end()&&found->second){const auto s=found->second->GetPixelSize();size={static_cast<float>(s.width),static_cast<float>(s.height)};}}
        return size;
    }
    // Card height at the current width; cached once the content is known.
    float CardHeightFor(const clipboard::Entry& e){
        namespace cards=clipboard::cards;const float card_width=CardWidth();
        const uint64_t fingerprint=Fingerprint(e);
        if(const auto it=card_metrics.find(e.id);it!=card_metrics.end()&&it->second.width==card_width&&it->second.fingerprint==fingerprint)return it->second.height;
        const auto kind=FlavorFor(e);cards::Measure m;bool stable=true;
        if(kind==clipboard::Flavor::Image){const auto size=NaturalImageSize(e);m.image_width=size.first;m.image_height=size.second;stable=size.first>0;}
        else if(kind==clipboard::Flavor::Code)m.code_lines=cards::CodeLines(e.text.substr(0,4096));
        else if(kind==clipboard::Flavor::Text){EnsureWriter();ClampLayout(CardText(e.text),cards::ContentWidth(card_width),Format(13),cards::TextLineHeight,cards::TextMaxLines,&m.text_lines);}
        const float h=cards::CardHeight(kind,m,card_width);
        if(stable){if(card_metrics.size()>2*clipboard::History::MaxTotal)card_metrics.clear();card_metrics[e.id]={card_width,h,fingerprint};}
        return h;
    }
    std::vector<const clipboard::Entry*> VisibleEntries()const{
        std::map<uint64_t,const clipboard::Entry*> by_id;for(const auto& e:history.entries)by_id[e.id]=&e;
        std::vector<const clipboard::Entry*> out;out.reserve(visible.size());for(const auto id:visible){const auto it=by_id.find(id);out.push_back(it==by_id.end()?nullptr:it->second);}
        return out;
    }
    // ---- Pixel scrolling (S3 part 2). Block = optional time-group heading + card + gap.
    static constexpr float ListEndPad=10;   // full.html #list padding-bottom
    void BuildBlocks(){
        namespace cards=clipboard::cards;const int n=static_cast<int>(visible.size());
        block_tops.assign(static_cast<size_t>(n)+1,0.f);block_header.assign(static_cast<size_t>(n),0);if(n==0)return;
        SYSTEMTIME now{};GetLocalTime(&now);const auto entries=VisibleEntries();std::optional<cards::TimeGroup> previous_group;float y=0;
        for(int i=0;i<n;++i){
            const auto* e=entries[static_cast<size_t>(i)];const auto group=e?cards::GroupOf(*e,now):cards::TimeGroup::Earlier;
            const bool heading=!previous_group||*previous_group!=group;previous_group=group;
            block_header[static_cast<size_t>(i)]=heading;block_tops[static_cast<size_t>(i)]=y;
            y+=(heading?cards::GroupHeaderHeight:0.f)+(e?CardHeightFor(*e):0.f)+cards::Gap;
        }
        block_tops[static_cast<size_t>(n)]=y;
    }
    float ViewHeight()const{return ListBottom-ListTop;}
    float Offset()const{return offset_index==scroll?scroll_offset:0.f;}
    float ScrollPx(){
        BuildBlocks();const int n=static_cast<int>(visible.size());if(n==0)return 0;
        const int i=std::clamp(scroll,0,n-1);return block_tops[static_cast<size_t>(i)]+(i==scroll?Offset():0.f);
    }
    float ContentHeight(){BuildBlocks();return visible.empty()?0.f:block_tops.back()-clipboard::cards::Gap+ListEndPad;}
    float MaxScrollPx(){return std::max(0.f,ContentHeight()-ViewHeight());}
    void SetScrollPx(float px){
        const int n=static_cast<int>(visible.size());if(n==0){scroll=0;scroll_offset=0;offset_index=-1;return;}
        px=std::clamp(std::round(px*scale)/scale,0.f,MaxScrollPx());   // whole device pixels keep text crisp; the ends stay exact
        const int i=std::clamp(static_cast<int>(std::upper_bound(block_tops.begin(),block_tops.begin()+n,px)-block_tops.begin())-1,0,n-1);
        scroll=i;scroll_offset=std::max(0.f,px-block_tops[static_cast<size_t>(i)]);offset_index=i;
    }
    void ClampScroll(){SetScrollPx(ScrollPx());}
    void ScrollToIndex(int index,float offset=0){
        BuildBlocks();const int n=static_cast<int>(visible.size());if(n==0){SetScrollPx(0);return;}
        SetScrollPx(block_tops[static_cast<size_t>(std::clamp(index,0,n-1))]+offset);
    }
    // Like full.html: going up shows the card with its heading, going down keeps 10 px below it.
    void RevealIndex(int index){
        StopScrollAnim();BuildBlocks();const int n=static_cast<int>(visible.size());if(index<0||index>=n)return;
        const float top=block_tops[static_cast<size_t>(index)],bottom=block_tops[static_cast<size_t>(index)+1]-clipboard::cards::Gap,px=ScrollPx();
        if(top<px)SetScrollPx(top);else if(bottom>px+ViewHeight())SetScrollPx(bottom+ListEndPad-ViewHeight());
    }
    // Cards whose block intersects the list viewport (partial cards included).
    std::pair<int,int> DrawnRange(){
        const int n=static_cast<int>(visible.size());if(n==0)return {0,0};
        const float px=ScrollPx(),limit=px+ViewHeight();int first=std::clamp(scroll,0,n-1);
        while(first<n-1&&block_tops[static_cast<size_t>(first)+1]-clipboard::cards::Gap<=px)++first;
        auto card_top=[&](int i){return block_tops[static_cast<size_t>(i)]+(block_header[static_cast<size_t>(i)]?clipboard::cards::GroupHeaderHeight:0.f);};
        int end=first;while(end<n&&card_top(end)<limit)++end;
        return {first,std::max(end,first+1)};
    }
    // Cards (with their headings) fully visible from `start`; at least one.
    int FitCount(int start){
        const int n=static_cast<int>(visible.size());if(n==0||start<0||start>=n)return 1;
        float px=0;if(start==std::clamp(scroll,0,n-1))px=ScrollPx();else{BuildBlocks();px=block_tops[static_cast<size_t>(start)];}
        const float limit=px+ViewHeight()+.5f;int count=0;
        for(int i=start;i<n;++i){if(count>0&&block_tops[static_cast<size_t>(i)+1]-clipboard::cards::Gap>limit)break;++count;}
        return std::max(1,count);
    }
    // Index of the top block at the end of the list.
    int MaxScroll(){
        const float limit_px=MaxScrollPx();const int n=static_cast<int>(visible.size());if(n<=1)return 0;
        return std::clamp(static_cast<int>(std::upper_bound(block_tops.begin(),block_tops.begin()+n,limit_px)-block_tops.begin())-1,0,n-1);
    }
    // Wheel: animated toward a target while moving only (timer 12); immediate
    // when client-area animation is off and in tests.
    void WheelScroll(int delta){
        UINT lines=3;SystemParametersInfoW(SPI_GETWHEELSCROLLLINES,0,&lines,0);
        const float step=lines==WHEEL_PAGESCROLL?ViewHeight()*.9f:static_cast<float>(lines)*33.f;   // ~100 DIP per notch, as in browsers
        const float base=scroll_target>=0?scroll_target:ScrollPx();
        const float goal=std::clamp(base-static_cast<float>(delta)/WHEEL_DELTA*step,0.f,MaxScrollPx());
        BOOL animate=TRUE;SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION,0,&animate,0);hover=-1;
        if(test_mode||!animate||!window){StopScrollAnim();SetScrollPx(goal);Invalidate();return;}
        if(scroll_target<0)scroll_tick=GetTickCount64();
        scroll_target=goal;SetTimer(EventWindow(),12,USER_TIMER_MINIMUM,nullptr);
    }
    void ScrollAnimTick(){
        if(scroll_target<0||!expanded){StopScrollAnim();return;}
        const ULONGLONG tick=GetTickCount64();const float dt=static_cast<float>(std::max<ULONGLONG>(1,tick-scroll_tick));scroll_tick=tick;
        const float current=ScrollPx(),k=1.f-std::exp(-dt/60.f);float next=current+(scroll_target-current)*k;
        const bool done=std::abs(scroll_target-next)*scale<.75f;if(done)next=scroll_target;
        SetScrollPx(next);Invalidate();
        if(done){StopScrollAnim();RefreshHover();}
    }
    void StopScrollAnim(){scroll_target=-1;if(window)KillTimer(EventWindow(),12);}
    void RefreshHover(){
        if(!window||!expanded||test_mode)return;POINT point{};GetCursorPos(&point);ScreenToClient(window,&point);Render();
        const float x=static_cast<float>(point.x)/scale,y=static_cast<float>(point.y)/scale;int hit=-1;
        for(size_t i=0;i<hits.size();++i){const auto r=hits[i].rect;if(x>=r.left&&x<r.right&&y>=r.top&&y<r.bottom)hit=static_cast<int>(i);}
        if(hit!=hover){hover=hit;Invalidate();}
    }
    void SelectFlavor(clipboard::Flavor value){tab=1;flavor=value;scroll=0;scroll_offset=0;reveal_tab=true;Filter();RevealSelection();Invalidate();}
    void SetPaused(bool value){
        paused=value;if(stack)stack->SetPaused(paused);
        // Resuming never imports what was copied while paused.
        sequence=GetClipboardSequenceNumber();
        status=!layout_path.empty()&&!clipboard::SavePaused(layout_path,paused)?L"暂停状态保存失败，重启后会恢复记录":paused?L"已暂停记录 · 已有历史仍可搜索和粘贴":L"已恢复记录";
        Invalidate();
    }
    ID2D1BitmapBrush* Checker(){
        if(!checker&&target){
            // .thumb: repeating-conic-gradient(#0000000d 0 25%,#0000 0 50%) 0 0/14px 14px
            uint32_t pixels[14*14]{};for(int y=0;y<14;++y)for(int x=0;x<14;++x)if(((x/7)+(y/7))%2==0)pixels[y*14+x]=0x0d000000u;
            ComPtr<ID2D1Bitmap> tile;const auto props=D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),96,96);
            if(SUCCEEDED(target->CreateBitmap(D2D1::SizeU(14,14),pixels,14*4,props,&tile)))
                target->CreateBitmapBrush(tile.Get(),D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_WRAP,D2D1_EXTEND_MODE_WRAP,D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR),&checker);
        }
        return checker.Get();
    }
    void DrawThumb(const clipboard::Entry& e,D2D1_RECT_F box,UINT32 edge,float edge_alpha){
        const auto rounded=D2D1::RoundedRect(box,7,7);
        if(auto* pattern=Checker()){pattern->SetTransform(D2D1::Matrix3x2F::Translation(box.left,box.top));target->FillRoundedRectangle(rounded,pattern);}
        ComPtr<ID2D1RoundedRectangleGeometry> shape;ComPtr<ID2D1Layer> layer;
        const bool masked=SUCCEEDED(factory->CreateRoundedRectangleGeometry(rounded,&shape))&&SUCCEEDED(target->CreateLayer(&layer));
        if(masked)target->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(),shape.Get()),layer.Get());
        Image(e,{box.left+1,box.top+1,box.right-1,box.bottom-1});
        if(masked)target->PopLayer();
        Stroke(box,edge,edge_alpha,7);
    }
    static const wchar_t* ShortLanguage(clipboard::CodeLanguage language){
        using L=clipboard::CodeLanguage;
        switch(language){case L::Cpp:return L"C++";case L::Python:return L"PY";case L::JavaScript:return L"JS";case L::TypeScript:return L"TS";case L::Json:return L"JSON";
            case L::Bash:return L"SH";case L::PowerShell:return L"PS";case L::Sql:return L"SQL";case L::CSharp:return L"C#";case L::Markdown:return L"MD";default:return L"TXT";}
    }
    CodeSnippet& SnippetFor(const clipboard::Entry& e){
        if(code_cache.size()>64)code_cache.clear();
        const uint64_t fingerprint=Fingerprint(e);auto [it,inserted]=code_cache.try_emplace(e.id);
        if(!inserted&&it->second.fingerprint!=fingerprint){it->second=CodeSnippet{};inserted=true;}
        if(inserted){it->second.fingerprint=fingerprint;
            // pre.code shows at most four 18.6 DIP lines; keep five, each bounded.
            std::wstring& out=it->second.text;int lines=0;size_t column=0;
            for(size_t i=0;i<e.text.size()&&i<4096&&lines<5;++i){const wchar_t c=e.text[i];
                if(c==L'\r')continue;if(c==L'\n'){if(++lines<5)out+=L'\n';column=0;continue;}
                if(column>=160)continue;if(c==L'\t'){out+=L"    ";column+=4;continue;}out+=c;++column;}
            while(!out.empty()&&(out.back()==L'\n'||out.back()==L' '))out.pop_back();
            it->second.highlight=clipboard::HighlightCode(out);
        }
        return it->second;
    }
    void DrawCode(const clipboard::Entry& e,D2D1_RECT_F box,UINT32 ink){
        namespace cards=clipboard::cards;
        Fill(box,dark?0x0a0e14:0xf0f3f8,dark?.55f:.9f,7);
        const auto& snippet=SnippetFor(e);if(snippet.text.empty())return;
        // .kw .str .cm .nb .fn of full.html (light / dark).
        auto color=[&](clipboard::CodeColor c)->UINT32{using C=clipboard::CodeColor;
            switch(c){case C::Keyword:case C::Preprocessor:return dark?0xfb923c:0xc2410c;case C::String:return dark?0x86efac:0x15803d;case C::Comment:return 0x8a94a3;
                case C::Number:return dark?0xd8b4fe:0x9333ea;case C::Type:return dark?0x7cb4ff:0x2563eb;default:return ink;}};
        auto* mono=Weighted(12,DWRITE_FONT_WEIGHT_NORMAL,true);
        const D2D1_RECT_F inner{box.left+cards::CodePadX,box.top+cards::CodePadY,box.right-cards::CodePadX,box.bottom};
        target->PushAxisAlignedClip({box.left,box.top,box.right,box.bottom},D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        size_t start=0;float y=inner.top;
        while(start<=snippet.text.size()&&y<box.bottom){
            const size_t end=std::min(snippet.text.find(L'\n',start),snippet.text.size());const std::wstring line=snippet.text.substr(start,end-start);
            ComPtr<IDWriteTextLayout> layout;
            if(!line.empty()&&SUCCEEDED(writer->CreateTextLayout(line.data(),static_cast<UINT32>(line.size()),mono,4096,cards::CodeLineHeight,&layout))){
                // Segments in token colours, positioned by the line's own layout.
                size_t at=0;
                auto emit=[&](size_t from,size_t to,UINT32 c){if(to<=from)return;FLOAT x{},yy{};DWRITE_HIT_TEST_METRICS hit{};layout->HitTestTextPosition(static_cast<UINT32>(from),FALSE,&x,&yy,&hit);
                    if(inner.left+x<inner.right)Label(line.substr(from,to-from),{inner.left+x,y,inner.right,y+cards::CodeLineHeight},mono,c,1.f,DWRITE_TEXT_ALIGNMENT_LEADING,true);};
                for(const auto& span:snippet.highlight.spans){
                    const size_t s0=std::max<size_t>(span.start,start),s1=std::min<size_t>(static_cast<size_t>(span.start)+span.length,end);if(s1<=s0)continue;
                    emit(at,s0-start,ink);emit(s0-start,s1-start,color(span.color));at=s1-start;
                }
                emit(at,line.size(),ink);
            }
            y+=cards::CodeLineHeight;start=end+1;
        }
        target->PopAxisAlignedClip();
    }
    void DrawExpanded(UINT32 bg){
        namespace cards=clipboard::cards;
        const UINT32 ink=glass::Pick(glass::Ink1,dark),ink2=glass::Pick(glass::Ink2,dark),ink3=glass::Pick(glass::Ink3,dark),accent=glass::Accent,edge=glass::Pick(glass::Edge,dark);
        const UINT32 white=0xffffff,solid=dark?0x1c2532u:0xf5f8fcu,warn=0xf5a524,ok=0x2fb36d;
        const float edge_a=dark?glass::EdgeAlphaDark:glass::EdgeAlphaLight,card_a=dark?glass::CardAlphaDark:glass::CardAlphaLight,hover_a=dark?glass::CardHoverAlphaDark:glass::CardHoverAlphaLight;
        auto* f11=Weighted(11,DWRITE_FONT_WEIGHT_NORMAL);auto* f11s=Weighted(11,DWRITE_FONT_WEIGHT_SEMI_BOLD);
        auto* f12=Weighted(12,DWRITE_FONT_WEIGHT_NORMAL);auto* f13=Weighted(13,DWRITE_FONT_WEIGHT_NORMAL);
        // .ph: logo, title + shortcut line, 记录中 pill, pin / settings / more.
        ClipboardIcon(14,12,30);
        const bool narrow=W<330;const std::wstring rec_label=paused?L"已暂停":L"记录中";
        const float rec_right=W-110,rec_left=rec_right-(narrow?26.f:29.f+MeasureWith(rec_label,f12)+10.f);
        {const D2D1_RECT_F rec{rec_left,15,rec_right,39};const bool hot=PushHit(rec,15,0);Fill(rec,white,hot?hover_a:card_a,12);
            const D2D1_POINT_2F dot{rec_left+13.5f,27};brush->SetColor(D2D1::ColorF(paused?warn:ok,paused?.22f:.2f));target->FillEllipse(D2D1::Ellipse(dot,6.5f,6.5f),brush.Get());
            brush->SetColor(D2D1::ColorF(paused?warn:ok));target->FillEllipse(D2D1::Ellipse(dot,3.5f,3.5f),brush.Get());
            if(!narrow)Label(rec_label,{rec_left+23.5f,15,rec_right-6,39},f12,ink2);}
        Label(L"剪贴板",{54,12,rec_left-8,31},Weighted(15,DWRITE_FONT_WEIGHT_BOLD),ink);
        Label((hotkey?ShortcutLabel(shortcut_modifiers,shortcut_key):std::wstring(L"侧边栏打开"))+L" · Alt+1–9 快速粘贴",{54,31,rec_left-8,45},f11,ink3);
        Icon(L'\xE718',W-106,12,10,0,pinned,false,30);Icon(L'\xE713',W-74,12,11,0,false,false,30);Icon(L'\xE712',W-42,12,12,0,menu_open,false,30);
        // .search
        {const D2D1_RECT_F sr{cards::SearchInset,cards::SearchTop,W-cards::SearchInset,cards::SearchBottom};const bool focused=naming||GetFocus()==search;
            if(focused)Fill({sr.left-3,sr.top-3,sr.right+3,sr.bottom+3},accent,dark?.24f:.14f,13);
            Fill(sr,white,card_a,cards::SearchRadius);if(focused)Fill(sr,solid,.5f,cards::SearchRadius);Stroke(sr,focused?accent:edge,focused?1.f:edge_a,cards::SearchRadius);
            Center(naming?L"\xE8F4":L"\xE721",{20,sr.top,40,sr.bottom},13,naming?accent:ink3,true);
            if(!IsWindowVisible(search)||menu_open)Label(query.empty()?std::wstring(naming?L"输入分组名称（最多 12 字）":L"搜索文本、OCR 文字、文件名…"):query,{44,sr.top,W-80,sr.bottom},f13,query.empty()?ink3:ink);
            const std::wstring key=naming?L"Esc 取消":L"Ctrl F";const float kw=MeasureWith(key,Weighted(10,DWRITE_FONT_WEIGHT_NORMAL))+10;
            const D2D1_RECT_F kr{sr.right-10-kw,sr.top+9,sr.right-10,sr.bottom-9};Stroke(kr,edge,dark?.16f:.18f,5);Label(key,kr,Weighted(10,DWRITE_FONT_WEIGHT_NORMAL),ink3,1.f,DWRITE_TEXT_ALIGNMENT_CENTER);}
        // .chips: type chips (empty 代码/链接/颜色 hidden), a separator, custom groups; scrolled horizontally.
        const auto chips=cards::Chips(history,tab,flavor,[&](const clipboard::Entry& e){return FlavorFor(e);});
        const float strip_left=cards::ChipInset-2,strip_right=W-70,top=cards::ChipTop,bottom=cards::ChipBottom,radius=(bottom-top)/2;
        struct Span{float left,right;};std::vector<Span> spans;float x=cards::ChipInset;int reveal=-1;
        for(size_t i=0;i<chips.size();++i){const float w=cards::ChipPadX*2+MeasureWith(chips[i].label,f12)+4+MeasureWith(std::to_wstring(chips[i].count),f11s);spans.push_back({x,x+w});if(chips[i].selected)reveal=static_cast<int>(i);x+=w+cards::ChipGap;}
        const float separator_x=x+2;if(!history.groups.empty())x+=9;
        for(size_t i=0;i<history.groups.size();++i){const float w=std::clamp(MeasureWith(history.groups[i].name,f12)+30.f,44.f,132.f);spans.push_back({x,x+w});
            if(tab==clipboard::History::GroupTabBase+static_cast<int>(i))reveal=static_cast<int>(chips.size()+i);x+=w+cards::ChipGap;}
        tab_scroll_max=std::max(0.f,x-cards::ChipGap-strip_right);
        if(reveal_tab&&reveal>=0){const auto span=spans[static_cast<size_t>(reveal)];if(span.left-tab_scroll<strip_left+4)tab_scroll=span.left-cards::ChipInset;if(span.right-tab_scroll>strip_right-4)tab_scroll=span.right-strip_right+6;}
        reveal_tab=false;tab_scroll=std::clamp(tab_scroll,0.f,tab_scroll_max);
        target->PushAxisAlignedClip({strip_left,top-2,strip_right,bottom+2},D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        for(size_t i=0;i<spans.size();++i){
            const D2D1_RECT_F r{spans[i].left-tab_scroll,top,spans[i].right-tab_scroll,bottom};if(r.right<strip_left||r.left>strip_right)continue;
            D2D1_RECT_F hit_rect=r;hit_rect.left=std::max(r.left,strip_left);hit_rect.right=std::min(r.right,strip_right);
            if(i<chips.size()){
                const auto& c=chips[i];const bool hot=PushHit(hit_rect,c.flavor?26:20+c.tab,c.flavor?1+static_cast<uint64_t>(*c.flavor):0);
                if(c.selected)Fill(r,ink,1,radius);else if(hot)Fill(r,white,card_a,radius);
                const float lw=MeasureWith(c.label,f12);Label(c.label,{r.left+cards::ChipPadX,top,r.left+cards::ChipPadX+lw+1,bottom},f12,c.selected?solid:ink2);
                Label(std::to_wstring(c.count),{r.left+cards::ChipPadX+lw+4,top+1,r.right,bottom},f11s,c.selected?solid:ink3,c.selected?.65f:1.f);
            }else{
                const auto& g=history.groups[i-chips.size()];const bool chosen=tab==clipboard::History::GroupTabBase+static_cast<int>(i-chips.size());
                const bool hot=PushHit(hit_rect,40,g.id);if(chosen)Fill(r,ink,1,radius);else if(hot)Fill(r,white,card_a,radius);
                brush->SetColor(D2D1::ColorF(g.color));target->FillEllipse(D2D1::Ellipse({r.left+13,(top+bottom)/2},3.5f,3.5f),brush.Get());
                Label(g.name,{r.left+21,top,r.right-8,bottom},f12,chosen?solid:ink2);
            }
        }
        if(!history.groups.empty()&&separator_x-tab_scroll>strip_left&&separator_x-tab_scroll<strip_right){brush->SetColor(D2D1::ColorF(edge,edge_a*1.6f));target->DrawLine({separator_x-tab_scroll,top+5},{separator_x-tab_scroll,bottom-5},brush.Get());}
        // Soft fades mark edges with more chips; wheel, Alt+←/→ and auto-reveal reach them.
        for(int i=0;i<14;++i){const float alpha=static_cast<float>(i+1)/14.f*.9f;brush->SetColor(D2D1::ColorF(bg,alpha));
            if(tab_scroll<tab_scroll_max-.5f)target->FillRectangle({strip_right-14+static_cast<float>(i),top-2,strip_right-13+static_cast<float>(i),bottom+2},brush.Get());
            if(tab_scroll>.5f)target->FillRectangle({strip_left+13-static_cast<float>(i),top-2,strip_left+14-static_cast<float>(i),bottom+2},brush.Get());}
        target->PopAxisAlignedClip();
        {const D2D1_RECT_F r{W-66,top,W-44,bottom};const bool hot=PushHit(r,41,0);Fill(r,white,hot?hover_a:card_a,7);
            Center(L"\xE710",r,9,history.groups.size()<clipboard::History::MaxGroups?ink2:ink3,true);}
        Icon(L'\xE8CB',W-40,top-2.5f,13,0,oldest,false,28);
        // #list: time-group headings and flavour cards.
        SYSTEMTIME now{};GetLocalTime(&now);
        target->PushAxisAlignedClip({0,ListTop,W,ListBottom},D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        {const auto [first,end]=DrawnRange();const float px=ScrollPx();hit_clip=D2D1_RECT_F{0,ListTop,W,ListBottom};
            for(int i=first;i<end;++i){
                const auto* e=history.Find(visible[static_cast<size_t>(i)]);if(!e)continue;
                float card_y=ListTop+block_tops[static_cast<size_t>(i)]-px;
                if(block_header[static_cast<size_t>(i)]){Label(cards::GroupLabel(cards::GroupOf(*e,now)),{cards::ListInset+6,card_y+10,W-cards::ListInset-6,card_y+25},f11s,ink3);card_y+=cards::GroupHeaderHeight;}
                const float h=CardHeightFor(*e);
                DrawCard(*e,i,{cards::ListInset,card_y,W-cards::ListInset,card_y+h},now);
            }
            // A heading may peek in below the last card before its card does.
            if(end<static_cast<int>(visible.size())&&block_header[static_cast<size_t>(end)])if(const float heading_y=ListTop+block_tops[static_cast<size_t>(end)]-px;heading_y<ListBottom)
                if(const auto* e=history.Find(visible[static_cast<size_t>(end)]))Label(cards::GroupLabel(cards::GroupOf(*e,now)),{cards::ListInset+6,heading_y+10,W-cards::ListInset-6,heading_y+25},f11s,ink3);
            hit_clip.reset();}
        if(visible.empty()){const float mid=(ListTop+ListBottom)/2;
            Center(query.empty()?L"\xE77F":L"\xE721",{0,mid-74,W,mid-18},40,ink3,true);
            Label(query.empty()?(TabGroup()?L"这个分组还是空的":tab==4?L"还没有收藏":L"这里还没有内容"):L"没有找到“"+query+L"”",{20,mid-12,W-20,mid+8},f13,ink3,1.f,DWRITE_TEXT_ALIGNMENT_CENTER);
            Label(query.empty()?(TabGroup()?(tab-4<=9?L"在其他标签选中记录按 Ctrl+"+std::to_wstring(tab-4)+L" 移入":std::wstring(L"右键记录，选择“移到分组”")):std::wstring(tab==4?L"点击记录上的星标，或按 Ctrl+D 收藏":L"复制任意内容，或截图后按 Enter")):std::wstring(L"试试其他关键词或文件名"),
                  {20,mid+10,W-20,mid+28},f12,ink3,1.f,DWRITE_TEXT_ALIGNMENT_CENTER);}
        target->PopAxisAlignedClip();
        if(const float limit_px=MaxScrollPx();limit_px>0){const float track=ViewHeight(),thumb=ScrollThumb(),y0=ListTop+(track-thumb)*ScrollPx()/limit_px;
            Fill({W-7,y0,W-4,y0+thumb},0x808080,.35f,1.5f);}
        // #bulk: accent bar between the chips and the list while 2+ cards are selected.
        if(msel.size()>1){
            const D2D1_RECT_F bar{cards::ListInset,cards::ListTop,W-cards::ListInset,cards::ListTop+BulkHeight};auto* f12b=Weighted(12,DWRITE_FONT_WEIGHT_NORMAL);
            Fill(bar,accent,1,10);
            struct Button{const wchar_t* label;int action;};const Button buttons[]{{L"\xE711",53},{L"删除",52},{L"收藏",51},{L"合并复制",50}};
            float right=bar.right-6;
            for(const auto& b:buttons){
                const bool glyph=b.action==53;const float bw=glyph?26.f:MeasureWith(b.label,f12b)+18;const D2D1_RECT_F br{right-bw,bar.top+6,right,bar.bottom-6};
                const bool hot=PushHit(br,b.action,0);Fill(br,white,hot?.3f:.18f,7);
                if(glyph)Center(b.label,br,9,white,true);else Label(b.label,br,f12b,white,1.f,DWRITE_TEXT_ALIGNMENT_CENTER);
                right-=bw+6;
            }
            Label(L"已选 "+std::to_wstring(msel.size())+L" 项",{bar.left+12,bar.top,right,bar.bottom},f12b,white);
        }
        // .pfoot: capacity text, hints (or status), 4 px capacity bar.
        brush->SetColor(D2D1::ColorF(edge,edge_a));target->DrawLine({0,H-cards::FooterHeight+.5f},{W,H-cards::FooterHeight+.5f},brush.Get());
        const size_t bytes=history.Bytes();const std::wstring capacity=cards::CapacityText(history.entries.size(),bytes,history.Limit());const float cw=std::min(MeasureWith(capacity,f11)+2,W-28);
        Label(capacity,{14,H-38,14+cw,H-22},f11,ink3);
        float status_right=W-14;
        if(!pending_delete.empty()){   // toast action: 撤销 (Ctrl+Z)
            const float uw=MeasureWith(L"撤销",f11)+16;const D2D1_RECT_F ur{W-14-uw,H-40,W-14,H-20};
            const bool hot=PushHit(ur,54,0);Fill(ur,accent,hot?.22f:.12f,6);Label(L"撤销",ur,f11,accent,1.f,DWRITE_TEXT_ALIGNMENT_CENTER);status_right=ur.left-6;
        }
        Label(status.empty()?std::wstring(paused?L"记录已暂停 · Enter 粘贴":L"↑↓ · Enter 粘贴 · 空格预览"):status,{14+cw+8,H-38,status_right,H-22},f11,status.empty()?ink3:ink2,1.f,DWRITE_TEXT_ALIGNMENT_TRAILING);
        Fill({14,H-16,W-14,H-12},white,dark?.12f:.7f,2);Fill({14,H-16,14+(W-28)*cards::CapacityRatio(bytes),H-12},accent,1,2);
    }
    void DrawCard(const clipboard::Entry& e,int index,D2D1_RECT_F r,const SYSTEMTIME& now){
        namespace cards=clipboard::cards;
        const UINT32 ink=glass::Pick(glass::Ink1,dark),ink2=glass::Pick(glass::Ink2,dark),ink3=glass::Pick(glass::Ink3,dark),accent=glass::Accent,edge=glass::Pick(glass::Edge,dark),white=0xffffff;
        const float edge_a=dark?glass::EdgeAlphaDark:glass::EdgeAlphaLight,card_a=dark?glass::CardAlphaDark:glass::CardAlphaLight,hover_a=dark?glass::CardHoverAlphaDark:glass::CardHoverAlphaLight;
        auto* f11=Weighted(11,DWRITE_FONT_WEIGHT_NORMAL);auto* f11s=Weighted(11,DWRITE_FONT_WEIGHT_SEMI_BOLD);auto* f12=Weighted(12,DWRITE_FONT_WEIGHT_NORMAL);auto* f13s=Weighted(13,DWRITE_FONT_WEIGHT_SEMI_BOLD);
        const auto kind=FlavorFor(e);const bool active=e.id==selected,hot=e.id==hover_card;
        // .item.new: from translateY(-12px) scale(.96) opacity 0, spring easing.
        ComPtr<ID2D1Layer> arriving;D2D1_MATRIX_3X2_F arrive_outer{};bool arrive=false;
        if(new_id==e.id)if(const ULONGLONG age=GetTickCount64()-new_tick;age<NewMs){
            const float k=SpringEase(static_cast<float>(age)/NewMs),sc=.96f+.04f*k;arrive=true;target->GetTransform(&arrive_outer);
            target->SetTransform(D2D1::Matrix3x2F::Scale(sc,sc,{(r.left+r.right)/2,(r.top+r.bottom)/2})*D2D1::Matrix3x2F::Translation(0,-12.f*(1.f-k))*(*D2D1::Matrix3x2F::ReinterpretBaseType(&arrive_outer)));
            if(SUCCEEDED(target->CreateLayer(&arriving)))target->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(),nullptr,D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,D2D1::IdentityMatrix(),std::clamp(k,0.f,1.f)),arriving.Get());
        }
        ComPtr<ID2D1Layer> lifted;if(drag_id&&e.id==drag_id&&SUCCEEDED(target->CreateLayer(&lifted)))target->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(),nullptr,D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,D2D1::IdentityMatrix(),.25f),lifted.Get());  // .lifted
        const auto& fx=FxFor(e.id,hot,active,active||hot);   // .item transition: background .12s, border-color .12s
        Fill(r,white,card_a+(hover_a-card_a)*Ease(fx.hover),cards::Radius);
        const bool marked=msel.size()>1&&msel.count(e.id)>0;   // .item.msel: accent-soft fill, accent border
        if(marked){Fill(r,accent,dark?.24f:.14f,cards::Radius);Stroke(r,accent,std::max(.45f,Ease(fx.active)),cards::Radius);}
        else if(fx.active>0)Stroke(r,accent,Ease(fx.active),cards::Radius);
        PushHit(r,30,e.id);
        // .idx: Alt+1–9 slot.
        {const D2D1_RECT_F ir{r.left+cards::IndexX,r.top+cards::IndexY,r.left+cards::IndexX+cards::IndexSize,r.top+cards::IndexY+cards::IndexSize};
            if(index<9||marked){Fill(ir,accent,1,cards::IndexRadius);if(index<9)Label(std::to_wstring(index+1),ir,Weighted(10,DWRITE_FONT_WEIGHT_BOLD),white,1.f,DWRITE_TEXT_ALIGNMENT_CENTER);}
            else Stroke(ir,edge,edge_a*1.5f,cards::IndexRadius);}
        const float x0=r.left+cards::PadLeft,x1=r.right-cards::PadRight,y0=r.top+cards::PadTop;
        const float body_h=(r.bottom-r.top)-cards::PadTop-cards::MetaGap-cards::MetaLine-cards::FootGap-cards::MetaLine-cards::PadBottom;
        const D2D1_RECT_F body{x0,y0,x1,y0+body_h};
        std::wstring detail;wchar_t glyph=L'\xE8D2';std::wstring kind_label=clipboard::FlavorName(kind);bool language_badge=false;std::wstring language;
        switch(kind){
        case clipboard::Flavor::Image:{
            DrawThumb(e,body,edge,edge_a);glyph=L'\xE8B9';const auto size=NaturalImageSize(e);
            detail=size.first>0?std::to_wstring(static_cast<int>(size.first))+L" × "+std::to_wstring(static_cast<int>(size.second)):std::to_wstring((e.Bytes()+1023)/1024)+L" KB";
            break;}
        case clipboard::Flavor::Code:{
            DrawCode(e,body,ink);language=ShortLanguage(SnippetFor(e).highlight.language);language_badge=true;
            detail=std::to_wstring(cards::CodeLines(e.text.substr(0,1<<16)))+L" 行";break;}
        case clipboard::Flavor::Link:{
            const std::wstring host=clipboard::LinkHost(e.text.substr(0,2048));const D2D1_RECT_F badge{x0,y0,x0+cards::BadgeSize,y0+cards::BadgeSize};
            const UINT32 base=clipboard::HostColor(host);
            ComPtr<ID2D1GradientStopCollection> stops;ComPtr<ID2D1LinearGradientBrush> gradient;
            const auto shade=[](UINT32 c,float k){return static_cast<UINT32>(std::lround(((c>>16)&255)*k))<<16|static_cast<UINT32>(std::lround(((c>>8)&255)*k))<<8|static_cast<UINT32>(std::lround((c&255)*k));};
            const D2D1_GRADIENT_STOP stop[2]={{0,D2D1::ColorF(base)},{1,D2D1::ColorF(shade(base,.78f))}};
            if(SUCCEEDED(target->CreateGradientStopCollection(stop,2,&stops))&&SUCCEEDED(target->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({badge.left,badge.top},{badge.right,badge.bottom}),stops.Get(),&gradient)))
                target->FillRoundedRectangle(D2D1::RoundedRect(badge,9,9),gradient.Get());else Fill(badge,base,1,9);
            Label(host.empty()?L"?":std::wstring(1,static_cast<wchar_t>(std::towupper(host[0]))),badge,Weighted(16,DWRITE_FONT_WEIGHT_EXTRA_BOLD),white,1.f,DWRITE_TEXT_ALIGNMENT_CENTER);
            Label(host.empty()?L"链接":host,{x0+48,y0+1,x1-4,y0+19},f13s,ink);
            Label(clipboard::Trimmed(e.text.substr(0,2048)),{x0+48,y0+20,x1-4,y0+37},f12,accent);
            glyph=L'\xE71B';detail=host;break;}
        case clipboard::Flavor::Color:{
            const uint32_t value=clipboard::ParseHexColor(e.text).value_or(0);const D2D1_RECT_F swatch{x0,y0,x0+cards::BadgeSize,y0+cards::BadgeSize};
            Fill(swatch,value,1,9);Stroke(swatch,0x000000,.12f,9);
            wchar_t hex[16]{},rgb[48]{};swprintf_s(hex,L"#%06X",value);swprintf_s(rgb,L"rgb(%u, %u, %u)",(value>>16)&255u,(value>>8)&255u,value&255u);
            Label(hex,{x0+48,y0+2,x1,y0+19},Weighted(13,DWRITE_FONT_WEIGHT_BOLD,true),ink);Label(rgb,{x0+48,y0+19,x1,y0+36},Weighted(12,DWRITE_FONT_WEIGHT_NORMAL,true),ink3);
            glyph=L'\xE790';detail=L"自动识别";break;}
        case clipboard::Flavor::Files:{
            const auto names=FileNames(e);const auto count=FileCount(e);const std::wstring first=names.empty()?std::wstring(L"文件"):names.front();
            // .fileic slot: the real shell icon (folders, executables), stacked for several files.
            {const size_t shown=std::min<size_t>(3,names.size());
                if(shown<=1)FileImage(clipboard::FirstFilePath(e.text),{x0+1,y0+1,x0+37,y0+37},ink3);
                else for(size_t k=0;k<shown;++k){const float o=static_cast<float>(k)*6;FileImage(names[k],{x0+o,y0+o,x0+26+o,y0+26+o},ink3);}}
            Label(count>1?first+L" 等 "+std::to_wstring(count)+L" 个":first,{x0+45,y0+1,x1-4,y0+19},f13s,ink);
            std::wstring path=clipboard::FirstFilePath(e.text);const auto slash=path.find_last_of(L"\\/");if(slash!=std::wstring::npos)path.resize(slash+1);
            Label(path,{x0+45,y0+20,x1-4,y0+37},f12,ink3);
            glyph=L'\xE8A5';detail=std::to_wstring(std::max<uint32_t>(1,count))+L" 个";break;}
        case clipboard::Flavor::Text:
        default:{
            int lines=1;if(auto layout=ClampLayout(CardText(e.text),body.right-body.left,Format(13),cards::TextLineHeight,cards::TextMaxLines,&lines))
                text_glyphs+=text_renderer.DrawLayout(target.Get(),writer.Get(),layout.Get(),{body.left,body.top},body,D2D1::ColorF(ink)).freetype_glyphs;
            detail=std::to_wstring(e.text.size())+L" 字";break;}
        }
        // .meta: kind (or language badge) and detail.
        float mx=x0,my=body.bottom+cards::MetaGap;
        if(language_badge){auto* f10b=Weighted(10,DWRITE_FONT_WEIGHT_EXTRA_BOLD);const float lw=MeasureWith(language,f10b)+10;const D2D1_RECT_F badge{mx,my+1.5f,mx+lw,my+13.5f};
            const UINT32 fill=language==L"JS"?0xf7df1e:language==L"TS"?0x3178c6:language==L"PY"?0x3776ab:0x8a94a3;Fill(badge,fill,1,4);Label(language,badge,f10b,language==L"JS"?0x222222:white,1.f,DWRITE_TEXT_ALIGNMENT_CENTER,true);mx+=lw+6;}
        else{Center(std::wstring(1,glyph),{mx,my,mx+12,my+cards::MetaLine},9,ink2,true);mx+=16;const float kw=MeasureWith(kind_label,f11s);Label(kind_label,{mx,my,mx+kw+1,my+cards::MetaLine},f11s,ink2);mx+=kw+6;}
        Label(detail,{mx,my,x1,my+cards::MetaLine},f11,ink3);
        // second .meta: ★, time, group.
        mx=x0;my+=cards::MetaLine+cards::FootGap;
        if(e.favorite){Label(L"★",{mx,my,mx+12,my+cards::MetaLine},f11,0xf5a524);mx+=14;}
        const std::wstring when=clipboard::cards::TimeLabel(e.time,now);const float tw=MeasureWith(when,f11);Label(when,{mx,my,mx+tw+1,my+cards::MetaLine},f11,ink3);mx+=tw+6;
        if(e.group&&tab<clipboard::History::GroupTabBase)if(const auto* g=history.FindGroup(e.group)){
            Label(L"·",{mx,my,mx+6,my+cards::MetaLine},f11,ink3);mx+=10;brush->SetColor(D2D1::ColorF(g->color));target->FillEllipse(D2D1::Ellipse({mx+3,my+cards::MetaLine/2},3.f,3.f),brush.Get());
            Label(g->name,{mx+10,my,x1,my+cards::MetaLine},f11,ink3);}
        // .acts (full.html actsFor): type actions, then group / star / delete.
        // Fades and slides 3 DIP (.12s); only a shown bar takes clicks (pointer-events).
        if(fx.acts>0){
            struct Act{wchar_t glyph;int action;bool on;};std::vector<Act> acts;acts.reserve(8);
            switch(kind){
            case clipboard::Flavor::Image:
                if(pin_request)acts.push_back({L'\xE718',36,false});
                if(image_action){acts.push_back({L'\xE8D2',37,false});acts.push_back({L'\xE70F',38,false});}
                acts.push_back({L'\xE8C8',32,false});break;
            case clipboard::Flavor::Link:acts.push_back({L'\xE8A7',39,false});acts.push_back({L'\xE8C8',32,false});break;
            case clipboard::Flavor::Files:acts.push_back({L'\xE838',42,false});acts.push_back({L'\xE8C8',43,false});break;
            case clipboard::Flavor::Color:acts.push_back({L'\xE8C8',44,false});break;
            default:acts.push_back({L'\xE8C8',32,false});acts.push_back({L'\xE77F',45,false});break;
            }
            acts.push_back({L'\xE8F4',35,e.group!=0});acts.push_back({L'\xE734',31,e.favorite});acts.push_back({L'\xE74D',34,false});
            const float n=static_cast<float>(acts.size()),w=cards::ActsPad*2+n*cards::ActButton+(n-1)*cards::ActGap;
            const D2D1_RECT_F bar{r.right-cards::ActsRight-w,r.top+cards::ActsTop,r.right-cards::ActsRight,r.top+cards::ActsTop+cards::ActsPad*2+cards::ActButton};
            const float t=Ease(fx.acts);const bool live=active||hot;
            ComPtr<ID2D1Layer> fade;if(t<.999f&&SUCCEEDED(target->CreateLayer(&fade)))target->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(),nullptr,D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,D2D1::IdentityMatrix(),t),fade.Get());
            D2D1_MATRIX_3X2_F outer{};target->GetTransform(&outer);
            if(t<.999f)target->SetTransform(D2D1::Matrix3x2F::Translation(0,-3.f*(1.f-t))*(*D2D1::Matrix3x2F::ReinterpretBaseType(&outer)));
            const UINT32 shadow=dark?0x000000:0x0a192e;
            for(int i=3;i>=1;--i){const float k=static_cast<float>(i)*1.6f;brush->SetColor(D2D1::ColorF(shadow,dark?.08f:.035f));target->FillRoundedRectangle(D2D1::RoundedRect({bar.left-k,bar.top-k+3,bar.right+k,bar.bottom+k+3},cards::ActsRadius+k,cards::ActsRadius+k),brush.Get());}
            Fill(bar,glass::Pick(glass::CardGlass,dark),dark?.96f:.97f,cards::ActsRadius);Stroke(bar,edge,edge_a,cards::ActsRadius);
            float bx=bar.left+cards::ActsPad;const float by=bar.top+cards::ActsPad;
            for(const auto& act:acts){Icon(act.glyph,bx,by,live?act.action:0,e.id,act.on,false,cards::ActButton);bx+=cards::ActButton+cards::ActGap;}
            target->SetTransform(outer);if(fade)target->PopLayer();
        }
        if(lifted)target->PopLayer();
        if(arriving)target->PopLayer();if(arrive)target->SetTransform(arrive_outer);
    }
    void Render(int requested_width=0,int requested_height=0,bool content_only=false){
        if(!enabled||!window)return;
        {BOOL animate=TRUE;SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION,0,&animate,0);fx_enabled=animate!=FALSE;}
        SyncBulk();
        if(!content_only)SelectWindow();
        std::erase_if(images,[&](const auto& item){return !ImageNeeded(item.first);});
        RECT client{};GetClientRect(window,&client);if(requested_width&&requested_height)client={0,0,requested_width,requested_height};if(!client.right||!client.bottom)return;
        if(!surface||width!=client.right||height!=client.bottom){target.Reset();width=client.right;height=client.bottom;surface=std::make_unique<DibSurface>(width,height);}
        if(!factory)CheckWin32(SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,factory.GetAddressOf())),"Clipboard renderer");
        if(!writer)CheckWin32(SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(writer.GetAddressOf()))),"Clipboard text");
        // D2D draws in place into the DIB handed to composition; a resized DIB gets a new
        // target. Cached thumbnails and file icons move over without another upload
        // (failures are re-created lazily); brush-holding icon geometry is rebuilt.
        if(!target){target=CreateMemoryRenderTarget(factory.Get(),surface->Pixels(),width,height,width);
            std::erase_if(images,[&](auto& item){return !ShareBitmap(target.Get(),item.second);});std::erase_if(file_bitmaps,[&](auto& item){return !ShareBitmap(target.Get(),item.second);});
            app_icon.Reset();brush.Reset();checker.Reset();CheckWin32(SUCCEEDED(target->CreateSolidColorBrush(D2D1::ColorF(0),&brush)),"Clipboard brush");}
        target->SetDpi(96*scale,96*scale);target->BeginDraw();target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        const UINT32 bg=dark?0x192330:0xf5f8fc,accent=0x267aff;
        if(content_only)target->Clear(D2D1::ColorF(0,0.f));
        else if(custom_outline)target->Clear(D2D1::ColorF(bg,PanelOpacity(dark,expanded&&acrylic)));
        else {target->Clear(D2D1::ColorF(0,0.f));const float radius=expanded?22.f:8.f;Color(bg);target->FillRoundedRectangle(D2D1::RoundedRect({0,0,width/scale,height/scale},radius,radius),brush.Get());}hover_card=HoverCard();hits.clear();
        if(!expanded){

            ClipboardIcon((CW-26)/2,6,26);
            Box({4,37,CW-4,57},dark?0x2a4870:0xe1edff,dark?0x2a4870:0xe1edff,10);
            Center(std::to_wstring(history.entries.size()),{2,37,CW-2,57},history.entries.size()>=100?9:10,accent);
            Color(dark?0x9aabc3:0x7c91ae);target->DrawLine({12,73},{16,77},brush.Get(),1.4f);target->DrawLine({16,77},{12,81},brush.Get(),1.4f);

        }else{
            DrawExpanded(bg);
        }
        if(menu_open&&expanded)DrawMore();if(popup_open&&expanded)DrawPopup();
        PaintSearch();
        const auto hr=target->EndDraw();if(hr==D2DERR_RECREATE_TARGET){CancelReads();images.clear();file_bitmaps.clear();app_icon.Reset();brush.Reset();checker.Reset();target.Reset();Invalidate();}else {CheckWin32(SUCCEEDED(hr),"Render clipboard");}
    }
    void FinishCopy(const clipboard::Entry& entry,bool paste,HWND destination,DWORD process){
        if(test_mode){last_read_id=entry.id;status=L"测试模式：未改动系统剪贴板";return;}
        if(copy_quick&&!QuickDestinationValid()){CloseQuick();return;}
        const HWND foreground=GetForegroundWindow();
        if(foreground!=window&&!IsChild(window,foreground)&&!((pinned||copy_quick)&&foreground==destination)&&!(copy_stack&&(foreground==destination||!paste))){status=L"已取消：输入焦点已改变";Invalidate();return;}
        DWORD current_process{};if(paste&&(!IsWindow(destination)||destination==window||!GetWindowThreadProcessId(destination,&current_process)||current_process!=process)){status=L"原输入窗口已失效，请重新选择目标窗口";Invalidate();return;}
        if(!clipboard::Write(window,entry,copy_plain,copy_remove_whitespace)){status=L"剪贴板正忙或内容不可用，请重试";Invalidate();return;}
        sequence=GetClipboardSequenceNumber();status=L"已复制";Invalidate();
        if(paste){ClosePreview();previous=destination;paste_target=destination;paste_focus=copy_focus;paste_process=process;paste_sequence=sequence;paste_wait=0;paste_keyboard=copy_keyboard;paste_quick=copy_quick;paste_stack=copy_stack;paste_activated=paste_returning=false;paste_inflight=true;SetTimer(EventWindow(),2,40,nullptr);}
    }
    void StopPaste(const wchar_t* message){KillTimer(EventWindow(),2);paste_inflight=paste_returning=paste_quick=paste_stack=false;status=message;UpdateQuick();Invalidate();}
    static bool PasteKeysHeld(){
        // Keep the triggering Enter (including its key-up/repeat) in our window.
        for(int key:{VK_RETURN,VK_LBUTTON,VK_RBUTTON,VK_CONTROL,VK_SHIFT,VK_MENU,VK_LWIN,VK_RWIN})if(GetAsyncKeyState(key)&0x8000)return true;
        return false;
    }
    bool RestoreInputFocus(){
        if(!paste_focus)return true; // Custom controls can expose only a top-level window.
        if(!IsWindow(paste_focus)||(paste_focus!=paste_target&&!IsChild(paste_target,paste_focus)))return false;
        const DWORD thread=GetWindowThreadProcessId(paste_target,nullptr);GUITHREADINFO info{sizeof(info)};
        if(GetGUIThreadInfo(thread,&info)&&info.hwndFocus==paste_focus)return true;
        const DWORD current=GetCurrentThreadId();const bool attach=thread!=current;
        if(attach&&!AttachThreadInput(current,thread,TRUE))return false;
        SetFocus(paste_focus);
        const bool restored=GetGUIThreadInfo(thread,&info)&&info.hwndFocus==paste_focus;
        if(attach)AttachThreadInput(current,thread,FALSE);
        return restored;
    }
    void PasteTick(){
        if(!paste_inflight)return;
        DWORD process{};
        if(++paste_wait>125||!IsWindow(paste_target)||
            !GetWindowThreadProcessId(paste_target,&process)||process!=paste_process||GetClipboardSequenceNumber()!=paste_sequence){
            StopPaste(L"自动粘贴已取消：目标窗口或剪贴板已变化");return;
        }
        const HWND foreground=GetForegroundWindow();
        if(paste_quick){
            if(!QuickDestinationValid()){CloseQuick();return;}
            // The quick picker never activates or restores focus: the original
            // application and its input control must still own it throughout.
            paste_activated=true;
        }
        // Stack pastes never activate anything either: the target kept focus.
        if(paste_stack)paste_activated=true;
        if(paste_returning){
            // SendInput queues input; leave time for the target to consume it.
            // Never resend or claim that the receiving application accepted it.
            if(foreground!=paste_target){StopPaste(L"已发送粘贴请求，输入窗口已切换");return;}
            if(GetTickCount64()<paste_return_at||PasteKeysHeld())return;
            StopPaste(L"已发送粘贴 · ↑↓ 继续选择 · Enter 插入");
            if(expanded&&pinned){
                if(SetForegroundWindow(window))SetFocus(window);
                else {status=L"已发送粘贴 · 按剪贴板快捷键继续选择";Invalidate();}
            }
            return;
        }
        if(!paste_activated){
            if(foreground!=paste_target&&foreground!=window&&!IsChild(window,foreground)){StopPaste(L"已取消：输入窗口已切换");return;}
            if(PasteKeysHeld())return;
            if(!pinned){Fold();paste_inflight=true;SetTimer(EventWindow(),2,40,nullptr);}
            if(foreground!=paste_target&&!SetForegroundWindow(paste_target)){StopPaste(L"无法返回输入窗口，内容已复制，请手动粘贴");return;}
            paste_activated=true;
            if(GetForegroundWindow()!=paste_target)return;
        }
        const HWND current_foreground=GetForegroundWindow();
        if(current_foreground!=paste_target){
            if(current_foreground==window||IsChild(window,current_foreground))return;
            StopPaste(L"已取消：输入窗口已切换");return;
        }
        if(PasteKeysHeld())return;
        if(!paste_quick&&!paste_stack&&!RestoreInputFocus()){StopPaste(L"无法恢复原输入位置，内容已复制，请手动粘贴");return;}
        INPUT input[4]{};for(auto& i:input)i.type=INPUT_KEYBOARD;
        input[0].ki.wVk=VK_CONTROL;input[1].ki.wVk='V';input[2].ki.wVk='V';input[2].ki.dwFlags=KEYEVENTF_KEYUP;input[3].ki.wVk=VK_CONTROL;input[3].ki.dwFlags=KEYEVENTF_KEYUP;
        if(SendInput(4,input,sizeof(INPUT))!=4){StopPaste(L"目标不允许自动粘贴，请手动 Ctrl+V");return;}
        if(paste_quick){StopPaste(L"");copy_quick=false;if(!quick_continuous)CloseQuick();}
        else if(pinned&&paste_keyboard){paste_returning=true;paste_return_at=GetTickCount64()+200;status=L"已发送粘贴请求…";Invalidate();}
        else StopPaste(pinned?L"已发送粘贴，可继续点击下一条":L"已发送粘贴请求");
    }
    static float Ease(float t){return t*t*(3.f-2.f*t);}   // close to CSS `ease` for 120 ms
    // full.html --ease-spring: cubic-bezier(.25,1.2,.4,1), slight overshoot.
    static float SpringEase(float t){
        const auto bezier=[](float a,float b,float u){const float v=1-u;return 3*v*v*u*a+3*v*u*u*b+u*u*u;};
        float lo=0,hi=1,u=t;for(int i=0;i<24;++i){u=(lo+hi)/2;if(bezier(.25f,.4f,u)<t)lo=u;else hi=u;}
        return bezier(1.2f,1.f,u);
    }
    static constexpr ULONGLONG NewMs=450;
    void MarkNew(uint64_t id){if(!expanded||!fx_enabled||test_mode)return;new_id=id;new_tick=GetTickCount64();StartFx();}
    CardFx& FxFor(uint64_t id,bool hot,bool active,bool acts){
        auto& fx=card_fx[id];fx.hover_to=hot?1.f:0.f;fx.active_to=active?1.f:0.f;fx.acts_to=acts?1.f:0.f;
        if(!fx_enabled||test_mode){fx.hover=fx.hover_to;fx.active=fx.active_to;fx.acts=fx.acts_to;}
        else if(fx.hover!=fx.hover_to||fx.active!=fx.active_to||fx.acts!=fx.acts_to)StartFx();
        return fx;
    }
    void StartFx(){if(fx_running||!window)return;fx_running=true;fx_tick=GetTickCount64();SetTimer(EventWindow(),13,USER_TIMER_MINIMUM,nullptr);}
    void StopFx(){card_fx.clear();new_id=0;if(fx_running&&window)KillTimer(EventWindow(),13);fx_running=false;}
    void FxTick(){
        const ULONGLONG tick=GetTickCount64();const float step=std::min(1.f,static_cast<float>(tick-fx_tick)/120.f);fx_tick=tick;bool moving=false;
        const auto toward=[&](float& v,float to){v=v<to?std::min(to,v+step):std::max(to,v-step);moving|=v!=to;};
        for(auto it=card_fx.begin();it!=card_fx.end();){auto& fx=it->second;toward(fx.hover,fx.hover_to);toward(fx.active,fx.active_to);toward(fx.acts,fx.acts_to);
            if(fx.hover==0&&fx.active==0&&fx.acts==0&&fx.hover_to==0&&fx.active_to==0&&fx.acts_to==0)it=card_fx.erase(it);else ++it;}
        if(new_id){if(tick-new_tick>=NewMs)new_id=0;else moving=true;}
        Invalidate();if(!moving){fx_running=false;if(window)KillTimer(EventWindow(),13);}
    }
    void SyncPreviewDetails(){
        if(!preview.IsOpen())return;const auto* e=history.Find(preview.CurrentId());if(!e||e->kind!=clipboard::Kind::Image)return;
        const auto size=NaturalImageSize(*e);const auto text=ocr_texts.find(e->id);
        preview.SetImageDetails(e->id,{static_cast<LONG>(size.first),static_cast<LONG>(size.second)},text!=ocr_texts.end()?text->second:std::wstring{},static_cast<bool>(image_action),ocr_pending_id==e->id);
    }
    void ImageOcrResult(std::wstring text){
        const uint64_t id=std::exchange(ocr_pending_id,uint64_t{});
        if(id&&!text.empty()){ocr_texts[id]=std::move(text);while(ocr_texts.size()>32)ocr_texts.erase(ocr_texts.begin());}
        status=id&&ocr_texts.count(id)?L"已识别文字并复制":L"未识别到文字";SyncPreviewDetails();Invalidate();
    }
    void SyncBulk(){const float want=clipboard::cards::ListTop+(msel.size()>1?BulkHeight+BulkGap:0.f);if(want!=ListTop){ListTop=want;BuildBlocks();ClampScroll();}}
    std::vector<uint64_t> Marked()const{std::vector<uint64_t> out;for(const auto id:visible)if(msel.count(id))out.push_back(id);return out;}   // list order
    void SelectRow(uint64_t id,bool ctrl,bool shift){
        const auto it=std::find(visible.begin(),visible.end(),id);if(it==visible.end())return;const size_t i=static_cast<size_t>(it-visible.begin());
        const auto current=std::find(visible.begin(),visible.end(),selected);
        if(ctrl){
            if(msel.empty()&&current!=visible.end()&&*current!=id)msel.insert(*current);   // like Explorer, the current card joins
            if(!msel.erase(id))msel.insert(id);last_click=i;
        }else if(shift){
            size_t pivot=last_click.value_or(current!=visible.end()?static_cast<size_t>(current-visible.begin()):i);pivot=std::min(pivot,visible.size()-1);
            for(size_t k=std::min(pivot,i);k<=std::max(pivot,i);++k)msel.insert(visible[k]);
        }
        selected=id;SyncBulk();if(preview.IsOpen())ShowPreview();Invalidate();
    }
    static std::vector<unsigned char> HdropBytes(const std::vector<std::filesystem::path>& paths){
        std::wstring list;for(const auto& path:paths){list+=path.wstring();list.push_back(L'\0');}list.push_back(L'\0');
        std::vector<unsigned char> bytes(sizeof(DROPFILES)+list.size()*sizeof(wchar_t));DROPFILES header{};header.pFiles=sizeof(DROPFILES);header.fWide=TRUE;
        std::memcpy(bytes.data(),&header,sizeof(header));std::memcpy(bytes.data()+sizeof(header),list.data(),list.size()*sizeof(wchar_t));return bytes;
    }
    std::vector<std::filesystem::path> PathsOf(uint64_t id){
        const auto* e=history.Find(id);if(!e)return {};std::vector<std::filesystem::path> paths;
        if(auto loaded=LoadForDrag(*e))paths=clipboard::FilePaths(*loaded);
        if(paths.empty())if(const auto* again=history.Find(id)){size_t start=0;const std::wstring& t=again->text;   // older entries may only carry the path text
            while(start<t.size()){size_t end=t.find_first_of(L"\r\n",start);if(end==std::wstring::npos)end=t.size();
                if(const auto line=clipboard::Trimmed(t.substr(start,end-start));!line.empty())paths.emplace_back(line);start=end+1;}}
        return paths;
    }
    // Multi-drag: texts merge, files merge into one multi-file CF_HDROP; images only travel alone.
    std::optional<clipboard::Entry> MergedSelection(uint64_t pressed){
        const auto ids=Marked();bool files=false,texts=false;
        for(const auto id:ids)if(const auto* e=history.Find(id)){if(e->kind==clipboard::Kind::Files)files=true;else if(e->kind==clipboard::Kind::Text)texts=true;}
        if(!files&&!texts)return std::nullopt;
        clipboard::Entry merged;merged.id=pressed;GetLocalTime(&merged.time);
        if(files&&!texts){
            std::vector<std::filesystem::path> paths;
            for(const auto id:ids){const auto* e=history.Find(id);if(!e||e->kind!=clipboard::Kind::Files)continue;for(auto& path:PathsOf(id))if(std::find(paths.begin(),paths.end(),path)==paths.end())paths.push_back(std::move(path));}
            if(paths.empty())return std::nullopt;
            merged.kind=clipboard::Kind::Files;for(const auto& path:paths){if(!merged.text.empty())merged.text+=L"\r\n";merged.text+=path.wstring();}
            merged.formats.push_back({CF_HDROP,HdropBytes(paths)});return merged;
        }
        merged.kind=clipboard::Kind::Text;merged.text=MergedText(ids);if(merged.text.empty())return std::nullopt;
        const auto* b=reinterpret_cast<const unsigned char*>(merged.text.c_str());merged.formats.push_back({CF_UNICODETEXT,{b,b+(merged.text.size()+1)*sizeof(wchar_t)}});
        return merged;
    }
    std::wstring MergedText(const std::vector<uint64_t>& ids){
        std::wstring text;for(const auto id:ids){const auto* e=history.Find(id);if(!e||e->kind==clipboard::Kind::Image)continue;if(!text.empty())text+=L"\r\n";text+=e->text;}return text;
    }
    void CommitPendingDelete(){
        if(pending_delete.empty())return;if(window)KillTimer(EventWindow(),14);
        CancelReads();for(const auto id:pending_delete){history.Erase(id);images.erase(id);}pending_delete.clear();
        if(disk)disk->Commit(history.entries,history.groups);ClearFileIcons();Filter();Invalidate();
    }
    void UndoDelete(){
        if(pending_delete.empty())return;if(window)KillTimer(EventWindow(),14);
        const size_t n=pending_delete.size();pending_delete.clear();Filter();status=L"已恢复 "+std::to_wstring(n)+L" 项";Invalidate();
    }
    // #bulk: 合并复制 50 · 收藏 51 · 删除 52 · ✕ 53; 撤销 54 (footer).
    void BulkAction(int action){
        if(action==54){UndoDelete();return;}
        const auto ids=Marked();
        if(action==53||ids.empty()){msel.clear();last_click.reset();SyncBulk();Invalidate();return;}
        const std::wstring n=std::to_wstring(ids.size());
        if(action==50){
            const auto text=MergedText(ids);
            if(text.empty()){status=L"图片不能合并复制";Invalidate();return;}
            CopyText(text,(L"已合并复制 "+n+L" 项").c_str(),true);   // the merged text joins the history, as in full.html
        }else if(action==51){
            size_t done=0,blocked=0;
            for(const auto id:ids){auto* e=history.Find(id);if(!e||e->favorite)continue;if(!history.CanProtect(*e)){++blocked;continue;}e->favorite=true;++done;}
            if(disk)disk->Commit(history.entries,history.groups);
            status=blocked?L"收藏和分组最多 200 条，已收藏 "+std::to_wstring(done)+L" 项":L"已收藏 "+n+L" 项";
        }else if(action==52){
            CommitPendingDelete();
            if(std::find(ids.begin(),ids.end(),selected)!=ids.end()){
                const auto it=std::find(visible.begin(),visible.end(),selected);uint64_t next=0;
                for(auto k=it;k!=visible.end();++k)if(!msel.count(*k)){next=*k;break;}
                if(!next)for(auto k=visible.begin();k!=it;++k)if(!msel.count(*k))next=*k;
                selected=next;
            }
            pending_delete=ids;status=L"已删除 "+n+L" 项";
            if(window&&!test_mode)SetTimer(EventWindow(),14,6000,nullptr);
        }
        msel.clear();last_click.reset();Filter(action!=50);SyncBulk();Invalidate();
    }
    // Copies derived text (paths, HEX) without recording it again as a new history entry.
    void CopyText(const std::wstring& text,const wchar_t* done,bool record=false){
        if(text.empty())return;
        if(test_mode){last_copied_text=text;status=done;Invalidate();return;}
        clipboard::Entry temp;temp.kind=clipboard::Kind::Text;temp.text=text;const auto* b=reinterpret_cast<const unsigned char*>(text.c_str());
        temp.formats.push_back({CF_UNICODETEXT,{b,b+(text.size()+1)*sizeof(wchar_t)}});
        if(!clipboard::Write(window,temp,false,false)){status=L"剪贴板正忙或内容不可用，请重试";Invalidate();return;}
        if(!record)sequence=GetClipboardSequenceNumber();
        status=done;Invalidate();
    }
    // New pins open beside the panel (left, else right), inside the work area.
    POINT PinPointFor(const Frame& frame){
        RECT panel_rect{};GetWindowRect(window,&panel_rect);MONITORINFO info{};info.cbSize=sizeof(info);GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&info);
        const LONG w=static_cast<LONG>(frame.Width()),h=static_cast<LONG>(frame.Height());const RECT work=info.rcWork;
        LONG x=panel_rect.left-w-16;if(x<work.left)x=panel_rect.right+16;if(x+w>work.right)x=std::max(work.left,work.right-w);
        const LONG y=std::clamp<LONG>(panel_rect.top+static_cast<LONG>(ListTop*scale),work.top,std::max(work.top,work.bottom-h));
        return {x,y};
    }
    // full.html actsFor(): type-specific card actions.
    void CardAction(int action,uint64_t id){
        const auto* e=history.Find(id);if(!e)return;selected=id;
        if(action==45){Copy(id,true,true);return;}
        if(action==44){if(const auto rgb=clipboard::ParseHexColor(e->text)){wchar_t hex[8]{};swprintf_s(hex,L"#%06X",*rgb&0xffffffu);CopyText(hex,L"已复制 HEX");}return;}
        if(action==39){
            std::wstring url=clipboard::Trimmed(e->text);if(!clipboard::LooksLikeLink(url)){status=L"不是有效的链接";Invalidate();return;}
            if(url.rfind(L"http://",0)!=0&&url.rfind(L"https://",0)!=0)url=L"https://"+url;
            if(test_mode)last_shell_target=url;else ShellExecuteW(nullptr,L"open",url.c_str(),nullptr,nullptr,SW_SHOWNORMAL);
            status=L"已在默认浏览器中打开";Invalidate();return;}
        auto loaded=LoadForDrag(*e);if(!loaded){status=L"读取记录失败，请重试";Invalidate();return;}
        if(action==42||action==43){
            auto paths=clipboard::FilePaths(*loaded);
            if(paths.empty()){size_t start=0;const std::wstring& t=loaded->text;   // older entries may only carry the path text
                while(start<t.size()){size_t end=t.find_first_of(L"\r\n",start);if(end==std::wstring::npos)end=t.size();
                    if(const auto line=clipboard::Trimmed(t.substr(start,end-start));!line.empty())paths.emplace_back(line);start=end+1;}}
            if(paths.empty()){status=L"文件已不存在";Invalidate();return;}
            if(action==43){std::wstring text;for(const auto& path:paths){if(!text.empty())text+=L"\r\n";text+=path.wstring();}CopyText(text,L"已复制路径");return;}
            std::error_code ec;if(!std::filesystem::exists(paths.front(),ec)){status=L"文件已不存在";Invalidate();return;}
            if(test_mode)last_shell_target=paths.front().wstring();
            else if(PIDLIST_ABSOLUTE item=ILCreateFromPathW(paths.front().c_str())){SHOpenFolderAndSelectItems(item,0,nullptr,0);ILFree(item);}
            status=L"已在资源管理器中定位";Invalidate();return;}
        if(action==36||action==37||action==38){
            const auto frame=clipboard::DecodeFrame(*loaded);if(!frame){status=L"图片无法读取";Invalidate();return;}
            const POINT at=PinPointFor(*frame);   // top-left; the pin handler takes the pin centre
            if(action==36){status=pin_request&&pin_request(frame,{at.x+frame->Width()/2,at.y+frame->Height()/2})?L"已钉到桌面":L"钉图失败";Invalidate();return;}
            const auto kind=action==37?ClipboardImageAction::Ocr:ClipboardImageAction::Annotate;
            const bool ok=image_action&&image_action(kind,frame,at);
            if(ok&&action==37){ocr_pending_id=e->id;SyncPreviewDetails();}
            status=ok?(action==37?L"正在识别文字…":L"已打开标注"):(action==37?L"文字识别当前不可用":L"正在截图或标注，请稍后再试");
            if(ok&&action==38&&!test_mode){Fold();return;}   // the edit session covers the screen
            Invalidate();return;}
    }
    void Copy(uint64_t id,bool paste,bool plain=false,bool keyboard=false,bool remove_whitespace=false){
        const auto* e=history.Find(id);if(!e||copy_pending||paste_inflight)return;
        if(plain&&e->kind!=clipboard::Kind::Text){status=L"纯文本粘贴仅支持文本记录";Invalidate();return;}
        if(pinned)RememberTarget(GetForegroundWindow());
        copy_target=previous;copy_process=previous_process;copy_focus=previous_focus;copy_remove_whitespace=remove_whitespace;copy_plain=plain;copy_keyboard=keyboard;copy_quick=false;copy_stack=false;
        if(e->payload&&disk){copy_paste=paste;copy_pending=true;const auto token=++copy_token;
            if(!disk->Load(*e,token)){copy_pending=false;status=L"读取队列已满，请重试";}else status=paste?L"正在准备粘贴…（Esc 取消）":L"正在准备复制…（Esc 取消）";Invalidate();return;}
        FinishCopy(*e,paste,copy_target,copy_process);
    }
    void Action(int action,uint64_t id,bool keep_focus=false){
        if(action>=20&&action<25){SelectTab(action-20);return;}
        if(action==26&&id>=1&&id<=6){SelectFlavor(static_cast<clipboard::Flavor>(id-1));return;}
        if(action==15){SetPaused(!paused);return;}
        if(action==40){const int index=history.GroupIndex(static_cast<uint32_t>(id));if(index>=0)SelectTab(clipboard::History::GroupTabBase+index);return;}
        if(action==41){BeginNaming(1,0,0);return;}
        if((action>=36&&action<=39)||(action>=42&&action<=45)){CardAction(action,id);return;}
        if(action>=50&&action<=54){BulkAction(action);return;}
        if(action==30){const WPARAM mods=std::exchange(click_mods,WPARAM{});
            if(mods&(MK_CONTROL|MK_SHIFT)){SelectRow(id,(mods&MK_CONTROL)!=0,(mods&MK_SHIFT)!=0);return;}
            if(!msel.empty()){msel.clear();SyncBulk();}
            if(const auto it=std::find(visible.begin(),visible.end(),id);it!=visible.end())last_click=static_cast<size_t>(it-visible.begin());}
        if(action==35){OpenEntryPopupAtButton(id,false);return;}
        if(action==10){RememberTarget(GetForegroundWindow());pinned=!pinned;if(pinned&&summon_cursor){const RECT body=VisualBodyBounds();anchor={body.right,body.top};summon_cursor.reset();}UpdateActivationPolicy();
            DWORD process{};if(pinned&&!keep_focus&&IsWindow(previous)&&GetWindowThreadProcessId(previous,&process)&&process==previous_process)SetForegroundWindow(previous);
            status=pinned?L"已固定：↑↓ 选择，Enter 连续粘贴":L"已取消固定：失焦时折叠";
            // Unpinning resumes normal focus handling; fold on the next deactivation.
            if(!pinned&&expanded){SetForegroundWindow(window);if(GetFocus()!=search)SetFocus(window);}
            Invalidate();}
        if(action==11){Fold();if(settings)settings();}
        if(action==12){if(menu_open)CloseMore();else OpenMore();}
        if(action==13){oldest=!oldest;Filter();RevealSelection();}if(action==14)Fold();
        if(action==30&&pinned&&!preview.IsOpen()){if(copy_pending||paste_inflight){status=L"上一条正在插入，请稍候";Invalidate();return;}selected=id;Copy(id,true);Invalidate();return;}
        if(action==30){if(thumbnail_failed.erase(id))images.erase(id);++copy_token;copy_pending=false;selected=id;if(preview.IsOpen())ShowPreview();else PrefetchPreview();Invalidate();}if(action==31){auto* e=history.Find(id);if(e&&!e->favorite&&!history.CanProtect(*e)){selected=id;status=L"收藏和分组最多 200 条，请先整理";Invalidate();return;}if(e){e->favorite=!e->favorite;if(disk)disk->Commit(history.entries,history.groups);selected=id;status=e->favorite?L"已收藏，清空历史时保留":L"已取消收藏";}Filter();RevealSelection();}
        if(action==32||action==33)Copy(id,action==33);
        if(action==34){
            const uint64_t viewport_top=scroll>=0&&scroll<static_cast<int>(visible.size())?visible[static_cast<size_t>(scroll)]:0;
            const int previous_scroll=scroll;
            const auto it=std::find(visible.begin(),visible.end(),id);
            if(id==selected&&it!=visible.end())selected=std::next(it)!=visible.end()?*std::next(it):it!=visible.begin()?*std::prev(it):0;
            CancelReads();history.Erase(id);if(disk)disk->Commit(history.entries,history.groups);images.erase(id);ClearFileIcons();Filter();
            // Deletion preserves the browsing viewport, even when the selected
            // row was scrolled offscreen. Only keyboard navigation reveals it.
            const auto top=std::find(visible.begin(),visible.end(),viewport_top);
            if(top!=visible.end())ScrollToIndex(static_cast<int>(top-visible.begin()),Offset());else ScrollToIndex(previous_scroll);
            Invalidate();
        }
    }
    void RevealSelection(){
        const auto it=std::find(visible.begin(),visible.end(),selected);if(it==visible.end())return;
        const int index=static_cast<int>(it-visible.begin());RevealIndex(index);Invalidate();
    }
    void TogglePreview(){
        if(preview.IsOpen()){preview.Hide();return;}
        ShowPreview();
    }
    bool RequestPreview(const clipboard::Entry& entry){
        if(!disk||!entry.payload)return false;
        if(preview_pending==entry.id)return true;
        ++preview_token;preview_pending=0;
        if(!disk->LoadPreview(entry,preview_token))return false;
        preview_pending=entry.id;return true;
    }
    void PrefetchPreview(){
        if(!expanded||!disk)return;
        const auto* entry=history.Find(selected);
        if(!entry)return;
        // Selection takes priority over speculative neighbors. Replacing the
        // request drops queued stale work; only bounded pixels return to UI.
        if(entry->kind==clipboard::Kind::Image&&!preview_cache.Find(selected)){
            RequestPreview(*entry);return;
        }
        if(preview_pending)return;
        const auto it=std::find(visible.begin(),visible.end(),selected);
        if(it==visible.end())return;
        const auto index=it-visible.begin();
        for(const auto offset:{1,-1}){
            const auto next=index+offset;
            if(next<0||next>=static_cast<decltype(index)>(visible.size()))continue;
            const auto* neighbor=history.Find(visible[static_cast<size_t>(next)]);
            if(neighbor&&neighbor->kind==clipboard::Kind::Image&&!preview_cache.Find(neighbor->id)){
                RequestPreview(*neighbor);return;
            }
        }
    }
    void ShowPreview(){
        auto* entry=history.Find(selected);
        if(!expanded||!entry){ClosePreview();return;}
        if(preview.IsOpen()&&preview.CurrentId()==selected)return;
        RECT rect{};GetWindowRect(window,&rect);
        clipboard::PreviewData data;data.image=preview_cache.Find(selected);
        const bool cached=data.image!=nullptr;
        if(!cached){data.image=thumbnail_cache.Find(selected);data.interim=data.image!=nullptr;}
        preview.SetCopyAction([this,id=entry->id]{Copy(id,false);});
        preview.SetOcrAction([this]{if(const uint64_t id=preview.CurrentId())CardAction(37,id);});
        preview.Show(window,*entry,rect,dark,scale,std::move(data));SyncPreviewDetails();
        if(cached){PrefetchPreview();return;}
        if(entry->payload&&disk){
            if(!RequestPreview(*entry)){
                clipboard::PreviewData error;error.error=L"预览读取队列已满，请关闭后重试";
                preview.SetContent(selected,std::move(error));
            }
        }else{
            preview.SetContent(selected,clipboard::PreparePreview(*entry));
        }
    }
    void Key(WPARAM key){
        if(key==VK_F2){
            const HWND foreground=GetForegroundWindow();
            if(!expanded||(foreground!=window&&!IsChild(window,foreground))||
                ((GetKeyState(VK_CONTROL)|GetKeyState(VK_SHIFT)|GetKeyState(VK_MENU)|GetKeyState(VK_LWIN)|GetKeyState(VK_RWIN))&0x8000))return;
            CloseMore();Action(10,0,true);status=pinned?L"已固定 · F2 取消固定":L"已取消固定 · F2 固定面板";
            SetTimer(EventWindow(),6,2000,nullptr);Invalidate();return;
        }
        if(menu_open){MenuKey(key);return;}
        if(popup_open&&PopupKey(key))return;
        if(naming){if(key==VK_ESCAPE)CancelNaming();else if(key==VK_RETURN)CommitNaming();return;}
        if(key==VK_ESCAPE&&!msel.empty()){BulkAction(53);return;}
        if(key==VK_ESCAPE){if(preview.IsOpen()){preview.Hide();return;}const HWND destination=previous;Fold();if(!test_mode&&IsWindow(destination))SetForegroundWindow(destination);return;}
        const bool ctrl=(GetKeyState(VK_CONTROL)&0x8000)!=0;
        if(key=='O'&&!ctrl&&preview.IsOpen()&&image_action)if(const auto* e=history.Find(preview.CurrentId());e&&e->kind==clipboard::Kind::Image){swallow_char=true;CardAction(37,e->id);return;}
        if(key==VK_TAB){SetFocus(search);return;}
        if(ctrl&&key=='F'){SetFocus(search);SendMessageW(search,EM_SETSEL,0,-1);return;}
        if(ctrl&&key=='D'){Action(31,selected);return;}
        if(ctrl&&(GetKeyState(VK_SHIFT)&0x8000)&&key=='N'){BeginNaming(1,0,history.Find(selected)?selected:0);return;}
        if(ctrl&&key>='0'&&key<='9'){
            if(!history.Find(selected)){status=L"请先选择一条记录";Invalidate();return;}
            if(key=='0'){AssignGroup(selected,0);return;}
            const size_t index=static_cast<size_t>(key-'1');
            if(index>=history.groups.size()){status=history.groups.empty()?L"还没有分组 · Ctrl+Shift+N 新建并移入":L"没有第 "+std::to_wstring(index+1)+L" 个分组";Invalidate();return;}
            AssignGroup(selected,history.groups[index].id);return;
        }
        if(ctrl&&key=='G'){if(history.Find(selected))OpenEntryPopupAtButton(selected,true);else{status=L"请先选择一条记录";Invalidate();}return;}
        if(ctrl&&key=='A'&&visible.size()>1){msel.insert(visible.begin(),visible.end());SyncBulk();Invalidate();return;}
        if(ctrl&&key=='Z'&&!pending_delete.empty()){UndoDelete();return;}
        if(msel.size()>1&&((ctrl&&key=='C')||key==VK_DELETE)){BulkAction(key==VK_DELETE?52:50);return;}
        if(ctrl&&key=='C'){Copy(selected,false);return;}if(key==VK_RETURN){Copy(selected,true,(GetKeyState(VK_SHIFT)&0x8000)!=0,true,ctrl&&(GetKeyState(VK_SHIFT)&0x8000)!=0);return;}
        if(key==VK_DELETE){Action(34,selected);return;}
        if(key==VK_SPACE){SetFocus(window);TogglePreview();return;}
        if(key==VK_LEFT||key==VK_RIGHT){const int n=TabCount();SelectTab((tab+(key==VK_RIGHT?1:n-1))%n);return;}
        if(key==VK_HOME||key==VK_END||key==VK_PRIOR||key==VK_NEXT){
            if(visible.empty())return;
            const auto it=std::find(visible.begin(),visible.end(),selected);
            const int index=it==visible.end()?0:static_cast<int>(it-visible.begin());
            const int next=key==VK_HOME?0:key==VK_END?static_cast<int>(visible.size())-1:std::clamp(index+(key==VK_NEXT?PageRows():-PageRows()),0,static_cast<int>(visible.size())-1);
            ++copy_token;copy_pending=false;selected=visible[static_cast<size_t>(next)];RevealSelection();if(preview.IsOpen())ShowPreview();else PrefetchPreview();Invalidate();return;
        }
        if(key==VK_UP||key==VK_DOWN){SetFocus(window);++copy_token;copy_pending=false;if(visible.empty())return;const auto it=std::find(visible.begin(),visible.end(),selected);int index=static_cast<int>(it-visible.begin());index=std::clamp(index+(key==VK_DOWN?1:-1),0,static_cast<int>(visible.size())-1);selected=visible[static_cast<size_t>(index)];RevealIndex(index);if(preview.IsOpen())ShowPreview();else PrefetchPreview();Invalidate();}
    }
    static LRESULT CALLBACK SearchProc(HWND w,UINT m,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR data){
        auto* p=reinterpret_cast<Impl*>(data);
        if(m==WM_LBUTTONDOWN){p->RememberTarget(GetForegroundWindow());SetForegroundWindow(p->window);SetFocus(w);}
        if(m==WM_IME_STARTCOMPOSITION){wchar_t value[513]{};GetWindowTextW(w,value,513);p->ime_original=value;p->ime_text.clear();p->ime_active=true;SendMessageW(w,EM_GETSEL,reinterpret_cast<WPARAM>(&p->ime_first),reinterpret_cast<LPARAM>(&p->ime_last));p->Invalidate();}
        if(m==WM_IME_COMPOSITION){HIMC context=ImmGetContext(w);if(context){if(lp&GCS_COMPSTR){const LONG bytes=ImmGetCompositionStringW(context,GCS_COMPSTR,nullptr,0);if(bytes>=0&&bytes<=1024){p->ime_text.resize(static_cast<size_t>(bytes)/sizeof(wchar_t));if(bytes)ImmGetCompositionStringW(context,GCS_COMPSTR,p->ime_text.data(),static_cast<DWORD>(bytes));}const LONG cursor=ImmGetCompositionStringW(context,GCS_CURSORPOS,nullptr,0);p->ime_cursor=cursor<0?0:static_cast<DWORD>(cursor);}if(lp&GCS_RESULTSTR)p->ime_text.clear();ImmReleaseContext(w,context);}p->Invalidate();}
        if(m==WM_IME_ENDCOMPOSITION){p->ime_active=false;p->ime_text.clear();p->ime_original.clear();p->Invalidate();}
        if(p->ime_active&&(m==WM_KEYDOWN||m==WM_CHAR))return DefSubclassProc(w,m,wp,lp);

        if(m==WM_SETFOCUS){p->suppress_search_space=false;p->caret_on=true;const UINT blink=GetCaretBlinkTime();if(blink&&blink!=INFINITE)SetTimer(p->EventWindow(),3,blink,nullptr);p->Invalidate();}
        if(m==WM_KILLFOCUS){KillTimer(p->EventWindow(),3);p->caret_on=false;p->CancelNaming();p->Invalidate();}
        if(m==WM_SYSKEYDOWN&&(wp==VK_LEFT||wp==VK_RIGHT)&&!p->naming&&!p->ime_active){p->Key(wp);return 0;}
        if(m==WM_PAINT){PAINTSTRUCT paint{};BeginPaint(w,&paint);EndPaint(w,&paint);p->Invalidate();return 0;}
        if(m==WM_KEYDOWN||m==WM_LBUTTONUP){p->caret_on=true;p->Invalidate();}
        if(m==WM_KEYDOWN){
            if(p->ime_active)return DefSubclassProc(w,m,wp,lp);
            if(p->naming){
                if(wp==VK_RETURN){if(!(lp&(1LL<<30)))p->CommitNaming();return 0;}
                if(wp==VK_ESCAPE){p->CancelNaming();return 0;}
                if(wp==VK_TAB||wp==VK_UP||wp==VK_DOWN||wp==VK_PRIOR||wp==VK_NEXT||wp==VK_F2)return 0;
                return DefSubclassProc(w,m,wp,lp);
            }
            if(wp==VK_F2){if(!(lp&(1LL<<30)))p->Key(wp);return 0;}
            if((GetKeyState(VK_CONTROL)&0x8000)&&((wp>='0'&&wp<='9')||wp=='G'||(wp=='N'&&(GetKeyState(VK_SHIFT)&0x8000)))){if(!(lp&(1LL<<30)))p->Key(wp);return 0;}
            if(wp=='D'&&(GetKeyState(VK_CONTROL)&0x8000)){if(!(lp&(1LL<<30)))p->Key(wp);return 0;}
            if(wp==VK_TAB){SetFocus(p->window);return 0;}
            if(wp==VK_SPACE&&p->query.empty()){
                p->suppress_search_space=true;
                if(!(lp&(1LL<<30)))p->Key(wp);
                return 0;
            }
            if(wp==VK_ESCAPE||wp==VK_RETURN||wp==VK_DOWN||wp==VK_UP||wp==VK_PRIOR||wp==VK_NEXT){if(wp!=VK_RETURN||!(lp&(1LL<<30)))p->Key(wp);return 0;}
            if(wp!=VK_SPACE)p->suppress_search_space=false;
        }
        // TranslateMessage emits WM_CHAR even when WM_KEYDOWN was handled.
        if(m==WM_CHAR&&(wp==VK_RETURN||wp==VK_ESCAPE||wp==VK_TAB||wp==4||wp==7||wp==14))return 0;
        if(m==WM_CHAR&&wp==L' '&&p->suppress_search_space){p->suppress_search_space=false;return 0;}
        if(m==WM_KEYUP&&wp==VK_SPACE)p->suppress_search_space=false;
        if(m==WM_NCDESTROY)RemoveWindowSubclass(w,SearchProc,1);return DefSubclassProc(w,m,wp,lp);
    }
    static LRESULT CALLBACK Proc(HWND w,UINT m,WPARAM wp,LPARAM lp){
        auto* p=reinterpret_cast<Impl*>(GetWindowLongPtrW(w,GWLP_USERDATA));if(m==WM_NCCREATE){p=static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);if(!p->window)p->window=w;SetWindowLongPtrW(w,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(p));}if(!p)return DefWindowProcW(w,m,wp,lp);
        if(p->opening&&w==p->folded_window){
            if(m==WM_ACTIVATE&&LOWORD(wp)==WA_INACTIVE&&reinterpret_cast<HWND>(lp)!=p->native_window&&!p->pinned){p->Fold();return 0;}
            if(m==WM_KEYDOWN||m==WM_CHAR){p->StopOpening(true);return SendMessageW(p->native_window,m,wp,lp);}
        }
        // Parked native HWND remains the stable async/controller endpoint.
        if(p->switching_window||(w!=p->window&&m!=WM_NCCREATE)){
            const bool service=m==StackRefreshMessage||m==clipboard::SessionStoreReady||m==clipboard::FileIconReady||m==WM_CLIPBOARDUPDATE||m==WM_HOTKEY||m==WM_TIMER||m==clipboard::QuickKeyMessage||m==clipboard::QuickWheelMessage||m==clipboard::QuickDismissMessage||m==WM_COMMAND||m==WM_CTLCOLOREDIT||(m==WM_SETTINGCHANGE&&w==p->EventWindow());
            if(!service){
                if(m==WM_NCCALCSIZE&&wp)return 0;
                if(m==WM_ERASEBKGND)return 1;
                if(m==WM_PAINT){PAINTSTRUCT ps{};BeginPaint(w,&ps);EndPaint(w,&ps);return 0;}
                return DefWindowProcW(w,m,wp,lp);
            }
        }
        try {switch(m){
        case clipboard::SessionStoreReady:p->DrainStore();return 0;
        case clipboard::FileIconReady:p->Invalidate();return 0;
        case WM_SETTINGCHANGE:if(!MotionEnabled()){p->StopOpening(true);p->StopLiquidDrag(true);}p->acrylic=p->ApplyBackdrop();p->ClearFileIcons();p->Invalidate();return 0;
        case WM_NCCALCSIZE:if(wp)return 0;break;
        case WM_NCHITTEST:return p->ResizeHit({GET_X_LPARAM(lp),GET_Y_LPARAM(lp)});
        case WM_GETMINMAXINFO:{auto* limits=reinterpret_cast<MINMAXINFO*>(lp);MONITORINFO monitor{sizeof(monitor)};GetMonitorInfoW(MonitorFromWindow(w,MONITOR_DEFAULTTONEAREST),&monitor);
            const LONG max_w=monitor.rcWork.right-monitor.rcWork.left-8,max_h=monitor.rcWork.bottom-monitor.rcWork.top-8;
            limits->ptMinTrackSize={std::min(max_w,static_cast<LONG>((p->expanded?static_cast<float>(clipboard::PanelMinWidthDip):p->CW)*p->scale)),std::min(max_h,static_cast<LONG>((p->expanded?static_cast<float>(clipboard::PanelMinHeightDip):p->CH)*p->scale))};
            limits->ptMaxTrackSize={max_w,max_h};return 0;}
        case WM_ENTERSIZEMOVE:p->resizing=p->expanded;p->CloseMore();return 0;
        case WM_SIZE:p->ResizeClient(LOWORD(lp),HIWORD(lp));return 0;
        case WM_EXITSIZEMOVE:if(p->resizing){p->resizing=false;p->summon_cursor.reset();RECT bounds{};GetWindowRect(w,&bounds);p->anchor={bounds.right,bounds.top};p->placed=true;
            if(!p->SaveLayout()){p->status=L"窗口尺寸保存失败";p->Invalidate();}}return 0;
        case WM_ERASEBKGND:return 1;
        case WM_PAINT:{PAINTSTRUCT ps{};BeginPaint(w,&ps);try{p->Present();}catch(...){EndPaint(w,&ps);throw;}EndPaint(w,&ps);return 0;}
        case WM_CLIPBOARDUPDATE:p->Read();return 0;
        case WM_MOUSEACTIVATE:p->RememberTarget(GetForegroundWindow());if(p->pinned||!p->expanded)return MA_NOACTIVATE;break;
        case WM_HOTKEY:if(p->shortcut_key&&(!p->hotkey_allowed||p->hotkey_allowed())&&!p->hotkeys_suspended)p->ShowFromShortcut();return 0;
        case clipboard::QuickKeyMessage:if(p->quick_active)p->QuickKey(wp,(lp&1)!=0);return 0;
        case clipboard::QuickWheelMessage:if(p->quick_active&&p->quick&&p->QuickDestinationValid())SendMessageW(p->quick->Window(),WM_MOUSEWHEEL,wp,lp);return 0;
        case clipboard::QuickDismissMessage:p->CloseQuick();return 0;
        case WM_TIMER:if(wp==13){p->FxTick();return 0;}if(wp==14){KillTimer(w,14);p->CommitPendingDelete();return 0;}if(wp==12){p->ScrollAnimTick();return 0;}if(wp==10){p->OpeningTick(GetTickCount64());return 0;}if(wp==9){p->LiquidDragTick(GetTickCount64());return 0;}if(wp==8){p->ShortcutFocusTick();return 0;}if(wp==7){if(p->quick_active&&(!p->QuickDestinationValid()||p->hotkeys_suspended||(p->hotkey_allowed&&!p->hotkey_allowed())))p->CloseQuick();return 0;}if(wp==6){KillTimer(w,6);if(p->status==L"已固定 · F2 取消固定"||p->status==L"已取消固定 · F2 固定面板"){p->status.clear();p->Invalidate();}return 0;}if(wp==5){KillTimer(w,5);return 0;}if(wp==4){KillTimer(w,4);if(!p->expanded&&p->stack){p->composition.reset();p->parked_composition.reset();p->ReleaseRenderer();ClipboardComposition::DropPrewarm();return 0;}if(!p->expanded&&!p->opening&&!p->liquid_drag_active){if(p->composition)p->composition->TrimIdle();p->parked_composition.reset();p->ReleaseRenderer();if(p->idle_layered)ClipboardComposition::DropPrewarm();else SetTimer(w,11,IdleStripDelayMs,nullptr);}return 0;}if(wp==11){KillTimer(w,11);if(!p->expanded&&!p->opening&&!p->liquid_drag_active){p->ReleaseRenderer();p->EnterIdleStrip();if(p->idle_layered)ClipboardComposition::DropPrewarm();}return 0;}if(wp==3){if(GetFocus()==p->search){p->caret_on=!p->caret_on;p->Invalidate();}else KillTimer(w,3);return 0;}if(wp==1)p->Read();if(wp==2)p->PasteTick();return 0;
        case WM_LBUTTONDOWN:p->RememberTarget(GetForegroundWindow());p->paste_click.reset();if(p->popup_open){const float mx=GET_X_LPARAM(lp)/p->scale,my=GET_Y_LPARAM(lp)/p->scale;if(p->InsidePopup(mx,my)){const int row=p->PopupHit(mx,my);if(row>=0)p->ChoosePopup(row);}else p->ClosePopup();return 0;}if(p->menu_open){const float mx=GET_X_LPARAM(lp)/p->scale,my=GET_Y_LPARAM(lp)/p->scale;const int row=p->MenuHit(mx,my);if(row>=0)p->ChooseMore(row);else if(mx<p->W-264||mx>p->W-16||my<64||my>252)p->CloseMore();return 0;}{const float x=GET_X_LPARAM(lp)/p->scale,y=GET_Y_LPARAM(lp)/p->scale;if(p->expanded&&x>=p->W-9&&y>=p->ListTop&&y<=p->ListBottom&&p->MaxScrollPx()>0){p->BeginScroll(y);return 0;}if(!p->expanded||p->HeaderDragHit(x,y)){POINT point{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};ClientToScreen(w,&point);p->BeginDrag(point);return 0;}}if(!p->pinned||GetForegroundWindow()==w)SetFocus(w);for(auto it=p->hits.rbegin();it!=p->hits.rend();++it){const float x=GET_X_LPARAM(lp)/p->scale,y=GET_Y_LPARAM(lp)/p->scale;const auto r=it->rect;if(x>=r.left&&x<r.right&&y>=r.top&&y<r.bottom){const auto h=*it;if(h.action==30&&p->expanded){POINT at{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};ClientToScreen(w,&at);p->row_press=RowPress{h.id,at};}p->click_mods=h.action==30?(wp&(MK_CONTROL|MK_SHIFT)):0;if(p->pinned&&(h.action==30||h.action==33)&&!p->preview.IsOpen()&&!p->click_mods)p->paste_click=h;else p->Action(h.action,h.id);break;}}return 0;
        case WM_LBUTTONUP:p->row_press.reset();if(p->paste_click){const auto hit=*p->paste_click;p->paste_click.reset();const float x=GET_X_LPARAM(lp)/p->scale,y=GET_Y_LPARAM(lp)/p->scale;if(x>=hit.rect.left&&x<hit.rect.right&&y>=hit.rect.top&&y<hit.rect.bottom)p->Action(hit.action,hit.id);return 0;}if(p->scroll_drag){p->ScrollAt(GET_Y_LPARAM(lp)/p->scale);p->scroll_drag=false;ReleaseCapture();return 0;}if(p->drag_pending){p->EndDrag();return 0;}break;
        case WM_CAPTURECHANGED:case WM_CANCELMODE:p->paste_click.reset();p->drag_pending=p->scroll_drag=false;p->ReleaseLiquidDrag();p->CancelDismiss();p->CloseMore();p->ClosePopup();return 0;
        case WM_MOUSEMOVE:{if(p->row_press&&p->RowDragMove())return 0;if(w==p->folded_window&&!p->expanded){if(p->idle_layered)ClipboardComposition::Prewarm();SetTimer(p->EventWindow(),4,10000,nullptr);}if(p->scroll_drag){p->ScrollAt(GET_Y_LPARAM(lp)/p->scale);return 0;}if(p->menu_open){const int row=p->MenuHit(GET_X_LPARAM(lp)/p->scale,GET_Y_LPARAM(lp)/p->scale);if(row!=p->menu_hover){p->menu_hover=row;p->Invalidate();}return 0;}if(p->popup_open){const int row=p->PopupHit(GET_X_LPARAM(lp)/p->scale,GET_Y_LPARAM(lp)/p->scale);if(row!=p->popup_hover){p->popup_hover=row;p->Invalidate();}return 0;}if(p->drag_pending){POINT point{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};ClientToScreen(w,&point);p->Drag(point);return 0;}int hit=-1;const float x=GET_X_LPARAM(lp)/p->scale,y=GET_Y_LPARAM(lp)/p->scale;for(size_t i=0;i<p->hits.size();++i){const auto r=p->hits[i].rect;if(x>=r.left&&x<r.right&&y>=r.top&&y<r.bottom)hit=static_cast<int>(i);}if(hit!=p->hover){p->hover=hit;const wchar_t* hint=L"";if(hit>=0){switch(p->hits[static_cast<size_t>(hit)].action){case 31:hint=L"收藏 / 取消收藏 · Ctrl+D";break;case 32:hint=L"复制内容 · Ctrl+C";break;case 33:hint=L"Enter 粘贴 · Shift+Enter 纯文本";break;case 34:hint=L"删除记录 · Delete";break;case 36:hint=L"钉到桌面";break;case 37:hint=L"识别图中文字并复制";break;case 38:hint=L"标注后钉到桌面";break;case 39:hint=L"用默认浏览器打开";break;case 42:hint=L"在资源管理器中显示";break;case 43:hint=L"复制文件路径";break;case 44:hint=L"复制 HEX 色值";break;case 45:hint=L"纯文本粘贴 · Shift+Enter";break;case 50:hint=L"按列表顺序合并文本并复制 · Ctrl+C";break;case 52:hint=L"删除所选 · Delete，可撤销";break;case 53:hint=L"取消多选 · Esc";break;case 54:hint=L"恢复刚删除的记录 · Ctrl+Z";break;case 35:hint=L"移到分组 · Ctrl+G · Ctrl+1–9";break;case 40:hint=L"右键可重命名、更换颜色或删除分组";break;case 41:hint=L"新建分组 · Ctrl+Shift+N";break;case 10:hint=L"固定 / 取消固定面板 · F2";break;case 15:hint=L"暂停后已有历史保留，仍可搜索和粘贴";break;case 26:hint=L"按内容类型筛选";break;case 14:hint=L"折叠到屏幕边缘 · Esc";break;default:break;}}p->status=hint;p->Invalidate();}return 0;}
        case WM_MOUSEWHEEL:if(p->preview.ForwardWheel(wp,lp))return 0;if(p->menu_open||p->popup_open)return 0;{POINT point{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};ScreenToClient(w,&point);const float x=point.x/p->scale,y=point.y/p->scale;if(p->expanded&&y>=clipboard::cards::ChipTop-2&&y<clipboard::cards::ChipBottom+4&&x<p->W-70){p->tab_scroll=std::clamp(p->tab_scroll-GET_WHEEL_DELTA_WPARAM(wp)/static_cast<float>(WHEEL_DELTA)*48.f,0.f,p->tab_scroll_max);p->Invalidate();return 0;}}p->WheelScroll(GET_WHEEL_DELTA_WPARAM(wp));return 0;
        case WM_KEYDOWN:{const bool ctrl_key=(GetKeyState(VK_CONTROL)&0x8000)&&(wp=='D'||wp=='G'||wp=='N'||(wp>='0'&&wp<='9'));if((wp!=VK_SPACE&&wp!=VK_F2&&wp!=VK_RETURN&&!ctrl_key)||!(lp&(1LL<<30)))p->Key(wp);return 0;}
        case WM_SYSKEYDOWN:if((wp==VK_LEFT||wp==VK_RIGHT)&&p->expanded&&!p->naming){p->Key(wp);return 0;}break;
        case WM_RBUTTONUP:{const float x=GET_X_LPARAM(lp)/p->scale,y=GET_Y_LPARAM(lp)/p->scale;if(p->popup_open){p->ClosePopup();return 0;}if(p->menu_open){p->CloseMore();return 0;}if(!p->expanded)return 0;
            for(auto it=p->hits.rbegin();it!=p->hits.rend();++it){const auto r=it->rect;if(x<r.left||x>=r.right||y<r.top||y>=r.bottom)continue;
                if(it->action==40){p->OpenGroupPopup(static_cast<uint32_t>(it->id),x-12,r.bottom+4);return 0;}
                if(it->action==41){p->BeginNaming(1,0,0);return 0;}
                if(p->IsCardAction(it->action)&&it->id){p->RememberTarget(GetForegroundWindow());p->OpenEntryPopup(it->id,x-12,y+6,false,y-6);return 0;}
                break;}
            return 0;}
        case WM_CHAR:if(std::exchange(p->swallow_char,false))return 0;if(p->menu_open||p->popup_open||wp==L' ')return 0;if(wp>32&&!(GetKeyState(VK_CONTROL)&0x8000)){SetFocus(p->search);SendMessageW(p->search,m,wp,lp);}return 0;
        case WM_COMMAND:if(LOWORD(wp)==201&&HIWORD(wp)==EN_CHANGE&&p->naming){p->Invalidate();return 0;}if(LOWORD(wp)==201&&HIWORD(wp)==EN_CHANGE){wchar_t text[513]{};GetWindowTextW(p->search,text,513);p->query=text;p->scroll=0;p->scroll_offset=0;p->Filter();}return 0;
        case WM_CTLCOLOREDIT:SetTextColor(reinterpret_cast<HDC>(wp),p->dark?RGB(231,239,251):RGB(40,51,70));SetBkColor(reinterpret_cast<HDC>(wp),p->dark?RGB(31,40,55):RGB(255,255,255));return reinterpret_cast<LRESULT>(p->edit_brush);
        case WM_ACTIVATE:if(LOWORD(wp)==WA_INACTIVE){p->CloseMore();p->ClosePopup();}p->RememberTarget(GetForegroundWindow());if(LOWORD(wp)==WA_INACTIVE&&p->expanded&&!p->pinned&&!p->drag_pending&&!p->resizing&&!p->preview.OwnsWindow(reinterpret_cast<HWND>(lp)))p->Fold();return 0;
        case WM_DPICHANGED:case WM_DISPLAYCHANGE:if(p->liquid_placing){PostMessageW(w,WM_APP+184,m==WM_DPICHANGED,0);return 0;}p->ReflowLiquid(m==WM_DPICHANGED);return 0;
        case WM_APP+184:p->ReflowLiquid(wp!=0);return 0;
        case StackRefreshMessage:p->stack_refresh_queued=false;p->RefreshStack();return 0;
        case WM_CLOSE:p->Fold();return 0;
        case WM_DESTROY:p->liquid_drag_active=p->liquid_drag_timer=false;KillTimer(w,9);KillTimer(w,5);RemoveClipboardFormatListener(w);UnregisterHotKey(w,1);KillTimer(w,1);KillTimer(w,2);KillTimer(w,3);KillTimer(w,4);return 0;
        case WM_NCDESTROY:if(w==p->window)p->window=nullptr;break;
        }}catch(...){
            if(p->liquid_drag_active){KillTimer(w,5);p->StopLiquidDrag(true);p->liquid_failed=true;p->liquid_surface.reset();p->LiquidBackdrop(false);p->compact_positioned=false;try{p->Place();}catch(...){p->Invalidate();}}
            p->status=L"操作失败，请重试";
        }
        return DefWindowProcW(w,m,wp,lp);
    }
};
ClipboardPanel::ClipboardPanel(HWND owner,std::function<void()> settings,std::function<bool()> allowed):impl_(std::make_unique<Impl>(owner,std::move(settings),std::move(allowed))){}
ClipboardPanel::~ClipboardPanel()=default;
bool ClipboardPanel::SetShortcut(UINT modifiers,UINT key){return impl_->SetShortcut(modifiers,key);}
bool ClipboardPanel::Enable(bool enabled,bool dark){return impl_->Enable(enabled,dark);}
bool ClipboardPanel::SetPersistent(bool persistent){return impl_->SetPersistent(persistent);}
void ClipboardPanel::Show(){impl_->Show();}
void ClipboardPanel::SetHotkeysSuspended(bool suspended){impl_->SetHotkeysSuspended(suspended);}
void ClipboardPanel::SetStripVisible(bool visible){impl_->SetStripVisible(visible);}
void ClipboardPanel::SetHistoryLimit(size_t entries){impl_->SetHistoryLimit(entries);}
void ClipboardPanel::SetStripStyle(bool classic){impl_->SetStripStyle(classic);}
void ClipboardPanel::SetCardPreview(bool show){impl_->SetCardPreview(show);}
void ClipboardPanel::SetStripDismissedHandler(std::function<void()> handler){impl_->strip_dismissed=std::move(handler);}
void ClipboardPanel::SetPinHandler(std::function<bool(std::shared_ptr<const Frame>,POINT)> handler){impl_->pin_request=std::move(handler);}
void ClipboardPanel::SetImageOcrResult(std::wstring text){impl_->ImageOcrResult(std::move(text));}
void ClipboardPanel::SetImageActionHandler(std::function<bool(ClipboardImageAction,std::shared_ptr<const Frame>,POINT)> handler){impl_->image_action=std::move(handler);impl_->Invalidate();}
}