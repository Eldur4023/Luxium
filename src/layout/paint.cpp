// Final geometry and the display list: absolute positions, then CSS 2.1
// Appendix E painting order with stacking contexts, backgrounds, borders,
// shadows, text and decorations.

#include <layout/box.hpp>

#include <algorithm>
#include <cmath>
#include <functional>

namespace luxium::layout {

using paint::Item;
using paint::Radii;
using style::Overflow;
using style::Position;
using style::Visibility;

// ── absolute geometry ───────────────────────────────────────────────────────

void finalize_positions(Env&, Tree& tree) {
    for (Box* k : tree.order) {
        if (k->kind == Box::Kind::Text || k->kind == Box::Kind::Inline || k->kind == Box::Kind::Break ||
            k->kind == Box::Kind::Marker)
            continue;
        Box* lp = k->lparent ? k->lparent : k->parent;
        while (lp && lp->kind == Box::Kind::Inline) lp = lp->parent;
        if (k->is_abs()) {
            const Box* cb = k->st->position == Position::Fixed ? nullptr : k->cb;
            const float bx = cb ? cb->ax + cb->border.l : 0.f, by = cb ? cb->ay + cb->border.t : 0.f;
            k->ax = bx + k->x;
            k->ay = by + k->y;
        } else if (!lp) {
            k->ax = k->x; k->ay = k->y;
        } else {
            k->ax = lp->ax + lp->border.l + lp->padding.l + k->x;
            k->ay = lp->ay + lp->border.t + lp->padding.t + k->y;
        }
        k->rel_dx = k->rel_dy = 0;
        if (k->st->position == Position::Relative) {
            const float cbw = lp ? lp->content_w() : 0, cbh = lp ? lp->content_h() : 0;
            const auto& in = k->st->inset;
            float dx = 0, dy = 0;
            if (!in[3].is_auto) dx = in[3].resolve(cbw);
            else if (!in[1].is_auto) dx = -in[1].resolve(cbw);
            if (!in[0].is_auto) dy = in[0].resolve(cbh);
            else if (!in[2].is_auto) dy = -in[2].resolve(cbh);
            k->rel_dx = dx; k->rel_dy = dy;
            k->ax += dx; k->ay += dy;
        }
    }
}

namespace {

float sn(float v) { return std::round(v); }

bool has_alpha(uint32_t c) { return (c >> 24) != 0; }

Radii radii_of(const ComputedStyle& s, float w, float h) {
    auto px = [&](const Len& l, float base) { return std::max(0.f, l.resolve(base)); };
    Radii r{px(s.radius[0], w), px(s.radius[1], w), px(s.radius[2], w), px(s.radius[3], w)};
    float f = 1.f;
    auto chk = [&](float sum, float avail) { if (sum > avail && sum > 0) f = std::min(f, avail / sum); };
    chk(r.tl + r.tr, w); chk(r.bl + r.br, w); chk(r.tl + r.bl, h); chk(r.tr + r.br, h);
    r.tl *= f; r.tr *= f; r.br *= f; r.bl *= f;
    return r;
}

struct Rect { float x = 0, y = 0, w = 0, h = 0; };

struct Painter {
    Env& env;
    Tree& tree;
    PaintOutput& out;
    paint::DisplayList* dl;
    float canvas_w, canvas_h;
    bool prop_html = false, prop_body = false;   // background propagated to the canvas

    Painter(Env& e, Tree& t, PaintOutput& o, float cw, float ch)
        : env(e), tree(t), out(o), dl(&o.main), canvas_w(cw), canvas_h(ch) {}

    // ── classification ──────────────────────────────────────────────────────
    static bool clips(const Box& b) {
        return b.st->overflow_x != Overflow::Visible || b.st->overflow_y != Overflow::Visible;
    }
    static bool is_item_of_flex(const Box& b) {
        return b.parent && b.parent->st && (b.parent->st->display == Display::Flex || b.parent->st->display == Display::Grid ||
                                            b.parent->st->display == Display::InlineFlex || b.parent->st->display == Display::InlineGrid);
    }
    static bool is_sc(const Box& b) {
        if (!b.parent) return true;
        const ComputedStyle& s = *b.st;
        if (s.position == Position::Fixed) return true;
        if (b.is_positioned() && !s.z_auto) return true;
        if (s.opacity < 1.f) return true;
        if (is_item_of_flex(b) && !s.z_auto) return true;
        return false;
    }
    static int zof(const Box& b) { return b.st->z_auto ? 0 : b.st->z_index; }

