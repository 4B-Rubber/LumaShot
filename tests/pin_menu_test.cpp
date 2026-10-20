#include "ui/pin_menu.h"
#include "export/png.h"
#include "ui/acrylic.h"
#include <wincodec.h>
#include <iostream>
#include <algorithm>
#include <cstdlib>
using namespace lumashot;
static int requested=0;
static bool alpha_popup=true;
static bool glass_popup=true;
static int glass_shots=0;static int margin_dark[2]{};static HWND glass_menu{};static int glass_range[2]{};
// Real DWM glass over this test's own topmost backdrop (never the user's
// desktop): the captured rect is the menu window, which lies inside it.
static LRESULT CALLBACK Backdrop(HWND w,UINT msg,WPARAM wp,LPARAM lp){
    if(msg==WM_ERASEBKGND)return 1;
    if(msg==WM_PAINT){PAINTSTRUCT ps{};HDC dc=BeginPaint(w,&ps);RECT c{};GetClientRect(w,&c);
        static constexpr COLORREF bands[]{RGB(255,94,98),RGB(255,196,61),RGB(70,200,120),RGB(40,140,255),RGB(150,90,230),RGB(20,30,45),RGB(250,250,250)};
        for(int y=0,i=0;y<c.bottom;y+=90,++i){RECT band{0,y,c.right,y+90};HBRUSH brush=CreateSolidBrush(bands[i%7]);FillRect(dc,&band,brush);DeleteObject(brush);}
        SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(0,0,0));for(int y=10;y<c.bottom;y+=45){RECT line{12,y,c.right,y+40};DrawTextW(dc,L"LumaShot glass backdrop  0123456789  ABCDEFGHIJKLMNOPQRSTUVWXYZ",-1,&line,DT_SINGLELINE);}
        EndPaint(w,&ps);return 0;}
    return DefWindowProcW(w,msg,wp,lp);
}
static void CALLBACK CaptureGlass(HWND,UINT,UINT_PTR id,DWORD){
    KillTimer(nullptr,id);RECT r{};GetWindowRect(glass_menu,&r);const int o=40;InflateRect(&r,o,o);const int w=r.right-r.left,h=r.bottom-r.top;
    HDC screen=GetDC(nullptr);HDC dc=CreateCompatibleDC(screen);BITMAPINFO info{};info.bmiHeader={sizeof(BITMAPINFOHEADER),w,-h,1,32,BI_RGB};void* bits{};
    HBITMAP bitmap=CreateDIBSection(screen,&info,DIB_RGB_COLORS,&bits,nullptr,0);HGDIOBJ old=SelectObject(dc,bitmap);BitBlt(dc,0,0,w,h,screen,r.left,r.top,SRCCOPY);GdiFlush();
    auto frame=MakeFrame({0,0,w,h});const auto* source=static_cast<const uint32_t*>(bits);for(size_t i=0;i<frame.pixels.size();++i)frame.pixels[i]=source[i]|0xff000000u;
    SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);ReleaseDC(nullptr,screen);
    // Body column left of the icons: varies with the backdrop bands only if the menu is translucent.
    const float scale=float(w-2*o)/PinMenuModel::Width;const int x=o+int(12*scale);int lo[3]{255,255,255},hi[3]{};
    for(int y=o+int(16*scale);y<h-o-int(16*scale);y+=4){const uint32_t c=frame.pixels[size_t(y)*w+x];for(int k=0;k<3;++k){const int v=int((c>>(8*k))&255);lo[k]=std::min(lo[k],v);hi[k]=std::max(hi[k],v);}}
    glass_range[glass_shots&1]=std::max({hi[0]-lo[0],hi[1]-lo[1],hi[2]-lo[2]});
    // Beside the glass body (inside the shadow window) the backdrop stays sharp: black text edges on bright bands.
    {int edges=0;const auto sum=[](uint32_t c){return int((c>>16)&255)+int((c>>8)&255)+int(c&255);};for(int y=o;y<h-o;++y)for(int mx=2;mx<o-4;++mx)if(std::abs(sum(frame.pixels[size_t(y)*w+mx])-sum(frame.pixels[size_t(y)*w+mx+1]))>200)++edges;margin_dark[glass_shots&1]=edges;}
    SavePng(frame,requested==16?L"pin-menu-glass-light.png":L"pin-menu-glass-dark.png");++glass_shots;PostMessageW(glass_menu,WM_KEYDOWN,VK_ESCAPE,0);
}
static void CALLBACK Drive(HWND,UINT,UINT_PTR id,DWORD){
    HWND menu=FindWindowW(L"LumaShot.PinMenu",nullptr);if(!menu)return;KillTimer(nullptr,id);
    alpha_popup&=(GetWindowLongPtrW(menu,GWL_EXSTYLE)&(WS_EX_LAYERED|WS_EX_NOREDIRECTIONBITMAP))!=0&&(GetClassLongPtrW(menu,GCL_STYLE)&CS_DROPSHADOW)==0;
    {const LONG_PTR ex=GetWindowLongPtrW(menu,GWL_EXSTYLE);const HWND shadow=FindWindowW(L"LumaShot.PopupShadow",nullptr);bool below=false;for(HWND h=GetWindow(menu,GW_HWNDNEXT);h&&shadow;h=GetWindow(h,GW_HWNDNEXT))if(h==shadow){below=true;break;}RECT mr{},sr{};GetWindowRect(menu,&mr);if(shadow)GetWindowRect(shadow,&sr);glass_popup&= TransparencyEffectsOn()?(ex&WS_EX_NOREDIRECTIONBITMAP)!=0&&(ex&WS_EX_LAYERED)==0&&shadow&&IsWindowVisible(shadow)&&below&&sr.left<mr.left&&sr.top<mr.top&&sr.right>mr.right&&sr.bottom>mr.bottom&&(GetWindowLongPtrW(shadow,GWL_EXSTYLE)&WS_EX_TRANSPARENT)!=0:(ex&WS_EX_LAYERED)!=0&&!shadow;}
    if(requested>=16){glass_menu=menu;SetTimer(nullptr,0,700,CaptureGlass);return;}
    if(requested==10){const float scale=float(GetDpiForWindow(menu))/96;PinMenuModel m;m.table=true;const LPARAM pt=MAKELPARAM(int((PinMenuInset(menu)+60)*scale),int((PinMenuInset(menu)+m.RowTop(5)+21)*scale));PostMessageW(menu,WM_LBUTTONDOWN,0,pt);PostMessageW(menu,WM_LBUTTONUP,0,pt);}
    else if(requested==11)PostMessageW(menu,WM_LBUTTONDOWN,0,PinMenuInset(menu)>0?MAKELPARAM(1,1):MAKELPARAM(-4,-4));  // shadow margin, or just outside the glass body
    else if(requested==12)PostMessageW(menu,WM_KEYDOWN,VK_SPACE,0);
    else if(requested==13)PostMessageW(menu,WM_KEYDOWN,'L',0);
    else if(requested==0)PostMessageW(menu,WM_KEYDOWN,VK_ESCAPE,0);
    else if(requested==14)PostMessageW(menu,WM_LBUTTONDOWN,0,MAKELPARAM(-20,-20));
    else if(requested==15)PostMessageW(menu,WM_KEYDOWN,'H',0);
    else {PostMessageW(menu,WM_KEYDOWN,VK_HOME,0);for(int i=1;i<requested;++i)PostMessageW(menu,WM_KEYDOWN,VK_DOWN,0);PostMessageW(menu,WM_KEYDOWN,VK_RETURN,0);}
}
int main(){
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);int failures=0;
    auto expect=[&](bool value,const char* message){std::cout<<(value?"PASS ":"FAIL ")<<message<<'\n';failures+=!value;};
    PinMenuModel m;m.table=true;for(float scale:{1.f,1.5f,2.f})for(POINT point:{POINT{-1920,0},POINT{-1,1079}}){const float fit=std::min(scale,1080.f/(m.Height()+2*PinMenuModel::Shadow));auto r=PlacePinMenu(point,{-1920,0,0,1080},fit,m);expect(r.left>=-1920&&r.top>=0&&r.right<=0&&r.bottom<=1080,"menu stays on negative-coordinate monitor at mixed DPI");}
    for(int i=0;i<PinMenuModel::Count;++i)if(m.Visible(i))expect(m.Hit(40,m.RowTop(i)+21)==i,"paint and hit rows agree");
    {PinMenuModel s=m;s.selection=true;for(int i=0;i<PinMenuModel::Count;++i)expect(s.Visible(i)&&s.Hit(40,s.RowTop(i)+21)==i,"paint and hit rows agree with the selection row");}
    m.enabled[0]=m.enabled[1]=false;expect(m.Next(12,1)==2&&m.Next(2,-1)==12&&m.Hit(40,29)==-1,"disabled rows skipped by mouse and keyboard");m.enabled.fill(true);
    HWND owner=CreateWindowExW(WS_EX_TOOLWINDOW,L"STATIC",L"Synthetic menu test",WS_POPUP,100,100,400,400,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);ShowWindow(owner,SW_SHOWNOACTIVATE);
    for(requested=0;requested<=15;++requested){SetTimer(nullptr,0,30,Drive);int command=TrackPinMenu(owner,{150,150},m);expect(command==(requested>=1&&requested<=9?PinMenuModel::Commands[requested-1]:requested==10?3:requested==12?7:requested==13?8:requested==15?13:0),"popup dispatch, Escape and outside-click dismissal");}
    expect(PinMenuModel::Commands[8]==10&&m.Visible(8)&&m.Hit(40,m.RowTop(8)+21)==8,"translate row sits after recognize");
    {PinMenuModel s=m;s.selection=true;expect(PinMenuModel::Commands[9]==11&&!m.Visible(9)&&s.Visible(9)&&s.Hit(40,s.RowTop(9)+21)==9&&s.RowTop(10)>=s.RowTop(9)+50&&s.Height()==m.Height()+42,"translate-selection row appears after translate only with selected text");}
    {PinMenuModel off=m;off.ocr_available=false;expect(!off.Visible(7)&&!off.Visible(8)&&!off.Visible(9)&&off.Height()==m.Height()-84&&off.Hit(40,off.RowTop(10)+21)==10,"recognize and translate rows hidden without OCR");}
    expect(PinMenuModel::Commands[10]==12&&PinMenuModel::Commands[11]==13&&PinMenuModel::Commands[12]==6&&m.Visible(10)&&m.Visible(11)&&m.RowTop(10)>=m.RowTop(8)+50,"clipboard-history and hang rows sit in the last group before close");
    {PinMenuModel off=m;off.hang=false;expect(!off.Visible(11)&&off.Height()==m.Height()-42&&off.Hit(40,off.RowTop(12)+21)==12&&off.Next(10,1)==12,"hang row hidden while the clothesline is off");}
    m.table=false;
    expect(m.Next(3,1)==5&&m.Next(5,-1)==3&&m.Hit(40,m.RowTop(5)+21)==5,"table row absent without a reliable result");
    m.table=true;expect(m.Next(3,1)==4&&PinMenuModel::Commands[4]==9,"table command reachable with a reliable result");
    expect(alpha_popup,"popup uses alpha compositing without native hard shadow");DestroyWindow(owner);
    expect(glass_popup,"glass menu clips acrylic to the body and keeps its shadow in a click-through window below (opaque look when transparency is off)");
    if(TransparencyEffectsOn()){
        // Physical pixels like the per-monitor-aware app (this test exe is DPI unaware).
        const DPI_AWARENESS_CONTEXT previous_dpi=SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        MONITORINFO mi{sizeof(mi)};GetMonitorInfoW(MonitorFromPoint({0,0},MONITOR_DEFAULTTOPRIMARY),&mi);const RECT wa=mi.rcWork;
        WNDCLASSW bc{};bc.hInstance=GetModuleHandleW(nullptr);bc.lpszClassName=L"LumaShot.MenuGlassBackdrop";bc.lpfnWndProc=Backdrop;RegisterClassW(&bc);
        HWND backdrop=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW,bc.lpszClassName,L"",WS_POPUP,wa.left,wa.top,wa.right-wa.left,wa.bottom-wa.top,nullptr,nullptr,bc.hInstance,nullptr);
        ShowWindow(backdrop,SW_SHOWNOACTIVATE);UpdateWindow(backdrop);
        for(const ULONGLONG end=GetTickCount64()+300;GetTickCount64()<end;){MSG msg{};while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}Sleep(10);}
        for(int theme=0;theme<2;++theme){PinMenuModel g=m;g.dark=theme==1;g.locked=false;requested=16+theme;SetTimer(nullptr,0,30,Drive);TrackPinMenu(backdrop,{wa.left+60,wa.top+40},g);}
        DestroyWindow(backdrop);SetThreadDpiAwarenessContext(previous_dpi);
        std::cout<<"glass body range light="<<glass_range[0]<<" dark="<<glass_range[1]<<"; sharp edges beside the menu light="<<margin_dark[0]<<" dark="<<margin_dark[1]<<"\n";
        expect(glass_shots==2&&glass_range[0]>25&&glass_range[1]>25,"real glass menu shows the backdrop through its body in both themes");
        expect(margin_dark[0]>20&&margin_dark[1]>20,"acrylic stays inside the rounded body (shadow margin shows the sharp backdrop)");
    }
    m.recognized=true;
    try{
        auto frame=MakeFrame({0,0,604,620},0xffffffff);ComPtr<IWICImagingFactory> wic;CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&wic));ComPtr<IWICBitmap> bitmap;
        wic->CreateBitmapFromMemory(frame.Width(),frame.Height(),GUID_WICPixelFormat32bppPBGRA,frame.Width()*4,UINT(frame.pixels.size()*4),reinterpret_cast<BYTE*>(frame.pixels.data()),&bitmap);
        ComPtr<ID2D1Factory> factory;D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,factory.GetAddressOf());ComPtr<ID2D1RenderTarget> target;
        factory->CreateWicBitmapRenderTarget(bitmap.Get(),D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),96,96),&target);
        target->BeginDraw();target->SetTransform(D2D1::Matrix3x2F::Translation(24,32));DrawPinMenuSurface(target.Get(),m,2);m.dark=true;m.locked=true;target->SetTransform(D2D1::Matrix3x2F::Translation(316,32));DrawPinMenuSurface(target.Get(),m,2);CheckWin32(SUCCEEDED(target->EndDraw()),"Preview render");
        bitmap->CopyPixels(nullptr,frame.Width()*4,UINT(frame.pixels.size()*4),reinterpret_cast<BYTE*>(frame.pixels.data()));SavePng(frame,L"pin-menu-preview.png");
        // Hovered close row: red-tinted background and red text (full.html .mi.danger:hover), both themes.
        target->BeginDraw();target->SetTransform(D2D1::Matrix3x2F::Identity());target->Clear(D2D1::ColorF(1.f,1.f,1.f,1.f));m.dark=false;m.locked=false;
        target->SetTransform(D2D1::Matrix3x2F::Translation(24,32));DrawPinMenuSurface(target.Get(),m,12);m.dark=true;target->SetTransform(D2D1::Matrix3x2F::Translation(316,32));DrawPinMenuSurface(target.Get(),m,12);CheckWin32(SUCCEEDED(target->EndDraw()),"Danger preview render");
        bitmap->CopyPixels(nullptr,frame.Width()*4,UINT(frame.pixels.size()*4),reinterpret_cast<BYTE*>(frame.pixels.data()));SavePng(frame,L"pin-menu-danger-preview.png");
        {const auto px=[&](int x,int y){return frame.pixels[size_t(y)*frame.Width()+x];};const int row=int(32+PinMenuModel::Shadow+m.RowTop(12)+4),normal=int(32+PinMenuModel::Shadow+m.RowTop(11)+4);
         const auto red=[](uint32_t c){return int((c>>16)&255)>int((c>>8)&255)+10;};
         expect(red(px(24+int(PinMenuModel::Shadow)+12,row))&&red(px(316+int(PinMenuModel::Shadow)+12,row))&&!red(px(24+int(PinMenuModel::Shadow)+12,normal)),"hovered close row uses the danger tint in both themes");}expect(true,"production menu renderer light and dark preview");
    }catch(const std::exception& e){std::cout<<e.what();++failures;}
    CoUninitialize();return failures?1:0;
}