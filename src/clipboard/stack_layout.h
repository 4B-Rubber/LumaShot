#pragma once
// Card-stack geometry: strip-concepts.html concept D ("卡片堆叠"), in DIP.
// Pure functions only (no Win32 drawing) so every pose, hit and region can be
// unit-tested. See docs/clipboard-card-stack.md section 3.
//
// The HTML box (.D, 230 x 220) is anchored to the screen edge. Here it sits in
// a larger window so the CSS shadows are never cut: PadInner on the side away
// from the edge, PadTop / PadBottom vertically. The edge side has no padding:
// the window edge is the screen edge and also clips the 14 DIP overhang of the
// mini cards. Coordinates below are "right side"; the left side is mirrored.
#include <windows.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace lumashot::clipboard::stack {

enum class Side : unsigned char { Left, Right };

struct Pose {
    float x{}, y{}, w{}, h{};
    float rotation{};  // degrees, clockwise (CSS rotate / Composition RotationAngleInDegrees)
    float scale{1};    // about the centre (CSS transform-origin: 50% 50%)
    float opacity{1};
    float radius{};
};
struct Point { float x{}, y{}; };

// ---- HTML values (strip-concepts.html, .D rules and layoutD()).
inline constexpr int MaxCards = 5, MiniVisible = 3;
inline constexpr float BoxW = 230, BoxH = 220;
inline constexpr float MiniW = 52, MiniH = 40, MiniRadius = 7, MiniOverhang = 14;
inline constexpr float MiniTop = 70, MiniStepY = 9, MiniStepX = 7, MiniRot0 = 2, MiniRotStep = 4, MiniScaleStep = .06f;
inline constexpr float RowW = 208, RowH = 46, RowRadius = 10, RowEdgeGap = 10, RowTop = 30, RowPitch = 50, RowPad = 3;
inline constexpr float ThumbRowW = 46, ThumbH = 40, ThumbRowRadius = 7, LabelGap = 8;
inline constexpr float GripW = 30, GripH = 22, GripRadius = 8, GripTopRest = 42, GripTopFan = 2;
inline constexpr float MoreTop = RowTop + RowPitch * MaxCards + 2, MoreEdgeGap = 14, MoreW = 200, MoreH = 16;
inline constexpr float HoverShift = 4;
// Motion: .sc transition .32s cubic-bezier(.3,1.25,.5,1); .grip transition .3s
// (CSS "ease"); new card: inslide .5s cubic-bezier(.3,1.3,.5,1) from
// translateX(80px edge-ward) rotate(8deg) opacity 0.
inline constexpr unsigned MoveMs = 320, GripMs = 300, IncomingMs = 500;
inline constexpr float MoveEase[4] = {.3f, 1.25f, .5f, 1.f};
inline constexpr float GripEase[4] = {.25f, .1f, .25f, 1.f};
inline constexpr float IncomingEase[4] = {.3f, 1.3f, .5f, 1.f};
inline constexpr float IncomingShift = 80, IncomingRotation = 8;
// Plan section 3.2: hover 120 ms before fanning, 300 ms grace after leaving;
// a moved stack fades from solid back to glass in 120 ms.
inline constexpr unsigned HoverDelayMs = 120, LeaveGraceMs = 300, SolidFadeMs = 120;

// ---- Window frame around the HTML box.
inline constexpr float PadInner = 56, PadTop = 8, PadBottom = 72;
inline constexpr float ContentH = MoreTop + MoreH;  // 298: last element of the open stack
inline constexpr float WindowW = BoxW + PadInner, WindowH = PadTop + ContentH + PadBottom;

// Box coordinates (right side) -> window coordinates for `side`.
inline Pose ToWindow(Pose p, Side side) {
    p.x += PadInner;
    p.y += PadTop;
    if (side == Side::Left) { p.x = WindowW - p.x - p.w; p.rotation = -p.rotation; }
    return p;
}

// Collapsed mini card i (0 is on top). HTML: right:-14px, top 70+9i,
// translateX(7i) rotate(-(4i-2)deg) scale(1-.06i); only the first 3 visible.
inline Pose MiniPose(int i, Side side) {
    const float k = static_cast<float>(i);
    const Pose p{BoxW + MiniOverhang - MiniW + MiniStepX * k, MiniTop + MiniStepY * k, MiniW, MiniH,
                 MiniRot0 - MiniRotStep * k, 1.f - MiniScaleStep * k, i < MiniVisible ? 1.f : 0.f, MiniRadius};
    return ToWindow(p, side);
}
// Fanned row i. HTML: right:10px, top 30+50i, 208x46, radius 10; :hover translateX(-4px).
inline Pose RowPose(int i, Side side, bool hovered = false) {
    const Pose p{BoxW - RowEdgeGap - RowW - (hovered ? HoverShift : 0.f), RowTop + RowPitch * static_cast<float>(i),
                 RowW, RowH, 0, 1, 1, RowRadius};
    return ToWindow(p, side);
}
inline Pose CardPose(int i, bool fanned, Side side, bool hovered = false) {
    return fanned ? RowPose(i, side, hovered) : MiniPose(i, side);
}
// Pose a new top card starts from (Incoming).
inline Pose IncomingPose(bool fanned, Side side) {
    Pose p = CardPose(0, fanned, side);
    p.x += side == Side::Right ? IncomingShift : -IncomingShift;
    p.rotation += side == Side::Right ? IncomingRotation : -IncomingRotation;
    p.opacity = 0;
    return p;
}
// Grip: right:0, top 42 (rest) / 2 (fan), 30x22, rounded only away from the edge.
inline Pose GripPose(bool fanned, Side side) {
    return ToWindow(Pose{BoxW - GripW, fanned ? GripTopFan : GripTopRest, GripW, GripH, 0, 1, 1, GripRadius}, side);
}
// "还有 N 条" line under the open stack (right:14px).
inline Pose MorePose(Side side) {
    return ToWindow(Pose{BoxW - MoreEdgeGap - MoreW, MoreTop, MoreW, MoreH, 0, 1, 1, 0}, side);
}
// Inside a card (card-local, unrotated): thumbnail fills the mini card and
// becomes a 46x40 tile with 3 px padding in a row.
inline Pose ThumbPose(bool fanned) {
    return fanned ? Pose{RowPad, RowPad, ThumbRowW, ThumbH, 0, 1, 1, ThumbRowRadius} : Pose{0, 0, MiniW, MiniH, 0, 1, 1, 0};
}
inline Pose LabelPose() {
    constexpr float x = RowPad + ThumbRowW + LabelGap;
    return {x, RowPad, RowW - x, ThumbH, 0, 1, 1, 0};
}
// The HTML box itself in window coordinates (pointer area of the open stack).
inline Pose BoxPose(Side side) { return ToWindow(Pose{0, 0, BoxW, BoxH, 0, 1, 1, 0}, side); }