    // ── display list helpers ────────────────────────────────────────────────
    void rect(float x, float y, float w, float h, uint32_t color, const Radii& r = {}) {
        if (w <= 0 || h <= 0 || !has_alpha(color)) return;
        Item it; it.kind = Item::Kind::Rect; it.x = x; it.y = y; it.w = w; it.h = h; it.color = color; it.radii = r;
        dl->push_back(std::move(it));
    }
    void clip_push(float x, float y, float w, float h, const Radii& r = {}) {
        Item it; it.kind = Item::Kind::ClipPush; it.x = x; it.y = y; it.w = w; it.h = h; it.radii = r;
        dl->push_back(std::move(it));
    }
    void clip_pop() { Item it; it.kind = Item::Kind::ClipPop; dl->push_back(std::move(it)); }

    size_t layer_push(float alpha) {
        Item it; it.kind = Item::Kind::LayerPush; it.alpha = alpha;
        dl->push_back(std::move(it));
        return dl->size() - 1;
    }
    void layer_pop(size_t at) {
        // bounds of everything emitted since the push
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        for (size_t i = at + 1; i < dl->size(); ++i) {
            const Item& it = (*dl)[i];
            float ix = it.x, iy = it.y, iw = it.w, ih = it.h;
            if (it.kind == Item::Kind::ClipPush || it.kind == Item::Kind::ClipPop ||
                it.kind == Item::Kind::LayerPush || it.kind == Item::Kind::LayerPop) continue;
            if (it.kind == Item::Kind::Text) { iw = it.run ? it.run->width : 0; iy = it.baseline - it.px; ih = it.px * 1.4f; }
            if (it.kind == Item::Kind::Shadow) { const float p = it.blur * 1.5f + 2.f; ix -= p; iy -= p; iw += 2 * p; ih += 2 * p; }
            x0 = std::min(x0, ix); y0 = std::min(y0, iy); x1 = std::max(x1, ix + iw); y1 = std::max(y1, iy + ih);
        }
        Item& push = (*dl)[at];
        if (x1 < x0) { push.x = push.y = push.w = push.h = 0; }
        else { push.x = x0 - 1; push.y = y0 - 1; push.w = x1 - x0 + 2; push.h = y1 - y0 + 2; }
        Item pop; pop.kind = Item::Kind::LayerPop;
        dl->push_back(std::move(pop));
    }

    // ── backgrounds ─────────────────────────────────────────────────────────
    std::shared_ptr<const render::ImageBitmap> image_for(const std::string& url) {
        std::string path = url;
        if (!path.starts_with('/')) path = env.base_dir + path;
        const render::ImageBitmap* bm = render::load_image(path);
        if (!bm) return nullptr;
        return std::shared_ptr<const render::ImageBitmap>(bm, [](const render::ImageBitmap*) {});
    }

    void emit_gradient(const style::Gradient& g, float x, float y, float w, float h, const Radii& r) {
        if (g.stops.size() < 2 || w <= 0 || h <= 0) return;
        Item it; it.kind = Item::Kind::Gradient; it.x = x; it.y = y; it.w = w; it.h = h; it.radii = r;
        it.repeating = g.repeating;
        float line_len;
        if (!g.radial) {
            float ang = g.angle;
            if (g.corner) {
                const float a = std::atan2(h, w) * 180.f / 3.14159265f;
                ang = g.corner == 1 ? a : g.corner == 2 ? 180.f - a : g.corner == 3 ? 180.f + a : 360.f - a;
            }
            const float rad = ang * 3.14159265f / 180.f;
            it.gdx = std::sin(rad); it.gdy = -std::cos(rad);
            line_len = std::fabs(w * it.gdx) + std::fabs(h * it.gdy);
            it.glen = std::max(1.f, line_len);
        } else {
            it.radial = true;
            it.gcx = x + g.cx.resolve(w); it.gcy = y + g.cy.resolve(h);
            const float cx = it.gcx - x, cy = it.gcy - y;
            const float fsx = std::max(cx, w - cx), fsy = std::max(cy, h - cy);
            const float csx = std::min(cx, w - cx), csy = std::min(cy, h - cy);
            float rx, ry;
            if (g.circle) {
                float rr;
                switch (g.extent) {
                case 1: rr = std::min(csx, csy); break;
                case 2: rr = std::max(fsx, fsy); break;
                case 3: rr = std::sqrt(csx * csx + csy * csy); break;
                default: rr = std::sqrt(fsx * fsx + fsy * fsy); break;
                }
                rx = ry = rr;
            } else {
                switch (g.extent) {
                case 1: rx = csx; ry = csy; break;
                case 2: rx = fsx; ry = fsy; break;
                case 3: rx = csx * 1.41421356f; ry = csy * 1.41421356f; break;
                default: rx = fsx * 1.41421356f; ry = fsy * 1.41421356f; break;
                }
            }
            it.grx = std::max(1.f, rx); it.gry = std::max(1.f, ry);
            line_len = it.grx;
        }
        // stop positions along the line (fractions)
        std::vector<float> t(g.stops.size(), -1.f);
        for (size_t i = 0; i < g.stops.size(); ++i)
            if (!g.stops[i].pos.is_auto) {
                const style::Len& p = g.stops[i].pos;
                t[i] = (p.px + p.pct * line_len / 100.f) / line_len;
            }
        if (t.front() < 0) t.front() = 0;
        if (t.back() < 0) t.back() = 1;
        float prev = t.front();
        for (size_t i = 1; i < t.size(); ++i) { if (t[i] >= 0) { t[i] = std::max(t[i], prev); prev = t[i]; } }
        for (size_t i = 0; i < t.size();) {
            if (t[i] >= 0) { ++i; continue; }
            size_t j = i;
            while (t[j] < 0) ++j;
            for (size_t k = i; k < j; ++k) t[k] = t[i - 1] + (t[j] - t[i - 1]) * float(k - i + 1) / float(j - i + 1);
            i = j;
        }
        for (size_t i = 0; i < g.stops.size(); ++i) it.stops.push_back({g.stops[i].color, t[i]});
        dl->push_back(std::move(it));
    }

