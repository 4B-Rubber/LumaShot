// Card stack inside the clipboard panel (docs/clipboard-card-stack.md, S1 part 2).
// Real panel in test mode with synthetic history only: never samples the
// clipboard, reads personal preferences, or injects system input. The layout
// file lives in the test's working directory under build/.
#include "../src/clipboard/panel.cpp"
#include "../src/clipboard/strip_dismiss.cpp"
#include <filesystem>
#include <cmath>
#include <cstring>
#include <iostream>
#include <string>

namespace {
int checks{}, failures{};
void Expect(bool ok, const char* message) {
    ++checks;
    if (!ok) {
        ++failures;
        std::cout << "FAIL " << message << '\n';
    }
}
void Pump(ULONGLONG milliseconds) {
    const auto end = GetTickCount64() + milliseconds;
    MSG msg{};
    while (GetTickCount64() < end) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(5);
    }
}
lumashot::clipboard::Entry Synthetic(uint64_t id, lumashot::clipboard::Kind kind, std::wstring text, WORD hour = 9, WORD minute = 5) {
    lumashot::clipboard::Entry e;
    e.id = id;
    e.kind = kind;
    e.text = std::move(text);
    e.time.wYear = 2026;
    e.time.wMonth = 10;
    e.time.wDay = 17;
    e.time.wHour = hour;
    e.time.wMinute = minute;
    return e;
}
}  // namespace

