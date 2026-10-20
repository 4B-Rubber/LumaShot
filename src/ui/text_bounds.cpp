#include "ui/text_renderer.h"
#include "model/selection.h"
#include "model/tag_appearance.h"
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
namespace lumashot {
void FitTextBounds(Mark& mark){
    if(mark.tool!=Tool::Text||!mark.text_auto_size||mark.text.empty())return;
    Microsoft::WRL::ComPtr<IDWriteFactory> factory;
    CheckWin32(SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(factory.GetAddressOf()))),"Text measurement factory");
    Microsoft::WRL::ComPtr<IDWriteTextFormat> format;
    CheckWin32(SUCCEEDED(factory->CreateTextFormat(mark.font_family.c_str(),nullptr,mark.text_bold?DWRITE_FONT_WEIGHT_BOLD:DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,mark.font_size,L"zh-CN",&format)),"Text measurement font");
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    const auto box=Normalize(mark.a,mark.b);const auto padding=IsTextTag(mark.text_background)?TextTagMetrics(mark.font_size):TagMetrics{};
    const float limit=std::max(1.f,mark.text_wrap_width>0?mark.text_wrap_width:box.right-box.left-2*padding.x);
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    CheckWin32(SUCCEEDED(factory->CreateTextLayout(mark.text.data(),static_cast<UINT32>(mark.text.size()),format.Get(),limit,1000000,&layout)),"Measure annotation text");
    DWRITE_TEXT_METRICS metrics{};CheckWin32(SUCCEEDED(layout->GetMetrics(&metrics)),"Read annotation text size");
    // Round outward and leave a subpixel guard so the last glyph never wraps
    // just because the measured layout was tightened to its content width.
    const float width=std::min(limit,std::max(1.f,std::ceil(metrics.widthIncludingTrailingWhitespace)+1));
    layout->SetMaxWidth(width);CheckWin32(SUCCEEDED(layout->GetMetrics(&metrics)),"Measure fitted text height");
    const float height=std::max(1.f,std::ceil(metrics.height)+1)+2*padding.y;
    const float outer_width=width+2*padding.x;
    const float alignment=mark.text_align==TextAlign::Center?.5f:mark.text_align==TextAlign::Right?1.f:0.f;
    const Point anchor{box.left+(box.right-box.left)*alignment,box.top};
    const auto world=RotatePoint(anchor,MarkCenter(mark),mark.rotation);
    mark.a={anchor.x-outer_width*alignment,box.top};mark.b={mark.a.x+outer_width,mark.a.y+height};
    const auto fitted=RotatePoint(anchor,MarkCenter(mark),mark.rotation);
    Translate(mark,{world.x-fitted.x,world.y-fitted.y});
}