    void bg_layers(const ComputedStyle& s, const Rect& area, const Rect& clip, const Radii& cr) {
        for (size_t li = s.bg.size(); li-- > 0;) {
            const style::BgLayer& L = s.bg[li];
            if (L.kind == style::BgLayer::Kind::None) continue;
            std::shared_ptr<const render::ImageBitmap> img;
            float nw = 0, nh = 0;
            if (L.kind == style::BgLayer::Kind::Url) {
                img = image_for(L.url);
                if (!img) continue;
                nw = float(img->w); nh = float(img->h);
            }
            float tw, th;
            using SM = style::BgSizeMode;
            if (L.size_mode == SM::Cover || L.size_mode == SM::Contain) {
                if (img) {
                    const float sc = L.size_mode == SM::Cover ? std::max(area.w / nw, area.h / nh) : std::min(area.w / nw, area.h / nh);
                    tw = nw * sc; th = nh * sc;
                } else { tw = area.w; th = area.h; }
            } else if (L.size_mode == SM::Explicit) {
                const bool wa = L.size_w.is_auto, ha = L.size_h.is_auto;
                tw = wa ? 0 : L.size_w.resolve(area.w);
                th = ha ? 0 : L.size_h.resolve(area.h);
                if (wa && ha) { tw = img ? nw : area.w; th = img ? nh : area.h; }
                else if (wa) tw = img ? th * nw / nh : area.w;
                else if (ha) th = img ? tw * nh / nw : area.h;
            } else { tw = img ? nw : area.w; th = img ? nh : area.h; }
            if (tw <= 0.5f || th <= 0.5f) continue;
            using RP = style::BgRepeat;
            // round: rescale to fit a whole number of tiles
            if (L.repeat_x == RP::Round) tw = area.w / std::max(1.f, std::round(area.w / tw));
            if (L.repeat_y == RP::Round) th = area.h / std::max(1.f, std::round(area.h / th));
            float px = area.x + L.pos_x.resolve(area.w - tw);
            float py = area.y + L.pos_y.resolve(area.h - th);
            float stepx = tw, stepy = th;
            if (L.repeat_x == RP::Space && area.w > tw) {
                const float n = std::floor(area.w / tw);
                if (n >= 2) { stepx = tw + (area.w - n * tw) / (n - 1); px = area.x; }
            }
            if (L.repeat_y == RP::Space && area.h > th) {
                const float n = std::floor(area.h / th);
                if (n >= 2) { stepy = th + (area.h - n * th) / (n - 1); py = area.y; }
            }
            const bool rx = L.repeat_x != RP::NoRepeat, ry = L.repeat_y != RP::NoRepeat;
            // tiles covering the clip rect
            float x_first = px, y_first = py;
            if (rx) x_first = px - std::ceil((px - clip.x) / stepx) * stepx;
            if (ry) y_first = py - std::ceil((py - clip.y) / stepy) * stepy;
            const bool need_clip = true;
            if (need_clip) clip_push(clip.x, clip.y, clip.w, clip.h, cr);
            int count = 0;
            for (float ty = y_first; ty < clip.y + clip.h + 0.5f && count < 20000; ty += stepy) {
                for (float tx = x_first; tx < clip.x + clip.w + 0.5f && count < 20000; tx += stepx) {
                    ++count;
                    const float dx = sn(tx), dy = sn(ty), dw = sn(tx + tw) - sn(tx), dh = sn(ty + th) - sn(ty);
                    if (dx + dw <= clip.x || dy + dh <= clip.y || dx >= clip.x + clip.w || dy >= clip.y + clip.h) {
                        if (!rx) break;
                        continue;
                    }
                    if (img) {
                        Item it; it.kind = Item::Kind::Image; it.x = dx; it.y = dy; it.w = dw; it.h = dh;
                        it.img = img; it.sx = 0; it.sy = 0; it.sw = nw; it.sh = nh;
                        dl->push_back(std::move(it));
                    } else emit_gradient(L.grad, dx, dy, dw, dh, {});
                    if (!rx) break;
                }
                if (!ry) break;
            }
            if (need_clip) clip_pop();
        }
    }

