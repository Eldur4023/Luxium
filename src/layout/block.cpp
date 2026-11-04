// Block formatting: widths and heights, margin collapsing, floats,
// absolute positioning, replaced elements and intrinsic sizes.

#include <layout/box.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace luxium::layout {

using style::Clear;
using style::Float;
using style::Overflow;
using style::Position;

// ── BFC float bookkeeping ───────────────────────────────────────────────────

float Bfc::bottom_of(bool left, bool right) const {
    float b = -1e9f;
    for (const auto& f : floats)
        if ((f.left && left) || (!f.left && right)) b = std::max(b, f.y1);
    return b;
}

void Bfc::avail(float y, float h, float x0, float x1, float& l, float& r) const {
    l = x0; r = x1;
    const float yb = y + std::max(h, 0.01f);
    for (const auto& f : floats) {
        if (f.y1 <= y + 0.001f || f.y0 >= yb - 0.001f) continue;
        if (!f.poly.empty()) {   // shape-outside: how far the shape reaches into this band of lines
            float mx = -1e30f, mn = 1e30f;
            bool hit = false;
            const size_t n = f.poly.size() / 2;
            for (size_t i = 0; i < n; ++i) {
                float ax = f.poly[2 * i], ay = f.poly[2 * i + 1];
                float bx = f.poly[2 * ((i + 1) % n)], by = f.poly[2 * ((i + 1) % n) + 1];
                if (ay > by) { std::swap(ax, bx); std::swap(ay, by); }
                if (by < y || ay > yb) continue;
                const float t0 = by > ay ? std::max(0.f, (y - ay) / (by - ay)) : 0.f, t1 = by > ay ? std::min(1.f, (yb - ay) / (by - ay)) : 1.f;
                const float x_a = ax + (bx - ax) * t0, x_b = ax + (bx - ax) * t1;
                mx = std::max(mx, std::max(x_a, x_b)); mn = std::min(mn, std::min(x_a, x_b));
                hit = true;
            }
            if (!hit) continue;
            if (f.left) l = std::max(l, std::min(f.x1, mx + f.shape_margin));
            else r = std::min(r, std::max(f.x0, mn - f.shape_margin));
            continue;
        }
        if (f.left) l = std::max(l, f.x1);
        else r = std::min(r, f.x0);
    }
}

// ── small helpers ───────────────────────────────────────────────────────────

float resolve_len(const Len& l, float base) {
    if (l.is_auto) return 0;
    return l.resolve(base);
}

static float snap_border(float w) { return w > 0 ? std::max(1.f, std::floor(w)) : 0.f; }

void resolve_edges(Box& b, float cb_w) {
    const ComputedStyle& s = *b.st;
    const float base = cb_w >= 0 ? cb_w : 0;
    auto m = [&](const Len& l, bool& is_auto) { is_auto = l.is_auto; return l.is_auto ? 0.f : l.resolve(base); };
    b.margin.t = m(s.margin[0], b.mar_t_auto);
    b.margin.r = m(s.margin[1], b.mar_r_auto);
    b.margin.b = m(s.margin[2], b.mar_b_auto);
    b.margin.l = m(s.margin[3], b.mar_l_auto);
    b.padding.t = std::max(0.f, s.padding[0].resolve(base));
    b.padding.r = std::max(0.f, s.padding[1].resolve(base));
    b.padding.b = std::max(0.f, s.padding[2].resolve(base));
    b.padding.l = std::max(0.f, s.padding[3].resolve(base));
    auto bw = [&](int i) {
        const auto st = s.border_style[i];
        return (st == style::BorderStyle::None || st == style::BorderStyle::Hidden) ? 0.f : snap_border(s.border_w[i]);
    };
    b.border.t = bw(0); b.border.r = bw(1); b.border.b = bw(2); b.border.l = bw(3);
    if (b.border_collapsed) b.border = b.coll_border;   // table model decides
}

bool establishes_bfc(const Box& b) {
    if (!b.st) return false;
    const ComputedStyle& s = *b.st;
    if (!b.parent) return true;                       // root
    if (b.is_float() || b.is_abs()) return true;
    if (b.is_atomic_inline()) return true;
    if (s.overflow_x != Overflow::Visible && s.overflow_x != Overflow::Clip) return true;
    if (s.overflow_y != Overflow::Visible && s.overflow_y != Overflow::Clip) return true;
    if (s.multicol()) return true;
    switch (s.display) {
    case Display::FlowRoot: case Display::Flex: case Display::Grid: case Display::Table:
    case Display::TableCell: case Display::TableCaption: case Display::InlineBlock:
    case Display::InlineFlex: case Display::InlineGrid: case Display::InlineTable: return true;
    default: break;
    }
    return false;
}

bool is_block_flow(const Box& b) {
    if (b.kind != Box::Kind::Block) return false;
    switch (b.st->display) {
    case Display::Block: case Display::ListItem: case Display::FlowRoot: case Display::InlineBlock:
    case Display::TableCell: case Display::TableCaption: return true;
    default: return false;
    }
}

static bool has_real_inline(const Box& b) {
    for (const auto& k : b.kids) {
        if (k->kind == Box::Kind::Marker || k->out_of_flow()) continue;
        if (k->inline_level()) return true;
    }
    return false;
}

