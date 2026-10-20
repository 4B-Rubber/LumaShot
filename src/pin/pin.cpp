#include "pin/pin.h"
#include "export/clipboard.h"
#include "export/png.h"
#include "export/drag_out.h"
#include "pin/selection_tools.h"
#include "pin/paper.h"
#include "pin/zoom.h"
#include "ui/pin_menu.h"
#include "ui/themed_message.h"
#include "ui/memory_target.h"
#include "ocr/availability.h"
#include "pin/translation.h"
#include "pin/translation_panel.h"
#include "translate/service.h"
#include <windowsx.h>
#include <commdlg.h>
#include <algorithm>
#include <cmath>
namespace lumashot {
// Posted after a card is pulled off the rope: the OLE drag starts once the rope's mouse handler has returned.
constexpr UINT kDragOutMessage=WM_APP+0x48;
// Hover bar buttons; stable ids independent of the layout order (annotate, OCR, copy, hang, more | close).
enum PinTool:int{PinToolHang=0,PinToolClose=1,PinToolAnnotate=2,PinToolOcr=3,PinToolCopy=4,PinToolMore=5,PinToolCount=6};
struct PinManager::Pin {
    PinManager* manager{};HWND window{};uint64_t id{},version{},revision{1};
    std::shared_ptr<const Frame> image,ocr_image,annotation_base;Document annotations;
    ocr::Text text;ocr::Selection selection;
    std::vector<SelectionMark> marks;
    std::vector<std::vector<SelectionMark>> undo;
    HWND tools{};ComPtr<ID2D1HwndRenderTarget> tools_target;SelectionBar bar;
    bool copied{};int tools_hover{-1},tools_down{-1},handle{-1};Point handle_offset{};
    std::wstring status;bool ocr_enabled{};bool selecting{},moving{},saving{},recognizing{},blank_click{},dragged{};
    POINT drag{},origin{},last_click{};DWORD click_time{};int clicks{};
    bool locked{},editing{};
    float dpi{96};
    float zoom{1},zoom_from{1},zoom_goal{1};POINT zoom_anchor{};Point zoom_image{};
    ULONGLONG zoom_started{},zoom_frame_time{},zoom_badge_until{};bool zoom_animating{};
    std::unique_ptr<DibSurface> surface;int surface_width{},surface_height{};
    PinStyle style{DefaultPinStyle};
    struct Translation {
        TranslationView::State state{TranslationView::State::Idle};uint64_t job{};
        translate::Language requested{translate::Language::Auto},source{translate::Language::Auto},target{translate::Language::ChineseSimplified};
        std::vector<translate::Block> blocks;std::vector<std::wstring> results;std::wstring error,engine;
        std::shared_ptr<const Frame> image;bool show{};std::unique_ptr<TranslationPanel> panel;
    } tr;
    // The frame the pin currently shows, copies and saves: the translated
    // rendering while it is toggled on, otherwise the original image.
    std::shared_ptr<const Frame> Shown()const{return tr.show&&tr.image?tr.image:image;}
    PaperLayout paper,shadow_layout;Frame shadow_image;ComPtr<ID2D1Bitmap> shadow_bitmap;unsigned shadow_builds{};
    Point offset{};POINT destination{};bool relocating{},presenting{};
    // Clothesline: the window is hidden while hung; hang_origin is where it flies back to.
    bool hung{},from_line{};POINT hang_origin{};BYTE alpha{255};
    // A system drag into another program is running (DoDragDrop loop).
    bool out_drag{};
    // Hover bar (full.html .pin .hbar): a small layered window owned by the pin
    // that floats 8 DIP above the image's right edge (below it when the work
    // area has no room above): annotate · OCR · copy · hang · more | close.
    // Tool ids are stable (0 hangs, 1 closes, as before). hang_tool is false
    // while the clothesline is switched off in settings, annotate_tool without
    // an annotation handler and ocr_tool without an OCR engine.
    struct ToolBar{D2D1_RECT_F bar{};D2D1_RECT_F rect[PinToolCount]{};int order[PinToolCount]{};int count{};float separator{-1},width{},height{};};
    bool hang_tool{true},annotate_tool{true},ocr_tool{true};
    bool hovered{},tracking{},closing{};int tool_hover{-1},tool_down{-1};float tool_alpha{};ULONGLONG tool_tick{};
    HWND hbar{};std::unique_ptr<DibSurface> bar_surface;ComPtr<ID2D1RenderTarget> bar_target;int bar_width{},bar_height{};TextRenderer bar_text;
    // bar_hovered: the cursor is on the bar window; leave_until bridges the gap
    // between pin and bar; tip_ready: the tooltip's 380 ms delay has passed.
    bool bar_hovered{},bar_tracking{},bar_below{},tip_ready{};ULONGLONG leave_until{};
    // Copy / OCR-copy feedback: the tooltip reads "已复制…" until copied_until.
    int copied_tool{-1};ULONGLONG copied_until{};
    ComPtr<ID2D1StrokeStyle> round_stroke;
    bool ToolsWanted()const{return (hovered||bar_hovered||tool_down>=0||GetTickCount64()<leave_until)&&!editing&&!locked&&!hung&&!closing&&!out_drag&&!flight.active&&!(moving&&dragged);}
    // Bar window client coordinates: an 8 DIP shadow margin around a 30 DIP
    // tooltip row (on the side away from the pin) and the bar itself: 3 DIP
    // padding, 30 DIP buttons 1 DIP apart, a separator with 5 DIP either side
    // before close; right-aligned in a window at least 200 DIP wide.
    bool ToolLayout(ToolBar& out)const{
        if(!image)return false;
        const float s=dpi/96,size=30*s,gap=1*s,pad=3*s,sep=11*s,m=8*s,tip=30*s;
        static constexpr int all[]={PinToolAnnotate,PinToolOcr,PinToolCopy,PinToolHang,PinToolMore};
        int ids[PinToolCount]{};int n=0;
        for(const int tool_id:all){
            if((tool_id==PinToolHang&&!hang_tool)||(tool_id==PinToolAnnotate&&!annotate_tool)||(tool_id==PinToolOcr&&!ocr_tool))continue;
            ids[n++]=tool_id;
        }
        const float w=2*pad+float(n+1)*size+float(n>1?n-1:0)*gap+sep;
        out=ToolBar{};out.width=std::ceil(std::max(w,200*s)+2*m);out.height=std::ceil(size+2*pad+tip+2*m);
        const float right=out.width-m,y=bar_below?m:m+tip;
        float x=right-w;out.bar=D2D1::RectF(x,y,right,y+size+2*pad);x+=pad;
        for(int i=0;i<n;++i){out.rect[ids[i]]=D2D1::RectF(x,y+pad,x+size,y+pad+size);out.order[out.count++]=ids[i];x+=size+(i+1<n?gap:0);}
        out.separator=x+sep/2;x+=sep;
        out.rect[PinToolClose]=D2D1::RectF(x,y+pad,x+size,y+pad+size);out.order[out.count++]=PinToolClose;
        return true;
    }
    // The hang (0) and close (1) slots; an empty rect when that button is absent.
    bool ToolRects(D2D1_RECT_F (&out)[2])const{ToolBar b;if(!ToolLayout(b))return false;out[0]=b.rect[PinToolHang];out[1]=b.rect[PinToolClose];return true;}
    // Hit test in bar window client coordinates.
    int ToolAt(LPARAM lp,float min_alpha=.5f)const{
        ToolBar b;if(tool_alpha<min_alpha||!ToolLayout(b))return -1;
        const float x=float(GET_X_LPARAM(lp)),y=float(GET_Y_LPARAM(lp));
        for(int i=0;i<b.count;++i){const auto& r=b.rect[b.order[i]];if(x>=r.left&&x<r.right&&y>=r.top&&y<r.bottom)return b.order[i];}
        return -1;
    }
    struct Flight{bool active{};ULONGLONG start{};float from_zoom{1},to_zoom{1};Point from{},to{};} flight;
    int Inset()const{return paper.inset+paper.shadow;}
    // Rounded up to even sizes: Direct2D's software target over the window's DIB
    // copies the whole surface on every draw when either side is odd (measured:
    // a clipped clear of 3389x1927 takes 15-30 ms, of 3388x1928 0.1 ms).
    int CanvasWidth()const{return (paper.width+2*paper.shadow+3)&~1;}
    int CanvasHeight()const{return (paper.height+2*paper.shadow+3)&~1;}
    Point WindowPoint(Point q)const{return {q.x*zoom+Inset()+offset.x,q.y*zoom+Inset()+offset.y};}
    ComPtr<ID2D1Factory> factory;ComPtr<ID2D1RenderTarget> target;ComPtr<ID2D1Bitmap> bitmap;int bitmap_uploads{};TextRenderer text_renderer;
    // While the window is hidden on the clothesline its canvas, renderer and the
    // software copy of the image are dropped (about two frames of memory for a
    // full-screen pin); the next Paint rebuilds them from `image`.
    void ReleaseSurface(){target.Reset();bitmap.Reset();surface.reset();surface_width=surface_height=0;}
    Point ImagePoint(LPARAM lp) const{return {(GET_X_LPARAM(lp)-Inset()-offset.x)/zoom,(GET_Y_LPARAM(lp)-Inset()-offset.y)/zoom};}
    SelectionHandles Handles()const{
        auto handles=TextHandles(text,selection,dpi/96/zoom);if(!handles.visible)return handles;
        const float margin=std::min({6*dpi/96,float(image->Width())/2,float(image->Height())/2});
        for(auto* p:{&handles.start,&handles.end}){p->x=std::clamp(p->x,margin,float(image->Width())-margin);p->y=std::clamp(p->y,margin,float(image->Height())-margin);}return handles;
    }
    bool Blank(Point point) const {
        if(point.x<0||point.y<0||point.x>=image->Width()||point.y>=image->Height())return true;
        if(ocr::OnText(text,point))return false;
        if(!recognizing)return true;
        // Until OCR finishes, only close on a locally uniform patch. Text must
        // not become a close target just because its recognition is pending.
        const int x=std::clamp(int(point.x),0,image->Width()-1),y=std::clamp(int(point.y),0,image->Height()-1);
        const auto reference=image->pixels[static_cast<size_t>(y)*image->Width()+x];
        for(int yy=std::max(0,y-12);yy<std::min(image->Height(),y+13);++yy)
            for(int xx=std::max(0,x-12);xx<std::min(image->Width(),x+13);++xx) {
                const auto pixel=image->pixels[static_cast<size_t>(yy)*image->Width()+xx];
                for(int shift:{0,8,16})if(std::abs(int((pixel>>shift)&255)-int((reference>>shift)&255))>12)return false;
            }
        return true;
    }
};
PinManager::PinManager(HWND host):host_(host),service_(host,kOcrReady) {
    WNDCLASSW wc{};wc.lpfnWndProc=Proc;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"LumaShot.Pin";
    wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.style=CS_DBLCLKS;
    if(!RegisterClassW(&wc)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)CheckWin32(false,"Register pin window");
    wc.lpfnWndProc=ToolsProc;wc.lpszClassName=L"LumaShot.SelectionTools";wc.style=CS_DROPSHADOW;
    if(!RegisterClassW(&wc)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)CheckWin32(false,"Register selection tools");
    wc.lpfnWndProc=HoverBarProc;wc.lpszClassName=L"LumaShot.PinBar";wc.style=0;
    if(!RegisterClassW(&wc)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)CheckWin32(false,"Register pin hover bar");
    // The rope window itself is created when the first pin opens (Arm) or on the first hang.
    line_=std::make_unique<Clothesline>(Clothesline::Callbacks{
        [this](uint64_t id,RECT from){RetrievePin(id,from);},
        [this](uint64_t id,POINT cursor){DetachPin(id,cursor);},
        [this]{for(const auto& [id,pin]:pins_)if(pin->window&&!pin->hung&&IsWindowVisible(pin->window))return true;return false;}});
}
PinManager::~PinManager(){try{PreserveSession();}catch(...){}session_writer_.reset();for(auto& [id,pin]:pins_){service_.Cancel(id);if(pin->window)DestroyWindow(pin->window);}pins_.clear();}
void PinManager::Create(Frame image,Frame ocr_image,POINT position,std::optional<Frame> base,Document annotations,bool recognize,const PinSessionRecord* restored) {
    auto pin=std::make_unique<Pin>();pin->manager=this;pin->id=next_id_++;
    if(!restored){image.bounds={0,0,image.Width(),image.Height()};ocr_image.bounds=image.bounds;}
    // Identical (or empty) OCR pixels share the image instead of a second copy;
    // for a long capture that is one full image less per pin.
    const bool same_ocr=!restored&&(ocr_image.pixels.empty()||ocr_image.pixels==image.pixels);
    pin->image=restored?restored->image:std::make_shared<Frame>(std::move(image));
    pin->ocr_image=restored?restored->ocr_image:same_ocr?pin->image:std::make_shared<Frame>(std::move(ocr_image));
    if(base)base->bounds={0,0,base->Width(),base->Height()};
    pin->annotation_base=base?std::make_shared<Frame>(std::move(*base)):pin->image;pin->annotations=std::move(annotations);
    if(restored){pin->annotation_base=restored->base;pin->annotations.marks=restored->annotations;pin->marks=restored->decorations;pin->locked=restored->locked;}
    pin->style=restored?restored->style:sticker_style?NormalizePinStyle(static_cast<int>(sticker_style())):DefaultPinStyle;
    pin->paper=MakePaperLayout(pin->image->Width(),pin->image->Height(),96,pin->style);
    pin->hang_tool=LineOn();pin->annotate_tool=bool(annotate);pin->ocr_tool=ocr::Available();
    const uint64_t id=pin->id;auto* p=pin.get();pins_.emplace(id,std::move(pin));last_created_=id;
    p->window=CreateWindowExW(WS_EX_LAYERED|WS_EX_TOPMOST|WS_EX_TOOLWINDOW,L"LumaShot.Pin",L"LumaShot 贴图",WS_POPUP,
        position.x-p->Inset(),position.y-p->Inset(),p->CanvasWidth(),p->CanvasHeight(),nullptr,nullptr,GetModuleHandleW(nullptr),p);
    if(!p->window){pins_.erase(id);CheckWin32(false,"Create pinned image");}
    p->dpi=float(GetDpiForWindow(p->window));
    if(restored)p->zoom=p->zoom_from=p->zoom_goal=PinZoomGoal(restored->zoom,0,p->image->Width(),p->image->Height(),p->dpi,p->style);
    else{
        // Tall images (long captures) start fitted to 90% of the work area instead of spilling off screen.
        MONITORINFO monitor{sizeof(monitor)};GetMonitorInfoW(MonitorFromPoint(position,MONITOR_DEFAULTTONEAREST),&monitor);
        const int work=monitor.rcWork.bottom-monitor.rcWork.top;
        if(work>0&&p->image->Height()>work)p->zoom=p->zoom_from=p->zoom_goal=PinZoomGoal(std::min(1.f,float(work)*.9f/float(p->image->Height())),0,p->image->Width(),p->image->Height(),p->dpi,p->style);
    }
    const auto paper=MakePaperLayout(std::max(1,int(std::lround(p->image->Width()*p->zoom))),std::max(1,int(std::lround(p->image->Height()*p->zoom))),p->dpi,p->style);
    ApplyPaper(*p,{float(position.x-paper.inset-paper.shadow),float(position.y-paper.inset-paper.shadow)});
    if(restored){
        MONITORINFO monitor{sizeof(monitor)};GetMonitorInfoW(MonitorFromPoint(position,MONITOR_DEFAULTTONEAREST),&monitor);
        const float left=std::clamp(restored->origin.x-p->Inset(),float(monitor.rcWork.left),float(std::max(monitor.rcWork.left,monitor.rcWork.right-p->CanvasWidth())));
        const float top=std::clamp(restored->origin.y-p->Inset(),float(monitor.rcWork.top),float(std::max(monitor.rcWork.top,monitor.rcWork.bottom-p->CanvasHeight())));
        ApplyPaper(*p,{left,top});
    }
    if(restored&&restored->hung&&LineOn()){
        // It was on the clothesline when the session was saved (reboot,
        // reinstall, restart): back on the rope, hidden, home kept for retrieval.
        RECT r{};GetWindowRect(p->window,&r);
        p->hang_origin={r.left,r.top};p->hung=true;p->alpha=255;p->ReleaseSurface();
        line_->Restore(p->id,p->Shown());
    }else ShowWindow(p->window,SW_SHOWNOACTIVATE);
    if(LineOn())line_->Arm();
    if(!restoring_session_)SetForegroundWindow(p->window);if(recognize)Recognize(*p);SessionChanged();
}
std::vector<PinSessionRecord> PinManager::SessionSnapshot()const{
    std::vector<PinSessionRecord> result;
    for(const auto& [id,p]:pins_){if(!p->window||!p->image)continue;RECT rect{};GetWindowRect(p->window,&rect);
        PinSessionRecord record;record.id=id;record.revision=p->revision;record.origin={rect.left+p->Inset()+p->offset.x,rect.top+p->Inset()+p->offset.y};record.zoom=p->zoom;record.style=p->style;record.locked=p->locked;record.recognize=p->ocr_enabled;record.hung=p->hung;
        record.image=p->image;record.ocr_image=p->ocr_image;record.base=p->annotation_base;record.annotations=p->annotations.marks;record.decorations=p->marks;result.push_back(std::move(record));
    }return result;
}
void PinManager::SessionChanged(){if(session_writer_&&!restoring_session_&&!preserving_session_)session_writer_->Request(SessionSnapshot());}
bool PinManager::FlushSession(){if(preserving_session_)return preserve_ok_;if(!session_writer_||restoring_session_)return true;session_writer_->Request(SessionSnapshot());return session_writer_->Flush();}
void PinManager::PreserveSession(){if(!preserving_session_){preserve_ok_=FlushSession();preserving_session_=true;}}
bool PinManager::EnableSession(const std::filesystem::path& directory){
    if(directory.empty()||session_store_)return false;
    try{
        session_store_=std::make_unique<PinSessionStore>(directory);
        // PNG decoding is performed on a worker, before any restored windows are created.
        auto load=std::async(std::launch::async,[this]{const HRESULT com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);if(FAILED(com))throw std::runtime_error("Session COM initialization");struct End{~End(){CoUninitialize();}}end;unsigned skipped{};auto records=session_store_->Load(skipped);return std::pair{std::move(records),skipped};});
        auto [records,skipped]=load.get();
        session_writer_=std::make_unique<DeferredWriter<std::vector<PinSessionRecord>>>([this](const auto& state){try{session_store_->Save(state);session_warning_=false;}catch(...){if(!session_warning_.exchange(true))PostMessageW(host_,kPinSessionError,0,0);throw;}},[]{CoInitializeEx(nullptr,COINIT_MULTITHREADED);},[]{CoUninitialize();});
        restoring_session_=true;
        for(const auto& record:records){try{Create({}, {},{LONG(std::lround(record.origin.x)),LONG(std::lround(record.origin.y))},std::nullopt,{},record.recognize,&record);}catch(...){++skipped;}}
        restoring_session_=false;return skipped==0;
    }catch(...){restoring_session_=false;session_writer_.reset();session_store_.reset();return false;}
}
void PinManager::Recognize(Pin& p){if(!ocr::Available())return;p.ocr_enabled=true;++p.version;p.text={};p.selection={};UpdateTools(p);p.recognizing=true;p.status=L"正在识别文字…";service_.Submit(p.id,p.version,p.ocr_image);InvalidateRect(p.window,nullptr,FALSE);}
void PinManager::Ready() {
    for(auto& result:service_.Take()) {
        const auto it=pins_.find(result.id);if(it==pins_.end()||it->second->version!=result.version||!it->second->window)continue;
        auto& p=*it->second;p.text=std::move(result.text);p.recognizing=false;
        p.status=!result.error.empty()?L"识别失败 · 右键可重试":(p.text.lines.empty()?L"未识别到文字":L"");
        if(p.tr.state==TranslationView::State::WaitingOcr){
            if(!result.error.empty()){p.tr.state=TranslationView::State::Failed;p.tr.error=L"文字识别失败："+result.error;UpdatePanel(p);}
            else StartTranslation(p);
        }
        InvalidateRect(p.window,nullptr,FALSE);
    }
    std::erase_if(pins_,[](const auto& item){return !item.second->window;});
}
void PinManager::ApplyPaper(Pin& p,Point origin){
    const auto layout=MakePaperLayout(std::max(1,int(std::lround(p.image->Width()*p.zoom))),std::max(1,int(std::lround(p.image->Height()*p.zoom))),p.dpi,p.style);
    const auto shadow_layout=PaperShadowPatchLayout(layout);
    if(!(shadow_layout==p.shadow_layout)||(layout.shadow&&p.shadow_image.pixels.empty())){
        p.shadow_layout=shadow_layout;p.shadow_image=PaperShadowImage(shadow_layout,true);p.shadow_bitmap.Reset();++p.shadow_builds;
    }
    p.paper=layout;p.destination={LONG(std::floor(origin.x)),LONG(std::floor(origin.y))};
    p.offset={origin.x-p.destination.x,origin.y-p.destination.y};p.relocating=true;
    Paint(p);UpdateTools(p);PlacePanel(p);
}
void PinManager::RefreshAppearance(){
    const auto style=sticker_style?NormalizePinStyle(static_cast<int>(sticker_style())):DefaultPinStyle;
    for(auto& [id,pin]:pins_){
        auto& p=*pin;if(!p.window||p.style==style)continue;
        RECT bounds{};GetWindowRect(p.window,&bounds);
        const Point image_origin{bounds.left+p.Inset()+p.offset.x,bounds.top+p.Inset()+p.offset.y};
        KillTimer(p.window,101);p.zoom_animating=false;p.zoom_goal=p.zoom;p.style=style;
        const auto layout=MakePaperLayout(std::max(1,int(std::lround(p.image->Width()*p.zoom))),std::max(1,int(std::lround(p.image->Height()*p.zoom))),p.dpi,p.style);
        ApplyPaper(p,{image_origin.x-layout.inset-layout.shadow,image_origin.y-layout.inset-layout.shadow});
    }SessionChanged();
}
namespace {
// Hover bar icons: full.html's 24-unit SVG icons (stroke 1.8) drawn at 16 DIP.
void DrawPinToolIcon(ID2D1RenderTarget* target,ID2D1Factory* factory,ID2D1Brush* brush,ID2D1StrokeStyle* round,int tool,float cx,float cy,float s){
    const float k=16*s/24,stroke=1.8f;D2D1_MATRIX_3X2_F previous{};target->GetTransform(&previous);
    target->SetTransform(D2D1::Matrix3x2F::Scale(k,k)*D2D1::Matrix3x2F::Translation(cx-12*k,cy-12*k)*previous);
    const auto line=[&](float x1,float y1,float x2,float y2){target->DrawLine(D2D1::Point2F(x1,y1),D2D1::Point2F(x2,y2),brush,stroke,round);};
    const auto polyline=[&](std::initializer_list<D2D1_POINT_2F> points){
        ComPtr<ID2D1PathGeometry> path;ComPtr<ID2D1GeometrySink> sink;
        if(FAILED(factory->CreatePathGeometry(path.GetAddressOf()))||FAILED(path->Open(sink.GetAddressOf())))return;
        auto it=points.begin();sink->BeginFigure(*it,D2D1_FIGURE_BEGIN_HOLLOW);for(++it;it!=points.end();++it)sink->AddLine(*it);
        sink->EndFigure(D2D1_FIGURE_END_OPEN);if(SUCCEEDED(sink->Close()))target->DrawGeometry(path.Get(),brush,stroke,round);
    };
    switch(tool){
    case PinToolClose:line(6.5f,6.5f,17.5f,17.5f);line(17.5f,6.5f,6.5f,17.5f);break;
    case PinToolHang:if(const auto hanger=CreateHangerIcon(factory))target->DrawGeometry(hanger.Get(),brush,1.9f,round);break;
    case PinToolAnnotate:{
        ComPtr<ID2D1PathGeometry> path;ComPtr<ID2D1GeometrySink> sink;
        if(SUCCEEDED(factory->CreatePathGeometry(path.GetAddressOf()))&&SUCCEEDED(path->Open(sink.GetAddressOf()))){
            sink->BeginFigure(D2D1::Point2F(4.5f,19.5f),D2D1_FIGURE_BEGIN_HOLLOW);sink->AddLine(D2D1::Point2F(8.3f,18.6f));sink->AddLine(D2D1::Point2F(19.f,7.9f));
            sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(16.1f,5.f),D2D1::SizeF(2.f,2.f),0.f,D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,D2D1_ARC_SIZE_SMALL));
            sink->AddLine(D2D1::Point2F(5.4f,15.7f));sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            if(SUCCEEDED(sink->Close()))target->DrawGeometry(path.Get(),brush,stroke,round);
        }
        line(14.f,20.f,20.f,20.f);break;}
    case PinToolOcr:
        polyline({{4.f,8.f},{4.f,4.f},{8.f,4.f}});polyline({{16.f,4.f},{20.f,4.f},{20.f,8.f}});
        polyline({{20.f,16.f},{20.f,20.f},{16.f,20.f}});polyline({{8.f,20.f},{4.f,20.f},{4.f,16.f}});
        line(8.f,10.f,16.f,10.f);line(8.f,14.f,13.f,14.f);break;
    case PinToolCopy:
        target->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(8.5f,8.5f,20.f,20.f),2.f,2.f),brush,stroke,round);
        polyline({{15.5f,8.5f},{15.5f,4.f},{4.f,4.f},{4.f,15.5f},{8.5f,15.5f}});break;
    case PinToolMore:for(const float x:{5.5f,12.f,18.5f})target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x,12.f),1.5f,1.5f),brush);break;
    default:break;
    }
    target->SetTransform(previous);
}
// Tooltip text and key cap (full.html data-tip / data-k), with copy feedback.
std::wstring PinToolLabel(int tool,bool recognizing,bool has_text,bool copied){
    switch(tool){
    case PinToolHang:return L"晾到绳上";
    case PinToolClose:return L"关闭";
    case PinToolAnnotate:return L"标注";
    case PinToolOcr:return copied?L"已复制全部文字":recognizing?L"正在识别文字…":has_text?L"复制识别出的文字":L"识别文字";
    case PinToolCopy:return copied?L"已复制 · 已加入剪贴板历史":L"复制图片（加入历史）";
    case PinToolMore:return L"更多";
    default:return {};
    }
}
std::wstring PinToolKey(int tool){return tool==PinToolAnnotate?L"空格":tool==PinToolHang?L"H":L"";}
}  // namespace
void PinManager::Paint(Pin& p) {
    if(!p.factory)CheckWin32(SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,p.factory.GetAddressOf())),"Create pin renderer");
    if(!p.surface||!p.target||p.surface_width!=p.CanvasWidth()||p.surface_height!=p.CanvasHeight()){
        // D2D draws in place into the layered window's DIB (no per-frame GDI round trip).
        // Zoom resizes the canvas every frame; the image and shadow move to the new
        // target as shared software bitmaps instead of being uploaded again.
        auto surface=std::make_unique<DibSurface>(p.CanvasWidth(),p.CanvasHeight());
        auto target=CreateMemoryRenderTarget(p.factory.Get(),surface->Pixels(),p.CanvasWidth(),p.CanvasHeight(),p.CanvasWidth());
        ShareBitmap(target.Get(),p.bitmap);ShareBitmap(target.Get(),p.shadow_bitmap);p.target=std::move(target);
        p.surface=std::move(surface);p.surface_width=p.CanvasWidth();p.surface_height=p.CanvasHeight();
    }
    if(!p.bitmap){
        const auto bmp=D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_IGNORE),96,96);
        const auto shown=p.Shown();CheckWin32(SUCCEEDED(p.target->CreateBitmap(D2D1::SizeU(shown->Width(),shown->Height()),shown->pixels.data(),shown->Width()*4,bmp,&p.bitmap)),"Create pin image");++p.bitmap_uploads;
    }
    Draw(p,p.target.Get());RECT rect{};GetWindowRect(p.window,&rect);POINT origin=p.relocating?p.destination:POINT{rect.left,rect.top},source{};SIZE size{p.CanvasWidth(),p.CanvasHeight()};BLENDFUNCTION blend{AC_SRC_OVER,0,p.alpha,AC_SRC_ALPHA};
    p.presenting=true;const BOOL ok=UpdateLayeredWindow(p.window,nullptr,&origin,&size,p.surface->Dc(),&source,0,&blend,ULW_ALPHA);p.presenting=false;p.relocating=false;
    CheckWin32(ok!=FALSE,"Present paper and shadow atomically");ValidateRect(p.window,nullptr);
    if(p.hung&&!IsWindowVisible(p.window))p.ReleaseSurface();
    PlaceHoverBar(p);
}
void PinManager::Zoom(Pin& p,int delta,POINT anchor){
    if(!delta||p.moving||p.selecting||p.locked||p.editing)return;
    POINT local=anchor;ScreenToClient(p.window,&local);p.zoom_image={(local.x-p.Inset()-p.offset.x)/p.zoom,(local.y-p.Inset()-p.offset.y)/p.zoom};p.zoom_anchor=anchor;
    p.zoom_from=p.zoom;
    p.zoom_goal=PinZoomGoal(p.zoom_goal,delta,p.image->Width(),p.image->Height(),p.dpi,p.style);
    const auto now=GetTickCount64();
    if(!p.zoom_animating)p.zoom_frame_time=now;
    // Restart easing from the last displayed frame, not the last input event:
    // wheel messages arriving immediately before a timer must not reset t to zero.
    p.zoom_started=p.zoom_frame_time;p.zoom_badge_until=now+1100;
    // Retarget without postponing the next tick during high-rate wheel input.
    if(!p.zoom_animating)p.zoom_animating=SetTimer(p.window,101,16,nullptr)!=0;
    SetTimer(p.window,102,1100,nullptr);
}
void PinManager::Draw(Pin& p,ID2D1RenderTarget* target,const D2D1_RECT_F* clip) {
    ComPtr<ID2D1SolidColorBrush> brush;target->CreateSolidColorBrush(D2D1::ColorF(0x202733),brush.GetAddressOf());
    target->BeginDraw();target->SetTransform(D2D1::Matrix3x2F::Identity());
    if(clip)target->PushAxisAlignedClip(*clip,D2D1_ANTIALIAS_MODE_ALIASED);
    target->Clear(D2D1::ColorF(0,0.f));
    ComPtr<ID2D1Bitmap> shadow;
    if(target==p.target.Get())shadow=p.shadow_bitmap;
    if(!shadow&&!p.shadow_image.pixels.empty()){
        const auto& f=p.shadow_image;CheckWin32(SUCCEEDED(target->CreateBitmap(D2D1::SizeU(f.Width(),f.Height()),f.pixels.data(),f.Width()*4,D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),96,96),&shadow)),"Cache composite shadow");
        if(target==p.target.Get())p.shadow_bitmap=shadow;
    }
    target->SetTransform(D2D1::Matrix3x2F::Translation(p.offset.x,p.offset.y));if(shadow)DrawPaperShadowPatch(target,shadow.Get(),p.paper);
    target->SetTransform(D2D1::Matrix3x2F::Translation(p.paper.shadow+p.offset.x,p.paper.shadow+p.offset.y));DrawPaper(target,p.paper);
    target->SetTransform(D2D1::Matrix3x2F::Scale(p.zoom,p.zoom)*D2D1::Matrix3x2F::Translation(p.Inset()+p.offset.x,p.Inset()+p.offset.y));
    ComPtr<ID2D1Bitmap> bitmap;if(target==p.target.Get())bitmap=p.bitmap;
    const auto props=D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_IGNORE),96,96);
    if(!bitmap){const auto shown=p.Shown();CheckWin32(SUCCEEDED(target->CreateBitmap(D2D1::SizeU(static_cast<UINT>(shown->Width()),static_cast<UINT>(shown->Height())),shown->pixels.data(),static_cast<UINT>(shown->Width()*4),props,bitmap.GetAddressOf())),"Render pinned image");}
    target->DrawBitmap(bitmap.Get(),D2D1::RectF(0,0,float(p.image->Width()),float(p.image->Height())));
    DrawTextMarks(target,p.marks);
    brush->SetColor(D2D1::ColorF(0x3388ff,0.32f));
    for(auto b:SelectionBoxes(p.text,p.selection))target->FillRectangle(D2D1::RectF(b.left,b.top,b.right,b.bottom),brush.Get());
    DrawTextHandles(target,p.Handles(),p.dpi/96/p.zoom);
    target->SetTransform(D2D1::Matrix3x2F::Identity());
    if(GetTickCount64()<p.zoom_badge_until){
        const float scale=p.dpi/96,w=64*scale,h=26*scale,x=p.paper.shadow+(p.paper.width-w)/2+p.offset.x,y=p.paper.shadow+6*scale+p.offset.y;
        brush->SetColor(D2D1::ColorF(0x243142,.9f));target->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x,y,x+w,y+h),h/2,h/2),brush.Get());
        ComPtr<IDWriteFactory> dw;DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(dw.GetAddressOf()));ComPtr<IDWriteTextFormat> format;
        dw->CreateTextFormat(L"Segoe UI",nullptr,DWRITE_FONT_WEIGHT_SEMI_BOLD,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,13*scale,L"en-US",&format);
        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        const auto label=std::to_wstring(int(std::lround(p.zoom*100)))+L"%";p.text_renderer.Draw(target,dw.Get(),label,format.Get(),D2D1::RectF(x,y,x+w,y+h),D2D1::ColorF(0xffffff),false);
    }
    // The hover bar is its own window (PaintTools): never on this canvas, so never in exports.
    if(clip)target->PopAxisAlignedClip();
    if(target->EndDraw()==D2DERR_RECREATE_TARGET){p.bitmap.Reset();p.shadow_bitmap.Reset();p.target.Reset();InvalidateRect(p.window,nullptr,FALSE);}
}
// Where the bar window goes (screen pixels): the bar's bottom 8 DIP above the
// image with right edges aligned; below the image when the work area has no
// room above (full.html .hbar.below); always kept inside the work area.
bool PinManager::HoverBarRect(Pin& p,RECT& out){
    if(!p.window||!p.image)return false;
    RECT win{};GetWindowRect(p.window,&win);
    const float s=p.dpi/96;
    const float left=float(win.left)+float(p.Inset())+p.offset.x,top=float(win.top)+float(p.Inset())+p.offset.y;
    const float right=left+float(p.image->Width())*p.zoom,bottom=top+float(p.image->Height())*p.zoom;
    MONITORINFO monitor{sizeof(monitor)};GetMonitorInfoW(MonitorFromWindow(p.window,MONITOR_DEFAULTTONEAREST),&monitor);const RECT work=monitor.rcWork;
    Pin::ToolBar layout;p.bar_below=false;if(!p.ToolLayout(layout))return false;
    float y=top-8*s-layout.bar.bottom;
    if(y<float(work.top)){p.bar_below=true;p.ToolLayout(layout);y=bottom+8*s-layout.bar.top;}
    y=std::max(float(work.top),std::min(y,float(work.bottom)-layout.height));
    const float x=std::max(float(work.left),std::min(right-layout.bar.right,float(work.right)-layout.width));
    out={LONG(std::lround(x)),LONG(std::lround(y)),LONG(std::lround(x))+LONG(layout.width),LONG(std::lround(y))+LONG(layout.height)};
    return true;
}
// The pin moved or zoomed: move the visible bar with it.
void PinManager::PlaceHoverBar(Pin& p){
    if(!p.hbar||!IsWindowVisible(p.hbar)||p.tool_alpha<.999f)return;
    const bool below=p.bar_below;RECT want{},have{};
    if(!HoverBarRect(p,want)){ShowWindow(p.hbar,SW_HIDE);return;}
    GetWindowRect(p.hbar,&have);
    if(!EqualRect(&want,&have)||below!=p.bar_below)PaintTools(p);
}
// full.html tokens, solid glass as with acrylic off: --glass-solid, --edge,
// --ink, --card-hover; the tooltip is #tip (glass chip, .kbd key cap).
void PinManager::DrawHoverBar(Pin& p){
    Pin::ToolBar layout;ID2D1RenderTarget* const target=p.bar_target.Get();if(!target||!p.ToolLayout(layout))return;
    const bool dark=dark_theme&&dark_theme();const float s=p.dpi/96;
    const D2D1::ColorF glass=dark?D2D1::ColorF(0x1c2532):D2D1::ColorF(0xf5f8fc);
    const D2D1::ColorF edge=dark?D2D1::ColorF(0xffffff,.12f):D2D1::ColorF(0x0a192e,.12f);
    const D2D1::ColorF ink=dark?D2D1::ColorF(0xe9eef5):D2D1::ColorF(0x18212d);
    const D2D1::ColorF hover=dark?D2D1::ColorF(0xffffff,.11f):D2D1::ColorF(0x0a192e,.07f);
    ComPtr<ID2D1SolidColorBrush> brush;if(FAILED(target->CreateSolidColorBrush(ink,brush.GetAddressOf())))return;
    if(!p.round_stroke)p.factory->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND,D2D1_CAP_STYLE_ROUND,D2D1_CAP_STYLE_ROUND,D2D1_LINE_JOIN_ROUND),nullptr,0,p.round_stroke.GetAddressOf());
    target->BeginDraw();target->SetTransform(D2D1::Matrix3x2F::Identity());target->Clear(D2D1::ColorF(0,0.f));
    // Soft drop shadow (--shadow-s: 0 6px 18px) from stacked translucent plates.
    for(int i=6;i>=1;--i){
        const float g=float(i)*1.1f*s;auto r=layout.bar;r.left-=g;r.right+=g;r.top-=g-3*s;r.bottom+=g+3*s;
        brush->SetColor(D2D1::ColorF(dark?0x000000:0x0a192e,dark?.07f:.035f));target->FillRoundedRectangle(D2D1::RoundedRect(r,12*s+g,12*s+g),brush.Get());
    }
    const auto panel=D2D1::RoundedRect(layout.bar,12*s,12*s);
    brush->SetColor(glass);target->FillRoundedRectangle(panel,brush.Get());
    brush->SetColor(edge);target->DrawRoundedRectangle(panel,brush.Get(),s);
    {const float cy=(layout.bar.top+layout.bar.bottom)/2;target->DrawLine(D2D1::Point2F(layout.separator,cy-10*s),D2D1::Point2F(layout.separator,cy+10*s),brush.Get(),s);}
    for(int i=0;i<layout.count;++i){
        const int id=layout.order[i];auto r=layout.rect[id];
        if(p.tool_hover==id){
            auto fill=hover;
            if(p.tool_down==id){const float k=.03f*(r.right-r.left);r.left+=k;r.right-=k;r.top+=k;r.bottom-=k;fill.a*=1.7f;}  // .ib:active scale(.94)
            brush->SetColor(fill);target->FillRoundedRectangle(D2D1::RoundedRect(r,8*s,8*s),brush.Get());
        }
        auto color=ink;if(id==PinToolOcr&&p.recognizing)color.a=.35f;  // .ib[disabled]
        brush->SetColor(color);
        DrawPinToolIcon(target,p.factory.Get(),brush.Get(),p.round_stroke.Get(),id,(r.left+r.right)/2,(r.top+r.bottom)/2,s);
    }
    // Tooltip on the side away from the pin, centred on the hovered button.
    if(p.tip_ready&&p.tool_hover>=0&&p.tool_hover<PinToolCount&&p.tool_down<0){
        const bool copied=p.copied_tool==p.tool_hover&&GetTickCount64()<p.copied_until;
        const std::wstring label=PinToolLabel(p.tool_hover,p.recognizing,p.ocr_enabled&&!p.text.lines.empty(),copied),key=copied?std::wstring{}:PinToolKey(p.tool_hover);
        ComPtr<IDWriteFactory> dw;ComPtr<IDWriteTextFormat> format,key_format;
        if(!label.empty()&&SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(dw.GetAddressOf())))&&
            SUCCEEDED(dw->CreateTextFormat(L"Microsoft YaHei UI",nullptr,DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,12*s,L"zh-CN",&format))&&
            SUCCEEDED(dw->CreateTextFormat(L"Microsoft YaHei UI",nullptr,DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,11*s,L"zh-CN",&key_format))){
            const auto measure=[&](const std::wstring& text,IDWriteTextFormat* f){
                ComPtr<IDWriteTextLayout> text_layout;DWRITE_TEXT_METRICS metrics{};
                if(text.empty()||FAILED(dw->CreateTextLayout(text.c_str(),UINT32(text.size()),f,4096,256,&text_layout))||FAILED(text_layout->GetMetrics(&metrics)))return 0.f;
                return metrics.widthIncludingTrailingWhitespace;
            };
            const float label_width=std::ceil(measure(label,format.Get())),key_width=key.empty()?0.f:std::ceil(measure(key,key_format.Get())+10*s);
            const float w=label_width+(key_width>0?key_width+6*s:0)+18*s,h=24*s;
            const auto& hot=layout.rect[p.tool_hover];
            const float x=std::max(2*s,std::min((hot.left+hot.right)/2-w/2,layout.width-2*s-w));
            const float y=p.bar_below?layout.bar.bottom+6*s:layout.bar.top-6*s-h;
            const auto chip=D2D1::RoundedRect(D2D1::RectF(x,y,x+w,y+h),7*s,7*s);
            brush->SetColor(glass);target->FillRoundedRectangle(chip,brush.Get());
            brush->SetColor(edge);target->DrawRoundedRectangle(chip,brush.Get(),s);
            format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            p.bar_text.Draw(target,dw.Get(),label,format.Get(),D2D1::RectF(x+9*s,y,x+9*s+label_width+2*s,y+h),ink,false);
            if(key_width>0){
                const float kx=x+9*s+label_width+6*s;const auto cap=D2D1::RectF(kx,y+4*s,kx+key_width,y+h-4*s);
                brush->SetColor(edge);target->DrawRoundedRectangle(D2D1::RoundedRect(cap,4*s,4*s),brush.Get(),s);
                key_format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);key_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);key_format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                auto muted=ink;muted.a=.7f;p.bar_text.Draw(target,dw.Get(),key,key_format.Get(),cap,muted,false);
            }
        }
    }
    if(target->EndDraw()==D2DERR_RECREATE_TARGET){p.bar_target.Reset();p.bar_surface.reset();}
}
// Redraw and present the bar window (fade step, hover, press, tooltip, move).
void PinManager::PaintTools(Pin& p){
    const auto hide=[&p]{if(p.hbar&&IsWindowVisible(p.hbar))ShowWindow(p.hbar,SW_HIDE);};
    if(!p.window||!p.factory||p.tool_alpha<=.01f||p.hung||p.closing||!IsWindowVisible(p.window)){hide();return;}
    RECT place{};if(!HoverBarRect(p,place)){hide();return;}
    const int w=place.right-place.left,h=place.bottom-place.top;
    if(!p.hbar){
        p.hbar=CreateWindowExW(WS_EX_LAYERED|WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,L"LumaShot.PinBar",L"",WS_POPUP,
            place.left,place.top,w,h,p.window,nullptr,GetModuleHandleW(nullptr),&p);
        if(!p.hbar)return;
    }
    if(!p.bar_surface||!p.bar_target||p.bar_width!=w||p.bar_height!=h){
        p.bar_target.Reset();p.bar_surface=std::make_unique<DibSurface>(w,h);
        p.bar_target=CreateMemoryRenderTarget(p.factory.Get(),p.bar_surface->Pixels(),w,h,w);p.bar_width=w;p.bar_height=h;
    }
    DrawHoverBar(p);
    if(!p.bar_target||!p.bar_surface)return;
    // full.html: opacity plus a 5 px slide (translateY(5px) -> none) over 160 ms.
    const float a=std::clamp(p.tool_alpha,0.f,1.f);
    POINT origin{place.left,place.top+LONG(std::lround((1-a)*5*p.dpi/96))},source{};SIZE size{w,h};
    BLENDFUNCTION blend{AC_SRC_OVER,0,BYTE(std::lround(255*a)),AC_SRC_ALPHA};
    UpdateLayeredWindow(p.hbar,nullptr,&origin,&size,p.bar_surface->Dc(),&source,0,&blend,ULW_ALPHA);
    if(!IsWindowVisible(p.hbar)){SetWindowDisplayAffinity(p.hbar,WDA_NONE);SetWindowPos(p.hbar,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_SHOWWINDOW);}
}
LRESULT CALLBACK PinManager::HoverBarProc(HWND window,UINT message,WPARAM wp,LPARAM lp){
    auto* p=reinterpret_cast<Pin*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if(message==WM_NCCREATE){p=static_cast<Pin*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(p));}
    if(!p)return DefWindowProcW(window,message,wp,lp);
    try{switch(message){
    case WM_MOUSEACTIVATE:return MA_NOACTIVATE;
    case WM_NCHITTEST:{
        // Only the bar takes the mouse; the shadow and tooltip row are see-through.
        POINT point{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};ScreenToClient(window,&point);Pin::ToolBar layout;
        return p->ToolLayout(layout)&&float(point.x)>=layout.bar.left&&float(point.x)<layout.bar.right&&float(point.y)>=layout.bar.top&&float(point.y)<layout.bar.bottom?HTCLIENT:HTTRANSPARENT;}
    case WM_SETCURSOR:{POINT point{};GetCursorPos(&point);ScreenToClient(window,&point);SetCursor(LoadCursorW(nullptr,p->ToolAt(MAKELPARAM(point.x,point.y))>=0?IDC_HAND:IDC_ARROW));return TRUE;}
    case WM_MOUSEMOVE:{
        if(!p->bar_tracking){TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,window,0};p->bar_tracking=TrackMouseEvent(&track)!=FALSE;}
        p->bar_hovered=true;
        const int hot=p->ToolsWanted()?p->ToolAt(lp,0.f):-1;
        if(hot!=p->tool_hover&&p->tool_down<0){
            p->tool_hover=hot;p->tip_ready=false;
            if(p->window){KillTimer(p->window,109);if(hot>=0)SetTimer(p->window,109,380,nullptr);}
            p->manager->PaintTools(*p);
        }
        p->manager->UpdateHoverTools(*p);return 0;}
    case WM_MOUSELEAVE:
        p->bar_tracking=false;p->bar_hovered=false;p->leave_until=GetTickCount64()+250;
        if(p->tool_down<0&&(p->tool_hover>=0||p->tip_ready)){p->tool_hover=-1;p->tip_ready=false;p->manager->PaintTools(*p);}
        if(p->window)SetTimer(p->window,108,260,nullptr);
        return 0;
    case WM_LBUTTONDOWN:{
        const int tool=p->ToolAt(lp);if(tool<0)return 0;
        p->tool_down=p->tool_hover=tool;p->tip_ready=false;SetCapture(window);p->manager->PaintTools(*p);return 0;}
    case WM_LBUTTONUP:{
        if(p->tool_down<0)return 0;
        const int tool=p->tool_down;const bool hit=p->ToolAt(lp)==tool;p->tool_down=-1;
        if(GetCapture()==window)ReleaseCapture();
        p->manager->PaintTools(*p);
        if(hit)p->manager->HoverTool(*p,tool);
        return 0;}
    case WM_CAPTURECHANGED:if(p->tool_down>=0){p->tool_down=-1;p->manager->PaintTools(*p);}return 0;
    case WM_RBUTTONUP:{POINT point{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};ClientToScreen(window,&point);p->manager->Menu(*p,point);return 0;}
    case WM_NCDESTROY:p->hbar=nullptr;p->bar_target.Reset();p->bar_surface.reset();p->bar_tracking=p->bar_hovered=false;SetWindowLongPtrW(window,GWLP_USERDATA,0);break;
    }}catch(const std::exception& e){ui::ShowThemedMessage(p->window,p->manager->dark_theme&&p->manager->dark_theme(),L"LumaShot",ocr::ErrorMessage(e));}
    return DefWindowProcW(window,message,wp,lp);
}