    // ── box decoration ──────────────────────────────────────────────────────
    void borders(const Box& b, const Rect& r, const Radii& rad, bool left = true, bool right = true) {
        const ComputedStyle& s = *b.st;
        Item it; it.kind = Item::Kind::Border; it.x = r.x; it.y = r.y; it.w = r.w; it.h = r.h; it.radii = rad;
        const float bw[4] = {b.border.t, b.border.r, b.border.b, b.border.l};
        bool any = false;
        for (int i = 0; i < 4; ++i) {
            it.bw[i] = bw[i]; it.bc[i] = s.border_color[i]; it.bs[i] = s.border_style[i];
            if (i == 1 && !right) it.bw[i] = 0;
            if (i == 3 && !left) it.bw[i] = 0;
            if (it.bw[i] > 0 && has_alpha(it.bc[i])) any = true;
        }
        if (any) dl->push_back(std::move(it));
    }

    void decor(Box& b, bool paint_bg = true) {
        const ComputedStyle& s = *b.st;
        if (s.visibility != Visibility::Visible) return;
        const float x0 = sn(b.ax), y0 = sn(b.ay), x1 = sn(b.ax + b.w), y1 = sn(b.ay + b.h);
        Rect r{x0, y0, x1 - x0, y1 - y0};
        if (r.w <= 0 || r.h <= 0) return;
        const Radii rad = radii_of(s, r.w, r.h);
        // outer shadows (first shadow on top -> paint last to first)
        for (size_t i = s.box_shadow.size(); i-- > 0;) {
            const style::Shadow& sh = s.box_shadow[i];
            if (sh.inset || !has_alpha(sh.color)) continue;
            Item it; it.kind = Item::Kind::Shadow;
            it.x = r.x + sh.x - sh.spread; it.y = r.y + sh.y - sh.spread;
            it.w = r.w + 2 * sh.spread; it.h = r.h + 2 * sh.spread;
            it.radii = {std::max(0.f, rad.tl + sh.spread), std::max(0.f, rad.tr + sh.spread),
                        std::max(0.f, rad.br + sh.spread), std::max(0.f, rad.bl + sh.spread)};
            it.cx = r.x; it.cy = r.y; it.cw = r.w; it.ch = r.h;
            it.blur = sh.blur; it.color = sh.color;
            // the box itself (rounded) is excluded inside the rasterizer via cx..ch + radii of the box
            Item box = it; (void)box;
            it.spread = sh.spread;
            dl->push_back(std::move(it));
            dl->back().radii = {std::max(0.f, rad.tl + sh.spread), std::max(0.f, rad.tr + sh.spread),
                                std::max(0.f, rad.br + sh.spread), std::max(0.f, rad.bl + sh.spread)};
        }
        if (paint_bg) {
            const Rect pad{r.x + b.border.l, r.y + b.border.t, r.w - b.border.h(), r.h - b.border.v()};
            // background color: clipped to the border box (or padding box)
            const bool clip_pad = !s.bg.empty() && s.bg.back().clip_padding;
            rect(r.x, r.y, r.w, r.h, s.bg_color, rad);
            (void)clip_pad;
            if (!s.bg.empty()) {
                const Radii prad{std::max(0.f, rad.tl - std::max(b.border.l, b.border.t)), std::max(0.f, rad.tr - std::max(b.border.r, b.border.t)),
                                 std::max(0.f, rad.br - std::max(b.border.r, b.border.b)), std::max(0.f, rad.bl - std::max(b.border.l, b.border.b))};
                bg_layers(s, pad, clip_pad ? pad : r, clip_pad ? prad : rad);
            }
        }
        // inset shadows
        for (size_t i = s.box_shadow.size(); i-- > 0;) {
            const style::Shadow& sh = s.box_shadow[i];
            if (!sh.inset || !has_alpha(sh.color)) continue;
            Item it; it.kind = Item::Kind::Shadow; it.inset = true;
            const Rect pad{r.x + b.border.l, r.y + b.border.t, r.w - b.border.h(), r.h - b.border.v()};
            it.cx = pad.x; it.cy = pad.y; it.cw = pad.w; it.ch = pad.h;
            it.x = pad.x + sh.x + sh.spread; it.y = pad.y + sh.y + sh.spread;
            it.w = pad.w - 2 * sh.spread; it.h = pad.h - 2 * sh.spread;
            it.radii = {std::max(0.f, rad.tl - b.border.l), std::max(0.f, rad.tr - b.border.r),
                        std::max(0.f, rad.br - b.border.r), std::max(0.f, rad.bl - b.border.l)};
            it.blur = sh.blur; it.color = sh.color;
            dl->push_back(std::move(it));
        }
        if (!b.border_collapsed) borders(b, r, rad);
    }