static bool zero_len(const Len& l) { return l.is_auto || (l.px == 0 && l.pct == 0); }

// vertical padding/border present (style-level check, usable before resolve)
static bool has_vertical_bp(const ComputedStyle& s) {
    return s.border_w[0] > 0 || s.border_w[2] > 0 || !zero_len(s.padding[0]) || !zero_len(s.padding[2]);
}

// An empty block whose top and bottom margins collapse together.
static bool collapses_through(const Box& b) {
    if (b.kind != Box::Kind::Block || !is_block_flow(b) || establishes_bfc(b)) return false;
    const ComputedStyle& s = *b.st;
    if (!s.height.is_auto && !(s.height.has_pct())) return false;
    if (!s.height.is_auto && s.height.px != 0) return false;
    if (!zero_len(s.min_height)) return false;
    if (has_vertical_bp(s)) return false;
    if (has_real_inline(b)) {
        for (const auto& k : b.kids)
            if (!k->out_of_flow() && k->kind != Box::Kind::Marker &&
                !(k->kind == Box::Kind::Text && k->text.find_first_not_of(" \t\r\n") == std::string::npos))
                return false;
    }
    for (const auto& k : b.kids)
        if (!k->out_of_flow() && k->kind != Box::Kind::Marker && !k->inline_level() && !collapses_through(*k)) return false;
    return true;
}

// Can the box's top margin merge with its first in-flow child's?
static bool merges_first(const Box& b) {
    if (b.kind != Box::Kind::Block || !is_block_flow(b) || establishes_bfc(b)) return false;
    const ComputedStyle& s = *b.st;
    return s.border_w[0] == 0 && zero_len(s.padding[0]);
}
static bool merges_last(const Box& b) {
    if (b.kind != Box::Kind::Block || !is_block_flow(b) || establishes_bfc(b)) return false;
    const ComputedStyle& s = *b.st;
    return s.border_w[2] == 0 && zero_len(s.padding[2]) && (s.height.is_auto || s.height.has_pct() ) &&
           zero_len(s.min_height);
}

// The margins at a box's top edge that collapse into one (own + first-child chain).
static Strut top_chain(Box& b, float cb_w) {
    resolve_edges(b, cb_w);
    Strut s;
    s.add(b.margin.t);
    if (!merges_first(b) || has_real_inline(b)) return s;
    for (auto& k : b.kids) {
        if (k->kind == Box::Kind::Marker || k->out_of_flow()) continue;
        if (k->inline_level()) break;
        Strut cs = top_chain(*k, cb_w);
        s.merge(cs);
        if (collapses_through(*k)) { s.add(k->margin.b); continue; }
        break;
    }
    return s;
}

// ── floats ──────────────────────────────────────────────────────────────────

