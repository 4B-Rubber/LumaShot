#include "pin/clothesline.h"
#include "ui/memory_target.h"
#include <windowsx.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <dxgi1_2.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#pragma comment(lib,"dwmapi.lib")
#pragma comment(lib,"shell32.lib")
#pragma comment(lib,"shcore.lib")

namespace lumashot {
using Microsoft::WRL::ComPtr;
namespace {
constexpr UINT_PTR kAnimationTimer=1,kPollTimer=2;
// One paced animation frame (posted by FramePacer, at most one in the queue).
constexpr UINT kFrameMessage=WM_APP+0x31;
int64_t Qpc(){LARGE_INTEGER v{};QueryPerformanceCounter(&v);return v.QuadPart;}
double QpcPerMs(){static const double per_ms=[]{LARGE_INTEGER f{};QueryPerformanceFrequency(&f);return double(f.QuadPart)/1000.0;}();return per_ms;}
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
struct HandleCloser{void operator()(HANDLE h)const{if(h)CloseHandle(h);}};
ComPtr<IDXGIOutput> FindOutput(IDXGIFactory1* factory,HMONITOR monitor){
    if(!factory||!monitor)return nullptr;
    ComPtr<IDXGIAdapter1> adapter;
    for(UINT a=0;factory->EnumAdapters1(a,adapter.ReleaseAndGetAddressOf())!=DXGI_ERROR_NOT_FOUND;++a){
        ComPtr<IDXGIOutput> output;
        for(UINT o=0;adapter->EnumOutputs(o,output.ReleaseAndGetAddressOf())!=DXGI_ERROR_NOT_FOUND;++o){
            DXGI_OUTPUT_DESC desc{};
            if(SUCCEEDED(output->GetDesc(&desc))&&desc.Monitor==monitor)return output;
        }
    }
    return nullptr;
}
constexpr wchar_t kClass[]=L"LumaShot.Clothesline",kGlowClass[]=L"LumaShot.ClotheslineGlow";
// Top edge: once ClotheslineEdge reveals it (a short rest on the edge), the
// glow fades out over kGlowReach DIP below the edge; the rope comes after a
// longer rest (ClotheslineEdge decides how long and when not at all).
constexpr float kGlowReach=ClotheslineEdge::kKeepDip,kGlowHeight=60,kGlowWindow=64;
// Slack around drawn content (DIP) that still keeps the lowered rope open.
constexpr float kKeepMargin=16;
constexpr float EaseOutBack(float t){const float c=1.3f,u=t-1;return 1+(c+1)*u*u*u+c*u*u;}
constexpr std::array<std::array<uint32_t,2>,5> kClipColors{{{0xe9c08a,0xa87a45},{0x9fd8c3,0x4f9a80},{0xf4a7a3,0xb5605b},{0xa9c7f5,0x5c7fb8},{0xf6d56b,0xb0902a}}};
D2D1_RECT_F Rect(const ClotheslineRect& r){return D2D1::RectF(r.x,r.y,r.x+r.w,r.y+r.h);}
// Area-average downscale (sampled to at most 8x8 per output pixel) so a 4K pin
// becomes a crisp thumbnail once, instead of being minified every frame.
Frame Downscale(const Frame& source,int width,int height){
    const int sw=source.Width(),sh=source.Height();
    width=std::clamp(width,1,std::max(1,sw));height=std::clamp(height,1,std::max(1,sh));
    Frame result;result.bounds={0,0,width,height};result.pixels.resize(size_t(width)*size_t(height));
    if(sw<=0||sh<=0||source.pixels.size()<size_t(sw)*size_t(sh))return result;
    for(int y=0;y<height;++y){
        const int y0=int(int64_t(y)*sh/height),y1=std::max(y0+1,int(int64_t(y+1)*sh/height)),sy=std::max(1,(y1-y0)/8);
        for(int x=0;x<width;++x){
            const int x0=int(int64_t(x)*sw/width),x1=std::max(x0+1,int(int64_t(x+1)*sw/width)),sx=std::max(1,(x1-x0)/8);
            uint32_t r=0,g=0,b=0,n=0;
            for(int yy=y0;yy<y1;yy+=sy)for(int xx=x0;xx<x1;xx+=sx){
                const uint32_t p=source.pixels[size_t(yy)*size_t(sw)+size_t(xx)];b+=p&255;g+=(p>>8)&255;r+=(p>>16)&255;++n;
            }
            result.pixels[size_t(y)*size_t(width)+size_t(x)]=0xff000000u|((r/n)<<16)|((g/n)<<8)|(b/n);
        }
    }
    return result;
}
}

// Display-paced frame clock. WM_TIMER is quantised to the 15.6 ms system tick
// and beats against a 60 / 144 Hz refresh (uneven steps, skipped refreshes),
// so frames come from a worker that waits for the vertical blank of the rope's
// monitor and posts one coalesced message. The thread exists only after the
// first animation and is parked on a condition variable while nothing moves.
// Where vertical blanks are unavailable or do not block (display asleep, some
// virtual adapters) a high-resolution waitable timer at the DWM refresh period
// takes over.
class FramePacer {
public:
    FramePacer(HWND window,UINT message):window_(window),message_(message){}
    ~FramePacer(){
        {std::lock_guard lock(mutex_);quit_=true;}
        wake_.notify_all();
        if(thread_.joinable())thread_.join();
    }
    FramePacer(const FramePacer&)=delete;
    FramePacer& operator=(const FramePacer&)=delete;
    void Start(HMONITOR monitor){
        {std::lock_guard lock(mutex_);running_=true;monitor_=monitor;}
        if(!thread_.joinable())thread_=std::thread([this]{Loop();});
        wake_.notify_all();
    }
    void Stop(){std::lock_guard lock(mutex_);running_=false;}
    // The posted frame was handled: the next vertical blank may post again.
    // Acknowledging after rendering keeps input responsive when a frame is slow.
    void Done(){pending_=false;}
private:
    void WaitPeriod(HANDLE timer)const{
        DWM_TIMING_INFO timing{};timing.cbSize=sizeof(timing);
        const double per_ms=QpcPerMs();
        double period=16.667*per_ms,phase=0;
        if(SUCCEEDED(DwmGetCompositionTimingInfo(nullptr,&timing))&&timing.qpcRefreshPeriod>per_ms*2&&timing.qpcRefreshPeriod<per_ms*100){
            period=double(timing.qpcRefreshPeriod);phase=double(timing.qpcVBlank);
        }
        const double now=double(Qpc());
        double due=period;
        if(phase>0){due=std::fmod(phase-now,period);if(due<=per_ms*.5)due+=period;}
        if(timer){
            LARGE_INTEGER when{};when.QuadPart=-std::max<LONGLONG>(1,LONGLONG(due/per_ms*10000.0));
            if(SetWaitableTimer(timer,&when,0,nullptr,nullptr,FALSE)){WaitForSingleObject(timer,200);return;}
        }
        Sleep(DWORD(std::max(1.0,due/per_ms)));
    }
    void Loop(){
        SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_ABOVE_NORMAL);
        ComPtr<IDXGIFactory1> factory;CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf()));
        HANDLE raw=CreateWaitableTimerExW(nullptr,nullptr,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_ALL_ACCESS);
        if(!raw)raw=CreateWaitableTimerW(nullptr,FALSE,nullptr);
        const std::unique_ptr<void,HandleCloser> timer(raw);
        ComPtr<IDXGIOutput> output;HMONITOR current{};int quick{},timer_rounds{};
        for(;;){
            {
                std::unique_lock lock(mutex_);
                wake_.wait(lock,[&]{return running_||quit_;});
                if(quit_)return;
                if(monitor_!=current){current=monitor_;output=FindOutput(factory.Get(),current);quick=timer_rounds=0;}
            }
            bool paced=false;
            if(timer_rounds>0){if(--timer_rounds==0&&!output)current=nullptr;}
            else if(output){
                const int64_t start=Qpc();
                if(SUCCEEDED(output->WaitForVBlank())){
                    // One blank right after the previous frame is normal; a run
                    // of blanks that never block means there is no real vsync.
                    if(double(Qpc()-start)<QpcPerMs()*.3){if(++quick>=3){quick=0;timer_rounds=120;}}else quick=0;
                    paced=true;
                }else{output.Reset();timer_rounds=120;}
            }else timer_rounds=120;
            if(!paced)WaitPeriod(timer.get());
            if(!pending_.exchange(true)&&!PostMessageW(window_,message_,0,0))pending_=false;
        }
    }
    HWND window_;UINT message_;
    std::mutex mutex_;std::condition_variable wake_;bool running_{},quit_{};HMONITOR monitor_{};
    std::atomic<bool> pending_{};
    std::thread thread_;
};

