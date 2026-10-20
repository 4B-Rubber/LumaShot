template<class Expect> static void TagEditorCases(Application& app,Application::View& view,Expect expect){
    app.document_.Reset();auto& state=app.state_;state.tool=Tool::Text;state.toolbar.scale=1;state.font_family=L"Segoe UI";state.font_size=24;state.text_align=TextAlign::Left;state.text_background=TextBackground::TagAutomatic;state.color=AnnotationColors[4];
    for(int y=0;y<app.frame_->Height();++y)for(int x=0;x<app.frame_->Width();++x)app.frame_->pixels[size_t(y)*app.frame_->Width()+x]=((x/12+y/12)%2)?0xffe8eef4:0xffc6d2df;
    app.BeginText(view,{150,150});expect(app.edit_&&app.state_.text_edit.mark&&app.state_.text_edit.mark->text_background==TextBackground::TagAutomatic,"tag input previews the final tag style in place");
    if(!app.edit_)return;
    SetWindowTextW(app.edit_,L"Tag 输入预览\r\n第二行");Pump();SendMessageW(app.edit_,EM_SETSEL,static_cast<WPARAM>(-1),-1);
    {
        const auto live=*app.state_.text_edit.mark;const auto canvas=Canvas(app);const auto box=Normalize(live.a,live.b);size_t changed=0,total=0;
        for(LONG y=LONG(box.top)+2;y<LONG(box.bottom)-2;++y)for(LONG x=LONG(box.left)+2;x<LONG(box.right)-2;++x){
            const size_t i=size_t(y-canvas.bounds.top)*canvas.Width()+x-canvas.bounds.left;++total;
            if((canvas.pixels[i]&0xffffff)!=(app.frame_->pixels[i]&0xffffff))++changed;
        }
        expect(live.text==L"Tag 输入预览\r\n第二行"&&total>0&&changed*10>total*8,"tag background and text are painted live on the screenshot");
        std::filesystem::create_directories(L"build/input-theme-preview");SavePng(Crop(canvas,{100,110,560,300}),L"build/input-theme-preview/tag-inplace.png");
    }
    SendMessageW(app.edit_,WM_IME_STARTCOMPOSITION,0,0);Key(app.edit_,VK_ESCAPE);expect(app.edit_!=nullptr,"tag preview preserves native IME composition and Escape handling");
    SendMessageW(app.edit_,WM_IME_ENDCOMPOSITION,0,0);
    Key(app.edit_,VK_RETURN);
    expect(app.document_.marks.size()==1&&app.document_.marks[0].a==Point{150,150},"new tag commits at its original outer anchor, without double padding");
    const auto saved=app.document_.marks[0];expect(saved.text_background==TextBackground::TagAutomatic&&saved.color==AnnotationColors[4]&&(*ResolveTextAppearance(*app.frame_,saved).background>>24)<255,"theme input leaves committed Tag hue and translucent output unchanged");
    app.BeginText(view,saved.a,0);SetWindowTextW(app.edit_,L"取消");Key(app.edit_,VK_ESCAPE);expect(app.document_.marks[0]==saved,"Escape preserves tag layout and colors");
    app.BeginText(view,saved.a,0);Key(app.edit_,VK_RETURN);expect(app.document_.marks[0]==saved,"reopening a tag does not accumulate padding or change its hue");
    app.BeginText(view,saved.a,0);SetWindowTextW(app.edit_,L"短");Key(app.edit_,VK_RETURN);expect(app.document_.marks[0].b.x<saved.b.x&&app.document_.marks[0].color==saved.color,"editing shorter text tightens the tag without changing the stored hue");
    state.toolbar.scale=1.5f;app.BeginText(view,{150,260});SetWindowTextW(app.edit_,L"150% Tag");Pump();expect(app.edit_size_==36&&app.state_.text_edit.mark&&app.state_.text_edit.mark->font_size==36,"high DPI input previews scaled metrics");
    SavePng(Crop(Canvas(app),{100,200,560,360}),L"build/input-theme-preview/light.png");app.CommitText(true);
    expect(!app.edit_&&!app.edit_backdrop_&&!app.state_.text_edit.mark,"native editing resources and the live preview are released on cancel");
    for(bool dark:{false,true})for(auto mode:{TextBackground::None,TextBackground::ToneLight,TextBackground::ToneDark,TextBackground::Automatic,TextBackground::TagAutomatic,TextBackground::TagLight,TextBackground::TagDark}){
        state.dark=dark;state.text_background=mode;app.BeginText(view,{150,260});
        expect(app.edit_&&app.state_.text_edit.mark&&app.state_.text_edit.mark->text_background==mode,"every text background mode is previewed in its final style");
        if(app.edit_){SetWindowTextW(app.edit_,L"150% Tag");Pump();if(dark&&mode==TextBackground::TagAutomatic)SavePng(Crop(Canvas(app),{100,200,560,360}),L"build/input-theme-preview/dark.png");app.CommitText(true);}
    }
    Mark note;note.tool=Tool::Number;note.number_combo=NumberCombo::Text;note.a={150,150};note.b={182,182};note.number_target={450,230};note.text=L"说明";app.document_.Reset();app.document_.Add(note);
    for(bool dark:{false,true}){state.dark=dark;app.BeginText(view,note.a,0);expect(app.edit_&&app.state_.text_edit.mark&&app.state_.text_edit.mark->tool==Tool::Number&&app.state_.text_edit.mark->text==L"说明","number note input is previewed on its own badge");app.CommitText(true);}
    // Reproduce dragging the placeholder under a custom caption, then double-click editing it.
    app.document_.marks[0].number_label=L"看这里";app.document_.marks[0].text.clear();state.tool=Tool::Select;
    const auto grouped=app.document_.marks[0];const auto detail=NumberDetailBounds(grouped);
    const Point grip{(detail.left+detail.right)/2,(detail.top+detail.bottom)/2},destination{grip.x+35,grip.y+25};
    app.PointerDown(view,grip);app.PointerMove(view,destination,MK_LBUTTON);app.PointerUp(view,destination);
    auto expected=grouped;Translate(expected,{35,25});expect(app.document_.marks[0]==expected,"dragging placeholder moves its custom caption too");
    app.PointerDoubleClick(view,destination);
    expect(app.edit_&&app.edit_index_==0&&!app.edit_badge_label_,"double click moved note still opens description editor, not caption editor");
    if(app.edit_){SetWindowTextW(app.edit_,L"组合移动后编辑说明");Key(app.edit_,VK_RETURN);}
    expect(app.document_.marks[0].text==L"组合移动后编辑说明"&&app.document_.marks[0].number_label==L"看这里"&&app.document_.marks[0].a==expected.a&&app.document_.marks[0].b==expected.b,"editing note after group movement preserves caption and badge position");
}