#include "paint/raster.hpp"

#include <render/images.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace luxium::paint {

namespace {

struct Target {
    uint32_t* px = nullptr;
    int w = 0, h = 0;
};

struct RoundClip { float x, y, w, h; Radii r; };

struct State {
    const render::Font& font;
    float ox, oy;
    std::vector<IRect> rects;              // intersected clip rects (stack)
    std::vector<std::vector<RoundClip>> rounds;
    std::vector<Target> targets;
    std::vector<std::vector<uint32_t>> layer_bufs;
    struct LayerInfo { IRect bounds; float alpha; };
    std::vector<LayerInfo> layers;

    explicit State(const render::Font& f, float x, float y) : font(f), ox(x), oy(y) {}
    Target& tgt() { return targets.back(); }
    const IRect& clip() const { return rects.back(); }
    const std::vector<RoundClip>& rc() const { return rounds.back(); }
};

// ── pixel math ──────────────────────────────────────────────────────────────

inline void blend(uint32_t& dst, uint32_t src, float cov) {
    const uint32_t sa8 = static_cast<uint32_t>(static_cast<float>(src >> 24) * cov + 0.5f);
    if (sa8 == 0) return;
    const uint32_t da = dst >> 24;
    if (sa8 == 255) { dst = 0xFF000000u | (src & 0xFFFFFF); return; }
    if (da == 255) {
        const uint32_t ia = 255 - sa8;
        const uint32_t r = (((src >> 16) & 255) * sa8 + ((dst >> 16) & 255) * ia + 127) / 255;
        const uint32_t g = (((src >> 8) & 255) * sa8 + ((dst >> 8) & 255) * ia + 127) / 255;
        const uint32_t b = ((src & 255) * sa8 + (dst & 255) * ia + 127) / 255;
        dst = 0xFF000000u | (r << 16) | (g << 8) | b;
        return;
    }
    // general over (dst has alpha: group layers)
    const float sa = sa8 / 255.f, daf = da / 255.f;
    const float oa = sa + daf * (1 - sa);
    if (oa <= 0) return;
    auto ch = [&](int shift) {
        const float sc = ((src >> shift) & 255) / 255.f, dc = ((dst >> shift) & 255) / 255.f;
        return (sc * sa + dc * daf * (1 - sa)) / oa;
    };
    dst = (uint32_t(oa * 255.f + 0.5f) << 24) | (uint32_t(ch(16) * 255.f + 0.5f) << 16) |
          (uint32_t(ch(8) * 255.f + 0.5f) << 8) | uint32_t(ch(0) * 255.f + 0.5f);
}

// signed distance to a rounded box (negative inside); radius picked per quadrant
inline float sdf_rrect(float px, float py, float x, float y, float w, float h, const Radii& r) {
    const float cx = x + w * 0.5f, cy = y + h * 0.5f;
    const float rad = px < cx ? (py < cy ? r.tl : r.bl) : (py < cy ? r.tr : r.br);
    const float qx = std::fabs(px - cx) - (w * 0.5f - rad);
    const float qy = std::fabs(py - cy) - (h * 0.5f - rad);
    const float ax = std::max(qx, 0.f), ay = std::max(qy, 0.f);
    return std::sqrt(ax * ax + ay * ay) + std::min(std::max(qx, qy), 0.f) - rad;
}

inline float cov_from_sdf(float d) { return std::clamp(0.5f - d, 0.f, 1.f); }

// coverage of the active rounded clips at a buffer-space pixel center
inline float clip_cov(const State& s, float bx, float by) {
    float c = 1.f;
    for (const auto& rc : s.rc()) {
        const float d = sdf_rrect(bx - s.ox, by - s.oy, rc.x, rc.y, rc.w, rc.h, rc.r);
        c *= cov_from_sdf(d);
        if (c <= 0) return 0;
    }
    return c;
}

IRect bounds_of(const State& s, float x, float y, float w, float h, int pad = 1) {
    IRect r;
    r.x0 = std::max(s.clip().x0, static_cast<int>(std::floor(x + s.ox)) - pad);
    r.y0 = std::max(s.clip().y0, static_cast<int>(std::floor(y + s.oy)) - pad);
    r.x1 = std::min(s.clip().x1, static_cast<int>(std::ceil(x + w + s.ox)) + pad);
    r.y1 = std::min(s.clip().y1, static_cast<int>(std::ceil(y + h + s.oy)) + pad);
    return r;
}

uint32_t shade(uint32_t c, float f) {   // f<1 darkens, f>1 lightens toward white
    auto ch = [&](int sh) {
        const float v = static_cast<float>((c >> sh) & 255);
        return uint32_t(std::clamp(f < 1 ? v * f : v + (255 - v) * (f - 1), 0.f, 255.f));
    };
    return (c & 0xFF000000u) | (ch(16) << 16) | (ch(8) << 8) | ch(0);
}

// ── primitives ──────────────────────────────────────────────────────────────

void fill_rect(State& s, const Item& it) {
    if (it.w <= 0 || it.h <= 0) return;
    const IRect b = bounds_of(s, it.x, it.y, it.w, it.h);
    Target& t = s.tgt();
    const bool rounded = it.radii.any();
    const bool rcl = !s.rc().empty();
    for (int y = b.y0; y < b.y1; ++y)
        for (int x = b.x0; x < b.x1; ++x) {
            const float px = x + 0.5f - s.ox, py = y + 0.5f - s.oy;
            float cov;
            if (rounded) cov = cov_from_sdf(sdf_rrect(px, py, it.x, it.y, it.w, it.h, it.radii));
            else cov = (px >= it.x && px < it.x + it.w && py >= it.y && py < it.y + it.h) ? 1.f : 0.f;
            if (cov <= 0) continue;
            if (rcl) cov *= clip_cov(s, x + 0.5f, y + 0.5f);
            if (cov > 0) blend(t.px[static_cast<size_t>(y) * t.w + x], it.color, cov);
        }
}

void fill_border(State& s, const Item& it) {
    const IRect b = bounds_of(s, it.x, it.y, it.w, it.h);
    Target& t = s.tgt();
    const float ix0 = it.x + it.bw[3], iy0 = it.y + it.bw[0];
    const float ix1 = it.x + it.w - it.bw[1], iy1 = it.y + it.h - it.bw[2];
    Radii ir;
    ir.tl = std::max(0.f, it.radii.tl - std::max(it.bw[3], it.bw[0]));
    ir.tr = std::max(0.f, it.radii.tr - std::max(it.bw[1], it.bw[0]));
    ir.br = std::max(0.f, it.radii.br - std::max(it.bw[1], it.bw[2]));
    ir.bl = std::max(0.f, it.radii.bl - std::max(it.bw[3], it.bw[2]));
    const bool rounded = it.radii.any();
    const bool rcl = !s.rc().empty();
    for (int y = b.y0; y < b.y1; ++y)
        for (int x = b.x0; x < b.x1; ++x) {
            const float px = x + 0.5f - s.ox, py = y + 0.5f - s.oy;
            float outer = rounded ? cov_from_sdf(sdf_rrect(px, py, it.x, it.y, it.w, it.h, it.radii))
                                  : ((px >= it.x && px < it.x + it.w && py >= it.y && py < it.y + it.h) ? 1.f : 0.f);
            if (outer <= 0) continue;
            float inner = 0;
            if (ix1 > ix0 && iy1 > iy0)
                inner = rounded ? cov_from_sdf(sdf_rrect(px, py, ix0, iy0, ix1 - ix0, iy1 - iy0, ir))
                                : ((px >= ix0 && px < ix1 && py >= iy0 && py < iy1) ? 1.f : 0.f);
            float cov = outer * (1.f - inner);
            if (cov <= 0) continue;
            // which side owns this pixel: largest distance past the inner edge, per border width
            const float dt = it.bw[0] > 0 ? (iy0 - py) / it.bw[0] : -1e9f;
            const float dr = it.bw[1] > 0 ? (px - ix1) / it.bw[1] : -1e9f;
            const float db = it.bw[2] > 0 ? (py - iy1) / it.bw[2] : -1e9f;
            const float dl = it.bw[3] > 0 ? (ix0 - px) / it.bw[3] : -1e9f;
            int side = 0; float best = dt;
            if (dr > best) { best = dr; side = 1; }
            if (db > best) { best = db; side = 2; }
            if (dl > best) { best = dl; side = 3; }
            if (it.bw[side] <= 0) continue;
            uint32_t col = it.bc[side];
            const float bwid = it.bw[side];
            // position inside the border band, 0 = outer edge .. 1 = inner edge
            const float band = side == 0 ? (py - it.y) / bwid : side == 2 ? (it.y + it.h - py) / bwid
                             : side == 3 ? (px - it.x) / bwid : (it.x + it.w - px) / bwid;
            const float along = (side == 0 || side == 2) ? px - it.x : py - it.y;
            using BS = style::BorderStyle;
            switch (it.bs[side]) {
            case BS::None: case BS::Hidden: continue;
            case BS::Dashed: {
                const float dash = std::max(2.f, bwid * 3.f), gap = dash;
                const float rem = std::fmod(along, dash + gap);
                if (rem >= dash) continue;
                break;
            }
            case BS::Dotted: {
                const float d = std::max(1.f, bwid);
                const float cxp = std::fmod(along, 2 * d) - d;
                const float cyp = (band - 0.5f) * bwid;
                if (cxp * cxp + cyp * cyp > (d * 0.5f) * (d * 0.5f)) continue;
                break;
            }
            case BS::Double:
                if (bwid >= 3 && band > 1.f / 3 && band < 2.f / 3) continue;
                break;
            case BS::Inset: col = (side == 0 || side == 3) ? shade(col, 0.55f) : col; break;
            case BS::Outset: col = (side == 2 || side == 1) ? shade(col, 0.55f) : col; break;
            case BS::Groove: col = band < 0.5f ? shade(col, 0.55f) : col; break;
            case BS::Ridge: col = band >= 0.5f ? shade(col, 0.55f) : col; break;
            default: break;
            }
            if (rcl) cov *= clip_cov(s, x + 0.5f, y + 0.5f);
            if (cov > 0) blend(t.px[static_cast<size_t>(y) * t.w + x], col, cov);
        }
}

uint32_t grad_color(const std::vector<Stop>& st, float t) {
    if (t <= st.front().t) return st.front().argb;
    if (t >= st.back().t) return st.back().argb;
    size_t i = 0;
    while (i + 1 < st.size() && st[i + 1].t <= t) ++i;
    const Stop &a = st[i], &b = st[i + 1];
    const float span = b.t - a.t;
    const float f = span > 1e-6f ? (t - a.t) / span : 0.f;
    // premultiplied interpolation (CSS)
    const float aa = (a.argb >> 24) / 255.f, ba = (b.argb >> 24) / 255.f;
    const float oa = aa + (ba - aa) * f;
    if (oa <= 0) return 0;
    auto ch = [&](int sh) {
        const float ac = ((a.argb >> sh) & 255) * aa, bc = ((b.argb >> sh) & 255) * ba;
        return uint32_t(std::clamp((ac + (bc - ac) * f) / oa, 0.f, 255.f) + 0.5f);
    };
    return (uint32_t(oa * 255.f + 0.5f) << 24) | (ch(16) << 16) | (ch(8) << 8) | ch(0);
}

void fill_gradient(State& s, const Item& it) {
    if (it.stops.size() < 2 || it.w <= 0 || it.h <= 0) return;
    const IRect b = bounds_of(s, it.x, it.y, it.w, it.h, 0);
    Target& t = s.tgt();
    const bool rounded = it.radii.any();
    const bool rcl = !s.rc().empty();
    const float cxm = it.x + it.w * 0.5f, cym = it.y + it.h * 0.5f;
    const float first = it.stops.front().t, last = it.stops.back().t;
    for (int y = b.y0; y < b.y1; ++y)
        for (int x = b.x0; x < b.x1; ++x) {
            const float px = x + 0.5f - s.ox, py = y + 0.5f - s.oy;
            if (px < it.x || px >= it.x + it.w || py < it.y || py >= it.y + it.h) continue;
            float cov = rounded ? cov_from_sdf(sdf_rrect(px, py, it.x, it.y, it.w, it.h, it.radii)) : 1.f;
            if (cov <= 0) continue;
            if (rcl) cov *= clip_cov(s, x + 0.5f, y + 0.5f);
            if (cov <= 0) continue;
            float tt;
            if (it.radial) {
                const float dx = (px - it.gcx) / it.grx, dy = (py - it.gcy) / it.gry;
                tt = std::sqrt(dx * dx + dy * dy);
            } else {
                tt = 0.5f + ((px - cxm) * it.gdx + (py - cym) * it.gdy) / it.glen;
            }
            if (it.repeating && last > first) {
                tt = first + std::fmod(std::fmod(tt - first, last - first) + (last - first), last - first);
            }
            blend(t.px[static_cast<size_t>(y) * t.w + x], grad_color(it.stops, tt), cov);
        }
}

void draw_image(State& s, const Item& it) {
    const render::ImageBitmap* bm = it.img.get();
    if (!bm || it.w <= 0 || it.h <= 0 || it.sw <= 0 || it.sh <= 0) return;
    const IRect b = bounds_of(s, it.x, it.y, it.w, it.h, 0);
    Target& t = s.tgt();
    const bool rcl = !s.rc().empty();
    const bool rounded = it.radii.any();
    for (int y = b.y0; y < b.y1; ++y)
        for (int x = b.x0; x < b.x1; ++x) {
            const float px = x + 0.5f - s.ox, py = y + 0.5f - s.oy;
            const float u = (px - it.x) / it.w, v = (py - it.y) / it.h;
            if (u < 0 || u >= 1 || v < 0 || v >= 1) continue;
            float cov = rounded ? cov_from_sdf(sdf_rrect(px, py, it.x, it.y, it.w, it.h, it.radii)) : 1.f;
            if (rcl) cov *= clip_cov(s, x + 0.5f, y + 0.5f);
            if (cov <= 0) continue;
            const float fx = std::clamp(it.sx + u * it.sw - 0.5f, 0.f, float(bm->w) - 1.f);
            const float fy = std::clamp(it.sy + v * it.sh - 0.5f, 0.f, float(bm->h) - 1.f);
            const int ix = int(fx), iy = int(fy);
            const int ix1 = std::min(ix + 1, bm->w - 1), iy1 = std::min(iy + 1, bm->h - 1);
            const float tx = fx - ix, ty = fy - iy;
            uint32_t rgb = 0;
            for (int chn = 0; chn < 3; ++chn) {
                const float p00 = bm->rgb[(size_t(iy) * bm->w + ix) * 3 + chn];
                const float p10 = bm->rgb[(size_t(iy) * bm->w + ix1) * 3 + chn];
                const float p01 = bm->rgb[(size_t(iy1) * bm->w + ix) * 3 + chn];
                const float p11 = bm->rgb[(size_t(iy1) * bm->w + ix1) * 3 + chn];
                const float top = p00 + (p10 - p00) * tx, bot = p01 + (p11 - p01) * tx;
                rgb |= uint32_t(top + (bot - top) * ty + 0.5f) << (16 - chn * 8);
            }
            blend(t.px[static_cast<size_t>(y) * t.w + x], 0xFF000000u | rgb, cov);
        }
}

void draw_shadow(State& s, const Item& it) {
    // it.x..: the (offset, spread-grown) shadow shape; it.c*: the casting box
    const float sigma = std::max(0.5f, it.blur * 0.5f);
    const float k = 0.5f / (sigma * 1.41421356f);
    const float pad = it.blur * 1.5f + 2.f;
    const IRect b = it.inset ? bounds_of(s, it.cx, it.cy, it.cw, it.ch, 0)
                             : bounds_of(s, it.x - pad, it.y - pad, it.w + 2 * pad, it.h + 2 * pad, 0);
    Target& t = s.tgt();
    const bool rcl = !s.rc().empty();
    Radii box_r = it.radii;
    for (int y = b.y0; y < b.y1; ++y)
        for (int x = b.x0; x < b.x1; ++x) {
            const float px = x + 0.5f - s.ox, py = y + 0.5f - s.oy;
            float cov;
            if (!it.inset) {
                // never under the casting box itself
                if (sdf_rrect(px, py, it.cx, it.cy, it.cw, it.ch, box_r) < -0.5f) continue;
                const float d = sdf_rrect(px, py, it.x, it.y, it.w, it.h, it.radii);
                cov = std::clamp(0.5f * std::erfc(d * k), 0.f, 1.f);
            } else {
                const float inside = cov_from_sdf(sdf_rrect(px, py, it.cx, it.cy, it.cw, it.ch, box_r));
                if (inside <= 0) continue;
                const float d = sdf_rrect(px, py, it.x, it.y, it.w, it.h, it.radii);   // the "hole"
                cov = std::clamp(0.5f * std::erfc(-d * k), 0.f, 1.f) * inside;
            }
            if (cov <= 0.004f) continue;
            if (rcl) cov *= clip_cov(s, x + 0.5f, y + 0.5f);
            if (cov > 0) blend(t.px[static_cast<size_t>(y) * t.w + x], it.color, cov);
        }
}

// Text coverage correction: Skia's contrast/gamma tables darken thin strokes of
// dark text on light backgrounds (fitted against Chromium's output).
inline float text_cov(float c) { return c <= 0 ? 0.f : std::min(1.f, std::pow(c, 0.87f)); }

void draw_text(State& s, const Item& it) {
    if (!it.run) return;
    Target& t = s.tgt();
    const bool rcl = !s.rc().empty();
    const uint32_t alpha = it.color >> 24;
    const IRect& c = s.clip();
    const bool layer = s.targets.size() > 1;   // LCD only over opaque pixels
    for (const auto& p : it.run->positions) {
        // sub-pixel positioning: x is kept fractional, quantized to quarter pixels (Skia)
        const float fxp = it.x + s.ox + p.x + 0.125f;
        const int ixp = static_cast<int>(std::floor(fxp));
        const int subx = std::min(3, static_cast<int>((fxp - static_cast<float>(ixp)) * 4.f));
        const render::Glyph& g = s.font.glyph_at(p.glyph_index, p.face >= 0 ? render::FaceKey{p.face} : it.face, it.px, subx);
        const int gx = ixp + g.bearing_x;
        const int gy = static_cast<int>(std::lround(it.baseline + s.oy) + std::lround(p.y)) - g.bearing_y;
        for (int r = 0; r < g.height; ++r) {
            const int fy = gy + r;
            if (fy < c.y0 || fy >= c.y1) continue;
            for (int cc = 0; cc < g.width; ++cc) {
                const int fx = gx + cc;
                if (fx < c.x0 || fx >= c.x1) continue;
                const size_t gi = static_cast<size_t>(r) * g.width + cc;
                float cr, cg, cb;
                if (!g.lcd.empty() && !layer) {
                    cr = g.lcd[gi * 3] / 255.f; cg = g.lcd[gi * 3 + 1] / 255.f; cb = g.lcd[gi * 3 + 2] / 255.f;
                } else {
                    cr = cg = cb = g.alpha[gi] / 255.f;
                }
                if (cr <= 0 && cg <= 0 && cb <= 0) continue;
                if (rcl) { const float k = clip_cov(s, fx + 0.5f, fy + 0.5f); cr *= k; cg *= k; cb *= k; }
                cr = text_cov(cr) * alpha / 255.f; cg = text_cov(cg) * alpha / 255.f; cb = text_cov(cb) * alpha / 255.f;
                uint32_t& dst = t.px[static_cast<size_t>(fy) * t.w + fx];
                if ((dst >> 24) == 255 && !layer) {
                    auto mix = [&](int sh, float k) {
                        const float sc = static_cast<float>((it.color >> sh) & 255), dc = static_cast<float>((dst >> sh) & 255);
                        return uint32_t(std::clamp(sc * k + dc * (1.f - k) + 0.5f, 0.f, 255.f));
                    };
                    dst = 0xFF000000u | (mix(16, cr) << 16) | (mix(8, cg) << 8) | mix(0, cb);
                } else {
                    const float k = (cr + cg + cb) / 3.f;
                    blend(dst, (it.color & 0xFFFFFF) | 0xFF000000u, k);
                }
            }
        }
    }
}

IRect intersect(const IRect& a, const IRect& b) {
    return {std::max(a.x0, b.x0), std::max(a.y0, b.y0), std::min(a.x1, b.x1), std::min(a.y1, b.y1)};
}

} // namespace

