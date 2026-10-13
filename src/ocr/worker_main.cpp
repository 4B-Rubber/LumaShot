#include "ocr/engine.h"
#include "ocr/protocol.h"
#include "ocr/table.h"
#include <objbase.h>
#include <memory>
int wmain(int argc,wchar_t** argv) {
    using namespace lumashot;using namespace lumashot::ocr;
    const auto com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    wchar_t path[32768]{};GetModuleFileNameW(nullptr,path,32768);
    const bool arguments_valid=argc==1||(argc==3&&std::wstring(argv[1])==L"--model-directory"&&argv[2][0]);
    const auto model_directory=argc==3?std::filesystem::path(argv[2]):std::filesystem::path(path).parent_path()/L"ocr";
    std::unique_ptr<Engine> engine;Request request;
    // Load and warm the models before the first request arrives; the service
    // may start this process while the user is still selecting a region.
    // A failure here is reported by the lazy load on the first request.
    if(arguments_valid)try{engine=std::make_unique<Engine>(model_directory);engine->Warmup();}catch(const std::exception&){engine.reset();}
    while(ReadExact(GetStdHandle(STD_INPUT_HANDLE),&request,sizeof(request))) {
        Text text;std::wstring error;
        try {
            Handle mapping(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(request.mapping)));
            if(!arguments_valid)throw std::runtime_error("Invalid OCR worker model-directory argument");
            if(request.magic!=kProtocol||request.reserved!=0||!request.width||!request.height||request.width>32768||request.height>32768||
                uint64_t(request.width)*request.height>128*1024*1024)throw std::runtime_error("Invalid OCR request");
            Frame frame=MakeFrame({0,0,static_cast<LONG>(request.width),static_cast<LONG>(request.height)});
            {MappingView view(mapping.Get(),FILE_MAP_READ,frame.pixels.size()*4);std::memcpy(frame.pixels.data(),view.data,frame.pixels.size()*4);}
            mapping.Reset();
            if(!engine)engine=std::make_unique<Engine>(model_directory);
            text=engine->Recognize(frame);
            text.table=AnalyzeTable(frame,text);
        }catch(const std::exception& e){error=ErrorMessage(e);}
        try {auto bytes=Encode(text,error);const auto size=static_cast<uint32_t>(bytes.data.size());
            if(!WriteExact(GetStdHandle(STD_OUTPUT_HANDLE),&size,sizeof(size))||!WriteExact(GetStdHandle(STD_OUTPUT_HANDLE),bytes.data.data(),size))break;
        }catch(...){break;}
    }
    engine.reset();if(SUCCEEDED(com))CoUninitialize();return 0;
}