    void table_edges(Box& t) {
        for (const auto& e : t.table_edges) {
            const float x0 = sn(t.ax + e.x), y0 = sn(t.ay + e.y), x1 = sn(t.ax + e.x + e.w), y1 = sn(t.ay + e.y + e.h);
            Item it; it.kind = Item::Kind::Border; it.x = x0; it.y = y0; it.w = std::max(1.f, x1 - x0); it.h = std::max(1.f, y1 - y0);
            const bool vertical = e.h > e.w;
            for (int i = 0; i < 4; ++i) { it.bw[i] = 0; it.bc[i] = e.color; it.bs[i] = e.style; }
            // paint the whole segment as one band: a vertical edge is a left border of full width
            if (vertical) it.bw[3] = it.w; else it.bw[0] = it.h;
            it.bw[vertical ? 3 : 0] = vertical ? it.w : it.h;
            dl->push_back(std::move(it));
        }
    }

    void replaced(Box& b) {
        const ComputedStyle& s = *b.st;
        if (s.visibility != Visibility::Visible || (!b.img && !b.media)) return;
        const float cx = sn(b.ax + b.border.l + b.padding.l), cy = sn(b.ay + b.border.t + b.padding.t);
        const float cw = sn(b.ax + b.w - b.border.r - b.padding.r) - cx, ch = sn(b.ay + b.h - b.border.b - b.padding.b) - cy;
        if (cw <= 0 || ch <= 0 || b.nat_w <= 0 || b.nat_h <= 0) return;
        float dw = cw, dh = ch;
        using OF = style::ObjectFit;
        switch (s.object_fit) {
        case OF::Fill: break;
        case OF::Contain: { const float sc = std::min(cw / b.nat_w, ch / b.nat_h); dw = b.nat_w * sc; dh = b.nat_h * sc; break; }
        case OF::Cover: { const float sc = std::max(cw / b.nat_w, ch / b.nat_h); dw = b.nat_w * sc; dh = b.nat_h * sc; break; }
        case OF::None: dw = b.nat_w; dh = b.nat_h; break;
        case OF::ScaleDown: { const float sc = std::min(1.f, std::min(cw / b.nat_w, ch / b.nat_h)); dw = b.nat_w * sc; dh = b.nat_h * sc; break; }
        }
        const float px = cx + (cw - dw) / 2, py = cy + (ch - dh) / 2;
        // intersect the placed image with the content box
        const float ix0 = std::max(px, cx), iy0 = std::max(py, cy), ix1 = std::min(px + dw, cx + cw), iy1 = std::min(py + dh, cy + ch);
        if (ix1 <= ix0 || iy1 <= iy0) return;
        Item it; it.kind = Item::Kind::Image; it.img = b.img;
        it.x = sn(ix0); it.y = sn(iy0); it.w = sn(ix1) - sn(ix0); it.h = sn(iy1) - sn(iy0);
        it.sx = (ix0 - px) / dw * b.nat_w; it.sy = (iy0 - py) / dh * b.nat_h;
        it.sw = (ix1 - ix0) / dw * b.nat_w; it.sh = (iy1 - iy0) / dh * b.nat_h;
        it.radii = radii_of(s, b.w, b.h);
        if (b.media) {
            // Poster or frame: the source rect is a fraction of the picture, whatever its size.
            MediaRect m;
            m.node = b.node; m.x = cx; m.y = cy; m.w = cw; m.h = ch;
            m.fx0 = (ix0 - px) / dw; m.fy0 = (iy0 - py) / dh;
            m.fx1 = (ix1 - px) / dw; m.fy1 = (iy1 - py) / dh;
            m.item = dl->size(); m.fixed = dl == &out.fixed;
            m.controls = b.media_controls; m.audio = b.node->name == "audio";
            out.media.push_back(m);
            if (b.img) {
                it.sx = m.fx0 * b.img->w; it.sy = m.fy0 * b.img->h;
                it.sw = (m.fx1 - m.fx0) * b.img->w; it.sh = (m.fy1 - m.fy0) * b.img->h;
            } else { it.kind = Item::Kind::Rect; it.color = 0xFF000000; }
        }
        dl->push_back(std::move(it));
    }

