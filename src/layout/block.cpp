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

    b.w = content_w + bp_h;
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
