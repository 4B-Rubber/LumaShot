#include "ui/render.h"
#include "model/arrow.h"
#include "model/note_color.h"
#include "ui/fonts.h"
#include <algorithm>
#include <cmath>
#include <string_view>

namespace lumashot {
namespace {
struct PropertyField { int id;ui::Kind kind;float width;LPCWSTR text=L"";int icon=-1; };
// segmented: the buttons share one rounded track (two or three mutually exclusive choices).
struct PropertyGroup { LPCWSTR title;std::vector<PropertyField> fields;bool segmented=false; };
std::vector<PropertyGroup> PropertyGroups(Tool tool,NumberCombo combo) {
    using K=ui::Kind;
    // Compact, content-sized fields; the caption sits on the left of its controls.
    PropertyGroup colors{L"颜色",{}};
    for(int id:AnnotationColorIds)colors.fields.push_back({id,K::Swatch,22});
    colors.fields.push_back({28,K::Swatch,22});
    const PropertyGroup width{L"线宽",{{27,K::Dropdown,66}}};
    const PropertyGroup line{L"线型",{{48,K::Dropdown,66}}};
    PropertyGroup fill{L"填充",{{40,K::Button,44,L"无",-2}}};
    for(int id:AnnotationFillIds)fill.fields.push_back({id,K::Swatch,22});
    switch(tool){
    case Tool::Rectangle:return {colors,width,line,fill,{L"透明度",{{46,K::Slider,124}}},{L"圆角",{{47,K::Slider,124}}}};
    case Tool::Ellipse:return {colors,width,fill,{L"透明度",{{46,K::Slider,124}}}};
    case Tool::Arrow:return {colors,width,line,{L"类型",{{49,K::Dropdown,84}}},{L"头部",{{66,K::Dropdown,104}}},{L"大小",{{50,K::Dropdown,66}}}};
    case Tool::Pen:return {{L"轨迹",{{83,K::Button,44,L"自由"},{84,K::Button,44,L"直线"},{85,K::Button,70,L"连续直线"}},true},{L"",{{86,K::Button,64,L"捕捉 开"}}},
        {L"模式",{{58,K::Button,44,L"普通"},{59,K::Button,58,L"荧光笔"}},true},colors,{L"线宽",{{27,K::Dropdown,66}}},{L"透明度",{{56,K::Slider,124}}},{L"平滑",{{57,K::Dropdown,72}}}};
    case Tool::Text:return {colors,{L"字体",{{80,K::Dropdown,140}}},{L"字号",{{27,K::Dropdown,70},{51,K::Button,30,L"",51}}},{L"背景",{{52,K::Dropdown,100}}},
        {L"对齐",{{53,K::Button,28,L"",53},{54,K::Button,28,L"",54},{55,K::Button,28,L"",55}},true}};
    case Tool::Mosaic:return {{L"模式",{{60,K::Button,44,L"像素"},{61,K::Button,44,L"模糊"}},true},{L"方式",{{62,K::Button,44,L"涂抹"},{63,K::Button,44,L"框选"}},true},
        {L"笔刷",{{64,K::Slider,132}}},{L"强度",{{65,K::Slider,132}}}};
    case Tool::Number:{
        std::vector<PropertyGroup> result{colors,{L"外形",{{67,K::Dropdown,86}}},{L"大小",{{68,K::Dropdown,66}}},{L"组合",{{69,K::Dropdown,92}}},{L"标签",{{81,K::TextField,136}}}};
        if(combo==NumberCombo::Text){result.push_back({L"文字色",{{79,K::Button,40,L"自动"},{72,K::Swatch,22},{73,K::Swatch,22},{74,K::Swatch,22},{75,K::Swatch,22},{76,K::Swatch,22},{77,K::Swatch,22},{70,K::Swatch,22}}});result.push_back({L"字号",{{71,K::Dropdown,58}}});}
        return result;
    }
    default:return {};
    }
}
// Property card metrics (DIP): 34 DIP rows (28 DIP fields), 6 DIP between wrapped rows, 2 DIP between
// fields, and 8 + 1 + 10 DIP around the divider between groups. Nothing is ever stretched.
constexpr float PropertyRow=34,PropertyRowGap=6,PropertyFieldGap=2,PropertyGroupGap=19,PropertyLabelGap=6,PropertySegmentPad=2;
// Caption width estimate for the 12 DIP UI font (CJK ~12 DIP, Latin ~7 DIP), plus the gap to the controls.
float PropertyLabelWidth(LPCWSTR title){float w=0;for(auto p=title;*p;++p)w+=*p<128?7.f:12.f;return w>0?w+PropertyLabelGap:0;}
// 属性栏样式 = 卡片: the caption sits above its controls instead (18 DIP caption band, 28 DIP fields),
// so a group is as wide as the wider of the two. Same gaps and dividers, still never stretched.
constexpr float PropertyCardRow=48,PropertyCardRowGap=10,PropertyCardFieldTop=20;
float PropertyFieldsWidth(const PropertyGroup& g){
    float w=0;for(size_t i=0;i<g.fields.size();++i)w+=g.fields[i].width+(i?PropertyFieldGap:0);
    return w+(g.segmented?2*PropertySegmentPad:0);
}
float PropertyGroupWidth(const PropertyGroup& g,bool card=false){
    const float label=PropertyLabelWidth(g.title);
    return card?std::max(label>0?label-PropertyLabelGap:0.f,PropertyFieldsWidth(g)):label+PropertyFieldsWidth(g);
}
// A single geometry source for drawing, hit testing, sliders and popup anchors. Groups keep their natural
// width and flow left to right; a group that does not fit starts a new left-aligned row (no divider).
std::vector<ui::Control> PropertyControls(Tool tool,NumberCombo combo,Box area,float scale,float* height=nullptr,bool card=false) {
    const auto groups=PropertyGroups(tool,combo);std::vector<ui::Control> result;
    const float available=(area.right-area.left)/scale;
    const auto box=[&](float l,float t,float r,float b){return Box{area.left+l*scale,area.top+t*scale,std::min(area.right,area.left+r*scale),area.top+b*scale};};
    const float row=card?PropertyCardRow:PropertyRow,row_gap=card?PropertyCardRowGap:PropertyRowGap;
    float x=0,y=0;bool row_start=true;
    for(const auto& g:groups){
        const float natural=PropertyGroupWidth(g,card);
        // Tolerance: the card is sized to the exact one-row width, and screen offsets cost float precision.
        if(!row_start&&x+PropertyGroupGap+natural>available+.05f){x=0;y+=row+row_gap;row_start=true;}
        if(!row_start){ui::Control separator;separator.kind=ui::Kind::Separator;separator.bounds=card?box(x+8,y+4,x+9,y+row-2):box(x+8,y+7,x+9,y+27);result.push_back(separator);x+=PropertyGroupGap;}
        // Only a group wider than the whole card is scaled down.
        const float fit=std::min(1.f,std::max(0.f,available-x)/natural);
        const float caption_width=PropertyLabelWidth(g.title)*fit;
        if(caption_width>0){ui::Control caption;caption.kind=ui::Kind::Label;caption.text=g.title;caption.caption=true;
            caption.bounds=card?box(x,y,x+natural*fit,y+17):box(x,y+8,x+caption_width,y+27);result.push_back(caption);}
        // Strip: controls follow the caption on its row. Card: controls start a 28 DIP band under it.
        const float field_y=card?y+PropertyCardFieldTop:y+3;
        float cx=card?x:x+caption_width;const float top=g.segmented?field_y+2:field_y,bottom=g.segmented?field_y+26:field_y+28;
        if(g.segmented){
            ui::Control track;track.kind=ui::Kind::Group;track.bounds=box(cx,field_y,cx+PropertyFieldsWidth(g)*fit,field_y+28);result.push_back(track);cx+=PropertySegmentPad*fit;
        }
        for(size_t i=0;i<g.fields.size();++i){
            const auto& f=g.fields[i];if(i)cx+=PropertyFieldGap*fit;
            ui::Control c;c.id=f.id;c.kind=f.kind;c.text=f.text;c.icon=f.icon;c.square=f.kind==ui::Kind::Swatch;c.rainbow=f.id==28||f.id==70;
            c.bounds=box(cx,top,cx+f.width*fit,bottom);result.push_back(c);cx+=f.width*fit;
        }
        x+=natural*fit;row_start=false;
    }
    if(height)*height=groups.empty()?0:y+row;
    return result;
}
// One-row width of the property groups.
float PropertyNaturalWidth(Tool tool,NumberCombo combo,bool card=false){
    const auto groups=PropertyGroups(tool,combo);float total=0;
    for(size_t i=0;i<groups.size();++i)total+=PropertyGroupWidth(groups[i],card)+(i?PropertyGroupGap:0.f);
    return total;
}
}
Box ToolbarLayout::Extent()const{
    if(!CardVisible())return bounds;
    return {std::min(bounds.left,card.left),std::min(bounds.top,card.top),std::max(bounds.right,card.right),std::max(bounds.bottom,card.bottom)};
}
// Short names for 按钮文字 = 图标 + 文字 (tool grid only).
static std::wstring_view ToolbarShortName(int id){
    switch(id){case 1:return L"矩形";case 2:return L"圆形";case 3:return L"箭头";case 4:return L"铅笔";case 5:return L"文字";case 6:return L"马赛克";
    case 14:return L"序号";case ToolbarMoreId:return L"更多";case ToolbarPropertiesId:return L"属性";default:return {};}
}
const wchar_t* ToolbarActionName(int id) {
    constexpr std::array<LPCWSTR,20> names{L"选择 / 移动 V",L"矩形 R",L"圆形 E",L"箭头 A",L"铅笔 P",L"文字 T",L"马赛克 M",L"撤销 Ctrl+Z",L"重做 Ctrl+Y",L"保存 Ctrl+S",L"复制 Ctrl+C",L"取消 Esc",L"完成 Enter",L"置顶贴图 F3",L"序号",L"识别文字 / 表格",L"录制",L"长截图 L",L"截图翻译",L"更多工具"};
    return id>=0&&id<static_cast<int>(names.size())?names[static_cast<size_t>(id)]:L"";
}
Box ToolbarLayout::Button(int id)const {
    const float s=scale;
    if(IsToolbarToolId(id)||id==ToolbarMoreId||id==ToolbarPropertiesId){
        // Panel tools follow the user's order; "更多" and then "属性" take the slots after them.
        const auto slots=PanelToolSlots();const auto found=std::find(slots.begin(),slots.end(),id);
        if(found==slots.end())return {};
        const int index=static_cast<int>(found-slots.begin());const float w=(tools.right-tools.left)/columns;const float x=tools.left+(index%columns)*w,y=tools.top+(index/columns)*36*s;return {x,y,x+w-4*s,y+32*s};}
    if(!custom.OnPanel(id)||(id==15&&!ocr_available)||((id==16||id==17)&&!recording_available)||(id==18&&!(ocr_available&&recording_available)))return {};
    // 17 = long capture; shares the recording availability (hidden while editing a pin).
    // 18 = screenshot translation (pin + OCR + translate); needs OCR, hidden while editing a pin.
    // The pin (13) and quick actions follow the user's order between the fixed actions.
    std::vector<int> order{0,7,8,9,10};
    for(int action:custom.order)if(!IsToolbarToolId(action)&&custom.OnPanel(action))order.push_back(action);
    order.push_back(11);order.push_back(12);
    const auto slots=[&](int action){if(!custom.OnPanel(action))return 0;return action==15?(ocr_available?1:0):(action==16||action==17)?(recording_available?1:0):action==18?(ocr_available&&recording_available?1:0):1;};
    int total=0,slot=0;bool found=false;
    for(int action:order){total+=slots(action);if(action==id)found=true;else if(!found)slot+=slots(action);}
    if(!found)return {};
    if(grouped){
        // Fixed 32 DIP action slots; a 12 DIP gap (with a divider) between 选择/撤销/重做, the output
        // actions and 取消/完成. Nothing is spread over spare width.
        float x=actions.left;int previous=-1;
        for(int action:order){
            if(!slots(action))continue;
            const int group=(action==0||action==7||action==8)?0:((action==11||action==12)?2:1);
            if(previous>=0&&group!=previous)x+=10*s;previous=group;
            if(action==id)return {x+1*s,actions.top,x+31*s,actions.top+32*s};
            x+=32*s;
        }
        return {};
    }
    const float pitch=(actions.right-actions.left)/total;
    const float x=actions.left+slot*pitch,y=actions.top;return {x+1*s,y+2*s,x+slots(id)*pitch-s,y+28*s};
}
Box ToolbarLayout::Property(int id,Tool tool)const {
    if(tool==Tool::Select)tool=property_tool;
    for(const auto& c:PropertyControls(tool,number_combo,colors,scale,nullptr,PropertyCardStyle()))if(c.id==id)return c.bounds;
    return {};
}
Box ToolbarTooltipReserve(const ToolbarLayout& toolbar,int hover,int property_drag) {
    // Mirrors the tooltip condition in Renderer::Toolbar (buttons 0-18, custom colour 28/70).
    if(property_drag>=0||!((hover>=0&&hover<=18)||hover==28||hover==70))return Box{0,0,0,0};
    const float s=toolbar.scale;const Box a=(hover==28||hover==70)?toolbar.Property(hover):toolbar.Button(hover);
    if(a.right<=a.left||a.bottom<=a.top)return Box{0,0,0,0};
    const float cx=(a.left+a.right)/2,half=std::max((a.right-a.left)/2,170*s);
    return Box{cx-half,a.top-48*s,cx+half,a.bottom+48*s};
}
ToolbarLayout PlaceToolbar(Box selection,RECT monitor,float scale,float expansion,Tool tool,NumberCombo combo,const ToolbarCustomization& custom,bool ocr_available,bool recording_available) {
    // Keep icon hit targets usable on narrow, high-DPI displays.
    scale=std::min(scale,float(monitor.right-monitor.left)/304.f);
    ToolbarLayout t;t.scale=scale;t.property_tool=tool;t.number_combo=combo;t.custom=custom;t.custom.Normalize();const float s=scale;
    t.ocr_available=ocr_available;t.recording_available=recording_available;
    t.properties_open=expansion>.5f;
    // 按钮文字: Text widens the grid (labels need 88 px slots) and may move the actions to their own
    // row; Auto and Icons use 40 px icon slots. 方案 C: the bar is the same for every tool (no widening,
    // no spreading, no growing). A drawing tool's properties open in a separate card next to the bar.
    const auto labels=t.custom.labels;
    const bool compact=labels!=ToolbarLabels::Text,narrow=compact;
    constexpr float textSlot=88.f;
    const int toolSlots=std::max(1,t.PanelSlotCount());const float extra=40.f*float(std::max(0,toolSlots-7));
    int quick=0;for(int id:{13,15,18,17,16})if(t.custom.OnPanel(id))++quick;
    const float monitorWidth=float(monitor.right-monitor.left)/s;
    const float actionWidth=(compact?336.f:308.f)*float(7+quick)/12.f,actionReserve=actionWidth+8;
    // Icon modes hug their content: 36 DIP tool slots, 32 DIP action slots, 12 DIP gaps with dividers
    // between the groups. Only a monitor too narrow for that falls back to the squeezed/wrapped grid.
    int available=0;for(int id:{13,15,18,17,16})if(t.custom.OnPanel(id)&&(id==15?ocr_available:(id==16||id==17)?recording_available:id==18?(ocr_available&&recording_available):true))++available;
    const float groupedActions=32.f*float(7+available)+20.f,groupedWidth=12+36.f*float(toolSlots)+7+groupedActions+11;
    const bool grouped=compact&&groupedWidth<=monitorWidth-16;t.grouped=grouped;
    const float width=grouped?groupedWidth:std::min(compact?640.f+extra:944.f,monitorWidth-16);
    const bool inlineActions=grouped||(width>=(compact?620.f+extra:844.f)&&(labels!=ToolbarLabels::Text||width-24-actionReserve>=textSlot*float(toolSlots)));
    const float compactTools=std::clamp(std::min(640.f+extra,monitorWidth-16)-24-(336.f*float(7+quick)/12.f+8),36.f*float(toolSlots),40.f*float(toolSlots));
    const float toolsWidth=grouped?36.f*float(toolSlots):std::min(width-24-(inlineActions?actionReserve:0),narrow?compactTools:textSlot*float(toolSlots));
    const int columns=std::clamp(int(toolsWidth/(narrow?36.f:textSlot)),1,toolSlots);t.columns=columns;
    const float toolHeight=36.f*float((toolSlots+columns-1)/columns);
    t.tools={12*s,11*s,(12+toolsWidth)*s,(11+toolHeight)*s};
    const float barHeight=18+toolHeight+(inlineActions?0:36);
    // Property card: natural one-row width (wrapping only past the monitor width), 8 DIP side and
    // 6 DIP top/bottom padding, so a one-row strip is 46 DIP tall like the bar's buttons plus padding.
    // 属性栏样式 = 卡片 puts captions above the controls: 8 DIP top/bottom padding, 64 DIP for one row.
    const bool card_style=t.PropertyCardStyle();
    constexpr float cardPad=8,cardGap=8;const float cardTop=card_style?8.f:6.f,cardBottom=card_style?8.f:6.f;
    const float natural=std::ceil(PropertyNaturalWidth(tool,combo,card_style));
    const float inner=std::min(natural,monitorWidth-16-2*cardPad);
    float propertyHeight=0;if(natural>0)PropertyControls(tool,combo,{0,0,inner*s,0},s,&propertyHeight,card_style);
    const float cardWidth=natural>0?inner+2*cardPad:0,cardHeight=natural>0?cardTop+propertyHeight+cardBottom:0;
    const float fullHeight=barHeight+(natural>0?cardGap+cardHeight:0);
    if(fullHeight*s>float(monitor.bottom-monitor.top)-16)return PlaceToolbar(selection,monitor,(float(monitor.bottom-monitor.top)-16)/fullHeight,expansion,tool,combo,custom,ocr_available,recording_available);
    const float actionTop=grouped?11:(inlineActions?12:14+toolHeight);const float actionLeft=grouped?12+36.f*float(toolSlots)+7:std::max(12.f,width-12-actionWidth);
    t.actions=grouped?Box{actionLeft*s,actionTop*s,(width-11)*s,(actionTop+32)*s}:Box{actionLeft*s,actionTop*s,(width-12)*s,(actionTop+30)*s};
    const float left=std::clamp(selection.right-width*s,float(monitor.left)+8*s,std::max(float(monitor.left)+8*s,float(monitor.right)-(width+8)*s));
    float top=selection.bottom+10*s;bool above=false;
    if(top+fullHeight*s>monitor.bottom-8*s&&selection.top-(fullHeight+10)*s>=monitor.top+8*s){top=selection.top-(barHeight+10)*s;above=true;}
    top=std::clamp(top,float(monitor.top)+8*s,std::max(float(monitor.top)+8*s,float(monitor.bottom)-(barHeight+8)*s));
    const auto move=[&](Box& b){b.left+=left;b.right+=left;b.top+=top;b.bottom+=top;};move(t.tools);move(t.actions);
    t.bounds={left,top,left+width*s,top+barHeight*s};
    t.colors={t.bounds.left+12*s,t.bounds.bottom,t.bounds.left+12*s,t.bounds.bottom};
    if(natural>0){
        // Away from the selection; flip to the other side of the bar when it would leave the monitor.
        bool card_above=above;const float span=(cardGap+cardHeight)*s;
        if(!card_above&&t.bounds.bottom+span>float(monitor.bottom)-8*s&&t.bounds.top-span>=float(monitor.top)+8*s)card_above=true;
        if(card_above&&t.bounds.top-span<float(monitor.top)+8*s)card_above=false;
        // Under the active tool's button (or 更多 when the tool is tucked away), kept on the monitor.
        Box anchor=t.Button(ToolbarToolId(tool));if(anchor.right<=anchor.left)anchor=t.Button(ToolbarMoreId);if(anchor.right<=anchor.left)anchor=t.tools;
        const float card_left=std::clamp(anchor.left-cardPad*s,float(monitor.left)+8*s,std::max(float(monitor.left)+8*s,float(monitor.right)-8*s-cardWidth*s));
        const float card_top=card_above?t.bounds.top-span:t.bounds.bottom+cardGap*s;
        t.card_full={card_left,card_top,card_left+cardWidth*s,card_top+cardHeight*s};t.card_above=card_above;
        t.colors={card_left+cardPad*s,card_top+cardTop*s,card_left+(cardPad+inner)*s,card_top+(cardTop+propertyHeight)*s};
        t.card_reveal=std::clamp(expansion,0.f,1.f);const float shown=cardHeight*s*t.card_reveal;
        t.card=card_above?Box{t.card_full.left,t.card_full.bottom-shown,t.card_full.right,t.card_full.bottom}:Box{t.card_full.left,t.card_full.top,t.card_full.right,t.card_full.top+shown};
    }
    return t;
}
std::vector<ui::Control> ToolbarLayout::Controls(Tool tool)const {
    using ui::Kind;std::vector<ui::Control> result;
    ui::Control surface;surface.kind=Kind::Surface;surface.bounds=bounds;result.push_back(surface);

    for(int i=0;i<=ToolbarPropertiesId;++i){if((i>=20&&i!=ToolbarPropertiesId)||(i==15&&!ocr_available)||((i==16||i==17)&&!recording_available)||(i==18&&!(ocr_available&&recording_available)))continue;ui::Control c;c.id=i;c.kind=Kind::Button;c.bounds=Button(i);if(c.bounds.right<=c.bounds.left)continue;c.accessible_name=i==ToolbarPropertiesId?L"属性":ToolbarActionName(i);c.icon=i;
        if(i==ToolbarPropertiesId)c.selected=properties_open;
        // 图标 + 文字: label grid buttons that have room for it; otherwise icon only.
        if(custom.labels==ToolbarLabels::Text){const auto label=ToolbarShortName(i);if(!label.empty()&&c.bounds.right-c.bounds.left>=(41+13.f*float(label.size()))*scale)c.text=label;}
        result.push_back(c);}
    if(grouped){
        // A divider in every 12 DIP gap between neighbouring bar buttons (tools | edit | output | finish).
        std::vector<Box> row;for(const auto& c:result)if(c.kind==Kind::Button)row.push_back(c.bounds);
        std::sort(row.begin(),row.end(),[](const Box& a,const Box& b){return a.left<b.left;});
        for(size_t i=1;i<row.size();++i){
            const Box a=row[i-1],b=row[i];
            if(std::abs(a.top-b.top)>4*scale||b.left-a.right<8*scale)continue;
            const float x=std::round((a.right+b.left)/2),y=(b.top+b.bottom)/2;
            ui::Control divider;divider.kind=Kind::Separator;divider.bounds={x-.5f*scale,y-10*scale,x+.5f*scale,y+10*scale};result.push_back(divider);
        }
    }
    if(tool!=Tool::Select){auto properties=PropertyControls(tool,number_combo,colors,scale,nullptr,PropertyCardStyle());result.insert(result.end(),properties.begin(),properties.end());}
    return result;
}
int ToolbarLayout::Hit(Point p,Tool tool)const{return ui::HitTest(Controls(tool),p);}
std::vector<int> ToolbarLayout::OverflowItems()const{
    std::vector<int> result;
    for(int id:custom.order){
        if(custom.OnPanel(id))continue;
        if((id==15&&!ocr_available)||((id==16||id==17)&&!recording_available)||(id==18&&!(ocr_available&&recording_available)))continue;
        result.push_back(id);
    }
    return result;
}
std::vector<int> ToolbarLayout::PanelToolSlots()const{
    std::vector<int> result;
    for(int id:custom.order)if(IsToolbarToolId(id)&&custom.OnPanel(id))result.push_back(id);
    if(!OverflowItems().empty())result.push_back(ToolbarMoreId);
    if(!custom.auto_properties&&property_tool!=Tool::Select)result.push_back(ToolbarPropertiesId);
    return result;
}
int ToolbarLayout::PanelSlotCount()const{
    int count=0;for(int id:custom.order)if(IsToolbarToolId(id)&&custom.OnPanel(id))++count;
    return count+(custom.overflow.empty()?0:1)+(!custom.auto_properties&&property_tool!=Tool::Select?1:0);
}
int ToolbarToolId(Tool tool){return tool==Tool::Number?14:static_cast<int>(tool);}
bool OpenToolbarMoreMenu(ViewState& state,RECT monitor){
    if(state.busy||!state.selected)return false;
    const auto items=state.toolbar.OverflowItems();if(items.empty())return false;
    ViewState::Dropdown popup;popup.property=ToolbarMoreId;
    const int current=ToolbarToolId(state.tool);
    for(int id:items){popup.items.emplace_back(ToolbarActionName(id),id);if(id==current)popup.selected=static_cast<int>(popup.items.size())-1;}
    popup.hover=popup.selected>=0?popup.selected:0;
    popup.Place(state.toolbar.Button(ToolbarMoreId),monitor,state.toolbar.scale,212.f);state.dropdown=std::move(popup);return true;
}
bool OpenToolbarContextMenu(ViewState& state,int id,RECT monitor){
    if(state.busy||!state.selected||!IsCustomizableToolbarId(id)||!state.toolbar.custom.OnPanel(id))return false;
    ViewState::Dropdown popup;popup.property=ToolbarContextMenuId;
    popup.items.emplace_back(L"收进“更多”菜单",id);
    if(!state.toolbar.custom.overflow.empty())popup.items.emplace_back(L"全部放回面板",ToolbarRestoreAllItem);
    popup.hover=0;
    popup.Place(state.toolbar.Button(id),monitor,state.toolbar.scale,168.f);state.dropdown=std::move(popup);return true;
}
Box ToolbarMorePinBox(const ViewState::Dropdown& popup,int index){
    const auto row=popup.Row(index);const float s=popup.scale;
    return {row.right-30*s,row.top+3*s,row.right-4*s,row.bottom-3*s};
}
std::optional<uint32_t> ToolbarColor(int id) {
    for(size_t i=0;i<AnnotationColors.size();++i){
        if(id==AnnotationColorIds[i]||id==AnnotationFillIds[i]||id==72+int(i))return AnnotationColors[i];
    }
    return std::nullopt;
}
std::optional<uint32_t> PenToolbarColor(int id,const ViewState&) {
    return ToolbarColor(id);
}
std::vector<ui::Control> ToolbarControls(const ViewState& state,const Document& document) {
    auto controls=state.toolbar.Controls(state.PropertyTool()==Tool::Select?state.closing_tool:state.PropertyTool());
    for(auto& c:controls) {
        c.enabled=c.enabled&&!state.busy;
        if(c.id==16)c.enabled=c.enabled&&state.toolbar.recording_available&&state.selected&&state.selection.right-state.selection.left>=8&&state.selection.bottom-state.selection.top>=8;
        if(c.id==17)c.enabled=c.enabled&&state.toolbar.recording_available&&state.selected&&state.selection.right-state.selection.left>=32&&state.selection.bottom-state.selection.top>=32;
        // Property controls work once the card has revealed them (the card grows out of the bar).
        if(c.id>=20&&c.id!=ToolbarPropertiesId){const auto& card=state.toolbar.card;
            c.enabled=c.enabled&&state.PropertyTool()!=Tool::Select&&state.toolbar.CardVisible()&&c.bounds.top>=card.top-.5f&&c.bounds.bottom<=card.bottom+.5f;}
        if(c.id>=0&&c.id<=6)c.selected=c.id==static_cast<int>(state.tool);
        if(c.id==14)c.selected=state.tool==Tool::Number;
        if(c.id==ToolbarMoreId){
            // The active tool lives in the menu: the button shows its icon and stays selected.
            const int current=ToolbarToolId(state.tool);const bool hidden=IsToolbarToolId(current)&&!state.toolbar.custom.OnPanel(current);
            c.selected=hidden||(state.dropdown.Open()&&state.dropdown.property==ToolbarMoreId);if(hidden)c.icon=current;
        }
        if(c.id==67){c.text=state.number_label.empty()?NumberShapeNames[static_cast<int>(state.number_shape)]:L"自动圆角";c.enabled=c.enabled&&state.number_label.empty();}
        if(c.id==81)c.text=state.number_label.empty()?L"自动序号（留空）":state.number_label;
        if(c.id==68)c.text=std::to_wstring(int(state.number_size))+L" px";
        if(c.id==69)c.text=NumberComboNames[static_cast<int>(state.number_combo)];
        if(c.id>=72&&c.id<=77)c.selected=state.number_text_preset==-1&&ToolbarColor(c.id)==state.number_text_color;
        if(c.id==79)c.selected=state.number_text_preset==-2;
        if(c.id==71)c.text=std::to_wstring(int(state.number_text_size));
        if(c.id==7)c.enabled=c.enabled&&(state.polyline_active||document.CanUndo());if(c.id==8)c.enabled=c.enabled&&!state.polyline_active&&document.CanRedo();c.primary=c.id==12;
        if(auto color=PenToolbarColor(c.id,state))c.color=*color;
        if(c.kind==ui::Kind::Swatch&&c.id<40)c.selected=c.color==state.ActiveColor();
        if(c.id==24)c.text=std::to_wstring(int(state.mosaic_brush))+L" px";
        if(c.id==27)c.text=std::to_wstring(int(state.PropertyTool()==Tool::Text?state.font_size:(state.PropertyTool()==Tool::Mosaic?state.mosaic_cell:state.ActiveWidth())))+L" px";
        if(c.id==40)c.selected=!state.fill_color;
        if((c.id>=41&&c.id<=45)||c.id==82){c.square=true;c.selected=state.fill_color==c.color;}
        if(c.id==46){c.value=1-state.fill_opacity;c.text=std::to_wstring(int(std::round(c.value*100)))+L"%";c.enabled=c.enabled&&state.fill_color.has_value();}
        if(c.id==47){c.value=state.corner_radius/32;c.text=std::to_wstring(int(state.corner_radius))+L" px";}
        if(c.id==48){constexpr LPCWSTR names[]={L"实线",L"虚线",L"点线",L"点划线"};c.text=names[static_cast<int>(state.line_style)];}
        if(c.id==49)c.text=ArrowTypeNames[static_cast<int>(state.arrow_type)];
        if(c.id==66)c.text=ArrowHeadNames[static_cast<int>(state.arrow_head)];
        if(c.id==50)c.text=std::to_wstring(int(state.arrow_size))+L" px";
        if(c.id==51)c.selected=state.text_bold;
        if(c.id==80)c.text=TextFontName(state.font_family);
        if(c.id==52){constexpr LPCWSTR names[]={L"无背景",L"旧版白底",L"旧版深底",L"旧版黄底",L"同色浅底",L"同色深底",L"旧版自动",L"半透明自动",L"半透明浅色",L"半透明深色"};c.text=names[static_cast<int>(state.text_background)];}
        if(c.id>=53&&c.id<=55)c.selected=static_cast<int>(state.text_align)==c.id-53;
        if(c.id==56){c.value=1-state.PenOpacity();c.text=std::to_wstring(int(std::round(c.value*100)))+L"%";}
        if(c.id==83)c.selected=!state.pen_straight&&!state.pen_polyline;
        if(c.id==84)c.selected=state.pen_straight&&!state.pen_polyline;
        if(c.id==85)c.selected=state.pen_polyline;
        if(c.id==86){c.selected=state.pen_snap;c.text=state.pen_snap?L"捕捉 开":L"捕捉 关";c.enabled=c.enabled&&state.pen_straight;}
        if(c.id==58||c.id==59)c.selected=(c.id==59)==(state.pen_mode==PenMode::Highlighter);
        if(c.id==57){constexpr LPCWSTR names[]={L"无平滑",L"标准",L"高平滑"};c.text=names[static_cast<int>(state.pen_smoothing)];c.enabled=c.enabled&&!state.pen_straight;}
        if(c.id==60||c.id==61)c.selected=static_cast<int>(state.mosaic_mode)==c.id-60;
        if(c.id==62||c.id==63)c.selected=static_cast<int>(state.mosaic_method)==c.id-62;
        if(c.id==64){c.value=(state.mosaic_brush-4)/124;c.text=std::to_wstring(int(state.mosaic_brush))+L" px";c.enabled=c.enabled&&state.mosaic_method==MosaicMethod::Brush;}
        if(c.id==65){c.value=(state.mosaic_strength-1)/99;c.text=std::to_wstring(int(state.mosaic_strength))+L"%";}
    }
    return controls;
}
bool SetToolbarSlider(ViewState& state,int id,Point point) {
    if(state.busy||!state.selected)return false;
    if(state.PropertyTool()==Tool::Mosaic&&id==64&&state.mosaic_method==MosaicMethod::Brush){state.mosaic_brush=std::round(4+124*ui::SliderValue(state.toolbar.Property(id),point,state.toolbar.scale));return true;}
    if(state.PropertyTool()==Tool::Mosaic&&id==65){state.mosaic_strength=std::round(1+99*ui::SliderValue(state.toolbar.Property(id),point,state.toolbar.scale));return true;}
    if(id==56&&state.PropertyTool()==Tool::Pen){state.PenOpacity()=1-ui::SliderValue(state.toolbar.Property(id,state.PropertyTool()),point,state.toolbar.scale);return true;}
    if(id==46&&(state.PropertyTool()==Tool::Rectangle||state.PropertyTool()==Tool::Ellipse)&&state.fill_color){state.fill_opacity=1-ui::SliderValue(state.toolbar.Property(id),point,state.toolbar.scale);return true;}
    if(id==47&&state.PropertyTool()==Tool::Rectangle){state.corner_radius=std::round(ui::SliderValue(state.toolbar.Property(id),point,state.toolbar.scale)*32);return true;}
    return false;
}
bool AdjustPropertyValue(ViewState& state,int id,int steps) {
    if(state.busy||!state.selected||state.PropertyTool()==Tool::Select||steps==0)return false;
    if(state.PropertyTool()==Tool::Number&&state.number_combo==NumberCombo::Text&&id==71){state.number_text_size=std::clamp(state.number_text_size+float(steps),8.f,64.f);return true;}
    if(state.PropertyTool()==Tool::Number&&id==68){state.number_size=std::clamp(state.number_size+float(steps),16.f,96.f);return true;}
    if(state.PropertyTool()==Tool::Mosaic&&id==64&&state.mosaic_method==MosaicMethod::Brush){state.mosaic_brush=std::clamp(state.mosaic_brush+float(steps),4.f,128.f);return true;}
    if(state.PropertyTool()==Tool::Mosaic&&id==65){state.mosaic_strength=std::clamp(state.mosaic_strength+float(steps),1.f,100.f);return true;}
    if(id==56&&state.PropertyTool()==Tool::Pen){state.PenOpacity()=std::clamp(state.PenOpacity()-float(steps)*.01f,0.f,1.f);return true;}
    if(id==50&&state.PropertyTool()==Tool::Arrow){state.arrow_size=std::clamp(state.arrow_size+float(steps),4.f,64.f);return true;}
    if(id==46&&(state.PropertyTool()==Tool::Rectangle||state.PropertyTool()==Tool::Ellipse)&&state.fill_color){state.fill_opacity=std::clamp(state.fill_opacity-float(steps)*.01f,0.f,1.f);return true;}
    if(id==47&&state.PropertyTool()==Tool::Rectangle){state.corner_radius=std::clamp(state.corner_radius+float(steps),0.f,32.f);return true;}
    if(id<24||id>27)return false;
    float* value=&state.ActiveWidth();float limit=state.Highlighting()?64.f:16.f;
    if(state.PropertyTool()==Tool::Text){value=&state.font_size;limit=96;}
    if(state.PropertyTool()==Tool::Mosaic){value=id==27?&state.mosaic_cell:&state.mosaic_brush;limit=id==27?64.f:128.f;}
    const float minimum=state.PropertyTool()==Tool::Text?8.f:(state.PropertyTool()==Tool::Mosaic?4.f:1.f);
    *value=std::clamp(*value+float(steps),minimum,limit);return true;
}
}