// Entry flavours, flavour filtering and panel card model (docs/clipboard-card-stack.md
// section 4, stage S3). Synthetic entries only; no clipboard, no files, no window.
#include "clipboard/panel_cards.h"
#include "clipboard/stack_items.h"
#include <cmath>
#include <cstring>
#include <iostream>
#include <string>

using namespace lumashot::clipboard;
namespace cards = lumashot::clipboard::cards;

namespace {
int checks{}, failures{};
void Expect(bool ok, const char* message) {
    ++checks;
    if (!ok) {
        ++failures;
        std::cout << "FAIL " << message << '\n';
    }
}
bool Near(float a, float b) { return std::fabs(a - b) < .01f; }
Entry Text(const std::wstring& text) {
    Entry e;
    e.kind = Kind::Text;
    e.text = text;
    GetLocalTime(&e.time);
    const auto* p = reinterpret_cast<const unsigned char*>(text.c_str());
    e.formats.push_back({CF_UNICODETEXT, std::vector<unsigned char>(p, p + (text.size() + 1) * sizeof(wchar_t))});
    return e;
}
Entry Image(int width, int height, bool png) {
    Entry e;
    e.kind = Kind::Image;
    e.text = L"图片";
    GetLocalTime(&e.time);
    if (png) {
        std::vector<unsigned char> b = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a, 0, 0, 0, 13, 'I', 'H', 'D', 'R'};
        for (int v : {width, height})
            for (int shift : {24, 16, 8, 0}) b.push_back(static_cast<unsigned char>((v >> shift) & 255));
        b.resize(40, 0);
        e.formats.push_back({RegisterClipboardFormatW(L"PNG"), b});
    } else {
        BITMAPINFOHEADER h{};
        h.biSize = sizeof(h);
        h.biWidth = width;
        h.biHeight = -height;
        h.biPlanes = 1;
        h.biBitCount = 32;
        std::vector<unsigned char> b(sizeof(h) + 16);
        std::memcpy(b.data(), &h, sizeof(h));
        e.formats.push_back({CF_DIB, b});
    }
    return e;
}
Entry Files(const std::wstring& path) {
    Entry e;
    e.kind = Kind::Files;
    e.text = path;
    GetLocalTime(&e.time);
    e.formats = {{CF_HDROP, {1, 2, 3}}};
    return e;
}
SYSTEMTIME At(WORD year, WORD month, WORD day, WORD hour, WORD minute) {
    SYSTEMTIME t{};
    t.wYear = year;
    t.wMonth = month;
    t.wDay = day;
    t.wHour = hour;
    t.wMinute = minute;
    return t;
}
}  // namespace