namespace {
using Microsoft::WRL::ComPtr;
struct EditLayout {ComPtr<IDWriteTextLayout> layout;Box content{};std::wstring text;};
EditLayout CreateEditLayout(const Mark& mark){
    EditLayout result;
    const bool note=mark.tool==Tool::Number;
    // Mirrors Renderer::NumberDetail and the Text MarkShape case.
    const float size=std::max(1.f,note?mark.number_text_size:mark.font_size);
    const std::wstring family=note?std::wstring(L"Segoe UI"):mark.font_family;
    const bool bold=!note&&mark.text_bold;
    const TextAlign align=note?TextAlign::Left:mark.text_align;
    result.content=note?TagContentBounds(NumberDetailBounds(mark),TextTagMetrics(mark.number_text_size)):TextContentBounds(mark);
    result.text=mark.text==L"\u200b"?std::wstring{}:mark.text;
    ComPtr<IDWriteFactory> factory;
    CheckWin32(SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(factory.GetAddressOf()))),"Text editing factory");
    ComPtr<IDWriteTextFormat> format;
    CheckWin32(SUCCEEDED(factory->CreateTextFormat(family.c_str(),nullptr,bold?DWRITE_FONT_WEIGHT_BOLD:DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,size,L"zh-CN",&format)),"Text editing font");
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    format->SetTextAlignment(align==TextAlign::Center?DWRITE_TEXT_ALIGNMENT_CENTER:(align==TextAlign::Right?DWRITE_TEXT_ALIGNMENT_TRAILING:DWRITE_TEXT_ALIGNMENT_LEADING));
    const float width=std::max(1.f,result.content.right-result.content.left),height=std::max(1.f,result.content.bottom-result.content.top);
    CheckWin32(SUCCEEDED(factory->CreateTextLayout(result.text.data(),static_cast<UINT32>(result.text.size()),format.Get(),width,height,&result.layout)),"Text editing layout");
    return result;
}
std::vector<Box> Range(const EditLayout& edit,int first,int last){
    std::vector<Box> boxes;
    const int size=static_cast<int>(edit.text.size());first=std::clamp(first,0,size);last=std::clamp(last,0,size);
    if(first>last)std::swap(first,last);
    if(first==last)return boxes;
    UINT32 count{};edit.layout->HitTestTextRange(static_cast<UINT32>(first),static_cast<UINT32>(last-first),edit.content.left,edit.content.top,nullptr,0,&count);
    std::vector<DWRITE_HIT_TEST_METRICS> parts(count);
    if(count&&SUCCEEDED(edit.layout->HitTestTextRange(static_cast<UINT32>(first),static_cast<UINT32>(last-first),edit.content.left,edit.content.top,parts.data(),count,&count))){
        parts.resize(count);
        for(const auto& part:parts)boxes.push_back({part.left,part.top,part.left+part.width,part.top+part.height});
    }
    return boxes;
}
}
TextEditGeometry MeasureTextEdit(const Mark& mark,int selection_start,int selection_end,int caret,int composition_start,int composition_length){
    TextEditGeometry geometry;const auto edit=CreateEditLayout(mark);geometry.content=edit.content;
    const int size=static_cast<int>(edit.text.size());
    FLOAT x{},y{};DWRITE_HIT_TEST_METRICS metrics{};
    CheckWin32(SUCCEEDED(edit.layout->HitTestTextPosition(static_cast<UINT32>(std::clamp(caret,0,size)),FALSE,&x,&y,&metrics)),"Text editing caret");
    geometry.caret={edit.content.left+x,edit.content.top+metrics.top,edit.content.left+x,edit.content.top+metrics.top+metrics.height};
    geometry.selection=Range(edit,selection_start,selection_end);
    if(composition_start>=0&&composition_length>0)geometry.composition=Range(edit,composition_start,composition_start+composition_length);
    if(mark.tool==Tool::Number)geometry.frame=NumberDetailBounds(mark);
    else {
        geometry.frame=Normalize(mark.a,mark.b);
        // An empty automatic box collapses to the insertion point.
        geometry.frame.left=std::min(geometry.frame.left,geometry.caret.left);geometry.frame.right=std::max(geometry.frame.right,geometry.caret.right);
        geometry.frame.top=std::min(geometry.frame.top,geometry.caret.top);geometry.frame.bottom=std::max(geometry.frame.bottom,geometry.caret.bottom);
    }
    return geometry;
}
int TextEditIndex(const Mark& mark,Point point){
    const auto edit=CreateEditLayout(mark);
    const auto local=RotatePoint(point,MarkCenter(mark),-mark.rotation);
    BOOL trailing{},inside{};DWRITE_HIT_TEST_METRICS metrics{};
    CheckWin32(SUCCEEDED(edit.layout->HitTestPoint(local.x-edit.content.left,local.y-edit.content.top,&trailing,&inside,&metrics)),"Text editing hit test");
    const int size=static_cast<int>(edit.text.size());
    int index=std::clamp(static_cast<int>(metrics.textPosition)+(trailing?static_cast<int>(metrics.length):0),0,size);
    // Clicking past the end of a line lands after its CR/LF; keep it on the line.
    if(trailing&&index>0&&(edit.text[size_t(index)-1]==L'\n'||edit.text[size_t(index)-1]==L'\r')){
        index=static_cast<int>(metrics.textPosition);
    }
    if(index>0&&index<size&&edit.text[size_t(index)-1]==L'\r'&&edit.text[size_t(index)]==L'\n')--index;
    return index;
}

}
