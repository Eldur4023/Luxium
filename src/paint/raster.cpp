#include "paint/raster.hpp"

#include <render/images.hpp>

#include <algorithm>
#include <map>
#include <tuple>
#include <cmath>
#include <chrono>
#include <cstdio>
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
    std::vector<std::pair<float, float>> shifts;
    std::vector<std::vector<uint32_t>> layer_bufs;
    struct LayerInfo { IRect bounds; float alpha; const Item* push = nullptr; };
    std::vector<LayerInfo> layers;
    // transform layers: content is drawn untransformed into a buffer, then resampled through `m` on pop
    struct TfLayer { IRect src; float m[6]; float ox, oy; };
    std::vector<TfLayer> tf_layers;

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
        // away from the corners (and a pixel from the straight edges) the coverage is exactly 1: skip the SDF
        const float px = bx - s.ox, py = by - s.oy;
        const float rm = std::max(std::max(rc.r.tl, rc.r.tr), std::max(rc.r.br, rc.r.bl)) + 1.f;
        if (px >= rc.x + 1.f && px <= rc.x + rc.w - 1.f && py >= rc.y + 1.f && py <= rc.y + rc.h - 1.f &&
            ((px >= rc.x + rm && px <= rc.x + rc.w - rm) || (py >= rc.y + rm && py <= rc.y + rc.h - rm)))
            continue;
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

