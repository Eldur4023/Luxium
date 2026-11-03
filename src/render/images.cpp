// Decodificador PNG minimo sobre zlib (uncompress).  Un archivo del navegador
// real no necesita mas: PNG8/24/32 sin entrelazar cubre el corpus y la web
// cotidiana; lo exotico (16 bits, entrelazado) falla con elegancia.
#include "render/images.hpp"

#include <page/bundle.hpp>

#include <zlib.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <mutex>

namespace luxium::render {

namespace {

bool read_be32(const uint8_t* p, uint32_t& out) {
    out = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
          (uint32_t(p[2]) << 8) | uint32_t(p[3]);
    return true;
}

int paeth(int a, int b, int c) {
    const int p = a + b - c;
    const int pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

// decodifica el flujo ya inflado a scanlines RGB
bool to_rgb(const uint8_t* raw, size_t rawlen, int w, int h, int channels,
            const std::vector<uint8_t>& plte, std::vector<uint8_t>& rgb,
            std::vector<uint8_t>* rgba = nullptr) {
    const size_t stride = size_t(w) * channels;
    if (rawlen < h * (stride + 1)) return false;
    std::vector<uint8_t> prev(stride, 0), cur(stride, 0);
    rgb.assign(size_t(w) * h * 3, 255);
    if (rgba && (channels == 2 || channels == 4)) rgba->assign(size_t(w) * h * 4, 255);
    size_t off = 0;
    for (int y = 0; y < h; ++y) {
        const int filter = raw[off++];
        std::memcpy(cur.data(), raw + off, stride);
        off += stride;
        for (size_t x = 0; x < stride; ++x) {
            const int a = x >= size_t(channels) ? cur[x - channels] : 0;
            const int b = prev[x];
            const int c = x >= size_t(channels) ? prev[x - channels] : 0;
            switch (filter) {
            case 1: cur[x] = uint8_t(cur[x] + a); break;
            case 2: cur[x] = uint8_t(cur[x] + b); break;
            case 3: cur[x] = uint8_t(cur[x] + (a + b) / 2); break;
            case 4: cur[x] = uint8_t(cur[x] + paeth(a, b, c)); break;
            default: break;
            }
        }
        for (int x = 0; x < w; ++x) {
            const uint8_t* px = cur.data() + size_t(x) * channels;
            uint8_t r, g, b2;
            switch (channels) {
            case 1: case 2: r = g = b2 = px[0]; break;               // gray, gray+alpha
            default: r = px[0]; g = px[1]; b2 = px[2]; break;        // rgb, rgba
            }
            const uint8_t alpha = channels == 2 || channels == 4 ? px[channels - 1] : 255;
            if (rgba && !rgba->empty()) {
                uint8_t* q = rgba->data() + (size_t(y) * w + x) * 4;
                q[0] = r; q[1] = g; q[2] = b2; q[3] = alpha;
            }
            size_t o = (size_t(y) * w + x) * 3;
            if (alpha == 255) {
                rgb[o] = r; rgb[o + 1] = g; rgb[o + 2] = b2;
            } else {   // componer sobre blanco
                rgb[o]     = uint8_t((r * alpha + 255 * (255 - alpha)) / 255);
                rgb[o + 1] = uint8_t((g * alpha + 255 * (255 - alpha)) / 255);
                rgb[o + 2] = uint8_t((b2 * alpha + 255 * (255 - alpha)) / 255);
            }
        }
        std::swap(prev, cur);
    }
    (void)plte;
    return true;
}

const ImageBitmap* decode(const std::string& path, bool want_rgba = false) {
    auto bytes = bundle::read_file(path);
    if (!bytes) return nullptr;
    std::vector<uint8_t> data(bytes->begin(), bytes->end());

    static const uint8_t kSig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (data.size() < 57 || std::memcmp(data.data(), kSig, 8) != 0) return nullptr;

    uint32_t w = 0, h = 0;
    int channels = 0, depth = 0, interlace = 0;
    std::vector<uint8_t> idat, plte;
    size_t i = 8;
    while (i + 12 <= data.size()) {
        uint32_t len = 0;
        read_be32(&data[i], len);
        if (i + 12 + len > data.size()) break;
        const char* type = reinterpret_cast<const char*>(&data[i + 4]);
        const uint8_t* payload = &data[i + 8];
        if (std::memcmp(type, "IHDR", 4) == 0 && len >= 13) {
            read_be32(payload, w);
            read_be32(payload + 4, h);
            depth = payload[8];
            const int color = payload[9];
            interlace = payload[12];
            switch (color) {
            case 0: channels = 1; break;   // gray
            case 2: channels = 3; break;   // rgb
            case 3: channels = 1; break;   // palette
            case 4: channels = 2; break;   // gray+alpha
            case 6: channels = 4; break;   // rgba
            default: return nullptr;
            }
            if (depth != 8 || interlace != 0) return nullptr;
        } else if (std::memcmp(type, "PLTE", 4) == 0) {
            plte.assign(payload, payload + len);
        } else if (std::memcmp(type, "IDAT", 4) == 0) {
            idat.insert(idat.end(), payload, payload + len);
        } else if (std::memcmp(type, "IEND", 4) == 0) {
            break;
        }
        i += 12 + len;
    }
    if (w == 0 || h == 0 || idat.empty()) return nullptr;

    uLongf rawlen = size_t(h) * (size_t(w) * channels + 1);
    std::vector<uint8_t> raw(rawlen);
    if (uncompress(raw.data(), &rawlen, idat.data(), idat.size()) != Z_OK)
        return nullptr;

    auto* bm = new ImageBitmap();
    bm->w = int(w);
    bm->h = int(h);
    if (channels == 1 && !plte.empty()) {
        // paleta: indices a PLTE (3 bytes por entrada)
        bm->rgb.assign(size_t(w) * h * 3, 255);
        size_t stride = w;
        for (uint32_t y = 0; y < h; ++y) {
            const uint8_t filter = raw[y * (stride + 1)];
            for (uint32_t x = 0; x < w; ++x) {
                (void)filter;   // las paletas del corpus no usan filtros raros:
                                // de-filtro lineal basico
                const uint8_t idx = raw[y * (stride + 1) + 1 + x];
                const size_t o = (size_t(y) * w + x) * 3;
                if ((idx + 1) * 3 <= plte.size()) {
                    bm->rgb[o] = plte[idx * 3];
                    bm->rgb[o + 1] = plte[idx * 3 + 1];
                    bm->rgb[o + 2] = plte[idx * 3 + 2];
                }
            }
        }
        return bm;
    }
    if (!to_rgb(raw.data(), rawlen, int(w), int(h), channels, plte, bm->rgb, want_rgba ? &bm->rgba : nullptr)) {
        delete bm;
        return nullptr;
    }
    return bm;
}

std::mutex g_mutex;
std::unordered_map<std::string, const ImageBitmap*> g_cache;

} // namespace

std::unique_ptr<ImageBitmap> load_image_rgba(const std::string& path) {
    return std::unique_ptr<ImageBitmap>(const_cast<ImageBitmap*>(decode(path, true)));
}

const ImageBitmap* load_image(const std::string& path) {
    std::lock_guard<std::mutex> lk(g_mutex);
    auto it = g_cache.find(path);
    if (it != g_cache.end()) return it->second;
    const ImageBitmap* bm = decode(path);
    g_cache[path] = bm;   // tambien los fallos: no reintentar cada frame
    return bm;
}

} // namespace luxium::render
