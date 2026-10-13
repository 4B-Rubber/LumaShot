#pragma once
#include <string>
#include <string_view>

namespace lumashot {
// Unicode White_Space, independent of locale. Preserve zero-width joiners and
// other format characters because removing them can change written text.
inline std::wstring RemoveTextWhitespace(std::wstring_view text) {
    std::wstring result;
    result.reserve(text.size());
    for (wchar_t c : text) {
        const bool whitespace = (c >= L'\t' && c <= L'\r') || c == L' ' ||
            c == 0x0085 || c == 0x00a0 || c == 0x1680 ||
            (c >= 0x2000 && c <= 0x200a) || c == 0x2028 || c == 0x2029 ||
            c == 0x202f || c == 0x205f || c == 0x3000;
        if (!whitespace) result += c;
    }
    return result;
}
}
