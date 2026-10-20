#pragma once
// Expanded clipboard panel cards (full.html #panel, plan section 4). Pure and
// header-only: geometry from the prototype CSS, time groups, filter chips and
// the capacity footer. The panel supplies measured line counts and image sizes.
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>
#include "clipboard/entry_flavor.h"
#include "clipboard/history.h"

namespace lumashot::clipboard::cards {

// ---- geometry (DIP) -------------------------------------------------------
// .ph / .search / .chips / .pfoot
inline constexpr float HeaderBottom = 54;  // .ph padding 14/8 around a 30 px logo + two-line title
inline constexpr float SearchTop = 56, SearchBottom = 92, SearchInset = 12, SearchRadius = 10;
inline constexpr float ChipTop = 92, ChipBottom = 115, ChipInset = 12, ChipGap = 4, ChipPadX = 10;
inline constexpr float ListTop = 121;     // chips padding-bottom 6
inline constexpr float FooterHeight = 45; // .pfoot 8 + 15 + 6 + 4 + 12
inline constexpr float ListInset = 10;    // #list padding 0 10 10
// .item
inline constexpr float PadTop = 9, PadRight = 10, PadBottom = 8, PadLeft = 32, Gap = 6, Radius = 10;
inline constexpr float IndexX = 8, IndexY = 10, IndexSize = 17, IndexRadius = 5;
inline constexpr float TextLineHeight = 19.5f;  // 13 px * 1.5
inline constexpr int TextMaxLines = 3;
inline constexpr float CodeLineHeight = 18.6f, CodePadY = 7, CodePadX = 9, CodeMax = 76;  // 12 px * 1.55
inline constexpr float BadgeSize = 38, BadgeGap = 10;  // .fav / .swatch / .fileic row
inline constexpr float ThumbMax = 150, ThumbMin = 48, ThumbPlaceholder = 110;
inline constexpr float MetaGap = 6, MetaLine = 15, FootGap = 4;
inline constexpr float GroupHeaderHeight = 30;  // .grp padding 10 / 5 around an 11.5 px line
// .acts (hover action bar)
inline constexpr float ActsRight = 7, ActsTop = 6, ActsPad = 2, ActButton = 28, ActGap = 1, ActsRadius = 9;

struct Measure {
    int text_lines = 1;  // wrapped lines of the text body (clamped to 3)
    int code_lines = 1;  // source lines of a code body
    float image_width = 0, image_height = 0;  // natural size; 0 = unknown yet
};

inline float ContentWidth(float card_width) { return std::max(1.f, card_width - PadLeft - PadRight); }

// Thumbnail box height: width 100 %, never upscaled, max-height 150.
inline float ThumbHeight(const Measure& m, float content_width) {
    if (m.image_width <= 0 || m.image_height <= 0) return ThumbPlaceholder;
    const float shown_width = std::min(content_width, m.image_width);
    return std::clamp(m.image_height * shown_width / m.image_width, ThumbMin, ThumbMax);
}
inline float BodyHeight(Flavor flavor, const Measure& m, float content_width) {
    switch (flavor) {
    case Flavor::Image: return ThumbHeight(m, content_width);
    case Flavor::Code: return std::min(CodeMax, CodePadY * 2 + CodeLineHeight * static_cast<float>(std::max(1, m.code_lines)));
    case Flavor::Link:
    case Flavor::Color:
    case Flavor::Files: return BadgeSize;
    case Flavor::Text:
    default: return TextLineHeight * static_cast<float>(std::clamp(m.text_lines, 1, TextMaxLines));
    }
}
// body, .meta (type + detail), second .meta (star, time, source).
inline float CardHeight(Flavor flavor, const Measure& m, float card_width) {
    return PadTop + BodyHeight(flavor, m, ContentWidth(card_width)) + MetaGap + MetaLine + FootGap + MetaLine + PadBottom;
}
inline int CodeLines(const std::wstring& text) {
    int lines = 1;
    for (wchar_t c : Trimmed(text))
        if (c == L'\n') ++lines;
    return lines;
}

// ---- time ----------------------------------------------------------------
enum class TimeGroup : uint8_t { Favorite, JustNow, Today, Yesterday, Earlier };

inline long long Seconds(const SYSTEMTIME& t) {
    FILETIME f{};
    if (!SystemTimeToFileTime(&t, &f)) return 0;
    return static_cast<long long>((static_cast<unsigned long long>(f.dwHighDateTime) << 32 | f.dwLowDateTime) / 10000000ull);
}
inline long long Midnight(const SYSTEMTIME& t) {
    SYSTEMTIME day = t;
    day.wHour = day.wMinute = day.wSecond = day.wMilliseconds = 0;
    return Seconds(day);
}
// groupOf(): < 1 h 刚刚, same day 今天, previous day 昨天, otherwise 更早.
// Entry times are local (GetLocalTime), so is `now`.
inline TimeGroup GroupOf(const SYSTEMTIME& t, const SYSTEMTIME& now) {
    const long long at = Seconds(t), current = Seconds(now), today = Midnight(now);
    if (current - at < 3600) return TimeGroup::JustNow;
    if (at >= today) return TimeGroup::Today;
    if (at >= today - 86400) return TimeGroup::Yesterday;
    return TimeGroup::Earlier;
}
inline TimeGroup GroupOf(const Entry& e, const SYSTEMTIME& now) {
    return e.favorite ? TimeGroup::Favorite : GroupOf(e.time, now);
}
inline const wchar_t* GroupLabel(TimeGroup g) {
    switch (g) {
    case TimeGroup::Favorite: return L"★ 收藏";
    case TimeGroup::JustNow: return L"刚刚";
    case TimeGroup::Today: return L"今天";
    case TimeGroup::Yesterday: return L"昨天";
    case TimeGroup::Earlier:
    default: return L"更早";
    }
}
// timeLabel(): 刚刚 / N 分钟前 / 今天 HH:MM / 昨天 HH:MM / M/D HH:MM.
inline std::wstring TimeLabel(const SYSTEMTIME& t, const SYSTEMTIME& now) {
    const long long at = Seconds(t), current = Seconds(now), today = Midnight(now), d = current - at;
    wchar_t text[40]{};
    if (d < 60) return L"刚刚";
    if (d < 3600) {
        swprintf_s(text, L"%lld 分钟前", d / 60);
        return text;
    }
    if (at >= today) swprintf_s(text, L"今天 %02u:%02u", t.wHour, t.wMinute);
    else if (at >= today - 86400) swprintf_s(text, L"昨天 %02u:%02u", t.wHour, t.wMinute);
    else swprintf_s(text, L"%u/%u %02u:%02u", t.wMonth, t.wDay, t.wHour, t.wMinute);
    return text;
}

// ---- filter chips ---------------------------------------------------------
// renderChips(): 全部 图片 文本 代码 链接 文件 颜色 ★ 收藏. 代码/链接/颜色 narrow
// the 文本 tab by flavour and are hidden while empty (unless selected); the tab
// indices 0..4 keep their meaning for the quick window and saved state.
struct Chip {
    int tab = 0;
    std::optional<Flavor> flavor;
    const wchar_t* label = L"";
    size_t count = 0;
    bool selected = false;
};
// flavor_of lets the panel reuse its per-entry classification cache.
template <class FlavorFn>
inline std::vector<Chip> Chips(const History& history, int tab, std::optional<Flavor> flavor, FlavorFn&& flavor_of) {
    size_t text = 0, image = 0, files = 0, favorite = 0, code = 0, link = 0, color = 0;
    for (const auto& e : history.entries) {
        if (e.favorite) ++favorite;
        switch (flavor_of(e)) {
        case Flavor::Image: ++image; break;
        case Flavor::Files: ++files; break;
        case Flavor::Code: ++text; ++code; break;
        case Flavor::Link: ++text; ++link; break;
        case Flavor::Color: ++text; ++color; break;
        case Flavor::Text:
        default: ++text; break;
        }
    }
    const Chip all[] = {
        {0, std::nullopt, L"全部", history.entries.size()},
        {2, std::nullopt, L"图片", image},
        {1, std::nullopt, L"文本", text},
        {1, Flavor::Code, L"代码", code},
        {1, Flavor::Link, L"链接", link},
        {3, std::nullopt, L"文件", files},
        {1, Flavor::Color, L"颜色", color},
        {4, std::nullopt, L"★ 收藏", favorite},
    };
    std::vector<Chip> chips;
    for (Chip c : all) {
        c.selected = c.tab == tab && c.flavor == flavor;
        if (c.flavor && !c.count && !c.selected) continue;
        chips.push_back(c);
    }
    return chips;
}

inline std::vector<Chip> Chips(const History& history, int tab, std::optional<Flavor> flavor) {
    return Chips(history, tab, flavor, [](const Entry& e) { return FlavorOf(e); });
}

// ---- footer ----------------------------------------------------------------
inline std::wstring CapacityText(size_t count, size_t bytes, size_t limit = History::MaxEntries) {
    wchar_t text[96]{};
    swprintf_s(text, L"%zu / %zu 条 · %.1f MB / %zu MB", count, limit, static_cast<double>(bytes) / (1024.0 * 1024.0),
               History::MaxBytes / (1024 * 1024));
    return text;
}
// .cap i width: max(3 %, used share).
inline float CapacityRatio(size_t bytes) {
    return std::clamp(static_cast<float>(static_cast<double>(bytes) / static_cast<double>(History::MaxBytes)), .03f, 1.f);
}

// Natural pixel size of an image entry from its DIB / PNG header, when the
// formats are in memory (spooled entries report 0 until a thumbnail exists).
inline std::pair<float, float> ImageSize(const Entry& e) {
    for (const auto& f : e.formats) {
        const auto& b = f.bytes;
        if ((f.id == CF_DIB || f.id == CF_DIBV5) && b.size() >= sizeof(BITMAPINFOHEADER)) {
            BITMAPINFOHEADER h{};
            std::copy_n(b.data(), sizeof(h), reinterpret_cast<unsigned char*>(&h));
            if (h.biWidth > 0 && h.biHeight != 0) return {static_cast<float>(h.biWidth), static_cast<float>(h.biHeight < 0 ? -h.biHeight : h.biHeight)};
        }
        static const unsigned char png[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
        if (b.size() >= 24 && std::equal(png, png + 8, b.begin())) {
            const auto be = [&](size_t at) { return static_cast<uint32_t>(b[at]) << 24 | static_cast<uint32_t>(b[at + 1]) << 16 | static_cast<uint32_t>(b[at + 2]) << 8 | static_cast<uint32_t>(b[at + 3]); };
            const uint32_t w = be(16), h = be(20);
            if (w && h) return {static_cast<float>(w), static_cast<float>(h)};
        }
    }
    return {0.f, 0.f};
}

}  // namespace lumashot::clipboard::cards