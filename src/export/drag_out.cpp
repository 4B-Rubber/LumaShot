#include "export/drag_out.h"
#include "export/clipboard.h"
#include <shlobj.h>
#include <ole2.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <climits>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <future>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace lumashot {
namespace {
using Microsoft::WRL::ComPtr;

struct Payload {
    std::shared_ptr<ClipboardFile> file;
    std::vector<BYTE> dib,png;
    bool ok{};
};

// "LumaShot-20261013-163015-042.png": readable where the target shows the
// file name (Explorer, chat attachments), still matched by the 24 h cleanup.
void GiveReadableName(ClipboardFile& file){
    SYSTEMTIME t{};GetLocalTime(&t);
    wchar_t stamp[48]{};
    swprintf_s(stamp,L"LumaShot-%04u%02u%02u-%02u%02u%02u-%03u",unsigned(t.wYear),unsigned(t.wMonth),unsigned(t.wDay),unsigned(t.wHour),unsigned(t.wMinute),unsigned(t.wSecond),unsigned(t.wMilliseconds));
    const auto extension=file.path.extension().wstring();
    for(int n=0;n<20;++n){
        auto target=file.path.parent_path()/(std::wstring(stamp)+(n?L"-"+std::to_wstring(n):L"")+extension);
        if(MoveFileExW(file.path.c_str(),target.c_str(),0)){file.path=std::move(target);return;}
        if(GetLastError()!=ERROR_ALREADY_EXISTS&&GetLastError()!=ERROR_FILE_EXISTS)return;
    }
}

Payload Encode(const std::function<std::shared_ptr<const Frame>()>& produce,int format){
    Payload payload;
    const HRESULT com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    try{
        const auto image=produce?produce():nullptr;
        if(image&&image->Width()>0&&image->Height()>0){
            payload.file=PrepareClipboardFile(*image,format);
            GiveReadableName(*payload.file);
            const auto prepared=PrepareClipboardImage(*image);
            if(const auto* bytes=static_cast<const BYTE*>(GlobalLock(prepared->image))){
                payload.dib.assign(bytes,bytes+GlobalSize(prepared->image));GlobalUnlock(prepared->image);
            }
            if(format==0){
                std::ifstream in(payload.file->path,std::ios::binary);
                payload.png.assign(std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>());
            }
            payload.ok=true;
        }
    }catch(...){payload={};}
    if(SUCCEEDED(com))CoUninitialize();
    return payload;
}

HGLOBAL Copy(const void* data,size_t size){
    const HGLOBAL memory=GlobalAlloc(GMEM_MOVEABLE,size?size:1);if(!memory)return nullptr;
    if(void* target=GlobalLock(memory)){if(size)memcpy(target,data,size);GlobalUnlock(memory);return memory;}
    GlobalFree(memory);return nullptr;
}
HGLOBAL DropFiles(const std::filesystem::path& path){
    const auto text=path.wstring();
    const HGLOBAL memory=GlobalAlloc(GMEM_MOVEABLE|GMEM_ZEROINIT,sizeof(DROPFILES)+(text.size()+2)*sizeof(wchar_t));if(!memory)return nullptr;
    auto* files=static_cast<DROPFILES*>(GlobalLock(memory));if(!files){GlobalFree(memory);return nullptr;}
    files->pFiles=sizeof(DROPFILES);files->fWide=TRUE;
    std::copy(text.begin(),text.end(),reinterpret_cast<wchar_t*>(files+1));
    GlobalUnlock(memory);return memory;
}

class DataObject final:public IDataObject {
public:
    DataObject(std::shared_future<Payload> payload,bool png):payload_(std::move(payload)){
        formats_.push_back({CLIPFORMAT(CF_HDROP),nullptr,DVASPECT_CONTENT,-1,TYMED_HGLOBAL});
        formats_.push_back({CLIPFORMAT(CF_DIB),nullptr,DVASPECT_CONTENT,-1,TYMED_HGLOBAL});
        if(png)formats_.push_back({png_format_,nullptr,DVASPECT_CONTENT,-1,TYMED_HGLOBAL});
    }
    ~DataObject(){for(auto& entry:extra_)GlobalFree(entry.second);}
    // A target accepted the drop: keep the file for it (the 24 h cleanup removes it later).
    void Publish(){
        try{if(payload_.wait_for(std::chrono::seconds(10))==std::future_status::ready&&payload_.get().file)payload_.get().file->published=true;}catch(...){}
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out)override{
        if(!out)return E_POINTER;
        if(id==IID_IUnknown||id==IID_IDataObject){*out=static_cast<IDataObject*>(this);AddRef();return S_OK;}
        *out=nullptr;return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef()override{return ++refs_;}
    ULONG STDMETHODCALLTYPE Release()override{const ULONG left=--refs_;if(!left)delete this;return left;}

    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* format,STGMEDIUM* medium)override{
        if(!format||!medium)return E_INVALIDARG;
        *medium={};
        if(!(format->tymed&TYMED_HGLOBAL)||format->dwAspect!=DVASPECT_CONTENT)return DV_E_FORMATETC;
        HGLOBAL memory{};
        if(const auto it=extra_.find(format->cfFormat);it!=extra_.end()){
            memory=Copy(GlobalLock(it->second),GlobalSize(it->second));GlobalUnlock(it->second);
        }else if(Offers(format->cfFormat)){
            const Payload* ready{};
            try{ready=&payload_.get();}catch(...){return DV_E_FORMATETC;}  // waits for the worker if it is still encoding
            const Payload& payload=*ready;
            if(!payload.ok)return DV_E_FORMATETC;
            if(format->cfFormat==CF_HDROP)memory=DropFiles(payload.file->path);
            else if(format->cfFormat==CF_DIB)memory=Copy(payload.dib.data(),payload.dib.size());
            else if(!payload.png.empty())memory=Copy(payload.png.data(),payload.png.size());
            else return DV_E_FORMATETC;
        }else return DV_E_FORMATETC;
        if(!memory)return E_OUTOFMEMORY;
        medium->tymed=TYMED_HGLOBAL;medium->hGlobal=memory;medium->pUnkForRelease=nullptr;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*,STGMEDIUM*)override{return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* format)override{
        if(!format)return E_INVALIDARG;
        if(!(format->tymed&TYMED_HGLOBAL)||format->dwAspect!=DVASPECT_CONTENT)return DV_E_FORMATETC;
        return Offers(format->cfFormat)||extra_.contains(format->cfFormat)?S_OK:DV_E_FORMATETC;
    }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*,FORMATETC* out)override{if(out)out->ptd=nullptr;return DATA_S_SAMEFORMATETC;}
    // The shell and drop targets attach their own small blobs (drop
    // description, performed effect...); keep HGLOBAL ones so they can read them back.
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC* format,STGMEDIUM* medium,BOOL release)override{
        if(!format||!medium)return E_INVALIDARG;
        if(medium->tymed!=TYMED_HGLOBAL||!medium->hGlobal||Offers(format->cfFormat))return E_NOTIMPL;
        const HGLOBAL memory=Copy(GlobalLock(medium->hGlobal),GlobalSize(medium->hGlobal));GlobalUnlock(medium->hGlobal);
        if(!memory)return E_OUTOFMEMORY;
        if(auto it=extra_.find(format->cfFormat);it!=extra_.end()){GlobalFree(it->second);it->second=memory;}
        else extra_.emplace(format->cfFormat,memory);
        if(release)ReleaseStgMedium(medium);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction,IEnumFORMATETC** out)override{
        if(!out)return E_POINTER;
        if(direction!=DATADIR_GET){*out=nullptr;return E_NOTIMPL;}
        return SHCreateStdEnumFmtEtc(UINT(formats_.size()),formats_.data(),out);
    }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*,DWORD,IAdviseSink*,DWORD*)override{return OLE_E_ADVISENOTSUPPORTED;}
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD)override{return OLE_E_ADVISENOTSUPPORTED;}
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**)override{return OLE_E_ADVISENOTSUPPORTED;}
private:
    bool Offers(CLIPFORMAT format)const{for(const auto& f:formats_)if(f.cfFormat==format)return true;return false;}
    std::atomic<ULONG> refs_{1};
    std::shared_future<Payload> payload_;
    const CLIPFORMAT png_format_{CLIPFORMAT(RegisterClipboardFormatW(L"PNG"))};
    std::vector<FORMATETC> formats_;
    std::map<CLIPFORMAT,HGLOBAL> extra_;
};