void rasterize(const DisplayList& dl, Canvas& canvas, const render::Font& font) {
    State s(font, canvas.ox, canvas.oy);
    s.targets.push_back({canvas.px.data(), canvas.w, canvas.h});
    s.rects.push_back(canvas.clip);
    s.rounds.emplace_back();

    for (const Item& it : dl) {
        switch (it.kind) {
        case Item::Kind::Rect: case Item::Kind::Line: fill_rect(s, it); break;
        case Item::Kind::Border: fill_border(s, it); break;
        case Item::Kind::Gradient: fill_gradient(s, it); break;
        case Item::Kind::Image: draw_image(s, it); break;
        case Item::Kind::Shadow: draw_shadow(s, it); break;
        case Item::Kind::Text: draw_text(s, it); break;
        case Item::Kind::ClipPush: {
            IRect r;
            r.x0 = static_cast<int>(std::floor(it.x + s.ox + 0.001f));
            r.y0 = static_cast<int>(std::floor(it.y + s.oy + 0.001f));
            r.x1 = static_cast<int>(std::ceil(it.x + it.w + s.ox - 0.001f));
            r.y1 = static_cast<int>(std::ceil(it.y + it.h + s.oy - 0.001f));
            s.rects.push_back(intersect(s.clip(), r));
            auto rc = s.rc();
            if (it.radii.any()) rc.push_back({it.x, it.y, it.w, it.h, it.radii});
            s.rounds.push_back(std::move(rc));
            break;
        }
        case Item::Kind::ClipPop:
            if (s.rects.size() > 1) { s.rects.pop_back(); s.rounds.pop_back(); }
            break;
        case Item::Kind::LayerPush: {
            const IRect b = bounds_of(s, it.x, it.y, it.w, it.h, 0);
            Target& cur = s.tgt();
            s.layer_bufs.emplace_back(static_cast<size_t>(cur.w) * cur.h, 0u);
            s.targets.push_back({s.layer_bufs.back().data(), cur.w, cur.h});
            s.layers.push_back({b, it.alpha});
            break;
        }
        case Item::Kind::LayerPop: {
            if (s.layers.empty()) break;
            const auto info = s.layers.back();
            s.layers.pop_back();
            Target layer = s.targets.back();
            s.targets.pop_back();
            Target& dst = s.tgt();
            for (int y = info.bounds.y0; y < info.bounds.y1; ++y)
                for (int x = info.bounds.x0; x < info.bounds.x1; ++x) {
                    const uint32_t p = layer.px[static_cast<size_t>(y) * layer.w + x];
                    if (p >> 24) blend(dst.px[static_cast<size_t>(y) * dst.w + x], p, info.alpha);
                }
            s.layer_bufs.pop_back();
            break;
        }
        }
    }
}

} // namespace luxium::paint
