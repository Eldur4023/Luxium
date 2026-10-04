#include "render/text.hpp"

#include <ft2build.h>
#include FT_FREETYPE_H
#include <hb.h>
#include <hb-ft.h>

#include <span>

#include <cstdio>
#include <cstring>

namespace luxium::render {

struct Font::Face {
    FT_Face      ft = nullptr;
    hb_font_t*   hb = nullptr;
    hb_buffer_t* buf = nullptr;   // reused per shape call (single-threaded UI)
    ~Face() {
        if (hb) hb_font_destroy(hb);
        if (buf) hb_buffer_destroy(buf);
        if (ft) FT_Done_Face(ft);
    }
};

static FT_Library ft_lib = nullptr;   // process-wide, fine per FreeType docs

std::unique_ptr<Font> Font::load(const std::string& regular, const std::string& bold,
                                 const std::string& mono) {
    if (!ft_lib && FT_Init_FreeType(&ft_lib)) return nullptr;
    auto font = std::unique_ptr<Font>(new Font());
    auto load_face = [&](std::unique_ptr<Face>& slot, const std::string& path) {
        auto f = std::make_unique<Face>();
        if (FT_New_Face(ft_lib, path.c_str(), 0, &f->ft)) return false;
        f->hb  = hb_ft_font_create(f->ft, nullptr);
        f->buf = hb_buffer_create();
        slot = std::move(f);
        return true;
    };
    if (!load_face(font->regular_, regular) || !load_face(font->bold_, bold) ||
        !load_face(font->mono_, mono))
        return nullptr;
    return font;
}

Font::~Font() = default;

Font::Face* Font::face(bool bold, bool mono) const {
    return mono ? mono_.get() : (bold ? bold_.get() : regular_.get());
}

void Font::set_size(float px) {
    size_ = px;
    for (Face* f : {regular_.get(), bold_.get(), mono_.get()})
        FT_Set_Pixel_Sizes(f->ft, 0, static_cast<FT_UInt>(px));
}

float Font::ascent() const {
    return static_cast<float>(face(false, false)->ft->size->metrics.ascender) / 64.f;
}

float Font::line_height() const {
    FT_Face f = face(false, false)->ft;
    return static_cast<float>(f->size->metrics.ascender + -f->size->metrics.descender) / 64.f;
}

ShapedRun Font::shape(std::string_view utf8, bool bold, bool mono) const {
    Face* f = face(bold, mono);
    hb_buffer_reset(f->buf);
    hb_buffer_add_utf8(f->buf, utf8.data(), static_cast<int>(utf8.size()), 0, -1);
    hb_buffer_set_direction(f->buf, HB_DIRECTION_LTR);
    hb_buffer_set_script(f->buf, HB_SCRIPT_LATIN);
    hb_buffer_set_language(f->buf, hb_language_from_string("es", -1));
    hb_shape(f->hb, f->buf, nullptr, 0);

    unsigned         count = 0;
    hb_glyph_info_t* info = hb_buffer_get_glyph_infos(f->buf, &count);
    hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(f->buf, &count);

    ShapedRun run;
    run.positions.reserve(count);
    float x = 0, y = 0;
    for (unsigned i = 0; i < count; ++i) {
        run.positions.push_back({info[i].codepoint,
                                 x + pos[i].x_offset / 64.f,
                                 y + pos[i].y_offset / 64.f,
                                 info[i].cluster});
        x += pos[i].x_advance / 64.f;
    }
    run.width = x;
    return run;
}

const Glyph& Font::glyph(uint32_t glyph_index, bool bold, bool mono) const {
    const uint64_t key = (static_cast<uint64_t>(glyph_index) << 2) | (bold ? 2 : 0) | (mono ? 1 : 0);
    auto it = cache_.find(key);
    if (it != cache_.end()) return it->second;

    Face* f = face(bold, mono);
    Glyph g;
    if (!FT_Load_Glyph(f->ft, glyph_index, FT_LOAD_DEFAULT) &&
        !FT_Render_Glyph(f->ft->glyph, FT_RENDER_MODE_NORMAL)) {
        FT_Bitmap& bm = f->ft->glyph->bitmap;
        g.width    = static_cast<int>(bm.width);
        g.height   = static_cast<int>(bm.rows);
        g.bearing_x = static_cast<int>(f->ft->glyph->bitmap_left);
        g.bearing_y = static_cast<int>(f->ft->glyph->bitmap_top);
        g.alpha.resize(static_cast<size_t>(g.width) * g.height);
        for (int r = 0; r < g.height; ++r)
            std::memcpy(g.alpha.data() + static_cast<size_t>(r) * g.width,
                        bm.buffer + static_cast<size_t>(r) * bm.pitch,
                        static_cast<size_t>(g.width));
    }
    return cache_.emplace(key, std::move(g)).first->second;
}

std::vector<Word> shape_words(const Font& font, std::string_view utf8, bool bold, bool mono) {
    // HTML white space: a newline or a tab inside a run of text is a space
    // (explicit breaks are <br>'s job).  Left in place, the bytes reach
    // HarfBuzz and come back as .notdef boxes on screen.
    std::string flat;
    flat.reserve(utf8.size());
    for (char c : utf8) flat.push_back(c == '\n' || c == '\r' || c == '\t' ? ' ' : c);

    std::vector<Word> out;
    size_t i = 0;
    while (i < flat.size()) {
        while (i < flat.size() && flat[i] == ' ') ++i;
        size_t start = i;
        while (i < flat.size() && flat[i] != ' ') ++i;
        if (i == start) break;
        Word w;
        w.text   = flat.substr(start, i - start);
        w.shaped = font.shape(w.text, bold, mono);
        w.width  = w.shaped.width;
        w.bold   = bold;
        w.mono   = mono;
        out.push_back(std::move(w));
    }
    return out;
}

void draw_word(std::span<uint32_t> fb, int fb_w, int fb_h,
               const Font& font, const Word& w, float x, float y, uint32_t argb) {
    const uint32_t rgb = argb & 0x00FFFFFF;
    const float ascent = font.ascent();   // line-box top -> baseline

    for (const auto& p : w.shaped.positions) {
        const Glyph& g = font.glyph(p.glyph_index, w.bold, w.mono);
        const int gx = static_cast<int>(x + p.x) + g.bearing_x;
        const int gy = static_cast<int>(y + ascent) - g.bearing_y;
        for (int r = 0; r < g.height; ++r) {
            const int fy = gy + r;
            if (fy < 0 || fy >= fb_h) continue;
            for (int c = 0; c < g.width; ++c) {
                const int fx = gx + c;
                if (fx < 0 || fx >= fb_w) continue;
                const uint8_t a = g.alpha[static_cast<size_t>(r) * g.width + c];
                if (!a) continue;
                uint32_t& dst = fb[static_cast<size_t>(fy) * fb_w + fx];
                // src-over with the glyph coverage; the framebuffer is opaque.
                const uint32_t dr = dst & 0xFF, dg = (dst >> 8) & 0xFF, db = (dst >> 16) & 0xFF;
                const uint32_t sr = rgb & 0xFF, sg = (rgb >> 8) & 0xFF, sb = (rgb >> 16) & 0xFF;
                const uint32_t ia = 255 - a;
                dst = 0xFF000000u |
                      ((sr * a + dr * ia) / 255) |
                      (((sg * a + dg * ia) / 255) << 8) |
                      (((sb * a + db * ia) / 255) << 16);
            }
        }
    }
}

} // namespace luxium::render
