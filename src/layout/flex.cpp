// Flexbox (CSS Flexible Box Layout L1): the spec's algorithm in order --
// flex base sizes, line collection, resolving flexible lengths with the
// freeze loop, cross sizing, alignment, and main-axis distribution.

#include <layout/box.hpp>

#include <algorithm>
#include <cmath>

namespace luxium::layout {

using style::Align;
using style::FlexDirection;
using style::FlexWrap;

namespace {

struct Item {
    Box* box = nullptr;
    // all main/cross quantities below are for the container's axes
    float base = 0;               // flex base size (content box)
    float hyp = 0;                // hypothetical main size (content box, clamped)
    float min_m = 0, max_m = 1e9f;   // content-box clamps
    float bp_m = 0, bp_c = 0;     // padding+border on each axis
    float mar_ms = 0, mar_me = 0, mar_cs = 0, mar_ce = 0;
    bool  auto_ms = false, auto_me = false, auto_cs = false, auto_ce = false;
    float grow = 0, shrink = 1;
    int   order = 0;
    float target = 0;             // resolved main size (content box)
    float cross = 0;              // cross size, border box
    bool  frozen = false;
    bool  cross_auto = true;
    float pos_m = 0, pos_c = 0;   // border-box position along the axes (relative to line start / container)
    float base_asc = 0;           // baseline offset from the margin-box top (cross axis)
    Align align = Align::Stretch;
    float outer_hyp() const { return hyp + bp_m + mar_ms + mar_me; }
};

struct Line {
    std::vector<size_t> idx;
    float cross = 0;
    float pos = 0;
    float main_used = 0;
};

Align resolved_align(const Box& item, const Box& cont) {
    Align a = item.st->align_self;
    if (a == Align::Auto) a = cont.st->align_items;
    if (a == Align::Normal) a = Align::Stretch;
    return a;
}

} // namespace

float layout_flex(Env& env, Box& b, const Constraint& c, float content_w, float content_h_def,
                  float min_h, float max_h) {
    const ComputedStyle& s = *b.st;
    const bool row = s.flex_direction == FlexDirection::Row || s.flex_direction == FlexDirection::RowReverse;
    const bool rev = s.flex_direction == FlexDirection::RowReverse || s.flex_direction == FlexDirection::ColumnReverse;
    const bool wrap = s.flex_wrap != FlexWrap::Nowrap;
    const bool wrap_rev = s.flex_wrap == FlexWrap::WrapReverse;
    const float main_gap = row ? s.column_gap.resolve(content_w) : s.row_gap.resolve(content_h_def > 0 ? content_h_def : 0);
    const float cross_gap = row ? s.row_gap.resolve(content_h_def > 0 ? content_h_def : 0) : s.column_gap.resolve(content_w);

    // container axes: main size (content box) and cross size (-1 if indefinite)
    float main_size = row ? content_w : content_h_def;     // < 0: indefinite (column, auto height)
    float cross_size = row ? content_h_def : content_w;

    // ── collect items ───────────────────────────────────────────────────────
    std::vector<Item> items;
    for (auto& up : b.kids) {
        Box& k = *up;
        if (k.kind == Box::Kind::Marker) continue;
        if (k.is_abs()) { k.static_parent = &b; k.static_x = 0; k.static_y = 0; env.abs_pending.push_back(&k); continue; }
        resolve_edges(k, content_w);
        Item it;
        it.box = &k;
        const ComputedStyle& ks = *k.st;
        it.order = ks.order; it.grow = ks.flex_grow; it.shrink = ks.flex_shrink;
        it.bp_m = row ? k.border.h() + k.padding.h() : k.border.v() + k.padding.v();
        it.bp_c = row ? k.border.v() + k.padding.v() : k.border.h() + k.padding.h();
        it.mar_ms = row ? k.margin.l : k.margin.t; it.mar_me = row ? k.margin.r : k.margin.b;
        it.mar_cs = row ? k.margin.t : k.margin.l; it.mar_ce = row ? k.margin.b : k.margin.r;
        it.auto_ms = row ? k.mar_l_auto : k.mar_t_auto; it.auto_me = row ? k.mar_r_auto : k.mar_b_auto;
        it.auto_cs = row ? k.mar_t_auto : k.mar_l_auto; it.auto_ce = row ? k.mar_b_auto : k.mar_r_auto;
        it.align = resolved_align(k, b);
        items.push_back(it);
    }
    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b2) { return a.order < b2.order; });
    if (items.empty()) return main_size >= 0 && !row ? 0.f : 0.f;

    auto content_of = [&](const ComputedStyle& ks, float spec_bb_or_content, float bp) {
        return ks.box_sizing == style::BoxSizing::Border ? std::max(0.f, spec_bb_or_content - bp) : spec_bb_or_content;
    };

    // ── flex base size and hypothetical main size ───────────────────────────
    for (Item& it : items) {
        Box& k = *it.box;
        const ComputedStyle& ks = *k.st;
        const Len& main_len = row ? ks.width : ks.height;
        const Len& min_len = row ? ks.min_width : ks.min_height;
        const Len& max_len = row ? ks.max_width : ks.max_height;
        const float base_for_pct = main_size >= 0 ? main_size : -1;
        auto resolve_main = [&](const Len& l, float& out) {
            if (l.is_auto) return false;
            if (l.has_pct() && base_for_pct < 0) return false;
            out = content_of(ks, l.resolve(std::max(0.f, base_for_pct)), it.bp_m);
            return true;
        };
        float v = 0;
        bool have_basis = false;
        if (!ks.flex_basis.is_auto && resolve_main(ks.flex_basis, v)) have_basis = true;
        else if (resolve_main(main_len, v)) have_basis = true;
        if (!have_basis) {
            // content size
            if (row) v = intrinsic_widths(env, k).max;
            else {
                Constraint cc; cc.cb_w = content_w; cc.cb_h = -1; cc.bfc = nullptr;
                const bool stretch = it.align == Align::Stretch && ks.width.is_auto;
                if (stretch) cc.forced_w = std::max(0.f, content_w - it.mar_cs - it.mar_ce);
                else { cc.shrink = true; cc.avail_w = content_w; }
                layout_box(env, k, cc);
                v = std::max(0.f, k.h - it.bp_m);
            }
        }
        it.base = std::max(0.f, v);
        // clamps
        float mn = 0, mx = 1e9f;
        float tmp;
        if (resolve_main(min_len, tmp)) mn = tmp;
        else if (min_len.is_auto || (min_len.px == 0 && !min_len.has_pct())) {
            // automatic minimum: min-content, bounded by a definite size
            if (ks.overflow_x == style::Overflow::Visible && ks.overflow_y == style::Overflow::Visible) {
                float content_min = row ? intrinsic_widths(env, k).min : 0.f;
                if (!row) {   // column: min-content height ~ content height at this width
                    Constraint cc; cc.cb_w = content_w; cc.cb_h = -1; cc.bfc = nullptr;
                    cc.forced_w = std::max(0.f, content_w - it.mar_cs - it.mar_ce);
                    layout_box(env, k, cc);
                    content_min = std::max(0.f, k.h - it.bp_m);
                }
                float spec = 1e9f;
                float sv;
                if (resolve_main(main_len, sv)) spec = sv;
                mn = std::min(content_min, spec);
            }
        }
        if (resolve_main(max_len, tmp)) mx = tmp;
        it.min_m = mn; it.max_m = mx;
        it.hyp = std::clamp(it.base, mn, std::max(mn, mx));
    }

    // ── lines ───────────────────────────────────────────────────────────────
    std::vector<Line> lines(1);
    if (!wrap || main_size < 0) {
        for (size_t i = 0; i < items.size(); ++i) lines[0].idx.push_back(i);
    } else {
        float used = 0;
        for (size_t i = 0; i < items.size(); ++i) {
            const float w = items[i].outer_hyp();
            const float add = lines.back().idx.empty() ? w : main_gap + w;
            if (!lines.back().idx.empty() && used + add > main_size + 0.01f) { lines.emplace_back(); used = 0; }
            used += lines.back().idx.empty() ? w : main_gap + w;
            lines.back().idx.push_back(i);
        }
    }

    // indefinite main size (column with auto height): the container is as tall as its single line
    if (main_size < 0) {
        float sum = 0;
        for (size_t i : lines[0].idx) sum += items[i].outer_hyp();
        sum += main_gap * float(std::max<size_t>(1, lines[0].idx.size()) - 1);
        if (!row) { sum = std::max(sum, min_h); if (max_h >= 0) sum = std::min(sum, std::max(max_h, min_h)); }
        main_size = sum;
    }

    // ── resolve flexible lengths, per line ──────────────────────────────────
    for (Line& L : lines) {
        float sum_hyp = main_gap * float(std::max<size_t>(1, L.idx.size()) - 1);
        for (size_t i : L.idx) sum_hyp += items[i].outer_hyp();
        const bool growing = sum_hyp < main_size;
        for (size_t i : L.idx) { items[i].frozen = false; items[i].target = items[i].base; }
        for (size_t i : L.idx) {
            Item& it = items[i];
            const float factor = growing ? it.grow : it.shrink;
            if (factor == 0 || (growing && it.base > it.hyp) || (!growing && it.base < it.hyp)) {
                it.frozen = true; it.target = it.hyp;
            }
        }
        for (int guard = 0; guard < 100; ++guard) {
            bool any_unfrozen = false;
            float used = main_gap * float(std::max<size_t>(1, L.idx.size()) - 1);
            float sum_factor = 0, sum_scaled = 0;
            for (size_t i : L.idx) {
                const Item& it = items[i];
                used += it.mar_ms + it.mar_me + it.bp_m + (it.frozen ? it.target : it.base);
                if (!it.frozen) {
                    any_unfrozen = true;
                    sum_factor += growing ? it.grow : it.shrink;
                    sum_scaled += it.shrink * it.base;
                }
            }
            if (!any_unfrozen) break;
            float free = main_size - used;
            if (growing && sum_factor < 1 && std::fabs(free * sum_factor) < std::fabs(free)) free *= sum_factor;
            float violation = 0;
            for (size_t i : L.idx) {
                Item& it = items[i];
                if (it.frozen) continue;
                float t = it.base;
                if (growing) { if (sum_factor > 0) t += free * it.grow / sum_factor; }
                else if (sum_scaled > 0) t += free * (it.shrink * it.base) / sum_scaled;
                const float clamped = std::clamp(t, it.min_m, std::max(it.min_m, it.max_m));
                violation += clamped - t;
                it.target = clamped;
            }
            for (size_t i : L.idx) {
                Item& it = items[i];
                if (it.frozen) continue;
                const float t_unclamped = it.target;
                (void)t_unclamped;
            }
            // freeze per violation sign
            for (size_t i : L.idx) {
                Item& it = items[i];
                if (it.frozen) continue;
                const bool at_min = it.target <= it.min_m + 1e-3f, at_max = it.target >= it.max_m - 1e-3f;
                if (violation == 0 || (violation > 0 && at_min) || (violation < 0 && at_max)) it.frozen = true;
            }
            if (violation == 0) break;
        }
        L.main_used = main_gap * float(std::max<size_t>(1, L.idx.size()) - 1);
        for (size_t i : L.idx) L.main_used += items[i].target + items[i].bp_m + items[i].mar_ms + items[i].mar_me;
    }

    // ── cross sizes ─────────────────────────────────────────────────────────
    for (Line& L : lines) {
        float asc_max = 0, desc_max = 0, cross_max = 0;
        bool have_base = false;
        for (size_t i : L.idx) {
            Item& it = items[i];
            Box& k = *it.box;
            const ComputedStyle& ks = *k.st;
            Constraint cc;
            cc.cb_w = content_w; cc.cb_h = content_h_def; cc.bfc = nullptr;
            const Len& cross_len = row ? ks.height : ks.width;
            it.cross_auto = cross_len.is_auto || (cross_len.has_pct() && (row ? content_h_def < 0 : false));
            if (row) {
                cc.forced_w = it.target + it.bp_m;
                layout_box(env, k, cc);
                it.cross = k.h;
            } else {
                cc.forced_h = it.target + it.bp_m;
                if (it.cross_auto && it.align == Align::Stretch) cc.forced_w = std::max(0.f, content_w - it.mar_cs - it.mar_ce);
                else if (it.cross_auto) { cc.shrink = true; cc.avail_w = content_w; }
                layout_box(env, k, cc);
                it.cross = k.w;
            }
            const float outer_c = it.cross + it.mar_cs + it.mar_ce;
            cross_max = std::max(cross_max, outer_c);
            if (row && it.align == Align::Baseline) {
                const float asc = it.mar_cs + (k.baseline >= 0 ? k.baseline : k.h);
                it.base_asc = asc;
                asc_max = std::max(asc_max, asc);
                desc_max = std::max(desc_max, outer_c - asc);
                have_base = true;
            }
        }
        L.cross = cross_max;
        if (have_base) L.cross = std::max(cross_max, asc_max + desc_max);
        (void)have_base;
        // store the baseline group's ascent in `pos` temporarily via first baseline item
        if (have_base) for (size_t i : L.idx) if (items[i].align == Align::Baseline) items[i].base_asc = items[i].base_asc;
        L.pos = asc_max;   // reuse: ascent of the baseline group (replaced later by line position)
    }
    std::vector<float> baseline_asc;
    for (Line& L : lines) baseline_asc.push_back(L.pos);

    // single-line container with a definite cross size: the line fills it
    float total_cross_lines = cross_gap * float(lines.size() - 1);
    for (Line& L : lines) total_cross_lines += L.cross;
    if (lines.size() == 1 && cross_size >= 0) lines[0].cross = cross_size;
    float container_cross = cross_size >= 0 ? cross_size : total_cross_lines;
    if (lines.size() == 1 && cross_size < 0) container_cross = lines[0].cross;
    if (row && cross_size < 0) {   // min/max-height clamp the container's cross size before items are stretched
        container_cross = std::max(container_cross, min_h);
        if (max_h >= 0) container_cross = std::min(container_cross, std::max(max_h, min_h));
        if (lines.size() == 1) lines[0].cross = container_cross;
    }

    // align-content for multi-line containers
    float cross_free = container_cross - total_cross_lines;
    if (lines.size() == 1 && (cross_size >= 0 || (row && lines[0].cross >= container_cross - 0.01f))) cross_free = 0;
    float lead = 0, between = 0;
    {
        Align ac = s.align_content;
        if (ac == Align::Normal) ac = Align::Stretch;
        const size_t n = lines.size();
        if (lines.size() > 1 || wrap) {
            if (cross_free > 0) {
                switch (ac) {
                case Align::Stretch: for (Line& L : lines) L.cross += cross_free / float(n); break;
                case Align::Center: lead = cross_free / 2; break;
                case Align::End: case Align::FlexEnd: lead = cross_free; break;
                case Align::SpaceBetween: if (n > 1) between = cross_free / float(n - 1); break;
                case Align::SpaceAround: between = cross_free / float(n); lead = between / 2; break;
                case Align::SpaceEvenly: between = cross_free / float(n + 1); lead = between; break;
                default: break;
                }
            }
        }
    }
    {   // line positions along the cross axis
        float y = lead;
        for (Line& L : lines) { L.pos = y; y += L.cross + cross_gap + between; }
        if (wrap_rev) for (Line& L : lines) L.pos = container_cross - L.pos - L.cross;
    }

    // ── alignment and final layout of every item ────────────────────────────
    for (size_t li = 0; li < lines.size(); ++li) {
        Line& L = lines[li];
        // main axis: free space, auto margins, justify-content
        float free = main_size - L.main_used;
        int auto_count = 0;
        for (size_t i : L.idx) { auto_count += items[i].auto_ms ? 1 : 0; auto_count += items[i].auto_me ? 1 : 0; }
        float start = 0, step_extra = 0;
        float auto_share = 0;
        if (auto_count > 0 && free > 0) auto_share = free / float(auto_count);
        else if (free > 0 || true) {
            Align jc = s.justify_content;
            if (jc == Align::Normal) jc = Align::FlexStart;
            const size_t n = L.idx.size();
            auto flip = [&](Align a) {   // start/end are physical for *-reverse via main-start edge handling below
                return a;
            };
            jc = flip(jc);
            if (free < 0) {
                if (jc == Align::SpaceBetween) jc = Align::FlexStart;
                else if (jc == Align::SpaceAround || jc == Align::SpaceEvenly) jc = Align::Center;
            }
            switch (jc) {
            case Align::FlexEnd: case Align::End: start = free; break;
            case Align::Center: start = free / 2; break;
            case Align::SpaceBetween: if (n > 1) step_extra = free / float(n - 1); break;
            case Align::SpaceAround: step_extra = free / float(n); start = step_extra / 2; break;
            case Align::SpaceEvenly: step_extra = free / float(n + 1); start = step_extra; break;
            default: break;
            }
            if (rev) {
                // main-start is the far edge: flex-start packs there
                if (jc == Align::FlexStart || jc == Align::Start || jc == Align::Stretch) start = free;
                else if (jc == Align::FlexEnd || jc == Align::End) start = 0;
            }
        }
        float pos = start;
        std::vector<size_t> order_idx = L.idx;
        if (rev) std::reverse(order_idx.begin(), order_idx.end());
        for (size_t i : order_idx) {
            Item& it = items[i];
            Box& k = *it.box;
            const float ms = it.auto_ms ? auto_share : it.mar_ms;
            const float me = it.auto_me ? auto_share : it.mar_me;
            it.pos_m = pos + ms;
            pos += ms + it.target + it.bp_m + me + main_gap + step_extra;

            // cross axis
            const float line_cross = L.cross;
            float cross_outer_free = line_cross - (it.cross + it.mar_cs + it.mar_ce);
            float cs = it.mar_cs, ce = it.mar_ce;
            float cross_pos;
            bool stretch = it.align == Align::Stretch && it.cross_auto && !(it.auto_cs || it.auto_ce);
            float new_cross = it.cross;
            if (stretch) {
                new_cross = std::max(0.f, line_cross - it.mar_cs - it.mar_ce);
                if (wrap_rev) {}
            }
            if (it.auto_cs || it.auto_ce) {
                const float free_c = std::max(0.f, cross_outer_free);
                if (it.auto_cs && it.auto_ce) { cs = free_c / 2; ce = free_c / 2; }
                else if (it.auto_cs) cs = free_c; else ce = free_c;
                cross_pos = cs;
            } else {
                switch (it.align) {
                case Align::Center: cross_pos = it.mar_cs + cross_outer_free / 2; break;
                case Align::FlexEnd: case Align::End: cross_pos = it.mar_cs + cross_outer_free; break;
                case Align::Baseline: cross_pos = row ? baseline_asc[li] - it.base_asc + it.mar_cs : it.mar_cs; break;
                default: cross_pos = it.mar_cs; break;
                }
                if (wrap_rev && (it.align == Align::FlexStart || it.align == Align::Start || it.align == Align::Stretch))
                    cross_pos = it.mar_cs;
                if (wrap_rev && it.align == Align::FlexEnd) cross_pos = it.mar_cs + cross_outer_free;
            }
            it.pos_c = L.pos + cross_pos;
            (void)ce;

            // re-layout when the cross size was stretched
            if (stretch && std::fabs(new_cross - it.cross) > 0.01f) {
                Constraint cc;
                cc.cb_w = content_w; cc.cb_h = content_h_def; cc.bfc = nullptr;
                if (row) { cc.forced_w = it.target + it.bp_m; cc.forced_h = new_cross; }
                else { cc.forced_h = it.target + it.bp_m; cc.forced_w = new_cross; }
                layout_box(env, k, cc);
                it.cross = new_cross;
            }
            if (row) { k.x = it.pos_m; k.y = it.pos_c; }
            else { k.x = it.pos_c; k.y = it.pos_m; }
        }
    }

    // content height of the container
    if (row) return cross_size >= 0 ? cross_size : container_cross;
    return main_size;
}

Intrinsic flex_intrinsic(Env& env, Box& b) {
    const bool row = b.st->flex_direction == FlexDirection::Row || b.st->flex_direction == FlexDirection::RowReverse;
    const bool wrap = b.st->flex_wrap != FlexWrap::Nowrap;
    Intrinsic r;
    size_t n = 0;
    float gap = row ? b.st->column_gap.px : 0;
    for (auto& k : b.kids) {
        if (k->kind == Box::Kind::Marker || k->is_abs()) continue;
        const Intrinsic ki = outer_intrinsic(env, *k);
        ++n;
        if (row) {
            r.max += ki.max;
            r.min = wrap ? std::max(r.min, ki.min) : r.min + ki.min;
        } else {
            r.max = std::max(r.max, ki.max);
            r.min = std::max(r.min, ki.min);
        }
    }
    if (row && n > 1) { r.max += gap * float(n - 1); if (!wrap) r.min += gap * float(n - 1); }
    return r;
}

} // namespace luxium::layout