    // ── text and inline content ─────────────────────────────────────────────
    void deco_lines(const LineFrag& f, float x, float baseline, float w) {
        if (!f.deco || w <= 0) return;
        const ComputedStyle& ts = f.deco_style ? *f.deco_style : *f.box->st;
        const float px = f.box ? f.box->st->font_size : ts.font_size;
        const float th = std::max(1.f, std::round(px / 16.f));
        auto line = [&](float y) {
            Item it; it.kind = Item::Kind::Line; it.x = sn(x); it.w = sn(x + w) - sn(x); it.y = sn(y); it.h = th;
            it.color = f.deco_color ? f.deco_color : ts.color;
            dl->push_back(std::move(it));
        };
        if (f.deco & 1) line(baseline + std::max(1.f, std::round(px * 0.1f)));
        if (f.deco & 2) line(baseline - std::round(px * 0.3f));
        if (f.deco & 4) line(baseline - std::round(px * 0.9f));
    }

    void paint_lines(Box& blk) {
        const float cx = blk.ax + blk.border.l + blk.padding.l, cy = blk.ay + blk.border.t + blk.padding.t;
        for (const LineBox& line : blk.lines) {
            for (const LineFrag& f : line.frags) {
                switch (f.kind) {
                case LineFrag::Kind::Inline: {
                    Box& ib = *f.box;
                    const ComputedStyle& s = *ib.st;
                    if (s.visibility != Visibility::Visible) break;
                    const Rect r{sn(cx + f.x), sn(cy + line.y + f.top), sn(cx + f.x + f.w) - sn(cx + f.x), sn(cy + line.y + f.bottom) - sn(cy + line.y + f.top)};
                    if (r.w > 0 && r.h > 0) {
                        Radii rad = radii_of(s, r.w, r.h);
                        if (!f.first) { rad.tl = rad.bl = 0; }
                        if (!f.last) { rad.tr = rad.br = 0; }
                        rect(r.x, r.y, r.w, r.h, s.bg_color, rad);
                        if (!s.bg.empty()) bg_layers(s, r, r, rad);
                        borders(ib, r, rad, f.first, f.last);
                    }
                    if (ib.node && (ib.node->name == "a" || ib.node->has_attr("id")) && r.w > 0)
                        out.hits.push_back({ib.node, r.x, r.y, r.w, r.h, false});
                    break;
                }
                case LineFrag::Kind::Text: {
                    const ComputedStyle& ts = f.box ? *f.box->st : *blk.st;
                    if (ts.visibility != Visibility::Visible || !f.run) break;
                    Item it; it.kind = Item::Kind::Text;
                    it.x = cx + f.x; it.baseline = cy + line.y + f.baseline;
                    it.run = f.run; it.face = f.face; it.px = f.px; it.color = ts.color;
                    if (!f.run->positions.empty()) dl->push_back(std::move(it));
                    for (const auto& ws : f.words)
                        out.words.push_back({ws.text, cx + f.x + ws.x, cy + line.y, ws.w, line.h});
                    deco_lines(f, cx + f.x, cy + line.y + f.baseline, f.w);
                    break;
                }
                case LineFrag::Kind::Atom:
                    paint_sc(*f.box);
                    break;
                default: break;
                }
            }
        }
    }

    // first line (box + line) of a list item, for its marker
    const LineBox* first_line(const Box& b, const Box*& owner) const {
        if (!b.lines.empty()) { owner = &b; return &b.lines.front(); }
        for (const auto& k : b.kids) {
            if (k->kind != Box::Kind::Block || k->out_of_flow()) continue;
            if (const LineBox* l = first_line(*k, owner)) return l;
        }
        return nullptr;
    }

