// Exercise the actual modal window and scoped keyboard hook with synthetic input.
#include "../src/app/settings_dialog.cpp"
#include "../src/app/settings_process.cpp"
#include "capture/frame.h"
#include "export/png.h"
#include "app/launch_options.h"
#include <iostream>
#include <chrono>
#include <algorithm>
#include <vector>
#include <fstream>
using namespace lumashot;
namespace {
int failures{},scenario{},phase{};bool callback_called{},input_blocked{};
void Expect(bool ok,const char* name){std::cout<<(ok?"PASS ":"FAIL ")<<name<<std::endl;failures+=!ok;}
void Key(Settings& s,UINT key,bool down){s.Input(key,down);Expect(input_blocked,"capture remains blocked during recording and key release");Expect(s.hook!=nullptr,"hook stays scoped to active recording until release");}
void Finish(Settings& s){SendMessageW(s.window,Finished,0,0);Expect(!s.hook&&!s.recording&&!s.draining&&!input_blocked,"hook removed after complete chord");}
// 恢复本页默认 on one page: the first click only arms the confirmation, the second restores.
void ResetPage(Settings& s,int page){
    SendMessageW(s.window,WM_COMMAND,NavIds[page],0);SendMessageW(s.window,WM_COMMAND,113,0);
    Expect(s.reset_armed&&s.page==page,"first click on restore page defaults asks for confirmation");
    SendMessageW(s.window,WM_COMMAND,113,0);Expect(!s.reset_armed,"second click restores the page");
}
void Snapshot(Settings& s,const wchar_t* file,float scale){
    RECT previous{};GetWindowRect(s.window,&previous);const float old=s.scale;
    std::array<RECT,std::size(Controls)> rects{};const auto& ids=Controls;
    for(int i=0;i<static_cast<int>(std::size(ids));++i){GetWindowRect(GetDlgItem(s.window,ids[i]),&rects[i]);MapWindowPoints(nullptr,s.window,reinterpret_cast<POINT*>(&rects[i]),2);}
    SetWindowPos(s.window,nullptr,0,0,int(PanelWidth*scale),int(PanelHeight*scale),SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);s.scale=scale;
    for(int i=0;i<static_cast<int>(std::size(ids));++i){const auto r=rects[i];SetWindowPos(GetDlgItem(s.window,ids[i]),nullptr,int(r.left/old*scale),int(r.top/old*scale),int((r.right-r.left)/old*scale),int((r.bottom-r.top)/old*scale),SWP_NOZORDER|SWP_NOACTIVATE);}
    s.RenderSurface();auto frame=MakeFrame({0,0,s.surface_width,s.surface_height});
    const auto* pixels=s.surface->Pixels();Expect((pixels[0]>>24)>0,"production surface reaches the corner (DWM rounds the window; no transparent wedge under the system corner)");
    const auto hasInk=[&](RECT bounds,UINT32 ink){
        unsigned count=0;
        for(int y=std::max(0L,bounds.top);y<std::min(LONG(frame.Height()),bounds.bottom);++y)
            for(int x=std::max(0L,bounds.left);x<std::min(LONG(frame.Width()),bounds.right);++x){
                // Small antialiased glyphs need not contain fully covered pixels.
                // Unpremultiply and require the sample to be closer to ink than
                // the panel background (at least half foreground coverage).
                const auto pixel=pixels[y*frame.Width()+x];const UINT alpha=pixel>>24;if(alpha<64)continue;
                int foreground_distance=0,background_distance=0;
                for(UINT shift:{0u,8u,16u}){const int channel=int(std::min(255u,(((pixel>>shift)&255)*255+alpha/2)/alpha));
                    foreground_distance+=std::abs(channel-int((ink>>shift)&255));
                    background_distance+=std::abs(channel-int((PanelBackground(s.material_dark)>>shift)&255));}
                if(foreground_distance<background_distance)++count;
            }
        return count>10;
    };
    const UINT32 ink=s.material_dark?0xedf4ff:0x243142;
    Expect(hasInk({LONG(60*scale),LONG(18*scale),LONG(220*scale),LONG(48*scale)},ink),"LumaText paints settings title at current DPI");
    for(int id:{103,IDCANCEL}){
        if(id==103&&s.page!=1)continue;
        RECT bounds{};GetWindowRect(GetDlgItem(s.window,id),&bounds);MapWindowPoints(nullptr,s.window,reinterpret_cast<POINT*>(&bounds),2);
        Expect(hasInk(bounds,ink),"LumaText paints translated shortcut and footer control text");
    }
    if(s.acrylic)Expect((pixels[int(185*scale)*frame.Width()+int(20*scale)]>>24)<255,"acrylic background retains alpha for DWM blur");
    // Synthetic backdrop only; no personal desktop capture. Shows the real alpha
    // surface over a smooth colored background, not a claim about DWM blur output.
    for(int y=0;y<frame.Height();++y)for(int x=0;x<frame.Width();++x){const auto source=pixels[y*frame.Width()+x];const UINT inverse=255-(source>>24);
        const UINT red=180+UINT(x*45/frame.Width()),green=195+UINT(y*35/frame.Height()),blue=225;
        const uint32_t bg=(red<<16)|(green<<8)|blue;uint32_t out=0xff000000;
        for(UINT shift:{0u,8u,16u})out|=std::min(255u,((source>>shift)&255)+((((bg>>shift)&255)*inverse+127)/255))<<shift;
        frame.pixels[y*frame.Width()+x]=out;
    }
    SavePng(frame,file);SetWindowPos(s.window,nullptr,0,0,previous.right-previous.left,previous.bottom-previous.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);s.Layout();
}
void Pump(){MSG m;while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}}
// Retained frame: moves and exposure render nothing, control-state changes repaint only
// that control, and the partial result is pixel-identical to a full redraw.
void RetainedFrame(Settings& s,HWND w){
    ShowWindow(w,SW_SHOWNA);SetWindowPos(w,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);Pump();
    RECT home{};GetWindowRect(w,&home);MONITORINFO mi{sizeof(mi)};GetMonitorInfoW(MonitorFromWindow(w,MONITOR_DEFAULTTONEAREST),&mi);
    auto settled=[&]{Pump();s.profile={};return s.presents;};
    unsigned before=settled();
    for(int i=0;i<40;++i){SetWindowPos(w,nullptr,mi.rcMonitor.left+40-i*8,home.top,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);Pump();}
    for(int i=0;i<40;++i){SetWindowPos(w,nullptr,mi.rcMonitor.left-280+i*8,home.top,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);Pump();}
    for(int i=0;i<30;++i){SetWindowPos(w,nullptr,home.left+i*4,home.top+i*2,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);Pump();}
    std::cout<<"Settings drag frames: full="<<s.profile.frames<<" partial="<<s.profile.partials<<" presents="<<s.presents-before<<std::endl;
    // The cursor is not captured here, so a button sliding under it may legitimately take hover (a partial frame).
    Expect(s.profile.frames==0&&s.profile.partials<=4,"dragging the window, including back from off-screen, renders no full frames");
    SetWindowPos(w,nullptr,home.left,home.top,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
    before=settled();InvalidateRect(GetDlgItem(w,IDOK),nullptr,FALSE);InvalidateRect(w,nullptr,FALSE);Pump();
    Expect(s.profile.frames==0&&s.profile.partials==0,"exposed controls with an unchanged look do not repaint");
    before=settled();SendMessageW(w,WM_ACTIVATE,WA_ACTIVE,0);Pump();
    Expect(s.profile.frames==0&&s.profile.partials==0,"activation without a translation change renders nothing");
    before=settled();
    std::vector<double> hover_ms;
    for(int i=0;i<20;++i){const double spent=s.profile.partial_ms;SendMessageW(GetDlgItem(w,i%2?IDOK:IDCANCEL),WM_MOUSEMOVE,0,MAKELPARAM(5,5));Pump();hover_ms.push_back(s.profile.partial_ms-spent);}
    std::sort(hover_ms.begin(),hover_ms.end());
    std::cout<<"Settings hover frame ms: median "<<hover_ms[hover_ms.size()/2]<<" min "<<hover_ms.front()<<" (partials="<<s.profile.partials<<")"<<std::endl;
    Expect(s.profile.frames==0&&s.profile.partials>=20&&s.presents-before>=20,"hover repaints only the hovered controls");
    const std::vector<uint32_t> partial(s.surface->Pixels(),s.surface->Pixels()+static_cast<size_t>(s.surface_width)*static_cast<size_t>(s.surface_height));
    s.RenderSurface();GdiFlush();
    // Same shapes and colours; LumaText's glyph edges may move by a few levels with the
    // absolute float origin (measured max 9/255 on text edges only).
    {size_t differ=0;unsigned worst=0;
     for(size_t i=0;i<partial.size();++i)if(partial[i]!=s.surface->Pixels()[i]){++differ;for(unsigned shift:{0u,8u,16u,24u}){const int x=static_cast<int>((partial[i]>>shift)&255),y=static_cast<int>((s.surface->Pixels()[i]>>shift)&255);worst=std::max(worst,static_cast<unsigned>(std::abs(x-y)));}}
     std::cout<<"Settings partial vs full: "<<differ<<" px differ, max channel delta "<<worst<<std::endl;
     Expect(worst<=16&&differ<static_cast<size_t>(s.surface_width)*8,"partial control frame matches a full redraw (glyph edges only)");}
    before=settled();SetFocus(GetDlgItem(w,IDCANCEL));Pump();
    Expect(s.profile.frames==0,"focus moves repaint controls, not the page");
    before=settled();SendMessageW(w,WM_COMMAND,127,0);Pump();Expect(s.profile.frames==1,"a setting change renders one full frame");SendMessageW(w,WM_COMMAND,127,0);Pump();
    s.hover=0;s.Refresh();SetWindowPos(w,HWND_NOTOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);Pump();
}
void CALLBACK Drive(HWND,UINT,UINT_PTR timer,DWORD){
    HWND w=GetActiveWindow();if(!w||!GetDlgItem(w,103))return;KillTimer(nullptr,timer);
    auto& s=*reinterpret_cast<Settings*>(GetWindowLongPtrW(w,DWLP_USER));
    Expect(s.prepared_before_show&&s.presents>0,"complete frame uploaded before dialog is shown");
    SetForegroundWindow(w);    Expect(s.Present(),"real layered window accepts complete alpha surface");
    RECT outer{},client{};GetWindowRect(w,&outer);GetClientRect(w,&client);POINT origin{};ClientToScreen(w,&origin);
    Expect(!(GetWindowLongPtrW(w,GWL_STYLE)&(WS_CAPTION|WS_BORDER))&&(GetWindowLongPtrW(w,GWL_EXSTYLE)&WS_EX_NOREDIRECTIONBITMAP),"actual dialog has no system caption or border (WS_THICKFRAME only for the DWM backdrop, zero frame)");
    Expect(origin.x==outer.left&&origin.y==outer.top&&client.right==outer.right-outer.left&&client.bottom==outer.bottom-outer.top,"actual client and window extents agree");
    for(int id:{142,143,148,144,145,146,147,128,115,116,117,118,121,122,123,124,125,126,132,129,127,130,131,113,IDCANCEL,IDOK}){RECT r{};GetWindowRect(GetDlgItem(w,id),&r);Expect(r.left>=outer.left&&r.right<=outer.right&&r.top>=outer.top&&r.bottom<=outer.bottom,"file paste and footer controls fully inside real window bounds");}
    if(scenario==4){
        if(phase++==0){
            const DWORD foreground=GetWindowThreadProcessId(GetForegroundWindow(),nullptr),current=GetCurrentThreadId();
            const bool attached=foreground!=current&&AttachThreadInput(current,foreground,TRUE);SetForegroundWindow(w);if(attached)AttachThreadInput(current,foreground,FALSE);
            if(GetForegroundWindow()!=w)std::cout<<"SKIP real SendInput integration: runner has no foreground input desktop; synthetic input state tests completed"<<std::endl;
            if(GetForegroundWindow()!=w){SendMessageW(w,WM_COMMAND,IDCANCEL,0);return;}
            SendMessageW(w,WM_COMMAND,103,0);
            INPUT keys[6]{};const WORD codes[]={VK_LWIN,VK_LCONTROL,VK_F11,VK_F11,VK_LCONTROL,VK_LWIN};
            for(int i=0;i<6;++i){keys[i].type=INPUT_KEYBOARD;keys[i].ki.wVk=codes[i];keys[i].ki.dwFlags=i>=3?KEYEVENTF_KEYUP:0;}
            Expect(SendInput(6,keys,sizeof(INPUT))==6,"synthetic Win chord sent through real hook");SetTimer(nullptr,0,100,Drive);return;
        }
        Expect(s.draft.modifiers==(MOD_WIN|MOD_CONTROL)&&s.draft.key==VK_F11&&!s.hook,"real hook records Win chord and releases after key-up");
        SendMessageW(w,WM_COMMAND,IDCANCEL,0);
    }else if(scenario==0){
        const auto count=s.formats.size();const auto* surface=s.surface.get();
        const auto render_start=std::chrono::steady_clock::now();
        std::vector<double> full_ms;
        for(int i=0;i<21;++i){const auto one=std::chrono::steady_clock::now();s.RenderSurface();full_ms.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-one).count());}
        std::cout<<"Settings full-frame average ms: "<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-render_start).count()/21<<std::endl;
        std::sort(full_ms.begin(),full_ms.end());std::cout<<"Settings full-frame ms: median "<<full_ms[full_ms.size()/2]<<" min "<<full_ms.front()<<std::endl;
        Expect(s.formats.size()==count&&s.surface.get()==surface,"full redraw reuses text formats and a single surface");
        RetainedFrame(s,w);
        for(int id:ShortcutControls){RECT r{};GetWindowRect(GetDlgItem(w,id),&r);MapWindowPoints(nullptr,w,reinterpret_cast<POINT*>(&r),2);Expect(r.bottom<int(FooterTop*s.scale),"all seven shortcuts fit above footer");}
        Expect(!s.draft.ocr_remove_whitespace,"OCR whitespace removal defaults off");SendMessageW(w,WM_COMMAND,144,0);Expect(s.draft.ocr_remove_whitespace,"OCR whitespace switch updates draft");SendMessageW(w,WM_COMMAND,144,0);
        Expect(!s.SetOcrModel(std::filesystem::current_path()/L"missing-model"),"missing OCR model files are rejected");
        SendMessageW(w,WM_COMMAND,146,0);Expect(s.draft.ocr_model_directory.empty(),"restore built-in OCR clears model directory");
        Expect(s.draft.start_with_windows,"login startup defaults on");SendMessageW(w,WM_COMMAND,127,0);Expect(!s.draft.start_with_windows,"startup switch disables draft");SendMessageW(w,WM_COMMAND,127,0);Expect(s.draft.start_with_windows,"startup switch enables draft");
        Expect(!s.draft.clipboard_enabled,"clipboard history defaults off");SendMessageW(w,WM_COMMAND,126,0);Expect(s.draft.clipboard_enabled,"clipboard switch enables draft");SendMessageW(w,WM_COMMAND,126,0);Expect(!s.draft.clipboard_enabled,"clipboard switch disables draft");
        Expect(!s.draft.clipboard_persist,"clipboard history persistence defaults off");SendMessageW(w,WM_COMMAND,129,0);Expect(s.draft.clipboard_persist,"persistence switch enables draft");ResetPage(s,4);Expect(!s.draft.clipboard_persist,"restore page defaults turns persistence off");
        Expect(!s.draft.clipboard_enabled&&!IsWindowEnabled(GetDlgItem(w,132)),"strip switch is disabled while the clipboard is off");
        SendMessageW(w,WM_COMMAND,126,0);Expect(IsWindowEnabled(GetDlgItem(w,132)),"enabling the clipboard enables the strip switch");SendMessageW(w,WM_COMMAND,126,0);
        Expect(s.draft.clipboard_strip_visible,"clipboard strip defaults visible");SendMessageW(w,WM_COMMAND,132,0);Expect(!s.draft.clipboard_strip_visible,"strip switch hides draft strip");
        ResetPage(s,4);Expect(s.draft.clipboard_strip_visible,"restore page defaults shows the strip again");
        {RECT a{},b{};GetWindowRect(GetDlgItem(w,126),&a);GetWindowRect(GetDlgItem(w,132),&b);RECT c{};GetWindowRect(GetDlgItem(w,129),&c);Expect(a.bottom<=c.top&&c.bottom<=b.top,"history switches come first, the strip group below");}
        {SendMessageW(w,WM_COMMAND,NavIds[4],0);const auto shown=[&](int id){return (GetWindowLongPtrW(GetDlgItem(w,id),GWL_STYLE)&WS_VISIBLE)!=0;};
         Expect(shown(LimitSegFirst+3)&&shown(StyleSegFirst+1)&&shown(CardPreviewToggle)&&!shown(131)&&!shown(HomepageButton),"clipboard page shows the limit, style and preview controls");
         {RECT last{};GetWindowRect(GetDlgItem(w,CardPreviewToggle),&last);MapWindowPoints(nullptr,w,reinterpret_cast<POINT*>(&last),2);Expect(last.bottom<int(FooterTop*s.scale),"clipboard page fits above the footer");}
         Expect(s.draft.clipboard_max_entries==100,"history limit defaults to 100");SendMessageW(w,WM_COMMAND,LimitSegFirst,0);Expect(s.draft.clipboard_max_entries==20,"limit segment updates the draft");
         Expect(!IsWindowEnabled(GetDlgItem(w,StyleSegFirst))&&!IsWindowEnabled(GetDlgItem(w,CardPreviewToggle)),"strip options are disabled while the clipboard is off");
         SendMessageW(w,WM_COMMAND,StyleSegFirst+1,0);Expect(!s.draft.clipboard_classic_strip,"a disabled style segment cannot change the draft");
         SendMessageW(w,WM_COMMAND,126,0);Expect(IsWindowEnabled(GetDlgItem(w,StyleSegFirst+1))&&IsWindowEnabled(GetDlgItem(w,CardPreviewToggle)),"strip options follow the clipboard switch");
         SendMessageW(w,WM_COMMAND,CardPreviewToggle,0);Expect(!s.draft.clipboard_card_preview,"preview switch updates the draft");
         Snapshot(s,L"settings-clipboard-custom.png",1);
         SendMessageW(w,WM_COMMAND,StyleSegFirst+1,0);Expect(s.draft.clipboard_classic_strip&&!IsWindowEnabled(GetDlgItem(w,CardPreviewToggle)),"classic strip disables the card preview switch");
         Snapshot(s,L"settings-clipboard-classic.png",1);
         SendMessageW(w,WM_COMMAND,113,0);Expect(s.reset_armed&&s.draft.clipboard_classic_strip&&s.draft.clipboard_max_entries==20,"first reset click changes nothing yet");
         Snapshot(s,L"settings-reset-confirm.png",1);
         SendMessageW(w,WM_COMMAND,NavIds[4],0);Expect(!s.reset_armed&&s.draft.clipboard_max_entries==20,"navigating cancels the pending reset");
         SendMessageW(w,WM_COMMAND,113,0);SendMessageW(w,WM_COMMAND,LimitSegFirst+1,0);Expect(!s.reset_armed&&s.draft.clipboard_max_entries==50,"any other click cancels the pending reset");
         s.draft.theme=2;ResetPage(s,4);
         Expect(s.draft.clipboard_max_entries==100&&!s.draft.clipboard_classic_strip&&s.draft.clipboard_card_preview&&!s.draft.clipboard_enabled&&s.draft.theme==2,"restore page defaults resets only the clipboard page");
         s.draft.theme=0;SendMessageW(w,WM_COMMAND,NavIds[7],0);Expect(shown(HomepageButton)&&shown(NoticesButton)&&shown(131)&&!shown(113),"about page shows the links and hides the page reset");
         SendMessageW(w,WM_COMMAND,NavIds[4],0);Expect(shown(113),"page reset returns on other pages");}
        Expect(s.draft.pin_style==DefaultPinStyle,"sticker default is simple border and shadow");
        for(int id=121;id<=125;++id){SendMessageW(w,WM_COMMAND,id,0);Expect(static_cast<int>(s.draft.pin_style)==id-121,"each sticker choice updates draft");}
        SendMessageW(w,WM_COMMAND,124,0);
        Expect(s.draft.paste_as_file&&s.draft.paste_file_format==0,"file paste defaults to enabled PNG");
        SendMessageW(w,WM_COMMAND,117,0);Expect(s.draft.paste_file_format==1,"JPEG selection updates draft");
        SendMessageW(w,WM_COMMAND,115,0);Expect(!s.draft.paste_as_file&&!IsWindowEnabled(GetDlgItem(w,116))&&!IsWindowEnabled(GetDlgItem(w,117))&&!IsWindowEnabled(GetDlgItem(w,118)),"disabling file paste disables all format controls");
        SendMessageW(w,WM_COMMAND,118,0);Expect(s.draft.paste_file_format==1,"disabled format cannot change draft");
        SendMessageW(w,WM_COMMAND,137,0);{const auto shown=[&](int id){return (GetWindowLongPtrW(GetDlgItem(w,id),GWL_STYLE)&WS_VISIBLE)!=0;};Expect(s.page==2&&shown(115)&&shown(116)&&!shown(103)&&!shown(126)&&shown(135)&&shown(IDOK),"sidebar shows one page at a time");}
        Snapshot(s,L"settings-file-disabled.png",1);
        SendMessageW(w,WM_COMMAND,115,0);SendMessageW(w,WM_COMMAND,118,0);Expect(s.draft.paste_as_file&&s.draft.paste_file_format==2&&IsWindowEnabled(GetDlgItem(w,118)),"reenabling file paste permits BMP selection");
        {const auto pages=[&](const wchar_t* prefix){for(int page=0;page<PageCount;++page){SendMessageW(w,WM_COMMAND,NavIds[page],0);Snapshot(s,(std::wstring(prefix)+std::to_wstring(page)+L".png").c_str(),1);}};
         SendMessageW(w,WM_COMMAND,111,0);pages(L"settings-light-p");SendMessageW(w,WM_COMMAND,112,0);Expect(s.draft.theme==2,"dark theme preview updates draft");pages(L"settings-dark-p");}
        SendMessageW(w,WM_COMMAND,137,0);Expect(s.tr_name==L"DeepSeek"&&!s.tr_ready,"translation page summarises the isolated engine config");
        Expect(s.draft.translate_auto_show,"translated pin switch defaults on");SendMessageW(w,WM_COMMAND,141,0);Expect(!s.draft.translate_auto_show,"translated pin switch toggles draft");SendMessageW(w,WM_COMMAND,141,0);
        SendMessageW(w,WM_COMMAND,140,0);{const auto shown=[&](int id){return (GetWindowLongPtrW(GetDlgItem(w,id),GWL_STYLE)&WS_VISIBLE)!=0;};RECT r{};GetWindowRect(GetDlgItem(w,147),&r);MapWindowPoints(nullptr,w,reinterpret_cast<POINT*>(&r),2);
         Expect(s.page==5&&shown(147)&&shown(121)&&!shown(141)&&!shown(110)&&r.bottom<int(FooterTop*s.scale),"clothesline switch sits on the sticker page above the footer");}
        Expect(s.draft.clothesline_enabled,"clothesline switch defaults on");SendMessageW(w,WM_COMMAND,147,0);Expect(!s.draft.clothesline_enabled,"clothesline switch toggles draft");
        Snapshot(s,L"settings-clothesline-off.png",1);SendMessageW(w,WM_COMMAND,147,0);Expect(s.draft.clothesline_enabled,"clothesline switch toggles back on");
        SendMessageW(w,WM_COMMAND,135,0);Snapshot(s,L"settings-dark-150.png",1.5f);
        {const auto shown=[&](int id){return (GetWindowLongPtrW(GetDlgItem(w,id),GWL_STYLE)&WS_VISIBLE)!=0;};
         const auto rect_of=[&](int id){RECT r{};GetWindowRect(GetDlgItem(w,id),&r);MapWindowPoints(nullptr,w,reinterpret_cast<POINT*>(&r),2);return r;};
         SendMessageW(w,WM_COMMAND,138,0);
         Expect(s.page==3&&shown(ChipIds.front())&&shown(ChipIds.back())&&shown(ChipControl(13))&&shown(LabelSegFirst)&&shown(AutoPropertiesToggle)&&!shown(126)&&!shown(103),"tool panel page shows chips and display options only");
         Expect(s.draft.toolbar==ToolbarCustomization{},"tool panel starts from the saved default layout");
         SendMessageW(w,WM_COMMAND,ChipControl(6),0);
         Expect(!s.draft.toolbar.OnPanel(6)&&s.draft.toolbar.overflow==std::vector<int>{6}&&rect_of(ChipControl(6)).top>=int(ZoneTopMore*s.scale),"clicking a panel chip moves it into the more zone");
         SendMessageW(w,WM_COMMAND,ChipControl(6),0);Expect(s.draft.toolbar.OnPanel(6)&&s.draft.toolbar.overflow.empty(),"clicking a more chip pins it back");
         s.MoveChip(6,true,1);Expect(s.draft.toolbar.OnPanel(6)&&s.draft.toolbar.order.front()==6,"dropping before the rectangle makes mosaic the first panel tool");
         s.ShiftChip(6,1);Expect(s.draft.toolbar.order[0]==1&&s.draft.toolbar.order[1]==6,"ctrl+arrow swaps a chip with its neighbour");
         s.ShiftChip(1,-1);Expect(s.draft.toolbar.order[0]==1,"ctrl+arrow stops at the zone edge");
         {const auto chips=s.ZoneChips(true);bool panel{};int before{};const auto r=chips[2].second;
          Expect(chips[0].first==s.draft.toolbar.order.front()&&s.DropTarget({r.left+2,(r.top+r.bottom)/2},panel,before)&&panel&&before==chips[2].first,"drop target picks the chip under the pointer");
          Expect(s.DropTarget({ContentRight-20,ZoneTopMore+ZoneHeight/2},panel,before)&&!panel&&before==-1,"empty more zone accepts drops at its end");
          Expect(!s.DropTarget({ContentLeft+40,ToolbarDisplayTop+30},panel,before),"drops outside both zones are rejected");}
         for(int id:{14,17,16})s.MoveChip(id,false,-1);s.Layout();
         Expect(s.draft.toolbar.overflow==std::vector<int>{14,17,16},"more zone keeps drop order");
         const auto chips_fit=[&]{bool fits=true;for(size_t ci=0;ci<ChipIds.size();++ci){const int id=ChipIds[ci];const auto r=rect_of(id);const bool panel=s.draft.toolbar.OnPanel(ChipEntry(id));const float top=panel?ZoneTopPanel:ZoneTopMore;
             fits=fits&&r.left>=int(ContentLeft*s.scale)&&r.right<=int(ContentRight*s.scale)+1&&r.top>=int(top*s.scale)&&r.bottom<=int((top+ZoneHeight)*s.scale)+1;
             for(size_t cj=ci+1;cj<ChipIds.size();++cj){RECT overlap{};const auto a=rect_of(id),b=rect_of(ChipIds[cj]);fits=fits&&!IntersectRect(&overlap,&a,&b);}}return fits;};
         Expect(chips_fit(),"chips stay inside their zone without overlapping");
         {const auto kept_layout=s.draft.toolbar;s.draft.toolbar.overflow.clear();s.Layout();Expect(chips_fit(),"all twelve chips fit the panel zone in two rows");
          for(int chip_entry:CustomizableToolbarIds)s.draft.toolbar.SetOnPanel(chip_entry,false);s.Layout();Expect(chips_fit(),"all twelve chips fit the more zone in two rows");
          s.draft.toolbar=kept_layout;s.Layout();}
         SendMessageW(w,WM_COMMAND,LabelSegFirst+2,0);Expect(s.draft.toolbar.labels==ToolbarLabels::Icons,"button text segment updates draft");
         SendMessageW(w,WM_COMMAND,AutoPropertiesToggle,0);Expect(!s.draft.toolbar.auto_properties,"auto-expand switch updates draft");
         Expect(shown(PropertyStyleSegFirst)&&shown(PropertyStyleSegFirst+1)&&s.draft.toolbar.property_style==PropertyBarStyle::Strip,"property style segment shows, strip by default");
         SendMessageW(w,WM_COMMAND,PropertyStyleSegFirst+1,0);Expect(s.draft.toolbar.property_style==PropertyBarStyle::Card,"property style segment switches to the card");
         Expect(rect_of(PropertyStyleSegFirst).bottom<=rect_of(AutoPropertiesToggle).top&&rect_of(LabelSegFirst).bottom<=rect_of(PropertyStyleSegFirst).top&&rect_of(PropertyStyleSegFirst).right<rect_of(PropertyStyleSegFirst+1).left,"display rows stack without overlap");
         Expect(rect_of(AutoPropertiesToggle).bottom<int(FooterTop*s.scale),"tool panel options sit above the footer");
         Snapshot(s,L"settings-toolbar-custom-dark.png",1);SendMessageW(w,WM_COMMAND,111,0);Snapshot(s,L"settings-toolbar-custom-light.png",1);Snapshot(s,L"settings-toolbar-custom-150.png",1.5f);SendMessageW(w,WM_COMMAND,112,0);
         SendMessageW(w,WM_COMMAND,LabelSegFirst,0);SendMessageW(w,WM_COMMAND,AutoPropertiesToggle,0);SendMessageW(w,WM_COMMAND,PropertyStyleSegFirst,0);}
        SendMessageW(w,WM_COMMAND,103,0);Expect(s.recording&&s.hook,"recording installs hook on demand");
        Key(s,VK_LWIN,true);Key(s,VK_LCONTROL,true);Key(s,VK_F11,true);Expect(s.draft.modifiers==(MOD_WIN|MOD_CONTROL)&&s.draft.key==VK_F11,"Win plus Ctrl plus F11 captured");
        Key(s,VK_F11,false);Key(s,VK_LCONTROL,false);Key(s,VK_LWIN,false);Finish(s);
        SendMessageW(w,WM_COMMAND,IDOK,0);
    }else if(scenario==1){
        const auto mods=s.draft.modifiers,key=s.draft.key;SendMessageW(w,WM_COMMAND,103,0);Key(s,VK_LWIN,true);Key(s,VK_ESCAPE,true);Key(s,VK_ESCAPE,false);Key(s,VK_LWIN,false);Finish(s);
        Expect(s.draft.modifiers==mods&&s.draft.key==key,"Escape restores shortcut and leaves dialog open");
        SendMessageW(w,WM_COMMAND,103,0);Key(s,'B',true);Key(s,'B',false);Expect(s.recording&&s.draft.key==key,"unmodified letter rejected");
        Key(s,VK_LCONTROL,true);Key(s,VK_F12,true);Key(s,VK_F12,false);Expect(s.recording&&s.draft.key==key,"reserved F12 rejected");
        Key(s,VK_LCONTROL,false);Key(s,VK_F1,true);Expect(!s.recording&&s.draining&&s.draft.key==VK_F1&&s.draft.modifiers==0,"standalone F1 accepted without modifier");
        Key(s,VK_F1,false);Finish(s);Expect(ShortcutLabel(s.draft.modifiers,s.draft.key)==L"F1","standalone shortcut label has no modifier prefix");
        SendMessageW(w,WM_COMMAND,103,0);Key(s,VK_LCONTROL,true);
        SendMessageW(w,WM_ACTIVATE,WA_INACTIVE,0);Expect(!s.hook&&!s.recording&&!input_blocked&&s.draft.key==VK_F1&&s.draft.modifiers==0,"focus loss removes keyboard hook and keeps committed standalone key");
        SendMessageW(w,WM_COMMAND,125,0);SendMessageW(w,WM_COMMAND,101,0);SendMessageW(w,WM_COMMAND,IDCANCEL,0);
    }else if(scenario==2){
        bool registered=RegisterHotKey(nullptr,88,MOD_CONTROL|MOD_ALT|MOD_SHIFT,VK_F10)!=FALSE;Expect(registered,"synthetic conflict registered");
        s.draft.modifiers=MOD_CONTROL|MOD_ALT|MOD_SHIFT;s.draft.key=VK_F10;SendMessageW(w,WM_COMMAND,IDOK,0);
        Expect(s.error&&!IsWindowEnabled(GetDlgItem(w,IDOK))&&!callback_called,"conflict stays inline and blocks save");Snapshot(s,L"settings-conflict.png",1);
        s.draft.clothesline_enabled=false;if(registered)UnregisterHotKey(nullptr,88);for(int reset_page:{5,1,6,2})ResetPage(s,reset_page);Expect(s.draft.clothesline_enabled,"restore page defaults turns the clothesline back on");Expect(s.draft.key=='A'&&s.draft.modifiers==(MOD_CONTROL|MOD_ALT)&&s.draft.theme==0&&s.draft.include_cursor&&s.draft.paste_as_file&&s.draft.paste_file_format==0&&s.draft.pin_style==DefaultPinStyle,"restore defaults changes dialog settings only");
        SendMessageW(w,WM_COMMAND,IDCANCEL,0);
    }else if(scenario==5){
        SendMessageW(w,WM_COMMAND,128,0);Key(s,VK_LCONTROL,true);Key(s,VK_F23,true);Key(s,VK_F23,false);Key(s,VK_LCONTROL,false);Finish(s);
        Expect(s.draft.clipboard_key==VK_F23&&s.draft.clipboard_modifiers==MOD_CONTROL&&s.draft.key=='A',"clipboard shortcut recorder edits independent chord");
        s.draft.clipboard_key=s.draft.key;s.draft.clipboard_modifiers=s.draft.modifiers;Expect(!s.Validate(),"clipboard cannot duplicate capture shortcut");
        SendMessageW(w,WM_COMMAND,128,0);Key(s,VK_BACK,true);Key(s,VK_BACK,false);Finish(s);Expect(s.draft.clipboard_key==0,"clipboard shortcut can be cleared");
        SendMessageW(w,WM_COMMAND,119,0);Key(s,VK_LCONTROL,true);Key(s,VK_LSHIFT,true);Key(s,'G',true);Key(s,'G',false);Key(s,VK_LSHIFT,false);Key(s,VK_LCONTROL,false);Finish(s);
        Expect(s.draft.gif_key=='G'&&s.draft.gif_modifiers==(MOD_CONTROL|MOD_SHIFT)&&s.draft.key=='A',"GIF recorder edits independent shortcut");
        SendMessageW(w,WM_COMMAND,120,0);Key(s,VK_BACK,true);Key(s,VK_BACK,false);Finish(s);Expect(s.draft.video_key==0,"recording shortcut can be disabled");
        for(int id:{142,143}){
            SendMessageW(w,WM_COMMAND,id,0);Key(s,VK_LCONTROL,true);Key(s,id==142?VK_F17:VK_F18,true);Key(s,id==142?VK_F17:VK_F18,false);Key(s,VK_LCONTROL,false);Finish(s);
            Expect(s.EditKey()==static_cast<UINT>(id==142?VK_F17:VK_F18)&&s.EditMods()==MOD_CONTROL,"OCR and pin each record their own shortcut");
            const auto saved=s.EditKey();SendMessageW(w,WM_COMMAND,id,0);Key(s,VK_ESCAPE,true);Key(s,VK_ESCAPE,false);Finish(s);Expect(s.EditKey()==saved,"OCR and pin Escape restores previous shortcut");
        }
        SendMessageW(w,WM_COMMAND,148,0);Key(s,VK_LCONTROL,true);Key(s,VK_F16,true);Key(s,VK_F16,false);Key(s,VK_LCONTROL,false);Finish(s);
        Expect(s.draft.selection_key==VK_F16&&s.draft.selection_modifiers==MOD_CONTROL,"selection translation records its own shortcut");
        s.draft.selection_key=s.draft.translate_key;s.draft.selection_modifiers=s.draft.translate_modifiers;Expect(!s.Validate(),"selection and screenshot translation duplicates are rejected");
        SendMessageW(w,WM_COMMAND,148,0);Key(s,VK_BACK,true);Key(s,VK_BACK,false);Finish(s);Expect(s.draft.selection_key==0,"selection translation shortcut can be disabled");
        s.draft.ocr_key=s.draft.pin_key;s.draft.ocr_modifiers=s.draft.pin_modifiers;Expect(!s.Validate(),"OCR and pin duplicates are rejected");
        SendMessageW(w,WM_COMMAND,142,0);Key(s,VK_BACK,true);Key(s,VK_BACK,false);Finish(s);Expect(s.draft.ocr_key==0,"OCR shortcut can be disabled");
        s.draft.gif_key=s.draft.key;s.draft.gif_modifiers=s.draft.modifiers;Expect(!s.Validate(),"duplicate shortcuts blocked before parent submission");
        SendMessageW(w,WM_COMMAND,IDCANCEL,0);
    }else{
        SendMessageW(w,WM_COMMAND,IDOK,0);Expect(s.error&&IsWindow(w)&&callback_called,"registration race keeps dialog open");SendMessageW(w,WM_COMMAND,IDCANCEL,0);
    }
}
}
int main(){
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    // Synthetic engine config: the translation page never reads the user's file.
    const auto translation_config=std::filesystem::current_path()/L"settings-translation-test.ini";
    {translate::Config config;config.provider="deepseek";translate::SaveConfig(translation_config,config);}
    SetEnvironmentVariableW(L"LUMASHOT_TRANSLATION_CONFIG",translation_config.c_str());
    Expect(Preferences{}.clipboard_key=='V'&&Preferences{}.clipboard_modifiers==MOD_WIN,"clipboard defaults to Win+V");
    Expect(!CaptureOnLaunch(L"--background")&&!ActivateExistingOnLaunch(L"--background")&&CaptureOnLaunch(L"--capture")&&CaptureOnLaunch(L""),"installer background launch neither captures nor activates existing instance");
    Expect(ShortcutLabel(MOD_WIN|MOD_SHIFT,'S').find(L"Win")!=std::wstring::npos,"shortcut label retains Win modifier");
    Expect(ValidShortcut(0,VK_F1)&&ValidShortcut(0,VK_F11)&&ValidShortcut(0,VK_SNAPSHOT)&&ValidShortcut(0,VK_PAUSE)&&ValidShortcut(MOD_SHIFT,VK_F1),"function keys valid alone or with modifiers");
    Expect(!ValidShortcut(0,'A')&&!ValidShortcut(0,VK_SPACE)&&!ValidShortcut(0,VK_F12)&&!ValidShortcut(MOD_CONTROL,VK_F12),"plain characters and F12 rejected");
    Expect(ShortcutLabel(0,VK_SNAPSHOT)==L"PrtSc"&&ShortcutLabel(0,VK_PAUSE)==L"Pause"&&ShortcutLabel(0,VK_F13)==L"F13"&&ShortcutLabel(MOD_SHIFT,VK_F24)==L"Shift + F24","standalone keys have readable labels");
    for(scenario=0;scenario<6;++scenario){Preferences p;p.theme=1;p.tools.width=9;const auto original=p;callback_called=false;SetTimer(nullptr,0,60,Drive);
        const bool saved=ShowSettingsDialog(nullptr,p,[&](const Preferences&){callback_called=true;return scenario!=3;},[](bool active){input_blocked=active;});
        Expect(saved==(scenario==0),"modal save/cancel result");if(scenario==0)Expect(p.modifiers==(MOD_WIN|MOD_CONTROL)&&p.key==VK_F11&&p.theme==2&&p.paste_as_file&&p.paste_file_format==2&&p.pin_style==PinStyle::Polaroid,"saved draft preserves Win modifier and theme");else Expect(p.modifiers==original.modifiers&&p.key==original.key&&p.include_cursor==original.include_cursor&&p.theme==original.theme&&p.paste_as_file==original.paste_as_file&&p.paste_file_format==original.paste_file_format&&p.pin_style==original.pin_style,"cancel leaves caller preferences unchanged");
        Expect(p.tools.width==9&&Settings::recorder==nullptr&&!input_blocked,"tool properties retained and hook lifetime closed");
    }
    const auto path=std::filesystem::current_path()/L"settings-roundtrip-test.ini";
    {Preferences custom;custom.ocr_modifiers=MOD_CONTROL|MOD_SHIFT;custom.ocr_key=VK_F17;custom.pin_modifiers=MOD_ALT|MOD_SHIFT;custom.pin_key=VK_F18;custom.selection_modifiers=MOD_CONTROL|MOD_ALT|MOD_SHIFT;custom.selection_key=VK_F16;custom.ocr_remove_whitespace=true;custom.ocr_model_directory=L"C:\\Synthetic\\OCR 模型";custom.SaveTo(path);
     const auto loaded_custom=Preferences::LoadFrom(path);Preferences decoded_custom;
     Expect(loaded_custom.ocr_key==VK_F17&&loaded_custom.pin_key==VK_F18&&loaded_custom.ocr_modifiers==custom.ocr_modifiers&&loaded_custom.pin_modifiers==custom.pin_modifiers&&loaded_custom.selection_key==VK_F16&&loaded_custom.selection_modifiers==custom.selection_modifiers&&loaded_custom.ocr_remove_whitespace&&loaded_custom.ocr_model_directory==custom.ocr_model_directory,"OCR and pin preferences persist including Unicode model path");
     Expect(Decode(Encode(custom),decoded_custom)&&AllShortcuts(decoded_custom)==AllShortcuts(custom)&&decoded_custom.ocr_remove_whitespace&&decoded_custom.ocr_model_directory==custom.ocr_model_directory,"isolated settings IPC carries OCR and pin preferences");
     auto invalid=Encode(custom);invalid.ocr_remove_whitespace=2;Expect(!Decode(invalid,decoded_custom),"IPC rejects invalid OCR boolean");
     invalid=Encode(custom);std::fill(std::begin(invalid.ocr_model_directory),std::end(invalid.ocr_model_directory),L'x');Expect(!Decode(invalid,decoded_custom),"IPC rejects unterminated model path");}
    {Preferences toolbar_prefs;toolbar_prefs.toolbar.order={16,3,1,2,4,5,6,14,15,18,17};toolbar_prefs.toolbar.overflow={14,17};toolbar_prefs.toolbar.labels=ToolbarLabels::Icons;toolbar_prefs.toolbar.auto_properties=false;toolbar_prefs.toolbar.property_style=PropertyBarStyle::Card;toolbar_prefs.toolbar.Normalize();toolbar_prefs.SaveTo(path);
     Expect(Preferences::LoadFrom(path).toolbar==toolbar_prefs.toolbar,"[Toolbar] order, overflow, labels, auto-properties and property style roundtrip");
     WritePrivateProfileStringW(L"Toolbar",L"Order",L"99,6,6,abc,1",path.c_str());WritePrivateProfileStringW(L"Toolbar",L"Overflow",L"0,7,19,14",path.c_str());WritePrivateProfileStringW(L"Toolbar",L"Labels",L"9",path.c_str());
     const auto toolbar_clean=Preferences::LoadFrom(path).toolbar;
     Expect(toolbar_clean.order.size()==CustomizableToolbarIds.size()&&toolbar_clean.order[0]==6&&toolbar_clean.order[1]==1&&toolbar_clean.overflow==std::vector<int>{14}&&toolbar_clean.labels==ToolbarLabels::Auto&&!toolbar_clean.auto_properties,"[Toolbar] drops unknown or fixed ids and resets invalid labels");
     Preferences toolbar_decoded;Expect(Decode(Encode(toolbar_prefs),toolbar_decoded)&&toolbar_decoded.toolbar==toolbar_prefs.toolbar,"settings IPC carries the toolbar layout");
     {auto bad_toolbar=Encode(toolbar_prefs);bad_toolbar.toolbar_order[0]=7;Preferences rejected;Expect(!Decode(bad_toolbar,rejected),"IPC rejects fixed toolbar ids in the order");}
     {auto bad_toolbar=Encode(toolbar_prefs);bad_toolbar.toolbar_order[1]=bad_toolbar.toolbar_order[0];Preferences rejected;Expect(!Decode(bad_toolbar,rejected),"IPC rejects duplicate toolbar ids");}
     {auto bad_toolbar=Encode(toolbar_prefs);bad_toolbar.toolbar_overflow_count=static_cast<UINT>(std::size(bad_toolbar.toolbar_overflow))+1;Preferences rejected;Expect(!Decode(bad_toolbar,rejected),"IPC rejects oversized overflow");}
     {auto bad_toolbar=Encode(toolbar_prefs);bad_toolbar.toolbar_labels=3;Preferences rejected;Expect(!Decode(bad_toolbar,rejected),"IPC rejects unknown label mode");}
     {auto bad_style=Encode(toolbar_prefs);bad_style.toolbar_property_style=2;Preferences rejected;Expect(!Decode(bad_style,rejected),"IPC rejects unknown property style");
      Preferences decoded;Expect(Decode(Encode(toolbar_prefs),decoded)&&decoded.toolbar.property_style==PropertyBarStyle::Card,"IPC carries the property style");}
     WritePrivateProfileStringW(L"General",L"SettingsTab",nullptr,path.c_str());WritePrivateProfileStringW(L"General",L"SettingsPage",L"3",path.c_str());
     Expect(Preferences::LoadFrom(path).settings_page==2,"legacy translation page opens the new capture page");
     WritePrivateProfileStringW(L"General",L"SettingsPage",L"5",path.c_str());Expect(Preferences::LoadFrom(path).settings_page==0,"legacy general page opens the new general page");
     Preferences toolbar_defaults;toolbar_defaults.SaveTo(path);Expect(Preferences::LoadFrom(path).toolbar==ToolbarCustomization{},"default toolbar layout roundtrips");}
    WritePrivateProfileStringW(L"Unrelated",L"Keep",L"yes",path.c_str());Preferences stored;stored.clipboard_enabled=true;stored.key=VK_F1;stored.modifiers=0;stored.gif_key='T';stored.gif_modifiers=MOD_CONTROL|MOD_SHIFT;stored.video_key=0;stored.clipboard_key=VK_F23;stored.clipboard_modifiers=MOD_WIN|MOD_SHIFT;stored.SaveTo(path);const auto loaded=Preferences::LoadFrom(path);Expect(loaded.clipboard_key==VK_F23&&loaded.clipboard_modifiers==(MOD_WIN|MOD_SHIFT),"clipboard shortcut file roundtrip");Expect(loaded.clipboard_enabled,"clipboard enable preference roundtrip");Preferences clipboard_decoded;Expect(Decode(Encode(stored),clipboard_decoded)&&clipboard_decoded.clipboard_enabled&&clipboard_decoded.clipboard_key==VK_F23&&clipboard_decoded.clipboard_modifiers==(MOD_WIN|MOD_SHIFT),"clipboard enable IPC roundtrip");
    Expect(loaded.key==VK_F1&&loaded.modifiers==0,"standalone function key survives settings reload without modifier fallback");
    Expect(!loaded.clipboard_persist,"clipboard history persistence defaults off on disk");
    {std::ifstream bom(path,std::ios::binary);char head[2]{};bom.read(head,2);Expect(bom.gcount()==2&&static_cast<unsigned char>(head[0])==0xFF&&static_cast<unsigned char>(head[1])==0xFE,"legacy ANSI settings migrate to UTF-16 file on save");}
    {Preferences unicode=stored;unicode.clipboard_persist=true;unicode.save_directory=L"C:\\Synthetic\\截图 Ünïcødé \U0001F308";unicode.SaveTo(path);const auto reread=Preferences::LoadFrom(path);
     Expect(reread.save_directory==unicode.save_directory,"non-ANSI save folder survives settings reload");Expect(reread.clipboard_persist,"clipboard persistence preference roundtrip");
     Preferences persist_decoded;Expect(Decode(Encode(unicode),persist_decoded)&&persist_decoded.clipboard_persist,"isolated settings IPC carries clipboard persistence");}
    {Expect(loaded.clipboard_strip_visible&&!loaded.clipboard_strip_hint_shown,"strip visible and hint unseen by default on disk");
     Preferences hidden=stored;hidden.clipboard_strip_visible=false;hidden.clipboard_strip_hint_shown=true;hidden.SaveTo(path);const auto reread=Preferences::LoadFrom(path);
     Expect(!reread.clipboard_strip_visible&&reread.clipboard_strip_hint_shown,"hidden strip and seen hint survive settings reload");
     Preferences strip_decoded;Expect(Decode(Encode(hidden),strip_decoded)&&!strip_decoded.clipboard_strip_visible,"isolated settings IPC carries strip visibility");
     auto bad=Encode(hidden);bad.clipboard_strip_visible=2;Preferences rejected;Expect(!Decode(bad,rejected),"IPC rejects invalid strip visibility");}
    {const auto fresh=std::filesystem::current_path()/L"settings-fresh-unicode-test.ini";std::filesystem::remove(fresh);Preferences created;created.save_directory=L"D:\\合成\\ß";created.SaveTo(fresh);
     Expect(Preferences::LoadFrom(fresh).save_directory==created.save_directory,"new settings file is created as Unicode");std::filesystem::remove(fresh);}
    Expect(loaded.gif_key=='T'&&loaded.gif_modifiers==(MOD_CONTROL|MOD_SHIFT)&&loaded.video_key==0,"new bindings persist through isolated settings file");
    for(int style=0;style<=4;++style){
        stored.pin_style=static_cast<PinStyle>(style);stored.SaveTo(path);
        Expect(Preferences::LoadFrom(path).pin_style==stored.pin_style,"every sticker style survives settings reload");
        Preferences decoded;Expect(Decode(Encode(stored),decoded)&&decoded.pin_style==stored.pin_style,"isolated settings IPC roundtrips every sticker style");
    }
    for(const wchar_t* invalid:{L"-1",L"5",L"999"}){WritePrivateProfileStringW(L"General",L"PinStyle",invalid,path.c_str());Expect(Preferences::LoadFrom(path).pin_style==DefaultPinStyle,"invalid persisted sticker style falls back to simple");}
    WritePrivateProfileStringW(L"General",L"PinStyle",nullptr,path.c_str());Expect(Preferences::LoadFrom(path).pin_style==DefaultPinStyle,"old settings without PinStyle migrate to simple");
    auto invalid=Encode(stored);invalid.pin_style=5;Preferences decoded;Expect(!Decode(invalid,decoded),"IPC rejects invalid sticker style");
    wchar_t keep[8]{};GetPrivateProfileStringW(L"Unrelated",L"Keep",L"",keep,8,path.c_str());Expect(std::wstring(keep)==L"yes","atomic settings save preserves unknown fields");std::filesystem::remove(path);
    {Preferences page;page.settings_page=3;page.translate_auto_show=false;page.SaveTo(path);const auto reread=Preferences::LoadFrom(path);
     Expect(reread.settings_page==3&&!reread.translate_auto_show,"settings page and translated pin switch survive settings reload");
     Preferences decoded_page;Expect(Decode(Encode(page),decoded_page)&&decoded_page.settings_page==3&&!decoded_page.translate_auto_show,"isolated settings IPC carries settings page and translated pin switch");
     auto bad=Encode(page);bad.settings_page=8;Preferences rejected;Expect(!Decode(bad,rejected),"IPC rejects invalid settings page");std::filesystem::remove(path);}
    {Preferences changed;changed.start_with_windows=false;changed.update_auto_check=false;changed.key='Q';changed.clipboard_key='C';changed.include_cursor=false;changed.translate_auto_show=false;
     changed.toolbar.labels=ToolbarLabels::Icons;changed.clipboard_enabled=true;changed.clipboard_max_entries=20;changed.clipboard_classic_strip=true;changed.clipboard_card_preview=false;
     changed.pin_style=PinStyle::Curl;changed.clothesline_enabled=false;changed.theme=2;
     const Preferences d;bool scoped=true;
     for(int reset=0;reset<8;++reset){auto x=changed;ResetSettingsPage(x,reset);
      const bool fields[]{(x.start_with_windows==d.start_with_windows)==(reset==0),(x.update_auto_check==d.update_auto_check)==(reset==0),(x.key==d.key)==(reset==1),(x.clipboard_key==d.clipboard_key)==(reset==1),
       (x.include_cursor==d.include_cursor)==(reset==2),(x.translate_auto_show==d.translate_auto_show)==(reset==2),(x.toolbar==d.toolbar)==(reset==3),(x.clipboard_enabled==d.clipboard_enabled)==(reset==4),
       (x.clipboard_max_entries==d.clipboard_max_entries)==(reset==4),(x.clipboard_classic_strip==d.clipboard_classic_strip)==(reset==4),(x.clipboard_card_preview==d.clipboard_card_preview)==(reset==4),
       (x.pin_style==d.pin_style)==(reset==5),(x.clothesline_enabled==d.clothesline_enabled)==(reset==5),(x.theme==d.theme)==(reset==6)};
      for(bool field:fields)scoped=scoped&&field;}
     Expect(scoped,"each page reset restores only that page's settings");
     Preferences clip;clip.clipboard_max_entries=200;clip.clipboard_classic_strip=true;clip.clipboard_card_preview=false;Preferences clip_decoded;
     Expect(Decode(Encode(clip),clip_decoded)&&clip_decoded.clipboard_max_entries==200&&clip_decoded.clipboard_classic_strip&&!clip_decoded.clipboard_card_preview,"settings IPC carries the clipboard display options");
     {auto bad=Encode(clip);bad.clipboard_max_entries=30;Preferences rejected;Expect(!Decode(bad,rejected),"IPC rejects an unsupported history limit");}
     {auto bad=Encode(clip);bad.clipboard_classic_strip=2;Preferences rejected;Expect(!Decode(bad,rejected),"IPC rejects an unknown strip style");}
     {auto bad=Encode(clip);bad.clipboard_card_preview=2;Preferences rejected;Expect(!Decode(bad,rejected),"IPC rejects an invalid preview switch");}
     const auto migrate=std::filesystem::current_path()/L"settings-strip-migration";std::error_code migrate_error;std::filesystem::remove_all(migrate,migrate_error);std::filesystem::create_directories(migrate,migrate_error);
     const auto ini=migrate/L"settings.ini",layout=migrate/L"clipboard-layout.ini";
     clip.SaveTo(ini);const auto clip_loaded=Preferences::LoadFrom(ini);Expect(clip_loaded.clipboard_max_entries==200&&clip_loaded.clipboard_classic_strip&&!clip_loaded.clipboard_card_preview,"clipboard display options survive a reload");
     WritePrivateProfileStringW(L"General",L"ClipboardMaxEntries",L"33",ini.c_str());Expect(Preferences::LoadFrom(ini).clipboard_max_entries==100,"an invalid history limit falls back to 100");
     WritePrivateProfileStringW(L"General",L"ClipboardStripStyle",nullptr,ini.c_str());Expect(!Preferences::LoadFrom(ini).clipboard_classic_strip,"strip style defaults to the card stack");
     WritePrivateProfileStringW(L"Strip",L"Style",L"Classic",layout.c_str());Expect(Preferences::LoadFrom(ini).clipboard_classic_strip,"strip style migrates from the clipboard layout file");
     std::filesystem::remove_all(migrate,migrate_error);}
    {Expect(Preferences::LoadFrom(path).clothesline_enabled,"clothesline defaults on for settings files without the key");
     Preferences off;off.clothesline_enabled=false;off.SaveTo(path);const auto reread=Preferences::LoadFrom(path);
     Expect(!reread.clothesline_enabled,"clothesline switch survives settings reload");
     Preferences decoded_off;Expect(Decode(Encode(off),decoded_off)&&!decoded_off.clothesline_enabled,"isolated settings IPC carries the clothesline switch");
     Preferences on;Preferences decoded_on;decoded_on.clothesline_enabled=false;Expect(Decode(Encode(on),decoded_on)&&decoded_on.clothesline_enabled,"isolated settings IPC carries the clothesline switch turned on");
     auto bad=Encode(off);bad.clothesline_enabled=2;Preferences rejected;Expect(!Decode(bad,rejected),"IPC rejects invalid clothesline switch");std::filesystem::remove(path);}
    std::filesystem::remove(translation_config);
    CoUninitialize();return failures?1:0;
}