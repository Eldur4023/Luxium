#pragma once
// Decodificador PNG minimo (8 bits, sin entrelazar) con cache por ruta.
// Suficiente para assets propios y la mayoria de imagenes de paginas:
// grayscale, RGB, palette, gray+alpha y RGBA; sobre blanco si trae alfa.
#include <cstdint>
#include <string>
#include <vector>

namespace luxium::render {

struct ImageBitmap {
    int w = 0, h = 0;
    std::vector<uint8_t> rgb;   // 3 bytes por pixel (RGB), fila a fila
};

// Decodifica una vez y devuelve el bitmap cacheado; null si falla.
const ImageBitmap* load_image(const std::string& path);

} // namespace luxium::render
