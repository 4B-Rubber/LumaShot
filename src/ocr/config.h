#pragma once
#include <filesystem>
#include <string>

namespace lumashot::ocr {
// An empty directory selects the bundled models. Loading and tensor validation
// remain in the isolated worker; settings only check the selected files.
inline std::wstring ModelDirectoryError(const std::filesystem::path& directory) {
    if(directory.empty())return {};
    std::error_code error;
    if(!std::filesystem::is_directory(directory,error))return L"OCR 模型目录不存在或无法访问。";
    for(const auto* name:{L"det.onnx",L"rec.onnx",L"dictionary.txt"}) {
        const auto file=directory/name;
        if(!std::filesystem::is_regular_file(file,error))return std::wstring(L"OCR 模型目录缺少文件：")+name;
        const auto size=std::filesystem::file_size(file,error);
        if(error||size==0)return std::wstring(L"OCR 模型文件为空或无法读取：")+name;
    }
    return {};
}
// Windows CRT argument quoting, including trailing backslashes and Unicode.
inline std::wstring QuoteModelArgument(const std::wstring& value) {
    std::wstring result=L"\"";size_t slashes=0;
    for(const auto ch:value) {
        if(ch==L'\\'){++slashes;continue;}
        result.append(ch==L'"'?slashes*2+1:slashes,L'\\');slashes=0;result+=ch;
    }
    result.append(slashes*2,L'\\');result+=L'"';return result;
}
}
