#include "app/application.h"
#include "model/text_cleanup.h"
#include <iostream>

namespace lumashot {
struct QuickCaptureTest {
    static int Run(){
        int failures=0;
        const auto expect=[&](bool ok,const char* label){std::cout<<(ok?"PASS ":"FAIL ")<<label<<'\n';failures+=!ok;};
        Application app;app.diagnostic_session_=true;app.toolbar_effects_=false;
        app.main_=CreateWindowExW(0,L"STATIC",L"Synthetic quick capture",WS_POPUP,0,0,1,1,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        std::wstring copied;int writes=0;
        app.publish_ocr_text_=[&](HWND,const std::wstring& text){copied=text;++writes;};
        const auto prepare=[&]{app.active_=true;app.state_={};app.state_.selected=true;app.state_.selection={0,0,180,100};app.state_.busy=true;app.quick_capture_=Application::QuickCapture::Text;app.quick_ocr_pending_=true;++app.generation_;};
        const auto completion=[&]{ocr::Completion c;c.id=1;c.version=app.generation_;ocr::Line line;line.box={0,0,100,20};line.glyphs.push_back({L"A B\r\n中\u3000文",{0,0,100,20}});c.text.lines.push_back(line);return c;};
        prepare();auto stale=completion();--stale.version;app.ApplyOcrCompletion(std::move(stale));
        expect(writes==0&&app.quick_ocr_pending_&&app.state_.busy,"stale OCR cannot publish or unlock a newer session");
        app.quick_remove_whitespace_=false;app.ApplyOcrCompletion(completion());
        expect(writes==1&&copied==L"A B\r\n中\u3000文"&&!app.active_,"quick OCR preserves formatting and closes capture after successful copy");
        prepare();app.quick_remove_whitespace_=true;app.ApplyOcrCompletion(completion());
        expect(writes==2&&copied==L"AB中文"&&!app.active_,"optional Unicode whitespace cleanup occurs before copy");
        prepare();auto late=completion();app.Cancel(false);app.ApplyOcrCompletion(std::move(late));
        expect(writes==2&&!app.active_,"canceled OCR never writes clipboard");
        prepare();auto failed=completion();failed.error=L"Synthetic unavailable model";app.ApplyOcrCompletion(std::move(failed));
        expect(writes==2&&app.active_&&!app.state_.busy&&app.state_.hint.find(L"Enter")!=std::wstring::npos,"OCR failure retains selection for retry without overwriting clipboard");
        prepare();auto empty=completion();empty.text={};app.ApplyOcrCompletion(std::move(empty));
        expect(writes==2&&app.active_&&!app.state_.busy,"empty recognition retains selection and clipboard");
        prepare();app.publish_ocr_text_=[](HWND,const std::wstring&){throw std::runtime_error("Synthetic clipboard busy");};app.ApplyOcrCompletion(completion());
        expect(app.active_&&!app.state_.busy&&!app.quick_ocr_pending_,"clipboard failure does not close capture and permits retry");
        app.state_.tool=Tool::Rectangle;app.state_.width=2;app.state_.toolbar.bounds={300,200,600,250};
        app.Wheel({50,50},1);expect(app.state_.width==3,"wheel on canvas adjusts rectangle stroke");
        app.Wheel({-20,-20},3);expect(app.state_.width==3,"wheel outside selection leaves stroke unchanged");
        app.Wheel({50,50},-100);expect(app.state_.width==1,"stroke width clamps to minimum");
        app.state_.busy=true;app.Wheel({50,50},4);expect(app.state_.width==1,"busy capture ignores wheel changes");app.state_.busy=false;
        app.state_.tool=Tool::Ellipse;app.Wheel({50,50},3);expect(app.state_.width==4,"ellipse canvas stroke can be adjusted");
        app.state_.tool=Tool::Text;const float font=app.state_.font_size;app.Wheel({50,50},4);expect(app.state_.font_size==font,"canvas stroke shortcut does not alter text size");
        app.state_.tool=Tool::Select;
        app.monitors_={{0,0,1280,900}};app.state_.closing_tool=Tool::Rectangle;app.toolbar_transition_.Reset(0);
        app.UpdateToolbar({50,50});
        expect(app.state_.toolbar.bounds.right-app.state_.toolbar.bounds.left<=640,"finished property collapse restores compact toolbar width");
        app.frame_=std::make_shared<Frame>(MakeFrame({0,0,180,100},0xffeeeeee));
        app.quick_capture_=Application::QuickCapture::Text;app.quick_ocr_pending_=false;
        expect(app.FinishQuickSelection()&&app.pending_,"text selection automatically starts background export");
        app.worker_.join();
        expect(app.result_&&app.result_->quick_text&&app.result_->error.empty()&&!app.result_->clipboard_image&&app.result_->ocr_frame.Width()==180,"quick OCR prepares pixels without publishing an image or temporary file");
        app.result_.reset();app.pending_=false;app.state_.busy=false;
        app.quick_capture_=Application::QuickCapture::Pin;
        expect(app.FinishQuickSelection()&&app.pending_,"pin selection automatically starts background export");app.worker_.join();
        expect(app.result_&&app.result_->pinned&&!app.result_->recognize&&app.result_->error.empty()&&!app.result_->clipboard_image,"pin shortcut creates a pin without OCR or replacing clipboard");
        app.result_.reset();app.pending_=false;app.state_.busy=false;
        Application::View view;view.app=&app;view.bounds={0,0,180,100};app.Key(view,VK_RETURN);app.worker_.join();
        expect(app.result_&&app.result_->pinned&&!app.result_->clipboard_image,"Enter retry retains quick pin intent instead of copying an image");
        app.result_.reset();app.pending_=false;app.Cancel(false);
        DestroyWindow(app.main_);app.main_=nullptr;
        return failures?1:0;
    }
};
}
int main(){CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);int result=lumashot::QuickCaptureTest::Run();CoUninitialize();return result;}
