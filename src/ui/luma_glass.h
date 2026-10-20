#pragma once
#include <cstdint>

// Luma Glass design tokens shared by the toolbar, clipboard (strip, card stack,
// panel), pins and dialogs. Constants only: no Win32/D2D dependency, no logic.
// Colors are 0xRRGGBB; alpha is separate (0..1). Sizes are DIP, durations ms.
// See docs/clipboard-card-stack.md section 2.
namespace lumashot::glass {

struct Tone { uint32_t light; uint32_t dark; };
constexpr uint32_t Pick(Tone tone, bool dark) { return dark ? tone.dark : tone.light; }

// Material. Fallback is the opaque fill used when DWM acrylic is unavailable or
// transparency effects are off; Opacity is the alpha drawn over DWM acrylic.
inline constexpr Tone Fallback{0xf7f9fcu, 0x252c38u};
inline constexpr float OpacityLight = .60f, OpacityDark = .72f;
constexpr float Opacity(bool dark) { return dark ? OpacityDark : OpacityLight; }
// Opaque body of the classic liquid strip and of the clipboard list content.
inline constexpr Tone LiquidFill{0xf5f8fcu, 0x192330u};

// Outline and cards.
inline constexpr Tone Border{0xffffffu, 0x8da7c5u};
inline constexpr float BorderAlphaLight = .75f, BorderAlphaDark = .22f;
inline constexpr Tone CardFill{0xffffffu, 0xffffffu};
inline constexpr float CardAlphaLight = .58f, CardAlphaDark = .055f;
inline constexpr float CardHoverAlphaLight = .88f, CardHoverAlphaDark = .11f;

// Ink.
inline constexpr Tone Ink1{0x18212du, 0xe9eef5u};
inline constexpr Tone Ink2{0x566174u, 0xaab5c4u};
inline constexpr Tone Ink3{0x8a94a3u, 0x7c8898u};
inline constexpr uint32_t Accent = 0x1a7cffu;

// Geometry (DIP).
inline constexpr float RadiusPanel = 14.f, RadiusCard = 10.f, RadiusControl = 8.f;

// Shadow for cards and drag images.
inline constexpr float CardShadowOffsetY = 6.f, CardShadowBlur = 18.f;
inline constexpr float CardShadowAlphaLight = .16f, CardShadowAlphaDark = .35f;

// Motion. Spring values match LiquidSpring's defaults.
inline constexpr float SpringFrequency = 29.f, SpringDamping = .82f;
inline constexpr unsigned MotionFastMs = 120, MotionBaseMs = 180, MotionSettleMs = 320;

// Card stack (docs/clipboard-card-stack.md section 3; strip-concepts.html D).
// Glass tint over the HostBackdrop blur, calibrated in the 2026-10-17 spike
// against the panel's DWM transient acrylic: light dE76 0.93; dark 3.06
// (accepted, target <= 4.5). Opaque fallback uses the same tone at alpha 1.
inline constexpr Tone CardGlass{0xf7f9fcu, 0x323a48u};
inline constexpr float CardGlassAlphaLight = .84f, CardGlassAlphaDark = .72f;
constexpr float CardGlassAlpha(bool dark) { return dark ? CardGlassAlphaDark : CardGlassAlphaLight; }
// Solid fill while the stack is being moved (#dock.moving .glass).
inline constexpr Tone StackSolid{0xf5f8fcu, 0x192330u};
inline constexpr Tone GlassBorder{0xffffffu, 0x8da7c5u};
inline constexpr float GlassBorderAlphaLight = .75f, GlassBorderAlphaDark = .24f;
inline constexpr Tone Edge{0x0a192eu, 0xffffffu};
inline constexpr float EdgeAlphaLight = .10f, EdgeAlphaDark = .08f;
inline constexpr Tone StackInk{0x1b2430u, 0xe8edf4u};
inline constexpr Tone StackInk3{0x8a94a3u, 0x7b8797u};
// --shadow: 0 18px 48px a, 0 2px 6px b (light rgba(10,25,46,..), dark black).
inline constexpr Tone StackShadow{0x0a192eu, 0x000000u};
inline constexpr float StackShadowAlphaLight = .24f, StackShadowAlphaDark = .50f;
inline constexpr float StackShadowNearAlphaLight = .10f, StackShadowNearAlphaDark = .30f;

}  // namespace lumashot::glass
