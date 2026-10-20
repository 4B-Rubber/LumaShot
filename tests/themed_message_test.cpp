#include "ui/themed_message.h"
#include "export/png.h"
#include <iostream>
#include <algorithm>
using namespace lumashot;
namespace {
int mode{},failures{},seen{};HWND owner{};
// Real DWM glass over the test's own topmost backdrop (never the user's desktop).
int glass_range[2]{},glass_shots{};bool glass_dark{},glass_pending{};
LRESULT CALLBACK Backdrop(HWND w,UINT msg,WPARAM wp,LPARAM lp){
    if(msg==WM_ERASEBKGND)return 1;
    if(msg==WM_PAINT){PAINTSTRUCT ps{};HDC dc=BeginPaint(w,&ps);RECT c{};GetClientRect(w,&c);
        static constexpr COLORREF bands[]{RGB(255,94,98),RGB(255,196,61),RGB(70,200,120),RGB(40,140,255),RGB(150,90,230),RGB(20,30,45),RGB(250,250,250)};
        for(int y=0,i=0;y<c.bottom;y+=60,++i){RECT band{0,y,c.right,y+60};HBRUSH brush=CreateSolidBrush(bands[i%7]);FillRect(dc,&band,brush);DeleteObject(brush);}
        EndPaint(w,&ps);return 0;}
    return DefWindowProcW(w,msg,wp,lp);
}
void CALLBACK CaptureGlass(HWND,UINT,UINT_PTR id,DWORD){
    KillTimer(nullptr,id);HWND w=FindWindowW(L"LumaShot.ThemedMessage",nullptr);if(!w)return;RECT r{};GetWindowRect(w,&r);const int cw=r.right-r.left,ch=r.bottom-r.top;
    HDC screen=GetDC(nullptr);HDC dc=CreateCompatibleDC(screen);BITMAPINFO info{};info.bmiHeader={sizeof(BITMAPINFOHEADER),cw,-ch,1,32,BI_RGB};void* bits{};
    HBITMAP bitmap=CreateDIBSection(screen,&info,DIB_RGB_COLORS,&bits,nullptr,0);HGDIOBJ old=SelectObject(dc,bitmap);BitBlt(dc,0,0,cw,ch,screen,r.left,r.top,SRCCOPY);GdiFlush();
    auto frame=MakeFrame({0,0,cw,ch});const auto* source=static_cast<const uint32_t*>(bits);for(size_t i=0;i<frame.pixels.size();++i)frame.pixels[i]=source[i]|0xff000000u;
    SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);ReleaseDC(nullptr,screen);
    // Empty column left of the text: varies with the backdrop bands only if the dialog is translucent.
    const float scale=float(cw)/480.f;const int x=int(12*scale);int lo[3]{255,255,255},hi[3]{};
    for(int y=int(12*scale);y<ch-int(12*scale);y+=3){const uint32_t c=frame.pixels[size_t(y)*cw+x];for(int k=0;k<3;++k){const int v=int((c>>(8*k))&255);lo[k]=std::min(lo[k],v);hi[k]=std::max(hi[k],v);}}
    glass_range[glass_dark?1:0]=std::max({hi[0]-lo[0],hi[1]-lo[1],hi[2]-lo[2]});
    wchar_t exe[MAX_PATH]{};GetModuleFileNameW(nullptr,exe,MAX_PATH);
    SavePng(frame,std::filesystem::path(exe).parent_path()/(glass_dark?L"themed-message-glass-dark.png":L"themed-message-glass-light.png"));
    ++glass_shots;PostMessageW(w,WM_KEYDOWN,VK_ESCAPE,0);
}
void Expect(bool value,const char* name){std::cout<<(value?"PASS ":"FAIL ")<<name<<'\n';if(!value)++failures;}
void CALLBACK Tick(HWND,UINT,UINT_PTR,DWORD){
    HWND w=FindWindowW(L"LumaShot.ThemedMessage",nullptr);if(!w)return;
    // The dialog is excluded from screen capture in production; the test lifts that to measure the real glass.
    if(mode==4){if(!glass_pending){glass_pending=true;SetWindowDisplayAffinity(w,WDA_NONE);SetTimer(nullptr,0,700,CaptureGlass);}return;}  // let DWM compose the backdrop first
    ++seen;Expect(!IsWindowEnabled(owner),"owner disabled during prompt");
    Expect((GetWindowLongPtrW(w,GWL_STYLE)&WS_CAPTION)==0,"no native dialog chrome");
    if(mode==0)PostMessageW(w,WM_KEYDOWN,VK_ESCAPE,0);
    if(mode==1)PostMessageW(w,WM_KEYDOWN,VK_RETURN,0);
    if(mode==2){PostMessageW(w,WM_KEYDOWN,VK_TAB,0);PostMessageW(w,WM_KEYDOWN,VK_RETURN,0);}
    if(mode==3)PostMessageW(w,WM_CLOSE,0,0);
}
}
int main(){
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    owner=CreateWindowExW(0,L"STATIC",L"Synthetic test owner",WS_POPUP,100,100,500,300,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    const auto timer=SetTimer(nullptr,0,100,Tick);
    for(bool dark:{false,true})for(mode=0;mode<4;++mode){
        const bool answer=ui::ShowThemedMessage(owner,dark,L"关闭录制？",L"当前录制尚未保存。关闭后将无法恢复这段内容。",L"放弃并关闭",L"保留录制");
        Expect(answer==(mode==2),"only explicit confirmation accepts");Expect(IsWindowEnabled(owner),"owner re-enabled");
        Expect(!FindWindowW(L"LumaShot.ThemedMessage",nullptr),"dialog destroyed");
    }
    mode=1;Expect(!ui::ShowThemedMessage(owner,false,L"导出未完成",L"导出已取消，原始录制仍可继续预览。"),"notice dismisses safely");
    Expect(seen>=9,"all prompts rendered and received input");
    if(TransparencyEffectsOn()){
        RECT wa{};SystemParametersInfoW(SPI_GETWORKAREA,0,&wa,0);
        WNDCLASSW bc{};bc.hInstance=GetModuleHandleW(nullptr);bc.lpszClassName=L"LumaShot.MessageGlassBackdrop";bc.lpfnWndProc=Backdrop;RegisterClassW(&bc);
        HWND backdrop=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW,bc.lpszClassName,L"",WS_POPUP,wa.left,wa.top,wa.right-wa.left,wa.bottom-wa.top,nullptr,nullptr,bc.hInstance,nullptr);
        ShowWindow(backdrop,SW_SHOWNOACTIVATE);UpdateWindow(backdrop);
        mode=4;for(bool dark:{false,true}){glass_dark=dark;glass_pending=false;ui::ShowThemedMessage(backdrop,dark,L"关闭录制？",L"当前录制尚未保存。关闭后将无法恢复这段内容。",L"放弃并关闭",L"保留录制");}
        DestroyWindow(backdrop);
        std::cout<<"glass body range light="<<glass_range[0]<<" dark="<<glass_range[1]<<"\n";
        Expect(glass_shots==2&&glass_range[0]>25&&glass_range[1]>25,"real glass message shows the backdrop through its body in both themes");
    }
    KillTimer(nullptr,timer);DestroyWindow(owner);CoUninitialize();return failures?1:0;
}