ComPtr<ID2D1PathGeometry> CreateHangerIcon(ID2D1Factory* factory){
    ComPtr<ID2D1PathGeometry> hanger;ComPtr<ID2D1GeometrySink> sink;
    if(!factory||FAILED(factory->CreatePathGeometry(hanger.GetAddressOf()))||FAILED(hanger->Open(sink.GetAddressOf())))return nullptr;
    sink->BeginFigure(D2D1::Point2F(12,4),D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(14,6),D2D1::SizeF(2,2),0,D2D1_SWEEP_DIRECTION_CLOCKWISE,D2D1_ARC_SIZE_SMALL));
    sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(14,7),D2D1::Point2F(13,7.5f),D2D1::Point2F(12,8.2f)));
    sink->AddLine(D2D1::Point2F(12,9));sink->AddLine(D2D1::Point2F(20.5f,15.2f));
    sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(21.4f,15.9f),D2D1::Point2F(20.9f,17.2f),D2D1::Point2F(19.8f,17.2f)));
    sink->AddLine(D2D1::Point2F(4.2f,17.2f));
    sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(3.1f,17.2f),D2D1::Point2F(2.6f,15.9f),D2D1::Point2F(3.5f,15.2f)));
    sink->AddLine(D2D1::Point2F(12,9));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    if(FAILED(sink->Close()))return nullptr;
    return hanger;
}
Clothesline::Clothesline(Callbacks callbacks):callbacks_(std::move(callbacks)){}
Clothesline::~Clothesline(){
    pacer_.reset();
    StopSleeping();
    if(window_){KillTimer(window_,kAnimationTimer);KillTimer(window_,kPollTimer);DestroyWindow(window_);}
    if(glow_window_)DestroyWindow(glow_window_);
}

void Clothesline::EnsureWindow(){
    if(window_)return;
    WNDCLASSW wc{};wc.lpfnWndProc=Proc;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=kClass;wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    if(!RegisterClassW(&wc)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)CheckWin32(false,"Register clothesline");
    CreateWindowExW(WS_EX_LAYERED|WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,kClass,L"LumaShot 晾衣绳",WS_POPUP,0,0,1,1,nullptr,nullptr,GetModuleHandleW(nullptr),this);
    CheckWin32(window_!=nullptr,"Create clothesline");
    CheckWin32(SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,factory_.GetAddressOf())),"Create clothesline renderer");
    CheckWin32(SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(text_factory_.GetAddressOf()))),"Create clothesline text");
    factory_->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND,D2D1_CAP_STYLE_ROUND,D2D1_CAP_STYLE_ROUND,D2D1_LINE_JOIN_ROUND),nullptr,0,round_.GetAddressOf());
    const float twist[]{0.5f,1.3f},dash[]{3.f,2.f};
    factory_->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT,D2D1_CAP_STYLE_FLAT,D2D1_CAP_STYLE_FLAT,D2D1_LINE_JOIN_ROUND,10,D2D1_DASH_STYLE_CUSTOM,0),twist,2,twist_.GetAddressOf());
    factory_->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND,D2D1_CAP_STYLE_ROUND,D2D1_CAP_STYLE_ROUND,D2D1_LINE_JOIN_ROUND,10,D2D1_DASH_STYLE_CUSTOM,0),dash,2,dashed_.GetAddressOf());
}
void Clothesline::Place(POINT point){
    MONITORINFO monitor{sizeof(monitor)};GetMonitorInfoW(MonitorFromPoint(point,MONITOR_DEFAULTTONEAREST),&monitor);
    const RECT r=monitor.rcMonitor;
    if(placed_&&EqualRect(&r,&monitor_))return;
    monitor_=r;placed_=true;
    SetWindowPos(window_,HWND_TOPMOST,r.left,r.top,r.right-r.left,1,SWP_NOACTIVATE|SWP_NOREDRAW);
    const UINT dpi=GetDpiForWindow(window_);
    model_.scale=float(dpi?dpi:96)/96.f;model_.width=float(r.right-r.left);
    // Another monitor (or the first placement after a restore): re-lay out the
    // thumbnails for its width and scale; hidden ones jump straight there.
    model_.Layout();
    if(!model_.visible){model_.rope_y=model_.HiddenY();model_.rope_v=0;for(auto& it:model_.items)it.x=it.tx;}
    ResetSprites();full_update_=true;
}
void Clothesline::ResetSprites(){
    for(auto& [id,thumb]:thumbs_){thumb.bitmap.Reset();for(auto& sprite:thumb.sprites)sprite={};}
    bar_sprite_.Reset();hint_sprite_.Reset();bar_sprite_hover_=-2;rope_cache_.Reset();rope_still_=0;
}

ClotheslineEdge::Output ClotheslineEdge::Update(const Input& in){
    Output out;
    if(in.buttons){buttons_=true;start_=0;revealed_=false;return out;}
    if(buttons_){buttons_=false;quiet_until_=in.now+kReleaseQuietMs;}
    if(in.foreground==Foreground::Fullscreen){start_=0;revealed_=false;return out;}
    const float scale=std::max(.5f,in.scale);
    if(wait_leave_){
        if(!leave_anchored_){leave_anchor_=in.cursor;leave_anchored_=true;}
        if((in.at_edge||in.on_tab)&&float(std::abs(in.cursor.x-leave_anchor_.x))<=kKeepDip*scale){start_=0;return out;}
        wait_leave_=false;
    }
    // A shown tab stays while the cursor can still reach it; leaving the keep
    // zone hides it until the edge is rested on again.
    if(revealed_&&!in.on_tab&&float(in.cursor.y-in.top)>=kKeepDip*scale)revealed_=false;
    if(!(in.at_edge||in.on_tab)||in.now<quiet_until_){start_=0;out.glow=revealed_;return out;}
    const float still=kStillDip*scale;
    if(!start_||float(std::abs(in.cursor.x-anchor_.x))>still||float(std::abs(in.cursor.y-anchor_.y))>still){
        start_=in.now;anchor_=in.cursor;
    }
    // The tab is an explicit target; only bare-edge resting over a maximized
    // window (tabs, menus) needs the longer wait.
    const ULONGLONG need=in.foreground==Foreground::Maximized&&!in.on_tab?kMaximizedDwellMs:kDwellMs;
    const ULONGLONG held=in.now-start_;
    if(held>=kRevealMs)revealed_=true;
    out.glow=revealed_;
    out.dwell=std::min(1.f,float(held)/float(need));
    if(held>=need){out.fire=true;start_=0;revealed_=false;}
    return out;
}
ClotheslineEdge::Foreground ClotheslineEdge::Classify(HWND window,const RECT& monitor){
    if(!window||IsIconic(window))return Foreground::Normal;
    const LONG_PTR style=GetWindowLongPtrW(window,GWL_STYLE);
    const bool zoomed=IsZoomed(window)!=FALSE;
    RECT r{};
    if(FAILED(DwmGetWindowAttribute(window,DWMWA_EXTENDED_FRAME_BOUNDS,&r,sizeof(r)))&&!GetWindowRect(window,&r))return Foreground::Normal;
    const bool covers=r.left<=monitor.left&&r.top<=monitor.top&&r.right>=monitor.right&&r.bottom>=monitor.bottom;
    const bool caption=(style&WS_CAPTION)==WS_CAPTION;
    // A maximized captioned window covers the monitor too when the taskbar
    // auto-hides; that is still "maximized".
    if(covers&&(!caption||!zoomed))return Foreground::Fullscreen;
    return zoomed?Foreground::Maximized:Foreground::Normal;
}
ClotheslineEdge::Foreground ClotheslineEdge::Current(HMONITOR monitor,const RECT& monitor_rect){
    HWND window=GetForegroundWindow();
    if(window)window=GetAncestor(window,GA_ROOT);
    if(!window||window==GetShellWindow()||window==GetDesktopWindow())return Foreground::Normal;
    DWORD process=0;GetWindowThreadProcessId(window,&process);
    if(process==GetCurrentProcessId())return Foreground::Normal;
    wchar_t name[64]{};GetClassNameW(window,name,64);
    for(const wchar_t* shell:{L"Progman",L"WorkerW",L"Shell_TrayWnd",L"Shell_SecondaryTrayWnd"})
        if(!wcscmp(name,shell))return Foreground::Normal;
    // A fullscreen app on another monitor must not disable this one.
    if(MonitorFromWindow(window,MONITOR_DEFAULTTONULL)!=monitor)return Foreground::Normal;
    QUERY_USER_NOTIFICATION_STATE state{};
    if(SUCCEEDED(SHQueryUserNotificationState(&state))&&(state==QUNS_BUSY||state==QUNS_RUNNING_D3D_FULL_SCREEN||state==QUNS_PRESENTATION_MODE))
        return Foreground::Fullscreen;
    return Classify(window,monitor_rect);
}