void place_float(Env& env, Box& f, Bfc& bfc, float content_w, float ox, float oy, float y_top) {
    Constraint fc;
    fc.cb_w = content_w; fc.cb_h = -1; fc.shrink = true; fc.avail_w = content_w;
    resolve_edges(f, content_w);
    layout_box(env, f, fc);
    const float mw = f.outer_w(), mh = f.outer_h();
    const bool left = f.st->float_ == Float::Left;
    float y = y_top;
    // floats never sit above earlier floats, and honor `clear`
    for (const auto& o : bfc.floats) y = std::max(y, o.y0);
    if (f.st->clear == Clear::Left || f.st->clear == Clear::Both) y = std::max(y, bfc.bottom_of(true, false));
    if (f.st->clear == Clear::Right || f.st->clear == Clear::Both) y = std::max(y, bfc.bottom_of(false, true));
    if (f.st->clear == Clear::Both) y = std::max(y, bfc.bottom_of(true, true));
    float x = ox;
    for (int guard = 0; guard < 1000; ++guard) {
        float l, r;
        bfc.avail(y, mh, ox, ox + content_w, l, r);
        if (r - l + 0.01f >= mw || (l <= ox + 0.01f && r >= ox + content_w - 0.01f)) {
            x = left ? l : r - mw;
            break;
        }
        // jump below the nearest overlapping float and retry
        float next = 1e9f;
        for (const auto& o : bfc.floats)
            if (o.y1 > y + 0.001f && o.y0 < y + std::max(mh, 0.01f)) next = std::min(next, o.y1);
        if (next >= 1e9f) { x = left ? l : r - mw; break; }
        y = next;
    }
    bfc.floats.push_back({x, x + mw, y, y + mh, left, &f});
    if (f.st->shape_outside.kind != style::ClipPath::Kind::None) {
        const style::ClipPath& cp = f.st->shape_outside;
        // reference box in BFC coordinates
        float bx = x, by = y, bw = mw, bh = mh;
        auto shrink = [&](const Edges& e, float k) { bx += k * e.l; by += k * e.t; bw -= k * e.h(); bh -= k * e.v(); };
        if (cp.box == 0) shrink(f.margin, 1);
        else if (cp.box == 1) { shrink(f.margin, 1); shrink(f.border, 1); }
        else if (cp.box == 2) { shrink(f.margin, 1); shrink(f.border, 1); shrink(f.padding, 1); }
        bw = std::max(bw, 0.f); bh = std::max(bh, 0.f);
        std::vector<float>& P = bfc.floats.back().poly;
        using K = style::ClipPath::Kind;
        if (cp.kind == K::Inset) {
            const float t = cp.edge[0].resolve(bh), r = cp.edge[1].resolve(bw), b = cp.edge[2].resolve(bh), l = cp.edge[3].resolve(bw);
            P = {bx + l, by + t, bx + bw - r, by + t, bx + bw - r, by + bh - b, bx + l, by + bh - b};
        } else if (cp.kind == K::Circle || cp.kind == K::Ellipse) {
            const float cx = bx + cp.cx.resolve(bw), cy = by + cp.cy.resolve(bh);
            const float dl_ = std::fabs(cx - bx), dr = std::fabs(bx + bw - cx), dt = std::fabs(cy - by), db = std::fabs(by + bh - cy);
            auto side = [&](int kw, const Len& len, bool horiz, bool circle) {
                if (kw == 1 || kw == 2) {
                    const float a = horiz ? dl_ : dt, c2 = horiz ? dr : db;
                    if (circle) return kw == 1 ? std::min({dl_, dr, dt, db}) : std::max({dl_, dr, dt, db});
                    return kw == 1 ? std::min(a, c2) : std::max(a, c2);
                }
                return len.resolve(circle ? std::sqrt((bw * bw + bh * bh) / 2) : horiz ? bw : bh);
            };
            const bool circ = cp.kind == K::Circle;
            const float sm = f.st->shape_margin.resolve(mw);   // an expanded circle / ellipse stays one: the margin grows the radii
            const float rx = std::max(0.f, side(cp.rkw[0], cp.r[0], true, circ)) + sm, ry = (circ ? rx - sm : std::max(0.f, side(cp.rkw[1], cp.r[1], false, false))) + sm;
            for (int i = 0; i < 64; ++i) { const float a = float(i) * 2.f * 3.14159265f / 64.f; P.push_back(cx + rx * std::cos(a)); P.push_back(cy + ry * std::sin(a)); }
        } else if (cp.kind == K::Polygon) {
            for (const auto& pt : cp.pts) { P.push_back(bx + pt.first.resolve(bw)); P.push_back(by + pt.second.resolve(bh)); }
        }
        if (P.size() < 6) P.clear();
        if (cp.kind != style::ClipPath::Kind::Circle && cp.kind != style::ClipPath::Kind::Ellipse) bfc.floats.back().shape_margin = f.st->shape_margin.resolve(mw);
    }
    f.x = x + f.margin.l - ox;
    f.y = y + f.margin.t - oy;
}

// ── replaced elements ───────────────────────────────────────────────────────

static void layout_replaced(Env& env, Box& b, const Constraint& c) {
    const ComputedStyle& s = *b.st;
    if (b.ctl) ctl_metrics(env, b);
    resolve_edges(b, c.cb_w);
    const float bp_h = b.border.h() + b.padding.h(), bp_v = b.border.v() + b.padding.v();
    auto adj = [&](float v, float bp) { return s.box_sizing == style::BoxSizing::Border ? std::max(0.f, v - bp) : v; };
    const float ratio = s.aspect_ratio > 0 ? s.aspect_ratio : (b.nat_w > 0 && b.nat_h > 0 && !b.ctl && !b.no_ratio) ? b.nat_w / b.nat_h : 0;
    const bool wa = s.width.is_auto || (s.width.has_pct() && c.cb_w < 0);
    const bool ha = s.height.is_auto || (s.height.has_pct() && c.cb_h < 0);
    float w = 0, h = 0;
    if (!wa) w = adj(resolve_len(s.width, c.cb_w), bp_h);
    if (!ha) h = adj(resolve_len(s.height, c.cb_h), bp_v);
    if (wa && ha) { w = b.nat_w; h = s.aspect_ratio > 0 ? w / ratio : b.nat_h; }
    else if (wa) w = ratio > 0 ? h * ratio : b.nat_w;
    else if (ha) h = ratio > 0 ? w / ratio : b.nat_h;
    // min/max keep the ratio when only one dimension is constrained
    auto clampw = [&](float v) {
        if (!s.max_width.is_auto) v = std::min(v, adj(resolve_len(s.max_width, c.cb_w), bp_h));
        return std::max(v, adj(resolve_len(s.min_width, c.cb_w), bp_h));
    };
    auto clamph = [&](float v) {
        if (!s.max_height.is_auto && !(s.max_height.has_pct() && c.cb_h < 0)) v = std::min(v, adj(resolve_len(s.max_height, c.cb_h), bp_v));
        return std::max(v, adj(resolve_len(s.min_height, c.cb_h), bp_v));
    };
    const float w2 = clampw(w), h2 = clamph(h);
    if (wa && ha && ratio > 0 && (w2 != w || h2 != h)) {
        if (w2 != w) { w = w2; h = clamph(w / ratio); } else { h = h2; w = clampw(h * ratio); }
    } else { w = w2; h = h2; }
    if (c.forced_w >= 0) w = std::max(0.f, c.forced_w - bp_h);
    if (c.forced_h >= 0) h = std::max(0.f, c.forced_h - bp_v);
    b.w = w + bp_h;
    b.h = h + bp_v;
    b.baseline = b.ctl && b.ctl_baseline >= 0 ? b.border.t + b.padding.t + b.ctl_baseline : -1;
}

