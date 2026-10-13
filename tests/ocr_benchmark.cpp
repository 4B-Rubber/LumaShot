#include "ocr/engine.h"
#include "ui/render.h"
#include "export/png.h"
#include "ocr/service.h"
#include <psapi.h>
#include <iostream>
#include <fstream>
#include <chrono>
#include <numeric>
#include <algorithm>
#include <atomic>
#include <thread>
#include <map>
#include <string>
using namespace lumashot;
static std::wstring Normalize(std::wstring s){std::erase_if(s,[](wchar_t c){return c==L'\r'||c==L'\n';});return s;}
static size_t Distance(const std::wstring& a,const std::wstring& b){std::vector<size_t> row(b.size()+1);std::iota(row.begin(),row.end(),size_t(0));for(size_t i=0;i<a.size();++i){size_t prev=row[0];row[0]=i+1;for(size_t j=0;j<b.size();++j){size_t old=row[j+1];row[j+1]=std::min({row[j]+1,row[j+1]+1,prev+(a[i]!=b[j]?1u:0u)});prev=old;}}return row.back();}
static Frame Fixture(int index,int lines,std::wstring& expected,bool full=false) {
    const bool dark=index%2!=0;auto frame=MakeFrame({0,0,full?1920:760,full?1080:240},dark?0xff202733:0xffffffff);
    Document document;const float size=full?22.0f:float(16+(index%5)*2);
    const std::wstring samples[]={L"截图文字可以直接选择复制",L"LumaShot offline text selection",L"版本 Version 2026",L"文件名称 Screenshot.png",L"精度与资源占用均衡",L"Hello Windows 1234567890",L"保存图片并继续工作",L"Copy text with Ctrl+C"};
    for(int i=0;i<lines;++i){Mark m;m.tool=Tool::Text;m.a={24,float(12+i*(full?48:52))};m.b={float(frame.Width()-20),m.a.y+size+12};m.font_size=size;m.color=dark?0xfff2f4f8:0xff202733;m.text=samples[(index+i)%8];if(i==0)m.text+=L" "+std::to_wstring(1000+index);document.Add(m);expected+=m.text;}
    Renderer renderer;return renderer.Flatten(frame,document,frame.bounds);
}
static Frame DenseFixture() {
    auto frame=MakeFrame({0,0,1920,1080},0xfff7f8fa);Document document;
    const std::wstring labels[]={L"复制",L"保存",L"设置",L"撤销",L"完成",L"关闭",L"编辑",L"文本",L"文件",L"图片",L"选择",L"导出",L"Copy",L"Save",L"Open",L"Help"};
    for(int column=0;column<4;++column)for(int row=0;row<16;++row){Mark m;m.tool=Tool::Text;m.a={float(30+column*460),float(40+row*60)};m.b={m.a.x+400,m.a.y+40};m.font_size=float(18+column*2);m.color=0xff202733;m.text=labels[(column+row)%16];document.Add(m);}
    Renderer renderer;return renderer.Flatten(frame,document,frame.bounds);
}
// Benchmarks the OCR engine in process and the worker round trip users wait
// for. The same source builds against older engines without tuning options.
using Clock=std::chrono::steady_clock;
static double Ms(Clock::time_point start){return std::chrono::duration<double,std::milli>(Clock::now()-start).count();}
static double Percentile(std::vector<double> v,double p){if(v.empty())return 0;std::sort(v.begin(),v.end());return v[static_cast<size_t>(double(v.size()-1)*p)];}
static size_t DenseFound(const std::wstring& got) {
    const std::wstring labels[]={L"复制",L"保存",L"设置",L"撤销",L"完成",L"关闭",L"编辑",L"文本",L"文件",L"图片",L"选择",L"导出",L"Copy",L"Save",L"Open",L"Help"};
    size_t found=0;
    for(const auto& label:labels){size_t n=0;for(size_t p=got.find(label);p!=std::wstring::npos;p=got.find(label,p+label.size()))++n;found+=std::min<size_t>(n,4);}
    return found;
}
// Round trip through ocr::Service and lumashot_ocr_worker.exe next to this exe.
static double ServiceRoundTrip(ocr::Service& service,uint64_t id,const std::shared_ptr<const Frame>& frame,std::wstring& error) {
    const auto start=Clock::now();service.Submit(id,1,frame);
    while(Ms(start)<60000) {
        for(auto& c:service.Take())if(c.id==id){error=c.error;return Ms(start);}
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    error=L"timeout";return Ms(start);
}
int main(int argc,char** argv) {
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);int code=0;
    std::atomic_size_t peak_private{};
    std::jthread sampler([&](std::stop_token stop){while(!stop.stop_requested()){PROCESS_MEMORY_COUNTERS_EX m{};m.cb=sizeof(m);if(GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&m),sizeof(m))){auto old=peak_private.load();while(m.PrivateUsage>old&&!peak_private.compare_exchange_weak(old,m.PrivateUsage)){}}std::this_thread::sleep_for(std::chrono::milliseconds(5));}});
    try {
        const auto folder=std::filesystem::absolute(argv[0]).parent_path();
        const int count=argc>1?std::stoi(argv[1]):100;
        const int threads=argc>2?std::stoi(argv[2]):0;
        const bool service_only=argc>3&&std::string(argv[3])=="service";
        const auto dense=DenseFixture();
        if(!service_only) {
        SavePng(dense,folder/L"ocr-dense-ui.png");
        auto start=Clock::now();
#ifdef LUMASHOT_OCR_ENGINE_OPTIONS
        ocr::EngineOptions options;options.threads=threads;
        ocr::Engine engine(folder/L"ocr",options);
        std::cout<<"threads="<<engine.Threads()<<std::endl;
#else
        (void)threads;ocr::Engine engine(folder/L"ocr");std::cout<<"threads=legacy"<<std::endl;
#endif
        std::cout<<"model_load_ms="<<Ms(start)<<std::endl;
#ifdef LUMASHOT_OCR_ENGINE_OPTIONS
        start=Clock::now();engine.Warmup();std::cout<<"warmup_ms="<<Ms(start)<<std::endl;
#endif
        {std::wstring expected;auto frame=Fixture(0,4,expected);start=Clock::now();engine.Recognize(frame);std::cout<<"first_recognize_ms="<<Ms(start)<<std::endl;}
        size_t errors=0,characters=0;std::vector<double> times;
        for(int i=0;i<count;++i){std::wstring expected;auto frame=Fixture(i,4,expected);const auto t=Clock::now();const auto result=engine.Recognize(frame);times.push_back(Ms(t));const auto got=Normalize(ocr::Selected(result,ocr::All(result)));errors+=Distance(expected,got);characters+=expected.size();
            if(i<2)SavePng(frame,folder/(i?L"ocr-fixture-dark.png":L"ocr-fixture-light.png"));
            if(i%25==24)std::cout<<"completed="<<i+1<<" CER="<<double(errors)/double(characters)<<std::endl;
        }
        std::cout<<"screenshots="<<count<<" characters="<<characters<<" errors="<<errors<<" CER="<<double(errors)/double(characters)<<" fixture_P50_ms="<<Percentile(times,0.5)<<" fixture_P95_ms="<<Percentile(times,0.95)<<std::endl;
        std::vector<double> full;size_t full_errors=0,full_characters=0;ocr::Timings sum{};
        for(int i=0;i<10;++i){std::wstring expected;auto frame=Fixture(i,20,expected,true);ocr::Timings stages;const auto t=Clock::now();const auto result=engine.Recognize(frame,&stages);full.push_back(Ms(t));
            sum.preprocess_ms+=stages.preprocess_ms;sum.detection_ms+=stages.detection_ms;sum.recognition_ms+=stages.recognition_ms;sum.decode_ms+=stages.decode_ms;
            const auto got=Normalize(ocr::Selected(result,ocr::All(result)));full_errors+=Distance(expected,got);full_characters+=expected.size();}
        std::cout<<"stages_avg preprocess="<<sum.preprocess_ms/10<<" detect="<<sum.detection_ms/10<<" rec="<<sum.recognition_ms/10<<" decode="<<sum.decode_ms/10<<std::endl;
        std::cout<<"1080p_20lines_P50_ms="<<Percentile(full,0.5)<<" 1080p_20lines_P95_ms="<<Percentile(full,0.95)<<" 1080p_CER="<<double(full_errors)/double(full_characters)<<std::endl;
        {const auto cpu=[]{FILETIME c,e,k,u;GetProcessTimes(GetCurrentProcess(),&c,&e,&k,&u);return (ULARGE_INTEGER{{k.dwLowDateTime,k.dwHighDateTime}}.QuadPart+ULARGE_INTEGER{{u.dwLowDateTime,u.dwHighDateTime}}.QuadPart)/10000.0;};
            std::wstring expected;auto frame=Fixture(1,20,expected,true);engine.Recognize(frame);
            const double before=cpu();std::this_thread::sleep_for(std::chrono::seconds(1));
            std::cout<<"idle_cpu_ms_after_request_1s="<<cpu()-before<<std::endl;}
        std::vector<double> dense_times;size_t dense_found=0;
        for(int i=0;i<5;++i){const auto t=Clock::now();const auto result=engine.Recognize(dense);dense_times.push_back(Ms(t));if(i==0)dense_found=DenseFound(Normalize(ocr::Selected(result,ocr::All(result))));}
        std::cout<<"dense_ui_64_P50_ms="<<Percentile(dense_times,0.5)<<" dense_found="<<dense_found<<"/64"<<std::endl;
        PROCESS_MEMORY_COUNTERS_EX memory{};memory.cb=sizeof(memory);GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),sizeof(memory));
        std::cout<<"peak_working_set_MB="<<double(memory.PeakWorkingSetSize)/1048576<<" private_MB="<<double(memory.PrivateUsage)/1048576<<" sampled_peak_private_MB="<<double(peak_private.load())/1048576<<std::endl;
        if(double(errors)/double(characters)>0.02||double(full_errors)/double(full_characters)>0.02)code=2;
        }
        // Worker round trips: cold (spawn+load+first run), then warm repeats.
        std::wstring expected;const auto frame=std::make_shared<const Frame>(Fixture(3,4,expected));
        const auto big=std::make_shared<const Frame>(Fixture(4,20,expected,true));
        for(int round=0;round<2;++round) {
            ocr::Service service(nullptr,0);service.SetModelDirectory(folder/L"ocr");std::wstring error;
#ifdef LUMASHOT_OCR_SERVICE_PREWARM
            if(round==1){service.Prewarm();std::this_thread::sleep_for(std::chrono::milliseconds(700));}
#else
            if(round==1){std::cout<<"service_prewarm=unsupported"<<std::endl;break;}
#endif
            const double cold=ServiceRoundTrip(service,1,frame,error);
            if(!error.empty()){std::wcout<<L"service error: "<<error<<std::endl;code=3;break;}
            std::vector<double> warm,warm_big;
            for(int i=0;i<5;++i)warm.push_back(ServiceRoundTrip(service,10+i,frame,error));
            for(int i=0;i<3;++i)warm_big.push_back(ServiceRoundTrip(service,20+i,big,error));
            std::cout<<(round?"service_prewarmed_first_ms=":"service_cold_first_ms=")<<cold<<" service_warm_P50_ms="<<Percentile(warm,0.5)<<" service_warm_1080p_P50_ms="<<Percentile(warm_big,0.5)<<std::endl;
        }
    }catch(const std::exception& e){std::cout<<e.what()<<std::endl;code=1;}
    CoUninitialize();return code;
}
