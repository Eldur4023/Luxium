#include "render/text.hpp"

#include <ft2build.h>
#include FT_FREETYPE_H
#include <hb.h>
#include <hb-ft.h>
#include <fontconfig/fontconfig.h>
#include FT_SFNT_NAMES_H
#include FT_TRUETYPE_TABLES_H
#include FT_OUTLINE_H
#include FT_SYNTHESIS_H
#include FT_LCD_FILTER_H

#include <span>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <unordered_map>

namespace luxium::render {

struct Font::Face {
    bool         fake_bold = false, fake_italic = false;   // synthesized when the file lacks the style
    int          req_weight = 400;
    bool         req_italic = false;
    FT_Face      ft = nullptr;
    bool         color = false;     // bitmap-strike colour font (CBDT/sbix: Noto Color Emoji): drawn by scaling the strike
    float        strike_ppem = 0;   // the strike's em in pixels, and the scale that makes its rounded advances exact
    float        adv_fix = 1;
    hb_font_t*   hb = nullptr;
    hb_buffer_t* buf = nullptr;   // reused per shape call (single-threaded UI)
    // Opens a colour bitmap font on its first strike; the HarfBuzz font then takes advances from the font's own
    // metrics (OpenType funcs), scaled like Blink does: the strike's integer advance over its ppem.
    void init_color() {
        color = FT_HAS_COLOR(ft) && FT_HAS_FIXED_SIZES(ft) && !FT_IS_SCALABLE(ft) && ft->num_fixed_sizes > 0;
        if (!color) return;
        FT_Select_Size(ft, 0);
        strike_ppem = float(ft->available_sizes[0].y_ppem) / 64.f;
        if (strike_ppem <= 0) strike_ppem = float(ft->available_sizes[0].height);
        if (hb) hb_font_destroy(hb);
        hb = hb_font_create(hb_ft_face_create_referenced(ft));
        const float adv = float(ft->max_advance_width) * strike_ppem / float(ft->units_per_EM);   // all emoji share one advance
        if (adv > 0) adv_fix = std::round(adv) / adv;
    }
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

std::unique_ptr<Font> Font::load_dejavu(const std::string& dir) {
    if (!ft_lib && FT_Init_FreeType(&ft_lib)) return nullptr;
    auto font = std::unique_ptr<Font>(new Font());
    static const char* names[3][2][2] = {
        {{"DejaVuSans.ttf", "DejaVuSans-Oblique.ttf"}, {"DejaVuSans-Bold.ttf", "DejaVuSans-BoldOblique.ttf"}},
        {{"DejaVuSerif.ttf", "DejaVuSerif-Italic.ttf"}, {"DejaVuSerif-Bold.ttf", "DejaVuSerif-BoldItalic.ttf"}},
        {{"DejaVuSansMono.ttf", "DejaVuSansMono-Oblique.ttf"}, {"DejaVuSansMono-Bold.ttf", "DejaVuSansMono-BoldOblique.ttf"}}};
    for (int f = 0; f < 3; ++f)
        for (int b = 0; b < 2; ++b)
            for (int i = 0; i < 2; ++i) {
                auto face = std::make_unique<Face>();
                if (FT_New_Face(ft_lib, (dir + "/" + names[f][b][i]).c_str(), 0, &face->ft)) continue;
                face->hb  = hb_ft_font_create(face->ft, nullptr);
                face->buf = hb_buffer_create();
                font->var_[f][b][i] = std::move(face);
            }
    if (!font->var_[0][0][0]) return nullptr;
    // legacy API (chrome text) keeps working on the same set
    return font;
}

Font::~Font() = default;

Font::Face* Font::face(FaceKey k) const {
    if (k.id >= 0 && k.id < static_cast<int>(dyn_.size()) && dyn_[static_cast<size_t>(k.id)])
        return dyn_[static_cast<size_t>(k.id)].get();
    if (var_[0][0][0]) return var_[0][0][0].get();
    return regular_.get();
}

void Font::use_size(Face* f, float px) const {
    if (f->color) {   // fixed strike: only the HarfBuzz scale follows the size
        const int q = static_cast<int>(std::lround(px * 64 * f->adv_fix));
        hb_font_set_scale(f->hb, q, q);
        return;
    }
    // cached per face: FT_Set_Char_Size + hb_ft_font_changed only on change
    static thread_local std::unordered_map<Face*, int> cur;
    const int q = static_cast<int>(std::lround(px * 64));
    auto it = cur.find(f);
    if (it != cur.end() && it->second == q) return;
    cur[f] = q;
    FT_Set_Char_Size(f->ft, q, q, 0, 0);
    hb_ft_font_changed(f->hb);
}

FaceMetrics Font::metrics(FaceKey key, float px) const {
    Face* f = face(key);
    FaceMetrics m;
    if (!f) return m;
    const float upem = static_cast<float>(f->ft->units_per_EM);
    float asc_u = static_cast<float>(f->ft->ascender), desc_u = static_cast<float>(-f->ft->descender);
    float height_u = static_cast<float>(f->ft->height);
    if (f->color)   // bitmap-strike fonts: the face fields may be in strike pixels; the hhea table is in font units
        if (auto* hh = static_cast<TT_HoriHeader*>(FT_Get_Sfnt_Table(f->ft, ft_sfnt_hhea))) {
            asc_u = float(hh->Ascender); desc_u = float(-hh->Descender); height_u = asc_u + desc_u + float(hh->Line_Gap);
        }
    m.ascent  = std::round(asc_u * px / upem);
    m.descent = std::round(desc_u * px / upem);
    const float gap = height_u - (asc_u + desc_u);
    m.line_gap = std::round(std::max(0.f, gap) * px / upem);
    float xh = 0.5f;
    if (auto* os2 = static_cast<TT_OS2*>(FT_Get_Sfnt_Table(f->ft, ft_sfnt_os2)))
        if (os2->version >= 2 && os2->sxHeight > 0) xh = static_cast<float>(os2->sxHeight) / upem;
    m.x_height = xh * px;
    if (auto* os2 = static_cast<TT_OS2*>(FT_Get_Sfnt_Table(f->ft, ft_sfnt_os2)))
        m.avg_char = static_cast<float>(os2->xAvgCharWidth) * px / upem;
    m.max_char = std::round(static_cast<float>(f->ft->bbox.xMax - f->ft->bbox.xMin) * px / upem);
    return m;
}

ShapedRun Font::shape_face(Face* f, std::string_view utf8, float px) const {
    use_size(f, px);
    hb_buffer_reset(f->buf);
    hb_buffer_add_utf8(f->buf, utf8.data(), static_cast<int>(utf8.size()), 0, -1);
    hb_buffer_set_direction(f->buf, HB_DIRECTION_LTR);
    hb_buffer_guess_segment_properties(f->buf);
    hb_shape(f->hb, f->buf, nullptr, 0);
    unsigned count = 0;
    hb_glyph_info_t* info = hb_buffer_get_glyph_infos(f->buf, &count);
    hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(f->buf, &count);
    ShapedRun run;
    run.positions.reserve(count);
    float x = 0, y = 0;
    for (unsigned i = 0; i < count; ++i) {
        run.positions.push_back({info[i].codepoint, x + pos[i].x_offset / 64.f, y + pos[i].y_offset / 64.f, info[i].cluster,
                                 pos[i].x_advance / 64.f, x});
        x += pos[i].x_advance / 64.f;
    }
    run.width = x;
    return run;
}

int Font::emoji_face() const {
    if (emoji_face_ != -2) return emoji_face_;
    emoji_face_ = -1;
    const int id = match_family("Noto Color Emoji", true, 400, false);
    if (id >= 0 && face(FaceKey{id})->color) emoji_face_ = id;
    return emoji_face_;
}

ShapedRun Font::shape_at(std::string_view utf8, FaceKey key, float px) const {
    Face* f = face(key);
    ShapedRun run = shape_face(f, utf8, px);
    auto decode = [&](size_t& i) {
        const unsigned char c = static_cast<unsigned char>(utf8[i]);
        if (c < 0x80) { ++i; return uint32_t(c); }
        int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
        uint32_t cp = c & (0x3F >> n);
        ++i;
        while (n-- > 0 && i < utf8.size() && (static_cast<unsigned char>(utf8[i]) & 0xC0) == 0x80) cp = (cp << 6) | (utf8[i++] & 0x3F);
        return cp;
    };
    auto is_ri = [](uint32_t cp) { return cp >= 0x1F1E6 && cp <= 0x1F1FF; };
    auto is_modifier = [](uint32_t cp) { return cp >= 0x1F3FB && cp <= 0x1F3FF; };
    auto is_extend = [&](uint32_t cp) {
        return cp == 0xFE0E || cp == 0xFE0F || cp == 0x200D || cp == 0x20E3 || is_modifier(cp) || (cp >= 0xE0020 && cp <= 0xE007F) ||
               (cp >= 0x300 && cp <= 0x36F) || (cp >= 0x20D0 && cp <= 0x20FF) || (cp >= 0xFE20 && cp <= 0xFE2F);
    };
    bool missing = false;
    for (const auto& p : run.positions) if (p.glyph_index == 0) { missing = true; break; }
    bool sequence = false;   // emoji sequences need their own face even when the primary has every code point
    for (size_t i = 0; i < utf8.size() && !missing && !sequence;) {
        const uint32_t cp = decode(i);
        sequence = cp == 0xFE0F || cp == 0x20E3 || cp == 0x200D || is_ri(cp) || is_modifier(cp);
    }
    if (!missing && !sequence) return run;

    // Cut the text into clusters (a base plus what extends it: variation selectors, ZWJ chains, skin tones, keycaps,
    // tags, flags), pick the face per cluster (colour emoji for emoji sequences, else the first face with the base
    // code point) and shape each maximal piece with its own face so ligatures and ZWJ sequences form.
    struct Seg { size_t b0, b1; int face; };   // face -1: primary
    std::vector<Seg> segs;
    for (size_t i = 0; i < utf8.size();) {
        const size_t at = i;
        const uint32_t base = decode(i);
        bool emoji_seq = false, text_pres = false;
        size_t nri = is_ri(base) ? 1 : 0;
        for (;;) {
            if (i >= utf8.size()) break;
            size_t j = i;
            const uint32_t cp = decode(j);
            if (cp == 0x200D) {   // ZWJ joins the next code point (and whatever extends it) to this cluster
                emoji_seq = true;
                i = j;
                if (i < utf8.size()) decode(i);
                continue;
            }
            if (is_ri(cp) && nri == 1) { ++nri; emoji_seq = true; i = j; continue; }
            if (!is_extend(cp)) break;
            if (cp == 0xFE0F || cp == 0x20E3 || is_modifier(cp)) emoji_seq = true;
            if (cp == 0xFE0E) text_pres = true;
            i = j;
        }
        int fid = -1;
        const bool control = base < 0x20 || base == 0x200B || base == 0x200C || base == 0x200D || base == 0xFEFF || base == 0xAD;
        if (emoji_seq && !text_pres && emoji_face() >= 0) fid = emoji_face();
        else if (!control && FT_Get_Char_Index(f->ft, base) == 0) fid = fallback_for(base, key);
        if (fid == key.id) fid = -1;
        if (!segs.empty() && segs.back().face == fid) segs.back().b1 = i; else segs.push_back({at, i, fid});
    }
    ShapedRun out;
    float x = 0;
    for (const Seg& sg : segs) {
        Face* sf = sg.face >= 0 ? face(FaceKey{sg.face}) : f;
        ShapedRun sub = shape_face(sf, utf8.substr(sg.b0, sg.b1 - sg.b0), px);
        for (auto p : sub.positions) {
            p.x += x; p.pen += x;
            p.cluster += static_cast<uint32_t>(sg.b0);
            if (sg.face >= 0) p.face = sg.face;
            out.positions.push_back(p);
        }
        x += sub.width;
    }
    out.width = x;
    return out;
}

// A colour bitmap glyph at `px`: the strike scaled down with a box filter (premultiplied).
const Glyph& Font::color_glyph(Face* f, uint32_t gi, float px, uint64_t key) const {
    Glyph g;
    if (!FT_Load_Glyph(f->ft, gi, FT_LOAD_COLOR | FT_LOAD_NO_HINTING) && f->ft->glyph->format == FT_GLYPH_FORMAT_BITMAP &&
        f->ft->glyph->bitmap.pixel_mode == FT_PIXEL_MODE_BGRA) {
        const FT_Bitmap& bm = f->ft->glyph->bitmap;
        const int bw = int(bm.width), bh = int(bm.rows), left = f->ft->glyph->bitmap_left, top = f->ft->glyph->bitmap_top;
        const float sc = px / f->strike_ppem;
        const int dx0 = int(std::floor(float(left) * sc)), dx1 = int(std::ceil(float(left + bw) * sc));
        const int dy_top = int(std::ceil(float(top) * sc)), dy_bot = int(std::floor(float(top - bh) * sc));
        const int dw = dx1 - dx0, dh = dy_top - dy_bot;
        if (dw > 0 && dh > 0 && dw < 4096 && dh < 4096) {
            g.width = dw; g.height = dh; g.bearing_x = dx0; g.bearing_y = dy_top;
            g.color.assign(size_t(dw) * dh * 4, 0);
            g.alpha.assign(size_t(dw) * dh, 0);
            // per-axis box weights: dest pixel i covers source [lo, hi) in bitmap pixels
            auto weights = [&](int n, int origin, bool flip, int src_n) {
                struct W { int s0; std::vector<float> w; };
                std::vector<W> out; out.resize(size_t(n));
                for (int i = 0; i < n; ++i) {
                    float lo, hi;   // in scaled space [a, a+1) -> source space
                    if (!flip) { lo = float(origin + i) / sc - float(left); hi = float(origin + i + 1) / sc - float(left); }
                    else { hi = float(top) - float(origin - i - 1) / sc; lo = float(top) - float(origin - i) / sc; }
                    const int s0 = std::max(0, int(std::floor(lo))), s1 = std::min(src_n, int(std::ceil(hi)));
                    W w; w.s0 = s0;
                    const float area = hi - lo;
                    for (int s = s0; s < s1; ++s) w.w.push_back((std::min(hi, float(s + 1)) - std::max(lo, float(s))) / area);
                    out[size_t(i)] = std::move(w);
                }
                return out;
            };
            const auto wx = weights(dw, dx0, false, bw), wy = weights(dh, dy_top, true, bh);
            for (int j = 0; j < dh; ++j) {
                for (int i = 0; i < dw; ++i) {
                    float acc[4] = {0, 0, 0, 0};
                    for (size_t yy = 0; yy < wy[size_t(j)].w.size(); ++yy) {
                        const uint8_t* row = bm.buffer + size_t(wy[size_t(j)].s0 + int(yy)) * size_t(bm.pitch);
                        for (size_t xx = 0; xx < wx[size_t(i)].w.size(); ++xx) {
                            const uint8_t* px4 = row + size_t(wx[size_t(i)].s0 + int(xx)) * 4;
                            const float k = wy[size_t(j)].w[yy] * wx[size_t(i)].w[xx];
                            acc[0] += k * px4[2]; acc[1] += k * px4[1]; acc[2] += k * px4[0]; acc[3] += k * px4[3];   // BGRA -> RGBA
                        }
                    }
                    uint8_t* q = &g.color[(size_t(j) * dw + i) * 4];
                    for (int c = 0; c < 4; ++c) q[c] = uint8_t(std::clamp(acc[c] + 0.5f, 0.f, 255.f));
                    g.alpha[size_t(j) * dw + i] = q[3];
                }
            }
        }
    }
    return cache_.emplace(key, std::move(g)).first->second;
}

const Glyph& Font::glyph_at(uint32_t glyph_index, FaceKey key, float px, int subx) const {
    Face* f = face(key);
    const uint64_t k = (static_cast<uint64_t>(glyph_index) << 40) |
                       (static_cast<uint64_t>(static_cast<uint32_t>(px * 4.f)) << 16) |
                       (static_cast<uint64_t>(subx & 3) << 14) | 0x2000u | static_cast<uint64_t>(key.id & 0x1FFF);
    auto it = cache_.find(k);
    if (it != cache_.end()) return it->second;
    use_size(f, px);
    if (f->color) return color_glyph(f, glyph_index, px, k);
    Glyph g;
    // Chromium on Linux: light (vertical-only) hinting, LCD sub-pixel AA.  The
    // glyph is rendered 3x wider, then filtered with FreeType's default 5-tap FIR
    // across the sub-pixels (the filter ClearType-style builds use).
    if (!FT_Load_Glyph(f->ft, glyph_index, FT_LOAD_TARGET_LIGHT)) {
        if (f->fake_bold) FT_GlyphSlot_Embolden(f->ft->glyph);
        if (f->fake_italic) FT_GlyphSlot_Oblique(f->ft->glyph);
        if (f->ft->glyph->format == FT_GLYPH_FORMAT_OUTLINE) {
            FT_Matrix m = {3 * 65536, 0, 0, 65536};
            FT_Outline_Transform(&f->ft->glyph->outline, &m);
            FT_Outline_Translate(&f->ft->glyph->outline, subx * 48, 0);   // subx/4 px = subx*0.75 sub-pixels
            if (!FT_Render_Glyph(f->ft->glyph, FT_RENDER_MODE_NORMAL)) {
                FT_Bitmap& bm = f->ft->glyph->bitmap;
                const int wb = static_cast<int>(bm.width), rows = static_cast<int>(bm.rows);
                const int s0 = f->ft->glyph->bitmap_left - 2;            // first filtered sub-pixel
                const int wp = wb + 4;
                auto floordiv = [](int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); };
                const int px0 = floordiv(s0, 3);
                const int n = (s0 + wp + 2) / 3 - px0 + 1;
                static const int kFir[5] = {0x08, 0x4D, 0x56, 0x4D, 0x08};
                g.width = n; g.height = rows;
                g.bearing_x = px0;
                g.bearing_y = static_cast<int>(f->ft->glyph->bitmap_top);
                g.lcd.assign(static_cast<size_t>(n) * 3 * rows, 0);
                g.alpha.assign(static_cast<size_t>(n) * rows, 0);
                std::vector<int> row(static_cast<size_t>(wp), 0);
                for (int r = 0; r < rows; ++r) {
                    const uint8_t* src = bm.buffer + static_cast<size_t>(r) * bm.pitch;
                    for (int i = 0; i < wp; ++i) {
                        int acc = 0;
                        for (int j = 0; j < 5; ++j) {
                            const int si = i + j - 4;   // padded index of the bitmap column
                            if (si >= 0 && si < wb) acc += kFir[j] * src[si];
                        }
                        row[static_cast<size_t>(i)] = std::min(255, acc >> 8);
                    }
                    for (int p = 0; p < n; ++p) {
                        int sum = 0;
                        for (int ch = 0; ch < 3; ++ch) {
                            const int idx = 3 * (px0 + p) + ch - s0;
                            const int v = (idx >= 0 && idx < wp) ? row[static_cast<size_t>(idx)] : 0;
                            g.lcd[(static_cast<size_t>(r) * n + p) * 3 + ch] = static_cast<uint8_t>(v);
                            sum += v;
                        }
                        g.alpha[static_cast<size_t>(r) * n + p] = static_cast<uint8_t>(sum / 3);
                    }
                }
            }
        }
    }
    return cache_.emplace(k, std::move(g)).first->second;
}

// ── fontconfig: CSS font-family -> an installed face ─────────────────────────

static std::string lc(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Skia's metric-compatible families (SkFontConfigInterface_direct.cpp).
static int equiv_class(const std::string& f) {
    static const std::pair<const char*, int> table[] = {
        {"arial", 1}, {"arimo", 1}, {"liberation sans", 1}, {"helvetica", 1},
        {"times new roman", 2}, {"tinos", 2}, {"liberation serif", 2},
        {"courier new", 3}, {"cousine", 3}, {"liberation mono", 3},
        {"cambria", 4}, {"caladea", 4}, {"calibri", 5}, {"carlito", 5},
        {"symbol", 6}, {"symbol neu", 6},
    };
    for (const auto& [n, c] : table) if (f == n) return c;
    return 0;
}

int Font::face_from_match(void* fc_pattern, int weight, bool italic) const {
    FcPattern* m = static_cast<FcPattern*>(fc_pattern);
    FcChar8* file = nullptr; int idx = 0; int fw = FC_WEIGHT_REGULAR, fs = FC_SLANT_ROMAN;
    FcPatternGetString(m, FC_FILE, 0, &file);
    FcPatternGetInteger(m, FC_INDEX, 0, &idx);
    FcPatternGetInteger(m, FC_WEIGHT, 0, &fw);
    FcPatternGetInteger(m, FC_SLANT, 0, &fs);
    if (!file) return -1;
    const bool fake_b = weight >= 600 && fw < FC_WEIGHT_DEMIBOLD;
    const bool fake_i = italic && fs == FC_SLANT_ROMAN;
    const std::string fkey = std::string(reinterpret_cast<const char*>(file)) + "#" + std::to_string(idx) +
                             (fake_b ? "b" : "") + (fake_i ? "i" : "");
    auto it = file_ids_.find(fkey);
    if (it != file_ids_.end()) return it->second;
    auto face = std::make_unique<Face>();
    if (FT_New_Face(ft_lib, reinterpret_cast<const char*>(file), idx, &face->ft)) return -1;
    face->hb = hb_ft_font_create(face->ft, nullptr);
    face->buf = hb_buffer_create();
    face->init_color();
    face->fake_bold = fake_b; face->fake_italic = fake_i;
    face->req_weight = weight; face->req_italic = italic;
    dyn_.push_back(std::move(face));
    const int id = static_cast<int>(dyn_.size()) - 1;
    file_ids_[fkey] = id;
    return id;
}

int Font::fallback_for(uint32_t cp, FaceKey base) const {
    const uint64_t k = (static_cast<uint64_t>(cp) << 20) | static_cast<uint64_t>(base.id & 0xFFFFF);
    auto it = fallback_cache_.find(k);
    if (it != fallback_cache_.end()) return it->second;
    static bool inited = false;
    if (!inited) inited = FcInit();
    Face* bf = face(base);
    const int weight = bf ? bf->req_weight : 400;
    const bool italic = bf ? bf->req_italic : false;
    FcPattern* pat = FcPatternCreate();
    FcCharSet* cs = FcCharSetCreate();
    FcCharSetAddChar(cs, cp);
    FcPatternAddCharSet(pat, FC_CHARSET, cs);
    FcPatternAddInteger(pat, FC_WEIGHT, FcWeightFromOpenType(weight));
    FcPatternAddInteger(pat, FC_SLANT, italic ? FC_SLANT_ITALIC : FC_SLANT_ROMAN);
    FcPatternAddBool(pat, FC_SCALABLE, FcTrue);
    FcConfigSubstitute(nullptr, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);
    FcResult res;
    FcFontSet* set = FcFontSort(nullptr, pat, FcTrue, nullptr, &res);
    int id = -1;
    if (set) {
        for (int i = 0; i < set->nfont && id < 0; ++i) {
            FcCharSet* fcs = nullptr;
            if (FcPatternGetCharSet(set->fonts[i], FC_CHARSET, 0, &fcs) != FcResultMatch || !FcCharSetHasChar(fcs, cp)) continue;
            FcPattern* m = FcFontRenderPrepare(nullptr, pat, set->fonts[i]);
            id = face_from_match(m, weight, italic);
            FcPatternDestroy(m);
        }
        FcFontSetDestroy(set);
    }
    FcCharSetDestroy(cs);
    FcPatternDestroy(pat);
    fallback_cache_[k] = id;
    return id;
}

int Font::match_family(const std::string& name, bool accept_any, int weight, bool italic) const {
    static bool inited = false;
    if (!inited) { inited = FcInit(); }
    FcPattern* pat = FcPatternCreate();
    FcPatternAddString(pat, FC_FAMILY, reinterpret_cast<const FcChar8*>(name.c_str()));
    FcPatternAddInteger(pat, FC_WEIGHT, FcWeightFromOpenType(weight));
    FcPatternAddInteger(pat, FC_SLANT, italic ? FC_SLANT_ITALIC : FC_SLANT_ROMAN);
    FcPatternAddBool(pat, FC_SCALABLE, FcTrue);
    FcConfigSubstitute(nullptr, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);
    FcResult res;
    // generics take whatever fontconfig prefers; named families only an equivalent one
    // (the first acceptable font in fontconfig's own preference order)
    FcPattern* m = nullptr;
    FcFontSet* sorted = nullptr;
    if (accept_any) {
        m = FcFontMatch(nullptr, pat, &res);
    } else {
        sorted = FcFontSort(nullptr, pat, FcTrue, nullptr, &res);
        if (sorted) {
            const std::string want = lc(name);
            for (int i = 0; i < sorted->nfont; ++i) {
                FcChar8* fam = nullptr;
                if (FcPatternGetString(sorted->fonts[i], FC_FAMILY, 0, &fam) != FcResultMatch) continue;
                const std::string got = lc(reinterpret_cast<const char*>(fam));
                if (want == got || (equiv_class(want) != 0 && equiv_class(want) == equiv_class(got))) {
                    m = FcFontRenderPrepare(nullptr, pat, sorted->fonts[i]);
                    break;
                }
            }
        }
    }
    int id = -1;
    if (m) {
        FcChar8* file = nullptr; int idx = 0; int fw = FC_WEIGHT_REGULAR, fs = FC_SLANT_ROMAN;
        FcPatternGetString(m, FC_FILE, 0, &file);
        FcPatternGetInteger(m, FC_INDEX, 0, &idx);
        FcPatternGetInteger(m, FC_WEIGHT, 0, &fw);
        FcPatternGetInteger(m, FC_SLANT, 0, &fs);
        id = face_from_match(m, weight, italic);
        FcPatternDestroy(m);
    }
    if (sorted) FcFontSetDestroy(sorted);
    FcPatternDestroy(pat);
    return id;
}

FaceKey Font::resolve(const std::string& family_list, int weight, bool italic) const {
    const std::string key = family_list + "|" + std::to_string(weight >= 600 ? 700 : weight >= 500 ? 500 : 400) +
                            (italic ? "i" : "n");
    auto it = resolve_cache_.find(key);
    if (it != resolve_cache_.end()) return {it->second};
    if (!ft_lib) FT_Init_FreeType(&ft_lib);
    int found = -1;
    const int w = weight >= 600 ? 700 : 400;
    size_t i = 0;
    while (i <= family_list.size() && found < 0) {
        size_t e = family_list.find(',', i);
        if (e == std::string::npos) e = family_list.size();
        std::string name = family_list.substr(i, e - i);
        i = e + 1;
        size_t a = name.find_first_not_of(" \t'\"");
        size_t z = name.find_last_not_of(" \t'\"");
        if (a == std::string::npos) continue;
        name = name.substr(a, z - a + 1);
        const std::string l = lc(name);
        bool any = false;
        std::string q = name;
        if (l == "serif" || l == "ui-serif") q = "Times New Roman";
        else if (l == "sans-serif") q = "Arial";
        else if (l == "system-ui" || l == "ui-sans-serif") { q = "sans-serif"; any = true; }
        else if (l == "monospace" || l == "ui-monospace") { q = "Monospace"; any = true; }
        else if (l == "cursive") { q = "Comic Sans MS"; any = true; }
        else if (l == "fantasy") { q = "Impact"; any = true; }
        found = match_family(q, any, w, italic);
    }
    if (found < 0) found = match_family("Times New Roman", false, w, italic);   // Chromium's standard font
    if (found < 0) found = match_family("DejaVu Sans", true, w, italic);
    if (found < 0) found = 0;
    resolve_cache_[key] = found;
    return {found};
}

Font::Face* Font::face(bool bold, bool mono) const {
    if (var_[0][0][0]) return var_[mono ? 2 : 0][bold][0].get();   // load_dejavu
    return mono ? mono_.get() : (bold ? bold_.get() : regular_.get());
}

void Font::set_size(float px) {
    size_ = px;
    std::vector<Face*> all{regular_.get(), bold_.get(), mono_.get()};
    for (int f = 0; f < 3; ++f) for (int b = 0; b < 2; ++b) for (int i = 0; i < 2; ++i) all.push_back(var_[f][b][i].get());
    for (Face* f : all) {
        if (!f) continue;
        // 26.6 fixed point at 72dpi: the em becomes exactly px PIXELS,
        // fractions included (15.5 stays 15.5 -- FT_Set_Pixel_Sizes would
        // truncate it to 15 and every advance would lie by 3%).
        FT_Set_Char_Size(f->ft, static_cast<FT_F26Dot6>(std::lround(px * 64)),
                         static_cast<FT_F26Dot6>(std::lround(px * 64)), 0, 0);
        // HarfBuzz copied the face's scale at create() and never looks at it
        // again: without this, advances come out scaled by whatever size the
        // face had when some earlier word was shaped (80 vs 95 vs 117px for
        // the same word at the same size, depending on history).
        hb_ft_font_changed(f->hb);
    }
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

const Glyph& Font::glyph(uint32_t glyph_index, bool bold, bool mono, float size) const {
    const uint64_t key = (static_cast<uint64_t>(glyph_index) << 34) |
                         (static_cast<uint64_t>(static_cast<uint32_t>(size * 4.f)) << 2) |
                         (bold ? 2 : 0) | (mono ? 1 : 0);
    auto it = cache_.find(key);
    if (it != cache_.end()) return it->second;

    // Rasterize at THIS word's em: the face's current size is whatever the
    // last shaped word had, so set it explicitly (cheap metric update), in
    // 26.6 so fractional ems stay fractional.
    FT_Set_Char_Size(face(bold, mono)->ft,
                     static_cast<FT_F26Dot6>(std::lround(size * 64)),
                     static_cast<FT_F26Dot6>(std::lround(size * 64)), 0, 0);
    Face* f = face(bold, mono);
    Glyph g;
    // No hinting: the advances are fractional (HarfBuzz, unhinted linear
        // metrics) and hinted bitmaps snap to a grid those advances never
        // touch -- that mismatch is "letters glued, then huge gaps".  Gray
        // AA at fractional sizes is what every browser renders at.
        if (!FT_Load_Glyph(f->ft, glyph_index, FT_LOAD_NO_HINTING) &&
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
        // oportunidad de salto tras un guion entre alfanumericos (UAX#14 HY)
        const std::string token = flat.substr(start, i - start);
        size_t p0 = 0;
        for (size_t k = 0; k < token.size(); ++k) {
            const bool brk = token[k] == '-' && k > 0 && k + 1 < token.size() &&
                             std::isalnum((unsigned char)token[k - 1]) &&
                             std::isalnum((unsigned char)token[k + 1]);
            if (!brk && k + 1 < token.size()) continue;
            const size_t end = brk ? k + 1 : token.size();
            Word w;
            w.text   = token.substr(p0, end - p0);
            w.shaped = font.shape(w.text, bold, mono);
            w.width  = w.shaped.width;
            w.bold   = bold;
            w.mono   = mono;
            w.size   = font.size();
            w.ascent = font.ascent();
            w.glued  = brk;
            out.push_back(std::move(w));
            p0 = end;
        }
    }
    return out;
}

void draw_word(std::span<uint32_t> fb, int fb_w, int fb_h,
               const Font& font, const Word& w, float x, float y, uint32_t argb) {
    const uint32_t rgb = argb & 0x00FFFFFF;

    for (const auto& p : w.shaped.positions) {
        const Glyph& g = font.glyph(p.glyph_index, w.bold, w.mono, w.size);
        const int gx = static_cast<int>(std::lround(x) + std::lround(p.x)) + g.bearing_x;
        const int gy = static_cast<int>(std::lround(y) + std::lround(w.ascent)) - g.bearing_y;
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
