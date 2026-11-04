#pragma once
// Decodificador PNG minimo (8 bits, sin entrelazar) con cache por ruta.
// Suficiente para assets propios y la mayoria de imagenes de paginas:
// grayscale, RGB, palette, gray+alpha y RGBA; sobre blanco si trae alfa.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace luxium::render {

namespace svg { struct Doc; }

struct ImageBitmap {
    int w = 0, h = 0;
    std::vector<uint8_t> rgb;   // 3 bytes por pixel (RGB), fila a fila
    std::vector<uint8_t> rgba;  // 4 bytes por pixel, alfa recta: solo si la imagen trae alfa (PNG/WebP/SVG)
    // SVG: the bitmap is the rasterisation at the intrinsic size (rgba always set); `svg` re-rasterises at any size
    std::shared_ptr<const svg::Doc> svg;
};

// A vector image drawn at w x h pixels (cached), or null when `bm` is not an SVG.
std::shared_ptr<const ImageBitmap> image_at(const ImageBitmap& bm, int w, int h);

// A page's image reference -> what load_image() takes: an absolute file path, or an http(s) URL when the
// page itself came over http ("/api/x" resolves against its origin, "x" against its directory).
std::string resolve_image(const std::string& base_dir, const std::string& src);

// Bumped whenever a network image finishes (success or failure): the shell relayouts when it changes.
uint64_t image_generation();

// Decodifica una vez y devuelve el bitmap cacheado; null si falla.
const ImageBitmap* load_image(const std::string& path);

// Igual, pero sin cache y con `rgba` relleno si el PNG trae alfa (iconos de ventana). Null si falla.
std::unique_ptr<ImageBitmap> load_image_rgba(const std::string& path);

} // namespace luxium::render
