#pragma once
// Software rasterizer for the display list.  Draws into an ARGB32 buffer
// (the shell's framebuffer, or a layer), translated by (ox, oy) and clipped
// to `clip`.  Everything is src-over with analytic anti-aliasing (signed
// distance coverage) for rounded shapes and shadows.

#include <paint/display_list.hpp>
#include <render/text.hpp>

#include <cstdint>
#include <span>

namespace luxium::paint {

struct IRect { int x0 = 0, y0 = 0, x1 = 0, y1 = 0; };

struct Canvas {
    std::span<uint32_t> px;
    int w = 0, h = 0;          // buffer size
    float ox = 0, oy = 0;      // doc -> buffer translation
    IRect clip;                // buffer-space clip (page area)
    float scale = 1.f;         // HiDPI: the list is in logical px; it is drawn k times larger (ox/oy/clip are already buffer px)
};

// Paints `dl` over what is already in the canvas.
void rasterize(const DisplayList& dl, Canvas& canvas, const render::Font& font);

} // namespace luxium::paint