bool Clothesline::Armed()const{return enabled_&&(!model_.items.empty()||(callbacks_.armed&&callbacks_.armed()));}
void Clothesline::Arm(){if(!enabled_)return;EnsureWindow();UpdatePoll();}
void Clothesline::SetEnabled(bool enabled){
    if(enabled==enabled_)return;
    enabled_=enabled;
    if(enabled){UpdatePoll();return;}
    if(held_){held_.reset();if(window_&&GetCapture()==window_)ReleaseCapture();}
    DragEnd();
    // Every hung pin goes home one after another, then the rope folds away.
    if(!model_.items.empty())RetrieveAll();
    HideGlow();Hide();edge_.Reset();dwell_=0;
    UpdatePoll();
}
void Clothesline::UpdatePoll(){
    const bool need=window_&&(model_.visible||Armed()||glow_shown_);
    // 16 ms frames only while the glow, tab or dwell bar is on screen.
    const UINT interval=glow_shown_||glow_goal_>0||edge_.Dwelling()?16:100;
    if(sleeping_&&(!need||model_.visible||interval!=100))StopSleeping();
    if(sleeping_)return;
    if(need&&(!polling_||poll_interval_!=interval)){polling_=SetTimer(window_,kPollTimer,interval,nullptr)!=0;poll_interval_=interval;}
    else if(!need&&polling_){KillTimer(window_,kPollTimer);polling_=false;edge_.Reset();}
}
void Clothesline::Animate(){
    if(!window_)return;
    if(!IsWindowVisible(window_)&&(model_.visible||!model_.Settled())){
        Render();ShowWindow(window_,SW_SHOWNOACTIVATE);
        SetWindowPos(window_,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
    }
    if(animating_)return;
    last_qpc_=Qpc();
    try{
        if(!pacer_)pacer_=std::make_unique<FramePacer>(window_,kFrameMessage);
        pacer_->Start(MonitorFromRect(&monitor_,MONITOR_DEFAULTTONEAREST));animating_=true;
    }catch(...){}
    // No worker thread available: fall back to the plain window timer.
    if(!animating_)animating_=SetTimer(window_,kAnimationTimer,15,nullptr)!=0;
}
void Clothesline::Show(){if(!enabled_)return;POINT cursor{};GetCursorPos(&cursor);ShowAt(cursor);}
void Clothesline::ShowAt(POINT point){
    EnsureWindow();
    if(!model_.visible){
        Place(point);
        HideGlow();
        model_.visible=true;model_.sag_v+=2*model_.scale;model_.Kick(0.06f);
        SetWindowPos(window_,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
    }
    hide_at_=0;Animate();UpdatePoll();
}
void Clothesline::Hide(){
    if(!window_)return;
    model_.visible=false;model_.preview.reset();hide_at_=0;edge_.Folded();hover_=bar_hover_=-1;
    Animate();
}
void Clothesline::Tick(){
    const auto now=GetTickCount64();
    // Elapsed time from the performance counter (GetTickCount64 moves in 15.6 ms
    // steps), in 60 Hz frames; a stall is capped so the cards are not flung.
    const int64_t qpc=Qpc();
    const float k=std::clamp(float(double(qpc-last_qpc_)/QpcPerMs()/16.667),0.f,2.5f);last_qpc_=qpc;
    if(!retrieve_queue_.empty()&&now>=next_retrieve_){
        const auto id=retrieve_queue_.front();retrieve_queue_.pop_front();
        if(Contains(id))Retrieve(id);
        next_retrieve_=now+90;
    }
    // Sub-steps of at most one 60 Hz frame keep the springs identical at any
    // refresh rate (a late frame integrates in pieces instead of one big step).
    const int steps=std::max(1,int(std::ceil(k-1e-3f)));
    bool moving=false;
    for(int i=0;i<steps;++i)moving|=model_.Step(k/float(steps));
    const bool active=moving||!retrieve_queue_.empty()||held_.has_value();
    // Thumbnails are re-sampled to an exact new size only once things settle.
    defer_thumbs_=active;Render();defer_thumbs_=false;
    if(!active){
        if(pacer_)pacer_->Stop();
        KillTimer(window_,kAnimationTimer);animating_=false;
        if(model_.Settled())ShowWindow(window_,SW_HIDE);
    }
}
void Clothesline::Poll(){
    POINT cursor{};GetCursorPos(&cursor);const auto now=GetTickCount64();
    if(!model_.visible){
        MONITORINFO monitor{sizeof(monitor)};GetMonitorInfoW(MonitorFromPoint(cursor,MONITOR_DEFAULTTONEAREST),&monitor);
        if(!Armed()){edge_.Reset();dwell_=0;UpdateGlow(cursor,now,false,monitor.rcMonitor);UpdatePoll();return;}
        // Top-edge dwell: the cursor resting still on the first pixel row of a
        // monitor (or on the tab) with no button held; a window dragged to the
        // edge does not count. See ClotheslineEdge for the fullscreen /
        // maximized / just-clicked rules.
        const HMONITOR hmonitor=MonitorFromPoint(cursor,MONITOR_DEFAULTTONEAREST);
        const bool buttons=(GetAsyncKeyState(VK_LBUTTON)&0x8000)||(GetAsyncKeyState(VK_RBUTTON)&0x8000)||(GetAsyncKeyState(VK_MBUTTON)&0x8000);
        const bool on_tab=tab_>.9f&&PtInRect(&tab_screen_,cursor);
        const bool at_edge=cursor.y<=monitor.rcMonitor.top+1;
        const UINT dpi=[&]{UINT x=96,y=96;return SUCCEEDED(GetDpiForMonitor(hmonitor,MDT_EFFECTIVE_DPI,&x,&y))?x:96u;}();
        const float scale=float(dpi)/96.f;
        const bool close_to_edge=float(cursor.y-monitor.rcMonitor.top)<kGlowReach*scale;
        if(!close_to_edge)foreground_tick_=0; // look again as soon as the cursor comes back
        else if(!foreground_tick_||now-foreground_tick_>=250||hmonitor!=foreground_monitor_){
            foreground_=ClotheslineEdge::Current(hmonitor,monitor.rcMonitor);foreground_tick_=now;foreground_monitor_=hmonitor;
        }
        const auto out=edge_.Update({cursor,now,buttons,at_edge,on_tab,close_to_edge?foreground_:ClotheslineEdge::Foreground::Normal,scale,monitor.rcMonitor.top});
        dwell_=out.dwell;
        if(out.fire){dwell_=0;ShowAt(cursor);return;}
        UpdateGlow(cursor,now,out.glow,monitor.rcMonitor);
        TrackStill(cursor,close_to_edge||at_edge||on_tab);
        return;
    }
    if(dragging_||held_){hide_at_=0;return;}
    // Only drawn content keeps the rope open, not the whole band under it.
    const bool inside=cursor.x>=monitor_.left&&cursor.x<monitor_.right&&cursor.y>=monitor_.top&&
        KeepOpen(float(cursor.x-monitor_.left),float(cursor.y-monitor_.top));
    if(inside||now<hold_until_){hide_at_=0;return;}
    if(!hide_at_)hide_at_=now+600;
    else if(now>=hide_at_)Hide();
}

void Clothesline::TrackStill(POINT cursor,bool near_edge){
    const bool resting=polling_&&poll_interval_==100&&!model_.visible&&!near_edge&&!glow_shown_&&glow_goal_<=0&&!edge_.Dwelling();
    if(!resting||cursor.x!=still_cursor_.x||cursor.y!=still_cursor_.y){still_cursor_=cursor;still_polls_=0;return;}
    if(++still_polls_<kSleepAfterPolls)return;
    const RAWINPUTDEVICE mouse{0x01,0x02,RIDEV_INPUTSINK,window_};
    if(!RegisterRawInputDevices(&mouse,1,sizeof(mouse))){still_polls_=0;return;}  // keep polling
    KillTimer(window_,kPollTimer);polling_=false;sleeping_=true;
}
void Clothesline::StopSleeping(){
    if(!sleeping_)return;
    sleeping_=false;still_polls_=0;
    const RAWINPUTDEVICE mouse{0x01,0x02,RIDEV_REMOVE,nullptr};
    RegisterRawInputDevices(&mouse,1,sizeof(mouse));
}
void Clothesline::WakeOnInput(){
    if(!sleeping_)return;
    StopSleeping();UpdatePoll();
    if(polling_)Poll();
}
void Clothesline::UpdateGlow(POINT cursor,ULONGLONG now,bool allowed,const RECT& monitor){
    const float dt=float(std::min<ULONGLONG>(100,now-glow_tick_));glow_tick_=now;
    float goal=0;
    if(allowed&&cursor.x>=monitor.left&&cursor.x<monitor.right){
        if(!glow_window_||!EqualRect(&monitor,&glow_monitor_)){
            if(!glow_window_){
                WNDCLASSW wc{};wc.lpfnWndProc=DefWindowProcW;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=kGlowClass;
                if(!RegisterClassW(&wc)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)return;
                glow_window_=CreateWindowExW(WS_EX_LAYERED|WS_EX_TRANSPARENT|WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,kGlowClass,L"",WS_POPUP,0,0,1,1,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
                if(!glow_window_)return;
            }
            glow_monitor_=monitor;
            SetWindowPos(glow_window_,HWND_TOPMOST,monitor.left,monitor.top,monitor.right-monitor.left,1,SWP_NOACTIVATE|SWP_NOREDRAW);
            const UINT dpi=GetDpiForWindow(glow_window_);glow_scale_=float(dpi?dpi:96)/96.f;
        }
        const float dy=float(cursor.y-monitor.top)/glow_scale_;
        if(dy<kGlowReach)goal=1-std::max(0.f,dy)/kGlowReach;
    }
    glow_goal_=goal;
    // Exponential easing, close to the prototype's 0.25 s opacity transition.
    glow_+=(goal-glow_)*(1-std::exp(-dt/70.f));
    tab_+=((goal>0?1.f:0.f)-tab_)*(1-std::exp(-dt/90.f));
    if(goal<=0&&glow_<.004f)glow_=0;
    if(goal<=0&&tab_<.004f)tab_=0;
    if(glow_<=0&&tab_<=0&&dwell_<=0)HideGlow();else RenderGlow();
    UpdatePoll();
}
void Clothesline::HideGlow(){
    if(glow_window_&&glow_shown_)ShowWindow(glow_window_,SW_HIDE);
    glow_shown_=false;glow_=glow_goal_=tab_=0;tab_screen_={};
}
float Clothesline::TextWidth(const wchar_t* text,IDWriteTextFormat* format)const{
    ComPtr<IDWriteTextLayout> layout;DWRITE_TEXT_METRICS metrics{};
    if(!format||FAILED(text_factory_->CreateTextLayout(text,UINT32(wcslen(text)),format,4096,256,layout.GetAddressOf()))||FAILED(layout->GetMetrics(&metrics)))return float(wcslen(text))*12;
    return metrics.widthIncludingTrailingWhitespace;
}
void Clothesline::RenderGlow(){
    if(!glow_window_||!factory_||!text_factory_)return;
    const float s=glow_scale_;
    const int width=std::max(1,int(glow_monitor_.right-glow_monitor_.left)),height=std::max(1,int(std::ceil(kGlowWindow*s)));
    const int surface_w=EvenSurfaceSize(width),surface_h=EvenSurfaceSize(height);
    if(!glow_surface_||!glow_target_||glow_width_!=surface_w||glow_height_!=surface_h){
        glow_target_.Reset();glow_brush_.Reset();
        glow_surface_=std::make_unique<DibSurface>(surface_w,surface_h);glow_width_=surface_w;glow_height_=surface_h;
        glow_target_=CreateMemoryRenderTarget(factory_.Get(),glow_surface_->Pixels(),surface_w,surface_h,surface_w);
        if(!glow_target_)return;
    }
    auto* t=glow_target_.Get();const float w=float(width);
    t->BeginDraw();t->SetTransform(D2D1::Matrix3x2F::Identity());t->Clear(D2D1::ColorF(0,0.f));
    if(!glow_brush_)t->CreateSolidColorBrush(D2D1::ColorF(0xffffff),glow_brush_.GetAddressOf());
    // Edge glow: a soft blue wash that brightens as the cursor approaches.
    if(glow_>0){
        const D2D1_GRADIENT_STOP stops[]{{0,D2D1::ColorF(.53f,.71f,1.f,.30f*glow_)},{.35f,D2D1::ColorF(.53f,.71f,1.f,.16f*glow_)},{1,D2D1::ColorF(.53f,.71f,1.f,0)}};
        ComPtr<ID2D1GradientStopCollection> collection;ComPtr<ID2D1LinearGradientBrush> wash;
        if(SUCCEEDED(t->CreateGradientStopCollection(stops,3,collection.GetAddressOf()))&&
            SUCCEEDED(t->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0,0),D2D1::Point2F(0,kGlowHeight*s)),collection.Get(),wash.GetAddressOf())))
            t->FillRectangle(D2D1::RectF(0,0,w,kGlowHeight*s),wash.Get());
        if(glow_brush_){glow_brush_->SetColor(D2D1::ColorF(.75f,.85f,1.f,.55f*glow_));t->FillRectangle(D2D1::RectF(0,0,w,std::max(1.f,s)),glow_brush_.Get());}
    }
    // Dwell bar: grows from the centre while the cursor rests on the edge.
    if(dwell_>0){
        const float bw=dwell_*std::min(520*s,w*.5f),cx=w/2;
        const D2D1_GRADIENT_STOP stops[]{{0,D2D1::ColorF(0x9cc2ff,0)},{.25f,D2D1::ColorF(0x9cc2ff)},{.5f,D2D1::ColorF(0xffffff)},{.75f,D2D1::ColorF(0x9cc2ff)},{1,D2D1::ColorF(0x9cc2ff,0)}};
        ComPtr<ID2D1GradientStopCollection> collection;ComPtr<ID2D1LinearGradientBrush> bar;
        if(bw>1&&SUCCEEDED(t->CreateGradientStopCollection(stops,5,collection.GetAddressOf()))&&
            SUCCEEDED(t->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(cx-bw/2,0),D2D1::Point2F(cx+bw/2,0)),collection.Get(),bar.GetAddressOf()))){
            for(const auto& [h,opacity]:std::array<std::pair<float,float>,3>{{{14.f,.12f},{8.f,.28f},{3.f,1.f}}}){
                bar->SetOpacity(opacity);
                t->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cx-bw/2,-h*s,cx+bw/2,h*s),3*s,3*s),bar.Get());
            }
        }
    }
    // Tab: slides down from the edge and explains what resting there does.
    tab_screen_={};
    if(tab_>0&&glow_brush_){
        if(!glow_format_||glow_format_scale_!=s){
            glow_format_.Reset();glow_format_scale_=s;
            if(SUCCEEDED(text_factory_->CreateTextFormat(L"Microsoft YaHei UI",nullptr,DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,12*s,L"zh-CN",glow_format_.GetAddressOf()))){
                glow_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);glow_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);glow_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            }
        }
        const std::wstring text=model_.items.empty()?std::wstring(L"晾衣绳 · 停在顶部展开"):L"晾着 "+std::to_wstring(model_.items.size())+L" 张 · 停在顶部展开";
        const float tw=TextWidth(text.c_str(),glow_format_.Get()),h=28*s,tabw=14*s+16*s+6*s+tw+14*s;
        const float x=std::round(w/2-tabw/2),y=-(1-EaseOutBack(tab_))*h;
        const auto pill=D2D1::RoundedRect(D2D1::RectF(x,y-14*s,x+tabw,y+h),14*s,14*s);
        glow_brush_->SetColor(D2D1::ColorF(0x141826,.84f*std::min(1.f,tab_*1.5f)));t->FillRoundedRectangle(pill,glow_brush_.Get());
        glow_brush_->SetColor(D2D1::ColorF(0xffffff,.13f));t->DrawRoundedRectangle(pill,glow_brush_.Get(),s);
        // Hanger icon (24-unit path from the prototype, drawn at 16 DIP).
        if(!hanger_)hanger_=CreateHangerIcon(factory_.Get());
        if(const auto& hanger=hanger_){
            const float k=16*s/24;
            t->SetTransform(D2D1::Matrix3x2F::Scale(k,k)*D2D1::Matrix3x2F::Translation(x+14*s,y+(h-16*s)/2));
            glow_brush_->SetColor(D2D1::ColorF(0xe8ecf6));t->DrawGeometry(hanger.Get(),glow_brush_.Get(),1.9f,round_.Get());
            t->SetTransform(D2D1::Matrix3x2F::Identity());
        }
        text_renderer_.Draw(t,text_factory_.Get(),text,glow_format_.Get(),D2D1::RectF(x+36*s,y,x+tabw,y+h),D2D1::ColorF(0xe8ecf6),false);
        tab_screen_={glow_monitor_.left+LONG(x),glow_monitor_.top,glow_monitor_.left+LONG(std::ceil(x+tabw)),glow_monitor_.top+LONG(std::ceil(std::max(0.f,y+h)))};
    }
    if(t->EndDraw()==D2DERR_RECREATE_TARGET){glow_target_.Reset();glow_brush_.Reset();return;}
    POINT origin{glow_monitor_.left,glow_monitor_.top},source{};SIZE size{width,height};BLENDFUNCTION blend{AC_SRC_OVER,0,255,AC_SRC_ALPHA};
    SetWindowPos(glow_window_,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|(glow_shown_?0:SWP_SHOWWINDOW));
    UpdateLayeredWindow(glow_window_,nullptr,&origin,&size,glow_surface_->Dc(),&source,0,&blend,ULW_ALPHA);
    glow_shown_=true;
}