// ── intrinsic sizes ─────────────────────────────────────────────────────────

Intrinsic outer_intrinsic(Env& env, Box& c) {
    resolve_edges(c, 0);
    const float extra = c.margin.h() + c.border.h() + c.padding.h();
    Intrinsic in;
    if (!c.st->width.is_auto && !c.st->width.has_pct()) {
        float w = c.st->width.px;
        if (c.st->box_sizing == style::BoxSizing::Border) w = std::max(0.f, w - c.border.h() - c.padding.h());
        in = {w, w};
    } else {
        in = intrinsic_widths(env, c);
    }
    auto minmax = [&](float& v) {
        if (!c.st->max_width.is_auto && !c.st->max_width.has_pct()) v = std::min(v, c.st->max_width.px);
        if (!c.st->min_width.has_pct()) v = std::max(v, c.st->min_width.px);
    };
    minmax(in.min); minmax(in.max);
    return {in.min + extra, in.max + extra};
}

Intrinsic intrinsic_widths(Env& env, Box& b) {
    if (b.has_intr) return {b.min_c, b.max_c};
    Intrinsic r;
    if (b.kind == Box::Kind::Replaced) {
        const ComputedStyle& s = *b.st;
        if (b.ctl) ctl_metrics(env, b);
        float w = b.nat_w;
        if (!s.width.is_auto && !s.width.has_pct()) w = s.width.px;
        else if (!s.height.is_auto && !s.height.has_pct() && b.nat_w > 0 && b.nat_h > 0) w = s.height.px * b.nat_w / b.nat_h;
        r = {w, w};
        if (s.width.has_pct()) r.min = 0;   // percentage widths contribute no minimum
    } else if (b.kind == Box::Kind::Block) {
        switch (b.st->display) {
        case Display::Flex: case Display::InlineFlex: r = flex_intrinsic(env, b); break;
        case Display::Grid: case Display::InlineGrid: r = grid_intrinsic(env, b); break;
        case Display::Table: case Display::InlineTable: r = table_intrinsic(env, b); break;
        default:
            if (has_real_inline(b)) r = inline_intrinsic(env, b);
            else {
                float fl_sum = 0;   // consecutive floats sit side by side
                for (auto& k : b.kids) {
                    if (k->kind == Box::Kind::Marker || k->is_abs()) continue;
                    const Intrinsic ki = outer_intrinsic(env, *k);
                    if (k->is_float()) { fl_sum += ki.max; r.min = std::max(r.min, ki.min); r.max = std::max(r.max, fl_sum); continue; }
                    fl_sum = 0;
                    r.min = std::max(r.min, ki.min);
                    r.max = std::max(r.max, ki.max);
                }
            }
        }
    } else if (b.kind == Box::Kind::Inline) {
        r = inline_intrinsic(env, b);
    }
    b.has_intr = true; b.min_c = r.min; b.max_c = r.max;
    return r;
}

// ── block flow ──────────────────────────────────────────────────────────────

static float layout_block_flow(Env& env, Box& b, float content_w, float content_h_spec, Bfc& bfc,
                               float ox, float oy, bool top_merge, Strut& pending_out);

