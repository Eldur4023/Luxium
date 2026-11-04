// Final geometry and the display list: absolute positions, then CSS 2.1
// Appendix E painting order with stacking contexts, backgrounds, borders,
// shadows, text and decorations.

#include <layout/box.hpp>
#include <layout/frame.hpp>
#include <style/values.hpp>

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

static bool scrolls(const Box* b) {
    if (!b || !b->node || b->kind != Box::Kind::Block) return false;
    using O = style::Overflow;
    auto sc = [](O o) { return o == O::Auto || o == O::Scroll || o == O::Hidden; };
    return sc(b->st->overflow_x) || sc(b->st->overflow_y);
}

static void place_all(Tree& tree) {
    for (Box* k : tree.order) {
        if (k->kind == Box::Kind::Text || k->kind == Box::Kind::Inline || k->kind == Box::Kind::Break ||
            k->kind == Box::Kind::Marker)
            continue;
        Box* lp = k->lparent ? k->lparent : k->parent;
        while (lp && lp->kind == Box::Kind::Inline) lp = lp->parent;
        if (k->is_abs()) {
            const Box* cb = k->st->position == Position::Fixed ? nullptr : k->cb;
            const float bx = cb ? cb->ax + cb->border.l : 0.f, by = cb ? cb->ay + cb->border.t : 0.f;
            const float sx = scrolls(cb) ? cb->node->scroll_x : 0.f, sy = scrolls(cb) ? cb->node->scroll_y : 0.f;
            k->ax = bx + k->x - sx;
            k->ay = by + k->y - sy;
        } else if (!lp) {
            k->ax = k->x; k->ay = k->y;
        } else {
            const float sx = scrolls(lp) ? lp->node->scroll_x : 0.f, sy = scrolls(lp) ? lp->node->scroll_y : 0.f;
            k->ax = lp->ax + lp->border.l + lp->padding.l + k->x - sx;
            k->ay = lp->ay + lp->border.t + lp->padding.t + k->y - sy;
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

// Absolute positions; scroll containers shift their content by the node's scroll offset, clamped to what
// the content actually overflows by (so a shrinking list never leaves the view scrolled past its end).
void finalize_positions(Env&, Tree& tree) {
    place_all(tree);
    bool any = false;
    for (Box* k : tree.order) if (scrolls(k)) { any = true; break; }
    if (!any) return;
    for (int pass = 0; pass < 2; ++pass) {
        // overflow extents, children before parents
        for (Box* k : tree.order) {
            if (scrolls(k)) { k->ext_r = k->ax + k->border.l + k->padding.l - k->node->scroll_x; k->ext_b = k->ay + k->border.t + k->padding.t + k->scroll_h - k->node->scroll_y; }   // content only (+ inline content height)
            else { k->ext_r = k->ax + k->w; k->ext_b = k->ay + k->h; }
        }
        for (size_t i = tree.order.size(); i-- > 0;) {
            Box* k = tree.order[i];
            if (k->kind == Box::Kind::Text || k->kind == Box::Kind::Inline || k->kind == Box::Kind::Break || k->kind == Box::Kind::Marker) continue;
            Box* lp = k->lparent ? k->lparent : k->parent;
            while (lp && lp->kind == Box::Kind::Inline) lp = lp->parent;
            if (!lp || k->st->position == Position::Fixed) continue;
            const bool clipped = lp->st->overflow_x != style::Overflow::Visible || lp->st->overflow_y != style::Overflow::Visible;
            if (clipped && !scrolls(lp)) continue;
            const bool kc = k->st->overflow_x != style::Overflow::Visible || k->st->overflow_y != style::Overflow::Visible;
            lp->ext_r = std::max(lp->ext_r, kc ? k->ax + k->w : k->ext_r);
            lp->ext_b = std::max(lp->ext_b, kc ? k->ay + k->h : k->ext_b);
        }
        bool changed = false;
        for (Box* k : tree.order) {
            if (!scrolls(k)) continue;
            // a clipping container's own extent must not include what it clips away from its parent: reset
            const float inner_r = k->ax + k->w - k->border.r, inner_b = k->ay + k->h - k->border.b;
            const float sx = k->node->scroll_x, sy = k->node->scroll_y;
            k->max_sx = std::max(0.f, k->ext_r + sx + k->padding.r - inner_r);
            k->max_sy = std::max(0.f, k->ext_b + sy + k->padding.b - inner_b);
            if (k->st->overflow_x == style::Overflow::Visible || k->st->overflow_x == style::Overflow::Clip) k->max_sx = 0;
            if (k->st->overflow_y == style::Overflow::Visible || k->st->overflow_y == style::Overflow::Clip) k->max_sy = 0;
            const float nx = std::clamp(sx, 0.f, k->max_sx), ny = std::clamp(sy, 0.f, k->max_sy);
            if (nx != sx || ny != sy) { auto* nn = const_cast<dom::Node*>(k->node); nn->scroll_x = nx; nn->scroll_y = ny; changed = true; }
        }
        if (!changed) break;
        place_all(tree);
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
    int  sticky_cur = -1;                        // index of the sticky group being painted
    struct Affine { float m[6] = {1, 0, 0, 1, 0, 0}; };   // x' = m0 x + m2 y + m4, y' = m1 x + m3 y + m5
    static Affine then(const Affine& first, const Affine& second) {   // apply `first`, then `second`
        const float* a = first.m; const float* b = second.m;
        Affine r;
        r.m[0] = b[0] * a[0] + b[2] * a[1]; r.m[1] = b[1] * a[0] + b[3] * a[1];
        r.m[2] = b[0] * a[2] + b[2] * a[3]; r.m[3] = b[1] * a[2] + b[3] * a[3];
        r.m[4] = b[0] * a[4] + b[2] * a[5] + b[4]; r.m[5] = b[1] * a[4] + b[3] * a[5] + b[5];
        return r;
    }
    Affine tcur;                                 // accumulated transform of the boxes being painted
    std::unordered_map<const Box*, Affine> tf_of;   // own transform of every transformed box
    // Maps a hit rectangle through `a` (axis-aligned bounding box of the image when it rotates or skews).
    static void map_hit(HitRect& h, const Affine& a) {
        const float* m = a.m;
        if (m[0] == 1 && m[1] == 0 && m[2] == 0 && m[3] == 1) { h.x += m[4]; h.y += m[5]; h.tx += m[4]; h.ty += m[5]; return; }
        float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
        for (int i = 0; i < 4; ++i) {
            const float px = h.x + ((i & 1) ? h.w : 0), py = h.y + ((i & 2) ? h.h : 0);
            const float qx = m[0] * px + m[2] * py + m[4], qy = m[1] * px + m[3] * py + m[5];
            x0 = std::min(x0, qx); x1 = std::max(x1, qx); y0 = std::min(y0, qy); y1 = std::max(y1, qy);
        }
        h.tx += (x0 - h.x); h.ty += (y0 - h.y);
        h.x = x0; h.y = y0; h.w = x1 - x0; h.h = y1 - y0;
    }
    void xform_hit(HitRect& h) const { map_hit(h, tcur); }
    // The full transform of a box: its own, then each ancestor's.
    Affine chain_of(const Box* b) const {
        Affine a;
        for (const Box* q = b; q; q = q->parent) { auto f = tf_of.find(q); if (f != tf_of.end()) a = then(a, f->second); }
        return a;
    }
    std::unordered_map<const Box*, int> sticky_roots;
    // sticky works against the page scroll only: inside a scroll container it behaves as static
    static bool sticky_ok(const Box& b) {
        for (const Box* p = b.parent; p; p = p->parent) if (p->kind == Box::Kind::Block && p->node && p->st && (p->st->overflow_x == Overflow::Auto || p->st->overflow_x == Overflow::Scroll || p->st->overflow_y == Overflow::Auto || p->st->overflow_y == Overflow::Scroll || p->st->overflow_y == Overflow::Hidden)) return false;
        return true;
    }

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
        if (!s.filter.empty() || !s.backdrop_filter.empty() || !s.mask.empty() || s.clip_path.kind != style::ClipPath::Kind::None || s.bg_clip_text) return true;
        if (s.translate_x.px != 0 || s.translate_y.px != 0 || s.translate_x.pct != 0 || s.translate_y.pct != 0 || s.tf_linear()) return true;
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

    // clip-path: the basic shape against its reference box, as a one-polygon display list (alpha = coverage).
    std::shared_ptr<const paint::DisplayList> clip_shape(const Box& sc, const style::ClipPath& cp) {
        float bx = sc.ax, by = sc.ay, bw = sc.w, bh = sc.h;
        auto shrink = [&](const Edges& e, float k) { bx += k * e.l; by += k * e.t; bw -= k * e.h(); bh -= k * e.v(); };
        if (cp.box == 1) shrink(sc.border, 1);
        else if (cp.box == 2) { shrink(sc.border, 1); shrink(sc.padding, 1); }
        else if (cp.box == 3) shrink(sc.margin, -1);
        bw = std::max(bw, 0.f); bh = std::max(bh, 0.f);
        std::vector<float> P;
        auto pt = [&](float x, float y) { P.push_back(x); P.push_back(y); };
        constexpr float kPi = 3.14159265f;
        auto arc = [&](float cx, float cy, float rx, float ry, float a0, float a1) {   // corner / ellipse arc, a0..a1 in turns of pi/2
            const int n = 24;
            for (int i = 0; i <= n; ++i) { const float a = (a0 + (a1 - a0) * i / n) * kPi / 2; pt(cx + rx * std::cos(a), cy + ry * std::sin(a)); }
        };
        bool evenodd = cp.evenodd;
        if (cp.kind == style::ClipPath::Kind::Inset) {
            const float t = cp.edge[0].resolve(bh), r = cp.edge[1].resolve(bw), b = cp.edge[2].resolve(bh), l = cp.edge[3].resolve(bw);
            const float x0 = bx + l, y0 = by + t, x1 = bx + bw - r, y1 = by + bh - b;
            const float w = std::max(0.f, x1 - x0), h = std::max(0.f, y1 - y0);
            float rx[4], ry[4];
            for (int k = 0; k < 4; ++k) { rx[k] = std::max(0.f, cp.rx[k].resolve(w)); ry[k] = std::max(0.f, cp.ry[k].resolve(h)); }
            float f = 1.f;   // radii overlap: scale all down (CSS Backgrounds 3 section 5.5)
            auto chk = [&](float sum, float avail) { if (sum > avail && sum > 0) f = std::min(f, avail / sum); };
            chk(rx[0] + rx[1], w); chk(rx[3] + rx[2], w); chk(ry[0] + ry[3], h); chk(ry[1] + ry[2], h);
            for (int k = 0; k < 4; ++k) { rx[k] *= f; ry[k] *= f; }
            if (w <= 0 || h <= 0) return std::make_shared<paint::DisplayList>();
            // clockwise from the top-left corner's end of the top edge; an arc of 0 radius collapses to its corner
            arc(x0 + rx[0], y0 + ry[0], rx[0], ry[0], 2, 3);
            arc(x1 - rx[1], y0 + ry[1], rx[1], ry[1], 3, 4);
            arc(x1 - rx[2], y1 - ry[2], rx[2], ry[2], 0, 1);
            arc(x0 + rx[3], y1 - ry[3], rx[3], ry[3], 1, 2);
        } else if (cp.kind == style::ClipPath::Kind::Circle || cp.kind == style::ClipPath::Kind::Ellipse) {
            const float cx = bx + cp.cx.resolve(bw), cy = by + cp.cy.resolve(bh);
            const float dl_ = std::fabs(cx - bx), dr = std::fabs(bx + bw - cx), dt = std::fabs(cy - by), db = std::fabs(by + bh - cy);
            auto side = [&](int kw, const Len& l, bool horiz, bool circle) {
                if (kw == 1 || kw == 2) {
                    const float a = horiz ? dl_ : dt, b = horiz ? dr : db;
                    if (circle) return kw == 1 ? std::min({dl_, dr, dt, db}) : std::max({dl_, dr, dt, db});
                    return kw == 1 ? std::min(a, b) : std::max(a, b);
                }
                return l.resolve(circle ? std::sqrt((bw * bw + bh * bh) / 2) : horiz ? bw : bh);
            };
            const bool circ = cp.kind == style::ClipPath::Kind::Circle;
            const float rx = std::max(0.f, side(cp.rkw[0], cp.r[0], true, circ));
            const float ry = circ ? rx : std::max(0.f, side(cp.rkw[1], cp.r[1], false, false));
            if (rx <= 0 || ry <= 0) return std::make_shared<paint::DisplayList>();
            arc(cx, cy, rx, ry, 0, 4);
        } else {
            for (const auto& q : cp.pts) pt(bx + q.first.resolve(bw), by + q.second.resolve(bh));
        }
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        for (size_t i = 0; i < P.size(); i += 2) { x0 = std::min(x0, P[i]); x1 = std::max(x1, P[i]); y0 = std::min(y0, P[i + 1]); y1 = std::max(y1, P[i + 1]); }
        if (P.size() < 6) return std::make_shared<paint::DisplayList>();
        paint::Item it; it.kind = Item::Kind::Poly; it.pts = std::move(P); it.evenodd = evenodd; it.color = 0xFF000000;
        it.x = x0; it.y = y0; it.w = x1 - x0; it.h = y1 - y0;
        auto m = std::make_shared<paint::DisplayList>();
        m->push_back(std::move(it));
        return m;
    }

    size_t layer_push(float alpha) {
        Item it; it.kind = Item::Kind::LayerPush; it.alpha = alpha;
        dl->push_back(std::move(it));
        return dl->size() - 1;
    }
    // Bounds (document coords) of the items emitted since `at`; nested transforms count by their transformed bounds.
    bool bounds_since(size_t at, float& x0, float& y0, float& x1, float& y1) const {
        x0 = y0 = 1e9f; x1 = y1 = -1e9f;
        auto add = [&](float ix, float iy, float iw, float ih) { x0 = std::min(x0, ix); y0 = std::min(y0, iy); x1 = std::max(x1, ix + iw); y1 = std::max(y1, iy + ih); };
        for (size_t i = at + 1; i < dl->size(); ++i) {
            const Item& it = (*dl)[i];
            float ix = it.x, iy = it.y, iw = it.w, ih = it.h;
            switch (it.kind) {
            case Item::Kind::ClipPush: case Item::Kind::ClipPop: case Item::Kind::LayerPush: case Item::Kind::LayerPop:
            case Item::Kind::ShiftPush: case Item::Kind::ShiftPop: case Item::Kind::TransformPop: continue;
            case Item::Kind::TransformPush: {
                for (int c = 0; c < 4; ++c) {
                    const float px = ix + ((c & 1) ? iw : 0), py = iy + ((c & 2) ? ih : 0);
                    add(it.m[0] * px + it.m[2] * py + it.m[4], it.m[1] * px + it.m[3] * py + it.m[5], 0, 0);
                }
                int depth = 1;   // skip the nested content: its bounds are the push's
                while (++i < dl->size() && depth) {
                    if ((*dl)[i].kind == Item::Kind::TransformPush) ++depth;
                    else if ((*dl)[i].kind == Item::Kind::TransformPop) --depth;
                }
                --i;
                continue;
            }
            case Item::Kind::Text: iw = it.run ? it.run->width : 0; iy = it.baseline - it.px; ih = it.px * 1.4f; break;
            case Item::Kind::Shadow: { const float p = it.blur * 1.5f + 2.f; ix -= p; iy -= p; iw += 2 * p; ih += 2 * p; break; }
            default: break;
            }
            add(ix, iy, iw, ih);
        }
        return x1 >= x0;
    }
    void layer_pop(size_t at) {
        float x0, y0, x1, y1;
        Item& push = (*dl)[at];
        if (!bounds_since(at, x0, y0, x1, y1)) { push.x = push.y = push.w = push.h = 0; }
        else { push.x = x0 - 1; push.y = y0 - 1; push.w = x1 - x0 + 2; push.h = y1 - y0 + 2; }
        Item pop; pop.kind = Item::Kind::LayerPop;
        dl->push_back(std::move(pop));
    }

    // ── backgrounds ─────────────────────────────────────────────────────────
    std::shared_ptr<const render::ImageBitmap> image_for(const std::string& url) {
        const render::ImageBitmap* bm = render::load_image(render::resolve_image(env.base_dir, url));
        if (!bm) return nullptr;
        return std::shared_ptr<const render::ImageBitmap>(bm, [](const render::ImageBitmap*) {});
    }

    void emit_gradient(const style::Gradient& g, float x, float y, float w, float h, const Radii& r) {
        if (g.stops.size() < 2 || w <= 0 || h <= 0) return;
        Item it; it.kind = Item::Kind::Gradient; it.x = x; it.y = y; it.w = w; it.h = h; it.radii = r;
        it.repeating = g.repeating;
        float line_len;
        if (g.conic) {
            it.conic = true;
            it.gcx = x + g.cx.resolve(w); it.gcy = y + g.cy.resolve(h);
            it.gdx = g.angle * 3.14159265f / 180.f;   // `from`
            line_len = 100.f;                          // stop positions are percentages of the turn
        } else if (!g.radial) {
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
        for (size_t i = 0; i < g.stops.size(); ++i) {
            if (i > 0 && !g.interp.empty() && t[i] > t[i - 1]) {   // a colour space other than sRGB: sampled between the stops
                constexpr int kSteps = 16;
                for (int k = 1; k < kSteps; ++k) {
                    const float f = float(k) / kSteps;
                    it.stops.push_back({style::mix_in_space(g.stops[i - 1].color, g.stops[i].color, f, g.interp), t[i - 1] + (t[i] - t[i - 1]) * f});
                }
            }
            it.stops.push_back({g.stops[i].color, t[i]});
        }
        dl->push_back(std::move(it));
    }

    void bg_layers(const ComputedStyle& s, const Rect& area, const Rect& clip, const Radii& cr) { bg_list(s.bg, area, clip, cr); }
    void bg_list(const std::vector<style::BgLayer>& layers, const Rect& area, const Rect& clip, const Radii& cr) {
        for (size_t li = layers.size(); li-- > 0;) {
            const style::BgLayer& L = layers[li];
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
            if (img && img->svg) {   // vector image: draw it at the tile's size
                auto r = render::image_at(*img, std::max(1, int(std::lround(tw))), std::max(1, int(std::lround(th))));
                if (!r) continue;
                img = r; nw = float(img->w); nh = float(img->h);
            }
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
        const bool native = native_border(b);
        const float nr = b.ctl ? 2.f : 3.f;
        const Radii rad = native ? Radii{nr, nr, nr, nr} : radii_of(s, r.w, r.h);
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
        if (native) {
            Item it; it.kind = Item::Kind::Border; it.x = r.x; it.y = r.y; it.w = r.w; it.h = r.h; it.radii = rad;
            const uint32_t col = b.ctl ? s.border_color[0] : 0xFF767676;
            for (int i = 0; i < 4; ++i) { it.bw[i] = 1; it.bc[i] = col; it.bs[i] = style::BorderStyle::Solid; }
            dl->push_back(std::move(it));
        } else if (!b.border_collapsed) borders(b, r, rad);
    }

    // Unstyled text fields/buttons keep the UA's `2px inset/outset #767676` in the cascade, but Chromium draws
    // them natively: a 1px #767676 hairline, 2px radius (3px for buttons).
    static bool native_border(const Box& b) {
        const ComputedStyle& s = *b.st;
        if (!b.node || b.border.t != 2 || b.border.l != 2) return false;
        const bool field = b.ctl && (b.ctl->type == Control::Type::Text || b.ctl->type == Control::Type::Password) &&
                           s.border_style[0] == style::BorderStyle::Inset;
        const bool button = (b.node->name == "button" || (b.node->name == "input" && !b.ctl)) && s.border_style[0] == style::BorderStyle::Outset &&
                            s.bc_current[0];
        return (field && s.border_color[0] == 0xFF767676) || button;
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
        if (b.ctl) { if (s.visibility == Visibility::Visible) paint_control(b); return; }
        if (s.visibility != Visibility::Visible) return;
        if (!b.img && !b.media && b.node && b.node->is_element("iframe")) {   // the framed document, drawn into a picture
            const float fw = std::round(b.w - b.border.h() - b.padding.h()), fh = std::round(b.h - b.border.v() - b.padding.v());
            if (auto pic = render_frame(env.font, *b.node, env.base_dir, int(fw), int(fh))) {
                b.img = pic; b.nat_w = float(pic->w); b.nat_h = float(pic->h);
            }
        }
        if (!b.img && !b.media) return;
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
        const float px = cx + s.object_pos_x.resolve(cw - dw), py = cy + s.object_pos_y.resolve(ch - dh);   // object-position
        // intersect the placed image with the content box
        const float ix0 = std::max(px, cx), iy0 = std::max(py, cy), ix1 = std::min(px + dw, cx + cw), iy1 = std::min(py + dh, cy + ch);
        if (ix1 <= ix0 || iy1 <= iy0) return;
        std::shared_ptr<const render::ImageBitmap> pic = b.img;
        float nat_w = b.nat_w, nat_h = b.nat_h;
        if (pic && pic->svg) {   // vector image: draw it at the size it is placed
            auto r = render::image_at(*pic, std::max(1, int(std::lround(dw))), std::max(1, int(std::lround(dh))));
            if (!r) return;
            pic = r; nat_w = float(r->w); nat_h = float(r->h);
        }
        Item it; it.kind = Item::Kind::Image; it.img = pic;
        it.x = sn(ix0); it.y = sn(iy0); it.w = sn(ix1) - sn(ix0); it.h = sn(iy1) - sn(iy0);
        it.sx = (ix0 - px) / dw * nat_w; it.sy = (iy0 - py) / dh * nat_h;
        it.sw = (ix1 - ix0) / dw * nat_w; it.sh = (iy1 - iy0) / dh * nat_h;
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

    // ── clipping and scrollbars ─────────────────────────────────────────────
    // Visible part of `b`: the padding boxes of the clipping ancestors, intersected.
    void clip_of(const Box& b, HitRect& h) const {
        for (const Box* p = b.lparent ? b.lparent : b.parent; p; p = p->lparent ? p->lparent : p->parent) {
            if (p->kind == Box::Kind::Inline || !p->st) continue;
            if (b.st && b.st->position == Position::Fixed) break;
            if (clips(*p) && p->kind == Box::Kind::Block) {
                h.clip_x0 = std::max(h.clip_x0, sn(p->ax + p->border.l)); h.clip_y0 = std::max(h.clip_y0, sn(p->ay + p->border.t));
                h.clip_x1 = std::min(h.clip_x1, sn(p->ax + p->w - p->border.r)); h.clip_y1 = std::min(h.clip_y1, sn(p->ay + p->h - p->border.b));
            }
        }
    }
    static bool dark_page(const Tree& t) { return t.root && t.root->node && t.root->node->attr_or("data-luxium-scheme") == "dark"; }
    // Vertical scrollbar track of a scroll container (overlay: it takes no layout space).
    static void sb_track(const Box& b, float& x, float& y, float& w, float& h) {
        const bool thin = b.st->scrollbar_thin;
        w = thin ? 8.f : 12.f;
        x = sn(b.ax + b.w - b.border.r - w); y = sn(b.ay + b.border.t);
        h = sn(b.ay + b.h - b.border.b) - y;
    }
    void scrollbar_h(Box& b) {
        if (!b.node || b.kind != Box::Kind::Block || b.max_sx <= 0 || (b.st->overflow_x != Overflow::Auto && b.st->overflow_x != Overflow::Scroll)) return;
        const ComputedStyle& s = *b.st;
        const bool dark = dark_page(tree);
        const float t = s.scrollbar_thin ? 8.f : 12.f;
        const float x = sn(b.ax + b.border.l), w = sn(b.ax + b.w - b.border.r) - x, y = sn(b.ay + b.h - b.border.b - t);
        const uint32_t track = s.scrollbar_set ? s.scrollbar_track : (dark ? 0xFF2C2C2C : 0xFFFCFCFC);
        const uint32_t thumb = s.scrollbar_set ? s.scrollbar_thumb : (dark ? 0xFF9F9F9F : 0xFF8B8B8B);
        if (s.scrollbar_set && !has_alpha(s.scrollbar_thumb)) return;
        rect(x, y, w, t, track);
        const float tl = std::max(24.f, w * w / (w + b.max_sx));
        const float tx = x + b.node->scroll_x / b.max_sx * (w - tl);
        rect(sn(tx) + 1.5f, y + 1.5f, tl - 3.f, t - 3.f, thumb, {(t - 3.f) / 2, (t - 3.f) / 2, (t - 3.f) / 2, (t - 3.f) / 2});
    }
    void scrollbar(Box& b) {
        scrollbar_h(b);
        if (!scrolls_y(b)) return;
        float x, y, w, h;
        sb_track(b, x, y, w, h);
        const bool dark = dark_page(tree);
        const ComputedStyle& s = *b.st;
        const uint32_t track = s.scrollbar_set ? s.scrollbar_track : (dark ? 0xFF2C2C2C : 0xFFFCFCFC);
        const uint32_t thumb = s.scrollbar_set ? s.scrollbar_thumb : (dark ? 0xFF9F9F9F : 0xFF8B8B8B);
        if (s.scrollbar_set && !has_alpha(s.scrollbar_thumb)) return;
        rect(x, y, w, h, track);
        const float view = h, total = view + b.max_sy;
        const float tl = std::max(24.f, h * view / total);
        const float ty = y + b.node->scroll_y / b.max_sy * (h - tl);
        rect(x + 1.5f, sn(ty) + 1.5f, w - 3.f, tl - 3.f, thumb, {(w - 3.f) / 2, (w - 3.f) / 2, (w - 3.f) / 2, (w - 3.f) / 2});
    }
    static bool scrolls_y(const Box& b) {
        return b.node && b.kind == Box::Kind::Block && b.max_sy > 0 &&
               (b.st->overflow_y == Overflow::Auto || b.st->overflow_y == Overflow::Scroll);
    }

    // ── form controls ───────────────────────────────────────────────────────
    // text-shadow: each shadow is the run again, offset and in its colour; blurred ones go through a blur layer.
    // The first shadow is on top, so they are painted last to first, all below the text.
    void text_shadows(const Item& text, const ComputedStyle& s) {
        for (size_t i = s.text_shadow.size(); i-- > 0;) {
            const style::Shadow& sh = s.text_shadow[i];
            if (!has_alpha(sh.color)) continue;
            Item t = text;
            t.x += sh.x; t.baseline += sh.y; t.color = sh.color;
            if (sh.blur <= 0) { dl->push_back(std::move(t)); continue; }
            const size_t at = layer_push(1.f);
            style::FilterFn f; f.kind = style::FilterFn::Kind::Blur; f.a = sh.blur / 2.f;   // shadow blur radius = 2 sigma
            (*dl)[at].filters.push_back(f);
            dl->push_back(std::move(t));
            layer_pop(at);
        }
    }

    void text_item(const std::string& txt, const ComputedStyle& s, float x, float baseline, uint32_t color) {
        if (txt.empty()) return;
        auto run = std::make_shared<render::ShapedRun>(env.font.shape_at(txt, face_of(env, s), s.font_size));
        if (run->positions.empty()) return;
        Item it; it.kind = Item::Kind::Text; it.x = x; it.baseline = baseline; it.run = run; it.face = face_of(env, s);
        it.px = s.font_size; it.color = color;
        dl->push_back(std::move(it));
    }
    float text_width(const std::string& txt, const ComputedStyle& s) {
        return txt.empty() ? 0.f : env.font.shape_at(txt, face_of(env, s), s.font_size).width;
    }
    static uint32_t fade(uint32_t c, float k) {   // scale alpha
        return (uint32_t(float(c >> 24) * k) << 24) | (c & 0xFFFFFF);
    }
    void circle(float x, float y, float d, uint32_t color) { rect(x, y, d, d, color, {d / 2, d / 2, d / 2, d / 2}); }

    void paint_control(Box& b) {
        using T = Control::Type;
        const ComputedStyle& s = *b.st;
        const Control& c = *b.ctl;
        const float cx = sn(b.ax + b.border.l + b.padding.l), cy = sn(b.ay + b.border.t + b.padding.t);
        const float cw = sn(b.ax + b.w - b.border.r - b.padding.r) - cx, ch = sn(b.ay + b.h - b.border.b - b.padding.b) - cy;
        const uint32_t accent = s.accent ? s.accent : 0xFF0075FF;
        const float dim = c.disabled ? 0.38f : 1.f;
        const render::FaceMetrics m = env.font.metrics(face_of(env, s), s.font_size);
        const float line = line_height_px(env, s);
        const float base = cy + m.ascent + (line - (m.ascent + m.descent)) / 2;
        const bool focused = b.node && (b.node->state & dom::Node::Focus);
        switch (c.type) {
        case T::Checkbox: {
            if (s.appearance_none) break;
            const uint32_t edge = c.checked ? accent : fade(0xFF767676, dim);
            rect(cx, cy, 13, 13, edge, {2, 2, 2, 2});
            rect(cx + 1, cy + 1, 11, 11, c.checked ? accent : fade(0xFFFFFFFF, dim), {1, 1, 1, 1});
            if (c.checked) {   // tick: two strokes stamped as 2x2 squares
                const float pts[][2] = {{3, 6}, {4, 7}, {5, 8}, {6, 9}, {7, 8}, {8, 7}, {9, 6}, {10, 5}, {10, 4}};
                for (auto& p : pts) rect(cx + p[0] - 0.f, cy + p[1] - 1.f, 2, 2, 0xFFFFFFFF);
            }
            break;
        }
        case T::Radio: {
            if (s.appearance_none) break;
            circle(cx, cy, 13, c.checked ? accent : fade(0xFF767676, dim));
            circle(cx + 1, cy + 1, 11, fade(0xFFFFFFFF, dim));
            if (c.checked) { circle(cx + 2, cy + 2, 9, accent); circle(cx + 3, cy + 3, 7, 0xFFFFFFFF); circle(cx + 4, cy + 4, 5, accent); }
            break;
        }
        case T::Range: {
            if (s.appearance_none) break;
            const float tx = cx + 1, tw = cw - 2, ty = cy + ch / 2 - 2, tf = tx + c.frac * (tw - 16) + 8;
            rect(tx, ty, tw, 4, fade(0xFFB9B9B9, dim), {2, 2, 2, 2});
            rect(tx, ty, tf - tx, 4, fade(accent, dim), {2, 0, 0, 2});
            circle(tf - 8, cy, 16, fade(accent, dim));
            break;
        }
        case T::Progress: case T::Meter: {
            if (s.appearance_none) break;
            const bool prog = c.type == T::Progress;
            const float tx = cx, tw = cw, th = std::min(ch, 8.f), ty = cy + (ch - th) / 2;   // Blink paints an 8px track in the 16px box
            rect(tx, ty, tw, th, fade(0xFFB2B2B2, dim), {th / 2, th / 2, th / 2, th / 2});
            rect(tx + 1, ty + 1, tw - 2, th - 2, fade(0xFFEEEEEE, dim), {th / 2 - 1, th / 2 - 1, th / 2 - 1, th / 2 - 1});
            const float fw = (tw - 2) * c.frac;
            if (fw > 0 && !(prog && c.indeterminate)) {
                const uint32_t col = prog ? fade(0xFF0075FF, dim) : fade(c.meter_state == 0 ? 0xFF118811 : c.meter_state == 1 ? 0xFFFFCC00 : 0xFFDD2222, dim);
                const float r = th / 2 - 1;
                rect(tx + 1, ty + 1, fw, th - 2, col, {r, fw >= tw - 2 ? r : 0.f, fw >= tw - 2 ? r : 0.f, r});
            }
            break;
        }
        case T::Color: {
            if (s.appearance_none) break;
            rect(cx, cy, cw, ch, fade(0xFF767676, dim), {2, 2, 2, 2});
            rect(cx + 1, cy + 1, cw - 2, ch - 2, fade(0xFFEFEFEF, dim), {1, 1, 1, 1});
            rect(cx + 4, cy + 4, cw - 8, ch - 8, fade(0xFF777777, dim));
            rect(cx + 5, cy + 5, cw - 10, ch - 10, fade(c.color, dim));
            break;
        }
        case T::File: {
            const float bw = text_width("Choose File", s) + 16;
            rect(cx, cy, bw, ch, fade(0xFFEFEFEF, dim), {3, 3, 3, 3});
            Item ring; ring.kind = Item::Kind::Border; ring.x = cx; ring.y = cy; ring.w = bw; ring.h = ch; ring.radii = {3, 3, 3, 3};
            for (int i = 0; i < 4; ++i) { ring.bw[i] = 1; ring.bc[i] = fade(0xFF767676, dim); ring.bs[i] = style::BorderStyle::Solid; }
            dl->push_back(std::move(ring));
            text_item("Choose File", s, cx + 8, cy + 3 + m.ascent, s.color);
            text_item(c.text.empty() ? "No file chosen" : c.text, s, cx + bw + 4, cy + 3 + m.ascent, s.color);
            break;
        }
        case T::Select: {
            clip_push(cx, cy, cw, ch);
            text_item(c.text, s, cx + (s.appearance_none ? 0.f : 4.f), cy + (s.appearance_none ? 0.f : 1.f) + m.ascent, s.color);
            clip_pop();
            if (!s.appearance_none) {   // chevron at the right edge
                const float ax = b.ax + b.w - b.border.r - 15, ay = cy + ch / 2 - 1;
                for (int i = 0; i < 4; ++i) rect(ax + i, ay + i - 1, 2, 2, fade(0xFF101010, dim));
                for (int i = 0; i < 4; ++i) rect(ax + 7 - i, ay + i - 1, 2, 2, fade(0xFF101010, dim));
            }
            break;
        }
        case T::Text: case T::Password: case T::Textarea: {
            const bool pw = c.type == T::Password;
            const std::string shown = pw ? password_mask(c.text) : c.text;
            const bool multi = c.type == T::Textarea;
            clip_push(cx, cy, cw, ch);
            const uint32_t fg = s.color;
            if (shown.empty() && !c.placeholder.empty() && !(focused && false))
                text_item(c.placeholder, s, cx, base, c.ph_color);
            const float sx = (b.node ? b.node->scroll_x : 0.f);
            if (!multi) {
                // selection + caret positions are byte offsets in the VALUE; password bullets are 3 bytes each
                auto at = [&](int byte) {
                    if (!pw) return text_width(shown.substr(0, size_t(std::min<int>(byte, int(shown.size())))), s);
                    int n = 0;
                    for (int i = 0; i < byte && i < int(c.text.size()); ++i) if ((static_cast<unsigned char>(c.text[i]) & 0xC0) != 0x80) ++n;
                    return text_width(shown.substr(0, size_t(n) * 3), s);
                };
                if (focused && b.node->caret != b.node->anchor) {
                    const float a = at(std::min(b.node->caret, b.node->anchor)), z = at(std::max(b.node->caret, b.node->anchor));
                    rect(cx - sx + a, cy, z - a, line, 0xFFB3D4FC);
                }
                const bool composing = focused && dom::g_ime.node == b.node && !dom::g_ime.text.empty();
                if (composing) {   // IME preedit sits at the caret, underlined
                    const size_t at_b = pw ? shown.size() : size_t(std::min<int>(b.node->caret, int(shown.size())));
                    const std::string pre = dom::g_ime.text;
                    text_item(shown.substr(0, at_b) + pre + shown.substr(at_b), s, cx - sx, base, fg);
                    const float x0 = cx - sx + text_width(shown.substr(0, at_b), s);
                    rect(x0, base + 2, text_width(pre, s), 1, fg);
                } else text_item(shown, s, cx - sx, base, fg);
                if (focused && (b.node->state & dom::Node::CaretOn) && !c.readonly)
                    rect(cx - sx + at(b.node->caret), cy, 1, line, s.color);
            } else {
                size_t pos = 0; float y = base;
                std::string rest = shown;
                for (;;) {
                    const size_t nl = rest.find('\n', pos);
                    text_item(rest.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos), s, cx, y, fg);
                    if (nl == std::string::npos) break;
                    pos = nl + 1; y += line;
                }
            }
            clip_pop();
            break;
        }
        }
    }

    // outline: outside the border edge (+ offset); `auto` is the keyboard focus ring.
    void outline(Box& b) {
        const ComputedStyle& s = *b.st;
        if (s.visibility != Visibility::Visible || s.outline_style == style::BorderStyle::None) return;
        float w = s.outline_w, off = s.outline_offset;
        uint32_t col = s.outline_current ? s.color : s.outline_color;
        if (s.outline_auto) { w = 2; col = 0xFF101010; off = 0; }
        if (w <= 0 || !has_alpha(col)) return;
        const float x0 = sn(b.ax - off - w), y0 = sn(b.ay - off - w), x1 = sn(b.ax + b.w + off + w), y1 = sn(b.ay + b.h + off + w);
        Item it; it.kind = Item::Kind::Border; it.x = x0; it.y = y0; it.w = x1 - x0; it.h = y1 - y0;
        const Radii rr = radii_of(s, b.w, b.h);
        const float g = w + off;
        it.radii = {rr.tl > 0 ? rr.tl + g : 0, rr.tr > 0 ? rr.tr + g : 0, rr.br > 0 ? rr.br + g : 0, rr.bl > 0 ? rr.bl + g : 0};
        for (int i = 0; i < 4; ++i) { it.bw[i] = w; it.bc[i] = col; it.bs[i] = s.outline_auto ? style::BorderStyle::Solid : s.outline_style; }
        dl->push_back(std::move(it));
        if (s.outline_auto) {   // white inner halo like Chromium's two-tone ring
            Item in = it; in.x = x0 + w; in.y = y0 + w; in.w -= 2 * w; in.h -= 2 * w; in.radii = {};
            for (int i = 0; i < 4; ++i) { in.bw[i] = 1; in.bc[i] = 0xFFFFFFFF; }
            dl->push_back(std::move(in));
        }
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
                    if (ib.node && (ib.node->name == "a" || ib.node->name == "label" || ib.node->has_attr("id")) && r.w > 0) {
                        HitRect h{ib.node, r.x, r.y, r.w, r.h, false};
                        clip_of(blk, h);
                        h.sticky = sticky_cur; xform_hit(h);
                        if (clips(blk) && blk.kind == Box::Kind::Block) {   // the block itself clips its lines too
                            h.clip_x0 = std::max(h.clip_x0, sn(blk.ax + blk.border.l)); h.clip_y0 = std::max(h.clip_y0, sn(blk.ay + blk.border.t));
                            h.clip_x1 = std::min(h.clip_x1, sn(blk.ax + blk.w - blk.border.r)); h.clip_y1 = std::min(h.clip_y1, sn(blk.ay + blk.h - blk.border.b));
                        }
                        out.hits.push_back(h);
                    }
                    break;
                }
                case LineFrag::Kind::Text: {
                    const ComputedStyle& ts = f.box ? *f.box->st : *blk.st;
                    if (ts.visibility != Visibility::Visible || !f.run) break;
                    Item it; it.kind = Item::Kind::Text;
                    it.x = cx + f.x; it.baseline = cy + line.y + f.baseline;
                    it.run = f.run; it.face = f.face; it.px = f.px; it.color = ts.color;
                    if (!f.run->positions.empty()) { text_shadows(it, ts); dl->push_back(std::move(it)); }
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
        if (mk->img) {
            Item it; it.kind = Item::Kind::Image; it.img = mk->img;
            it.w = mk->nat_w; it.h = mk->nat_h; it.x = sn(left - it.w - 8.f); it.y = sn(baseline - it.h);
            it.sx = 0; it.sy = 0; it.sw = float(mk->img->w); it.sh = float(mk->img->h);
            dl->push_back(std::move(it));
        } else if (mk->marker_shape) {
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
        outline(b);
        if (b.node && b.kind != Box::Kind::Inline && !b.anonymous) {
            const std::string& tg = b.node->name;
            if (b.ctl) {
                const ComputedStyle& cs = *b.st;
                HitRect h{b.node, sn(b.ax), sn(b.ay), b.w, b.h, false};
                h.cursor = cs.cursor; h.ctl = int(b.ctl->type);
                h.tx = sn(b.ax + b.border.l + b.padding.l); h.ty = sn(b.ay + b.border.t + b.padding.t);
                h.th = line_height_px(env, cs); h.px = cs.font_size; h.face = face_of(env, cs).id;
                h.password = b.ctl->type == Control::Type::Password;
                clip_of(b, h);
                h.sticky = sticky_cur; xform_hit(h);
                out.hits.push_back(h);
            } else if (b.node->has_attr("id") || tg == "a" || tg == "button" || b.media) {
                HitRect h{b.node, sn(b.ax), sn(b.ay), b.w, b.h, tg == "button"};
                h.cursor = b.st->cursor;
                clip_of(b, h);
                h.sticky = sticky_cur; xform_hit(h);
                out.hits.push_back(h);
            }
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
        if (s.position == Position::Sticky && sticky_cur < 0 && dl != &out.fixed && sticky_ok(sc)) {
            const auto& in = s.inset;
            render::StickyGroup g;
            g.has_top = !in[0].is_auto; g.has_bottom = !in[2].is_auto;
            if (g.has_top || g.has_bottom) {
                const Box* cbx = sc.lparent ? sc.lparent : sc.parent;
                while (cbx && cbx->kind == Box::Kind::Inline) cbx = cbx->parent;
                g.top = g.has_top ? in[0].resolve(cbx ? cbx->content_h() : 0) : 0;
                g.bottom = g.has_bottom ? in[2].resolve(cbx ? cbx->content_h() : 0) : 0;
                g.orig_y = sn(sc.ay); g.h = sc.h;
                g.cb_top = cbx ? cbx->ay + cbx->border.t + cbx->padding.t : 0;
                g.cb_bottom = cbx ? cbx->ay + cbx->h - cbx->border.b - cbx->padding.b : 1e9f;
                const int idx = int(out.sticky.size());
                out.sticky.push_back(std::move(g));
                sticky_roots[&sc] = idx;
                auto* saved = dl;
                std::vector<paint::Item> tmp;
                paint::DisplayList mine;
                dl = &mine;
                sticky_cur = idx;
                paint_sc(sc, bg);
                sticky_cur = -1;
                dl = saved;
                out.sticky[size_t(idx)].list = std::move(mine);
                return;
            }
        }
        // transform: a pure translation shifts the subtree (and its hit boxes) by whole pixels; anything else
        // draws it into a layer that the rasterizer resamples through the matrix
        Affine own;
        bool moved = false, general = false;
        if (s.tf_linear()) {
            const float ox = sc.ax + s.origin_x.resolve(sc.w), oy = sc.ay + s.origin_y.resolve(sc.h);
            const float tx = s.translate_x.resolve(sc.w), ty = s.translate_y.resolve(sc.h);
            own.m[0] = s.tf[0]; own.m[1] = s.tf[1]; own.m[2] = s.tf[2]; own.m[3] = s.tf[3];
            own.m[4] = ox + tx - own.m[0] * ox - own.m[2] * oy; own.m[5] = oy + ty - own.m[1] * ox - own.m[3] * oy;
            moved = general = true;
        } else {
            own.m[4] = std::round(s.translate_x.resolve(sc.w)); own.m[5] = std::round(s.translate_y.resolve(sc.h));
            moved = own.m[4] != 0 || own.m[5] != 0;
        }
        size_t tf_at = 0;
        const Affine saved_cur = tcur;
        if (moved) {
            Item it;
            if (general) { it.kind = Item::Kind::TransformPush; std::copy(own.m, own.m + 6, it.m); }
            else { it.kind = Item::Kind::ShiftPush; it.x = own.m[4]; it.y = own.m[5]; }
            dl->push_back(std::move(it));
            tf_at = dl->size() - 1;
            tcur = then(own, tcur);
            tf_of[&sc] = own;
        }
        struct Unshift {
            Painter* p; bool on, general; size_t at; Affine saved;
            ~Unshift() {
                if (!on) return;
                if (general) {   // source bounds of the layer = everything drawn inside
                    float x0, y0, x1, y1;
                    Item& push = (*p->dl)[at];
                    if (!p->bounds_since(at, x0, y0, x1, y1)) { push.x = push.y = push.w = push.h = 0; }
                    else { push.x = x0 - 1; push.y = y0 - 1; push.w = x1 - x0 + 2; push.h = y1 - y0 + 2; }
                }
                Item it; it.kind = general ? Item::Kind::TransformPop : Item::Kind::ShiftPop;
                p->dl->push_back(std::move(it));
                p->tcur = saved;
            }
        } unshift{this, moved, general, tf_at, saved_cur};
        size_t layer = 0;
        const bool lay = s.opacity < 1.f || !s.filter.empty() || !s.backdrop_filter.empty() || !s.mask.empty() || s.clip_path.kind != style::ClipPath::Kind::None || s.bg_clip_text;
        if (lay) {
            layer = layer_push(s.opacity);
            const float x0 = sn(sc.ax), y0 = sn(sc.ay), x1 = sn(sc.ax + sc.w), y1 = sn(sc.ay + sc.h);
            {
                Item& L = (*dl)[layer];
                L.filters = s.filter; L.backdrop = s.backdrop_filter;
                L.cx = x0; L.cy = y0; L.cw = x1 - x0; L.ch = y1 - y0;
                L.radii = radii_of(s, x1 - x0, y1 - y0);
            }
            if (!s.mask.empty()) {   // mask-origin/-clip: border-box
                auto m = std::make_shared<paint::DisplayList>();
                paint::DisplayList* saved = dl;
                dl = m.get();
                const Rect area{x0, y0, x1 - x0, y1 - y0};
                bg_list(s.mask, area, area, {});
                dl = saved;
                (*dl)[layer].mask = std::move(m);
            }
            if (s.clip_path.kind != style::ClipPath::Kind::None) (*dl)[layer].clip = clip_shape(sc, s.clip_path);
            if (s.bg_clip_text) {   // background-clip: text: the mask is the element's own glyphs, drawn opaque
                auto m = std::make_shared<paint::DisplayList>();
                paint::DisplayList* saved = dl;
                dl = m.get();
                content(sc);
                dl = saved;
                paint::DisplayList glyphs;
                for (auto& it : *m)
                    if (it.kind == Item::Kind::Text || it.kind == Item::Kind::Line) { it.color = 0xFF000000u; glyphs.push_back(std::move(it)); }
                (*dl)[layer].mask = std::make_shared<paint::DisplayList>(std::move(glyphs));
            }
        }
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
        if (cl) scrollbar(sc);
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
            {
                HitRect ar{k->node, k->ax, k->ay, k->w, k->h, false};
                ar.cursor = k->st->cursor;
                Painter::map_hit(ar, p.chain_of(k));
                p.clip_of(*k, ar);
                for (const Box* q = k; q; q = q->parent) { auto f = p.sticky_roots.find(q); if (f != p.sticky_roots.end()) { ar.sticky = f->second; break; } }
                if (scrolls(k)) {
                    ar.max_sx = k->max_sx; ar.max_sy = k->max_sy;
                    if (Painter::scrolls_y(*k)) Painter::sb_track(*k, ar.sb_x, ar.sb_y, ar.sb_w, ar.sb_h);
                }
                out.areas.push_back(ar);
            }

    // data-t boxes for the css-tests probe
    for (Box* k : tree.order)
        if (k->node && !k->anonymous && k->node->has_attr("data-t") && (k->kind == Box::Kind::Block || k->kind == Box::Kind::Replaced))
            {   // getBoundingClientRect: the bounding box after the element's and its ancestors' transforms
                HitRect r{k->node, k->ax, k->ay, k->w, k->h, false};
                Painter::map_hit(r, p.chain_of(k));
                out.boxes.push_back({k->node->attr_or("data-t"), r.x, r.y, r.w, r.h});
            }
}

} // namespace luxium::layout