bool Clothesline::DragUpdate(POINT cursor,SIZE image){
    if(!enabled_){DragEnd();return false;}
    EnsureWindow();dragging_=true;
    if(!model_.visible)Place(cursor);
    const bool zone=cursor.x>=monitor_.left&&cursor.x<monitor_.right&&cursor.y>=monitor_.top-8&&
        float(cursor.y-monitor_.top)<ClotheslineModel::HangZone*model_.scale;
    if(zone){
        if(!model_.visible)ShowAt(cursor);
        float tw{},th{};model_.Thumb(float(image.cx),float(image.cy),tw,th);
        model_.preview=model_.InsertIndex(float(cursor.x-monitor_.left));model_.preview_w=tw;model_.preview_h=th;
        hide_at_=0;Animate();return true;
    }
    if(model_.preview){model_.preview.reset();Animate();}
    return false;
}
bool Clothesline::DragDrop(uint64_t id,std::shared_ptr<const Frame> image,POINT cursor){
    if(!enabled_||!dragging_||!model_.preview||!image||image->Width()<=0||image->Height()<=0){DragEnd();return false;}
    const int index=*model_.preview;const float b=2*model_.Border();
    const float w=model_.preview_w+b,h=model_.preview_h+b;
    model_.preview.reset();dragging_=false;
    const float x=float(cursor.x-monitor_.left),y=float(cursor.y-monitor_.top);
    Add(id,std::move(image),{x-w/2,y-h*.15f,w,h},index,300);
    hold_until_=GetTickCount64()+900;
    return true;
}
void Clothesline::DragEnd(){
    dragging_=false;
    if(model_.preview){model_.preview.reset();Animate();}
}
void Clothesline::Hang(uint64_t id,std::shared_ptr<const Frame> image,RECT from){
    if(!enabled_||!image||image->Width()<=0||image->Height()<=0)return;
    ShowAt({(from.left+from.right)/2,(from.top+from.bottom)/2});
    const ClotheslineRect r{float(from.left-monitor_.left),float(from.top-monitor_.top),float(from.right-from.left),float(from.bottom-from.top)};
    Add(id,std::move(image),r,model_.InsertIndex(r.x+r.w/2),520);
    hold_until_=GetTickCount64()+1500;
}
void Clothesline::Add(uint64_t id,std::shared_ptr<const Frame> image,ClotheslineRect from,int index,float ms){
    EnsureWindow();
    if(Contains(id))Remove(id);
    ClotheslineItem item;item.id=id;item.image_w=float(image->Width());item.image_h=float(image->Height());
    item.x=item.tx=from.x+from.w/2;item.from=from;item.fly=0;item.fly_ms=ms;
    thumbs_[id]=Thumb{std::move(image),nullptr,0,0};
    model_.Add(item,index);hover_=-1;
    Animate();UpdatePoll();
}
void Clothesline::Restore(uint64_t id,std::shared_ptr<const Frame> image){
    if(!enabled_||!image||image->Width()<=0||image->Height()<=0||Contains(id))return;
    EnsureWindow();
    if(!placed_){POINT cursor{};GetCursorPos(&cursor);Place(cursor);}
    ClotheslineItem item;item.id=id;item.image_w=float(image->Width());item.image_h=float(image->Height());item.fly=1;
    thumbs_[id]=Thumb{std::move(image),nullptr,0,0};
    model_.Add(item,int(model_.items.size())); // saved order
    if(!model_.visible)for(auto& it:model_.items)it.x=it.tx;
    UpdatePoll();
}
void Clothesline::Remove(uint64_t id){
    if(held_&&held_->id==id){held_.reset();if(window_&&GetCapture()==window_)ReleaseCapture();}
    std::erase(retrieve_queue_,id);
    if(model_.Remove(id)){thumbs_.erase(id);hover_=-1;Animate();}
    UpdatePoll();
}
void Clothesline::Retrieve(uint64_t id){
    const int i=model_.Find(id);if(i<0)return;
    const auto& item=model_.items[size_t(i)];
    auto r=model_.FrameRect(item);const float b=model_.Border();
    const RECT from{monitor_.left+LONG(std::lround(r.x+b)),monitor_.top+LONG(std::lround(r.y+b)),monitor_.left+LONG(std::lround(r.x+r.w-b)),monitor_.top+LONG(std::lround(r.y+r.h-b))};
    model_.Remove(id);thumbs_.erase(id);hover_=-1;
    Animate();UpdatePoll();
    if(callbacks_.retrieve)callbacks_.retrieve(id,from);
}
void Clothesline::RetrieveAll(){
    for(const auto& item:model_.items)if(std::find(retrieve_queue_.begin(),retrieve_queue_.end(),item.id)==retrieve_queue_.end())retrieve_queue_.push_back(item.id);
    next_retrieve_=0;hold_until_=GetTickCount64()+400;Animate();
}