class DropSource final:public IDropSource {
public:
    explicit DropSource(std::function<void(POINT,bool)> moved):moved_(std::move(moved)){}
    bool Escaped()const{return escaped_;}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out)override{
        if(!out)return E_POINTER;
        if(id==IID_IUnknown||id==IID_IDropSource){*out=static_cast<IDropSource*>(this);AddRef();return S_OK;}
        *out=nullptr;return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef()override{return ++refs_;}
    ULONG STDMETHODCALLTYPE Release()override{const ULONG left=--refs_;if(!left)delete this;return left;}
    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escape,DWORD keys)override{
        if(escape){escaped_=true;return DRAGDROP_S_CANCEL;}
        if(!(keys&MK_LBUTTON))return DRAGDROP_S_DROP;
        Follow();return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD effect)override{
        over_target_=effect!=DROPEFFECT_NONE;Follow(true);
        if(over_target_)return DRAGDROP_S_USEDEFAULTCURSORS;
        // Nothing here takes the picture: releasing keeps the normal drop
        // (place the pin, or hang it on the rope), so no "forbidden" cursor.
        SetCursor(LoadCursorW(nullptr,IDC_ARROW));return S_OK;
    }
private:
    void Follow(bool force=false){
        POINT cursor{};GetCursorPos(&cursor);
        if(!force&&cursor.x==last_.x&&cursor.y==last_.y&&reported_==over_target_)return;
        last_=cursor;reported_=over_target_;
        if(moved_)moved_(cursor,over_target_);
    }
    std::atomic<ULONG> refs_{1};
    std::function<void(POINT,bool)> moved_;
    POINT last_{LONG_MIN,LONG_MIN};bool over_target_{},reported_{},escaped_{};
};
}

