#include "app/application.h"
#include "export/png.h"
#include "model/note_color.h"
#include "ui/selection_render.h"
#include "ui/fonts.h"
#include "model/number_label.h"
#include "number_reconnect_cases.h"
#include <string_view>
#include <commctrl.h>
#include <imm.h>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <filesystem>

namespace lumashot {
struct TextEditTest {
    static void Pump(){MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}}
    static void Key(HWND window,WPARAM key,bool control=false,bool shift=false){
        BYTE saved[256]{};GetKeyboardState(saved);BYTE pressed[256]{};std::copy_n(saved,256,pressed);
        pressed[VK_CONTROL]=control?0x80:0;pressed[VK_SHIFT]=shift?0x80:0;SetKeyboardState(pressed);
        SendMessageW(window,WM_KEYDOWN,key,0);if(key==VK_RETURN&&shift&&IsWindow(window))SendMessageW(window,WM_CHAR,VK_RETURN,0);SetKeyboardState(saved);
    }
    static Frame Preview(Application& app){
        auto image=app.edit_frame_->Snapshot();const RECT area=app.edit_frame_->TextRect();
        DibSurface text(area.right-area.left,area.bottom-area.top);PrintWindow(app.edit_host_,text.Dc(),0);
        for(int y=0;y<area.bottom-area.top;++y)for(int x=0;x<area.right-area.left;++x)
            image.pixels[size_t(y+area.top)*image.Width()+x+area.left]=text.Pixels()[size_t(y)*(area.right-area.left)+x]|0xff000000;
        const uint32_t background=app.state_.dark?0xff151b23u:0xffffffffu;
        for(auto& pixel:image.pixels){const unsigned alpha=pixel>>24;uint32_t result=0xff000000;for(int shift:{0,8,16})result|=std::min(255u,((pixel>>shift)&255)+(((background>>shift)&255)*(255-alpha)+127)/255)<<shift;pixel=result;}
        return image;
    }
    static Frame Canvas(Application& app){Renderer renderer;return renderer.Canvas(*app.frame_,app.document_,app.state_,app.edit_&&!app.edit_badge_label_?app.edit_index_:-1);}
    #include "number_label_editor_cases.h"
    #include "tag_editor_cases.h"
    static int Run(bool numberLabelOnly=false){
        std::cout<<std::unitbuf;
        int failures=0;const auto expect=[&](bool value,const char* label){std::cout<<(value?"PASS ":"FAIL ")<<label<<'\n';if(!value)++failures;};
        Application app;const RECT bounds{40,40,940,640};app.monitors_={bounds};
        app.frame_=std::make_shared<Frame>(MakeFrame(bounds,0xffeef2f7));app.acrylic_=app.frame_;
        WNDCLASSW wc{};wc.lpfnWndProc=Application::OverlayProc;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"LumaShot.TextTestOverlay";RegisterClassW(&wc);
        auto view=std::make_unique<Application::View>();view->app=&app;view->bounds=bounds;view->renderer=std::make_unique<Renderer>();
        view->window=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_TOPMOST,wc.lpszClassName,L"Synthetic text editing fixture",WS_POPUP,40,40,900,600,nullptr,nullptr,wc.hInstance,view.get());
        if(!view->window)return 2;
        auto& v=*view;app.views_.push_back(std::move(view));
        auto& state=app.state_;state.selected=true;state.selection={80,80,850,530};state.tool=Tool::Text;
        state.toolbar=PlaceToolbar(state.selection,bounds,1,1,Tool::Text);
        ShowWindow(v.window,SW_SHOW);SetForegroundWindow(v.window);UpdateWindow(v.window);Pump();
        if(numberLabelOnly){LabelEditorCases(app,v,expect);return failures?1:0;}
        state.color=0xff243142;app.BeginText(v,{140,150});Pump();
        expect(app.edit_&&app.edit_host_&&!app.edit_frame_&&GetAncestor(app.edit_,GA_ROOT)==app.edit_host_&&GetWindow(app.edit_host_,GW_OWNER)==v.window,"in-place text opens no popup card; the input host is owned by the canvas");
        {const auto ex=GetWindowLongPtrW(app.edit_host_,GWL_EXSTYLE);BYTE alpha=255;DWORD flags{};GetLayeredWindowAttributes(app.edit_host_,nullptr,&alpha,&flags);
         expect((ex&WS_EX_LAYERED)&&(ex&WS_EX_TRANSPARENT)&&(flags&LWA_ALPHA)&&alpha<=1,"native input host is invisible and lets clicks reach the canvas");}
        GUITHREADINFO gui{sizeof(gui)};GetGUIThreadInfo(GetCurrentThreadId(),&gui);
        expect(gui.hwndFocus==app.edit_&&app.state_.text_edit.mark&&app.state_.text_edit.mark->tool==Tool::Text&&app.state_.text_edit.caret==0&&app.state_.text_edit.caret_visible,"empty in-place editor owns keyboard focus and shows a canvas caret at the click point");
        const auto empty_box=app.state_.text_edit.mark?Normalize(app.state_.text_edit.mark->a,app.state_.text_edit.mark->b):Box{};
        expect(empty_box.left==140&&empty_box.right-empty_box.left<8,"empty automatic text collapses to the insertion point instead of a wide input field");
        for(wchar_t ch:std::wstring(L"可再次编辑的文字"))SendMessageW(app.edit_,WM_CHAR,ch,0);
        Key(app.edit_,VK_RETURN,false,true);
        for(wchar_t ch:std::wstring(L"第二行，也能实时看见"))SendMessageW(app.edit_,WM_CHAR,ch,0);
        Pump();
        const std::wstring typed=L"可再次编辑的文字\r\n第二行，也能实时看见";
        expect(app.edit_&&app.state_.text_edit.mark&&app.state_.text_edit.mark->text==typed&&app.state_.text_edit.caret==int(typed.size()),"typed Chinese text and Shift+Enter update the live canvas text without committing");
        const Mark live=app.state_.text_edit.mark?*app.state_.text_edit.mark:Mark{};
        expect(live.b.y-live.a.y>2*live.font_size&&live.a==Point{140,150},"live text grows to two lines from the click point");
        const auto end_caret=MeasureTextEdit(live,int(typed.size()),int(typed.size()),int(typed.size()));
        GetGUIThreadInfo(GetCurrentThreadId(),&gui);POINT system_caret{gui.rcCaret.left,gui.rcCaret.top};ClientToScreen(gui.hwndCaret?gui.hwndCaret:app.edit_,&system_caret);
        std::cout<<"Painted caret: "<<end_caret.caret.left<<","<<end_caret.caret.top<<" system caret: "<<system_caret.x<<","<<system_caret.y<<'\n';
        expect(gui.hwndCaret==app.edit_&&std::abs(float(system_caret.x)-end_caret.caret.left)<=1.5f&&std::abs(float(system_caret.y)-end_caret.caret.top)<=1.5f,"system caret follows the painted caret so IME candidates open beside it");
        for(int i=0;i<4;++i){v.renderer->FrameReady();InvalidateRect(v.window,nullptr,FALSE);UpdateWindow(v.window);Pump();}
        GetGUIThreadInfo(GetCurrentThreadId(),&gui);expect(gui.hwndFocus==app.edit_,"canvas repaints preserve text focus");
        const auto canvas=Canvas(app);
        size_t ink=0;for(LONG y=LONG(live.a.y);y<LONG(std::ceil(live.b.y));++y)for(LONG x=LONG(live.a.x);x<LONG(std::ceil(live.b.x));++x)if(NoteLuminance(canvas.pixels[size_t(y-canvas.bounds.top)*canvas.Width()+x-canvas.bounds.left])<.5)++ink;
        expect(ink>100,"canvas paints the live text in its final colour and font");
        expect(app.document_.marks.empty(),"live preview is not an annotation until committed");
        SavePng(Crop(canvas,{100,110,560,260}),L"build/text-inplace-typing.png");
        SendMessageW(app.edit_,EM_SETSEL,0,3);
        expect(app.state_.text_edit.selection_start==0&&app.state_.text_edit.selection_end==3,"native selection is mirrored on the canvas");
        {const auto selected=Canvas(app);const auto parts=MeasureTextEdit(live,0,3,3).selection;size_t tinted=0;
         for(const auto& part:parts)for(LONG y=LONG(part.top)+1;y<LONG(part.bottom)-1;++y)for(LONG x=LONG(part.left)+1;x<LONG(part.right)-1;++x){const auto c=selected.pixels[size_t(y-selected.bounds.top)*selected.Width()+x-selected.bounds.left];if(int(c&255)>int((c>>16)&255)+25)++tinted;}
         expect(!parts.empty()&&tinted>20,"selection has a visible blue wash over the final glyphs");SavePng(Crop(selected,{100,110,560,260}),L"build/text-inplace-selection.png");}
        SendMessageW(app.edit_,EM_SETSEL,3,1);DWORD reverse_first{},reverse_last{};
        SendMessageW(app.edit_,EM_GETSEL,reinterpret_cast<WPARAM>(&reverse_first),reinterpret_cast<LPARAM>(&reverse_last));
        expect(reverse_first==1&&reverse_last==3&&app.state_.text_edit.caret==1,"reverse selection keeps its active end at the leading position");
        Key(app.edit_,VK_LEFT);SendMessageW(app.edit_,EM_GETSEL,reinterpret_cast<WPARAM>(&reverse_first),reinterpret_cast<LPARAM>(&reverse_last));
        expect(reverse_first==1&&reverse_last==1&&app.state_.text_edit.caret==1,"Left collapses a reverse selection at the leading caret");
        const auto at=[&](int index){if(!app.state_.text_edit.mark)return Point{};const auto g=MeasureTextEdit(*app.state_.text_edit.mark,index,index,index);return Point{g.caret.left+.5f,(g.caret.top+g.caret.bottom)/2};};
        reverse_first=reverse_last=99;app.PointerDown(v,at(2));app.PointerUp(v,at(2));if(app.edit_)SendMessageW(app.edit_,EM_GETSEL,reinterpret_cast<WPARAM>(&reverse_first),reinterpret_cast<LPARAM>(&reverse_last));
        expect(app.edit_&&reverse_first==2&&reverse_last==2&&GetFocus()==app.edit_,"clicking inside the text box moves the caret and keeps editing");
        if(app.edit_){app.PointerDown(v,at(0));app.PointerMove(v,at(4),MK_LBUTTON);app.PointerUp(v,at(4));SendMessageW(app.edit_,EM_GETSEL,reinterpret_cast<WPARAM>(&reverse_first),reinterpret_cast<LPARAM>(&reverse_last));}
        expect(app.edit_&&reverse_first==0&&reverse_last==4,"dragging inside the text box selects characters");
        if(app.edit_){app.PointerDoubleClick(v,at(2));SendMessageW(app.edit_,EM_GETSEL,reinterpret_cast<WPARAM>(&reverse_first),reinterpret_cast<LPARAM>(&reverse_last));}
        expect(app.edit_&&reverse_first==0&&reverse_last==8,"double click selects the run of characters under the pointer");
        if(app.edit_)expect(app.TextEditHit(at(2))&&!app.TextEditHit({700,450}),"I-beam hit area covers only the edit box");
        if(app.edit_){SendMessageW(app.edit_,EM_SETSEL,static_cast<WPARAM>(-1),-1);Pump();}
        const Mark preview_before_commit=app.state_.text_edit.mark?*app.state_.text_edit.mark:Mark{};
        Key(app.edit_,VK_RETURN);expect(!app.edit_&&!app.edit_host_&&!app.state_.text_edit.mark&&app.document_.marks.size()==1&&app.document_.marks[0].text.find(L"\r\n")!=std::wstring::npos,"Enter commits multiline content and removes the live preview");
        expect(app.document_.marks.size()==1&&app.document_.marks[0]==preview_before_commit,"committed annotation is exactly what was shown while typing");
        expect(app.document_.marks[0].a==Point{140,150}&&app.document_.marks[0].b.x<440&&app.document_.marks[0].text_auto_size,"new text bounds fit its content without including editor padding or the return button");
        expect(app.document_.marks[0].text_background==TextBackground::None,"temporary editing background is excluded from the annotation");
        app.document_.marks[0].rotation=25;app.document_.marks[0].text_auto_size=false;const auto original=app.document_.marks[0];app.BeginText(v,original.a,0);Pump();
        DWORD start{},end{};SendMessageW(app.edit_,EM_GETSEL,reinterpret_cast<WPARAM>(&start),reinterpret_cast<LPARAM>(&end));
        expect(start==0&&end==original.text.size(),"reopening a text object selects its existing content");
        SetWindowTextW(app.edit_,L"取消的文字");Key(app.edit_,VK_ESCAPE);expect(app.document_.marks[0]==original&&!app.edit_,"Escape cancels edits and preserves rotated object geometry and style");
        app.BeginText(v,original.a,0);SetWindowTextW(app.edit_,L"继续编辑");Key(app.edit_,VK_RETURN);
        expect(app.document_.marks[0].text==L"继续编辑"&&app.document_.marks[0].a==original.a&&app.document_.marks[0].b==original.b&&app.document_.marks[0].rotation==25,"reediting commits content without resetting rotation or dimensions");
        app.BeginText(v,original.a,0);SetWindowTextW(app.edit_,L"");Key(app.edit_,VK_RETURN);expect(app.document_.marks.empty(),"committing an emptied text object removes it");
        app.document_.Undo();expect(app.document_.marks.size()==1&&app.document_.marks[0].text==L"继续编辑","undo restores cleared text");
        for(bool dark:{false,true})for(uint32_t color:{0xff000000u,0xffffffffu,0xffffff00u,0xffff574fu}){
            state.dark=dark;state.color=color;state.toolbar.scale=1.5f;app.BeginText(v,{150,150});if(app.edit_)SendMessageW(app.edit_,WM_CHAR,L'A',0);
            expect(app.state_.text_edit.mark&&app.state_.text_edit.mark->color==color&&app.edit_color_==color,"live text uses the exact annotation colour on the screenshot, not an input-panel tint");app.CommitText(true);
        }
        state.color=0xff243142;app.BeginText(v,{150,150});
        SendMessageW(app.edit_,WM_IME_STARTCOMPOSITION,0,0);Key(app.edit_,VK_ESCAPE);expect(app.edit_!=nullptr,"IME composition consumes Escape without cancelling the text object");
        app.edit_ime_text_=L"中文预编辑";app.edit_ime_first_=app.edit_ime_last_=0;app.edit_ime_cursor_=2;app.UpdateTextPreview();
        expect(app.state_.text_edit.mark&&app.state_.text_edit.mark->text==L"中文预编辑"&&app.state_.text_edit.composition_start==0&&app.state_.text_edit.composition_length==5&&app.state_.text_edit.caret==2&&GetWindowTextLengthW(app.edit_)==0,"IME preedit is painted inline with its own caret and underline, without committing native text");
        SavePng(Crop(Canvas(app),{100,110,560,260}),L"build/text-inplace-ime.png");
        {
            const LPARAM all=static_cast<LPARAM>(ISC_SHOWUIALL),inplace=Application::TextImeContext(all,true);
            expect(!(inplace&static_cast<LPARAM>(ISC_SHOWUICOMPOSITIONWINDOW))&&(inplace&static_cast<LPARAM>(ISC_SHOWUICANDIDATEWINDOW))&&Application::TextImeContext(all,false)==all,"in-place text hides the IME's own preedit window (no doubled string) but keeps candidates");
        }
        SendMessageW(app.edit_,WM_IME_ENDCOMPOSITION,0,0);SetWindowTextW(app.edit_,L"失焦提交");SetFocus(v.window);Pump();
        expect(!app.edit_&&app.document_.marks.back().text==L"失焦提交","focus loss commits text after the native notification completes");
        app.document_.Reset();Mark label;label.tool=Tool::Number;label.number_combo=NumberCombo::Text;label.a={150,150};label.b={182,182};label.number_target={440,210};label.number_text_preset=-2;label.text=L"原说明";app.document_.Add(label);
        const auto appearance=ResolveNoteAppearance(*app.frame_,label);app.BeginText(v,label.a,0);
        expect(app.state_.text_edit.mark&&app.state_.text_edit.mark->tool==Tool::Number&&app.state_.text_edit.mark->text==L"原说明","number note description is edited in place beside its badge");
        expect(app.edit_color_==appearance.ink&&!app.edit_bold_,"number note editing uses its resolved ink and the same regular weight as rendering");
        SetWindowTextW(app.edit_,L"修改说明");Key(app.edit_,VK_RETURN);
        expect(app.document_.marks[0].number_text_preset==-2&&app.document_.marks[0].text==L"修改说明","editing a badge label preserves automatic color mode");
        for(bool dark:{false,true}){
            app.document_.Reset();state.dark=dark;state.toolbar.scale=1.5f;state.color=0xffff574f;state.font_size=24;app.BeginText(v,{150,150});SetWindowTextW(app.edit_,L"水电费是的啊");const int length=GetWindowTextLengthW(app.edit_);SendMessageW(app.edit_,EM_SETSEL,length,length);Pump();
            SavePng(Crop(Canvas(app),{100,100,620,260}),dark?L"docs/text-editor-dark-preview.png":L"docs/text-editor-preview.png");
            {Document other;Mark box;box.a={120,215};box.b={300,240};other.Add(box);other.selected=0;const auto saved=app.document_;app.document_=other;
             const auto with_selected=Canvas(app);app.document_.selected=-1;const auto without=Canvas(app);app.document_=saved;
             expect(with_selected.pixels==without.pixels,"selection handles of other objects are hidden while typing");}
            const size_t count=app.document_.marks.size();
            app.PointerDown(v,{700,300});app.PointerUp(v,{700,300});
            expect(!app.edit_&&app.document_.marks.size()==count+1&&app.document_.marks.back().text==L"水电费是的啊"&&app.document_.marks.back().color==state.color,"clicking outside the box finishes the text, keeps its exact colour and starts no second text");
        }
        app.BeginText(v,{750,450});
        expect(app.edit_&&app.state_.text_edit.mark&&app.state_.text_edit.mark->a==Point{750,450},"text near the selection edge opens in place at the requested anchor");
        SetWindowTextW(app.edit_,L"边缘");Key(app.edit_,VK_RETURN);
        expect(app.document_.marks.back().a==Point{750,450}&&app.document_.marks.back().b.x<850,"repositioning the editor at a monitor edge preserves the requested annotation anchor and fits its content");
        state.dark=false;state.toolbar.scale=1.5f;app.document_.Reset();app.BeginText(v,{150,150});SetWindowTextW(app.edit_,L"地方没错");Key(app.edit_,VK_RETURN);
        auto short_text=app.document_.marks.back();const float short_width=short_text.b.x-short_text.a.x;
        expect(short_width>100&&short_width<160&&short_text.b.y-short_text.a.y<60,"four Chinese characters at 150% use a tight content box instead of the 450px input field");
        state.tool=Tool::Select;app.document_.selected=0;
        const auto handle=EditHandles(short_text,EditPart::Whole,state.toolbar.scale)[3].point;
        app.PointerDown(v,handle);app.PointerMove(v,{handle.x+short_width*.5f,handle.y},MK_LBUTTON);app.PointerUp(v,{handle.x+short_width*.5f,handle.y});
        expect(std::abs(app.document_.marks[0].font_size-short_text.font_size*1.5f)<.01f,"actual pointer dragging a text side handle enlarges its font without Shift");
        app.document_.Undo();expect(app.document_.marks[0]==short_text,"undo restores text dimensions and font size together");app.document_.Redo();expect(app.document_.marks[0].font_size>short_text.font_size,"redo restores enlarged glyphs");app.document_.Undo();state.tool=Tool::Text;
        {
            Mark sample=short_text;Translate(sample,{55-sample.a.x,65-sample.a.y});Document document;document.marks.push_back(sample);
            Renderer renderer;auto image=renderer.Flatten(MakeFrame({0,0,400,170},0xffffffff),document,{0,0,400,170});DibSurface bitmap(image.Width(),image.Height());std::copy(image.pixels.begin(),image.pixels.end(),bitmap.Pixels());
            ComPtr<ID2D1Factory> factory;D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,factory.GetAddressOf());ComPtr<ID2D1DCRenderTarget> target;
            const auto properties=D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),96,96);
            CheckWin32(SUCCEEDED(factory->CreateDCRenderTarget(&properties,&target)),"Bounds preview target");RECT rect{0,0,image.Width(),image.Height()};target->BindDC(bitmap.Dc(),&rect);target->BeginDraw();DrawSelectionEditor(target.Get(),sample,EditPart::Whole,1);CheckWin32(SUCCEEDED(target->EndDraw()),"Bounds preview");
            std::copy_n(bitmap.Pixels(),image.pixels.size(),image.pixels.begin());SavePng(image,L"docs/text-bounds-preview.png");
            Document sizes;for(float factor:{.65f,1.f,1.6f}){auto scaled=EditMark(sample,EditPart::Whole,3,{sample.b.x,(sample.a.y+sample.b.y)/2},{sample.a.x+(sample.b.x-sample.a.x)*factor,(sample.a.y+sample.b.y)/2},false,false);const float x=sizes.marks.empty()?30.f:sizes.marks.size()==1?170.f:370.f;Translate(scaled,{x-scaled.a.x,70-scaled.a.y});sizes.marks.push_back(scaled);}
            SavePng(renderer.Flatten(MakeFrame({0,0,640,180},0xffffffff),sizes,{0,0,640,180}),L"docs/text-scale-preview.png");
        }
        app.BeginText(v,short_text.a,0);SetWindowTextW(app.edit_,L"地方没错，文字变长");Key(app.edit_,VK_RETURN);expect(app.document_.marks[0].b.x-app.document_.marks[0].a.x>short_width,"automatic bounds grow when the text is edited to a longer phrase");
        app.BeginText(v,short_text.a,0);SetWindowTextW(app.edit_,L"短字");Key(app.edit_,VK_RETURN);expect(app.document_.marks[0].b.x-app.document_.marks[0].a.x<short_width,"automatic bounds shrink again when characters are removed");
        auto fixed=app.document_.marks[0];fixed=EditMark(fixed,EditPart::Whole,3,fixed.b,{fixed.b.x+100,fixed.b.y},false,false);app.document_.marks[0]=fixed;
        Renderer refresh_renderer;const auto before_edit=refresh_renderer.Flatten(*app.frame_,app.document_,app.frame_->bounds);
        app.BeginText(v,fixed.a,0);SetWindowTextW(app.edit_,L"新");Key(app.edit_,VK_RETURN);
        expect(app.document_.marks[0].text_auto_size&&app.document_.marks[0].text==L"新"&&app.document_.marks[0].font_size==fixed.font_size&&app.document_.marks[0].b.x-app.document_.marks[0].a.x<fixed.b.x-fixed.a.x,"confirming edited scaled text replaces its content and refits bounds while retaining enlarged font size");
        const auto after_edit=refresh_renderer.Flatten(*app.frame_,app.document_,app.frame_->bounds);Renderer fresh_renderer;
        expect(before_edit.pixels!=after_edit.pixels&&after_edit.pixels==fresh_renderer.Flatten(*app.frame_,app.document_,app.frame_->bounds).pixels,"reused text renderer paints the new scaled text without stale cached content");
        app.document_.Undo();expect(app.document_.marks[0]==fixed,"undo restores the prior scaled text and bounds together");app.document_.Redo();
        const auto current=app.document_.marks[0];state.tool=Tool::Text;
        app.PointerDown(v,{current.a.x+4,current.a.y+8});app.PointerUp(v,{current.a.x+4,current.a.y+8});
        expect(!app.edit_&&state.tool==Tool::Select&&app.document_.selected==0,"single click on existing text activates selection rather than creating or reopening text");
        app.PointerDoubleClick(v,{current.a.x+4,current.a.y+8});expect(app.edit_&&app.edit_index_==0,"double click reopens the directly selected text object");
        if(app.edit_){SendMessageW(app.edit_,EM_SETSEL,0,-1);SendMessageW(app.edit_,EM_REPLACESEL,TRUE,reinterpret_cast<LPARAM>(L"再改"));Key(app.edit_,VK_RETURN);}
        expect(app.document_.marks.size()==1&&app.document_.marks[0].text==L"再改","repeated pointer editing replaces the same text object without leaving the old content underneath");
        if(app.document_.marks.size()==1){
            const auto committed=app.document_.marks[0];const auto refreshed=refresh_renderer.Flatten(*app.frame_,app.document_,app.frame_->bounds);
            expect(refreshed.pixels!=after_edit.pixels&&refreshed.pixels==fresh_renderer.Flatten(*app.frame_,app.document_,app.frame_->bounds).pixels,"pointer-confirmed content is present in newly rendered pixels");
            SavePng(Crop(refreshed,{100,60,520,290}),L"docs/scaled-text-edit-preview.png");
            app.PointerDoubleClick(v,{committed.a.x+4,committed.a.y+8});SetWindowTextW(app.edit_,L"不保存");Key(app.edit_,VK_ESCAPE);
            expect(app.document_.marks.size()==1&&app.document_.marks[0]==committed,"cancelling a repeated pointer edit preserves the original scaled object without duplicates");
        }
        if(app.document_.marks.size()>1)app.document_.marks.resize(1);
        fixed=app.document_.marks[0];fixed.text_auto_size=false;app.document_.marks[0]=fixed;app.document_.selected=0;
        state.toolbar=PlaceToolbar(state.selection,bounds,1,1,Tool::Text);
        OpenToolbarDropdown(state,80,bounds);app.ApplyToolbarDropdown(static_cast<int>(TextFonts().size())-1);
        expect(app.document_.marks[0].font_family==TextFonts().back().family&&app.document_.marks[0].b==fixed.b,"font dropdown changes the active text while respecting a manually sized box");
        app.BeginText(v,fixed.a,0);LOGFONTW native{};GetObjectW(app.edit_font_,sizeof(native),&native);
        expect(std::wstring(native.lfFaceName)==TextFonts().back().family,"native text editing uses the selected family");app.CommitText(true);
        app.document_.Reset();state.font_family=TextFonts().back().family;app.BeginText(v,{150,150});SetWindowTextW(app.edit_,L"iiiiWWWW");Key(app.edit_,VK_RETURN);
        expect(app.document_.marks[0].font_family==state.font_family&&app.document_.marks[0].text_auto_size,"new text retains its chosen font with automatic sizing");
        Renderer font_renderer;const auto selected_font=font_renderer.Flatten(*app.frame_,app.document_,app.frame_->bounds);
        if(TextFonts().size()>1){app.document_.marks[0].font_family=TextFonts().front().family;FitTextBounds(app.document_.marks[0]);const auto different_font=font_renderer.Flatten(*app.frame_,app.document_,app.frame_->bounds);expect(selected_font.pixels!=different_font.pixels,"exported pixels and font cache follow font family changes");}
        Renderer toolbar_renderer;const auto toolbar_preview=toolbar_renderer.Demo(true,false,Tool::Text,false,-1,80);SavePng(Crop(toolbar_preview,{240,360,1125,740}),L"docs/text-font-picker-preview.png");
        TagEditorCases(app,v,expect);
        return failures?1:0;
    }
};
}
int main(int argc,char** argv){SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_STANDARD_CLASSES};InitCommonControlsEx(&controls);int result=lumashot::TextEditTest::Run(argc==2&&std::string_view(argv[1])=="--number-label");CoUninitialize();return result;}