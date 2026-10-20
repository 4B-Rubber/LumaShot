#include "ui/render.h"
#include "model/arrow.h"
#include "ui/fonts.h"
#include "export/png.h"
#include <algorithm>
#include <iostream>

using namespace lumashot;
// A control sits inside the bar or inside the property card (方案 C).
static bool InToolbar(const ToolbarLayout& t,Box p){
    const auto in=[&p](Box b){return Contains(b,{p.left,p.top})&&Contains(b,{p.right,p.bottom});};
    return in(t.bounds)||(t.card_full.right>t.card_full.left&&in(t.card_full));
}
int main(int argc,char** argv) {
    if(FAILED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED)))return 2;
    int failures=0;
    const auto expect=[&](bool ok,const char* name){std::cout<<(ok?"[PASS] ":"[FAIL] ")<<name<<'\n';if(!ok)++failures;};
    try {
        if(argc>=3&&std::string_view(argv[1])=="--record-style"){
            // Preview renders of 属性栏样式 (窄条 / 卡片); synthetic demo content only.
            const std::filesystem::path folder=argv[2];std::filesystem::create_directories(folder);
            for(bool dark:{false,true})for(Tool tool:{Tool::Arrow,Tool::Pen,Tool::Text,Tool::Number})for(bool card:{false,true}){
                ToolbarCustomization custom;custom.property_style=card?PropertyBarStyle::Card:PropertyBarStyle::Strip;Renderer renderer;
                SavePng(renderer.Demo(true,dark,tool,false,-1,-1,false,&custom),folder/(std::string(card?"card-":"strip-")+std::to_string(static_cast<int>(tool))+(dark?"-dark.png":"-light.png")));
            }
            CoUninitialize();return 0;
        }
        if(argc>=2&&std::string_view(argv[1])=="--record-icon"){
            for(float scale:{1.f,1.5f,2.f})for(int width:{480,800,1280})for(bool ocr:{false,true}){
                ViewState state;state.selected=true;state.selection={-400,100,-50,400};state.toolbar=PlaceToolbar(state.selection,{-width,0,0,800},scale);state.toolbar.ocr_available=ocr;
                const auto controls=ToolbarControls(state,Document{});const auto icon=std::find_if(controls.begin(),controls.end(),[](const auto& c){return c.id==16;});
                expect(icon!=controls.end()&&icon->kind==ui::Kind::Button&&icon->icon==16&&icon->text.empty(),"single recording icon, no label or dropdown arrow");
                const auto b=icon->bounds,cancel=state.toolbar.Button(11);expect(state.toolbar.Hit({(b.left+b.right)/2,(b.top+b.bottom)/2},Tool::Select)==16&&b.right<cancel.left&&std::abs((b.right-b.left)-(cancel.right-cancel.left))<.01f,"record icon uses one action slot and correct DPI/negative-origin hit target");
                expect(!OpenToolbarDropdown(state,16,{-width,0,0,800})&&!state.dropdown.Open(),"recording dropdown removed");
                state.busy=true;expect(ui::HitTest(ToolbarControls(state,Document{}),{(b.left+b.right)/2,(b.top+b.bottom)/2})!=16,"busy recording icon disabled");
                state.busy=false;state.selection.right=state.selection.left+7;expect(ui::HitTest(ToolbarControls(state,Document{}),{(b.left+b.right)/2,(b.top+b.bottom)/2})!=16,"tiny recording selection disabled");
                state.toolbar.recording_available=false;const auto pinned=state.toolbar.Controls(Tool::Select);expect(std::none_of(pinned.begin(),pinned.end(),[](const auto& c){return c.id==16;}),"pinned-image editor hides recording icon");
            }
            if(argc>=3){const std::filesystem::path folder=argv[2];std::filesystem::create_directories(folder);for(bool dark:{false,true})for(bool hover:{false,true}){Renderer renderer;auto frame=renderer.Demo(true,dark,Tool::Select,false,hover?16:-1);SavePng(frame,folder/(std::string(dark?"dark":"light")+(hover?"-hover.png":".png")));}}
            CoUninitialize();return failures?1:0;
        }
        for(float scale:{1.0f,1.5f,2.0f})for(int width:{480,800,1280})for(Tool tool:{Tool::Select,Tool::Rectangle,Tool::Ellipse,Tool::Arrow,Tool::Pen,Tool::Text,Tool::Mosaic,Tool::Number})for(auto combo:{NumberCombo::Plain,NumberCombo::Text}) {
            const auto t=PlaceToolbar({-300,200,-40,600},{-width,0,0,1080},scale,1,tool,combo);
            bool valid=t.bounds.top>=0&&t.bounds.bottom<=1080&&t.bounds.left>=-width&&t.bounds.right<=0;
            const auto card=t.card_full;
            valid=valid&&(tool==Tool::Select)==(card.right<=card.left)&&(card.right<=card.left||(card.top>=0&&card.bottom<=1080&&card.left>=-width&&card.right<=0&&
                !(std::min(card.right,t.bounds.right)>std::max(card.left,t.bounds.left)&&std::min(card.bottom,t.bounds.bottom)>std::max(card.top,t.bounds.top))));
            const auto controls=t.Controls(tool);
            const auto pin=t.Button(13),ocr=t.Button(15),record=t.Button(16),cancel=t.Button(11);
            const auto longshot=t.Button(17);
            valid=valid&&pin.right<ocr.left&&ocr.right<longshot.left&&longshot.right<record.left&&std::abs((longshot.right-longshot.left)-(record.right-record.left))<.01f&&record.right<cancel.left&&record.right-record.left>=18*t.scale&&std::abs((record.right-record.left)-(cancel.right-cancel.left))<.01f&&ocr.right-ocr.left>=18*t.scale;
            const auto entry=std::find_if(controls.begin(),controls.end(),[](const ui::Control& c){return c.id==15;});
            valid=valid&&entry!=controls.end()&&entry->text.empty()&&entry->icon==15&&!entry->accessible_name.empty();
            for(const auto& c:controls)if(c.id>=0&&c.id<=18)valid=valid&&c.icon==c.id&&c.text.empty()&&!c.accessible_name.empty();
            for(const auto& c:controls){const auto p=c.bounds;valid=valid&&InToolbar(t,p);if(c.Interactive())valid=valid&&t.Hit({(p.left+p.right)/2,(p.top+p.bottom)/2},tool)==c.id;}
            for(size_t a=0;a<controls.size();++a)for(size_t b=a+1;b<controls.size();++b)if(controls[a].Interactive()&&controls[b].Interactive()){
                const auto x=controls[a].bounds,y=controls[b].bounds;valid=valid&&!(std::min(x.right,y.right)>std::max(x.left,y.left)&&std::min(x.bottom,y.bottom)>std::max(x.top,y.top));
            }
            expect(valid,"property layout and hit regions fit narrow/negative-origin/DPI display");
        }
        for(auto tool:{Tool::Rectangle,Tool::Ellipse,Tool::Arrow,Tool::Pen,Tool::Text,Tool::Mosaic,Tool::Number})for(auto combo:{NumberCombo::Plain,NumberCombo::Text}){
            const auto layout=PlaceToolbar({100,100,1100,500},{0,0,1280,800},1,1,tool,combo);
            // Rows are 34 DIP apart plus a 6 DIP gap; fields are 28 DIP tall (24 DIP inside a segmented track).
            const auto controls=layout.Controls(tool);float centre=-1,max_offset=0;bool aligned=true;
            for(const auto& c:controls)if(c.Interactive()&&c.id>=20){const float mid=(c.bounds.top+c.bounds.bottom)/2;if(centre<0)centre=mid;const float offset=mid-centre;max_offset=std::max(max_offset,offset);
                const float h=c.bounds.bottom-c.bounds.top;aligned=aligned&&(std::abs(offset)<.01f||std::abs(offset-40)<.01f)&&(std::abs(h-28)<.01f||std::abs(h-24)<.01f);}
            expect(aligned&&max_offset<.01f&&layout.bounds.bottom-layout.bounds.top<=54.01f&&std::abs(layout.card_full.bottom-layout.card_full.top-46)<.01f,"wide properties use one compact 46 DIP row with centred fields");
            // No stretching: the card is exactly as wide as its content.
            float right=0;for(const auto& c:controls)if(c.bounds.top>=layout.card_full.top-.01f)right=std::max(right,c.bounds.right);
            expect(std::abs(layout.card_full.right-right-8)<.6f,"property card hugs its controls (8 DIP padding)");
        }
        {   // 属性栏样式 = 卡片: captions above the controls, still content-sized, same controls as the strip.
            ToolbarCustomization card_style;card_style.property_style=PropertyBarStyle::Card;
            const auto ids=[](const std::vector<ui::Control>& cs){std::vector<int> r;for(const auto& c:cs)if(c.Interactive()&&c.id>=20)r.push_back(c.id);std::sort(r.begin(),r.end());return r;};
            for(auto tool:{Tool::Rectangle,Tool::Ellipse,Tool::Arrow,Tool::Pen,Tool::Text,Tool::Mosaic,Tool::Number})for(auto combo:{NumberCombo::Plain,NumberCombo::Text}){
                const auto strip=PlaceToolbar({100,100,1100,500},{0,0,1280,800},1,1,tool,combo);
                const auto card=PlaceToolbar({100,100,1100,500},{0,0,1280,800},1,1,tool,combo,card_style);
                const auto cs=card.Controls(tool);const float band=card.colors.top+20;
                bool fields=true,captions=true,hits=true;int caption_count=0;float right=0;
                for(const auto& c:cs){
                    if(c.bounds.top>=card.card_full.top-.01f)right=std::max(right,c.bounds.right);
                    if(c.kind==ui::Kind::Label&&c.caption){++caption_count;captions=captions&&c.bounds.top>=card.colors.top-.01f&&c.bounds.bottom<=band-2.99f;}
                    if(c.Interactive()&&c.id>=20){const float h=c.bounds.bottom-c.bounds.top;
                        fields=fields&&((std::abs(c.bounds.top-band)<.01f&&std::abs(h-28)<.01f)||(std::abs(c.bounds.top-band-2)<.01f&&std::abs(h-24)<.01f));
                        hits=hits&&card.Hit({(c.bounds.left+c.bounds.right)/2,(c.bounds.top+c.bounds.bottom)/2},tool)==c.id&&InToolbar(card,c.bounds);
                        const auto anchor=card.Property(c.id);hits=hits&&std::abs(anchor.left-c.bounds.left)<.01f&&std::abs(anchor.top-c.bounds.top)<.01f;}
                }
                expect(ids(cs)==ids(strip.Controls(tool))&&caption_count>0,"card style offers exactly the strip's property controls");
                expect(fields&&captions&&std::abs(card.card_full.bottom-card.card_full.top-64)<.01f,"card style puts captions above one 28 DIP field band (64 DIP card)");
                expect(hits&&std::abs(card.card_full.right-right-8)<.6f,"card style hugs its content and keeps hit targets and popup anchors");
                expect(card.bounds.right==strip.bounds.right&&card.bounds.bottom==strip.bounds.bottom,"property style never changes the main bar");
            }
            // Narrow, scaled and negative-origin monitors: wrapped card rows stay inside, hittable, non-overlapping.
            bool valid=true;
            for(float scale:{1.0f,1.5f,2.0f})for(int width:{480,800,1280})for(Tool tool:{Tool::Pen,Tool::Text,Tool::Number}){
                const auto t=PlaceToolbar({-300,200,-40,600},{-width,0,0,1080},scale,1,tool,NumberCombo::Text,card_style);const auto cs=t.Controls(tool);
                valid=valid&&t.card_full.left>=-width&&t.card_full.right<=0&&t.card_full.top>=0&&t.card_full.bottom<=1080;
                for(const auto& c:cs){if(!InToolbar(t,c.bounds))valid=false;if(c.Interactive()&&t.Hit({(c.bounds.left+c.bounds.right)/2,(c.bounds.top+c.bounds.bottom)/2},tool)!=c.id)valid=false;}
                for(size_t a=0;a<cs.size();++a)for(size_t b=a+1;b<cs.size();++b)if(cs[a].Interactive()&&cs[b].Interactive()){
                    const auto x=cs[a].bounds,y=cs[b].bounds;if(std::min(x.right,y.right)>std::max(x.left,y.left)&&std::min(x.bottom,y.bottom)>std::max(x.top,y.top))valid=false;}
            }
            expect(valid,"card style wraps safely on narrow/negative-origin/DPI displays");
        }
        {   // 方案 C: the bar never changes with the tool; properties open in a card beside it.
            const RECT monitor{0,0,1280,800};const Box sel{100,100,1100,500};
            const auto idle=PlaceToolbar(sel,monitor,1,0,Tool::Select),open=PlaceToolbar(sel,monitor,1,1,Tool::Arrow),closed=PlaceToolbar(sel,monitor,1,0,Tool::Arrow);
            const auto same=[](Box a,Box b){return std::abs(a.left-b.left)<.01f&&std::abs(a.top-b.top)<.01f&&std::abs(a.right-b.right)<.01f&&std::abs(a.bottom-b.bottom)<.01f;};
            expect(same(idle.bounds,open.bounds)&&same(idle.Button(1),open.Button(1))&&same(idle.Button(12),open.Button(12))&&!idle.CardVisible(),"the bar is identical with and without a drawing tool");
            // Where the card has room, it starts under the active tool (8 DIP padding before the first control).
            const auto roomy=PlaceToolbar({100,100,700,500},monitor,1,1,Tool::Arrow);const auto arrow=roomy.Button(3);
            expect(open.CardVisible()&&std::abs(open.card_full.top-(open.bounds.bottom+8))<.01f&&std::abs(roomy.card_full.left-(arrow.left-8))<.01f&&same(open.card,open.card_full),"card opens below the bar under the active tool");
            expect(open.colors.left>=open.card_full.left&&open.colors.right<=open.card_full.right&&open.colors.top>=open.card_full.top&&open.colors.bottom<=open.card_full.bottom,"property controls live inside the card");
            const Point inside{(open.card.left+open.card.right)/2,(open.card.top+open.card.bottom)/2};
            expect(Contains(open,inside)&&!closed.CardVisible()&&!Contains(closed,inside),"the visible card is part of the toolbar, a collapsed one is not");
            const auto half=PlaceToolbar(sel,monitor,1,.5f,Tool::Arrow);
            expect(half.CardVisible()&&std::abs(half.card.top-half.card_full.top)<.01f&&half.card.bottom<half.card_full.bottom-1,"the card grows out of the bar while expanding");
            ViewState state;state.selected=true;state.tool=Tool::Arrow;state.selection=sel;state.toolbar=open;
            const auto enabled=[&](int id){for(const auto& c:ToolbarControls(state,Document{}))if(c.id==id)return c.enabled;return false;};
            const bool on=enabled(27);state.toolbar=closed;
            expect(on&&!enabled(27),"property controls work only once the card shows them");
            const auto low=PlaceToolbar({100,100,1100,780},monitor,1,1,Tool::Arrow);
            expect(low.card_above&&low.card_full.bottom<=low.bounds.top-7.99f&&low.card_full.top>=8,"card flips above the bar near the bottom of the monitor");
            const auto edge=PlaceToolbar({900,100,1270,500},monitor,1,1,Tool::Pen);
            expect(edge.card_full.right<=1272.01f&&edge.card_full.left>=7.99f,"card stays on the monitor");
        }
        for(float scale:{1.f,1.5f,2.f}){
            const auto compact=PlaceToolbar({100,100,1100,500},{-1280,0,1280,1080},scale,0,Tool::Select);
            expect(compact.grouped&&std::abs((compact.bounds.right-compact.bounds.left)/compact.scale-686)<.01f&&compact.columns==7,"selection toolbar hugs seven tools and twelve actions (686 DIP)");
            {   // Fixed slots: equal steps inside a group, one 12 DIP gap with a divider between groups.
                const auto step=[&](int a,int b){return (compact.Button(b).left-compact.Button(a).right)/compact.scale;};
                expect(std::abs(step(1,2)-4)<.01f&&std::abs(step(7,8)-2)<.01f&&std::abs(step(9,10)-2)<.01f&&std::abs(step(8,9)-12)<.01f&&std::abs(step(16,11)-12)<.01f&&std::abs(step(14,0)-12)<.01f,"bar uses fixed slots and 12 DIP group gaps");
                int dividers=0;for(const auto& c:compact.Controls(Tool::Select))if(c.kind==ui::Kind::Separator&&Contains(compact.bounds,{c.bounds.left,c.bounds.top}))++dividers;
                expect(dividers==3,"three dividers: tools | edit | output | finish");
                const auto pinEdit=PlaceToolbar({100,100,1100,500},{-1280,0,1280,1080},scale,0,Tool::Select,NumberCombo::Plain,ToolbarCustomization{},true,false);
                expect(std::abs((compact.bounds.right-compact.bounds.left)-(pinEdit.bounds.right-pinEdit.bounds.left)-3*32*scale)<.01f&&std::abs(pinEdit.Button(12).right-(pinEdit.bounds.right-12*scale))<.01f,"unavailable quick actions leave no gap");}
            const auto tool=compact.Button(1),action=compact.Button(12);
            expect(tool.right-tool.left>=32*compact.scale&&action.right-action.left>=26*compact.scale&&std::abs(tool.top-action.top)<4*compact.scale,"compact row preserves usable tool and action hit targets");
            for(Tool drawing:{Tool::Rectangle,Tool::Arrow,Tool::Pen}){
                const auto expanded=PlaceToolbar({100,100,1100,500},{-1280,0,1280,1080},scale,1,drawing);bool same=expanded.columns==compact.columns;
                for(int id:{1,2,3,4,5,6}){const auto a=compact.Button(id),b=expanded.Button(id);
                    same=same&&std::abs((a.left-compact.bounds.left)-(b.left-expanded.bounds.left))<.01f&&std::abs((a.right-a.left)-(b.right-b.left))<.01f;}
                expect(same&&expanded.Button(1).right-expanded.Button(1).left<=37*scale,"a drawing tool keeps the 40 DIP tool slots instead of spreading them");
                expect(expanded.Button(12).right<=expanded.bounds.right-11*scale&&expanded.Button(12).right>=expanded.bounds.right-14*scale,"actions stay right-aligned on the wide toolbar");
            }
            const auto narrow=PlaceToolbar({-300,100,-20,500},{-480,0,0,1080},scale,0,Tool::Select);
            expect((narrow.bounds.right-narrow.bounds.left)/narrow.scale<=640&&narrow.Button(0).top>=narrow.tools.bottom,"narrow selection toolbar wraps actions below icon tools");
        }
        {
            ViewState record;record.selected=true;record.selection={-400,200,-100,400};
            record.toolbar=PlaceToolbar(record.selection,{-800,0,0,600},1.5f);
            const auto controls=record.toolbar.Controls(Tool::Select);
            const auto icon=std::find_if(controls.begin(),controls.end(),[](const auto& c){return c.id==16;});
            expect(icon!=controls.end()&&icon->kind==ui::Kind::Button&&icon->icon==16&&icon->text.empty(),"record entry is one icon button without text or dropdown");
            expect(!OpenToolbarDropdown(record,16,{-800,0,0,600}),"record entry has no video/GIF dropdown");
            const auto longshot=std::find_if(controls.begin(),controls.end(),[](const auto& c){return c.id==17;});
            expect(longshot!=controls.end()&&longshot->kind==ui::Kind::Button&&longshot->icon==17&&longshot->text.empty(),"long capture entry is one icon button");
            record.toolbar.recording_available=false;
            const auto pinned=record.toolbar.Controls(Tool::Select);expect(std::none_of(pinned.begin(),pinned.end(),[](const auto& c){return c.id==16;}),"pinned-image annotation omits recording entry");
            expect(std::none_of(pinned.begin(),pinned.end(),[](const auto& c){return c.id==17;}),"pinned-image annotation omits long capture entry");
        }
        ViewState state;state.selected=true;state.tool=Tool::Text;
        state.toolbar=PlaceToolbar({100,100,1100,500},{0,0,1280,800},1,1,Tool::Text);
        expect(OpenToolbarDropdown(state,80,{0,0,1280,800})&&state.dropdown.items.size()==TextFonts().size(),"font dropdown lists available common system fonts");
        ChooseToolbarDropdown(state,static_cast<int>(TextFonts().size())-1);expect(state.font_family==TextFonts().back().family,"font selection stores the actual font family");
        expect(AdjustPropertyValue(state,24,4)&&state.font_size==28&&state.width==3,"text adjustment changes font size only");
        AdjustPropertyValue(state,27,-1000);expect(state.font_size==8,"font lower bound");
        state.tool=Tool::Mosaic;AdjustPropertyValue(state,24,8);
        expect(state.mosaic_brush==40&&state.mosaic_cell==12,"brush adjustment preserves mosaic cell size");
        AdjustPropertyValue(state,27,4);expect(state.mosaic_cell==16&&state.mosaic_brush==40,"cell adjustment preserves brush size");
        state.tool=Tool::Pen;AdjustPropertyValue(state,27,1000);expect(state.width==16,"stroke width upper bound");
        state.pen_mode=PenMode::Highlighter;
        expect(state.ActiveWidth()==24&&state.ActiveColor()==0xffe8b339&&state.PenOpacity()==.4f,"highlighter defaults to a broad translucent gold stroke");
        AdjustPropertyValue(state,27,1000);expect(state.ActiveWidth()==64&&state.width==16,"highlighter width clamps independently");
        AdjustPropertyValue(state,56,20);expect(std::abs(state.PenOpacity()-.2f)<.001f&&state.pen_opacity==1,"highlighter opacity is independent");
        state.toolbar=PlaceToolbar({100,100,400,400},{0,0,1280,800},1,1,state.tool);
        expect(OpenToolbarDropdown(state,27,{0,0,1280,800})&&state.dropdown.items[3].second==24,"highlighter menu offers broad widths");ChooseToolbarDropdown(state,3);
        state.ActiveColor()=0xffff68c8;state.tool=Tool::Rectangle;
        expect(state.ActiveWidth()==16&&state.ActiveColor()==state.color,"other tools retain their original style");
        state.tool=Tool::Pen;expect(state.ActiveWidth()==24&&state.ActiveColor()==0xffff68c8,"returning to highlighter restores custom style");
        state.pen_mode=PenMode::Normal;
        state.tool=Tool::Select;expect(!AdjustPropertyValue(state,27,1),"selection tool has no numeric properties");
        state.tool=Tool::Text;state.busy=true;expect(!AdjustPropertyValue(state,27,1),"busy state ignores property edits");
        state.busy=false;state.tool=Tool::Arrow;state.arrow_size=12;state.width=3;
        AdjustPropertyValue(state,50,1000);expect(state.arrow_size==64&&state.width==3,"arrow size clamps independently of line width");
        AdjustPropertyValue(state,50,-1000);expect(state.arrow_size==4,"arrow size has a safe lower bound");
        for(auto tool:{Tool::Rectangle,Tool::Arrow,Tool::Text}){
            state.tool=tool;state.toolbar=PlaceToolbar({0,0,300,300},{0,0,1280,1080},1,1,state.tool);
            Document document;const auto controls=ToolbarControls(state,document);
            for(int id:(tool==Tool::Arrow?std::vector<int>{48,49,50}:(tool==Tool::Rectangle?std::vector<int>{48}:std::vector<int>{27,80}))){
                const auto b=state.toolbar.Property(id,tool);
                expect(ui::HitTest(controls,{(b.left+b.right)/2,(b.top+b.bottom)/2})==id,"new property uses the visible control hit area");
            }
        }
        if(argc==2) {
            state.tool=Tool::Arrow;state.toolbar=PlaceToolbar({100,100,400,400},{0,0,1280,800},1,1,state.tool);
            expect(OpenToolbarDropdown(state,49,{0,0,1280,800})&&state.dropdown.items.size()==6,"arrow type menu exposes six types");ChooseToolbarDropdown(state,3);
            expect(OpenToolbarDropdown(state,66,{0,0,1280,800})&&state.dropdown.items.size()==11,"arrow head menu exposes eleven heads");ChooseToolbarDropdown(state,9);
            expect(state.arrow_type==ArrowType::HandDrawn&&state.arrow_head==ArrowHead::Slanted,"arrow type and head can be combined independently");
            state.tool=Tool::Mosaic;state.mosaic_method=MosaicMethod::Brush;state.mosaic_strength=70;
            AdjustPropertyValue(state,65,1000);expect(state.mosaic_strength==100,"mosaic strength clamps at one hundred percent");
            AdjustPropertyValue(state,65,-1000);expect(state.mosaic_strength==1,"mosaic strength keeps a nonzero effect");
            state.toolbar=PlaceToolbar({100,100,400,400},{0,0,1280,800},1,1,state.tool);Document mosaic_document;
            for(int id:{60,61,62,63,64,65}){const auto b=state.toolbar.Property(id,Tool::Mosaic);expect(ui::HitTest(ToolbarControls(state,mosaic_document),{(b.left+b.right)/2,(b.top+b.bottom)/2})==id,"mosaic option hit area matches painting");}
            state.tool=Tool::Pen;state.pen_opacity=1;AdjustPropertyValue(state,56,80);expect(std::abs(state.pen_opacity-.2f)<.001f,"pen transparency maps to inverse stroke opacity");
            expect(OpenToolbarDropdown(state,57,{-1280,-200,0,700}),"pen smoothing opens themed dropdown");ChooseToolbarDropdown(state,2);expect(state.pen_smoothing==PenSmoothing::High,"pen smoothing dropdown applies high smoothing");
            for(float scale:{1.f,1.5f,2.f})for(int id:{24,27,48,49,50}){
                state.tool=id==24?Tool::Mosaic:(id==27?Tool::Text:Tool::Arrow);
                const RECT monitor{-1280,-200,0,700};state.toolbar=PlaceToolbar({-300,300,-10,680},monitor,scale,1,state.tool);
                expect(OpenToolbarDropdown(state,id,monitor),"every dropdown uses the shared popup");
                const auto bounds=state.dropdown.bounds;
                expect(bounds.left>=monitor.left&&bounds.right<=monitor.right&&bounds.top>=monitor.top&&bounds.bottom<=monitor.bottom,"popup stays within negative-origin screen at mixed DPI");
                for(int i=0;i<static_cast<int>(state.dropdown.items.size());++i){const auto row=state.dropdown.Row(i);expect(state.dropdown.Hit({(row.left+row.right)/2,(row.top+row.bottom)/2})==i,"popup row geometry matches hit testing");}
                ChooseToolbarDropdown(state,-1);expect(!state.dropdown.Open(),"popup cancellation closes the menu");
            }
            const std::filesystem::path folder=argv[1];std::filesystem::create_directories(folder);
            {
                auto canvas=MakeFrame({0,0,640,550},0xfffafafa);for(int y=0;y<550;++y)for(int x=320;x<640;++x)canvas.pixels[static_cast<size_t>(y)*640+x]=0xff141414;
                constexpr uint32_t colors[]={0xffeb2f96,0xfff5222d,0xfffa541c,0xfffa8c16,0xfffaad14,0xffa0d911,0xff52c41a,0xff13c2c2,0xff1677ff,0xff2f54eb,0xff722ed1};
                constexpr LPCWSTR names[]={L"magenta",L"red",L"volcano",L"orange",L"gold",L"lime",L"green",L"cyan",L"blue",L"geekblue",L"purple"};
                Document notes;for(int side=0;side<2;++side)for(int i=0;i<11;++i){Mark note;note.tool=Tool::Number;note.number_combo=NumberCombo::Text;note.number=i+1;note.number_size=24;note.number_text_preset=-2;note.number_text_size=16;note.color=colors[i];const float x=16.f+side*320,y=12.f+i*48;note.a={x,y};note.b={x+24,y+24};note.number_target={x+200,y+36};note.text=names[i];notes.Add(note);}
                Renderer renderer;SavePng(renderer.Flatten(canvas,notes,canvas.bounds),folder/"note-color-contrast.png");            }
            for(bool dark:{false,true}){Renderer renderer;const auto preview=renderer.Demo(true,dark,Tool::Pen,false,-1,-1,true);const auto path=folder/(dark?"highlighter-dark.png":"highlighter-light.png");SavePng(preview,path);expect(ReadPng(path).pixels==preview.pixels,"highlighter preview survives PNG export");}
            for(bool dark:{false,true}){Renderer renderer;SavePng(renderer.Demo(true,dark,Tool::Number,false,-1,70),folder/(dark?"number-text-dark.png":"number-text-light.png"));}
            state.tool=Tool::Number;state.number_combo=NumberCombo::Text;state.selected=true;state.busy=false;state.number_size=32;state.number_text_size=16;const auto badge_color=state.color;state.toolbar=PlaceToolbar({100,100,400,400},{0,0,1280,800},1,1,state.tool,state.number_combo);
            expect(!OpenToolbarDropdown(state,70,{0,0,1280,800}),"number label uses swatches instead of dropdown");state.number_text_color=*ToolbarColor(73);
            expect(state.number_text_color==*ToolbarColor(73)&&state.color==badge_color,"label color does not change badge color");
            AdjustPropertyValue(state,71,8);expect(state.number_text_size==24&&state.number_size==32,"label font size does not change badge size");
            {Frame canvas=MakeFrame({0,0,1200,540},0xffedf2f9);for(int y=0;y<540;++y)for(int x=0;x<1200;++x)if((x/12+y/12)%2==0)canvas.pixels[static_cast<size_t>(y)*1200+x]=0xffe4eaf4;Document combinations;
                for(int i=1;i<=6;++i){const float x=35.f+((i-1)%3)*395,y=35.f+((i-1)/3)*245;Mark badge;badge.tool=Tool::Number;badge.a={x,y+45};badge.b={x+42,y+87};badge.number_size=42;badge.number=i;badge.color=0xff0784ff;badge.number_shape=i==3?NumberShape::Pin:NumberShape::Circle;badge.number_combo=static_cast<NumberCombo>(i);badge.number_target={x+250,y+120};badge.text=L"这里是说明文字";combinations.Add(badge);
                    Mark label;label.tool=Tool::Text;label.a={x,y+170};label.b={x+340,y+210};label.font_size=19;label.text=NumberComboNames[i];label.color=0xff172b43;combinations.Add(label);}
                Renderer renderer;SavePng(renderer.Flatten(canvas,combinations,canvas.bounds),folder/"number-combinations.png");}
            {Frame canvas=MakeFrame({0,0,1000,280},0xffedf2f9);Document numbered;
                for(int i=0;i<10;++i){const float x=30.f+(i%5)*195,y=20.f+(i/5)*135;Mark badge;badge.tool=Tool::Number;badge.a={x+50,y};badge.b={x+(i==5?131:104),y+54};badge.number=i+1;badge.color=0xff0784ff;badge.number_shape=static_cast<NumberShape>(i);numbered.Add(badge);
                    Mark label;label.tool=Tool::Text;label.a={x+15,y+68};label.b={x+175,y+108};label.font_size=18;label.text=NumberShapeNames[i];label.color=0xff243142;label.text_align=TextAlign::Center;numbered.Add(label);}
                Renderer renderer;SavePng(renderer.Flatten(canvas,numbered,canvas.bounds),folder/"number-catalog.png");}
            {
                Frame canvas=MakeFrame({0,0,1320,1060},0xfff0f4fa);Document gallery;
                const auto label=[&](std::wstring text,Point a,Point b,float size){Mark mark;mark.tool=Tool::Text;mark.text=std::move(text);mark.a=a;mark.b=b;mark.font_size=size;mark.color=0xff243142;gallery.Add(mark);};
                for(int type=0;type<6;++type)label(ArrowTypeNames[type],{180.f+type*185,15},{360.f+type*185,50},18);
                for(int head=0;head<11;++head){const float y=70.f+head*88;label(ArrowHeadNames[head],{12,y},{178,y+42},16);
                    for(int type=0;type<6;++type){Mark arrow;arrow.tool=Tool::Arrow;arrow.a={190.f+type*185,y+32};arrow.b={330.f+type*185,y+15};arrow.width=3;arrow.arrow_size=23;arrow.color=0xff0784ff;arrow.arrow_type=static_cast<ArrowType>(type);arrow.arrow_head=static_cast<ArrowHead>(head);gallery.Add(arrow);}
                }
                Renderer renderer;SavePng(renderer.Flatten(canvas,gallery,canvas.bounds),folder/"arrow-catalog.png");
            }
            for(bool dark:{false,true})for(int id:{48,49,50,27,66}){
                Renderer renderer;const auto tool=id==27?Tool::Text:(id==48?Tool::Rectangle:Tool::Arrow);
                SavePng(renderer.Demo(true,dark,tool,false,-1,id),folder/(std::string(dark?"dropdown-dark-":"dropdown-light-")+std::to_string(id)+".png"));
            }
            for(bool dark:{false,true})for(int hover:{11,0,15,16}){
                Renderer renderer;auto frame=renderer.Demo(true,dark,Tool::Select,false,hover);
                SavePng(Crop(frame,{240,560,1120,670}),folder/(std::string(dark?"dark-tooltip-":"light-tooltip-")+std::to_string(hover)+".png"));
            }
            auto panel_gallery=MakeFrame({0,0,2016,1072},0xffeef1f5);
            for(bool dark:{false,true})for(Tool tool:{Tool::Select,Tool::Rectangle,Tool::Ellipse,Tool::Arrow,Tool::Pen,Tool::Text,Tool::Mosaic,Tool::Number}) {
                Renderer renderer;auto frame=renderer.Demo(true,dark,tool);
                const auto path=folder/(std::string(dark?"dark-":"light-")+std::to_string(static_cast<int>(tool))+".png");
                SavePng(frame,path);expect(ReadPng(path).pixels==frame.pixels,"context panel preview renders and roundtrips");
                const auto panel_bounds=PlaceToolbar({180,90,1110,600},frame.bounds,1,tool==Tool::Select?0.f:1.f,tool).bounds;
                const auto panel=Crop(frame,{LONG(panel_bounds.left)-2,LONG(panel_bounds.top)-2,LONG(panel_bounds.right)+2,LONG(panel_bounds.bottom)+2});
                const int gx=dark?1012:4,gy=static_cast<int>(tool)*134+4;
                for(int y=0;y<panel.Height();++y)for(int x=0;x<panel.Width();++x)panel_gallery.pixels[static_cast<size_t>(gy+y)*2016+gx+x]=panel.pixels[static_cast<size_t>(y)*panel.Width()+x];
                if(tool==Tool::Rectangle||tool==Tool::Select){const auto b=PlaceToolbar({180,90,1110,600},frame.bounds,1,tool==Tool::Select?0.f:1.f,tool).bounds;SavePng(Crop(frame,{LONG(b.left)-2,LONG(b.top)-2,LONG(b.right)+2,LONG(b.bottom)+2}),folder/(tool==Tool::Select?(dark?"dark-compact.png":"light-compact.png"):(dark?"dark-panel.png":"light-panel.png")));}
            }
            SavePng(panel_gallery,folder/"all-toolbars.png");
            {ToolbarCustomization preview_layout;preview_layout.overflow={14,17,16};preview_layout.Normalize();
             for(bool dark:{false,true}){
                Renderer label_renderer;ToolbarCustomization label_layout;label_layout.labels=ToolbarLabels::Text;ToolbarCustomization icon_layout;icon_layout.labels=ToolbarLabels::Icons;
                ToolbarCustomization manual_layout;manual_layout.auto_properties=false;manual_layout.overflow={17,16};manual_layout.Normalize();
                const std::string suffix=dark?"-dark.png":"-light.png";
                SavePng(label_renderer.Demo(true,dark,Tool::Select,false,-1,-1,false,&label_layout),folder/("labels-text-select"+suffix));
                Renderer label_active;SavePng(label_active.Demo(true,dark,Tool::Arrow,false,-1,-1,false,&label_layout),folder/("labels-text-arrow"+suffix));
                Renderer icon_active;SavePng(icon_active.Demo(true,dark,Tool::Arrow,false,-1,-1,false,&icon_layout),folder/("labels-icons-arrow"+suffix));
                Renderer manual_renderer;SavePng(manual_renderer.Demo(true,dark,Tool::Arrow,false,-1,-1,false,&manual_layout),folder/("properties-collapsed"+suffix));
                Renderer more_renderer;const auto more_frame=more_renderer.Demo(true,dark,Tool::Number,false,-1,ToolbarMoreId,false,&preview_layout);
                const auto preview_path=folder/(dark?"customized-more-dark.png":"customized-more-light.png");SavePng(more_frame,preview_path);
                Renderer plain_renderer;expect(ReadPng(preview_path).pixels==more_frame.pixels&&plain_renderer.Demo(true,dark,Tool::Number,false,-1,ToolbarMoreId).pixels!=more_frame.pixels,"customized toolbar preview renders and differs from default");
                Renderer context_renderer;SavePng(context_renderer.Demo(true,dark,Tool::Arrow,false,6,ToolbarContextMenuId,false,&preview_layout),folder/(dark?"customized-context-dark.png":"customized-context-light.png"));
                ToolbarCustomization pin_more;pin_more.overflow={13};pin_more.Normalize();
                Renderer pin_menu;SavePng(pin_menu.Demo(true,dark,Tool::Select,false,-1,ToolbarMoreId,false,&pin_more),folder/("pin-more"+suffix));
                Renderer pin_context;SavePng(pin_context.Demo(true,dark,Tool::Select,false,13,ToolbarContextMenuId,false,nullptr),folder/("pin-context"+suffix));
             }}
        }
        // Toolbar customization: order, overflow ("更多") menu and unchanged default geometry.
        {
            ToolbarCustomization parsed;parsed.order=ParseToolbarIds(L"16, 3,3,99,abc,1");parsed.overflow=ParseToolbarIds(L"16,0,7");parsed.Normalize();
            expect(parsed.order.size()==CustomizableToolbarIds.size()&&parsed.order[0]==13&&parsed.order[1]==16&&parsed.order[2]==3&&parsed.order[3]==1&&parsed.overflow==std::vector<int>{16},"customization normalizes ids");
            expect(ParseToolbarIds(FormatToolbarIds(parsed.order))==parsed.order,"customization ids roundtrip");
            for(float scale:{1.f,1.5f,2.f})for(int width:{480,800,1280})for(Tool tool:{Tool::Select,Tool::Rectangle}){
                const RECT monitor{-width,0,0,1080};const Box sel{-300,200,-40,600};
                const auto base=PlaceToolbar(sel,monitor,scale,1,tool,NumberCombo::Plain);
                expect(base.Button(ToolbarMoreId).right<=base.Button(ToolbarMoreId).left&&base.PanelToolSlots().size()==7,"default customization: seven tools, no more button");
                ToolbarCustomization custom;custom.order={3,1,2,4,5,6,14,18,15,17,16};custom.overflow={2,6,17};custom.Normalize();
                auto t=PlaceToolbar(sel,monitor,scale,1,tool,NumberCombo::Plain,custom);
                expect(t.PanelToolSlots()==std::vector<int>{3,1,4,5,14,ToolbarMoreId},"panel follows custom order and ends with more");
                expect(t.Button(2).right<=t.Button(2).left&&t.Button(6).right<=t.Button(6).left&&t.Button(17).right<=t.Button(17).left,"hidden entries have no panel button");
                expect(t.Button(18).right<t.Button(15).left,"quick actions follow custom order");
                expect(custom.order==std::vector<int>{3,1,2,4,5,6,14,13,18,15,17,16}&&t.Button(13).right<t.Button(18).left,"a saved order without the pin keeps it before the quick actions");
                expect(t.OverflowItems()==std::vector<int>{2,6,17},"overflow lists hidden entries in order");
                const auto controls=t.Controls(tool);bool valid=true;
                for(const auto& c:controls){const auto p=c.bounds;valid=valid&&InToolbar(t,p);if(c.Interactive())valid=valid&&t.Hit({(p.left+p.right)/2,(p.top+p.bottom)/2},tool)==c.id;}
                for(size_t a=0;a<controls.size();++a)for(size_t b=a+1;b<controls.size();++b)if(controls[a].Interactive()&&controls[b].Interactive()){const auto x=controls[a].bounds,y=controls[b].bounds;valid=valid&&!(std::min(x.right,y.right)>std::max(x.left,y.left)&&std::min(x.bottom,y.bottom)>std::max(x.top,y.top));}
                expect(valid,"custom toolbar controls fit, hit and do not overlap");
                t.recording_available=false;expect(t.OverflowItems()==std::vector<int>{2,6},"unavailable hidden actions leave the menu");
                ViewState menu;menu.selected=true;menu.selection=sel;menu.toolbar=PlaceToolbar(sel,monitor,scale,1,tool,NumberCombo::Plain,custom);
                expect(OpenToolbarMoreMenu(menu,monitor)&&menu.dropdown.property==ToolbarMoreId&&menu.dropdown.items.size()==3&&menu.dropdown.items[0].second==2,"more menu lists hidden entries");
                menu.dropdown.Close();menu.tool=Tool::Mosaic;
                const auto shown=ToolbarControls(menu,Document{});const auto more=std::find_if(shown.begin(),shown.end(),[](const auto& c){return c.id==ToolbarMoreId;});
                expect(more!=shown.end()&&more->selected&&more->icon==6,"more button shows the hidden active tool");
                ToolbarCustomization none;for(int id:CustomizableToolbarIds)none.SetOnPanel(id,false);
                const auto bare=PlaceToolbar(sel,monitor,scale,1,tool,NumberCombo::Plain,none);
                expect(bare.PanelToolSlots()==std::vector<int>{ToolbarMoreId}&&bare.Button(0).right>bare.Button(0).left,"everything hidden keeps select and more");
                ViewState ctx;ctx.selected=true;ctx.selection=sel;ctx.toolbar=PlaceToolbar(sel,monitor,scale,1,tool,NumberCombo::Plain,custom);
                expect(OpenToolbarContextMenu(ctx,3,monitor)&&ctx.dropdown.property==ToolbarContextMenuId&&ctx.dropdown.items.size()==2&&ctx.dropdown.items[0].second==3&&ctx.dropdown.items[1].second==ToolbarRestoreAllItem,"right-click menu tucks a panel entry away");
                ctx.dropdown.Close();
                expect(!OpenToolbarContextMenu(ctx,2,monitor)&&!OpenToolbarContextMenu(ctx,7,monitor)&&!OpenToolbarContextMenu(ctx,0,monitor)&&!OpenToolbarContextMenu(ctx,ToolbarMoreId,monitor)&&!ctx.dropdown.Open(),"right-click menu skips hidden, fixed and more entries");
                ToolbarCustomization pin_layout;pin_layout.SetOnPanel(ToolbarPinId,false);
                const auto pinned=PlaceToolbar(sel,monitor,scale,1,tool,NumberCombo::Plain,pin_layout);
                expect(pinned.Button(13).right<=pinned.Button(13).left&&pinned.OverflowItems()==std::vector<int>{13}&&pinned.Button(ToolbarMoreId).right>pinned.Button(ToolbarMoreId).left&&pinned.Button(10).right<pinned.Button(15).left&&pinned.actions.right-pinned.actions.left<=base.actions.right-base.actions.left,"pin moves into the more menu and frees its action slot");
                {const auto pin_controls=pinned.Controls(tool);bool pin_valid=true;
                 for(const auto& pc:pin_controls){const auto pb=pc.bounds;pin_valid=pin_valid&&InToolbar(pinned,pb);if(pc.Interactive())pin_valid=pin_valid&&pinned.Hit({(pb.left+pb.right)/2,(pb.top+pb.bottom)/2},tool)==pc.id;}
                 for(size_t pa=0;pa<pin_controls.size();++pa)for(size_t pz=pa+1;pz<pin_controls.size();++pz)if(pin_controls[pa].Interactive()&&pin_controls[pz].Interactive()){const auto pin_first=pin_controls[pa].bounds,pin_second=pin_controls[pz].bounds;pin_valid=pin_valid&&!(std::min(pin_first.right,pin_second.right)>std::max(pin_first.left,pin_second.left)&&std::min(pin_first.bottom,pin_second.bottom)>std::max(pin_first.top,pin_second.top));}
                 expect(pin_valid,"toolbar without the pin fits, hits and does not overlap");}
                ViewState pin_ctx;pin_ctx.selected=true;pin_ctx.selection=sel;pin_ctx.toolbar=base;
                expect(OpenToolbarContextMenu(pin_ctx,13,monitor)&&pin_ctx.dropdown.items.size()==1&&pin_ctx.dropdown.items[0].second==13,"right-click menu tucks the pin away");
                ctx.toolbar=base;expect(OpenToolbarContextMenu(ctx,1,monitor)&&ctx.dropdown.items.size()==1,"restore-all appears only when something is hidden");
                expect(OpenToolbarMoreMenu(menu,monitor),"more menu reopens");bool pins=true;
                for(int i=0;i<static_cast<int>(menu.dropdown.items.size());++i){const auto row=menu.dropdown.Row(i),pin=ToolbarMorePinBox(menu.dropdown,i);pins=pins&&pin.left>(row.left+row.right)/2&&pin.right<=row.right&&pin.top>=row.top&&pin.bottom<=row.bottom&&menu.dropdown.Hit({(pin.left+pin.right)/2,(pin.top+pin.bottom)/2})==i;}
                expect(pins,"more menu pin zones sit at the row end and hit their row");
                const auto layout_ok=[&](const ToolbarLayout& l,Tool active){
                    const auto cs=l.Controls(active);bool ok=true;
                    for(const auto& c:cs){const auto p=c.bounds;ok=ok&&InToolbar(l,p);if(c.Interactive())ok=ok&&l.Hit({(p.left+p.right)/2,(p.top+p.bottom)/2},active)==c.id;}
                    for(size_t a=0;a<cs.size();++a)for(size_t b=a+1;b<cs.size();++b)if(cs[a].Interactive()&&cs[b].Interactive()){const auto x=cs[a].bounds,y=cs[b].bounds;ok=ok&&!(std::min(x.right,y.right)>std::max(x.left,y.left)&&std::min(x.bottom,y.bottom)>std::max(x.top,y.top));}
                    return ok;};
                {ToolbarCustomization labelled;labelled.labels=ToolbarLabels::Text;
                 const auto lt=PlaceToolbar(sel,monitor,scale,1,tool,NumberCombo::Plain,labelled);bool roomy=true;int count=0;
                 for(const auto& c:lt.Controls(tool))if(c.kind==ui::Kind::Button&&IsToolbarToolId(c.id)){if(!c.text.empty())++count;roomy=roomy&&(c.text.empty()||c.bounds.right-c.bounds.left>=(41+13.f*float(c.text.size()))*lt.scale);}
                 expect(roomy&&(float(width)/lt.scale<960.f||count==7)&&layout_ok(lt,tool),"icon+text labels every tool when there is room and never overflows a button");
                 ToolbarCustomization auto_labels;expect(PlaceToolbar(sel,monitor,scale,1,tool,NumberCombo::Plain,auto_labels).bounds.right==PlaceToolbar(sel,monitor,scale,1,tool).bounds.right,"auto labels keep the default geometry");
                 ToolbarCustomization icons;icons.labels=ToolbarLabels::Icons;const auto narrow=PlaceToolbar(sel,monitor,scale,1,Tool::Rectangle,NumberCombo::Plain,icons);
                 expect(narrow.Button(1).right-narrow.Button(1).left<=37*narrow.scale&&layout_ok(narrow,Tool::Rectangle),"icon-only grid keeps narrow buttons with a tool active");}
                {ToolbarCustomization manual;manual.auto_properties=false;
                 const auto closed=PlaceToolbar(sel,monitor,scale,0,Tool::Rectangle,NumberCombo::Plain,manual),opened=PlaceToolbar(sel,monitor,scale,1,Tool::Rectangle,NumberCombo::Plain,manual);
                 const auto slots=PlaceToolbar(sel,monitor,scale,1,Tool::Rectangle).PanelToolSlots();
                 expect(std::find(slots.begin(),slots.end(),ToolbarPropertiesId)==slots.end(),"auto-expanding toolbar has no properties button");
                 expect(closed.PanelToolSlots().back()==ToolbarPropertiesId&&!closed.CardVisible()&&opened.CardVisible()&&std::abs((closed.bounds.bottom-closed.bounds.top)-(opened.bounds.bottom-opened.bounds.top))<.01f,"manual mode adds a properties button and starts collapsed");
                 const auto find_props=[](const ToolbarLayout& l){for(const auto& c:l.Controls(Tool::Rectangle))if(c.id==ToolbarPropertiesId)return c.selected?1:0;return -1;};
                 expect(find_props(closed)==0&&find_props(opened)==1&&layout_ok(opened,Tool::Rectangle),"properties button reflects the open state");
                 for(Tool active:{Tool::Rectangle,Tool::Ellipse,Tool::Arrow,Tool::Pen,Tool::Text,Tool::Mosaic,Tool::Number}){
                     const auto l=PlaceToolbar(sel,monitor,scale,1,active,NumberCombo::Text,manual);const auto cs=l.Controls(active);
                     expect(std::count_if(cs.begin(),cs.end(),[](const auto& c){return c.id==ToolbarPropertiesId;})==1,"properties button id never collides with a property control (e.g. the green swatch)");}
                 const auto idle=PlaceToolbar(sel,monitor,scale,0,Tool::Select,NumberCombo::Plain,manual);expect(idle.Button(ToolbarPropertiesId).right<=idle.Button(ToolbarPropertiesId).left,"no properties button without an active tool");}
            }
        }
    }catch(const std::exception& e){std::cout<<"[FAIL] "<<e.what()<<'\n';++failures;}
    CoUninitialize();return failures?1:0;
}