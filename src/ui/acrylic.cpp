#include "ui/acrylic.h"
#include <cstring>
#include <dwmapi.h>
#pragma comment(lib,"dwmapi.lib")
#pragma comment(lib,"d2d1.lib")
#pragma comment(lib,"advapi32.lib")
// ClipboardComposition (glass popups present through DirectComposition); every target with glass windows links this file.
#pragma comment(lib,"d3d11.lib")
#pragma comment(lib,"dxgi.lib")
#pragma comment(lib,"dcomp.lib")
namespace lumashot {
using Microsoft::WRL::ComPtr;
void ConfigureBorderlessWindow(HWND window){
    SetWindowLongPtrW(window,GWL_STYLE,(GetWindowLongPtrW(window,GWL_STYLE)&~(WS_CAPTION|WS_THICKFRAME|WS_BORDER|DS_MODALFRAME))|WS_POPUP);
    SetWindowLongPtrW(window,GWL_EXSTYLE,(GetWindowLongPtrW(window,GWL_EXSTYLE)&~(WS_EX_DLGMODALFRAME|WS_EX_WINDOWEDGE|WS_EX_CLIENTEDGE))|WS_EX_LAYERED);
    const DWORD round=2;DwmSetWindowAttribute(window,33,&round,sizeof(round));
    SetWindowPos(window,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED);
}
void ResizeBorderlessWindow(HWND window,float scale,float radius){
    RECT client{};GetClientRect(window,&client);HRGN region=CreateRoundRectRgn(0,0,client.right+1,client.bottom+1,int(2*radius*scale),int(2*radius*scale));if(region&&!SetWindowRgn(window,region,TRUE))DeleteObject(region);
}
bool TransparencyEffectsOn(){
    DWORD transparency=1,size=sizeof(transparency);
    RegGetValueW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",L"EnableTransparency",RRF_RT_REG_DWORD,nullptr,&transparency,&size);
    HIGHCONTRASTW contrast{sizeof(contrast)};
    const bool high_contrast=SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(contrast),&contrast,0)&&(contrast.dwFlags&HCF_HIGHCONTRASTON);
    return transparency!=0&&!high_contrast;
}
bool SetSettingsAcrylic(HWND window,bool dark){
    // Runtime lookup keeps the app compatible with Windows builds without this
    // composition entry point. The caller renders an opaque fallback on failure.
    struct Accent {int state,flags;DWORD gradient;int animation;};
    struct Attribute {int attribute;void* data;SIZE_T size;};
    using Apply=BOOL(WINAPI*)(HWND,Attribute*);
    const auto address=GetProcAddress(GetModuleHandleW(L"user32.dll"),"SetWindowCompositionAttribute");
    Apply apply{};static_assert(sizeof(apply)==sizeof(address));std::memcpy(&apply,&address,sizeof(apply));
    if(!apply)return false;
    // The tint is the only glass layer (AABBGGRR). It used to be 0xb3 (70%)
    // while the settings dialog painted another 60/72% over it, which made the
    // dialog ~88/92% opaque instead of full.html's .62/.72.
    const bool on=TransparencyEffectsOn();
    const DWORD alpha=DWORD(glass::Opacity(dark)*255.f+.5f),tone=glass::Pick(glass::Fallback,dark);
    const DWORD abgr=(alpha<<24)|((tone&0xff)<<16)|(tone&0xff00)|((tone>>16)&0xff);
    Accent accent{on?4:0,0,abgr,0};
    Attribute data{19,&accent,sizeof(accent)};
    return apply(window,&data)!=FALSE&&on;
}
bool ApplyPopupBackdrop(HWND window,bool dark){
    if(!TransparencyEffectsOn())return false;
    const BOOL night=dark;DwmSetWindowAttribute(window,20,&night,sizeof(night));
    const DWORD corner=2;DwmSetWindowAttribute(window,33,&corner,sizeof(corner));
    const MARGINS margins{-1,-1,-1,-1};DwmExtendFrameIntoClientArea(window,&margins);
    const DWORD backdrop=3;return SUCCEEDED(DwmSetWindowAttribute(window,38,&backdrop,sizeof(backdrop)));
}
PopupShadow::~PopupShadow(){if(window_&&IsWindow(window_)){SetWindowDisplayAffinity(window_,WDA_EXCLUDEFROMCAPTURE);DestroyWindow(window_);}}
bool PopupShadow::Show(HWND popup,HWND owner,RECT bounds,float scale,const std::function<void(ID2D1RenderTarget*)>& draw){
    const int width=bounds.right-bounds.left,height=bounds.bottom-bounds.top;if(width<=0||height<=0)return false;
    WNDCLASSW wc{};wc.hInstance=GetModuleHandleW(nullptr);wc.lpfnWndProc=DefWindowProcW;wc.lpszClassName=L"LumaShot.PopupShadow";
    if(!RegisterClassW(&wc)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)return false;
    // Owned by the popup's owner, not the popup: an owned window always sits
    // above its owner, and the shadow must stay below the menu.
    window_=CreateWindowExW(WS_EX_LAYERED|WS_EX_TRANSPARENT|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE|WS_EX_TOPMOST,wc.lpszClassName,L"",WS_POPUP,bounds.left,bounds.top,width,height,owner,nullptr,wc.hInstance,nullptr);
    if(!window_)return false;
    BITMAPINFO info{};info.bmiHeader.biSize=sizeof(info.bmiHeader);info.bmiHeader.biWidth=width;info.bmiHeader.biHeight=-height;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
    void* bits{};HDC screen=GetDC(nullptr);HDC dc=CreateCompatibleDC(screen);HBITMAP bitmap=CreateDIBSection(screen,&info,DIB_RGB_COLORS,&bits,nullptr,0);ReleaseDC(nullptr,screen);
    bool ok=dc&&bitmap;HGDIOBJ old{};
    if(ok){
        old=SelectObject(dc,bitmap);
        ComPtr<ID2D1Factory> factory;ComPtr<ID2D1DCRenderTarget> target;
        const auto props=D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),96*scale,96*scale);
        RECT r{0,0,width,height};
        ok=SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,factory.GetAddressOf()))&&SUCCEEDED(factory->CreateDCRenderTarget(&props,&target))&&SUCCEEDED(target->BindDC(dc,&r));
        if(ok){target->BeginDraw();target->Clear(D2D1::ColorF(0,0.f));draw(target.Get());ok=SUCCEEDED(target->EndDraw());}
        if(ok){POINT origin{bounds.left,bounds.top},source{};SIZE size{width,height};BLENDFUNCTION blend{AC_SRC_OVER,0,255,AC_SRC_ALPHA};
            ok=UpdateLayeredWindow(window_,nullptr,&origin,&size,dc,&source,0,&blend,ULW_ALPHA)!=FALSE;}
        SelectObject(dc,old);
    }
    if(bitmap)DeleteObject(bitmap);if(dc)DeleteDC(dc);
    if(!ok){DestroyWindow(window_);window_=nullptr;return false;}
    SetWindowPos(window_,popup,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_SHOWWINDOW);
    return true;
}
OutsideLayer::OutsideLayer(ID2D1RenderTarget* target,D2D1_ROUNDED_RECT body):target_(target){
    ComPtr<ID2D1Factory> factory;target->GetFactory(factory.GetAddressOf());if(!factory)return;
    ComPtr<ID2D1RectangleGeometry> outer;ComPtr<ID2D1RoundedRectangleGeometry> inner;ComPtr<ID2D1GeometrySink> sink;
    if(FAILED(factory->CreateRectangleGeometry(D2D1::RectF(-100000.f,-100000.f,100000.f,100000.f),&outer))||FAILED(factory->CreateRoundedRectangleGeometry(body,&inner))||
       FAILED(factory->CreatePathGeometry(&mask_))||FAILED(mask_->Open(&sink)))return;
    const bool combined=SUCCEEDED(outer->CombineWithGeometry(inner.Get(),D2D1_COMBINE_MODE_EXCLUDE,nullptr,sink.Get()));
    if(FAILED(sink->Close())||!combined||FAILED(target->CreateLayer(nullptr,&layer_)))return;
    target->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(),mask_.Get()),layer_.Get());pushed_=true;
}
OutsideLayer::~OutsideLayer(){if(pushed_)target_->PopLayer();}
}