bool Clothesline::KeepOpen(float x,float y)const{
    const float m=kKeepMargin*model_.scale;
    // From the top edge down to just below the rope: the way in from the edge,
    // the hooks, the rope and the bar above it.
    if(x>=model_.Left()-m&&x<=model_.Right()+m&&y<=model_.RopeY(std::clamp(x,model_.Left(),model_.Right()))+m)return true;
    if(model_.Hit(x,y,m)>=0||BarHit(x,y)>=0)return true;
    const auto close_to=[&](const ClotheslineRect& r){return r.w>0&&r.h>0&&x>=r.x-m&&x<r.x+r.w+m&&y>=r.y-m&&y<r.y+r.h+m;};
    return close_to(hint_rect_)||(model_.preview&&close_to(model_.preview_rect));
}
int Clothesline::BarHit(float x,float y)const{
    if(bar_all_.Contains(x,y))return 1;
    if(bar_fold_.Contains(x,y))return 2;
    return bar_.Contains(x,y)?0:-1;
}
LRESULT CALLBACK Clothesline::Proc(HWND window,UINT message,WPARAM wp,LPARAM lp){
    auto* self=reinterpret_cast<Clothesline*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if(message==WM_NCCREATE){self=static_cast<Clothesline*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);self->window_=window;SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
    if(message==WM_NCDESTROY&&self){SetWindowLongPtrW(window,GWLP_USERDATA,0);if(self->pacer_)self->pacer_->Stop();self->window_=nullptr;self->animating_=self->polling_=false;return DefWindowProcW(window,message,wp,lp);}
    if(!self)return DefWindowProcW(window,message,wp,lp);
    try{return self->Handle(message,wp,lp);}catch(...){return DefWindowProcW(window,message,wp,lp);}
}
LRESULT Clothesline::Handle(UINT message,WPARAM wp,LPARAM lp){
    switch(message){
    case WM_TIMER:if(wp==kAnimationTimer){if(animating_)Tick();}else if(wp==kPollTimer)Poll();return 0;
    case WM_INPUT:WakeOnInput();break;  // DefWindowProc frees the raw input
    case kFrameMessage:
        if(animating_)Tick();
        if(pacer_)pacer_->Done();
        return 0;
    case WM_MOUSEACTIVATE:return MA_NOACTIVATE;
    case WM_ERASEBKGND:return 1;
    case WM_PAINT:ValidateRect(window_,nullptr);return 0;
    case WM_DISPLAYCHANGE:placed_=false;return 0;
    case WM_DPICHANGED:return 0;
    case WM_NCHITTEST:{
        POINT local{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};ScreenToClient(window_,&local);
        const float x=float(local.x),y=float(local.y);
        const bool hit=held_||model_.Hit(x,y)>=0||BarHit(x,y)>=0||(model_.visible&&model_.OnRope(x,y));
        return hit?HTCLIENT:HTTRANSPARENT;
    }
    case WM_SETCURSOR:
        if(LOWORD(lp)==HTCLIENT){
            POINT local{};GetCursorPos(&local);ScreenToClient(window_,&local);
            const bool hand=held_||model_.Hit(float(local.x),float(local.y))>=0||BarHit(float(local.x),float(local.y))>0;
            SetCursor(LoadCursorW(nullptr,hand?IDC_HAND:IDC_ARROW));return TRUE;
        }
        break;
    case WM_MOUSEMOVE:{
        const float x=float(GET_X_LPARAM(lp)),y=float(GET_Y_LPARAM(lp)),vx=x-float(last_mouse_.x);
        last_mouse_={GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
        if(!tracking_){TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,window_,0};tracking_=TrackMouseEvent(&track)!=FALSE;}
        if(held_){
            POINT screen{};GetCursorPos(&screen);
            const float dx=float(screen.x-held_->start.x),dy=float(screen.y-held_->start.y),distance=std::hypot(dx,dy);
            if(distance>5*model_.scale)held_->moved=true;
            const int i=model_.Find(held_->id);
            if(i>=0){model_.items[size_t(i)].pull_x=dx;model_.items[size_t(i)].pull_y=dy;}
            if(dy>115*model_.scale||distance>150*model_.scale){
                // Pulled off: the rope snaps back and the pin continues the drag.
                const auto id=held_->id;held_.reset();hover_=-1;
                model_.Remove(id);thumbs_.erase(id);Animate();UpdatePoll();
                if(callbacks_.detach)callbacks_.detach(id,screen);
                if(GetCapture()==window_)ReleaseCapture();
                return 0;
            }
            Animate();return 0;
        }
        const int hover=model_.Hit(x,y),bar=BarHit(x,y);
        if(hover!=hover_||bar!=bar_hover_){hover_=hover;bar_hover_=bar;Animate();}
        model_.Wind(x,y,vx);if(vx!=0)Animate();
        hide_at_=0;return 0;
    }
    case WM_MOUSELEAVE:tracking_=false;if(hover_>=0||bar_hover_>=0){hover_=bar_hover_=-1;Animate();}return 0;
    case WM_LBUTTONDOWN:{
        const float x=float(GET_X_LPARAM(lp)),y=float(GET_Y_LPARAM(lp));
        const int i=model_.Hit(x,y);
        if(i>=0){
            auto& item=model_.items[size_t(i)];item.held=true;item.pull_x=item.pull_y=0;
            POINT screen{};GetCursorPos(&screen);held_=Held{item.id,screen,GetTickCount64(),false};
            SetCapture(window_);Animate();return 0;
        }
        const int bar=BarHit(x,y);
        if(bar==1)RetrieveAll();else if(bar==2){hold_until_=0;Hide();}
        return 0;
    }
    case WM_LBUTTONUP:{
        if(!held_)return 0;
        const Held held=*held_;held_.reset();
        const int i=model_.Find(held.id);
        if(i>=0)model_.items[size_t(i)].held=false;
        if(GetCapture()==window_)ReleaseCapture();
        if(i>=0){
            if(!held.moved&&GetTickCount64()-held.time<450)Retrieve(held.id);
            else model_.items[size_t(i)].vdy-=2*model_.scale;
        }
        Animate();return 0;
    }
    case WM_CAPTURECHANGED:
        if(held_){const int i=model_.Find(held_->id);if(i>=0)model_.items[size_t(i)].held=false;held_.reset();Animate();}
        return 0;
    }
    return DefWindowProcW(window_,message,wp,lp);
}

