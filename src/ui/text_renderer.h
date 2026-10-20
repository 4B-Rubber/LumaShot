#pragma once
#include <d2d1.h>
#include <dwrite.h>
#include <lumatext/lumatext.hpp>
#include <string_view>
#include <memory>
#include <vector>
#include "model/document.h"

namespace lumashot {
// Uses the same DirectWrite font, fallback and wrapping as annotation rendering.
void FitTextBounds(Mark& mark);
// In-place text editing geometry in the mark's unrotated space. It uses the
// exact DirectWrite format and layout box that annotation rendering uses, so
// the caret, selection and IME underline sit on the final glyphs. Number notes
// measure their detail text; Text marks measure their content box.
struct TextEditGeometry {
    Box content{};             // layout origin and limits
    Box frame{};               // editing outline before outset
    Box caret{};               // insertion point; width 0
    std::vector<Box> selection, composition;
};
TextEditGeometry MeasureTextEdit(const Mark& mark,int selection_start,int selection_end,
    int caret,int composition_start=-1,int composition_length=0);
// Nearest caret position for a canvas point, honouring rotation. Never splits
// a CR/LF pair.
int TextEditIndex(const Mark& mark,Point point);
// DirectWrite keeps layout and font fallback; the DLL rasterizes the glyphs.
// Owners on one thread share a bounded CPU glyph context; no render target
// survives a Draw call.
class TextRenderer {
public:
    lt_frame_stats Draw(ID2D1RenderTarget* target, IDWriteFactory* factory,
        std::wstring_view text, IDWriteTextFormat* format, D2D1_RECT_F bounds,
        D2D1_COLOR_F color, bool clip = true);
    lt_frame_stats DrawLayout(ID2D1RenderTarget* target, IDWriteFactory* factory,
        IDWriteTextLayout* layout, D2D1_POINT_2F origin, D2D1_RECT_F clip,
        D2D1_COLOR_F color, bool clipped = true);
private:
    struct Context;
    std::shared_ptr<Context> context_;
};
}