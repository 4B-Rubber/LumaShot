#pragma once
// History entry -> card stack item (tiles of strip-concepts.html .th-*, plan
// section 3.4). Pure and header-only; the panel supplies the cached thumbnail.
#include <windows.h>
#include <cstdio>
#include <cwctype>
#include <iterator>
#include <optional>
#include <string>
#include "capture/frame.h"
#include "clipboard/entry_flavor.h"
#include "clipboard/history.h"
#include "clipboard/stack_window.h"

namespace lumashot::clipboard {

// Domain-initial avatar colour: stable per host, from the group palette.
inline uint32_t HostColor(const std::wstring& host) {
    uint32_t h = 2166136261u;
    for (wchar_t c : host) h = (h ^ static_cast<uint32_t>(c)) * 16777619u;
    return History::Palette[h % std::size(History::Palette)];
}
inline std::wstring ClockLabel(const SYSTEMTIME& t) {
    wchar_t text[16]{};
    swprintf_s(text, L"%02u:%02u", t.wHour, t.wMinute);
    return text;
}

inline StackItem MakeStackItem(const Entry& e, const Frame* thumb, const std::wstring& first_file = {}, uint32_t file_count = 0) {
    StackItem it;
    it.id = e.id;
    const std::wstring when = ClockLabel(e.time);
    switch (e.kind) {
    case Kind::Image:
        it.tile = StackTile::Image;
        it.title = L"图片";
        it.subtitle = L"图片 · " + when;
        if (thumb && thumb->Width() > 0 && thumb->Height() > 0 &&
            thumb->pixels.size() >= static_cast<size_t>(thumb->Width()) * static_cast<size_t>(thumb->Height())) {
            it.title = L"图片 " + std::to_wstring(thumb->Width()) + L"×" + std::to_wstring(thumb->Height());
            it.thumb_width = thumb->Width();
            it.thumb_height = thumb->Height();
            it.thumb = thumb->pixels;
        }
        break;
    case Kind::Files: {
        it.tile = StackTile::File;
        const std::wstring name = first_file.empty() ? FirstLine(e.text) : first_file;
        const auto slash = name.find_last_of(L"\\/");
        it.title = slash == std::wstring::npos ? name : name.substr(slash + 1);
        if (file_count > 1) it.title += L" 等 " + std::to_wstring(file_count) + L" 个";
        it.glyph = Extension(it.title.substr(0, it.title.find(L" 等 ")));
        it.accent = 0x2f7cf6;
        it.subtitle = L"文件 · " + when;
        break;
    }
    case Kind::Text:
    default:
        it.title = FirstLine(e.text);
        if (const auto color = ParseHexColor(e.text)) {
            it.tile = StackTile::Color;
            it.accent = *color;
            it.title = Trimmed(e.text);
            wchar_t rgb[40]{};
            swprintf_s(rgb, L"颜色 · RGB %u %u %u", (*color >> 16) & 255u, (*color >> 8) & 255u, *color & 255u);
            it.subtitle = rgb;
        } else if (LooksLikeLink(e.text)) {
            it.tile = StackTile::Link;
            const std::wstring host = LinkHost(e.text);
            it.glyph = host.empty() ? L"#" : std::wstring(1, static_cast<wchar_t>(std::towupper(host[0])));
            it.accent = HostColor(host);
            it.subtitle = L"链接 · " + host;
        } else if (LooksLikeCode(e.text)) {
            it.tile = StackTile::Code;
            it.glyph = L"</>";
            it.subtitle = L"代码 · " + when;
        } else {
            it.tile = StackTile::Text;
            it.glyph = L"Aa";
            it.subtitle = L"文本 · " + when;
        }
        if (it.title.empty()) it.title = L"（空白文本）";
        break;
    }
    return it;
}

// 卡片显示内容预览 off (demos, screen sharing): keep the card's type tile and nothing
// readable -- no thumbnail, text, link host or colour value.
inline StackItem Concealed(StackItem it) {
    it.thumb.clear();it.thumb.shrink_to_fit();it.thumb_width = it.thumb_height = 0;
    switch (it.tile) {
    case StackTile::Image: it.title = L"图片"; break;
    case StackTile::File: it.title = L"文件"; break;
    case StackTile::Link: it.title = L"链接"; it.glyph = L"#"; it.accent = 0x6b7a90; break;
    case StackTile::Code: it.title = L"代码"; break;
    case StackTile::Color: it.title = L"颜色"; it.accent = 0x9aa6b5; break;
    case StackTile::Text: default: it.title = L"文本"; it.glyph = L"Aa"; break;
    }
    it.subtitle = L"内容已隐藏";
    return it;
}

}  // namespace lumashot::clipboard