void Clothesline::Render(){
    if(!window_||!factory_)return;
    const int width=std::max(1,int(std::lround(model_.width))),height=std::max(1,int(std::lround(model_.WindowHeight())));
    // The rope is 300 DIP tall: odd at 125/175/225 %. Only the real size is presented.
    const int surface_w=EvenSurfaceSize(width),surface_h=EvenSurfaceSize(height);
    if(!surface_||!target_||surface_width_!=surface_w||surface_height_!=surface_h){
        target_.Reset();brush_.Reset();ResetSprites();
        surface_=std::make_unique<DibSurface>(surface_w,surface_h);surface_width_=surface_w;surface_height_=surface_h;
        target_=CreateMemoryRenderTarget(factory_.Get(),surface_->Pixels(),surface_w,surface_h,surface_w);
        full_update_=true;
    }
    if(!target_)return;
    Draw(target_.Get());
    const int64_t blit=Qpc();
    // Only rows that held pixels last frame or hold pixels now are pushed to the
    // compositor; a 4K-wide rope window is mostly empty below the cards.
    const int bottom=std::clamp(int(std::ceil(content_bottom_))+2,1,height);
    RECT dirty{0,0,width,std::max(bottom,std::min(drawn_bottom_,height))};
    POINT origin{monitor_.left,monitor_.top},source{};SIZE size{width,height};BLENDFUNCTION blend{AC_SRC_OVER,0,255,AC_SRC_ALPHA};
    UPDATELAYEREDWINDOWINFO info{sizeof(info)};
    info.pptDst=&origin;info.psize=&size;info.hdcSrc=surface_->Dc();info.pptSrc=&source;info.pblend=&blend;info.dwFlags=ULW_ALPHA;
    info.prcDirty=full_update_?nullptr:&dirty;
    if(!UpdateLayeredWindowIndirect(window_,&info)&&info.prcDirty){info.prcDirty=nullptr;UpdateLayeredWindowIndirect(window_,&info);}
    full_update_=false;drawn_bottom_=bottom;
    cost_.blit=double(Qpc()-blit)/QpcPerMs();
}
void Clothesline::Label(ID2D1RenderTarget* target,const wchar_t* text,D2D1_RECT_F rect,D2D1_COLOR_F color){
    if(!text_format_||text_scale_!=model_.scale){
        text_format_.Reset();text_scale_=model_.scale;
        if(FAILED(text_factory_->CreateTextFormat(L"Microsoft YaHei UI",nullptr,DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,12.5f*model_.scale,L"zh-CN",text_format_.GetAddressOf())))return;
        text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);text_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        text_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    text_renderer_.Draw(target,text_factory_.Get(),text,text_format_.Get(),rect,color,false);
}
ID2D1Bitmap* Clothesline::ThumbBitmap(ID2D1RenderTarget* target,const ClotheslineItem& item){
    const auto found=thumbs_.find(item.id);if(found==thumbs_.end()||!found->second.image)return nullptr;
    auto& thumb=found->second;
    const int w=std::max(1,int(std::lround(item.tw))),h=std::max(1,int(std::lround(item.th)));
    if(thumb.bitmap&&std::abs(thumb.w-w)<=2&&std::abs(thumb.h-h)<=2)return thumb.bitmap.Get();
    // Mid-animation (the layout shrinks or grows as cards come and go) the
    // current bitmap is drawn scaled; re-sampling every card in one frame was a
    // visible hitch. The exact size is rebuilt on the settling frame.
    if(thumb.bitmap&&defer_thumbs_)return thumb.bitmap.Get();
    const Frame scaled=Downscale(*thumb.image,w,h);thumb.bitmap.Reset();++cost_.thumb_builds;
    for(auto& sprite:thumb.sprites)sprite={};
    const auto props=D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_IGNORE),96,96);
    if(FAILED(target->CreateBitmap(D2D1::SizeU(UINT(scaled.Width()),UINT(scaled.Height())),scaled.pixels.data(),UINT(scaled.Width())*4,props,thumb.bitmap.GetAddressOf())))return nullptr;
    thumb.w=w;thumb.h=h;return thumb.bitmap.Get();
}
ID2D1Bitmap* Clothesline::Bake(ID2D1RenderTarget* target,int width,int height,const std::function<void(ID2D1RenderTarget*)>& paint){
    // A compatible target shares resources (brush, thumbnails) with `target`.
    ComPtr<ID2D1BitmapRenderTarget> layer;
    if(width<=0||height<=0||FAILED(target->CreateCompatibleRenderTarget(D2D1::SizeF(float(width),float(height)),D2D1::SizeU(UINT(width),UINT(height)),
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),D2D1_COMPATIBLE_RENDER_TARGET_OPTIONS_NONE,layer.GetAddressOf())))return nullptr;
    layer->BeginDraw();layer->Clear(D2D1::ColorF(0,0.f));paint(layer.Get());
    if(FAILED(layer->EndDraw()))return nullptr;
    ID2D1Bitmap* bitmap{};
    return SUCCEEDED(layer->GetBitmap(&bitmap))?bitmap:nullptr;   // caller owns the reference
}
void Clothesline::PaintCard(ID2D1RenderTarget* target,const ClotheslineRect& r,ID2D1Bitmap* bitmap,float b,bool hover,uint64_t id,bool clip){
    const float s=model_.scale;
    for(int j=4;j>=1;--j){
        const float e=float(j)*1.6f*s;brush_->SetColor(D2D1::ColorF(0,hover?.07f:.055f));
        target->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.x-e*.5f,r.y+float(2+j)*s,r.x+r.w+e*.5f,r.y+r.h+float(2+j)*s+e*.5f),3*s+e,3*s+e),brush_.Get());
    }
    brush_->SetColor(D2D1::ColorF(0xfffdf8));
    target->FillRoundedRectangle(D2D1::RoundedRect(Rect(r),3*s,3*s),brush_.Get());
    if(bitmap)target->DrawBitmap(bitmap,D2D1::RectF(r.x+b,r.y+b,r.x+r.w-b,r.y+r.h-b),1.f,D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    if(clip){
        const auto& c=kClipColors[size_t(id%kClipColors.size())];
        const float cx=r.x+r.w/2,top=r.y-17*s;
        for(const float left:{cx-6.4f*s,cx+.4f*s}){
            const auto prong=D2D1::RoundedRect(D2D1::RectF(left,top,left+6*s,top+35*s),2.2f*s,2.2f*s);
            brush_->SetColor(D2D1::ColorF(c[0]));target->FillRoundedRectangle(prong,brush_.Get());
            brush_->SetColor(D2D1::ColorF(c[1]));target->DrawRoundedRectangle(prong,brush_.Get(),.9f*s);
        }
        brush_->SetColor(D2D1::ColorF(0xffffff,.45f));
        target->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cx-5.4f*s,top+1.5f*s,cx-3.8f*s,top+31.5f*s),.8f*s,.8f*s),brush_.Get());
        brush_->SetColor(D2D1::ColorF(0xa3acb7));
        target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx,top+15*s),3.8f*s,3.8f*s),brush_.Get(),1.7f*s);
    }
}
const Clothesline::Sprite* Clothesline::CardSprite(ID2D1RenderTarget* target,const ClotheslineItem& item,bool hover){
    auto* bitmap=ThumbBitmap(target,item);
    const auto found=thumbs_.find(item.id);if(!bitmap||found==thumbs_.end())return nullptr;
    auto& sprite=found->second.sprites[hover?1:0];
    if(sprite.bitmap)return &sprite;
    // Paper at whole-pixel offsets; margins hold the soft shadow below and the
    // clip above, plus 2 transparent pixels so rotated edges filter smoothly.
    const float s=model_.scale,b=model_.Border(),cw=float(found->second.w)+2*b,ch=float(found->second.h)+2*b;
    const float ox=std::ceil(std::max(3.2f*s,7*s-cw/2))+2,oy=std::ceil(17*s)+2;
    const int w=int(std::ceil(cw+2*ox)),h=int(std::ceil(oy+ch+9.2f*s))+2;
    const ClotheslineRect r{ox,oy,cw,ch};
    ++cost_.sprite_bakes;
    sprite.bitmap.Attach(Bake(target,w,h,[&](ID2D1RenderTarget* layer){PaintCard(layer,r,bitmap,b,hover,item.id,true);}));
    if(!sprite.bitmap)return nullptr;
    sprite.ox=ox;sprite.oy=oy;sprite.cw=cw;sprite.ch=ch;sprite.w=float(w);sprite.h=float(h);
    return &sprite;
}
float Clothesline::DrawItem(ID2D1RenderTarget* target,const ClotheslineItem& item,bool hover){
    const float s=model_.scale;
    const auto r=model_.FrameRect(item);float px{},py{};model_.Pivot(item,px,py);
    auto m=D2D1::Matrix3x2F::Identity();
    if(item.fly>=1){
        if(hover)m=D2D1::Matrix3x2F::Scale(1.06f,1.06f,D2D1::Point2F(r.x+r.w/2,r.y));
        m=m*D2D1::Matrix3x2F::Rotation(item.theta*57.2957795f,D2D1::Point2F(px,py));
    }
    // Lowest point of the card (paper, shadow, clip) after the transform, for
    // the dirty region.
    auto bottom=[&](const D2D1::Matrix3x2F& t,float x0,float y0,float x1,float y1){
        float y=-1e9f;
        for(const auto& p:{D2D1::Point2F(x0,y0),D2D1::Point2F(x1,y0),D2D1::Point2F(x0,y1),D2D1::Point2F(x1,y1)})y=std::max(y,t.TransformPoint(p).y);
        return y;
    };
    if(item.fly>=1){
        if(const auto* sprite=CardSprite(target,item,hover)){
            // One rotated blit of the pre-rendered card instead of shadows,
            // paper, picture and clip every frame.
            const float sx=r.w/sprite->cw,sy=r.h/sprite->ch;
            const auto t=D2D1::Matrix3x2F::Scale(sx,sy)*D2D1::Matrix3x2F::Translation(r.x-sprite->ox*sx,r.y-sprite->oy*sy)*m;
            target->SetTransform(t);
            target->DrawBitmap(sprite->bitmap.Get(),D2D1::RectF(0,0,sprite->w,sprite->h),1.f,D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            target->SetTransform(D2D1::Matrix3x2F::Identity());
            return bottom(t,0,0,sprite->w,sprite->h)+1;
        }
    }
    target->SetTransform(m);
    const float ratio=std::clamp(r.w/std::max(1.f,item.tw+2*model_.Border()),.05f,20.f);
    PaintCard(target,r,ThumbBitmap(target,item),model_.Border()*ratio,hover,item.id,item.fly>.7f);
    target->SetTransform(D2D1::Matrix3x2F::Identity());
    return bottom(m,r.x-7*s,r.y-18*s,r.x+r.w+7*s,r.y+r.h+10*s)+1;
}
void Clothesline::DrawRope(ID2D1RenderTarget* target){
    const float s=model_.scale,left=model_.Left(),right=model_.Right();
    auto& xs=rope_xs_;xs.clear();
    for(float x=left;x<right;x+=10*s)xs.push_back(x);
    xs.push_back(right);
    for(const auto& item:model_.items)if(item.fly>=1)xs.push_back(std::clamp(item.x+item.hx,left,right));
    for(const auto& ghost:model_.ghosts)xs.push_back(std::clamp(ghost.x,left,right));
    std::sort(xs.begin(),xs.end());xs.erase(std::unique(xs.begin(),xs.end()),xs.end());
    auto& ys=rope_ys_;ys.resize(xs.size());
    float lowest=-1e9f,highest=1e9f;
    for(size_t i=0;i<xs.size();++i){ys[i]=model_.RopeY(xs[i]);lowest=std::max(lowest,ys[i]);highest=std::min(highest,ys[i]);}
    content_bottom_=std::max(content_bottom_,lowest+10*s);
    auto same=[](const std::vector<float>& a,const std::vector<float>& b,float eps){
        if(a.size()!=b.size())return false;
        for(size_t i=0;i<a.size();++i)if(std::abs(a[i]-b[i])>eps)return false;
        return true;
    };
    const bool still=same(xs,rope_cache_xs_,0.f)&&same(ys,rope_last_ys_,.02f);
    rope_still_=still?rope_still_+1:0;rope_last_ys_=ys;
    if(rope_cache_&&(!same(xs,rope_cache_xs_,0.f)||!same(ys,rope_cache_ys_,.02f)))rope_cache_.Reset();
    // Five antialiased strokes along a 4K-wide polyline cost a few ms in the
    // software rasteriser; once the rope has held still for two frames (cards
    // swinging, rope settled) it is baked into a band bitmap and blitted.
    const float top=std::floor(highest-4*s)-2,bottom=std::ceil(lowest+10*s)+2;
    auto paint=[&](ID2D1RenderTarget* t,float dy){
        ComPtr<ID2D1PathGeometry> path;ComPtr<ID2D1GeometrySink> sink;
        if(FAILED(factory_->CreatePathGeometry(path.GetAddressOf()))||FAILED(path->Open(sink.GetAddressOf())))return;
        sink->BeginFigure(D2D1::Point2F(xs.front(),ys.front()+dy),D2D1_FIGURE_BEGIN_HOLLOW);
        for(size_t i=1;i<xs.size();++i)sink->AddLine(D2D1::Point2F(xs[i],ys[i]+dy));
        sink->EndFigure(D2D1_FIGURE_END_OPEN);sink->Close();
        t->SetTransform(D2D1::Matrix3x2F::Translation(0,6*s));
        brush_->SetColor(D2D1::ColorF(0,.12f));t->DrawGeometry(path.Get(),brush_.Get(),7*s,round_.Get());
        brush_->SetColor(D2D1::ColorF(0,.12f));t->DrawGeometry(path.Get(),brush_.Get(),3*s,round_.Get());
        t->SetTransform(D2D1::Matrix3x2F::Identity());
        brush_->SetColor(D2D1::ColorF(0x4d3b28));t->DrawGeometry(path.Get(),brush_.Get(),5.2f*s,round_.Get());
        brush_->SetColor(D2D1::ColorF(0xf1e7d4));t->DrawGeometry(path.Get(),brush_.Get(),3.2f*s,round_.Get());
        brush_->SetColor(D2D1::ColorF(0xb49c78));t->DrawGeometry(path.Get(),brush_.Get(),3.2f*s,twist_.Get());
    };
    if(!rope_cache_&&rope_still_>=2&&bottom>top){
        ++cost_.rope_bakes;
        rope_cache_.Attach(Bake(target,std::max(1,int(std::lround(model_.width))),int(bottom-top),[&](ID2D1RenderTarget* layer){paint(layer,-top);}));
        if(rope_cache_){rope_cache_xs_=xs;rope_cache_ys_=ys;rope_cache_top_=top;}
    }
    if(rope_cache_){
        const auto size=rope_cache_->GetPixelSize();
        target->DrawBitmap(rope_cache_.Get(),D2D1::RectF(0,rope_cache_top_,float(size.width),rope_cache_top_+float(size.height)),1.f,D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
        return;
    }
    if(!same(xs,rope_cache_xs_,0.f))rope_cache_xs_=xs;   // reference for the stillness test
    paint(target,0);
}
void Clothesline::Draw(ID2D1RenderTarget* target){
    const int64_t t0=Qpc();
    target->BeginDraw();target->SetTransform(D2D1::Matrix3x2F::Identity());
    // Rows below last frame's content are still transparent: clear only above.
    if(full_update_)target->Clear(D2D1::ColorF(0,0.f));
    else if(drawn_bottom_>0){
        target->PushAxisAlignedClip(D2D1::RectF(0,0,model_.width,float(drawn_bottom_)),D2D1_ANTIALIAS_MODE_ALIASED);
        target->Clear(D2D1::ColorF(0,0.f));target->PopAxisAlignedClip();
    }
    content_bottom_=0;
    const int64_t t1=Qpc();int64_t t2=t1;
    if(!brush_)target->CreateSolidColorBrush(D2D1::ColorF(0xffffff),brush_.GetAddressOf());
    bar_=bar_all_=bar_fold_=hint_rect_={};
    const float s=model_.scale,left=model_.Left(),right=model_.Right();
    if(brush_&&(model_.visible||model_.rope_y>model_.HiddenY()+1||!model_.ghosts.empty())){
        brush_->SetColor(D2D1::ColorF(0xffffff,.38f));
        for(const float x:{left,right})target->DrawLine(D2D1::Point2F(x,-2),D2D1::Point2F(x,model_.rope_y),brush_.Get(),1.2f*s);
        DrawRope(target);
        for(const float x:{left,right}){
            const auto hook=D2D1::Ellipse(D2D1::Point2F(x,model_.rope_y),5.5f*s,5.5f*s);
            brush_->SetColor(D2D1::ColorF(0xd6dbe3));target->FillEllipse(hook,brush_.Get());
            brush_->SetColor(D2D1::ColorF(0x6b7380));target->DrawEllipse(hook,brush_.Get(),1.5f*s);
            target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x,model_.rope_y),2*s,2*s),brush_.Get());
        }
        content_bottom_=std::max(content_bottom_,model_.rope_y+8*s);
        if(model_.preview){
            const auto slot=D2D1::RoundedRect(Rect(model_.preview_rect),6*s,6*s);
            brush_->SetColor(D2D1::ColorF(0x5b9cff,.2f));target->FillRoundedRectangle(slot,brush_.Get());
            brush_->SetColor(D2D1::ColorF(0xffffff,.85f));target->DrawRoundedRectangle(slot,brush_.Get(),2*s,dashed_.Get());
            Label(target,L"松手晾起",Rect(model_.preview_rect),D2D1::ColorF(0xffffff));
            content_bottom_=std::max(content_bottom_,model_.preview_rect.y+model_.preview_rect.h+3*s);
        }
        t2=Qpc();
        for(size_t i=0;i<model_.items.size();++i)content_bottom_=std::max(content_bottom_,DrawItem(target,model_.items[i],int(i)==hover_&&!held_));
        if(model_.items.empty()&&!model_.preview&&model_.rope_y>0){
            // Empty rope (opened from the top edge): say how to hang a pin.
            if(!hint_format_||hint_format_scale_!=s){
                hint_format_.Reset();hint_format_scale_=s;hint_sprite_.Reset();
                if(SUCCEEDED(text_factory_->CreateTextFormat(L"Microsoft YaHei UI",nullptr,DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,13*s,L"zh-CN",hint_format_.GetAddressOf()))){
                    hint_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);hint_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);hint_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                }
            }
            const wchar_t* hint=L"把钉图拖到这里晾起来，或在钉图上按 H";
            if(!hint_sprite_){
                // Baked once: text layout and rasterisation are the costliest
                // parts of this box, and it only moves with the rope.
                hint_w_=std::ceil(TextWidth(hint,hint_format_.Get())+28*s);
                const float hw=hint_w_,hh=std::ceil(34*s);
                hint_sprite_.Attach(Bake(target,int(hw)+4,int(hh)+4,[&](ID2D1RenderTarget* layer){
                    const auto box=D2D1::RoundedRect(D2D1::RectF(2,2,2+hw,2+hh),10*s,10*s);
                    brush_->SetColor(D2D1::ColorF(0x141826,.6f));layer->FillRoundedRectangle(box,brush_.Get());
                    brush_->SetColor(D2D1::ColorF(0xffffff,.5f));layer->DrawRoundedRectangle(box,brush_.Get(),1.5f*s,dashed_.Get());
                    if(hint_format_)text_renderer_.Draw(layer,text_factory_.Get(),hint,hint_format_.Get(),D2D1::RectF(2,2,2+hw,2+hh),D2D1::ColorF(0xffffff,.9f),false);
                }));
            }
            if(hint_sprite_){
                const auto size=hint_sprite_->GetPixelSize();
                const float hx=std::round(model_.width/2-hint_w_/2)-2,hy=std::round(model_.RopeY(model_.width/2)+22*s)-2;
                target->DrawBitmap(hint_sprite_.Get(),D2D1::RectF(hx,hy,hx+float(size.width),hy+float(size.height)),1.f,D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
                hint_rect_={hx+2,hy+2,hint_w_,std::ceil(34*s)};
                content_bottom_=std::max(content_bottom_,hy+float(size.height));
            }
        }
        if(!model_.items.empty()&&model_.rope_y>0){
            // Whole pixels so the baked labels stay crisp while the rope moves.
            const float w=(4+96+76+64+4)*s,h=30*s,x=std::round(model_.width/2-w/2),y=std::round(model_.rope_y-46*s);
            bar_={x,y,w,h};bar_all_={x+100*s,y+4*s,76*s,22*s};bar_fold_={x+176*s,y+4*s,64*s,22*s};
            if(!bar_sprite_||bar_sprite_count_!=model_.items.size()||bar_sprite_hover_!=bar_hover_){
                bar_sprite_count_=model_.items.size();bar_sprite_hover_=bar_hover_;
                const std::wstring title=L"晾着 "+std::to_wstring(model_.items.size())+L" 张";
                const ClotheslineRect all{bar_all_.x-x+2,bar_all_.y-y+2,bar_all_.w,bar_all_.h},fold{bar_fold_.x-x+2,bar_fold_.y-y+2,bar_fold_.w,bar_fold_.h};
                bar_sprite_.Attach(Bake(target,int(std::ceil(w))+4,int(std::ceil(h))+4,[&](ID2D1RenderTarget* layer){
                    const auto pill=D2D1::RoundedRect(D2D1::RectF(2,2,2+w,2+h),h/2,h/2);
                    brush_->SetColor(D2D1::ColorF(0x141826,.84f));layer->FillRoundedRectangle(pill,brush_.Get());
                    brush_->SetColor(D2D1::ColorF(0xffffff,.13f));layer->DrawRoundedRectangle(pill,brush_.Get(),s);
                    for(int i:{1,2}){
                        if(bar_hover_!=i)continue;
                        brush_->SetColor(D2D1::ColorF(0xffffff,.14f));layer->FillRoundedRectangle(D2D1::RoundedRect(Rect(i==1?all:fold),11*s,11*s),brush_.Get());
                    }
                    Label(layer,title.c_str(),D2D1::RectF(2+4*s,2,2+100*s,2+h),D2D1::ColorF(0xffffff,.9f));
                    Label(layer,L"全部取下",Rect(all),D2D1::ColorF(0xffffff));
                    Label(layer,L"收起",Rect(fold),D2D1::ColorF(0xffffff));
                }));
            }
            if(bar_sprite_){
                const auto size=bar_sprite_->GetPixelSize();
                target->DrawBitmap(bar_sprite_.Get(),D2D1::RectF(x-2,y-2,x-2+float(size.width),y-2+float(size.height)),1.f,D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
            }
        }
    }
    const HRESULT drawn=target->EndDraw();
    // Software targets rasterise lazily: EndDraw flushes the batched work, so
    // its time is attributed to the rest of the frame (labels, bar, flush).
    const double per_ms=QpcPerMs();
    cost_.clear=double(t1-t0)/per_ms;cost_.rope=double(t2-t1)/per_ms;cost_.cards=double(Qpc()-t2)/per_ms;
    if(drawn==D2DERR_RECREATE_TARGET){target_.Reset();brush_.Reset();ResetSprites();full_update_=true;}
}
}