namespace lumashot {
struct ClipboardPanelTest {
    static void Adapter() {
        using namespace clipboard;
        {
            const auto it = MakeStackItem(Synthetic(1, Kind::Text, L"  \r\n第一行文字\t示例\n第二行"), nullptr);
            Expect(it.tile == StackTile::Text && it.title == L"第一行文字 示例" && it.glyph == L"Aa", "text: first non-empty line, tabs as spaces, Aa tile");
            Expect(it.subtitle == L"文本 · 09:05", "text subtitle carries kind and time");
        }
        {
            const auto it = MakeStackItem(Synthetic(2, Kind::Text, L"const total = items.reduce((a, b) => a + b);"), nullptr);
            Expect(it.tile == StackTile::Code && it.glyph == L"</>", "code uses the panel's code rule");
        }
        {
            const auto it = MakeStackItem(Synthetic(3, Kind::Text, L"https://www.Example.org/path?q=1"), nullptr);
            Expect(it.tile == StackTile::Link && it.glyph == L"E" && it.subtitle == L"链接 · example.org", "link: domain initial and host");
            Expect(it.accent == HostColor(L"example.org") && it.accent == MakeStackItem(Synthetic(9, Kind::Text, L"http://example.org"), nullptr).accent,
                   "link accent is stable per host");
            Expect(MakeStackItem(Synthetic(4, Kind::Text, L"https://a.example see this"), nullptr).tile == StackTile::Text, "text containing a URL and words stays text");
        }
        {
            const auto six = MakeStackItem(Synthetic(5, Kind::Text, L" #2F7CF6 "), nullptr);
            const auto three = MakeStackItem(Synthetic(6, Kind::Text, L"#abc"), nullptr);
            Expect(six.tile == StackTile::Color && six.accent == 0x2f7cf6 && six.title == L"#2F7CF6", "#RRGGBB becomes a colour tile");
            Expect(six.subtitle == L"颜色 · RGB 47 124 246", "colour subtitle lists RGB");
            Expect(three.tile == StackTile::Color && three.accent == 0xaabbcc, "#RGB expands");
            Expect(MakeStackItem(Synthetic(7, Kind::Text, L"#abcg"), nullptr).tile != StackTile::Color, "non-hex is not a colour");
        }
        {
            auto files = Synthetic(8, Kind::Files, L"C:\\work\\季度报告.pptx\r\nC:\\work\\b.txt");
            const auto many = MakeStackItem(files, nullptr, L"C:\\work\\季度报告.pptx", 3);
            Expect(many.tile == StackTile::File && many.title == L"季度报告.pptx 等 3 个" && many.glyph == L"PPTX", "files: first name, count, extension");
            const auto folder = MakeStackItem(files, nullptr, L"C:\\work\\资料", 1);
            Expect(folder.title == L"资料" && folder.glyph == L"FILE", "extensionless name gets the generic badge");
        }
        {
            Frame frame;
            frame.bounds = {0, 0, 4, 2};
            frame.pixels.assign(8, 0xff336699);
            const auto it = MakeStackItem(Synthetic(10, Kind::Image, L""), &frame);
            Expect(it.tile == StackTile::Image && it.thumb_width == 4 && it.thumb_height == 2 && it.thumb.size() == 8, "image thumbnail is passed through");
            Expect(it.title == L"图片 4×2", "image title shows dimensions");
            Expect(MakeStackItem(Synthetic(11, Kind::Image, L""), nullptr).thumb.empty(), "image without a thumbnail keeps the placeholder");
        }
        {
            std::wstring long_line(120, L'字');
            long_line[95] = static_cast<wchar_t>(0xd83d);  // high surrogate exactly at the cut
            const auto it = MakeStackItem(Synthetic(12, Kind::Text, long_line), nullptr);
            Expect(it.title.size() == 95, "long titles are cut without splitting a surrogate pair");
            Expect(MakeStackItem(Synthetic(13, Kind::Text, L" \r\n "), nullptr).title == L"（空白文本）", "blank text has a placeholder title");
        }
        {
            Frame big;
            big.bounds = {0, 0, 640, 480};
            big.pixels.assign(640u * 480u, 0xff204060);
            const auto reduced = ClipboardPanel::Impl::StackThumbnail(big);
            Expect(reduced && reduced->Width() <= 120 && reduced->Height() >= 84 && reduced->Width() >= 96, "thumbnails are reduced to cover the tile");
            Expect(reduced && reduced->pixels[0] == 0xff204060, "area average keeps flat colours exact");
        }
    }
    static void Layout(const std::filesystem::path& ini) {
        using namespace clipboard;
        std::error_code ec;
        std::filesystem::remove(ini, ec);
        Expect(LoadStripStyle(ini) == StripStyle::Stack, "missing setting: stack is the default (upgrades too)");
        Expect(SaveStripStyle(ini, StripStyle::Classic) && LoadStripStyle(ini) == StripStyle::Classic, "classic style persists");
        Expect(SavePosition(ini, {1800, 240}) && LoadStripStyle(ini) == StripStyle::Classic, "saving the position keeps the style");
        const auto position = LoadPosition(ini);
        Expect(position && position->x == 1800 && position->y == 240, "position still round-trips");
        Expect(SaveStripStyle(ini, StripStyle::Stack) && SavePosition(ini, {1800, 260}) && LoadStripStyle(ini) == StripStyle::Stack, "stack style persists");
        std::filesystem::remove(ini, ec);
    }
    static void Panel(const std::filesystem::path& ini) {
        using namespace clipboard;
        int dismissed = 0;
        ClipboardPanel panel(nullptr, [] {});
        panel.SetStripDismissedHandler([&] { ++dismissed; });
        auto& p = *panel.impl_;
        p.test_mode = true;
        p.stack_style = true;
        p.layout_path = ini;
        if (!p.Enable(true, false)) { Expect(false, "create isolated panel"); return; }
        if (!p.stack) {
            std::cout << "SKIP stack: Windows.UI.Composition unavailable, classic strip kept\n";
            Expect(p.folded_window != nullptr, "classic strip remains as fallback");
            p.Enable(false, false);
            return;
        }
        p.Fold();
        Pump(50);
        Expect(!p.expanded && p.stack->Visible(), "folded panel shows the card stack");
        Expect(!IsWindowVisible(p.folded_window) && !IsWindowVisible(p.native_window), "liquid strip and panel stay hidden in stack mode");
        Expect(p.stack->Inspect().cards == 0 && p.stack_ids.empty(), "empty history: no cards");
        p.Present();
        Expect(!IsWindowVisible(p.folded_window), "Present never reveals the liquid strip in stack mode");

        // Synthetic history, newest first.
        p.history.entries.push_back(Synthetic(3, Kind::Text, L"#2f7cf6"));
        p.history.entries.push_back(Synthetic(2, Kind::Text, L"https://docs.example.com/guide"));
        p.history.entries.push_back(Synthetic(1, Kind::Text, L"示例文本"));
        p.RefreshStack();
        Expect((p.stack_ids == std::vector<uint64_t>{3, 2, 1}) && p.stack_total == 3 && p.stack->Inspect().cards == 3, "top entries become cards in history order");
        const unsigned draws = p.stack->Inspect().surface_draws;
        p.RefreshStack();
        p.RefreshStack();
        Expect(p.stack->Inspect().surface_draws == draws, "unchanged history redraws nothing");
        for (uint64_t id = 4; id <= 7; ++id) p.history.entries.insert(p.history.entries.begin(), Synthetic(id, Kind::Text, L"条目 " + std::to_wstring(id)));
        p.RefreshStack();
        Expect(p.stack_ids.size() == 5 && p.stack_ids.front() == 7 && p.stack_total == 7, "at most five cards; total counts all");
        Pump(600);  // incoming slide-in finishes
        Expect(p.stack->Inspect().timers == 0, "no timers left after the update settles");
        {   // A burst of store results coalesces into one stack update per message-loop turn.
            const unsigned before = p.stack->Inspect().surface_draws;
            for (uint64_t id = 8; id <= 27; ++id) {
                p.history.entries.insert(p.history.entries.begin(), Synthetic(id, Kind::Text, L"批量 " + std::to_wstring(id)));
                p.QueueStackRefresh();
            }
            Expect(p.stack_refresh_queued && p.stack_ids.front() == 7, "burst only queues a refresh");
            Pump(30);
            const unsigned burst = p.stack->Inspect().surface_draws - before;
            Expect(!p.stack_refresh_queued && p.stack_ids.front() == 27 && p.stack_total == 27, "queued refresh shows the newest state");
            std::cout << "burst draws=" << burst << " (one refresh of five new cards = 12)\n";
            Expect(burst <= 16, "twenty copies cost one redraw");
            Pump(600);
        }
        {   // Image cards: the card is built before its thumbnail loads; the tile must switch to the real image.
            p.history.entries.insert(p.history.entries.begin(), Synthetic(200, Kind::Image, L""));
            p.RefreshStack();
            Pump(600);
            Expect(p.stack_ids.front() == 200 && p.stack->Inspect().thumbnail_cards == 0, "image card starts with the placeholder while its thumbnail loads");
            auto loaded = std::make_shared<Frame>();
            loaded->bounds = {0, 0, 8, 6};
            loaded->pixels.assign(48, 0xff336699);
            p.thumbnail_cache.Put(200, loaded);
            p.RefreshStack();
            Expect(p.stack_has_thumb.front() && p.stack->Inspect().thumbnail_cards == 1, "folded image card shows its thumbnail once it loads");
            const unsigned settled = p.stack->Inspect().surface_draws;
            p.RefreshStack();
            Expect(p.stack->Inspect().surface_draws == settled, "a shown thumbnail is not redrawn again");
            p.SetCardPreview(false);
            Expect(p.stack->Inspect().thumbnail_cards == 0, "content preview off conceals the thumbnail");
            p.SetCardPreview(true);
            p.thumbnail_cache.Put(200, loaded);
            p.RefreshStack();
            Expect(p.stack->Inspect().thumbnail_cards == 1, "content preview on shows it again");
            Pump(600);
        }
        p.dark = true;
        p.RefreshStack();
        Expect(p.stack_dark, "theme follows the panel");
        p.dark = false;
        p.RefreshStack();

        // Card click: copy (test mode never touches the system clipboard).
        p.last_read_id = 0;
        p.StackPaste(1);
        Expect(p.last_read_id == p.stack_ids[1], "card click copies that entry");
        p.StackPaste(99);
        Expect(p.last_read_id == p.stack_ids[1], "out-of-range index is ignored");
        // Wheel paging: one history entry per notch, clamped; folding returns to the newest.
        p.StackScroll(2);
        Expect(p.stack_first == 2 && p.stack_ids.size() == 5 && p.stack_ids.front() == p.history.entries[2].id && p.stack->Inspect().cards == 5, "wheel shows older entries one per notch");
        p.StackScroll(1000);
        Expect(p.stack_first == p.history.entries.size() - 5 && p.stack_ids.back() == p.history.entries.back().id, "wheel stops at the oldest five");
        p.StackScroll(-1);
        Expect(p.stack_first == p.history.entries.size() - 6, "wheel up shows a newer entry");
        {
            const uint64_t expected_paged = p.history.entries[p.history.entries.size() - 6].id, paged = p.stack_ids[0];
            p.last_read_id = 0;
            p.StackPaste(0);
            Expect(paged == expected_paged && p.last_read_id == paged, "a paged card click copies the entry it shows");
        }
        p.StackFolded();
        Expect(p.stack_first == 0 && p.stack_ids.front() == p.history.entries.front().id, "folding returns to the newest entries");
        p.copy_stack = false;

        // ---- S2: card drag-out. The drag loop is stubbed (no DoDragDrop); files go under build/.
        {
            const auto image = p.stack->RenderDragImage(0);
            const UINT dpi = GetDpiForWindow(p.stack->VisualWindow());
            const float ds = dpi ? dpi / 96.f : 1.f;
            Expect(image.width == static_cast<int>(std::ceil(252 * ds)) && image.height == static_cast<int>(std::ceil(92 * ds)),
                   "ghost canvas: the tilted 1.03 row card plus shadow room");
            Expect(image.offset.x == std::lround((126 - 104 + 26) * ds) && image.offset.y == std::lround((46 - 23 + 22) * ds), "pointer holds the card at (26, 22)");
            const auto px = [&](int x, int y) { return image.pixels[static_cast<size_t>(y) * image.width + x]; };
            Expect(!image.pixels.empty() && (px(image.width / 2, image.height / 2) >> 24) > 0xf0 && (px(0, 0) >> 24) == 0, "card is opaque, corners transparent");
            Expect(p.stack->RenderDragImage(99).pixels.empty(), "no image for a missing card");
            // Manual review only: LUMASHOT_DUMP_DRAG=1 writes the image into this build/ directory.
            wchar_t dump[4]{};
            if (GetEnvironmentVariableW(L"LUMASHOT_DUMP_DRAG", dump, 4) && !image.pixels.empty()) {
                Microsoft::WRL::ComPtr<IWICImagingFactory> wic;
                Microsoft::WRL::ComPtr<IWICBitmap> bitmap;
                Microsoft::WRL::ComPtr<IWICStream> stream;
                Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
                Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;
                const UINT w = static_cast<UINT>(image.width), h = static_cast<UINT>(image.height);
                if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
                    SUCCEEDED(wic->CreateBitmapFromMemory(w, h, GUID_WICPixelFormat32bppPBGRA, w * 4, w * h * 4,
                                                          reinterpret_cast<BYTE*>(const_cast<uint32_t*>(image.pixels.data())), &bitmap)) &&
                    SUCCEEDED(wic->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename((std::filesystem::current_path() / L"drag-image.png").c_str(), GENERIC_WRITE)) &&
                    SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) && SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
                    SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) && SUCCEEDED(frame->WriteSource(bitmap.Get(), nullptr)) &&
                    SUCCEEDED(frame->Commit()))
                    encoder->Commit();
            }
        }
        {
            int runs = 0;
            std::wstring url;
            bool running = false;
            StackWindow::GhostInfo during;
            CardDragResult answer = CardDragResult::Intercepted;
            POINT release{};
            p.drag_runner = [&](CardDragPayload payload, CardDragOptions options) {
                ++runs;
                url = payload.url;
                running = p.drag_running;
                if (options.moved) options.moved(release, false, false);
                during = p.stack->InspectGhost();
                if (options.on_release) options.on_release(release, 0);
                return answer;
            };
            const RECT r = p.stack->WindowRect();
            release = {(r.left + r.right) / 2, (r.top + r.bottom) / 2};
            const uint64_t third = p.stack_ids[2];
            Expect(p.StackDrag(2, release) && runs == 1 && running, "dragging a card runs one drag loop");
            Expect(p.history.entries.front().id == third && p.stack_ids.front() == third, "released on the stack: moved to the top");
            Expect(p.status == L"已移到最前", "move to top is reported");
            Expect(during.active && during.hint == L"松手：放回并置顶" && during.target.scale == .92f && during.pose.opacity == .8f && during.animating,
                   "over the stack the ghost shrinks to .92 and says so");
            Expect(during.card.right > during.card.left && during.hint_rect.right > during.hint_rect.left, "ghost and hint windows are shown");
            Expect(!p.stack->InspectGhost().active, "ghost windows are destroyed after the drag");
            // Refused by the target elsewhere: copy (test mode never touches the clipboard).
            release = {r.left - 400, r.top};
            answer = CardDragResult::NoTarget;
            p.last_read_id = 0;
            const uint64_t second = p.stack_ids[1];
            p.StackDrag(1, release);
            Expect(during.hint == L"拖到程序或桌面" || during.hint.starts_with(L"松手：在桌面"), "away from the stack the hint changes");
            Expect(p.last_read_id == second && p.status == L"目标不接受拖放，已复制，可按 Ctrl+V", "no target: copied with a tip");
            answer = CardDragResult::Canceled;
            const auto order = p.stack_ids;
            p.status.clear();
            p.StackDrag(0, release);
            Expect(p.stack_ids == order && p.status.empty(), "Esc changes nothing");
            // Link cards carry the URL.
            p.history.entries.insert(p.history.entries.begin(), Synthetic(40, Kind::Text, L"https://docs.example.com/guide"));
            p.RefreshStack();
            answer = CardDragResult::Dropped;
            p.StackDrag(0, release);
            Expect(url == L"https://docs.example.com/guide", "link drag offers the URL");
            // Missing files: no drag at all.
            auto gone = Synthetic(41, Kind::Files, L"missing");
            const std::wstring list = (std::filesystem::current_path() / L"no-such-file.bin").wstring() + L'\0' + L'\0';
            std::vector<unsigned char> drop(sizeof(DROPFILES) + list.size() * sizeof(wchar_t));
            DROPFILES header{};
            header.pFiles = sizeof(DROPFILES);
            header.fWide = TRUE;
            std::memcpy(drop.data(), &header, sizeof(header));
            std::memcpy(drop.data() + sizeof(header), list.data(), list.size() * sizeof(wchar_t));
            gone.formats.push_back({CF_HDROP, drop});
            p.history.entries.insert(p.history.entries.begin(), gone);
            p.RefreshStack();
            const int before = runs;
            Expect(!p.StackDrag(0, release) && runs == before && p.status == L"文件已不存在", "missing files refuse the drag");
            p.copy_pending = true;
            Expect(!p.StackDrag(1, release) && runs == before, "no drag while a copy is pending");
            p.copy_pending = false;
            p.drag_runner = nullptr;
            p.history.Erase(40);
            p.history.Erase(41);
            p.RefreshStack();
            Pump(400);
        }
        {
            // Release actions that write: the desktop is replaced by a folder under build/.
            const auto folder = std::filesystem::current_path() / L"drag-desktop";
            std::error_code ec;
            std::filesystem::remove_all(folder, ec);
            std::filesystem::create_directories(folder, ec);
            p.drag_save_folder = folder;
            auto text = Synthetic(50, Kind::Text, L"周报 草稿");
            const std::wstring full = L"周报 草稿\r\n完整正文";
            const auto* bytes = reinterpret_cast<const unsigned char*>(full.c_str());
            text.formats.push_back({CF_UNICODETEXT, {bytes, bytes + (full.size() + 1) * sizeof(wchar_t)}});
            p.ApplyRelease(ReleaseAction::SaveText, text, text.id, POINT{});
            Expect(std::filesystem::exists(folder / L"周报 草稿.txt") && p.status == L"已保存到桌面：周报 草稿.txt", "text on the desktop becomes a .txt");
            p.ApplyRelease(ReleaseAction::SaveText, Synthetic(51, Kind::Text, L"https://docs.example.com/guide"), 51, POINT{});
            Expect(std::filesystem::exists(folder / L"docs.example.com.url"), "link on the desktop becomes a .url");
            // Pin: the decoded full image goes to the handler at the release point.
            auto picture = Synthetic(52, Kind::Image, L"图片");
            BITMAPINFOHEADER h{};
            h.biSize = sizeof(h);
            h.biWidth = 3;
            h.biHeight = 2;
            h.biPlanes = 1;
            h.biBitCount = 32;
            std::vector<unsigned char> dib(sizeof(h) + 3 * 2 * 4, 0x80);
            std::memcpy(dib.data(), &h, sizeof(h));
            picture.formats.push_back({CF_DIB, dib});
            std::shared_ptr<const Frame> pinned;
            POINT pinned_at{};
            panel.SetPinHandler([&](std::shared_ptr<const Frame> frame, POINT at) { pinned = std::move(frame); pinned_at = at; return true; });
            p.ApplyRelease(ReleaseAction::Pin, picture, picture.id, POINT{120, 340});
            Expect(pinned && pinned->Width() == 3 && pinned->Height() == 2 && pinned_at.x == 120 && pinned_at.y == 340 && p.status == L"已钉到桌面",
                   "image on the desktop: pin at the release point");
            panel.SetPinHandler(nullptr);
            p.drag_save_folder.clear();
            std::filesystem::remove_all(folder, ec);
        }

        // Grip drag: the anchor is shared with the classic strip and persisted.
        MONITORINFO monitor{sizeof(monitor)};
        const RECT shown = p.stack->WindowRect();
        GetMonitorInfoW(MonitorFromRect(&shown, MONITOR_DEFAULTTONEAREST), &monitor);
        const RECT work = monitor.rcWork;
        const float s = GetDpiForWindow(p.stack->VisualWindow()) / 96.f;
        const LONG top = work.top + static_cast<LONG>(300 * s);
        p.stack->Place(stack::Side::Left, top, work, s);
        p.StackMoved();
        const auto saved = LoadPosition(ini);
        Expect(p.anchor.x == work.left + static_cast<LONG>(ClipboardPanel::Impl::CW * s) && std::abs(p.anchor.y - top) <= 1, "left dock: classic-compatible anchor");
        Expect(saved && saved->x == p.anchor.x && saved->y == p.anchor.y, "moved stack position is saved");
        p.Place();
        Expect(p.stack->Side() == stack::Side::Left, "re-placing keeps the left edge");
        p.stack->Place(stack::Side::Right, top, work, s);
        p.StackMoved();
        p.Place();
        Expect(p.anchor.x == work.right - 6 && p.stack->Side() == stack::Side::Right, "right dock restores the default anchor");

        // Dragging onto the close target hides the stack and keeps its place.
        {
            const POINT before = p.anchor;
            const DismissZone zone{work, s};
            const POINT c = zone.Center();
            POINT at{work.right - 20, before.y + 10};
            p.StackDragging(at);
            at.x -= static_cast<LONG>(60 * s);
            at.y += static_cast<LONG>(30 * s);
            p.StackDragging(at);
            Expect(p.dismiss_tracker.Active() && !p.dismiss_tracker.Armed(), "moving the stack reveals the target");
            p.StackDragging(c);
            Expect(p.dismiss_tracker.Armed(), "reaching the target arms it");
            p.StackMoved();
            Expect(p.strip_hidden && !p.stack->Visible() && dismissed == 1, "release on the target hides the stack");
            Expect(p.anchor.x == before.x && p.anchor.y == before.y, "hidden stack returns to its old place later");
            panel.SetStripVisible(true);
            Expect(!p.strip_hidden && p.stack->Visible(), "tray/settings restore the stack");
            p.StackDragging({work.right - 20, before.y});
            p.StackDragging({work.right - 20 - static_cast<LONG>(60 * s), before.y + static_cast<LONG>(30 * s)});
            p.stack_moving = false;
            p.CancelDismiss();
            Expect(!p.dismiss_tracker.Active() && !p.strip_hidden, "Esc cancels without hiding");
        }

        // Expanding hides the stack; folding brings it back; idle trim frees the panel renderer.
        p.Show(false);
        Expect(p.expanded && !p.stack->Visible(), "open panel hides the stack");
        {   // Whole-card drag from the open panel: dropped back on the panel moves it to the top.
            uint64_t lifted = 0;
            StackWindow::GhostInfo g;
            const uint64_t last = p.history.entries.back().id;
            RECT pr{};
            GetWindowRect(p.window, &pr);
            const POINT inside{(pr.left + pr.right) / 2, (pr.top + pr.bottom) / 2};
            p.drag_runner = [&](CardDragPayload, CardDragOptions options) {
                lifted = p.drag_id;
                if (options.moved) options.moved(inside, false, false);
                g = p.stack->InspectGhost();
                if (options.on_release) options.on_release(inside, 0);
                return CardDragResult::Intercepted;
            };
            Expect(p.DragEntry(last, true) && lifted == last && p.drag_id == 0, "the panel card is dimmed only while dragged");
            Expect(p.history.entries.front().id == last && p.visible.front() == last, "dropped back on the panel: moved to the top");
            Expect(g.active && g.hint == L"松手：放回并置顶", "the ghost also runs for panel drags (stack hidden)");
            p.drag_runner = nullptr;
        }
        p.Fold();
        Expect(!p.expanded && p.stack->Visible(), "folding shows the stack again");
        {   // Fold <-> expand continuity (docs 4.3): the cards fly to / from panel slots in DWM, then settle.
            p.stack->SetMotion(true);
            const RECT sw = p.stack->WindowRect();
            std::vector<RECT> slots;
            for (LONG i = 0; i < 3; ++i) slots.push_back(RECT{sw.left - 400, sw.top + i * 80, sw.left - 40, sw.top + i * 80 + 70});
            Expect(p.stack->Fly(slots, true) && p.stack->Flying(), "stack cards fly toward the panel slots");
            RECT grown{};
            GetWindowRect(p.stack->VisualWindow(), &grown);
            Expect(grown.left <= slots[0].left && grown.right >= sw.right && grown.bottom >= slots[2].bottom, "the visual window covers the flight path");
            p.stack->Show(false);
            Expect(p.stack->Visible() && p.stack->Flying(), "hiding mid-flight keeps the cards until they land");
            Pump(500);
            Expect(!p.stack->Flying() && !p.stack->Visible() && p.stack->Inspect().timers == 0, "landed: stack hidden, no timers left");
            p.stack->Show(true);
            Expect(p.stack->Fly(slots, false) && p.stack->Flying(), "cards fly back from the panel");
            Pump(500);
            RECT back{};
            GetWindowRect(p.stack->VisualWindow(), &back);
            Expect(!p.stack->Flying() && p.stack->Visible() && EqualRect(&back, &sw) && p.stack->Inspect().timers == 0, "landed: window back on the stack rect");
            p.stack->SetMotion(false);
            Expect(!p.stack->Fly(slots, true) && !p.stack->Flying(), "no flight when animations are off");
            p.stack->SetMotion(p.MotionEnabled());
        }
        SendMessageW(p.EventWindow(), WM_TIMER, 4, 0);
        Expect(!p.composition && !p.parked_composition && !p.target, "idle trim releases the hidden panel's composition and renderer");
        Expect(!IsWindowVisible(p.folded_window) && !p.idle_strip, "stack mode never hands off to the idle strip");

        p.Enable(false, false);
        Expect(!p.stack, "disabling the panel destroys the stack");
    }
};
}  // namespace lumashot

int main() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const auto ini = std::filesystem::current_path() / L"stack-panel-layout.ini";
    lumashot::ClipboardPanelTest::Adapter();
    lumashot::ClipboardPanelTest::Layout(ini);
    lumashot::ClipboardPanelTest::Panel(ini);
    std::error_code ec;
    std::filesystem::remove(ini, ec);
    CoUninitialize();
    std::cout << "STACK_PANEL checks=" << checks << " failures=" << failures << '\n';
    return failures ? 1 : 0;
}