    void paint_marker(Box& li) {
        const ComputedStyle& s = *li.st;
        if (s.visibility != Visibility::Visible) return;
        Box* mk = nullptr;
        for (auto& k : li.kids) if (k->kind == Box::Kind::Marker) { mk = k.get(); break; }
        if (!mk) return;
        const Box* owner = nullptr;
        const LineBox* fl = first_line(li, owner);
        const float px = s.font_size;
        const render::FaceMetrics fm = env.font.metrics(face_of(env, s), px);
        float baseline, left;
        if (fl && owner) {
            baseline = owner->ay + owner->border.t + owner->padding.t + fl->y + fl->baseline;
            left = owner->ax + owner->border.l + owner->padding.l + (fl->frags.empty() ? 0.f : 0.f);
        } else {
            baseline = li.ay + li.border.t + li.padding.t + fm.ascent;
            left = li.ax + li.border.l + li.padding.l;
        }
        if (mk->marker_shape) {
            // Blink: bullet diameter = (ascent*2/3 + 1)/2, 3/4 of the free ascent above the baseline
            const float asc = fm.ascent;
            const float d = std::floor((std::floor(asc * 2.f / 3.f) + 1.f) / 2.f);
            const float bx = left - d - std::max(2.f, std::round(px / 2.f - 1.f));
            const float by = baseline - asc + std::floor(3.f * (asc - std::floor(asc * 2.f / 3.f)) / 2.f) - 1.f + d;
            Item it; it.kind = mk->marker_shape == 3 ? Item::Kind::Rect : Item::Kind::Rect;
            it.color = s.color;
            if (mk->marker_shape == 1) { it.x = sn(bx); it.y = sn(by - d); it.w = d; it.h = d; it.radii = {d / 2, d / 2, d / 2, d / 2}; dl->push_back(std::move(it)); }
            else if (mk->marker_shape == 3) { it.x = sn(bx); it.y = sn(by - d); it.w = d; it.h = d; dl->push_back(std::move(it)); }
            else {   // circle: ring
                Item ring; ring.kind = Item::Kind::Border; ring.x = sn(bx); ring.y = sn(by - d); ring.w = d; ring.h = d;
                ring.radii = {d / 2, d / 2, d / 2, d / 2};
                for (int i = 0; i < 4; ++i) { ring.bw[i] = 1; ring.bc[i] = s.color; ring.bs[i] = style::BorderStyle::Solid; }
                dl->push_back(std::move(ring));
            }
        } else if (!mk->text.empty()) {
            auto run = std::make_shared<render::ShapedRun>(env.font.shape_at(mk->text, face_of(env, s), px));
            Item it; it.kind = Item::Kind::Text;
            it.x = left - run->width; it.baseline = baseline; it.run = run; it.face = face_of(env, s); it.px = px; it.color = s.color;
            dl->push_back(std::move(it));
        }
    }

    // ── stacking ────────────────────────────────────────────────────────────
    struct Lists {
        std::vector<Box*> neg, zero, pos, floats, blocks, owners;
    };

    static bool owns_lines(const Box& b) { return b.has_inline_content && !b.lines.empty(); }

    void gather(Box& p, Lists& L) {
        for (auto& up : p.kids) {
            Box& k = *up;
            if (k.kind == Box::Kind::Text || k.kind == Box::Kind::Break || k.kind == Box::Kind::Marker) continue;
            if (k.kind == Box::Kind::Inline) { gather(k, L); continue; }
            if (is_sc(k) || k.is_positioned()) {
                const int z = zof(k);
                (z < 0 ? L.neg : z > 0 ? L.pos : L.zero).push_back(&k);
                continue;
            }
            if (k.is_float()) { L.floats.push_back(&k); continue; }
            if (k.is_atomic_inline()) continue;
            L.blocks.push_back(&k);
            if (clips(k)) continue;   // painted atomically
            if (owns_lines(k)) L.owners.push_back(&k);
            gather(k, L);
        }
    }

    void self(Box& b, bool bg = true) {
        decor(b, bg && !(prop_body && &b == tree.body_box));
        if (b.kind == Box::Kind::Replaced) replaced(b);
        if (b.node && b.kind != Box::Kind::Inline && !b.anonymous) {
            const std::string& tg = b.node->name;
            if (b.node->has_attr("id") || tg == "a" || tg == "button" || b.media)
                out.hits.push_back({b.node, sn(b.ax), sn(b.ay), b.w, b.h, tg == "button"});
        }
    }

