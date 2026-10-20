#pragma once
#include <windows.h>
#include <d2d1.h>
#include <functional>
#include <wrl/client.h>
#include "ui/luma_glass.h"
namespace lumashot {
inline unsigned PanelBackground(bool dark){return glass::Pick(glass::Fallback,dark);}
inline float PanelOpacity(bool dark,bool acrylic){return acrylic?glass::Opacity(dark):1.f;}
// DWM owns backdrop sampling/blur. No desktop capture or repaint timer.
void ConfigureBorderlessWindow(HWND window);
void ResizeBorderlessWindow(HWND window,float scale,float radius=12);
// Windows "transparency effects" on and high contrast off. Without them every
// glass surface uses the opaque PanelBackground fill.
bool TransparencyEffectsOn();
// DWM acrylic tinted with the Luma Glass tone at glass::Opacity (full.html
// --glass: #f7f9fc / #252c38 at .60 / .72). Callers draw only the 1/255
// hit-test fill over it (see DrawGlassSurface), never a second tint. Returns
// false (accent cleared) when acrylic is unavailable or transparency is off.
bool SetSettingsAcrylic(HWND window,bool dark);

// Glass popup menus (full.html .menu.glass) on Windows 11: a non-layered
// WS_EX_NOREDIRECTIONBITMAP window the size of the menu body gets the DWM
// transient acrylic backdrop, rounded corners and outline (like the clipboard
// quick window); content is presented with ClipboardComposition. Legacy
// acrylic on a layered window ignores the window region and blurs the whole
// rectangle, shadow margin included, so menus never use it. False when the
// backdrop is unavailable (Windows 10, transparency off): use the opaque menu.
bool ApplyPopupBackdrop(HWND window,bool dark);
// Soft shadow for a glass popup: a click-through, no-activate layered window
// with the popup's bounds, kept directly below it. `draw` paints in DIPs
// (target DPI = 96 * scale) onto a cleared surface; use OutsideLayer so the
// shadow never darkens the glass body.
class PopupShadow {
public:
    PopupShadow()=default;
    PopupShadow(const PopupShadow&)=delete;
    PopupShadow& operator=(const PopupShadow&)=delete;
    ~PopupShadow();
    bool Show(HWND popup,HWND owner,RECT bounds,float scale,const std::function<void(ID2D1RenderTarget*)>& draw);
    HWND Window()const{return window_;}
private:
    HWND window_{};
};
// Restricts drawing to everything outside `body` until destroyed.
class OutsideLayer {
public:
    OutsideLayer(ID2D1RenderTarget* target,D2D1_ROUNDED_RECT body);
    OutsideLayer(const OutsideLayer&)=delete;
    OutsideLayer& operator=(const OutsideLayer&)=delete;
    ~OutsideLayer();
private:
    ID2D1RenderTarget* target_{};
    Microsoft::WRL::ComPtr<ID2D1Layer> layer_;
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> mask_;
    bool pushed_{};
};
}