// Multi-column container (CSS Multi-column 1, column-fill: balance).  The content is laid out once at the column
// width as a single tall column, then cut into columns between line boxes and between blocks: each piece moves
// right by its column's offset and up by the column's start.  Limits: a block taller than a column is not split
// (it stays whole), and a block with its own box decoration that is split across columns keeps its box in the
// first column.
static float layout_columns(Env& env, Box& b, float content_w, float spec_h, Bfc& bfc, float ox, float oy) {
    const ComputedStyle& s = *b.st;
    const float em = s.font_size;
    const float gap = s.column_gap_set ? std::max(0.f, s.column_gap.resolve(content_w)) : em;
    const float want_w = s.column_width.is_auto ? -1.f : std::max(1.f, s.column_width.resolve(content_w));
    int n;
    if (want_w > 0 && s.column_count > 0) n = std::max(1, std::min(s.column_count, int((content_w + gap) / (want_w + gap))));
    else if (want_w > 0) n = std::max(1, int((content_w + gap) / (want_w + gap)));
    else n = std::max(1, s.column_count);
    const float col_w = std::max(0.f, (content_w - gap * float(n - 1)) / float(n));
    b.mc_n = n; b.mc_w = col_w; b.mc_gap = gap;

    Strut pending;
    const float total = layout_block_flow(env, b, col_w, -1.f, bfc, ox, oy, false, pending) + pending.value();
    if (n == 1) return spec_h >= 0 ? spec_h : total;

    // the breakable pieces, in column-0 coordinates (y from the container's content top)
    struct Atom { float top, bottom; Box* box; LineBox* line; };
    std::vector<Atom> atoms;
    auto gather = [&](auto& self, Box& p, float py) -> void {
        // py: p's content top in container coordinates
        if (p.has_inline_content) {
            for (LineBox& l : p.lines) atoms.push_back({py + l.y, py + l.y + l.h, &p, &l});
            return;
        }
        for (auto& k : p.kids) {
            if (k->out_of_flow()) continue;
            const bool splittable = k->kind == Box::Kind::Block && k->has_inline_content && !k->is_positioned() &&
                                    k->border.v() == 0 && k->padding.v() == 0;
            if (splittable) self(self, *k, py + k->y + k->border.t + k->padding.t);
            else atoms.push_back({py + k->y - k->margin.t, py + k->y + k->h + k->margin.b, k.get(), nullptr});
        }
    };
    gather(gather, b, 0.f);
    if (atoms.empty()) return spec_h >= 0 ? spec_h : total;

    // column height: the smallest that fits everything in n columns (or the given height)
    auto pack = [&](float hc, std::vector<int>* col_of) {
        int col = 0; float start = atoms[0].top;
        for (size_t i = 0; i < atoms.size(); ++i) {
            if (atoms[i].bottom - start > hc + 0.01f && i > 0 && atoms[i].top > start) { ++col; start = atoms[i].top; }
            if (col_of) (*col_of)[i] = col;
        }
        return col + 1;
    };
    float hc = std::max(1.f, (atoms.back().bottom - atoms[0].top) / float(n));
    {
        float step = 1.f;
        for (const Atom& a : atoms) step = std::max(step, std::min(a.bottom - a.top, 4.f));
        for (int guard = 0; pack(hc, nullptr) > n && guard < 4096; ++guard) hc += step;
        if (spec_h >= 0) hc = std::min(hc, spec_h);   // column-fill: balance, but never taller than the given height
    }
    std::vector<int> col_of(atoms.size(), 0);
    pack(hc, &col_of);
    std::vector<float> start(size_t(n), -1.f);
    for (size_t i = 0; i < atoms.size(); ++i) {
        const int c = std::min(col_of[i], n - 1);
        if (start[size_t(c)] < 0) start[size_t(c)] = atoms[i].top;
    }
    float used_h = 0;
    for (size_t i = 0; i < atoms.size(); ++i) {
        const int c = std::min(col_of[i], n - 1);
        const float dx = float(c) * (col_w + gap);
        const float base = atoms[0].top;
        const float ndy = c == 0 ? 0.f : -(start[size_t(c)] - base);
        if (atoms[i].line) {
            atoms[i].line->y += ndy;
            for (LineFrag& f : atoms[i].line->frags) f.x += dx;
        } else {
            atoms[i].box->x += dx; atoms[i].box->y += ndy;
        }
        used_h = std::max(used_h, atoms[i].bottom + ndy - base);
    }
    return spec_h >= 0 ? spec_h : used_h + atoms[0].top;
}

// Lays out the in-flow children of a block container.  Returns the content
// height WITHOUT the trailing margin strut, which goes to `pending`.
static float layout_block_flow(Env& env, Box& b, float content_w, float content_h_spec, Bfc& bfc,
                               float ox, float oy, bool top_merge, Strut& pending_out) {
    if (has_real_inline(b)) {
        b.has_inline_content = true;
        // out-of-flow children among the inline content are handled by the inline layout
        const float h = layout_inline_content(env, b, content_w, bfc, ox, oy);
        pending_out = Strut{};
        return h;
    }
    float cursor = 0;
    Strut pending;
    bool leading = top_merge;
    bool saw_inflow = false;
    for (auto& up : b.kids) {
        Box& c = *up;
        if (c.kind == Box::Kind::Marker) continue;
        if (c.is_abs()) {
            c.static_parent = &b;
            c.static_x = 0;
            c.static_y = cursor + (leading ? 0.f : pending.value());
            env.abs_pending.push_back(&c);
            continue;
        }
        if (c.is_float()) {
            place_float(env, c, bfc, content_w, ox, oy, oy + cursor + (saw_inflow && !leading ? pending.value() : 0.f));
            continue;
        }
        const Strut top = top_chain(c, content_w);
        const bool through = collapses_through(c);
        Strut t;
        float y;
        if (leading) { y = cursor; }
        else { t = pending; t.merge(top); y = cursor + t.value(); }

        // clearance
        const Clear clr = c.st->clear;
        if (clr != Clear::None) {
            float cb = -1e9f;
            if (clr == Clear::Left || clr == Clear::Both) cb = std::max(cb, bfc.bottom_of(true, false));
            if (clr == Clear::Right || clr == Clear::Both) cb = std::max(cb, bfc.bottom_of(false, true));
            if (cb - oy > y) y = cb - oy;
        }
        // a BFC root beside floats shrinks to the free space (or drops below)
        float x0 = 0, aw = content_w;
        const bool bfc_root = establishes_bfc(c);
        if (bfc_root && !through && !bfc.floats.empty()) {
            float l, r;
            bfc.avail(oy + y, 1.f, ox, ox + content_w, l, r);
            if (l > ox + 0.01f || r < ox + content_w - 0.01f) { x0 = l - ox; aw = r - l; }
        }

        c.y = y;
        const size_t float_mark = bfc.floats.size(), abs_mark2 = env.abs_pending.size();
        Constraint cc;
        cc.cb_w = aw;
        cc.cb_h = content_h_spec;
        cc.bfc = bfc_root ? nullptr : &bfc;
        cc.bfc_x = ox + x0 + c.margin.l;
        cc.bfc_y = oy + y;
        layout_box(env, c, cc);

        // auto margins: center / push
        if (c.mar_l_auto || c.mar_r_auto) {
            float free = aw - (c.w + (c.mar_l_auto ? 0.f : c.margin.l) + (c.mar_r_auto ? 0.f : c.margin.r));
            float ml = c.margin.l, mr = c.margin.r;
            if (c.mar_l_auto && c.mar_r_auto) { if (free >= 0) ml = mr = free / 2; else { ml = 0; mr = free; } }
            else if (c.mar_l_auto) ml = std::max(0.f, free);
            else mr = std::max(0.f, free);
            const float old = c.margin.l;
            c.margin.l = ml; c.margin.r = mr;
            if (std::fabs(old - ml) > 0.01f && !bfc_root) {   // floats inside need the real x
                cc.bfc_x = ox + x0 + ml;
                bfc.floats.resize(float_mark);          // forget what the first pass placed
                env.abs_pending.resize(abs_mark2);
                layout_box(env, c, cc);
                c.margin.l = ml; c.margin.r = mr;
            }
        }
        c.x = x0 + c.margin.l;

        if (through) {
            if (!leading) { pending = t; pending.merge(c.bottom_strut); }
            c.y = cursor;
        } else {
            cursor = y + c.h;
            pending = c.bottom_strut;
            leading = false;
        }
        saw_inflow = true;
    }
    pending_out = leading ? Strut{} : pending;
    return cursor;
}