DragOutResult DragImageOut(DragOutOptions options){
    // DoDragDrop needs OLE on this thread (main only initializes COM).
    static const HRESULT ole=OleInitialize(nullptr);
    if(FAILED(ole)||!options.image)return DragOutResult::Failed;
    const int format=options.file_format>=0&&options.file_format<=2?options.file_format:0;
    // Encode while the user is still moving; a detached thread so that a
    // canceled drag never waits for it (the last owner deletes the unused file).
    std::promise<Payload> promise;auto payload=promise.get_future().share();
    try{
        std::thread([promise=std::move(promise),produce=std::move(options.image),format]()mutable{
            try{promise.set_value(Encode(produce,format));}catch(...){}
        }).detach();
    }catch(...){return DragOutResult::Failed;}
    ComPtr<DataObject> data;data.Attach(new DataObject(payload,format==0));
    ComPtr<DropSource> source;source.Attach(new DropSource(std::move(options.moved)));
    DWORD effect=DROPEFFECT_NONE;
    const HRESULT result=DoDragDrop(data.Get(),source.Get(),DROPEFFECT_COPY,&effect);
    if(result==DRAGDROP_S_DROP&&effect!=DROPEFFECT_NONE){data->Publish();return DragOutResult::Dropped;}
    if(FAILED(result))return DragOutResult::Failed;
    return source->Escaped()?DragOutResult::Canceled:DragOutResult::NoTarget;
}
}