void fill_poly(State& s, const Item& it) {
    const size_t n = it.pts.size() / 2;
    if (n < 3 || it.w <= 0 || it.h <= 0) return;
    const IRect b = bounds_of(s, it.x, it.y, it.w, it.h);
    if (b.x1 <= b.x0 || b.y1 <= b.y0) return;
    Target& t = s.tgt();
    const bool rcl = !s.rc().empty();
    constexpr int kSub = 16;   // sub-scanlines per pixel row; exact horizontal coverage on each
    std::vector<float> row(static_cast<size_t>(b.x1 - b.x0));
    std::vector<std::pair<float, int>> xs;
    for (int y = b.y0; y < b.y1; ++y) {
        std::fill(row.begin(), row.end(), 0.f);
        for (int k = 0; k < kSub; ++k) {
            const float sy = y + (k + 0.5f) / kSub - s.oy;
            xs.clear();
            for (size_t i = 0; i < n; ++i) {
                const size_t j = (i + 1) % n;
                const float x0 = it.pts[2 * i], y0 = it.pts[2 * i + 1], x1 = it.pts[2 * j], y1 = it.pts[2 * j + 1];
                if ((y0 <= sy) == (y1 <= sy)) continue;
                xs.emplace_back(x0 + (sy - y0) * (x1 - x0) / (y1 - y0) + s.ox, y1 > y0 ? 1 : -1);
            }
            std::sort(xs.begin(), xs.end());
            int wind = 0;
            for (size_t i = 0; i + 1 < xs.size(); ++i) {
                wind += xs[i].second;
                const bool in = it.evenodd ? (i % 2 == 0) : wind != 0;
                if (!in) continue;
                const float a = std::max<float>(xs[i].first, b.x0), e = std::min<float>(xs[i + 1].first, b.x1);
                if (e <= a) continue;
                const int ia = int(a), ie = int(e);
                for (int x = ia; x <= ie && x < b.x1; ++x)
                    row[x - b.x0] += (std::min<float>(e, x + 1) - std::max<float>(a, x)) / kSub;
            }
        }
        for (int x = b.x0; x < b.x1; ++x) {
            float cov = std::min(1.f, row[x - b.x0]);
            if (cov <= 0.002f) continue;
            if (rcl) cov *= clip_cov(s, x + 0.5f, y + 0.5f);
            if (cov > 0) blend(t.px[static_cast<size_t>(y) * t.w + x], it.color, cov);
        }
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
    // Rows well inside the inner rectangle (clear of its rounded corners) only have ink in the left and right
    // bands: visit those columns instead of the whole row.
    const float rin = std::max(std::max(ir.tl, ir.tr), std::max(ir.br, ir.bl)) + 2.f;
    for (int y = b.y0; y < b.y1; ++y) {
        const float pyc = y + 0.5f - s.oy;
        const bool mid = ix1 > ix0 && iy1 > iy0 && pyc >= iy0 + rin && pyc <= iy1 - rin;
        int xa = b.x0, xb = b.x1, xc = b.x1, xd = b.x1;   // visit [xa,xb) and [xc,xd)
        if (mid) {
            xb = std::min(b.x1, std::max(b.x0, int(std::ceil(ix0 + s.ox)) + 1));
            xc = std::max(xb, std::min(b.x1, int(std::floor(ix1 + s.ox)) - 1)); xd = b.x1;
        } else { xb = b.x1; xc = xd = b.x1; }
        for (int seg = 0; seg < 2; ++seg)
        for (int x = seg ? xc : xa; x < (seg ? xd : xb); ++x) {
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

// Box-filtered 1/n copy (a poster drawn far smaller than its file would otherwise alias: bilinear only reads 2x2 texels).
static std::shared_ptr<const render::ImageBitmap> reduced(const render::ImageBitmap& bm, int n) {
    static std::map<std::tuple<const render::ImageBitmap*, int, int, int>, std::shared_ptr<const render::ImageBitmap>> cache;
    const auto key = std::make_tuple(&bm, bm.w, bm.h, n);
    if (auto f = cache.find(key); f != cache.end()) return f->second;
    if (cache.size() > 48) cache.clear();
    auto out = std::make_shared<render::ImageBitmap>();
    out->w = (bm.w + n - 1) / n; out->h = (bm.h + n - 1) / n;
    const bool alpha = !bm.rgba.empty();
    const int bpp = alpha ? 4 : 3;
    const uint8_t* src = alpha ? bm.rgba.data() : bm.rgb.data();
    std::vector<uint8_t>& dst = alpha ? out->rgba : out->rgb;
    dst.resize(size_t(out->w) * out->h * bpp);
    for (int y = 0; y < out->h; ++y)
        for (int x = 0; x < out->w; ++x) {
            uint32_t sum[4] = {0, 0, 0, 0}, cnt = 0;
            for (int yy = y * n; yy < std::min(bm.h, y * n + n); ++yy)
                for (int xx = x * n; xx < std::min(bm.w, x * n + n); ++xx) {
                    const uint8_t* q = src + (size_t(yy) * bm.w + xx) * bpp;
                    const uint32_t a = alpha ? q[3] : 255;
                    for (int c = 0; c < 3; ++c) sum[c] += q[c] * a;   // weight by alpha: transparent texels add no colour
                    sum[3] += a; ++cnt;
                }
            uint8_t* d = &dst[(size_t(y) * out->w + x) * bpp];
            for (int c = 0; c < 3; ++c) d[c] = sum[3] ? uint8_t((sum[c] + sum[3] / 2) / sum[3]) : 0;
            if (alpha) d[3] = uint8_t((sum[3] + cnt / 2) / cnt);
        }
    return cache[key] = out;
}

void draw_image(State& s, const Item& it0) {
    Item it = it0;   // a copy: the source rect is rewritten when the bitmap is reduced
    const render::ImageBitmap* bm = it.img.get();
    if (!bm || it.w <= 0 || it.h <= 0 || it.sw <= 0 || it.sh <= 0) return;
    if (const int n = int(std::min(it.sw / it.w, it.sh / it.h)); n >= 2 && bm->w > 1 && bm->h > 1) {
        it.img = reduced(*bm, n);
        bm = it.img.get();
        it.sx /= n; it.sy /= n; it.sw /= n; it.sh /= n;
    }
    const IRect b = bounds_of(s, it.x, it.y, it.w, it.h, 0);
    if (b.x1 <= b.x0 || b.y1 <= b.y0) return;
    Target& t = s.tgt();
    const bool rcl = !s.rc().empty();
    const bool rounded = it.radii.any();
    // Per-column source coordinates are the same for every row: compute them once (bilinear, 8-bit fixed point).
    struct Col { int ix, ix1, tx; bool in; };
    std::vector<Col> cols(size_t(b.x1 - b.x0));
    for (int x = b.x0; x < b.x1; ++x) {
        const float px = x + 0.5f - s.ox;
        const float u = (px - it.x) / it.w;
        Col& c = cols[size_t(x - b.x0)];
        c.in = u >= 0 && u < 1;
        const float fx = std::clamp(it.sx + u * it.sw - 0.5f, 0.f, float(bm->w) - 1.f);
        c.ix = int(fx); c.ix1 = std::min(c.ix + 1, bm->w - 1); c.tx = int((fx - float(c.ix)) * 256.f);
    }
    const bool alpha = !bm->rgba.empty();   // vector images carry straight alpha
    const uint8_t* src = alpha ? bm->rgba.data() : bm->rgb.data();
    const int bpp = alpha ? 4 : 3;
    for (int y = b.y0; y < b.y1; ++y) {
        const float py = y + 0.5f - s.oy;
        const float v = (py - it.y) / it.h;
        if (v < 0 || v >= 1) continue;
        const float fy = std::clamp(it.sy + v * it.sh - 0.5f, 0.f, float(bm->h) - 1.f);
        const int iy = int(fy), iy1 = std::min(iy + 1, bm->h - 1), ty = int((fy - float(iy)) * 256.f);
        const uint8_t* r0 = src + size_t(iy) * bm->w * bpp;
        const uint8_t* r1 = src + size_t(iy1) * bm->w * bpp;
        uint32_t* dst = t.px + static_cast<size_t>(y) * t.w;
        for (int x = b.x0; x < b.x1; ++x) {
            const Col& c = cols[size_t(x - b.x0)];
            if (!c.in) continue;
            float cov = 1.f;
            if (rounded) cov = cov_from_sdf(sdf_rrect(x + 0.5f - s.ox, py, it.x, it.y, it.w, it.h, it.radii));
            if (rcl) cov *= clip_cov(s, x + 0.5f, y + 0.5f);
            if (cov <= 0) continue;
            auto samp = [&](int chn) {
                const int p00 = r0[c.ix * bpp + chn], p10 = r0[c.ix1 * bpp + chn], p01 = r1[c.ix * bpp + chn], p11 = r1[c.ix1 * bpp + chn];
                const int top = p00 * 256 + (p10 - p00) * c.tx, bot = p01 * 256 + (p11 - p01) * c.tx;
                return (top * 256 + (bot - top) * ty + (1 << 15)) >> 16;
            };
            uint32_t rgb = 0;
            if (alpha) {
                // interpolate premultiplied so transparent texels do not bleed their colour
                float a = 0, r = 0, g = 0, bl = 0;
                const int ww[4] = {(256 - c.tx) * (256 - ty), c.tx * (256 - ty), (256 - c.tx) * ty, c.tx * ty};
                const uint8_t* q[4] = {r0 + c.ix * 4, r0 + c.ix1 * 4, r1 + c.ix * 4, r1 + c.ix1 * 4};
                for (int k = 0; k < 4; ++k) {
                    const float al = float(q[k][3]) * float(ww[k]) / 65536.f;
                    a += al; r += float(q[k][0]) * al / 255.f; g += float(q[k][1]) * al / 255.f; bl += float(q[k][2]) * al / 255.f;
                }
                if (a < 0.5f) continue;
                const uint32_t a8 = uint32_t(std::clamp(a + 0.5f, 0.f, 255.f));
                auto un = [&](float v) { return uint32_t(std::clamp(v / (a / 255.f) + 0.5f, 0.f, 255.f)); };
                blend(dst[x], (a8 << 24) | (un(r * 255.f / 255.f) << 16) | (un(g) << 8) | un(bl), cov);
                continue;
            }
            for (int chn = 0; chn < 3; ++chn) rgb |= uint32_t(samp(chn)) << (16 - chn * 8);
            if (cov >= 1.f) dst[x] = 0xFF000000u | rgb;
            else blend(dst[x], 0xFF000000u | rgb, cov);
        }
    }
}

// Gaussian-blurred rounded box, integrated analytically along x and with 4 samples along y (Evan Wallace,
// "Fast rounded rectangle shadows"). The 1-D erfc of the edge distance it replaces assumes a half-plane, so a
// thin caster (a 3 px bar with a 10 px glow) came out as bright as a big one.
static float blurred_rrect(float px, float py, float x, float y, float w, float h, const Radii& r, float sigma) {
    const float hx = w * 0.5f, hy = h * 0.5f;
    if (hx <= 0 || hy <= 0) return 0.f;
    px -= x + hx; py -= y + hy;
    float corner = px < 0 ? (py < 0 ? r.tl : r.bl) : (py < 0 ? r.tr : r.br);
    corner = std::min({corner, hx, hy});
    const float k = 0.70710678f / sigma;
    auto along_x = [&](float yy) {
        const float delta = std::min(hy - corner - std::fabs(yy), 0.f);
        const float curved = hx - corner + std::sqrt(std::max(0.f, corner * corner - delta * delta));
        return 0.5f * (std::erf((px + curved) * k) - std::erf((px - curved) * k));
    };
    const float lo = py - hy, hi = py + hy;
    const float a = std::clamp(-3.f * sigma, lo, hi), b = std::clamp(3.f * sigma, lo, hi);
    const float step = (b - a) / 4.f;
    float v = 0, t = a + step * 0.5f;
    const float g = 1.f / (2.5066283f * sigma);
    for (int i = 0; i < 4; ++i, t += step) v += along_x(py - t) * g * std::exp(-t * t / (2.f * sigma * sigma)) * step;
    return v;
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
                cov = it.blur > 0 ? std::clamp(blurred_rrect(px, py, it.x, it.y, it.w, it.h, it.radii, sigma), 0.f, 1.f)
                                  : cov_from_sdf(sdf_rrect(px, py, it.x, it.y, it.w, it.h, it.radii));
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
                if (!g.color.empty()) {   // colour glyph: its own pixels, only the text alpha applies
                    const uint8_t* q = &g.color[gi * 4];
                    if (!q[3]) continue;
                    float k = float(alpha) / 255.f;
                    if (rcl) k *= clip_cov(s, fx + 0.5f, fy + 0.5f);
                    const float a = float(q[3]);
                    auto un = [&](uint8_t v) { return uint32_t(std::clamp(float(v) * 255.f / a + 0.5f, 0.f, 255.f)); };
                    blend(t.px[static_cast<size_t>(fy) * t.w + fx], (uint32_t(q[3]) << 24) | (un(q[0]) << 16) | (un(q[1]) << 8) | un(q[2]), k);
                    continue;
                }
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

// HiDPI: the same list at k times the size, so glyphs and edges are rasterized at device resolution.
static DisplayList scale_list(const DisplayList& dl, float k) {
    DisplayList out = dl;
    for (Item& it : out) {
        it.x *= k; it.y *= k; it.w *= k; it.h *= k;
        it.radii.tl *= k; it.radii.tr *= k; it.radii.br *= k; it.radii.bl *= k;
        for (float& b : it.bw) b *= k;
        it.glen *= k; it.gcx *= k; it.gcy *= k; it.grx *= k; it.gry *= k;
        it.blur *= k; it.spread *= k; it.cx *= k; it.cy *= k; it.cw *= k; it.ch *= k;
        it.px *= k; it.baseline *= k;
        it.m[4] *= k; it.m[5] *= k;
        for (auto* fl : {&it.filters, &it.backdrop})
            for (auto& f : *fl) {
                if (f.kind == style::FilterFn::Kind::Blur) f.a *= k;
                f.dx *= k; f.dy *= k; f.blur *= k;
            }
        if (it.mask) it.mask = std::make_shared<const DisplayList>(scale_list(*it.mask, k));
        if (it.clip) it.clip = std::make_shared<const DisplayList>(scale_list(*it.clip, k));
        for (float& q : it.pts) q *= k;
        if (it.run) {
            auto r = std::make_shared<render::ShapedRun>(*it.run);
            for (auto& p : r->positions) { p.x *= k; p.y *= k; p.adv *= k; p.pen *= k; }
            r->width *= k;
            it.run = std::move(r);
        }
    }
    return out;
}

// ── filter effects (filter, backdrop-filter, text-shadow blur, mask) ──────────

namespace {

// A region of a target as premultiplied float RGBA (0..1).
struct FBuf {
    IRect r;
    int w = 0, h = 0;
    std::vector<float> p;
};

FBuf read_region(const Target& t, const IRect& r) {
    FBuf b;
    b.r = r; b.w = std::max(0, r.x1 - r.x0); b.h = std::max(0, r.y1 - r.y0);
    b.p.resize(size_t(b.w) * b.h * 4);
    for (int y = 0; y < b.h; ++y)
        for (int x = 0; x < b.w; ++x) {
            const uint32_t q = t.px[size_t(y + r.y0) * t.w + (x + r.x0)];
            const float a = float(q >> 24) / 255.f;
            float* o = &b.p[(size_t(y) * b.w + x) * 4];
            o[0] = float((q >> 16) & 255) / 255.f * a; o[1] = float((q >> 8) & 255) / 255.f * a; o[2] = float(q & 255) / 255.f * a; o[3] = a;
        }
    return b;
}

uint32_t pack_premul(const float* v) {
    const float a = std::clamp(v[3], 0.f, 1.f);
    if (a <= 0.f) return 0;
    auto c = [&](float x) { return uint32_t(std::clamp(x / a, 0.f, 1.f) * 255.f + 0.5f); };
    return (uint32_t(a * 255.f + 0.5f) << 24) | (c(v[0]) << 16) | (c(v[1]) << 8) | c(v[2]);
}

// Running-sum box blur of radius r along one axis (premultiplied, all 4 channels); edges clamp.
void box_pass(std::vector<float>& p, int w, int h, int r, bool horiz) {
    if (r <= 0) return;
    const int n = horiz ? w : h, lines = horiz ? h : w;
    std::vector<float> line(size_t(n) * 4), out(size_t(n) * 4);
    const float inv = 1.f / float(2 * r + 1);
    for (int l = 0; l < lines; ++l) {
        auto at = [&](int i) -> float* { return horiz ? &p[(size_t(l) * w + i) * 4] : &p[(size_t(i) * w + l) * 4]; };
        for (int i = 0; i < n; ++i) std::copy(at(i), at(i) + 4, &line[size_t(i) * 4]);
        float acc[4] = {0, 0, 0, 0};
        for (int i = -r; i <= r; ++i) { const float* q = &line[size_t(std::clamp(i, 0, n - 1)) * 4]; for (int c = 0; c < 4; ++c) acc[c] += q[c]; }
        for (int i = 0; i < n; ++i) {
            for (int c = 0; c < 4; ++c) out[size_t(i) * 4 + c] = acc[c] * inv;
            const float* add = &line[size_t(std::min(i + r + 1, n - 1)) * 4];
            const float* sub = &line[size_t(std::max(i - r, 0)) * 4];
            for (int c = 0; c < 4; ++c) acc[c] += add[c] - sub[c];
        }
        for (int i = 0; i < n; ++i) std::copy(&out[size_t(i) * 4], &out[size_t(i) * 4] + 4, at(i));
    }
}

// Gaussian of standard deviation sigma as three box blurs per axis (W3C filter-effects approximation).
void gaussian(FBuf& b, float sigma) {
    if (sigma < 0.3f || b.w == 0 || b.h == 0) return;
    const float wi = std::sqrt(12.f * sigma * sigma / 3.f + 1.f);
    int wl = int(std::floor(wi));
    if (wl % 2 == 0) --wl;
    const int wu = wl + 2;
    const float mi = (12.f * sigma * sigma - 3.f * wl * wl - 12.f * wl - 9.f) / (-4.f * wl - 4.f);
    const int m = int(std::round(mi));
    for (int i = 0; i < 3; ++i) {
        const int r = ((i < m ? wl : wu) - 1) / 2;
        box_pass(b.p, b.w, b.h, r, true);
        box_pass(b.p, b.w, b.h, r, false);
    }
}

void color_matrix(FBuf& b, const float m[9]) {
    for (size_t i = 0; i < b.p.size(); i += 4) {
        float* v = &b.p[i];
        const float a = v[3];
        if (a <= 0) continue;
        const float r = v[0] / a, g = v[1] / a, bl = v[2] / a;
        v[0] = std::clamp(m[0] * r + m[1] * g + m[2] * bl, 0.f, 1.f) * a;
        v[1] = std::clamp(m[3] * r + m[4] * g + m[5] * bl, 0.f, 1.f) * a;
        v[2] = std::clamp(m[6] * r + m[7] * g + m[8] * bl, 0.f, 1.f) * a;
    }
}

template <class F> void per_channel(FBuf& b, F f) {   // f(unpremultiplied channel) -> channel
    for (size_t i = 0; i < b.p.size(); i += 4) {
        float* v = &b.p[i];
        const float a = v[3];
        if (a <= 0) continue;
        for (int c = 0; c < 3; ++c) v[c] = std::clamp(f(v[c] / a), 0.f, 1.f) * a;
    }
}

void apply_filter(FBuf& b, const style::FilterFn& f) {
    using K = style::FilterFn::Kind;
    const float a = f.a;
    switch (f.kind) {
    case K::Blur: gaussian(b, a); break;
    case K::Brightness: per_channel(b, [a](float c) { return c * a; }); break;
    case K::Contrast: per_channel(b, [a](float c) { return (c - 0.5f) * a + 0.5f; }); break;
    case K::Invert: per_channel(b, [a](float c) { return a * (1 - c) + (1 - a) * c; }); break;
    case K::Opacity: for (float& v : b.p) v *= a; break;
    case K::Grayscale: {
        const float k = 1 - a;
        const float m[9] = {0.2126f + 0.7874f * k, 0.7152f - 0.7152f * k, 0.0722f - 0.0722f * k,
                            0.2126f - 0.2126f * k, 0.7152f + 0.2848f * k, 0.0722f - 0.0722f * k,
                            0.2126f - 0.2126f * k, 0.7152f - 0.7152f * k, 0.0722f + 0.9278f * k};
        color_matrix(b, m); break;
    }
    case K::Sepia: {
        const float k = 1 - a;
        const float m[9] = {0.393f + 0.607f * k, 0.769f - 0.769f * k, 0.189f - 0.189f * k,
                            0.349f - 0.349f * k, 0.686f + 0.314f * k, 0.168f - 0.168f * k,
                            0.272f - 0.272f * k, 0.534f - 0.534f * k, 0.131f + 0.869f * k};
        color_matrix(b, m); break;
    }
    case K::Saturate: {
        const float m[9] = {0.213f + 0.787f * a, 0.715f - 0.715f * a, 0.072f - 0.072f * a,
                            0.213f - 0.213f * a, 0.715f + 0.285f * a, 0.072f - 0.072f * a,
                            0.213f - 0.213f * a, 0.715f - 0.715f * a, 0.072f + 0.928f * a};
        color_matrix(b, m); break;
    }
    case K::HueRotate: {
        const float r = a * 3.14159265f / 180.f, c = std::cos(r), sn = std::sin(r);
        const float m[9] = {0.213f + c * 0.787f - sn * 0.213f, 0.715f - c * 0.715f - sn * 0.715f, 0.072f - c * 0.072f + sn * 0.928f,
                            0.213f - c * 0.213f + sn * 0.143f, 0.715f + c * 0.285f + sn * 0.140f, 0.072f - c * 0.072f - sn * 0.283f,
                            0.213f - c * 0.213f - sn * 0.787f, 0.715f - c * 0.715f + sn * 0.715f, 0.072f + c * 0.928f + sn * 0.072f};
        color_matrix(b, m); break;
    }
    case K::DropShadow: {
        // the alpha, blurred, offset and coloured, under the content
        FBuf sh = b;
        const float ca = float(f.color >> 24) / 255.f;
        const float cr = float((f.color >> 16) & 255) / 255.f, cg = float((f.color >> 8) & 255) / 255.f, cb = float(f.color & 255) / 255.f;
        const int dx = int(std::lround(f.dx)), dy = int(std::lround(f.dy));
        for (int y = 0; y < b.h; ++y)
            for (int x = 0; x < b.w; ++x) {
                const int sx = x - dx, sy = y - dy;
                const float al = (sx >= 0 && sy >= 0 && sx < b.w && sy < b.h) ? b.p[(size_t(sy) * b.w + sx) * 4 + 3] * ca : 0.f;
                float* o = &sh.p[(size_t(y) * b.w + x) * 4];
                o[0] = cr * al; o[1] = cg * al; o[2] = cb * al; o[3] = al;
            }
        gaussian(sh, f.blur);   // drop-shadow()'s length is the standard deviation (unlike box-shadow's 2-sigma radius)
        for (size_t i = 0; i < b.p.size(); i += 4) {
            const float k = 1 - b.p[i + 3];
            for (int c = 0; c < 4; ++c) b.p[i + c] += sh.p[i + c] * k;
        }
        break;
    }
    }
}

// How far the filters can move paint outside the content (blur tails, drop-shadow offsets).
int filter_reach(const std::vector<style::FilterFn>& fs) {
    float r = 0;
    for (const auto& f : fs) {
        if (f.kind == style::FilterFn::Kind::Blur) r += 3.f * f.a;
        else if (f.kind == style::FilterFn::Kind::DropShadow) r += std::max(std::fabs(f.dx), std::fabs(f.dy)) + 3.f * f.blur;
    }
    return int(std::ceil(r)) + 1;
}

IRect grow(const IRect& r, int d, const IRect& lim) {
    return {std::max(lim.x0, r.x0 - d), std::max(lim.y0, r.y0 - d), std::min(lim.x1, r.x1 + d), std::min(lim.y1, r.y1 + d)};
}

} // namespace

void rasterize(const DisplayList& dl0, Canvas& canvas, const render::Font& font) {
    DisplayList scaled;
    if (canvas.scale != 1.f) scaled = scale_list(dl0, canvas.scale);
    const DisplayList& dl = canvas.scale != 1.f ? scaled : dl0;
    State s(font, canvas.ox, canvas.oy);
    s.targets.push_back({canvas.px.data(), canvas.w, canvas.h});
    s.rects.push_back(canvas.clip);
    s.rounds.emplace_back();

    double acc[16] = {0}; int cnt[16] = {0};
    struct Report { double* a; int* c; bool on; ~Report() { if (!on) return; for (int i = 0; i < 16; ++i) if (c[i]) std::fprintf(stderr, "   kind %d: %d items %.2f ms\n", i, c[i], a[i]); } };
    const bool prof = std::getenv("LUXIUM_RASTER_PROF") != nullptr;
    Report report{acc, cnt, prof};
    for (const Item& it : dl) {
        const auto t0 = prof ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        struct Acc { bool on; std::chrono::steady_clock::time_point t0; double* a; int* c; ~Acc() { if (on) { *a += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); ++*c; } } } acct{prof, t0, &acc[int(it.kind)], &cnt[int(it.kind)]};
        switch (it.kind) {
        case Item::Kind::Rect: case Item::Kind::Line: fill_rect(s, it); break;
        case Item::Kind::Border: fill_border(s, it); break;
        case Item::Kind::Poly: fill_poly(s, it); break;
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
        case Item::Kind::ShiftPush: s.shifts.push_back({it.x, it.y}); s.ox += it.x; s.oy += it.y; break;
        case Item::Kind::ShiftPop:
            if (!s.shifts.empty()) { s.ox -= s.shifts.back().first; s.oy -= s.shifts.back().second; s.shifts.pop_back(); }
            break;
        case Item::Kind::ClipPop:
            if (s.rects.size() > 1) { s.rects.pop_back(); s.rounds.pop_back(); }
            break;
        case Item::Kind::TransformPush: {
            Target& cur = s.tgt();
            s.layer_bufs.emplace_back(static_cast<size_t>(cur.w) * cur.h, 0u);
            s.targets.push_back({s.layer_bufs.back().data(), cur.w, cur.h});
            IRect b;   // source bounds in buffer space (the ancestors' clip applies to the result, not to the source)
            b.x0 = std::max(s.rects.front().x0, int(std::floor(it.x + s.ox)) - 1); b.y0 = std::max(s.rects.front().y0, int(std::floor(it.y + s.oy)) - 1);
            b.x1 = std::min(s.rects.front().x1, int(std::ceil(it.x + it.w + s.ox)) + 1); b.y1 = std::min(s.rects.front().y1, int(std::ceil(it.y + it.h + s.oy)) + 1);
            State::TfLayer tl{b, {}, s.ox, s.oy};
            std::copy(it.m, it.m + 6, tl.m);
            s.tf_layers.push_back(tl);
            s.rects.push_back(s.rects.front());
            s.rounds.emplace_back();
            break;
        }
        case Item::Kind::TransformPop: {
            if (s.tf_layers.empty()) break;
            const State::TfLayer tl = s.tf_layers.back();
            s.tf_layers.pop_back();
            s.rects.pop_back(); s.rounds.pop_back();
            Target layer = s.targets.back();
            s.targets.pop_back();
            Target& dst = s.tgt();
            const float* m = tl.m;
            const float det = m[0] * m[3] - m[1] * m[2];
            if (std::fabs(det) > 1e-9f && tl.src.x1 > tl.src.x0 && tl.src.y1 > tl.src.y0) {
                const float i0 = m[3] / det, i1 = -m[1] / det, i2 = -m[2] / det, i3 = m[0] / det;
                // dest bbox: the source corners through the matrix (doc space = buffer space minus the origin)
                float bx0 = 1e30f, by0 = 1e30f, bx1 = -1e30f, by1 = -1e30f;
                for (int c = 0; c < 4; ++c) {
                    const float px = float((c & 1) ? tl.src.x1 : tl.src.x0) - tl.ox, py = float((c & 2) ? tl.src.y1 : tl.src.y0) - tl.oy;
                    const float qx = m[0] * px + m[2] * py + m[4] + tl.ox, qy = m[1] * px + m[3] * py + m[5] + tl.oy;
                    bx0 = std::min(bx0, qx); bx1 = std::max(bx1, qx); by0 = std::min(by0, qy); by1 = std::max(by1, qy);
                }
                const IRect clip = s.clip();
                const int X0 = std::max(clip.x0, int(std::floor(bx0)) - 1), X1 = std::min(clip.x1, int(std::ceil(bx1)) + 1);
                const int Y0 = std::max(clip.y0, int(std::floor(by0)) - 1), Y1 = std::min(clip.y1, int(std::ceil(by1)) + 1);
                auto texel = [&](int x, int y, float& a, float& r, float& g, float& b) {   // premultiplied
                    a = r = g = b = 0;
                    if (x < tl.src.x0 || x >= tl.src.x1 || y < tl.src.y0 || y >= tl.src.y1) return;
                    const uint32_t p = layer.px[static_cast<size_t>(y) * layer.w + x];
                    a = float(p >> 24) / 255.f;
                    r = float((p >> 16) & 255) * a; g = float((p >> 8) & 255) * a; b = float(p & 255) * a;
                };
                for (int y = Y0; y < Y1; ++y)
                    for (int x = X0; x < X1; ++x) {
                        const float qx = float(x) + 0.5f - tl.ox - m[4], qy = float(y) + 0.5f - tl.oy - m[5];
                        const float sx = i0 * qx + i2 * qy + tl.ox - 0.5f, sy = i1 * qx + i3 * qy + tl.oy - 0.5f;
                        const int ix = int(std::floor(sx)), iy = int(std::floor(sy));
                        const float fx = sx - float(ix), fy = sy - float(iy);
                        float A = 0, R = 0, G = 0, B = 0;
                        const float wgt[4] = {(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy};
                        for (int k = 0; k < 4; ++k) {
                            float a, r, g, b;
                            texel(ix + (k & 1), iy + (k >> 1), a, r, g, b);
                            A += wgt[k] * a; R += wgt[k] * r; G += wgt[k] * g; B += wgt[k] * b;
                        }
                        if (A <= 0.002f) continue;
                        const uint32_t out = (uint32_t(std::clamp(A * 255.f + 0.5f, 0.f, 255.f)) << 24) |
                                             (uint32_t(std::clamp(R / A + 0.5f, 0.f, 255.f)) << 16) |
                                             (uint32_t(std::clamp(G / A + 0.5f, 0.f, 255.f)) << 8) | uint32_t(std::clamp(B / A + 0.5f, 0.f, 255.f));
                        blend(dst.px[static_cast<size_t>(y) * dst.w + x], out, 1.f);
                    }
            }
            s.layer_bufs.pop_back();
            break;
        }
        case Item::Kind::LayerPush: {
            IRect b = bounds_of(s, it.x, it.y, it.w, it.h, 0);
            Target& cur = s.tgt();
            if (!it.backdrop.empty() && it.cw > 0 && it.ch > 0) {
                // backdrop-filter: what is already painted behind the border box, filtered in place (clipped to the
                // rounded box); the element's own paint then goes over it.  The blur samples around the box too.
                const IRect box = bounds_of(s, it.cx, it.cy, it.cw, it.ch, 0);
                const IRect lim{0, 0, cur.w, cur.h};
                FBuf fb = read_region(cur, grow(box, filter_reach(it.backdrop), lim));
                for (const auto& f : it.backdrop) apply_filter(fb, f);
                const bool rcl = !s.rc().empty();
                for (int y = box.y0; y < box.y1; ++y)
                    for (int x = box.x0; x < box.x1; ++x) {
                        float cov = cov_from_sdf(sdf_rrect(x + 0.5f - s.ox, y + 0.5f - s.oy, it.cx, it.cy, it.cw, it.ch, it.radii));
                        if (rcl) cov *= clip_cov(s, x + 0.5f, y + 0.5f);
                        if (cov <= 0) continue;
                        const uint32_t q = pack_premul(&fb.p[(size_t(y - fb.r.y0) * fb.w + (x - fb.r.x0)) * 4]);
                        uint32_t& d = cur.px[size_t(y) * cur.w + x];
                        if (cov >= 1.f) d = (d & 0xFF000000u) == 0xFF000000u ? (q | 0xFF000000u) : q;
                        else blend(d, q | 0xFF000000u, cov);
                    }
            }
            if (!it.filters.empty()) b = grow(b, filter_reach(it.filters), s.clip());
            s.layer_bufs.emplace_back(static_cast<size_t>(cur.w) * cur.h, 0u);
            s.targets.push_back({s.layer_bufs.back().data(), cur.w, cur.h});
            s.layers.push_back({b, it.alpha, &it});
            break;
        }
        case Item::Kind::LayerPop: {
            if (s.layers.empty()) break;
            const auto info = s.layers.back();
            s.layers.pop_back();
            Target layer = s.targets.back();
            s.targets.pop_back();
            Target& dst = s.tgt();
            const Item* push = info.push;
            if (push && (!push->filters.empty() || push->mask || push->clip) && info.bounds.x1 > info.bounds.x0 && info.bounds.y1 > info.bounds.y0) {
                FBuf fb = read_region(layer, info.bounds);
                for (const auto& f : push->filters) apply_filter(fb, f);
                for (const auto& mk : {push->mask, push->clip}) {   // mask-image, then clip-path: each one's alpha scales the group
                    if (!mk) continue;
                    std::vector<uint32_t> mbuf(static_cast<size_t>(layer.w) * layer.h, 0u);
                    Canvas mc;
                    mc.px = mbuf; mc.w = layer.w; mc.h = layer.h; mc.ox = s.ox; mc.oy = s.oy; mc.clip = info.bounds;
                    rasterize(*mk, mc, s.font);
                    for (int y = 0; y < fb.h; ++y)
                        for (int x = 0; x < fb.w; ++x) {
                            const float k = float(mbuf[size_t(y + fb.r.y0) * layer.w + (x + fb.r.x0)] >> 24) / 255.f;
                            float* v = &fb.p[(size_t(y) * fb.w + x) * 4];
                            for (int c = 0; c < 4; ++c) v[c] *= k;
                        }
                }
                for (int y = 0; y < fb.h; ++y)
                    for (int x = 0; x < fb.w; ++x)
                        layer.px[size_t(y + fb.r.y0) * layer.w + (x + fb.r.x0)] = pack_premul(&fb.p[(size_t(y) * fb.w + x) * 4]);
            }
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