float last_baseline(const Box& b) {
    // distance from b's border-box top to its last in-flow baseline, or -1
    const float inset = b.border.t + b.padding.t;
    if (!b.lines.empty()) return inset + b.lines.back().y + b.lines.back().baseline;
    for (size_t i = b.kids.size(); i-- > 0;) {
        const Box& k = *b.kids[i];
        if (k.kind != Box::Kind::Block || k.out_of_flow()) continue;
        const float kb = last_baseline(k);
        if (kb >= 0) return inset + k.y + kb;
    }
    return -1;
}

static bool spec_h_or_max(float spec_h, float max_h) { return spec_h >= 0 || max_h >= 0; }

void layout_box(Env& env, Box& b, const Constraint& c) {
    if (b.kind == Box::Kind::Replaced) { layout_replaced(env, b, c); return; }
    const ComputedStyle& s = *b.st;
    resolve_edges(b, c.cb_w);

    const float bp_h = b.border.h() + b.padding.h(), bp_v = b.border.v() + b.padding.v();
    const bool bb = s.box_sizing == style::BoxSizing::Border;
    const size_t abs_mark = env.abs_pending.size();

    // tables size themselves
    if (s.display == Display::Table || s.display == Display::InlineTable) {
        layout_table(env, b, c);
        if (b.is_positioned() || !b.parent) resolve_abs(env, b.is_positioned() ? &b : nullptr, abs_mark);
        return;
    }

    // ── used width (content box) ────────────────────────────────────────────
    float content_w;
    auto minmax_w = [&](float w) {
        if (!s.max_width.is_auto && !(s.max_width.has_pct() && c.cb_w < 0)) {
            float v = resolve_len(s.max_width, c.cb_w);
            w = std::min(w, bb ? std::max(0.f, v - bp_h) : v);
        }
        if (!(s.min_width.has_pct() && c.cb_w < 0)) {
            float v = resolve_len(s.min_width, c.cb_w);
            w = std::max(w, bb ? std::max(0.f, v - bp_h) : v);
        }
        return w;
    };
    if (c.forced_w >= 0) {
        content_w = std::max(0.f, c.forced_w - bp_h);
    } else {
        const bool w_auto = s.width.is_auto || (s.width.has_pct() && c.cb_w < 0);
        if (!w_auto) {
            const float v = resolve_len(s.width, c.cb_w);
            content_w = bb ? std::max(0.f, v - bp_h) : v;
        } else if (s.width.kw) {   // fit-content / max-content / min-content on a block: sized by its content
            const Intrinsic in = intrinsic_widths(env, b);
            const float avail = std::max(0.f, c.cb_w - b.margin.h() - bp_h);
            content_w = s.width.kw == 2 ? in.max : s.width.kw == 3 ? in.min : std::min(std::max(in.min, avail), in.max);
            content_w = std::max(0.f, minmax_w(content_w));
        } else if (s.aspect_ratio > 0 && !s.height.is_auto && !(s.height.has_pct() && c.cb_h < 0) && (c.shrink || s.float_ != style::Float::None)) {
            // aspect-ratio with a definite height and an auto width that would shrink to fit: width follows the height
            const float hv = resolve_len(s.height, c.cb_h);
            const float h_border = bb ? hv : hv + bp_v;
            content_w = std::max(0.f, h_border * s.aspect_ratio - bp_h);
            content_w = std::max(0.f, minmax_w(content_w));
        } else if (c.shrink) {
            const Intrinsic in = intrinsic_widths(env, b);
            const float avail = std::max(0.f, (c.avail_w >= 0 ? c.avail_w : c.cb_w) - b.margin.h() - bp_h);
            content_w = std::min(std::max(in.min, avail), in.max);
        } else {
            content_w = std::max(0.f, c.cb_w - b.margin.h() - bp_h);
        }
        content_w = std::max(0.f, minmax_w(content_w));
    }
    // overflow: scroll reserves the classic scrollbar's gutter up front; overflow: auto does once the content overflows (below)
    if (s.overflow_y == Overflow::Scroll && !s.scrollbar_none && b.sb_gutter == 0 && b.node) b.sb_gutter = s.scrollbar_thin ? 11.f : 15.f;
    content_w = std::max(0.f, content_w - b.sb_gutter);

    // ── height specification ────────────────────────────────────────────────
    float spec_h = -1;   // content-box height when definite
    if (c.forced_h >= 0) spec_h = std::max(0.f, c.forced_h - bp_v);
    else if (!s.height.is_auto && !(s.height.has_pct() && c.cb_h < 0)) {
        const float v = resolve_len(s.height, c.cb_h);
        spec_h = bb ? std::max(0.f, v - bp_v) : v;
    }
    // aspect-ratio: an auto height follows the used width (the ratio is of the box that box-sizing names)
    if (spec_h < 0 && s.aspect_ratio > 0 && (s.height.is_auto || (s.height.has_pct() && c.cb_h < 0)))
        spec_h = bb ? std::max(0.f, (content_w + bp_h) / s.aspect_ratio - bp_v) : content_w / s.aspect_ratio;
    auto clamp_h = [&](float h) {
        if (!s.max_height.is_auto && !(s.max_height.has_pct() && c.cb_h < 0)) {
            const float v = resolve_len(s.max_height, c.cb_h);
            h = std::min(h, bb ? std::max(0.f, v - bp_v) : v);
        }
        if (!(s.min_height.has_pct() && c.cb_h < 0)) {
            const float v = resolve_len(s.min_height, c.cb_h);
            h = std::max(h, bb ? std::max(0.f, v - bp_v) : v);
        }
        return h;
    };

    // ── formatting context and origin ──────────────────────────────────────
    Bfc local;
    const bool new_bfc = establishes_bfc(b) || !c.bfc;
    Bfc& bfc = new_bfc ? local : *c.bfc;
    const float ox = new_bfc ? 0.f : c.bfc_x + b.border.l + b.padding.l;
    const float oy = new_bfc ? 0.f : c.bfc_y + b.border.t + b.padding.t;

    float content_h = 0;
    b.lines.clear();
    b.has_inline_content = false;
    Strut pending;
    bool merge_last = false;

    // content-box min/max height (for containers that distribute space over their height)
    float min_h_c = 0, max_h_c = -1;
    if (!(s.min_height.has_pct() && c.cb_h < 0)) { const float v = resolve_len(s.min_height, c.cb_h); min_h_c = bb ? std::max(0.f, v - bp_v) : v; }
    if (!s.max_height.is_auto && !(s.max_height.has_pct() && c.cb_h < 0)) { const float v = resolve_len(s.max_height, c.cb_h); max_h_c = bb ? std::max(0.f, v - bp_v) : v; }
    switch (s.display) {
    case Display::Flex: case Display::InlineFlex:
        content_h = layout_flex(env, b, c, content_w, spec_h, min_h_c, max_h_c);
        break;
    case Display::Grid: case Display::InlineGrid:
        content_h = layout_grid(env, b, c, content_w, spec_h, min_h_c, max_h_c);
        break;
    default: {
        if (s.multicol() && s.display != Display::Table) {
            content_h = layout_columns(env, b, content_w, spec_h, bfc, ox, oy);
            break;
        }
        const bool top_merge = !new_bfc && merges_first(b);
        content_h = layout_block_flow(env, b, content_w, spec_h, bfc, ox, oy, top_merge, pending);
        merge_last = !new_bfc && merges_last(b) && spec_h < 0;
        if (!merge_last) content_h += pending.value();
        break;
    }
    }
    // a BFC root contains its floats
    if (new_bfc && spec_h < 0 && !bfc.floats.empty())
        content_h = std::max(content_h, bfc.bottom_of(true, true) - oy);

    float final_h = spec_h >= 0 ? spec_h : content_h;
    b.scroll_h = std::max(content_h, 0.f);
    final_h = std::max(0.f, clamp_h(final_h));

    // overflow: auto whose content does not fit gets its scrollbar: lay it out again with the gutter taken off
    if (s.overflow_y == Overflow::Auto && !s.scrollbar_none && b.sb_gutter == 0 && b.node && b.kind == Box::Kind::Block &&
        s.display != Display::Table && b.scroll_h > final_h + 0.5f && spec_h_or_max(spec_h, max_h_c)) {
        b.sb_gutter = s.scrollbar_thin ? 11.f : 15.f;
        env.abs_pending.resize(abs_mark);
        layout_box(env, b, c);
        return;
    }
    b.w = content_w + bp_h + b.sb_gutter;
    b.h = final_h + bp_v;

    // margin sticking out below
    b.bottom_strut = Strut{};
    b.bottom_strut.add(b.margin.b);
    if (merge_last) b.bottom_strut.merge(pending);

    // baseline for inline-block alignment
    b.baseline = -1;
    if (s.overflow_x == Overflow::Visible && s.overflow_y == Overflow::Visible &&
        (s.display == Display::Block || s.display == Display::InlineBlock || s.display == Display::ListItem ||
         s.display == Display::FlowRoot || s.display == Display::TableCell || s.display == Display::Flex ||
         s.display == Display::InlineFlex || s.display == Display::Grid || s.display == Display::InlineGrid))
        b.baseline = last_baseline(b);

    // abs descendants: resolved once their containing block is complete
    if (b.is_positioned() || !b.parent) resolve_abs(env, b.is_positioned() ? &b : nullptr, abs_mark);
}