// ---- Geometry helpers.
inline std::array<Point, 4> Corners(const Pose& p) {
    const float cx = p.x + p.w / 2, cy = p.y + p.h / 2, hw = p.w * p.scale / 2, hh = p.h * p.scale / 2;
    const float r = p.rotation * 3.14159265358979f / 180.f, c = std::cos(r), s = std::sin(r);
    const float dx[4] = {-hw, hw, hw, -hw}, dy[4] = {-hh, -hh, hh, hh};
    std::array<Point, 4> out{};
    for (size_t k = 0; k < 4; ++k) out[k] = {cx + dx[k] * c - dy[k] * s, cy + dx[k] * s + dy[k] * c};
    return out;
}
inline bool Contains(const Pose& p, Point q) {
    const float cx = p.x + p.w / 2, cy = p.y + p.h / 2;
    const float r = -p.rotation * 3.14159265358979f / 180.f, c = std::cos(r), s = std::sin(r);
    const float x = (q.x - cx) * c - (q.y - cy) * s, y = (q.x - cx) * s + (q.y - cy) * c;
    return std::abs(x) <= p.w * p.scale / 2 && std::abs(y) <= p.h * p.scale / 2;
}

enum class HitKind : unsigned char { None, Card, Grip, Box };
struct Hit { HitKind kind{HitKind::None}; int index{-1}; };
inline Hit HitTest(Point q, int count, bool fanned, Side side, int hovered = -1) {
    const int n = std::clamp(count, 0, MaxCards);
    for (int i = 0; i < n; ++i) {  // 0 is drawn on top
        const Pose p = CardPose(i, fanned, side, i == hovered);
        if (p.opacity > 0 && Contains(p, q)) return {HitKind::Card, i};
    }
    if (Contains(GripPose(fanned, side), q)) return {HitKind::Grip, -1};
    if (fanned && Contains(BoxPose(side), q)) return {HitKind::Box, -1};
    return {};
}

// Input region in physical pixels relative to the window. Collapsed: only the
// visible cards and the grip catch the pointer (.D is pointer-events:none).
// Open: the whole HTML box plus every row (.D.open is pointer-events:auto).
using Polygon = std::array<POINT, 4>;
inline Polygon ToPixels(const std::array<Point, 4>& c, float scale) {
    Polygon out{};
    for (size_t k = 0; k < 4; ++k)
        out[k] = {static_cast<LONG>(std::lround(c[k].x * scale)), static_cast<LONG>(std::lround(c[k].y * scale))};
    return out;
}
inline std::vector<Polygon> InputPolygons(int count, bool fanned, Side side, float scale) {
    std::vector<Polygon> out;
    const int n = std::clamp(count, 0, MaxCards);
    for (int i = 0; i < n; ++i) {
        const Pose p = CardPose(i, fanned, side);
        if (p.opacity > 0) out.push_back(ToPixels(Corners(p), scale));
    }
    out.push_back(ToPixels(Corners(GripPose(fanned, side)), scale));
    if (fanned) out.push_back(ToPixels(Corners(BoxPose(side)), scale));
    return out;
}

// ---- Placement in physical pixels. The stack keeps the classic strip's anchor
// (top of the grip == stored top) and docks to the nearer work-area edge.
inline Side NearestSide(LONG x, RECT work) {
    return x - work.left < work.right - x ? Side::Left : Side::Right;
}
inline SIZE WindowSize(float scale) {
    return {static_cast<LONG>(std::lround(WindowW * scale)), static_cast<LONG>(std::lround(WindowH * scale))};
}
inline RECT WindowRect(Side side, LONG grip_top, RECT work, float scale) {
    const SIZE size = WindowSize(scale);
    const LONG pad_top = static_cast<LONG>(std::lround(PadTop * scale));
    const LONG grip = static_cast<LONG>(std::lround((PadTop + GripTopRest) * scale));
    const LONG content = static_cast<LONG>(std::lround((PadTop + ContentH) * scale));
    // Keep the whole open stack inside the work area; shadows may overhang.
    const LONG low = work.top - pad_top, high = std::max(low, work.bottom - content);
    const LONG top = std::clamp(grip_top - grip, low, high);
    const LONG left = side == Side::Right ? work.right - size.cx : work.left;
    return {left, top, left + size.cx, top + size.cy};
}
inline LONG GripTop(RECT window, float scale) {
    return window.top + static_cast<LONG>(std::lround((PadTop + GripTopRest) * scale));
}

}  // namespace lumashot::clipboard::stack
