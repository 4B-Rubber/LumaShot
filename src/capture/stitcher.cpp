#include "capture/stitcher.h"
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <utility>

namespace lumashot {
namespace {
constexpr uint64_t FnvOffset = 1469598103934665603ull, FnvPrime = 1099511628211ull;
constexpr size_t MaxHashRepeats = 8;

int Luma(uint32_t p) noexcept {
    return static_cast<int>((((p >> 16) & 255) * 77 + ((p >> 8) & 255) * 150 + (p & 255) * 29) >> 8);
}

int EffectiveGuard(int width, int guard) noexcept {
    return std::clamp(guard, 0, width / 6);
}

// Rows fixed at the same position from the top (or bottom) of both frames:
// sticky headers, toolbars and footers that do not scroll with the page.
int FixedRows(const RowSignatures& a, const RowSignatures& b, bool from_top) {
    const int h = static_cast<int>(a.hash.size());
    const int limit = h / 3;
    int n = 0;
    while (n < limit) {
        const int y = from_top ? n : h - 1 - n;
        if (a.hash[y] != b.hash[y]) break;
        ++n;
    }
    // A run made only of blank rows is page margin, not a fixed bar.
    bool content = false;
    for (int i = 0; i < n && !content; ++i) content = !a.uniform[from_top ? i : h - 1 - i];
    return content ? n : 0;
}
}

RowSignatures SignRows(const Frame& frame, int guard) {
    const int w = frame.Width(), h = frame.Height();
    const int cw = std::max(1, w - EffectiveGuard(w, guard));
    RowSignatures s;
    s.hash.resize(h);
    s.uniform.resize(h);
    s.profile.resize(h);
    for (int y = 0; y < h; ++y) {
        const uint32_t* row = frame.pixels.data() + static_cast<size_t>(y) * w;
        uint64_t hash = FnvOffset;
        bool uniform = true;
        const uint32_t first = row[0] & 0xffffff;
        std::array<int, 8> sum{};
        std::array<int, 8> count{};
        for (int x = 0; x < cw; ++x) {
            const uint32_t p = row[x] & 0xffffff;
            hash = (hash ^ p) * FnvPrime;
            uniform = uniform && p == first;
            const int bin = x * 8 / cw;
            sum[bin] += Luma(p);
            ++count[bin];
        }
        s.hash[y] = hash;
        s.uniform[y] = uniform ? 1 : 0;
        for (int i = 0; i < 8; ++i)
            s.profile[y][i] = static_cast<uint8_t>(count[i] ? sum[i] / count[i] : 0);
    }
    return s;
}

double SamePositionRatio(const RowSignatures& a, const RowSignatures& b) {
    if (a.hash.size() != b.hash.size() || a.hash.empty()) return 0;
    size_t considered = 0, equal = 0, all_equal = 0;
    for (size_t y = 0; y < a.hash.size(); ++y) {
        const bool same = a.hash[y] == b.hash[y];
        all_equal += same;
        if (a.uniform[y] && b.uniform[y]) continue;
        ++considered;
        equal += same;
    }
    if (!considered) return static_cast<double>(all_equal) / static_cast<double>(a.hash.size());
    return static_cast<double>(equal) / static_cast<double>(considered);
}

Stitcher::Stitcher(StitchOptions options) : options_(options) {}

int Stitcher::BandRows() const {
    return std::max(0, frame_height_ - header_ - std::max(footer_, 0));
}

int Stitcher::ViewportTop() const {
    // The current frame's scrolling rows end where the content ends.
    return std::max(0, content_rows_ - (frame_height_ - std::max(footer_, 0) - header_));
}

double Stitcher::MatchRatio(const RowSignatures& f, int shift, int top, int bottom, int* informative) const {
    const int end = frame_height_ - bottom - shift;
    int total = 0, equal = 0;
    for (int y = top; y < end; ++y) {
        const bool blank = f.uniform[y] && last_signatures_.uniform[y + shift];
        if (blank && f.hash[y] == last_signatures_.hash[y + shift]) continue;
        ++total;
        equal += f.hash[y] == last_signatures_.hash[y + shift];
    }
    if (informative) *informative = total;
    if (!total) return end - top > 0 ? 1.0 : 0.0;
    return static_cast<double>(equal) / total;
}

int Stitcher::ProfileFixedRows(const RowSignatures& f, bool from_top) const {
    const int h = frame_height_;
    int n = 0;
    while (n < h / 3) {
        const int y = from_top ? n : h - 1 - n;
        int err = 0;
        for (int i = 0; i < 8; ++i) err += std::abs(int(f.profile[y][i]) - int(last_signatures_.profile[y][i]));
        if (err > 8) break;
        ++n;
    }
    return n;
}

Stitcher::Estimate Stitcher::Find(const RowSignatures& f, int top, int bottom, int hint) const {
    const int h = frame_height_;
    const int band_end = h - bottom;
    const int max_shift = band_end - top - options_.min_overlap;
    Estimate best{0, 0, false, top, bottom, 0};
    if (max_shift < 1) return best;
    if (hint <= 0) hint = last_shift_ > 0 ? last_shift_ : (band_end - top) / 2;
    auto closer = [hint](int a, int b) { return std::abs(a - hint) < std::abs(b - hint); };
    // 1) Exact row-hash voting. Rows of the previous frame that are distinctive
    //    (not blank, not repeated too often) vote for shift = y_prev - y_new.
    std::unordered_map<uint64_t, std::vector<int>> positions;
    positions.reserve(static_cast<size_t>(band_end - top));
    for (int y = top; y < band_end; ++y) {
        if (last_signatures_.uniform[y]) continue;
        auto& list = positions[last_signatures_.hash[y]];
        if (list.size() <= MaxHashRepeats) list.push_back(y);
    }
    std::vector<int> votes(static_cast<size_t>(max_shift) + 1);
    for (int y = top; y < band_end; ++y) {
        if (f.uniform[y]) continue;
        const auto it = positions.find(f.hash[y]);
        if (it == positions.end() || it->second.size() > MaxHashRepeats) continue;
        for (int p : it->second) {
            const int d = p - y;
            if (d >= 1 && d <= max_shift) ++votes[d];
        }
    }
    int top_votes = 0;
    for (int d = 1; d <= max_shift; ++d) {
        if (votes[d] > top_votes || (votes[d] == top_votes && votes[d] > 0 && closer(d, best.shift))) {
            top_votes = votes[d];
            best.shift = d;
        }
    }
    best.votes = top_votes;
    if (top_votes >= 3) {
        // Several shifts can collect votes on repetitive content; keep the one
        // whose whole overlap agrees best.
        double best_match = MatchRatio(f, best.shift, top, bottom);
        for (int d = 1; d <= max_shift; ++d) {
            if (d == best.shift || votes[d] * 2 < top_votes) continue;
            const double m = MatchRatio(f, d, top, bottom);
            if (m > best_match + 1e-9 || (m > best_match - 1e-9 && closer(d, best.shift))) {
                best_match = m;
                best.shift = d;
            }
        }
        best.match = best_match;
        if (best.match >= options_.min_match) return best;
    }
    // 2) Ambiguous content (blank paper, identical table rows): every shift
    //    whose overlap agrees exactly is a candidate; prefer the expected one.
    //    The overlap must carry information unless the new band has none.
    {
        int informative_rows = 0;
        for (int y = top; y < band_end; ++y) informative_rows += !f.uniform[y];
        const int needed = std::min(options_.min_overlap / 2, informative_rows);
        std::vector<double> matches(static_cast<size_t>(max_shift) + 1, -1.0);
        double top_match = 0;
        for (int d = 1; d <= max_shift; ++d) {
            int informative = 0;
            const double m = MatchRatio(f, d, top, bottom, &informative);
            if (m < options_.min_match || informative < needed) continue;
            matches[d] = m;
            top_match = std::max(top_match, m);
        }
        int shift = 0;
        for (int d = 1; d <= max_shift; ++d)
            if (matches[d] >= top_match - .02 && matches[d] >= 0 && (!shift || closer(d, shift))) shift = d;
        if (shift) return {shift, matches[shift], false, top, bottom, top_votes};
    }
    // 3) Anti-aliasing / subpixel noise: compare coarse luminance profiles of
    //    every row and require a clear unique minimum.
    const int ftop = std::max(top, ProfileFixedRows(f, true));
    const int fbottom = std::max(bottom, ProfileFixedRows(f, false));
    const int fband_end = h - fbottom;
    const int fmax = fband_end - ftop - options_.min_overlap;
    double err_best = 1e9, err_second = 1e9;
    int fuzzy_shift = 0;
    for (int d = 1; d <= fmax; ++d) {
        const int end = fband_end - d;
        long long err = 0;
        int rows = 0, informative = 0;
        for (int y = ftop; y < end; ++y) {
            const auto& a = f.profile[y];
            const auto& b = last_signatures_.profile[y + d];
            for (int i = 0; i < 8; ++i) err += std::abs(int(a[i]) - int(b[i]));
            ++rows;
            informative += !(f.uniform[y] && last_signatures_.uniform[y + d]);
        }
        if (!rows || informative < options_.min_overlap / 2) continue;
        const double e = static_cast<double>(err) / (rows * 8.0);
        if (e < err_best) {
            if (std::abs(d - fuzzy_shift) > 2) err_second = err_best;
            err_best = e;
            fuzzy_shift = d;
        } else if (e < err_second && std::abs(d - fuzzy_shift) > 2) {
            err_second = e;
        }
    }
    if (fuzzy_shift && err_best < 2.0 && err_second - err_best > 1.0)
        return {fuzzy_shift, std::max(MatchRatio(f, fuzzy_shift, ftop, fbottom), 1.0 - err_best / 16.0), true, ftop, fbottom, top_votes};
    // 4) A voted shift with a poor overlap (content re-laid out after the
    //    scroll) is offered to the user; anything weaker is no overlap.
    if (best.shift && top_votes >= 8) return best;
    return {0, 0, false, top, bottom, top_votes};
}

StitchResult Stitcher::Add(const Frame& frame, int hint) {
    if (frame.Width() <= 0 || frame.Height() <= 0) return {};
    if (!Started()) {
        width_ = frame.Width();
        frame_height_ = frame.Height();
        guard_ = EffectiveGuard(width_, options_.guard);
        if (frame_height_ > options_.max_height) {
            width_ = frame_height_ = 0;
            return {StitchStatus::Limit};
        }
        WriteContent(0, frame.pixels.data(), frame_height_);
        content_rows_ = frame_height_;
        last_ = frame;
        last_signatures_ = SignRows(frame, guard_);
        return {StitchStatus::First, 0, 1.0, frame_height_};
    }
    if (frame.Width() != width_ || frame.Height() != frame_height_) return {StitchStatus::Mismatch};
    if (Height() >= options_.max_height) return {StitchStatus::Limit};
    RowSignatures signatures = SignRows(frame, guard_);
    const double same = SamePositionRatio(last_signatures_, signatures);
    if (same >= .97) {
        // Nothing scrolled. Refresh the last frame so that late repaints of
        // fixed bars and the visible content are what gets composed.
        pending_.reset();
        if (history_.empty() && footer_ < 0 && frame.pixels != last_.pixels) {
            WriteContent(0, frame.pixels.data(), frame_height_);
            ++epoch_;
        }
        last_ = frame;
        last_signatures_ = std::move(signatures);
        return {StitchStatus::Unchanged, 0, same, 0};
    }
    StitchStatus status = StitchStatus::Appended;
    const Estimate estimate = Evaluate(signatures, hint, &status);
    pending_.reset();
    if (status == StitchStatus::NoOverlap) return {StitchStatus::NoOverlap, estimate.shift, estimate.shift > 0 ? estimate.match : 0, 0};
    if (status == StitchStatus::LowMatch) {
        pending_ = Pending{frame, std::move(signatures), estimate.shift, estimate.top, estimate.bottom, estimate.match};
        return {StitchStatus::LowMatch, estimate.shift, estimate.match, 0};
    }
    StitchResult result = Append(frame, std::move(signatures), estimate.shift, estimate.top, estimate.bottom);
    result.match = estimate.match;
    return result;
}

Stitcher::Estimate Stitcher::Evaluate(const RowSignatures& signatures, int hint, StitchStatus* status) const {
    const int top = FixedRows(last_signatures_, signatures, true);
    const int bottom = std::max(FixedRows(last_signatures_, signatures, false), std::max(footer_, 0));
    const Estimate estimate = Find(signatures, top, bottom, hint);
    const int footer = footer_ >= 0 ? footer_ : estimate.bottom;
    if (estimate.shift <= 0 || estimate.shift > frame_height_ - footer - estimate.top - options_.min_overlap)
        *status = StitchStatus::NoOverlap;
    else if (estimate.match < options_.min_match)
        *status = StitchStatus::LowMatch;
    else
        *status = StitchStatus::Appended;
    return estimate;
}

StitchResult Stitcher::AcceptPending(const Frame* fresh, int hint) {
    if (!pending_) return {};
    Pending p = std::move(*pending_);
    pending_.reset();
    if (fresh && fresh->Width() == width_ && fresh->Height() == frame_height_) {
        // The held frame was taken right after the scroll settled, often while
        // an overlay or animation was still on screen. A capture taken when
        // the user decides is preferred: a clean match appends normally.
        RowSignatures signatures = SignRows(*fresh, guard_);
        if (SamePositionRatio(last_signatures_, signatures) < .97) {
            StitchStatus status = StitchStatus::NoOverlap;
            const Estimate e = Evaluate(signatures, hint > 0 ? hint : p.shift, &status);
            if (status == StitchStatus::Appended) {
                StitchResult result = Append(*fresh, std::move(signatures), e.shift, e.top, e.bottom);
                result.match = e.match;
                return result;
            }
            if (status == StitchStatus::LowMatch && e.match > p.match)
                p = Pending{*fresh, std::move(signatures), e.shift, e.top, e.bottom, e.match};
        }
    }
    StitchResult result = Append(std::move(p.frame), std::move(p.signatures), p.shift, p.top, p.bottom, true);
    result.match = p.match;
    return result;
}

StitchResult Stitcher::Append(Frame frame, RowSignatures signatures, int shift, int top, int bottom, bool forced) {
    const int h = frame_height_;
    const int previous_header = header_;
    header_ = top;
    if (footer_ < 0) {
        // The fixed footer is decided on the first real scroll; from now on the
        // content excludes it and the footer comes from the latest frame.
        footer_ = std::min(bottom, h / 3);
        content_rows_ -= footer_;
    }
    const int available = options_.max_height - footer_ - content_rows_;
    StitchStatus status = StitchStatus::Appended;
    int rows = shift;
    if (available <= 0) return {StitchStatus::Limit, shift, 0, 0};
    if (rows > available) {
        rows = available;
        status = StitchStatus::Limit;
    }
    // A forced frame disagrees with the previous one in many rows; its edge
    // columns say nothing reliable about a scrollbar.
    if (!forced) DetectScrollbar(frame, shift, top, bottom);
    history_.push_back(Delta(previous_header));
    History& entry = history_.back();
    entry.suspect = suspect_from_;
    if (!forced && suspect_from_ >= 0) {
        // The previous segment was accepted despite a poor overlap, so its
        // rows may carry a transient overlay (a sliding sticky bar, a hover
        // card, a fading toast). This frame agrees with it well; where its
        // overlap still shows a row differently, the newer, consistent capture
        // wins. Scrollbar columns are left alone (the thumb moves).
        const int band_end = h - footer_;
        const int base = content_rows_ - band_end + shift; // content row of frame row 0
        const int cw = std::max(1, width_ - scrollbar_); // DetectScrollbar ran just above
        for (int y = std::max(top, 0); y < band_end - shift; ++y) {
            const int c = base + y;
            if (c < suspect_from_ || c >= content_rows_) continue;
            const uint32_t* src = frame.pixels.data() + static_cast<size_t>(y) * width_;
            uint32_t* dst = ContentRow(c);
            if (std::memcmp(src, dst, static_cast<size_t>(cw) * sizeof(uint32_t)) == 0) continue;
            entry.repaired.push_back({c, std::vector<uint32_t>(dst, dst + width_)});
            std::memcpy(dst, src, static_cast<size_t>(cw) * sizeof(uint32_t));
        }
        if (!entry.repaired.empty()) {
            repaired_rows_ += static_cast<int>(entry.repaired.size());
            ++epoch_;
        }
    }
    suspect_from_ = forced ? content_rows_ : -1;
    if (history_.size() > static_cast<size_t>(std::max(1, options_.history))) history_.pop_front();
    seams_.push_back(content_rows_);
    const int first = h - footer_ - shift;
    WriteContent(content_rows_, frame.pixels.data() + static_cast<size_t>(first) * width_, rows);
    content_rows_ += rows;
    last_ = std::move(frame);
    last_signatures_ = std::move(signatures);
    last_shift_ = shift;
    return {status, shift, 1.0, rows};
}

void Stitcher::DetectScrollbar(const Frame& frame, int shift, int top, int bottom) {
    // Scrollbar thumbs move against the content: columns near the right edge
    // that disagree under the content shift belong to the scrollbar.
    const int zone = std::max(guard_, std::min(width_ / 6, 32));
    const int end = frame_height_ - bottom - shift;
    const int rows = end - top;
    if (zone <= 0 || rows <= 0) return;
    // Only rows whose content (left of the zone) agrees are evidence: a row
    // changed by an overlay or animation differs in every column.
    std::vector<uint8_t> agree(static_cast<size_t>(rows));
    int agreeing = 0;
    for (int y = top; y < end; ++y) {
        const bool same = std::memcmp(frame.pixels.data() + static_cast<size_t>(y) * width_,
            last_.pixels.data() + static_cast<size_t>(y + shift) * width_, static_cast<size_t>(width_ - zone) * sizeof(uint32_t)) == 0;
        agree[static_cast<size_t>(y - top)] = same ? 1 : 0;
        agreeing += same;
    }
    if (agreeing <= 0) return;
    const int threshold = std::max(2, agreeing / 50);
    int leftmost = -1;
    int gap = 0;
    for (int x = width_ - 1; x >= width_ - zone; --x) {
        int mismatches = 0;
        for (int y = top; y < end; ++y) {
            if (!agree[static_cast<size_t>(y - top)]) continue;
            const uint32_t a = frame.pixels[static_cast<size_t>(y) * width_ + x] & 0xffffff;
            const uint32_t b = last_.pixels[static_cast<size_t>(y + shift) * width_ + x] & 0xffffff;
            mismatches += a != b;
        }
        if (mismatches > threshold) {
            leftmost = x;
            gap = 0;
        } else if (leftmost >= 0) {
            if (++gap > 2) break;
        } else if (x < width_ - 3) {
            break; // only a thin matching border is allowed at the very edge
        }
    }
    if (leftmost >= 0) scrollbar_ = std::max(scrollbar_, width_ - leftmost);
}

bool Stitcher::Undo() {
    if (history_.empty()) return false;
    History entry = std::move(history_.back());
    history_.pop_back();
    content_rows_ = entry.content_rows;
    TruncateContent(content_rows_);
    // Put back rows this segment repaired in the previous one, so the frame
    // restored from the delta and the content match exactly again.
    for (auto it = entry.repaired.rbegin(); it != entry.repaired.rend(); ++it)
        if (it->row < content_rows_) std::memcpy(ContentRow(it->row), it->pixels.data(), static_cast<size_t>(width_) * sizeof(uint32_t));
    if (!entry.repaired.empty()) {
        repaired_rows_ -= static_cast<int>(entry.repaired.size());
        ++epoch_;
    }
    suspect_from_ = entry.suspect;
    last_ = Restore(entry);
    last_signatures_ = SignRows(last_, guard_);
    header_ = entry.header;
    last_shift_ = entry.shift;
    if (!seams_.empty()) seams_.pop_back();
    pending_.reset();
    return true;
}

const uint32_t* Stitcher::Row(int y) const {
    if (y < 0 || y >= Height()) return nullptr;
    if (y < content_rows_) return ContentRow(y);
    const int fy = frame_height_ - (Height() - y);
    return last_.pixels.data() + static_cast<size_t>(fy) * width_;
}

Frame Stitcher::Compose(bool crop_scrollbar) const& {
    if (!Started()) return {};
    const int crop = crop_scrollbar ? std::min(scrollbar_, width_ - 1) : 0;
    const int w = width_ - crop, h = Height();
    Frame result = MakeFrame({0, 0, w, h});
    for (int y = 0; y < h; ++y)
        std::memcpy(result.pixels.data() + static_cast<size_t>(y) * w, Row(y), static_cast<size_t>(w) * sizeof(uint32_t));
    return result;
}

Frame Stitcher::Compose(bool crop_scrollbar) && {
    if (!Started()) return {};
    const int crop = crop_scrollbar ? std::min(scrollbar_, width_ - 1) : 0;
    const int w = width_ - crop, h = Height();
    history_.clear();
    pending_.reset();
    Frame result = MakeFrame({0, 0, w, h});
    for (int y = 0; y < h; ++y) {
        std::memcpy(result.pixels.data() + static_cast<size_t>(y) * w, Row(y), static_cast<size_t>(w) * sizeof(uint32_t));
        // A block is released as soon as its last row is copied.
        if (y < content_rows_ && (y % BlockRows == BlockRows - 1 || y == content_rows_ - 1))
            blocks_[static_cast<size_t>(y / BlockRows)].reset();
    }
    blocks_.clear();
    last_ = {};
    last_signatures_ = {};
    content_rows_ = 0;
    width_ = 0;
    return result;
}

void Stitcher::WriteContent(int y, const uint32_t* source, int rows) {
    const size_t needed = static_cast<size_t>((y + rows + BlockRows - 1) / BlockRows);
    while (blocks_.size() < needed)
        blocks_.push_back(std::make_unique_for_overwrite<uint32_t[]>(static_cast<size_t>(BlockRows) * width_));
    for (int i = 0; i < rows; ++i)
        std::memcpy(ContentRow(y + i), source + static_cast<size_t>(i) * width_, static_cast<size_t>(width_) * sizeof(uint32_t));
}

void Stitcher::TruncateContent(int rows) {
    blocks_.resize(static_cast<size_t>((std::max(rows, 0) + BlockRows - 1) / BlockRows));
}

Stitcher::History Stitcher::Delta(int previous_header) const {
    // At this point the content ends with the scrolling rows of last_: frame
    // row y sits at content row content_rows_ + y - (h - footer). Fixed header
    // rows usually repeat the top of the content (the first frame). Only the
    // columns that neither reproduces are stored.
    const int h = frame_height_, w = width_, band_end = h - std::max(footer_, 0);
    History entry{content_rows_, previous_header, last_shift_, last_.bounds, std::vector<Line>(static_cast<size_t>(h)), {}};
    const auto span = [w](const uint32_t* a, const uint32_t* b) {
        int x0 = 0, x1 = w;
        while (x0 < w && a[x0] == b[x0]) ++x0;
        if (x0 == w) return std::pair{0, 0};
        while (x1 > x0 && a[x1 - 1] == b[x1 - 1]) --x1;
        return std::pair{x0, x1};
    };
    for (int y = 0; y < h; ++y) {
        const uint32_t* row = last_.pixels.data() + static_cast<size_t>(y) * w;
        Line line{-1, 0, w};
        const int aligned = y < band_end ? content_rows_ + y - band_end : -1;
        if (aligned >= 0 && aligned < content_rows_) {
            const auto [x0, x1] = span(row, ContentRow(aligned));
            line = {aligned, x0, x1};
        }
        if (line.x1 - line.x0 > 64 && y < content_rows_ && y != aligned) {
            const auto [x0, x1] = span(row, ContentRow(y));
            if (x1 - x0 < line.x1 - line.x0) line = {y, x0, x1};
        }
        entry.lines[static_cast<size_t>(y)] = line;
        entry.patch.insert(entry.patch.end(), row + line.x0, row + line.x1);
    }
    entry.patch.shrink_to_fit();
    return entry;
}

Frame Stitcher::Restore(const History& entry) const {
    Frame frame = MakeFrame(entry.bounds);
    const int w = width_;
    size_t offset = 0;
    for (int y = 0; y < frame_height_; ++y) {
        const Line& line = entry.lines[static_cast<size_t>(y)];
        uint32_t* row = frame.pixels.data() + static_cast<size_t>(y) * w;
        if (line.ref >= 0) std::memcpy(row, ContentRow(line.ref), static_cast<size_t>(w) * sizeof(uint32_t));
        const size_t count = static_cast<size_t>(line.x1 - line.x0);
        if (count) std::memcpy(row + line.x0, entry.patch.data() + offset, count * sizeof(uint32_t));
        offset += count;
    }
    return frame;
}

size_t Stitcher::HistoryBytes() const {
    size_t bytes = 0;
    for (const History& entry : history_) {
        bytes += entry.lines.capacity() * sizeof(Line) + entry.patch.capacity() * sizeof(uint32_t);
        for (const Repair& r : entry.repaired) bytes += r.pixels.capacity() * sizeof(uint32_t);
    }
    return bytes;
}
}