// ── absolute positioning ────────────────────────────────────────────────────

void resolve_abs(Env& env, Box* container, size_t from) {
    // take the boxes that belong to this containing block
    std::vector<Box*> mine, keep;
    for (size_t i = from; i < env.abs_pending.size(); ++i) {
        Box* a = env.abs_pending[i];
        const bool fixed = a->st->position == Position::Fixed;
        if (container == nullptr || !fixed) mine.push_back(a);
        else keep.push_back(a);
    }
    env.abs_pending.resize(from);
    for (Box* a : keep) env.abs_pending.push_back(a);

    for (Box* a : mine) {
        const ComputedStyle& s = *a->st;
        const bool fixed = s.position == Position::Fixed;
        Box* cbx = fixed ? nullptr : container;
        float cb_w, cb_h;
        if (cbx) { cb_w = cbx->w - cbx->border.h(); cb_h = cbx->h - cbx->border.v(); }
        else { cb_w = env.vw; cb_h = env.vh; }

        // static position relative to the containing block's padding box
        float sx = 0, sy = 0;
        {
            Box* p = a->static_parent ? a->static_parent : a->parent;
            for (; p && p != cbx; p = p->parent) {
                sx += p->x + p->border.l + p->padding.l;
                sy += p->y + p->border.t + p->padding.t;
            }
            if (cbx) { sx += cbx->padding.l; sy += cbx->padding.t; }
            sx += a->static_x; sy += a->static_y;
        }

        resolve_edges(*a, cb_w);
        const bool L = !s.inset[3].is_auto, R = !s.inset[1].is_auto, T = !s.inset[0].is_auto, B = !s.inset[2].is_auto;
        const float l = L ? resolve_len(s.inset[3], cb_w) : 0, r = R ? resolve_len(s.inset[1], cb_w) : 0;
        const float t = T ? resolve_len(s.inset[0], cb_h) : 0, bt = B ? resolve_len(s.inset[2], cb_h) : 0;
        const float bp_h = a->border.h() + a->padding.h(), bp_v = a->border.v() + a->padding.v();
        const bool bbx = s.box_sizing == style::BoxSizing::Border;

        Constraint c;
        c.cb_w = cb_w; c.cb_h = cb_h;
        const bool w_auto = s.width.is_auto && !s.width.kw;     // fit-content & co: sized by content, so not "stretched"
        const bool h_auto = s.height.is_auto && !s.height.kw;
        float mar_l = a->margin.l, mar_r = a->margin.r;
        if (s.width.kw) {   // shrink to fit what is left between the insets
            c.shrink = true;
            c.avail_w = std::max(0.f, cb_w - (L ? l : 0) - (R ? r : 0) - mar_l - mar_r);
        } else if (w_auto) {
            if (L && R) c.forced_w = std::max(0.f, cb_w - l - r - mar_l - mar_r);
            else {
                c.shrink = true;
                const float start = L ? l : (R ? r : sx);
                c.avail_w = std::max(0.f, cb_w - start);
            }
        }
        if (h_auto && T && B) c.forced_h = std::max(0.f, cb_h - t - bt - a->margin.t - a->margin.b);

        layout_box(env, *a, c);

        // horizontal position
        float x;
        if (L) {
            x = l + a->margin.l;
            if (R && !w_auto) {
                const float free = cb_w - l - r - a->w - (a->mar_l_auto ? 0 : a->margin.l) - (a->mar_r_auto ? 0 : a->margin.r);
                if (a->mar_l_auto && a->mar_r_auto) x = l + std::max(0.f, free) / 2;
                else if (a->mar_l_auto) x = l + std::max(0.f, free);
            }
        } else if (R) x = cb_w - r - a->margin.r - a->w;
        else {
            x = sx + a->margin.l;
            if (a->static_fx > 0 && a->static_parent) x += (a->static_parent->content_w() - a->outer_w()) * a->static_fx;
        }
        // vertical position
        float y;
        if (T) {
            y = t + a->margin.t;
            if (B && !h_auto) {
                const float free = cb_h - t - bt - a->h - (a->mar_t_auto ? 0 : a->margin.t) - (a->mar_b_auto ? 0 : a->margin.b);
                if (a->mar_t_auto && a->mar_b_auto) y = t + std::max(0.f, free) / 2;
                else if (a->mar_t_auto) y = t + std::max(0.f, free);
            }
        } else if (B) y = cb_h - bt - a->margin.b - a->h;
        else {
            y = sy + a->margin.t;
            if (a->static_fy > 0 && a->static_parent) y += (a->static_parent->content_h() - a->outer_h()) * a->static_fy;
        }
        (void)bbx; (void)bp_h; (void)bp_v;
        a->x = x; a->y = y;
        a->cb = cbx;
    }
}

} // namespace luxium::layout
