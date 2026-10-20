#pragma once
// Content flavours of clipboard history entries (plan section 4.2). History
// only knows Text / Image / Files; links, colours and code are Text entries
// recognised from their contents. One rule set is shared by the card stack,
// the panel cards and History::Filter. Pure, header-only, no Windows headers.
#include <cstdint>
#include <cwctype>
#include <optional>
#include <string>
#include <string_view>

namespace lumashot::clipboard {

enum class Flavor : uint8_t { Text, Code, Link, Color, Image, Files };

inline bool LooksLikeCode(const std::wstring& text) {
    return text.find(L"=>") != std::wstring::npos || text.starts_with(L"const ") || text.starts_with(L"function ") ||
           text.starts_with(L"def ");
}
inline std::wstring Trimmed(const std::wstring& text) {
    const auto first = text.find_first_not_of(L" \t\r\n\x3000");
    if (first == std::wstring::npos) return {};
    const auto last = text.find_last_not_of(L" \t\r\n\x3000");
    return text.substr(first, last - first + 1);
}
// First non-empty line, tabs as spaces, at most `limit` characters.
inline std::wstring FirstLine(const std::wstring& text, size_t limit = 96) {
    const std::wstring t = Trimmed(text);
    std::wstring line = t.substr(0, t.find_first_of(L"\r\n"));
    for (auto& c : line)
        if (c == L'\t') c = L' ';
    if (line.size() > limit) {
        line.resize(limit);
        if (line.back() >= 0xd800 && line.back() <= 0xdbff) line.pop_back();
    }
    return line;
}
// "#RGB" / "#RRGGBB" (whole entry) -> 0xRRGGBB.
inline std::optional<uint32_t> ParseHexColor(const std::wstring& text) {
    const std::wstring t = Trimmed(text);
    if ((t.size() != 4 && t.size() != 7) || t[0] != L'#') return {};
    uint32_t value = 0;
    for (size_t i = 1; i < t.size(); ++i) {
        const wchar_t c = static_cast<wchar_t>(std::towlower(t[i]));
        uint32_t digit{};
        if (c >= L'0' && c <= L'9') digit = static_cast<uint32_t>(c - L'0');
        else if (c >= L'a' && c <= L'f') digit = static_cast<uint32_t>(c - L'a' + 10);
        else return {};
        value = value * 16 + digit;
        if (t.size() == 4) value = value * 16 + digit;
    }
    return value;
}
inline bool LooksLikeLink(const std::wstring& text) {
    const std::wstring t = Trimmed(text);
    return (t.starts_with(L"http://") || t.starts_with(L"https://")) && t.find_first_of(L" \t\r\n") == std::wstring::npos;
}
inline std::wstring LinkHost(const std::wstring& url) {
    std::wstring t = Trimmed(url);
    const auto scheme = t.find(L"://");
    if (scheme != std::wstring::npos) t = t.substr(scheme + 3);
    t = t.substr(0, t.find_first_of(L"/?#:"));
    if (t.starts_with(L"www.")) t = t.substr(4);
    for (auto& c : t) c = static_cast<wchar_t>(std::towlower(c));
    return t;
}
inline std::wstring Extension(const std::wstring& name) {
    const auto dot = name.find_last_of(L'.');
    const auto slash = name.find_last_of(L"\\/");
    if (dot == std::wstring::npos || dot + 1 >= name.size() || (slash != std::wstring::npos && dot < slash)) return L"FILE";
    std::wstring ext = name.substr(dot + 1, 4);
    for (auto& c : ext) c = static_cast<wchar_t>(std::towupper(c));
    return ext;
}
// Text entry flavour; priority colour > link > code > text (same as the stack
// tiles). Runs on every history entry (chips, filters), so it never copies the
// text: entries can hold up to 16 MB.
inline Flavor TextFlavor(const std::wstring& text) {
    constexpr wchar_t space[] = L" \t\r\n\x3000";
    const auto first = text.find_first_not_of(space);
    if (first == std::wstring::npos) return Flavor::Text;
    const auto last = text.find_last_not_of(space);
    const size_t length = last - first + 1;
    if (length <= 7 && ParseHexColor(text.substr(first, length))) return Flavor::Color;
    const std::wstring_view view(text.data() + first, length);
    if ((view.starts_with(L"http://") || view.starts_with(L"https://")) && view.find_first_of(L" \t\r\n") == std::wstring_view::npos)
        return Flavor::Link;
    if (LooksLikeCode(text)) return Flavor::Code;
    return Flavor::Text;
}
inline const wchar_t* FlavorName(Flavor f) {
    switch (f) {
    case Flavor::Code: return L"代码";
    case Flavor::Link: return L"链接";
    case Flavor::Color: return L"颜色";
    case Flavor::Image: return L"图片";
    case Flavor::Files: return L"文件";
    case Flavor::Text:
    default: return L"文本";
    }
}

}  // namespace lumashot::clipboard