void PinManager::Save(Pin& p) {
    if(p.saving)return;
    wchar_t filename[32768]=L"LumaShot-pin.png";OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.hwndOwner=p.window;dialog.lpstrFilter=L"PNG 图片 (*.png)\0*.png\0\0";dialog.lpstrFile=filename;dialog.nMaxFile=32768;dialog.lpstrDefExt=L"png";dialog.Flags=OFN_OVERWRITEPROMPT|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
    if(!GetSaveFileNameW(&dialog))return;
    const auto image=p.Shown();const auto marks=p.marks;const auto id=p.id;const HWND host=host_;const std::filesystem::path path=filename;p.saving=true;
    saves_.push_back(std::async(std::launch::async,[image,marks,id,path,host] {
        SaveResult result;result.id=id;const HRESULT com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
        std::filesystem::path temp=path;temp+=L".lumashot-pin-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(id)+L".tmp";
        bool reserved=false;
        try {
            CheckWin32(SUCCEEDED(com),"Initialize PNG export");ocr::Handle file(CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
            CheckWin32(file.Get()!=INVALID_HANDLE_VALUE,"Reserve PNG file");reserved=true;file.Reset();SavePng(FlattenTextMarks(*image,marks),temp);
            CheckWin32(MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE,"Save pinned image");
        }catch(const std::exception& e){result.error=ocr::ErrorMessage(e);}
        if(reserved)DeleteFileW(temp.c_str());if(SUCCEEDED(com))CoUninitialize();PostMessageW(host,kPinSaveReady,0,0);return result;
    }));
    SetTimer(host_,kPinSaveReady,100,nullptr);
}
void PinManager::Copy(Pin& p){
    if(p.saving)return;p.saving=true;
    const auto image=p.Shown();const auto marks=p.marks;const auto id=p.id;const auto host=host_;const int format=clipboard_format?clipboard_format():-1;
    saves_.push_back(std::async(std::launch::async,[image,marks,id,host,format]{
        SaveResult result;result.id=id;const HRESULT com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
        try{CheckWin32(SUCCEEDED(com),"Initialize clipboard export");Frame flattened;const Frame* source=image.get();if(!marks.empty()){flattened=FlattenTextMarks(*image,marks);source=&flattened;}auto file=format>=0?PrepareClipboardFile(*source,format):nullptr;result.clipboard_image=PrepareClipboardImage(*source,file);}
        catch(const std::exception& e){result.error=ocr::ErrorMessage(e);}
        if(SUCCEEDED(com))CoUninitialize();PostMessageW(host,kPinSaveReady,0,0);return result;
    }));SetTimer(host_,kPinSaveReady,100,nullptr);
}
void PinManager::Saved() {
    // Both completion messages and the timer can arrive in a modal message loop.
    if(processing_saves_)return;
    processing_saves_=true;
    struct Reset{bool& flag;~Reset(){flag=false;}} reset{processing_saves_};
    std::vector<SaveResult> completed;
    for(auto it=saves_.begin();it!=saves_.end();) {
        if(it->wait_for(std::chrono::seconds(0))!=std::future_status::ready){++it;continue;}
        completed.push_back(it->get());it=saves_.erase(it);
    }
    // Never retain a saves_ iterator across clipboard or dialog calls. New jobs
    // may be submitted while the dialog is open; the timer will drain them later.
    for(const auto& result:completed) {
        const auto pin=pins_.find(result.id);
        if(pin==pins_.end()||!pin->second->window)continue;
        const HWND owner=pin->second->window;pin->second->saving=false;
        if(!result.error.empty())ui::ShowThemedMessage(owner,dark_theme&&dark_theme(),L"导出失败",result.error);
        else if(result.clipboard_image)try{PublishClipboardImage(owner,*result.clipboard_image);}catch(const std::exception& e){ui::ShowThemedMessage(IsWindow(owner)?owner:host_,dark_theme&&dark_theme(),L"复制失败",ocr::ErrorMessage(e));}
    }
    if(saves_.empty())KillTimer(host_,kPinSaveReady);
}
void PinManager::Menu(Pin& source,POINT point) {
    const auto id=source.id;
    if(point.x==-1&&point.y==-1){RECT r{};GetWindowRect(source.window,&r);point={r.left+source.Inset()+16,r.top+source.Inset()+16};}
    PinMenuModel model;model.dark=dark_theme?dark_theme():false;model.status=source.status;model.locked=source.locked;model.recognized=source.ocr_enabled;model.table=source.text.table.has_value();
    model.ocr_available=ocr::Available();model.translated=source.tr.state==TranslationView::State::Done;model.selection=!source.selection.Empty()&&bool(translate_selection);
    model.enabled={bool(annotate)&&!source.editing,true,!source.selection.Empty(),!source.text.lines.empty(),model.table,true,!source.saving,!source.recognizing&&model.ocr_available,model.ocr_available&&!source.editing,!source.selection.Empty()&&bool(translate_selection),!source.saving,LineOn()&&!source.editing&&!source.hung,true};model.hang=LineOn();
    if(source.tools)ShowWindow(source.tools,SW_HIDE);
    const int command=TrackPinMenu(source.window,point,model);
    // OCR notifications may remove a closed pin during the popup's message loop.
    const auto found=pins_.find(id);if(found==pins_.end()||!found->second->window)return;
    auto& p=*found->second;
    if(command==1)CopyText(p.window,ocr::Selected(p.text,p.selection));if(command==2)CopyText(p.window,ocr::Selected(p.text,ocr::All(p.text)));
    if(command==9&&p.text.table)CopyText(p.window,ocr::TableTsv(*p.text.table));
    if(command==3||command==12)Copy(p);if(command==4)Save(p);if(command==5)Recognize(p);
    if(command==6){DestroyWindow(p.window);return;}if(command==13){HangPin(p);return;}if(command==10)TranslatePin(p);if(command==11&&translate_selection){auto text=ocr::Selected(p.text,p.selection);if(!text.empty())translate_selection(std::move(text),point);}if(command==7)Annotate(p);if(command==8)ToggleLock(p);UpdateTools(p);
}
void PinManager::ToggleLock(Pin& p){
    p.locked=!p.locked;KillTimer(p.window,101);p.zoom_animating=false;p.zoom_goal=p.zoom;p.moving=p.selecting=false;
    if(GetCapture()==p.window)ReleaseCapture();UpdateTools(p);SessionChanged();
}
void PinManager::Annotate(Pin& p){
    if(!annotate||p.editing)return;
    KillTimer(p.window,101);p.zoom_animating=false;p.zoom_goal=p.zoom;p.moving=p.selecting=false;
    if(GetCapture()==p.window)ReleaseCapture();
    POINT origin{};ClientToScreen(p.window,&origin);auto a=p.WindowPoint({0,0}),b=p.WindowPoint({float(p.image->Width()),float(p.image->Height())});
    RECT bounds{origin.x+LONG(std::lround(a.x)),origin.y+LONG(std::lround(a.y)),origin.x+LONG(std::lround(b.x)),origin.y+LONG(std::lround(b.y))};
    p.editing=true;if(p.tools)ShowWindow(p.tools,SW_HIDE);if(p.tr.panel)p.tr.panel->Hide();
    // Hide the hover bar at once (no fade) and keep it out of the screen copy the editor takes now.
    if(p.hbar){SetWindowDisplayAffinity(p.hbar,WDA_EXCLUDEFROMCAPTURE);ShowWindow(p.hbar,SW_HIDE);}
    if(p.tool_tick){KillTimer(p.window,105);p.tool_tick=0;}p.tool_alpha=0;p.tool_hover=-1;p.tip_ready=false;
    try{
        // Entering annotate must not copy the pin image twice on the UI thread.
        // Without text marks the shared source is forwarded with no copy at all.
        const auto flattened=p.marks.empty()?p.annotation_base:std::make_shared<Frame>(FlattenTextMarks(*p.annotation_base,p.marks));
        if(!annotate(p.id,flattened,p.annotations,bounds))p.editing=false;}catch(...){p.editing=false;throw;}
}
void PinManager::CompleteAnnotation(uint64_t id,std::optional<Frame> image,Document annotations,std::optional<Frame> base,bool recognize){
    const auto found=pins_.find(id);if(found==pins_.end()||!found->second->window)return;auto& p=*found->second;p.editing=false;
    if(base)base->bounds={0,0,base->Width(),base->Height()};
    if(image){++p.revision;image->bounds={0,0,image->Width(),image->Height()};p.image=std::make_shared<Frame>(std::move(*image));p.ocr_image=p.image;p.annotation_base=base?std::make_shared<Frame>(std::move(*base)):p.image;p.annotations=std::move(annotations);p.marks.clear();p.undo.clear();p.bitmap.Reset();p.target.Reset();p.shadow_bitmap.Reset();
        ResetTranslation(p);service_.Cancel(p.id);++p.version;p.text={};p.selection={};p.recognizing=false;p.status.clear();
        if(p.ocr_enabled||recognize)Recognize(p);}
    UpdateTools(p);InvalidateRect(p.window,nullptr,FALSE);SetForegroundWindow(p.window);SessionChanged();
}
void PinManager::UpdateTools(Pin& p){UpdateHoverTools(p);
    const auto boxes=SelectionBoxes(p.text,p.selection);
    if(boxes.empty()||p.selecting||p.moving||p.editing){if(p.tools)ShowWindow(p.tools,SW_HIDE);return;}
    Box selected=boxes.front();for(auto b:boxes){selected.left=std::min(selected.left,b.left);selected.right=std::max(selected.right,b.right);selected.top=std::min(selected.top,b.top);selected.bottom=std::max(selected.bottom,b.bottom);}
    POINT origin{p.Inset(),p.Inset()};ClientToScreen(p.window,&origin);selected.left=selected.left*p.zoom+origin.x+p.offset.x;selected.right=selected.right*p.zoom+origin.x+p.offset.x;selected.top=selected.top*p.zoom+origin.y+p.offset.y;selected.bottom=selected.bottom*p.zoom+origin.y+p.offset.y;
    MONITORINFO monitor{sizeof(monitor)};GetMonitorInfoW(MonitorFromPoint({LONG((selected.left+selected.right)/2),LONG(selected.bottom)},MONITOR_DEFAULTTONEAREST),&monitor);
    p.bar=PlaceSelectionBar(selected,monitor.rcWork,p.dpi/96);
    if(!p.tools)p.tools=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,L"LumaShot.SelectionTools",L"选中文字",WS_POPUP,0,0,1,1,p.window,nullptr,GetModuleHandleW(nullptr),&p);
    const auto r=p.bar.bounds;SetWindowPos(p.tools,HWND_TOPMOST,r.left,r.top,r.right-r.left,r.bottom-r.top,SWP_NOACTIVATE|SWP_SHOWWINDOW);
    const int w=r.right-r.left,h=r.bottom-r.top,n=int(8*p.bar.scale);
    HRGN region=CreateRoundRectRgn(0,p.bar.above?0:n,w+1,p.bar.above?h-n:h+1,int(28*p.bar.scale),int(28*p.bar.scale));
    POINT points[]={{w/2-n,p.bar.above?h-n:n},{w/2,p.bar.above?h:0},{w/2+n,p.bar.above?h-n:n}};HRGN triangle=CreatePolygonRgn(points,3,WINDING);CombineRgn(region,region,triangle,RGN_OR);DeleteObject(triangle);
    if(!SetWindowRgn(p.tools,region,TRUE))DeleteObject(region);
    InvalidateRect(p.tools,nullptr,FALSE);
}
void PinManager::ToolCommand(Pin& p,int command){
    if(p.selection.Empty())return;
    if(command==0){const auto text=ocr::Selected(p.text,p.selection);if(text.empty())return;CopyText(p.window,text);p.copied=true;if(p.tools){SetTimer(p.tools,1,1400,nullptr);InvalidateRect(p.tools,nullptr,FALSE);}return;}
    if(command<1||command>4)return;
    if(p.undo.size()>=50)p.undo.erase(p.undo.begin());p.undo.push_back(p.marks);
    SelectionMark mark{static_cast<TextDecoration>(command-1),SelectionBoxes(p.text,p.selection)};
    const auto same=[&](const SelectionMark& other){if(other.kind!=mark.kind||other.boxes.size()!=mark.boxes.size())return false;for(size_t i=0;i<mark.boxes.size();++i){const auto a=other.boxes[i],b=mark.boxes[i];if(a.left!=b.left||a.top!=b.top||a.right!=b.right||a.bottom!=b.bottom)return false;}return true;};
    auto existing=std::find_if(p.marks.begin(),p.marks.end(),same);if(existing!=p.marks.end())p.marks.erase(existing);else p.marks.push_back(std::move(mark));++p.revision;SessionChanged();
    InvalidateRect(p.window,nullptr,FALSE);
}
LRESULT CALLBACK PinManager::ToolsProc(HWND window,UINT message,WPARAM wp,LPARAM lp){
    auto* p=reinterpret_cast<Pin*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if(message==WM_NCCREATE){p=static_cast<Pin*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(p));}
    if(!p)return DefWindowProcW(window,message,wp,lp);
    try{switch(message){
    case WM_TIMER:if(wp==1){KillTimer(window,1);p->copied=false;InvalidateRect(window,nullptr,FALSE);}return 0;
    case WM_KEYDOWN:if(wp==VK_ESCAPE&&p->window){SendMessageW(p->window,WM_KEYDOWN,wp,lp);return 0;}break;
    case WM_MOUSEACTIVATE:return MA_NOACTIVATE;
    case WM_ERASEBKGND:return 1;
    case WM_SIZE:if(p->tools_target)p->tools_target->Resize(D2D1::SizeU(LOWORD(lp),HIWORD(lp)));return 0;
    case WM_PAINT:{PAINTSTRUCT ps{};BeginPaint(window,&ps);try{RECT r{};GetClientRect(window,&r);
        if(!p->factory)D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,p->factory.GetAddressOf());
        if(!p->tools_target)CheckWin32(SUCCEEDED(p->factory->CreateHwndRenderTarget(D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,D2D1::PixelFormat(),96,96),D2D1::HwndRenderTargetProperties(window,D2D1::SizeU(r.right,r.bottom)),&p->tools_target)),"Create selection tools");
        p->tools_target->BeginDraw();p->tools_target->Clear(D2D1::ColorF(0xf4f8ff));DrawSelectionBar(p->tools_target.Get(),float(r.right),float(r.bottom),p->bar.scale,p->tools_hover,p->bar.above,p->copied,p->tools_down);
        if(p->tools_target->EndDraw()==D2DERR_RECREATE_TARGET){p->tools_target.Reset();InvalidateRect(window,nullptr,FALSE);}
        }catch(...){EndPaint(window,&ps);throw;}EndPaint(window,&ps);return 0;}
    case WM_MOUSEMOVE:{RECT r{};GetClientRect(window,&r);const int hover=std::clamp(GET_X_LPARAM(lp)*5/std::max(1L,r.right),0L,4L);if(hover!=p->tools_hover){p->tools_hover=hover;InvalidateRect(window,nullptr,FALSE);}TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,window,0};TrackMouseEvent(&track);return 0;}
    case WM_MOUSELEAVE:p->tools_hover=-1;InvalidateRect(window,nullptr,FALSE);return 0;
    case WM_LBUTTONDOWN:{RECT r{};GetClientRect(window,&r);p->tools_down=std::clamp(GET_X_LPARAM(lp)*5/std::max(1L,r.right),0L,4L);SetCapture(window);InvalidateRect(window,nullptr,FALSE);return 0;}
    case WM_LBUTTONUP:{RECT r{};GetClientRect(window,&r);const POINT point{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};const int down=p->tools_down;p->tools_down=-1;ReleaseCapture();if(PtInRect(&r,point)&&down==point.x*5/std::max(1L,r.right))p->manager->ToolCommand(*p,down);return 0;}
    case WM_CAPTURECHANGED:p->tools_down=-1;InvalidateRect(window,nullptr,FALSE);return 0;
    case WM_NCDESTROY:p->tools=nullptr;p->tools_target.Reset();SetWindowLongPtrW(window,GWLP_USERDATA,0);break;
    }}catch(const std::exception& e){ui::ShowThemedMessage(p->window,p->manager->dark_theme&&p->manager->dark_theme(),L"LumaShot",ocr::ErrorMessage(e));}
    return DefWindowProcW(window,message,wp,lp);
}
LRESULT CALLBACK PinManager::Proc(HWND window,UINT message,WPARAM wp,LPARAM lp) {
    auto* p=reinterpret_cast<Pin*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if(message==WM_NCCREATE){p=static_cast<Pin*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);p->window=window;SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(p));}
    if(!p)return DefWindowProcW(window,message,wp,lp);
    try {
        switch(message) {
        case WM_PAINT:{PAINTSTRUCT ps{};BeginPaint(window,&ps);try{p->manager->Paint(*p);}catch(...){EndPaint(window,&ps);throw;}EndPaint(window,&ps);return 0;}
        case WM_ERASEBKGND:return 1;
        case WM_SIZE:if(!p->presenting)InvalidateRect(window,nullptr,FALSE);return 0;
        case WM_NCHITTEST:{POINT local{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};ScreenToClient(window,&local);HRGN region=PaperRegion(p->paper);const bool inside=PtInRegion(region,int(local.x-p->paper.shadow-p->offset.x),int(local.y-p->paper.shadow-p->offset.y))!=0;DeleteObject(region);return inside?HTCLIENT:HTTRANSPARENT;}
        case kDragOutMessage:if(p->moving&&p->from_line&&!p->hung&&!p->out_drag)p->manager->DragOut(*p);return 0;
        case WM_MOUSEWHEEL:p->manager->Zoom(*p,GET_WHEEL_DELTA_WPARAM(wp),{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)});return 0;
        case WM_TIMER:
            if(wp==101){
                if(!p->zoom_animating)return 0;
                p->zoom_frame_time=GetTickCount64();
                const float t=std::min(1.f,float(p->zoom_frame_time-p->zoom_started)/140.f),ease=1-std::pow(1-t,3.f);
                p->zoom=p->zoom_from+(p->zoom_goal-p->zoom_from)*ease;
                const auto layout=MakePaperLayout(std::max(1,int(std::lround(p->image->Width()*p->zoom))),std::max(1,int(std::lround(p->image->Height()*p->zoom))),p->dpi,p->style);
                p->manager->ApplyPaper(*p,{p->zoom_anchor.x-layout.inset-layout.shadow-p->zoom_image.x*p->zoom,p->zoom_anchor.y-layout.inset-layout.shadow-p->zoom_image.y*p->zoom});
                if(t>=1){KillTimer(window,101);p->zoom_animating=false;p->manager->SessionChanged();}return 0;
            }
            if(wp==104){p->manager->FlyStep(*p);return 0;}
            if(wp==105){p->manager->HoverToolsStep(*p);return 0;}
            if(wp==107){KillTimer(window,107);p->copied_tool=-1;p->copied_until=0;p->manager->PaintTools(*p);return 0;}
            if(wp==108){KillTimer(window,108);p->manager->UpdateHoverTools(*p);return 0;}
            if(wp==109){KillTimer(window,109);p->tip_ready=p->tool_hover>=0;p->manager->PaintTools(*p);return 0;}
            if(wp==106){const int a=int(p->alpha)-28;if(a<=0){KillTimer(window,106);DestroyWindow(window);return 0;}p->alpha=BYTE(a);p->manager->Paint(*p);return 0;}
            if(wp==103){KillTimer(window,103);p->manager->SessionChanged();return 0;}
            if(wp==102){KillTimer(window,102);p->zoom_badge_until=0;InvalidateRect(window,nullptr,FALSE);return 0;}break;
        case WM_DPICHANGED:{p->dpi=float(HIWORD(wp));const auto* r=reinterpret_cast<RECT*>(lp);p->manager->ApplyPaper(*p,{float(r->left),float(r->top)});return 0;}
        case WM_MOVE:if(!p->presenting)p->manager->PlacePanel(*p);if(!p->presenting&&!p->moving){p->manager->UpdateTools(*p);if(p->manager->session_writer_&&!p->manager->restoring_session_)SetTimer(window,103,180,nullptr);}return 0;
        case WM_SETCURSOR:if(LOWORD(lp)==HTCLIENT){POINT pt{};GetCursorPos(&pt);ScreenToClient(window,&pt);SetCursor(LoadCursorW(nullptr,ocr::OnText(p->text,p->ImagePoint(MAKELPARAM(pt.x,pt.y)))&&!(GetKeyState(VK_SPACE)&0x8000)?IDC_IBEAM:IDC_ARROW));return TRUE;}break;
        case WM_LBUTTONDOWN:case WM_LBUTTONDBLCLK:{
            if(p->editing||p->flight.active||p->closing)return 0;KillTimer(window,101);p->zoom_animating=false;p->zoom_goal=p->zoom;SetFocus(window);
            const auto point=p->ImagePoint(lp);
            const bool space=false;
            p->handle=space?-1:HitTextHandle(p->Handles(),point,p->dpi/96/p->zoom);
            if(p->handle>=0){
                const auto boxes=SelectionBoxes(p->text,p->selection);
                auto lo=std::min(p->selection.anchor,p->selection.caret),hi=std::max(p->selection.anchor,p->selection.caret);
                p->selection=p->handle==0?ocr::Selection{hi,lo}:ocr::Selection{lo,hi};
                const auto b=p->handle==0?boxes.front():boxes.back();
                p->handle_offset={(p->handle==0?b.left:b.right)-point.x,(b.top+b.bottom)/2-point.y};p->selecting=true;p->blank_click=false;p->clicks=0;
                SetCapture(window);p->manager->UpdateTools(*p);return 0;
            }
            const bool on_text=ocr::OnText(p->text,point);
            if(message==WM_LBUTTONDBLCLK&&!p->locked&&!space&&p->blank_click&&!p->dragged&&p->Blank(point)){DestroyWindow(window);return 0;}
            p->blank_click=!space&&p->Blank(point);p->dragged=false;
            if(space||!on_text) {
                if(p->locked)return 0;
                if(!space)p->selection={};
                p->moving=true;GetCursorPos(&p->drag);RECT r{};GetWindowRect(window,&r);p->origin={r.left,r.top};p->clicks=0;
            }else {
                const DWORD now=GetTickCount();const POINT local{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
                p->clicks=now-p->click_time<=GetDoubleClickTime()&&std::abs(local.x-p->last_click.x)<=GetSystemMetrics(SM_CXDOUBLECLK)&&std::abs(local.y-p->last_click.y)<=GetSystemMetrics(SM_CYDOUBLECLK)?p->clicks%3+1:1;
                p->click_time=now;p->last_click=local;
                if(p->clicks>=2)p->selection=ocr::Word(p->text,point,p->clicks==3);
                else {const auto hit=ocr::Hit(p->text,point);p->selection={hit,hit};p->selecting=true;}
            }SetCapture(window);p->manager->UpdateTools(*p);InvalidateRect(window,nullptr,FALSE);return 0;}
        case WM_MOUSEMOVE:
            if(!p->tracking){TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,window,0};p->tracking=TrackMouseEvent(&track)!=FALSE;}
            p->hovered=true;
            p->manager->UpdateHoverTools(*p);
            if(p->moving){POINT pt{};GetCursorPos(&pt);const bool was_dragged=p->dragged;if(std::abs(pt.x-p->drag.x)>=GetSystemMetrics(SM_CXDRAG)||std::abs(pt.y-p->drag.y)>=GetSystemMetrics(SM_CYDRAG))p->dragged=true;SetWindowPos(window,nullptr,p->origin.x+pt.x-p->drag.x,p->origin.y+pt.y-p->drag.y,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
                // Ctrl+drag: drop the picture into another program instead of only moving the pin.
                if(!was_dragged&&p->dragged&&!p->from_line&&(GetKeyState(VK_CONTROL)&0x8000)){p->manager->DragOut(*p);return 0;}
                // Near the top edge the clothesline drops down; the pin turns translucent to reveal the slot.
                if(p->dragged&&p->manager->LineOn()){const BYTE alpha=p->manager->line_->DragUpdate(pt,{p->image->Width(),p->image->Height()})?BYTE(150):BYTE(255);if(alpha!=p->alpha){p->alpha=alpha;p->manager->Paint(*p);}}}
            if(p->selecting){auto point=p->ImagePoint(lp);if(p->handle>=0){point.x+=p->handle_offset.x;point.y+=p->handle_offset.y;}p->selection.caret=ocr::Hit(p->text,point,true);InvalidateRect(window,nullptr,FALSE);}return 0;
        // Leaving the pin toward its bar: a short grace (timer 108) bridges the 8 DIP gap.
        case WM_MOUSELEAVE:p->tracking=false;p->hovered=false;p->leave_until=GetTickCount64()+250;SetTimer(window,108,260,nullptr);p->manager->UpdateHoverTools(*p);return 0;
        case WM_LBUTTONUP:
            if(p->moving&&p->dragged&&p->manager->DropOnLine(*p))return 0;if(p->moving)p->manager->EndLineDrag(*p);if(p->selecting)SendMessageW(window,WM_MOUSEMOVE,wp,lp);p->moving=p->selecting=false;p->handle=-1;if(GetCapture()==window)ReleaseCapture();p->manager->UpdateTools(*p);p->manager->SessionChanged();InvalidateRect(window,nullptr,FALSE);return 0;
        case WM_CAPTURECHANGED:if(p->moving)p->manager->EndLineDrag(*p);p->moving=p->selecting=false;p->handle=-1;p->tool_down=-1;p->manager->UpdateTools(*p);p->manager->UpdateHoverTools(*p);return 0;
        case WM_CONTEXTMENU:p->manager->Menu(*p,{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)});return 0;
        case WM_KEYDOWN:
            if(!(GetKeyState(VK_CONTROL)&0x8000)&&!(GetKeyState(VK_MENU)&0x8000)&&!(lp&(1LL<<30))){
                // Escape first dismisses a text selection; only a second Escape closes the pin.
                if(wp==VK_ESCAPE&&!p->selection.Empty()){p->selection={};p->manager->UpdateTools(*p);InvalidateRect(window,nullptr,FALSE);return 0;}
                if(wp==VK_ESCAPE){SendMessageW(window,WM_CLOSE,0,0);return 0;}
                if(wp==VK_SPACE){p->manager->Annotate(*p);return 0;}
                if(wp=='L'){p->manager->ToggleLock(*p);return 0;}
                if(wp=='H'&&!p->editing){p->manager->HangPin(*p);return 0;}
                if(wp=='T'&&ocr::Available()&&!p->editing){p->manager->TranslateKey(*p);return 0;}
            }
            if(GetKeyState(VK_CONTROL)&0x8000){if(wp=='A'){p->selection=ocr::All(p->text);p->manager->UpdateTools(*p);InvalidateRect(window,nullptr,FALSE);}if(wp=='C')CopyText(window,ocr::Selected(p->text,p->selection));if(wp=='Z'&&!p->undo.empty()){p->marks=std::move(p->undo.back());p->undo.pop_back();++p->revision;p->manager->SessionChanged();InvalidateRect(window,nullptr,FALSE);}}return 0;
        case WM_QUERYENDSESSION:p->manager->FlushSession();return TRUE;
        case WM_ENDSESSION:if(wp)p->manager->PreserveSession();return 0;
        case WM_CLOSE:DestroyWindow(window);return 0;
        case WM_DESTROY:KillTimer(window,104);p->flight.active=false;KillTimer(window,103);KillTimer(window,101);KillTimer(window,102);p->zoom_animating=false;p->moving=p->selecting=false;p->selection={};return 0;
        case WM_NCDESTROY:if(p->hung&&p->manager->line_)p->manager->line_->Remove(p->id);p->hung=false;if(p->tools)DestroyWindow(p->tools);if(p->tr.job&&p->manager->translator_)p->manager->translator_->Cancel(p->tr.job);p->tr.job=0;p->tr.image.reset();p->manager->service_.Cancel(p->id);p->window=nullptr;if(!p->manager->preserving_session_&&!p->manager->restoring_session_){p->manager->SessionChanged();p->manager->FlushSession();}p->image.reset();p->ocr_image.reset();p->text={};p->target.Reset();p->bitmap.Reset();SetWindowLongPtrW(window,GWLP_USERDATA,0);PostMessageW(p->manager->host_,kOcrReady,0,0);break;
        }
    }catch(const std::exception& e){ui::ShowThemedMessage(window,p->manager->dark_theme&&p->manager->dark_theme(),L"LumaShot",ocr::ErrorMessage(e));}
    return DefWindowProcW(window,message,wp,lp);
}
PinManager::Pin* PinManager::FindPin(uint64_t id){const auto it=pins_.find(id);return it!=pins_.end()&&it->second->window?it->second.get():nullptr;}
PinManager::FocusedText PinManager::FocusedSelection(){
    const HWND foreground=GetForegroundWindow();if(!foreground)return {};
    for(auto& [id,pin]:pins_){
        auto& p=*pin;if(!p.window||(foreground!=p.window&&(!p.tools||foreground!=p.tools)))continue;
        FocusedText out;out.pin=true;if(!p.selection.Empty())out.text=ocr::Selected(p.text,p.selection);return out;
    }
    return {};
}
void PinManager::TranslateLast(){if(auto* p=FindPin(last_created_))TranslatePin(*p);}
void PinManager::TranslatePin(Pin& p){
    using State=TranslationView::State;
    if(p.tr.state==State::Done){if(!p.tr.show)ShowTranslation(p,true);UpdatePanel(p);return;}
    if(p.tr.state==State::Running||p.tr.state==State::WaitingOcr){UpdatePanel(p);return;}
    StartOrWait(p);
}
void PinManager::TranslateKey(Pin& p){
    if(p.tr.state==TranslationView::State::Done){ShowTranslation(p,!p.tr.show);return;}
    TranslatePin(p);
}
void PinManager::StartOrWait(Pin& p){
    using State=TranslationView::State;
    const auto config=translate::LoadConfig(translate::DefaultConfigPath());
    std::wstring reason;
    if(!translate::Resolve(config,&reason)){
        p.tr.state=State::NeedsSetup;p.tr.error=reason;p.tr.engine.clear();UpdatePanel(p);
        // First use: open the guided settings once per session; later the panel offers it.
        if(!settings_prompted_&&open_translation_settings){settings_prompted_=true;open_translation_settings();}
        return;
    }
    if(!ocr::Available()){p.tr.state=State::Failed;p.tr.error=L"文字识别组件不可用，无法翻译";UpdatePanel(p);return;}
    // Offline engine: load the model while OCR runs instead of after it.
    if(!p.ocr_enabled||p.recognizing){translate::Prewarm(config);if(!p.ocr_enabled)Recognize(p);p.tr.state=State::WaitingOcr;UpdatePanel(p);return;}
    StartTranslation(p);
}
void PinManager::StartTranslation(Pin& p){
    using State=TranslationView::State;
    const auto config=translate::LoadConfig(translate::DefaultConfigPath());
    std::wstring reason;auto credentials=translate::Resolve(config,&reason);
    if(!credentials){p.tr.state=State::NeedsSetup;p.tr.error=reason;UpdatePanel(p);return;}
    p.tr.engine=std::wstring(credentials->preset->label);
    p.tr.blocks=translate::BuildBlocks(p.text);p.tr.results.clear();
    if(p.tr.blocks.empty()){p.tr.state=State::Failed;p.tr.error=L"图片中没有识别到可翻译的文字";UpdatePanel(p);return;}
    std::wstring all;for(const auto& block:p.tr.blocks){all+=block.text;all+=L'\n';}
    p.tr.source=translate::Detect(all);
    p.tr.target=translate::ResolveTarget(p.tr.requested!=translate::Language::Auto?p.tr.requested:config.target,p.tr.source);
    translate::Job job;job.source=p.tr.source;job.target=p.tr.target;
    for(const auto& block:p.tr.blocks)job.texts.push_back(block.text);
    if(!translator_)translator_=std::make_unique<translate::Service>(host_,kTranslateReady);
    if(p.tr.job)translator_->Cancel(p.tr.job);
    p.tr.job=translator_->Submit(std::move(*credentials),std::move(job));
    p.tr.state=State::Running;p.tr.error.clear();UpdatePanel(p);
}
void PinManager::TranslationReady(uint64_t job){
    if(!translator_||!job)return;
    auto outcome=translator_->Take(job);
    if(!outcome)return;
    Pin* found=nullptr;for(auto& [id,pin]:pins_)if(pin->window&&pin->tr.job==job){found=pin.get();break;}
    if(!found)return;
    auto& p=*found;p.tr.job=0;
    using State=TranslationView::State;
    if(!outcome->ok||outcome->texts.size()!=p.tr.blocks.size()){p.tr.state=State::Failed;p.tr.error=outcome->error.empty()?L"翻译服务没有返回结果":outcome->error;UpdatePanel(p);return;}
    p.tr.results=std::move(outcome->texts);
    try{p.tr.image=std::make_shared<Frame>(pin_translation::Render(*p.image,p.tr.blocks,p.tr.results,p.tr.target));}
    catch(const std::exception& e){p.tr.image.reset();p.tr.state=State::Failed;p.tr.error=L"译文渲染失败："+ocr::ErrorMessage(e);UpdatePanel(p);return;}
    p.tr.state=State::Done;ShowTranslation(p,!translate_auto_show||translate_auto_show());
}
void PinManager::ClotheslineConfigChanged(){
    if(!line_)return;
    const bool on=LineOn();
    // Off: the rope sends every hung pin home through RetrievePin (one flight
    // each) and folds away; drags near the top edge and H no longer hang.
    line_->SetEnabled(on);
    for(auto& [id,pin]:pins_){
        auto& p=*pin;
        if(!on&&p.hung&&!line_->Contains(id)&&p.window){
            // Not on the rope any more (should not happen): show it at home.
            p.hung=false;p.from_line=false;p.alpha=255;
            SetWindowPos(p.window,HWND_TOPMOST,p.hang_origin.x,p.hang_origin.y,0,0,SWP_NOSIZE|SWP_NOACTIVATE|SWP_SHOWWINDOW);
        }
        if(p.hang_tool==on)continue;
        p.hang_tool=on;if(!on&&p.tool_hover==0)p.tool_hover=-1;if(!on&&p.tool_down==0)p.tool_down=-1;
        if(p.window&&!p.hung){Paint(p);PaintTools(p);}
    }
    if(on&&!pins_.empty())line_->Arm();
}
void PinManager::TranslationConfigChanged(){
    using State=TranslationView::State;
    for(auto& [id,pin]:pins_)if(pin->window&&(pin->tr.state==State::NeedsSetup||(pin->tr.state==State::Failed&&pin->tr.panel&&pin->tr.panel->Visible())))StartOrWait(*pin);
}
void PinManager::ResetTranslation(Pin& p){
    if(p.tr.job&&translator_)translator_->Cancel(p.tr.job);
    const bool had_panel=p.tr.panel&&p.tr.panel->Visible();
    auto panel=std::move(p.tr.panel);const auto requested=p.tr.requested;
    p.tr={};p.tr.requested=requested;p.tr.panel=std::move(panel);
    if(had_panel)p.tr.panel->Hide();
}
void PinManager::ShowTranslation(Pin& p,bool show){
    p.tr.show=show&&p.tr.image;p.bitmap.Reset();p.selection={};UpdateTools(p);
    if(p.window)Paint(p);
    UpdatePanel(p);
}
void PinManager::EnsurePanel(Pin& p){
    if(p.tr.panel&&p.tr.panel->Window())return;
    const auto id=p.id;
    TranslationPanel::Callbacks callbacks;
    callbacks.show_translation=[this,id](bool show){if(auto* pin=FindPin(id))ShowTranslation(*pin,show);};
    callbacks.retarget=[this,id](translate::Language language){
        auto* pin=FindPin(id);if(!pin)return;pin->tr.requested=language;
        using State=TranslationView::State;
        if(pin->tr.state==State::Done||pin->tr.state==State::Failed){pin->tr.show=false;pin->tr.image.reset();pin->bitmap.Reset();Paint(*pin);StartOrWait(*pin);}
        else UpdatePanel(*pin);
    };
    callbacks.retry=[this,id]{if(auto* pin=FindPin(id))StartOrWait(*pin);};
    callbacks.settings=[this]{if(open_translation_settings)open_translation_settings();};
    // Closing only hides the panel; the translated rendering stays on the pin.
    callbacks.closed=[this,id]{if(auto* pin=FindPin(id);pin&&pin->tr.panel)pin->tr.panel->Hide();};
    p.tr.panel=std::make_unique<TranslationPanel>(p.window,std::move(callbacks));
}
void PinManager::UpdatePanel(Pin& p){
    if(!p.window)return;
    EnsurePanel(p);
    TranslationView view;view.state=p.tr.state;view.engine=p.tr.engine;view.error=p.tr.error;view.source=p.tr.source;view.target=p.tr.target;view.requested=p.tr.requested;
    view.show_translation=p.tr.show;view.dark=dark_theme&&dark_theme();
    if(p.tr.state==TranslationView::State::Done){for(const auto& block:p.tr.blocks)view.sources.push_back(block.text);view.results=p.tr.results;}
    else if(p.tr.state==TranslationView::State::Running){for(const auto& block:p.tr.blocks)view.sources.push_back(block.text);}
    p.tr.panel->Update(view);
    RECT r{};GetWindowRect(p.window,&r);
    const LONG left=r.left+p.paper.shadow+LONG(p.offset.x),top=r.top+p.paper.shadow+LONG(p.offset.y);
    p.tr.panel->Place({left,top,left+p.paper.width,top+p.paper.height});
    p.tr.panel->Show();
}
void PinManager::PlacePanel(Pin& p){
    if(!p.window||!p.tr.panel||!p.tr.panel->Visible())return;
    RECT r{};GetWindowRect(p.window,&r);
    // During ApplyPaper the window has not moved yet; use the pending destination.
    const POINT origin=p.relocating?p.destination:POINT{r.left,r.top};
    const LONG left=origin.x+p.paper.shadow+LONG(p.offset.x),top=origin.y+p.paper.shadow+LONG(p.offset.y);
    p.tr.panel->Place({left,top,left+p.paper.width,top+p.paper.height});
}
void PinManager::HangPin(Pin& p,bool keep_home){
    if(!LineOn()||p.hung||p.editing||!p.window||!IsWindowVisible(p.window))return;
    KillTimer(p.window,101);p.zoom_animating=false;p.zoom_goal=p.zoom;p.moving=p.selecting=false;p.flight.active=false;
    if(GetCapture()==p.window)ReleaseCapture();
    RECT r{};GetWindowRect(p.window,&r);
    const float left=float(r.left)+float(p.Inset())+p.offset.x,top=float(r.top)+float(p.Inset())+p.offset.y;
    const RECT from{LONG(std::lround(left)),LONG(std::lround(top)),LONG(std::lround(left+float(p.image->Width())*p.zoom)),LONG(std::lround(top+float(p.image->Height())*p.zoom))};
    if(!keep_home)p.hang_origin={r.left,r.top};
    p.hung=true;p.from_line=false;p.alpha=255;p.selection={};
    KillTimer(p.window,105);p.hovered=p.tracking=false;p.tool_hover=p.tool_down=-1;p.tool_alpha=0;p.tool_tick=0;
    p.bar_hovered=p.tip_ready=false;p.leave_until=0;if(p.hbar)ShowWindow(p.hbar,SW_HIDE);
    if(p.tools)ShowWindow(p.tools,SW_HIDE);
    if(p.tr.panel)p.tr.panel->Hide();
    ShowWindow(p.window,SW_HIDE);p.ReleaseSurface();
    line_->Hang(p.id,p.Shown(),from);
    SessionChanged();
}
void PinManager::UpdateHoverTools(Pin& p){
    if(!p.window)return;
    const float goal=p.ToolsWanted()?1.f:0.f;
    if(goal==p.tool_alpha){if(p.tool_tick){KillTimer(p.window,105);p.tool_tick=0;}if(goal==0){if(p.hbar&&IsWindowVisible(p.hbar))ShowWindow(p.hbar,SW_HIDE);}else PlaceHoverBar(p);return;}
    if(!p.tool_tick){p.tool_tick=GetTickCount64();SetTimer(p.window,105,15,nullptr);}
}
// 160 ms fade (prototype: .16s), redrawn through the layered window.
void PinManager::HoverToolsStep(Pin& p){
    const auto now=GetTickCount64();const float step=float(now-p.tool_tick)/160.f;p.tool_tick=now;
    const float goal=p.ToolsWanted()?1.f:0.f;
    p.tool_alpha=goal>p.tool_alpha?std::min(goal,p.tool_alpha+step):std::max(goal,p.tool_alpha-step);
    if(p.tool_alpha==goal){KillTimer(p.window,105);p.tool_tick=0;}
    PaintTools(p);
}
// Hover bar buttons (full.html pinAction).
void PinManager::HoverTool(Pin& p,int tool){
    if(!p.window)return;
    switch(tool){
    case PinToolHang:HangPin(p);return;
    case PinToolClose:FadeClose(p);return;
    case PinToolAnnotate:Annotate(p);return;
    case PinToolCopy:
        // The published image is picked up by the clipboard history like any copy.
        Copy(p);p.copied_tool=PinToolCopy;p.copied_until=GetTickCount64()+1400;SetTimer(p.window,107,1400,nullptr);PaintTools(p);return;
    case PinToolOcr:
        if(p.recognizing||!ocr::Available())return;
        if(p.ocr_enabled&&!p.text.lines.empty()){
            CopyText(p.window,ocr::Selected(p.text,ocr::All(p.text)));
            p.copied_tool=PinToolOcr;p.copied_until=GetTickCount64()+1400;SetTimer(p.window,107,1400,nullptr);PaintTools(p);return;
        }
        Recognize(p);PaintTools(p);return;
    case PinToolMore:{
        // The pin menu opens under the button (it holds everything the bar dropped).
        Pin::ToolBar bar;if(!p.ToolLayout(bar))return;
        const auto& r=bar.rect[PinToolMore];POINT anchor{LONG(std::lround(r.left)),LONG(std::lround(r.bottom+6*p.dpi/96))};ClientToScreen(p.hbar?p.hbar:p.window,&anchor);
        p.tool_hover=-1;p.tip_ready=false;PaintTools(p);Menu(p,anchor);return;}
    default:return;
    }
}
// Close button: fade the pin out over about 140 ms, then destroy it.
void PinManager::FadeClose(Pin& p){
    if(p.closing||!p.window)return;
    p.closing=true;p.tool_hover=-1;KillTimer(p.window,105);p.tool_tick=0;if(p.hbar)ShowWindow(p.hbar,SW_HIDE);
    if(p.tools)ShowWindow(p.tools,SW_HIDE);
    if(p.tr.panel)p.tr.panel->Hide();
    SetTimer(p.window,106,15,nullptr);
}
bool PinManager::DropOnLine(Pin& p){
    if(!LineOn())return false;
    POINT cursor{};GetCursorPos(&cursor);
    if(!line_->DragDrop(p.id,p.Shown(),cursor)){EndLineDrag(p);return false;}
    // A pin pulled off the rope keeps its original home; otherwise home is where this drag began.
    if(!p.from_line)p.hang_origin=p.origin;
    p.from_line=false;p.moving=p.selecting=false;p.hung=true;p.alpha=255;p.selection={};
    if(GetCapture()==p.window)ReleaseCapture();
    if(p.tools)ShowWindow(p.tools,SW_HIDE);
    if(p.tr.panel)p.tr.panel->Hide();
    // Park the hidden window at home: the session keeps that position.
    SetWindowPos(p.window,nullptr,p.hang_origin.x,p.hang_origin.y,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_HIDEWINDOW);p.ReleaseSurface();
    SessionChanged();
    return true;
}
void PinManager::EndLineDrag(Pin& p){
    if(line_)line_->DragEnd();
    p.from_line=false;
    if(p.alpha!=255){p.alpha=255;if(p.window&&!p.hung)Paint(p);}
}
void PinManager::RetrievePin(uint64_t id,RECT from){
    auto* p=FindPin(id);if(!p||!p->hung)return;
    p->hung=false;p->from_line=false;p->alpha=255;
    const float goal=p->zoom;
    const float start=std::clamp(float(from.right-from.left)/float(std::max(1,p->image->Width())),0.02f,std::max(0.02f,goal));
    const auto layout=MakePaperLayout(std::max(1,int(std::lround(float(p->image->Width())*goal))),std::max(1,int(std::lround(float(p->image->Height())*goal))),p->dpi,p->style);
    p->flight={true,GetTickCount64(),start,goal,{float(from.left),float(from.top)},
        {float(p->hang_origin.x+layout.inset+layout.shadow),float(p->hang_origin.y+layout.inset+layout.shadow)}};
    FlyStep(*p);
    ShowWindow(p->window,SW_SHOWNOACTIVATE);
    SetWindowPos(p->window,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
    if(p->flight.active&&!SetTimer(p->window,104,15,nullptr)){p->flight.start=0;FlyStep(*p);}
}
void PinManager::FlyStep(Pin& p){
    auto& f=p.flight;
    if(!f.active||!p.window){KillTimer(p.window,104);return;}
    const float t=std::clamp(float(GetTickCount64()-f.start)/320.f,0.f,1.f),e=1-std::pow(1-t,3.f);
    // Slight overshoot on position so the pin lands instead of stopping dead.
    const float c=1.4f,back=1+(c+1)*std::pow(t-1,3.f)+c*std::pow(t-1,2.f);
    p.zoom=p.zoom_from=p.zoom_goal=f.from_zoom+(f.to_zoom-f.from_zoom)*e;
    if(t>=1){
        f.active=false;KillTimer(p.window,104);p.zoom=p.zoom_from=p.zoom_goal=f.to_zoom;
        ApplyPaper(p,{float(p.hang_origin.x),float(p.hang_origin.y)});SessionChanged();return;
    }
    const auto layout=MakePaperLayout(std::max(1,int(std::lround(float(p.image->Width())*p.zoom))),std::max(1,int(std::lround(float(p.image->Height())*p.zoom))),p.dpi,p.style);
    ApplyPaper(p,{f.from.x+(f.to.x-f.from.x)*back-float(layout.inset+layout.shadow),f.from.y+(f.to.y-f.from.y)*back-float(layout.inset+layout.shadow)});
}
void PinManager::DetachPin(uint64_t id,POINT cursor){
    auto* p=FindPin(id);if(!p||!p->hung)return;
    p->hung=false;p->from_line=true;p->flight.active=false;p->alpha=150;
    const float w=float(p->image->Width())*p->zoom,h=float(p->image->Height())*p->zoom;
    const auto layout=MakePaperLayout(std::max(1,int(std::lround(w))),std::max(1,int(std::lround(h))),p->dpi,p->style);
    // Held near the top edge, as if by the clothespin.
    ApplyPaper(*p,{float(cursor.x)-w*.5f-float(layout.inset+layout.shadow),float(cursor.y)-std::min(h*.06f,24.f*p->dpi/96)-float(layout.inset+layout.shadow)});
    ShowWindow(p->window,SW_SHOWNOACTIVATE);
    SetWindowPos(p->window,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
    RECT r{};GetWindowRect(p->window,&r);
    p->moving=p->dragged=true;p->selecting=false;p->drag=cursor;p->origin={r.left,r.top};
    SetCapture(p->window);
    if(line_)line_->DragUpdate(cursor,{p->image->Width(),p->image->Height()});
    // Without Shift the card is dragged into other programs (Shift: place it as a pin, as before).
    const int button=GetSystemMetrics(SM_SWAPBUTTON)?VK_RBUTTON:VK_LBUTTON;
    if(!(GetKeyState(VK_SHIFT)&0x8000)&&(GetAsyncKeyState(button)&0x8000))PostMessageW(p->window,kDragOutMessage,0,0);
}
void PinManager::DragOut(Pin& p){
    if(!p.window||!p.image||p.out_drag)return;
    const uint64_t id=p.id;const bool from_line=p.from_line;const POINT home=p.origin;
    POINT cursor{};GetCursorPos(&cursor);RECT r{};GetWindowRect(p.window,&r);
    const POINT grab{cursor.x-r.left,cursor.y-r.top};
    // The window keeps following the cursor but must not hide the program
    // under it from the drop: a layered + transparent window is skipped by hit testing.
    p.out_drag=true;p.moving=false;
    if(GetCapture()==p.window)ReleaseCapture();
    const LONG_PTR style=GetWindowLongPtrW(p.window,GWL_EXSTYLE);
    SetWindowLongPtrW(p.window,GWL_EXSTYLE,style|WS_EX_TRANSPARENT);
    UpdateHoverTools(p);
    DragOutOptions options;
    options.image=[image=p.Shown(),marks=p.marks]()->std::shared_ptr<const Frame>{
        if(marks.empty())return image;
        return std::make_shared<const Frame>(FlattenTextMarks(*image,marks));
    };
    options.file_format=clipboard_format?clipboard_format():-1;
    options.moved=[this,id,grab](POINT pt,bool over_target){
        auto* q=FindPin(id);if(!q||!q->window)return;
        SetWindowPos(q->window,nullptr,pt.x-grab.x,pt.y-grab.y,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
        // Over a program that takes the picture the pin turns into a faint
        // ghost so the input box stays visible; near the top it previews the rope slot.
        const bool slot=LineOn()&&line_->DragUpdate(pt,{q->image->Width(),q->image->Height()});
        const BYTE alpha=over_target?BYTE(110):slot?BYTE(150):BYTE(255);
        if(alpha!=q->alpha){q->alpha=alpha;Paint(*q);}
    };
    const auto result=DragImageOut(std::move(options));
    auto* q=FindPin(id);if(!q||!q->window)return;  // closed during the drag
    SetWindowLongPtrW(q->window,GWL_EXSTYLE,style);
    q->out_drag=false;
    const auto restore=[&]{  // back to where the drag started, fully opaque
        if(line_)line_->DragEnd();
        q->from_line=false;q->alpha=255;
        SetWindowPos(q->window,nullptr,home.x,home.y,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
        Paint(*q);UpdateTools(*q);SessionChanged();
    };
    switch(result){
    case DragOutResult::Dropped:
    case DragOutResult::Canceled:
        // A card from the rope goes back up (its home is unchanged); a pin returns to where it was.
        if(from_line&&LineOn()){if(line_)line_->DragEnd();q->alpha=255;HangPin(*q,true);}
        else restore();
        return;
    case DragOutResult::Failed:
        // No OLE drag possible: continue as an ordinary move while the button is held.
        if(GetAsyncKeyState(GetSystemMetrics(SM_SWAPBUTTON)?VK_RBUTTON:VK_LBUTTON)&0x8000){
            GetCursorPos(&cursor);GetWindowRect(q->window,&r);
            q->moving=q->dragged=true;q->drag=cursor;q->origin={r.left,r.top};SetCapture(q->window);return;
        }
        [[fallthrough]];
    case DragOutResult::NoTarget:
        // Released where nothing takes the picture: the usual drop (rope slot, or the pin stays here).
        q->moving=q->dragged=true;
        if(DropOnLine(*q))return;
        EndLineDrag(*q);q->moving=false;UpdateTools(*q);SessionChanged();InvalidateRect(q->window,nullptr,FALSE);
        return;
    }
}
}