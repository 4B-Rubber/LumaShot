#pragma once
#include "ui/render.h"
#include <array>
namespace lumashot {
struct PinMenuModel {
    // Rows (full.html .menu): annotate, lock | copy selected, copy all, table, copy image, save,
    // recognize, translate, translate selected | add to clipboard history, hang on the line, close.
    static constexpr int Count=13;
    static constexpr std::array<int,Count> Commands{7,8,1,2,9,3,4,5,10,11,12,13,6};
    std::array<bool,Count> enabled{true,true,true,true,true,true,true,true,true,true,true,true,true};
    std::wstring status;
    bool dark{},locked{},recognized{},table{},translated{},selection{};
    bool ocr_available{true};
    bool hang{true};  // "晾到绳上" row: only while the clothesline is switched on
    bool glassy{};  // drawn over the DWM backdrop (full.html .menu.glass): token tint, no outline, accent-soft hover
    static constexpr float Width=224.f;
    static constexpr float Shadow=20.f;
    bool Visible(int i)const{return (i!=4||table)&&((i<7||i>9)||ocr_available)&&(i!=9||selection)&&(i!=11||hang);}
    float Height()const{return 460.f+(hang?42.f:0.f)+(table?42.f:0.f)+(selection&&ocr_available?42.f:0.f)-(ocr_available?0.f:84.f)+(status.empty()?0.f:36.f);}
    float RowTop(int i)const{return 8.f+(status.empty()?0.f:36.f)+42.f*(i-(!table&&i>4?1:0)-(!ocr_available&&i>7?1:0)-(!ocr_available&&i>8?1:0)-(!(ocr_available&&selection)&&i>9?1:0)-(!hang&&i>11?1:0))+(i>=2?8.f:0.f)+(i>=5?8.f:0.f)+(i>=10?8.f:0.f);}
    int Hit(float x,float y)const;
    int Next(int current,int direction)const;
};
RECT PlacePinMenu(POINT anchor,RECT work,float scale,const PinMenuModel& model);
void DrawPinMenu(ID2D1RenderTarget* target,const PinMenuModel& model,int hover);
void DrawPinMenuShadow(ID2D1RenderTarget* target,const PinMenuModel& model,bool hollow);
void DrawPinMenuSurface(ID2D1RenderTarget* target,const PinMenuModel& model,int hover);
int TrackPinMenu(HWND owner,POINT anchor,const PinMenuModel& model);
// DIP offset of the menu body inside the popup window: the shadow margin for the
// opaque layered menu, 0 for the glass menu (its window is the body).
float PinMenuInset(HWND menu);
}