    void paint_sc(Box& sc, bool bg = true) {
        const ComputedStyle& s = *sc.st;
        if (s.position == Position::Fixed && dl != &out.fixed) {
            auto* saved = dl;
            dl = &out.fixed;
            paint_sc(sc, bg);
            dl = saved;
            return;
        }
        size_t layer = 0;
        const bool lay = s.opacity < 1.f;
        if (lay) layer = layer_push(s.opacity);
        self(sc, bg);
        const bool cl = clips(sc);
        if (cl) {
            const float x0 = sn(sc.ax + sc.border.l), y0 = sn(sc.ay + sc.border.t);
            const float x1 = sn(sc.ax + sc.w - sc.border.r), y1 = sn(sc.ay + sc.h - sc.border.b);
            const Radii rad = radii_of(s, sc.w, sc.h);
            clip_push(x0, y0, x1 - x0, y1 - y0, {std::max(0.f, rad.tl - sc.border.l), std::max(0.f, rad.tr - sc.border.r),
                                                  std::max(0.f, rad.br - sc.border.r), std::max(0.f, rad.bl - sc.border.l)});
        }
        content(sc);
        if (cl) clip_pop();
        if (lay) layer_pop(layer);
    }

    void content(Box& sc) {
        Lists L;
        gather(sc, L);
        auto by_z = [](Box* a, Box* b) { const int za = zof(*a), zb = zof(*b); return za != zb ? za < zb : a->order < b->order; };
        std::stable_sort(L.neg.begin(), L.neg.end(), by_z);
        std::stable_sort(L.pos.begin(), L.pos.end(), by_z);
        for (Box* k : L.neg) paint_sc(*k);
        // in-flow block backgrounds (clipping boxes are painted whole)
        for (Box* k : L.blocks) {
            if (clips(*k)) paint_sc(*k);
            else self(*k);
        }
        for (Box* k : L.blocks) if (!k->table_edges.empty()) table_edges(*k);
        for (Box* f : L.floats) paint_sc(*f);
        // inline content: markers, then text/inline boxes
        if (sc.st->display == Display::ListItem) paint_marker(sc);
        for (Box* k : L.blocks) if (!clips(*k) && k->st->display == Display::ListItem) paint_marker(*k);
        if (owns_lines(sc)) paint_lines(sc);
        for (Box* o : L.owners) paint_lines(*o);
        for (Box* k : L.zero) paint_sc(*k);
        for (Box* k : L.pos) paint_sc(*k);
    }
};

} // namespace

void paint_tree(Env& env, Tree& tree, float canvas_w, float canvas_h, PaintOutput& out) {
    Painter p(env, tree, out, canvas_w, canvas_h);
    Box& root = *tree.root;

    // canvas background: html's, or body's when html has none
    auto has_bg = [](const ComputedStyle* s) {
        return s && ((s->bg_color >> 24) != 0 || std::any_of(s->bg.begin(), s->bg.end(), [](const style::BgLayer& l) { return l.kind != style::BgLayer::Kind::None; }));
    };
    const ComputedStyle* src = nullptr;
    const Box* src_box = nullptr;
    if (has_bg(tree.html_style)) { src = tree.html_style; src_box = &root; p.prop_html = true; }
    else if (has_bg(tree.body_style)) { src = tree.body_style; src_box = tree.body_box; p.prop_body = true; }
    if (src) {
        uint32_t c = src->bg_color;
        if ((c >> 24) == 255) out.canvas_bg = c;
        else if ((c >> 24) > 0) {   // composite over white
            const float a = (c >> 24) / 255.f;
            auto ch = [&](int sh) { return uint32_t(((c >> sh) & 255) * a + 255 * (1 - a)); };
            out.canvas_bg = 0xFF000000u | (ch(16) << 16) | (ch(8) << 8) | ch(0);
        }
        if (!src->bg.empty() && src_box) {
            const Rect area{sn(src_box->ax), sn(src_box->ay), sn(src_box->w), sn(src_box->h)};
            p.bg_layers(*src, area, Rect{0, 0, canvas_w, canvas_h}, {});
        }
    }
    p.paint_sc(root, !p.prop_html);
    if (p.prop_body && tree.body_box) {
        // body painted its own box without background (propagated): handled by skipping in `self`
    }

    // every element box, for :hover hit-testing
    for (Box* k : tree.order)
        if (k->node && !k->anonymous && (k->kind == Box::Kind::Block || k->kind == Box::Kind::Replaced))
            out.areas.push_back({k->node, k->ax, k->ay, k->w, k->h, false});

    // data-t boxes for the css-tests probe
    for (Box* k : tree.order)
        if (k->node && !k->anonymous && k->node->has_attr("data-t") && (k->kind == Box::Kind::Block || k->kind == Box::Kind::Replaced))
            out.boxes.push_back({k->node->attr_or("data-t"), k->ax, k->ay, k->w, k->h});
}

} // namespace luxium::layout