int main() {
    // Flavour rules: colour > link > code > text, shared with the stack tiles.
    Expect(TextFlavor(L"#1A7CFF") == Flavor::Color && TextFlavor(L"  #fff \n") == Flavor::Color, "hex colours");
    Expect(TextFlavor(L"#12345") == Flavor::Text && TextFlavor(L"#GGGGGG") == Flavor::Text, "malformed hex is text");
    Expect(TextFlavor(L"https://example.invalid/a?b=1") == Flavor::Link && TextFlavor(L"\thttp://x.invalid\r\n") == Flavor::Link, "links");
    Expect(TextFlavor(L"see https://example.invalid now") == Flavor::Text, "sentence with a link is text");
    Expect(TextFlavor(L"const a = () => 1;") == Flavor::Code && TextFlavor(L"def f():\n  pass") == Flavor::Code, "code");
    Expect(TextFlavor(L"") == Flavor::Text && TextFlavor(L" \x3000 ") == Flavor::Text && TextFlavor(L"普通文本") == Flavor::Text, "plain text");
    {
        std::wstring big(1 << 20, L'a');
        big += L" => ";
        Expect(TextFlavor(big) == Flavor::Code, "large text classified without copying");
    }
    Expect(FlavorOf(Image(4, 4, false)) == Flavor::Image && FlavorOf(Files(L"C:\\Synthetic\\a.txt")) == Flavor::Files, "kinds map to flavours");
    {
        const Entry link = Text(L"https://example.invalid");
        Expect(MakeStackItem(link, nullptr).tile == StackTile::Link, "stack tiles keep the same link rule");
        Expect(MakeStackItem(Text(L"#00ff00"), nullptr).tile == StackTile::Color, "stack tiles keep the same colour rule");
    }
    Expect(std::wstring(FlavorName(Flavor::Code)) == L"代码" && std::wstring(FlavorName(Flavor::Files)) == L"文件", "flavour names");

    // History::Filter: optional flavour narrows, tab semantics unchanged.
    History h;
    h.Add(Text(L"普通文本 one"));
    h.Add(Text(L"https://example.invalid/one"));
    h.Add(Text(L"const x = () => 2;"));
    h.Add(Text(L"#336699"));
    h.Add(Image(120, 80, false));
    h.Add(Files(L"C:\\Synthetic\\report.pdf"));
    Expect(h.Filter(1, L"").size() == 4, "text tab keeps every text flavour");
    Expect(h.Filter(1, L"", false, Flavor::Link).size() == 1 && h.Filter(1, L"", false, Flavor::Code).size() == 1, "flavour filter");
    Expect(h.Filter(0, L"", false, Flavor::Color).size() == 1 && h.Filter(2, L"", false, Flavor::Color).empty(), "flavour combines with tab");
    Expect(h.Filter(1, L"example", false, Flavor::Link).size() == 1 && h.Filter(1, L"example", false, Flavor::Code).empty(), "flavour combines with query");
    Expect(h.Filter(0, L"").size() == 6 && h.Filter(2, L"").size() == 1 && h.Filter(3, L"").size() == 1, "default filter unchanged");

    // Chips: order of full.html, empty 代码/链接/颜色 hidden unless selected.
    {
        const auto chips = cards::Chips(h, 0, std::nullopt);
        Expect(chips.size() == 8, "all chips when every flavour exists");
        Expect(std::wstring(chips[0].label) == L"全部" && chips[0].count == 6 && chips[0].selected, "全部 first and selected");
        Expect(std::wstring(chips[1].label) == L"图片" && chips[2].count == 4 && std::wstring(chips[3].label) == L"代码" && chips[3].count == 1, "chip order and counts");
        Expect(std::wstring(chips[7].label) == L"★ 收藏" && chips[7].tab == 4, "favourites last");
        History plain;
        plain.Add(Text(L"only plain"));
        const auto few = cards::Chips(plain, 0, std::nullopt);
        Expect(few.size() == 5, "empty extra filters hidden, fixed tabs stay");
        const auto kept = cards::Chips(plain, 1, Flavor::Link);
        Expect(kept.size() == 6 && kept[3].selected && kept[3].count == 0, "selected empty filter stays visible");
        size_t calls = 0;
        cards::Chips(h, 0, std::nullopt, [&](const Entry& e) { ++calls; return FlavorOf(e); });
        Expect(calls == h.entries.size(), "chips classify each entry once through the supplied cache");
    }

    // Card geometry from the prototype CSS.
    {
        const float width = 380;  // 400 DIP panel - #list padding
        cards::Measure m;
        Expect(Near(cards::ContentWidth(width), 338), "content width = card - 32 - 10");
        Expect(Near(cards::CardHeight(Flavor::Text, m, width), 9 + 19.5f + 6 + 15 + 4 + 15 + 8), "one-line text card");
        m.text_lines = 7;
        Expect(Near(cards::BodyHeight(Flavor::Text, m, 338), 58.5f), "text clamps to three lines");
        m.code_lines = 1;
        Expect(Near(cards::BodyHeight(Flavor::Code, m, 338), 14 + 18.6f), "one-line code");
        m.code_lines = 12;
        Expect(Near(cards::BodyHeight(Flavor::Code, m, 338), 76), "code max-height 76");
        Expect(Near(cards::BodyHeight(Flavor::Link, m, 338), 38) && Near(cards::BodyHeight(Flavor::Files, m, 338), 38) && Near(cards::BodyHeight(Flavor::Color, m, 338), 38), "row flavours");
        cards::Measure image;
        Expect(Near(cards::BodyHeight(Flavor::Image, image, 338), cards::ThumbPlaceholder), "unknown image size uses placeholder");
        image.image_width = 1600;
        image.image_height = 900;
        Expect(Near(cards::BodyHeight(Flavor::Image, image, 338), 150), "tall thumbnails cap at 150");
        image.image_width = 676;
        image.image_height = 100;
        Expect(Near(cards::BodyHeight(Flavor::Image, image, 338), 50), "wide image scales to width");
        image.image_width = 60;
        image.image_height = 90;
        Expect(Near(cards::BodyHeight(Flavor::Image, image, 338), 90), "small image is not upscaled");
        image.image_width = 2000;
        image.image_height = 20;
        Expect(Near(cards::BodyHeight(Flavor::Image, image, 338), cards::ThumbMin), "very wide image keeps a minimum");
        Expect(cards::CodeLines(L"a\nb\nc\n") == 3 && cards::CodeLines(L"one") == 1, "code line count ignores trailing newline");
        Expect(cards::ListTop > cards::ChipBottom && cards::ChipTop >= cards::SearchBottom && cards::SearchTop > cards::HeaderBottom, "header, search, chips, list stack vertically");
    }
    {
        const auto dib = cards::ImageSize(Image(321, 123, false));
        const auto png = cards::ImageSize(Image(640, 480, true));
        Expect(Near(dib.first, 321) && Near(dib.second, 123), "DIB header size (top-down)");
        Expect(Near(png.first, 640) && Near(png.second, 480), "PNG IHDR size");
        Expect(cards::ImageSize(Text(L"x")).first == 0, "no size for text");
    }

    // Time groups and labels (groupOf / timeLabel).
    {
        const SYSTEMTIME now = At(2026, 10, 17, 15, 30);
        Expect(cards::GroupOf(At(2026, 10, 17, 15, 0), now) == cards::TimeGroup::JustNow, "within an hour: 刚刚");
        Expect(cards::GroupOf(At(2026, 10, 17, 9, 0), now) == cards::TimeGroup::Today, "earlier today: 今天");
        Expect(cards::GroupOf(At(2026, 10, 16, 23, 59), now) == cards::TimeGroup::Yesterday, "previous day: 昨天");
        Expect(cards::GroupOf(At(2026, 10, 15, 23, 59), now) == cards::TimeGroup::Earlier, "older: 更早");
        Expect(cards::GroupOf(At(2026, 10, 1, 0, 0), At(2026, 10, 1, 0, 30)) == cards::TimeGroup::JustNow, "just after midnight");
        Expect(cards::GroupOf(At(2026, 9, 30, 23, 0), At(2026, 10, 1, 8, 0)) == cards::TimeGroup::Yesterday, "yesterday across a month");
        Entry fav = Text(L"star");
        fav.favorite = true;
        Expect(cards::GroupOf(fav, now) == cards::TimeGroup::Favorite, "pinned favourites form their own group");
        Expect(std::wstring(cards::GroupLabel(cards::TimeGroup::Yesterday)) == L"昨天", "group label");
        Expect(cards::TimeLabel(At(2026, 10, 17, 15, 30), now) == L"刚刚", "time label now");
        Expect(cards::TimeLabel(At(2026, 10, 17, 15, 4), now) == L"26 分钟前", "time label minutes");
        Expect(cards::TimeLabel(At(2026, 10, 17, 8, 5), now) == L"今天 08:05", "time label today");
        Expect(cards::TimeLabel(At(2026, 10, 16, 21, 0), now) == L"昨天 21:00", "time label yesterday");
        Expect(cards::TimeLabel(At(2026, 3, 2, 7, 9), now) == L"3/2 07:09", "time label date");
    }

    // Capacity footer.
    Expect(cards::CapacityText(8, 629146) == L"8 / 100 条 · 0.6 MB / 128 MB", "capacity text matches the prototype format");
    Expect(Near(cards::CapacityRatio(0), .03f) && Near(cards::CapacityRatio(History::MaxBytes / 2), .5f) && Near(cards::CapacityRatio(History::MaxBytes * 2), 1), "capacity bar ratio");

    Expect(cards::CapacityText(8, 629146, 50) == L"8 / 50 条 · 0.6 MB / 128 MB", "capacity text shows the configured history limit");
    {
        Entry link; link.kind = Kind::Text; link.text = L"https://example.com/secret-path";
        const auto shown = MakeStackItem(link, nullptr), hidden = Concealed(shown);
        Expect(hidden.tile == shown.tile && hidden.title == L"链接" && hidden.glyph == L"#" && hidden.subtitle.find(L"example") == std::wstring::npos, "concealed link card keeps only its type");
        Entry note; note.kind = Kind::Text; note.text = L"password 123456";
        Expect(Concealed(MakeStackItem(note, nullptr)).title == L"文本", "concealed text card hides the text");
        StackItem image; image.tile = StackTile::Image; image.title = L"图片 2×2"; image.thumb_width = image.thumb_height = 2; image.thumb = {1, 2, 3, 4};
        const auto plain = Concealed(image);
        Expect(plain.thumb.empty() && plain.thumb_width == 0 && plain.title == L"图片" && plain.subtitle == L"内容已隐藏", "concealed image card drops its thumbnail");
    }
    std::cout << "CLIPBOARD_ENTRY_FLAVOR checks=" << checks << " failures=" << failures << std::endl;
    return failures ? 1 : 0;
}