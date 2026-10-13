#include "ocr/service.h"
#include "ocr/config.h"
#include "ui/render.h"
#include <iostream>
#include <chrono>
#include <algorithm>
#include <tlhelp32.h>
#include <fstream>
using namespace lumashot;
int main(int argc,char** argv) {
    const HRESULT com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    // Every return must destroy the renderer and worker before COM teardown.
    struct ComCleanup {HRESULT result;~ComCleanup(){if(SUCCEEDED(result))CoUninitialize();}} com_cleanup{com};
    int failures=0;
    try {
        auto frame=MakeFrame({0,0,1920,1080},0xffffffff);Document document;
        for(int i=0;i<20;++i){Mark m;m.tool=Tool::Text;m.a={24,float(12+i*48)};m.b={1000,m.a.y+36};m.font_size=22;m.text=L"LumaShot 中英文字选择复制 "+std::to_wstring(i);m.color=0xff202733;document.Add(m);}
        Renderer renderer;auto image=std::make_shared<Frame>(renderer.Flatten(frame,document,frame.bounds));std::vector<double> cold;
        if(argc>1&&std::string(argv[1])=="--custom-models") {
            wchar_t module[32768]{};GetModuleFileNameW(nullptr,module,32768);
            const auto base=std::filesystem::path(module).parent_path();
            const auto custom=base/(L"ocr 自定义 test "+std::to_wstring(GetCurrentProcessId()));
            std::filesystem::create_directory(custom);
            struct Cleanup {std::filesystem::path path;~Cleanup(){std::error_code ec;for(const auto* name:{L"det.onnx",L"rec.onnx",L"dictionary.txt"})std::filesystem::remove(path/name,ec);std::filesystem::remove(path,ec);}} cleanup{custom};
            for(const auto* name:{L"det.onnx",L"rec.onnx",L"dictionary.txt"})std::filesystem::copy_file(base/L"ocr"/name,custom/name);
            ocr::Service configured(nullptr,WM_APP+20);
            auto check=[&](uint64_t version,bool success,const char* label) {
                configured.Submit(1,version,image);bool ok=false;const auto limit=GetTickCount64()+15000;
                while(GetTickCount64()<limit){auto results=configured.Take();if(!results.empty()){ok=results.front().version==version&&(success?(results.front().error.empty()&&!results.front().text.lines.empty()):!results.front().error.empty());break;}Sleep(5);}
                std::cout<<(ok?"PASS ":"FAIL ")<<label<<std::endl;if(!ok)++failures;
            };
            configured.SetModelDirectory(custom);check(1,true,"custom model directory with Unicode and spaces");
            configured.SetModelDirectory(custom/L"missing");check(2,false,"missing custom directory reports a recoverable error");
            std::filesystem::copy_file(custom/L"det.onnx",custom/L"rec.onnx",std::filesystem::copy_options::overwrite_existing);
            configured.SetModelDirectory(custom);check(3,false,"incompatible recognition tensor layout is rejected");
            configured.SetModelDirectory(custom/L"missing");check(4,false,"switch away before replacing model files");
            std::filesystem::copy_file(base/L"ocr"/L"rec.onnx",custom/L"rec.onnx",std::filesystem::copy_options::overwrite_existing);
            // A different dictionary must fail even though all three files exist.
            {std::ofstream dictionary(custom/L"dictionary.txt",std::ios::binary|std::ios::trunc);dictionary<<"A\n";}
            configured.SetModelDirectory(custom);check(5,false,"mismatched dictionary is rejected");
            configured.SetModelDirectory({});check(6,true,"switching back to bundled models restarts worker");
            return failures?1:0;
        }
        for(int run=0;run<10;++run){const auto t=std::chrono::steady_clock::now();ocr::Service service(nullptr,WM_APP+20);service.Submit(1,1,image);
            const auto deadline=GetTickCount64()+10000;bool complete=false;
            while(GetTickCount64()<deadline){auto results=service.Take();if(!results.empty()){complete=results.front().error.empty()&&!results.front().text.lines.empty();if(!complete)std::wcerr<<results.front().error<<'\n';break;}Sleep(5);}
            if(!complete)++failures;
            const auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t).count();cold.push_back(ms);std::cout<<"cold_ipc_ms="<<ms<<std::endl;
        }
        std::sort(cold.begin(),cold.end());std::cout<<"cold_1080p_20lines_P95_ms="<<cold.back()<<std::endl;
        std::cout<<(failures?"FAIL":"PASS")<<" repeated cold launches with real model and shared memory"<<std::endl;
        // Canceled queued results must never be observable, even with reused ids.
        ocr::Service service(nullptr,WM_APP+20);service.Submit(7,1,image);service.Cancel(7);service.Submit(7,2,image);
        const auto deadline=GetTickCount64()+10000;bool latest=false;
        while(GetTickCount64()<deadline){for(auto& r:service.Take()){if(r.version!=2)++failures;latest=r.version==2&&r.error.empty();}if(latest)break;Sleep(5);}
        std::cout<<(latest?"PASS":"FAIL")<<" canceled version never replaces latest"<<std::endl;if(!latest)++failures;
        service.Submit(7,3,image);
        {ocr::Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0));PROCESSENTRY32W entry{sizeof(entry)};
            if(Process32FirstW(snapshot.Get(),&entry))do{if(entry.th32ParentProcessID==GetCurrentProcessId()&&std::wstring(entry.szExeFile)==L"lumashot_ocr_worker.exe") {
                ocr::Handle process(OpenProcess(PROCESS_TERMINATE,FALSE,entry.th32ProcessID));if(process.Get())TerminateProcess(process.Get(),99);
            }}while(Process32NextW(snapshot.Get(),&entry));}
        bool failed=false;const auto crash_deadline=GetTickCount64()+5000;
        while(GetTickCount64()<crash_deadline){auto results=service.Take();if(!results.empty()){failed=!results.front().error.empty();break;}Sleep(5);}
        std::cout<<(failed?"PASS":"FAIL")<<" worker crash becomes a recoverable error"<<std::endl;if(!failed)++failures;
        service.Submit(7,4,image);bool recovered=false;const auto retry_deadline=GetTickCount64()+10000;
        while(GetTickCount64()<retry_deadline){auto results=service.Take();if(!results.empty()){recovered=results.front().error.empty()&&results.front().version==4;break;}Sleep(5);}
        std::cout<<(recovered?"PASS":"FAIL")<<" retry starts a new worker after crash"<<std::endl;if(!recovered)++failures;
        // Prewarm starts the worker and loads models before the first request.
        const auto workers=[]{int n=0;ocr::Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0));PROCESSENTRY32W entry{sizeof(entry)};
            if(Process32FirstW(snapshot.Get(),&entry))do{if(entry.th32ParentProcessID==GetCurrentProcessId()&&std::wstring(entry.szExeFile)==L"lumashot_ocr_worker.exe")++n;}while(Process32NextW(snapshot.Get(),&entry));return n;};
        {
            const int before=workers();ocr::Service warm(nullptr,WM_APP+20);warm.Prewarm();warm.Prewarm();
            bool started=false;const auto start_deadline=GetTickCount64()+5000;
            while(GetTickCount64()<start_deadline){if(workers()==before+1){started=true;break;}Sleep(5);}
            const bool quiet=started&&warm.Take().empty();
            std::cout<<(quiet?"PASS":"FAIL")<<" prewarm starts one worker without producing a result"<<std::endl;if(!quiet)++failures;
            Sleep(1500);const auto t=std::chrono::steady_clock::now();warm.Submit(9,1,image);bool warmed=false;const auto warm_deadline=GetTickCount64()+10000;
            while(GetTickCount64()<warm_deadline){auto results=warm.Take();if(!results.empty()){warmed=results.front().id==9&&results.front().error.empty()&&!results.front().text.lines.empty();break;}Sleep(5);}
            std::cout<<"prewarmed_first_1080p_ms="<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t).count()<<std::endl;
            std::cout<<(warmed&&workers()==before+1?"PASS":"FAIL")<<" first request reuses the prewarmed worker"<<std::endl;if(!warmed||workers()!=before+1)++failures;
        }
        {
            wchar_t module[32768]{};GetModuleFileNameW(nullptr,module,32768);
            ocr::Service broken(nullptr,WM_APP+20);broken.SetModelDirectory(std::filesystem::path(module).parent_path()/L"ocr-missing-prewarm");broken.Prewarm();Sleep(300);
            broken.Submit(3,1,image);bool reported=false;const auto broken_deadline=GetTickCount64()+10000;
            while(GetTickCount64()<broken_deadline){auto results=broken.Take();if(!results.empty()){reported=!results.front().error.empty();break;}Sleep(5);}
            std::cout<<(reported?"PASS":"FAIL")<<" prewarm with missing models still reports a recoverable error"<<std::endl;if(!reported)++failures;
        }
    }catch(const std::exception& e){std::cout<<"FAIL "<<e.what()<<std::endl;++failures;}
    return failures?1:0;
}