// CSS Grid (Level 1 subset): explicit and implicit tracks, auto-placement
// (sparse/dense, row/column flow), named areas, spans, track sizing for
// px / % / fr / auto / min-content / max-content / minmax, gaps and alignment.

#include <layout/box.hpp>

#include <algorithm>
#include <cmath>
#include <map>

namespace luxium::layout {

using style::Align;
using style::GridLine;
using style::Track;

namespace {

struct GItem {
    Box* box = nullptr;
    int col = -1, col_span = 1, row = -1, row_span = 1;   // 0-based track indices
    float min_w = 0, max_w = 0;                            // outer content contributions (columns)
    float h = 0;                                           // outer height contribution at the final width
};

struct TrackState {
    Track t;
    float base = 0;          // resolved size (grows during sizing)
    float limit = 1e9f;      // growth limit
    float fr = 0;            // flex factor if flexible
    bool flexible = false;
    bool intrinsic_min = false;   // min is auto/min-content/max-content
    bool max_content_min = false;
    bool auto_max = false;        // max is auto: stretches with free space
};

struct Model {
    std::vector<GItem> items;
    std::vector<Track> cols, rows;          // explicit + implicit
    int n_cols = 0, n_rows = 0;
};

// ── placement ───────────────────────────────────────────────────────────────

struct Axis { int pos = -1; int span = 1; };

Axis resolve_axis(const GridLine& s, const GridLine& e, int explicit_count) {
    auto idx = [&](int line) { return line > 0 ? line - 1 : explicit_count + line + 1; };
    Axis a;
    const bool sl = !s.auto_ && s.line != 0, el = !e.auto_ && e.line != 0;
    const int ssp = s.span, esp = e.span;
    if (sl && el) {
        int p = idx(s.line), q = idx(e.line);
        if (q < p) std::swap(p, q);
        a.pos = std::max(0, p); a.span = std::max(1, q - p);
    } else if (sl) { a.pos = std::max(0, idx(s.line)); a.span = esp > 0 ? esp : 1; }
    else if (el) {
        const int sp = ssp > 0 ? ssp : 1;
        a.span = sp; a.pos = std::max(0, idx(e.line) - sp);
    } else { a.pos = -1; a.span = ssp > 0 ? ssp : (esp > 0 ? esp : 1); }
    return a;
}

Model build_model(Env& env, Box& b, float content_w) {
    const ComputedStyle& s = *b.st;
    Model m;
    m.cols = s.grid_template_columns;
    m.rows = s.grid_template_rows;
    // subgrid: the tracks are the parent's, over the span this box occupies
    auto adopt_tracks = [](std::vector<Track>& dst, const std::vector<float>& sizes) {
        dst.clear();
        for (float w : sizes) { Track t; t.kind = Track::Kind::Len; t.len = Len::Px(w); dst.push_back(t); }
    };
    if (s.grid_cols_sub && !b.sub_cols.empty()) adopt_tracks(m.cols, b.sub_cols);
    if (s.grid_rows_sub && !b.sub_rows.empty()) adopt_tracks(m.rows, b.sub_rows);

    // repeat(auto-fill / auto-fit): as many repetitions as fit
    if (s.grid_col_autorepeat && !s.grid_col_auto_pattern.empty()) {
        const float gap = s.column_gap.resolve(content_w);
        float fixed = 0;
        for (const Track& t : m.cols) fixed += t.kind == Track::Kind::Len ? t.len.resolve(content_w) : 0;
        float pat = 0;
        for (const Track& t : s.grid_col_auto_pattern) {
            if (t.kind == Track::Kind::Len) pat += t.len.resolve(content_w);
            else if (t.kind == Track::Kind::MinMax && t.min_kind == 0) pat += t.min_len.resolve(content_w);
            else pat += 1;
        }
        pat += gap * float(s.grid_col_auto_pattern.size());
        int reps = pat > 0 ? int(std::floor((content_w - fixed + gap) / pat)) : 1;
        reps = std::max(1, reps);
        std::vector<Track> exp;
        for (int r = 0; r < reps; ++r) exp.insert(exp.end(), s.grid_col_auto_pattern.begin(), s.grid_col_auto_pattern.end());
        m.cols.insert(m.cols.begin() + std::min(s.grid_col_auto_pos, m.cols.size()), exp.begin(), exp.end());
    }

    // named areas
    struct Area { int r0, r1, c0, c1; };
    std::map<std::string, Area> areas;
    int area_rows = int(s.grid_template_areas.size()), area_cols = 0;
    for (int r = 0; r < area_rows; ++r) {
        area_cols = std::max(area_cols, int(s.grid_template_areas[r].size()));
        for (int c = 0; c < int(s.grid_template_areas[r].size()); ++c) {
            const std::string& n = s.grid_template_areas[r][c];
            if (n == ".") continue;
            auto it = areas.find(n);
            if (it == areas.end()) areas[n] = {r, r + 1, c, c + 1};
            else { it->second.r1 = std::max(it->second.r1, r + 1); it->second.c1 = std::max(it->second.c1, c + 1); }
        }
    }
    int exp_cols = std::max(int(m.cols.size()), area_cols);
    int exp_rows = std::max(int(m.rows.size()), area_rows);

    // items
    for (auto& up : b.kids) {
        Box& k = *up;
        if (k.kind == Box::Kind::Marker || k.out_of_flow()) continue;
        GItem it; it.box = &k;
        const ComputedStyle& ks = *k.st;
        Axis ca, ra;
        if (!ks.grid_area_name.empty() && areas.count(ks.grid_area_name)) {
            const Area& a = areas[ks.grid_area_name];
            ca = {a.c0, a.c1 - a.c0}; ra = {a.r0, a.r1 - a.r0};
        } else {
            ca = resolve_axis(ks.grid_column_start, ks.grid_column_end, exp_cols);
            ra = resolve_axis(ks.grid_row_start, ks.grid_row_end, exp_rows);
        }
        it.col = ca.pos; it.col_span = ca.span; it.row = ra.pos; it.row_span = ra.span;
        m.items.push_back(it);
    }

    // auto-placement (transposed when the flow is by column)
    const bool flow_col = s.grid_auto_flow_column;
    const bool dense = s.grid_auto_flow_dense;
    auto major_of = [&](GItem& it) -> std::pair<int&, int&> { return flow_col ? std::pair<int&, int&>{it.col, it.col_span} : std::pair<int&, int&>{it.row, it.row_span}; };
    auto minor_of = [&](GItem& it) -> std::pair<int&, int&> { return flow_col ? std::pair<int&, int&>{it.row, it.row_span} : std::pair<int&, int&>{it.col, it.col_span}; };
    // `major` = the axis new tracks are added along (rows for row flow); `minor` = filled first
    int minor_count = flow_col ? exp_rows : exp_cols;
    int major_count = flow_col ? exp_cols : exp_rows;
    for (GItem& it : m.items) minor_count = std::max(minor_count, minor_of(it).second + (minor_of(it).first >= 0 ? minor_of(it).first : 0));
    minor_count = std::max(1, minor_count);

    std::vector<std::vector<char>> occ;   // occ[major][minor]
    auto ensure = [&](int major) { while (int(occ.size()) <= major) occ.emplace_back(size_t(minor_count), char(0)); };
    auto fits = [&](int mj, int mn, int mjs, int mns) {
        if (mn + mns > minor_count) return false;
        ensure(mj + mjs);
        for (int a = mj; a < mj + mjs; ++a) for (int c = mn; c < mn + mns; ++c) if (occ[a][c]) return false;
        return true;
    };
    auto take = [&](int mj, int mn, int mjs, int mns) {
        ensure(mj + mjs);
        for (int a = mj; a < mj + mjs; ++a) for (int c = mn; c < mn + mns; ++c) occ[a][c] = 1;
    };
    // 1. fully positioned
    for (GItem& it : m.items) {
        auto mj = major_of(it), mn = minor_of(it);
        if (mj.first >= 0 && mn.first >= 0) take(mj.first, mn.first, mj.second, mn.second);
    }
    // 2. definite major position (minor auto)
    std::map<int, int> cursor_minor;
    for (GItem& it : m.items) {
        auto mj = major_of(it), mn = minor_of(it);
        if (mj.first >= 0 && mn.first < 0) {
            int c = dense ? 0 : cursor_minor[mj.first];
            while (!fits(mj.first, c, mj.second, mn.second) && c < minor_count) ++c;
            if (c + mn.second > minor_count) { minor_count = c + mn.second; for (auto& r : occ) r.resize(size_t(minor_count), 0); }
            mn.first = c;
            take(mj.first, c, mj.second, mn.second);
            cursor_minor[mj.first] = c + mn.second;
        }
    }
    // 3. the rest, with the auto-placement cursor
    int cur_mj = 0, cur_mn = 0;
    for (GItem& it : m.items) {
        auto mj = major_of(it), mn = minor_of(it);
        if (mj.first >= 0) continue;
        if (dense) { cur_mj = 0; cur_mn = 0; }
        if (mn.first >= 0) {   // definite minor position, auto major
            if (!dense && mn.first < cur_mn) ++cur_mj;
            cur_mn = mn.first;
            while (!fits(cur_mj, cur_mn, mj.second, mn.second)) ++cur_mj;
            mj.first = cur_mj;
            take(cur_mj, cur_mn, mj.second, mn.second);
            continue;
        }
        if (mn.second > minor_count) { minor_count = mn.second; for (auto& r : occ) r.resize(size_t(minor_count), 0); }
        for (;;) {
            if (cur_mn + mn.second > minor_count) { cur_mn = 0; ++cur_mj; continue; }
            if (fits(cur_mj, cur_mn, mj.second, mn.second)) break;
            ++cur_mn;
        }
        mj.first = cur_mj; mn.first = cur_mn;
        take(cur_mj, cur_mn, mj.second, mn.second);
        cur_mn += mn.second;
    }
    for (GItem& it : m.items) major_count = std::max(major_count, major_of(it).first + major_of(it).second);
    m.n_cols = flow_col ? major_count : minor_count;
    m.n_rows = flow_col ? minor_count : major_count;
    for (GItem& it : m.items) {
        m.n_cols = std::max(m.n_cols, it.col + it.col_span);
        m.n_rows = std::max(m.n_rows, it.row + it.row_span);
    }
    // implicit tracks
    auto fill = [&](std::vector<Track>& v, const std::vector<Track>& autos, int n) {
        for (int i = int(v.size()); i < n; ++i) {
            const int k = i - int(v.size() >= 0 ? 0 : 0);
            (void)k;
            Track t;
            if (!autos.empty()) t = autos[size_t(i) % autos.size()];
            else t.kind = Track::Kind::Auto;
            v.push_back(t);
        }
    };
    fill(m.cols, s.grid_auto_columns, m.n_cols);
    fill(m.rows, s.grid_auto_rows, m.n_rows);
    (void)env;
    return m;
}

// ── track sizing ────────────────────────────────────────────────────────────

std::vector<TrackState> init_tracks(const std::vector<Track>& tr, float base_size) {
    std::vector<TrackState> out;
    const float bs = std::max(0.f, base_size);
    for (const Track& t : tr) {
        TrackState st; st.t = t;
        switch (t.kind) {
        case Track::Kind::Len: st.base = st.limit = std::max(0.f, t.len.resolve(bs)); break;
        case Track::Kind::Fr: st.flexible = true; st.fr = t.fr; st.intrinsic_min = true; break;
        case Track::Kind::Auto: st.intrinsic_min = true; st.auto_max = true; break;
        case Track::Kind::MinContent: st.intrinsic_min = true; st.limit = 0; break;
        case Track::Kind::MaxContent: st.intrinsic_min = true; st.max_content_min = true; break;
        case Track::Kind::MinMax:
            if (t.min_kind == 0) st.base = std::max(0.f, t.min_len.resolve(bs));
            else st.intrinsic_min = true;
            if (t.min_kind == 4) st.max_content_min = true;
            if (t.max_kind == 0) st.limit = std::max(st.base, t.max_len.resolve(bs));
            else if (t.max_kind == 1) { st.flexible = true; st.fr = t.max_fr; }
            else if (t.max_kind == 2) st.auto_max = true;
            else if (t.max_kind == 4) st.limit = 1e9f;
            break;
        }
        out.push_back(st);
    }
    return out;
}

// distributes `extra` over tracks spanned by an item (non-fixed first)
void distribute(std::vector<TrackState>& tr, int from, int span, float extra) {
    if (extra <= 0) return;
    std::vector<int> grow;
    for (int i = from; i < from + span; ++i) if (tr[i].intrinsic_min || tr[i].auto_max || tr[i].flexible) grow.push_back(i);
    if (grow.empty()) for (int i = from; i < from + span; ++i) grow.push_back(i);
    for (int i : grow) tr[i].base += extra / float(grow.size());
}

// resolves the fr tracks and stretches auto tracks into the free space
void resolve_free(std::vector<TrackState>& tr, float avail, float gap, bool stretch_auto) {
    const int n = int(tr.size());
    float used = gap * float(std::max(0, n - 1));
    float fr_sum = 0;
    for (auto& t : tr) { if (t.flexible) fr_sum += t.fr; }
    // non-flexible tracks first
    for (auto& t : tr) if (!t.flexible) used += t.base;
    if (fr_sum > 0) {
        // find the fr size honoring each flexible track's own minimum (its base)
        std::vector<char> fixed(size_t(n), 0);
        float fr_size = 0;
        for (int iter = 0; iter < n + 1; ++iter) {
            float free = avail - gap * float(std::max(0, n - 1));
            float fs = 0;
            for (int i = 0; i < n; ++i) {
                if (!tr[i].flexible || fixed[i]) free -= tr[i].flexible ? tr[i].base : tr[i].base;
                else if (!tr[i].flexible) free -= tr[i].base;
            }
            for (int i = 0; i < n; ++i) if (tr[i].flexible && !fixed[i]) fs += tr[i].fr;
            if (fs <= 0) break;
            fr_size = std::max(0.f, free) / std::max(fs, 1.f);
            bool changed = false;
            for (int i = 0; i < n; ++i)
                if (tr[i].flexible && !fixed[i] && tr[i].fr * fr_size < tr[i].base) { fixed[i] = 1; changed = true; }
            if (!changed) break;
        }
        for (int i = 0; i < n; ++i) if (tr[i].flexible && !fixed[i]) tr[i].base = std::max(tr[i].base, tr[i].fr * fr_size);
        return;
    }
    if (stretch_auto) {
        float free = avail - used;
        if (free > 0) {
            int autos = 0;
            for (auto& t : tr) if (t.auto_max) ++autos;
            if (autos > 0) for (auto& t : tr) if (t.auto_max) t.base += free / float(autos);
        }
    }
}

} // namespace

// The track sizes a subgrid child lays itself out on: the parent's tracks over its span, widened by half the gap
// difference at each interior boundary, minus its own border and padding at the two ends.
static void sub_tracks(const Box&, bool own_set, float own_gap, float parent_gap, int from, int span,
                       const std::vector<TrackState>& tr, std::vector<float>& out, float& gap_out, float lead, float trail) {
    const float og = own_set ? own_gap : parent_gap, d = parent_gap - og;
    out.clear();
    for (int i = 0; i < span; ++i) {
        float v = tr[size_t(from + i)].base + (i > 0 ? d / 2 : 0.f) + (i < span - 1 ? d / 2 : 0.f);
        if (i == 0) v -= lead;
        if (i == span - 1) v -= trail;
        out.push_back(std::max(0.f, v));
    }
    gap_out = og;
}

float layout_grid(Env& env, Box& b, const Constraint& c, float content_w, float content_h_def,
                  float min_h, float max_h) {
    const ComputedStyle& s = *b.st;
    Model m = build_model(env, b, content_w);
    (void)max_h;
    const float col_gap = s.grid_cols_sub && !b.sub_cols.empty() ? b.sub_cgap : s.column_gap.resolve(content_w);
    const float row_gap = s.grid_rows_sub && !b.sub_rows.empty() ? b.sub_rgap : s.row_gap.resolve(std::max(0.f, content_h_def));

    // abs children: static position at the container origin
    // (the content box acts as the grid area; justify-self / align-self place the box inside it)
    for (auto& up : b.kids) if (up->is_abs()) {
        up->static_parent = &b; up->static_x = 0; up->static_y = 0;
        auto frac = [](Align a) { return a == Align::Center ? 0.5f : (a == Align::End || a == Align::FlexEnd) ? 1.f : 0.f; };
        const ComputedStyle& ks = *up->st;
        up->static_fx = frac(ks.justify_self != Align::Auto ? ks.justify_self : s.justify_items);
        up->static_fy = frac(ks.align_self != Align::Auto ? ks.align_self : s.align_items);
        env.abs_pending.push_back(up.get());
    }
    if (m.items.empty() && m.n_cols == 0) return 0;

    // contributions of each item to the column tracks
    for (GItem& it : m.items) {
        resolve_edges(*it.box, content_w);
        const Intrinsic in = outer_intrinsic(env, *it.box);
        it.min_w = in.min; it.max_w = in.max;
    }
    std::vector<TrackState> cols = init_tracks(m.cols, content_w);
    // 1. single-span items
    for (GItem& it : m.items) {
        if (it.col_span != 1) continue;
        TrackState& t = cols[it.col];
        if (t.intrinsic_min) t.base = std::max(t.base, t.max_content_min || t.auto_max || t.flexible ? (t.max_content_min ? it.max_w : it.min_w) : it.min_w);
        if (t.auto_max && !t.flexible) {}
    }
    // auto tracks: base = min-content, then grow toward max-content when there is room
    // (the classic result: auto tracks share the free space; handled in resolve_free)
    // 2. spanning items
    for (GItem& it : m.items) {
        if (it.col_span <= 1) continue;
        float have = col_gap * float(it.col_span - 1);
        for (int i = it.col; i < it.col + it.col_span; ++i) have += cols[i].base;
        distribute(cols, it.col, it.col_span, it.min_w - have);
    }
    // max-content growth of auto tracks beyond their minimum, bounded by free space
    {
        float used = col_gap * float(std::max(0, m.n_cols - 1));
        for (auto& t : cols) used += t.base;
        float free = content_w - used;
        bool any_fr = false;
        for (auto& t : cols) if (t.flexible) any_fr = true;
        if (!any_fr && free > 0) {
            // grow each auto track up to its max-content contribution first
            std::vector<float> want(size_t(m.n_cols), 0.f);
            for (GItem& it : m.items) if (it.col_span == 1) want[it.col] = std::max(want[it.col], it.max_w);
            float need = 0;
            for (int i = 0; i < m.n_cols; ++i) if (cols[i].auto_max && want[i] > cols[i].base) need += want[i] - cols[i].base;
            if (need > 0) {
                const float share = std::min(1.f, free / need);
                for (int i = 0; i < m.n_cols; ++i)
                    if (cols[i].auto_max && want[i] > cols[i].base) cols[i].base += (want[i] - cols[i].base) * share;
            }
        }
    }
    const bool stretch_cols = s.justify_content == Align::Normal || s.justify_content == Align::Stretch;
    resolve_free(cols, content_w, col_gap, stretch_cols);
    const float cols_total = [&] { float t = col_gap * float(std::max(0, m.n_cols - 1)); for (auto& x : cols) t += x.base; return t; }();

    auto span_w = [&](const GItem& it) {
        float w = col_gap * float(it.col_span - 1);
        for (int i = it.col; i < it.col + it.col_span; ++i) w += cols[i].base;
        return w;
    };

    // ── rows ────────────────────────────────────────────────────────────────
    std::vector<TrackState> rows = init_tracks(m.rows, content_h_def);
    // each item's height at its final column span width
    for (GItem& it : m.items) {
        Box& k = *it.box;
        const ComputedStyle& ks = *k.st;
        const float aw = span_w(it);
        k.sub_cols.clear(); k.sub_rows.clear();
        if (ks.grid_cols_sub) { sub_tracks(k, ks.column_gap_set, ks.column_gap.resolve(aw), col_gap, it.col, it.col_span, cols, k.sub_cols, k.sub_cgap, k.padding.l + k.border.l, k.padding.r + k.border.r); }
        Constraint cc; cc.cb_w = aw; cc.cb_h = -1; cc.bfc = nullptr;
        const Align ja = ks.justify_self != Align::Auto ? ks.justify_self : s.justify_items;
        const bool stretch_w = (ja == Align::Normal || ja == Align::Stretch) && ks.width.is_auto && k.kind != Box::Kind::Replaced &&
                               !k.mar_l_auto && !k.mar_r_auto;
        if (stretch_w) cc.forced_w = std::max(0.f, aw - k.margin.h());
        else { cc.shrink = true; cc.avail_w = aw; }
        layout_box(env, k, cc);
        it.h = k.h + k.margin.v();
    }
    for (GItem& it : m.items) {
        if (it.row_span != 1) continue;
        TrackState& t = rows[it.row];
        if (t.intrinsic_min || t.auto_max) t.base = std::max(t.base, it.h);
    }
    // a rows-subgrid child shares its own rows with the parent: each inner row sizes the matching parent track
    // (the gap difference between parent and subgrid is taken off the tracks on either side of each boundary)
    for (GItem& it : m.items) {
        Box& k = *it.box;
        if (!k.st->grid_rows_sub || k.own_rows.empty()) continue;
        const int n = std::min<int>(it.row_span, int(k.own_rows.size()));
        const float d = k.st->row_gap_set ? row_gap - k.st->row_gap.resolve(0) : 0.f;
        for (int r = 0; r < n; ++r) {
            float need = k.own_rows[size_t(r)] - (r > 0 ? d / 2 : 0.f) - (r < n - 1 ? d / 2 : 0.f);
            if (r == 0) need += k.margin.t + k.border.t + k.padding.t;
            if (r == n - 1) need += k.margin.b + k.border.b + k.padding.b;
            TrackState& t = rows[size_t(it.row + r)];
            if (t.intrinsic_min || t.auto_max) t.base = std::max(t.base, need);
        }
    }
    for (GItem& it : m.items) {
        if (it.row_span <= 1) continue;
        if (it.box->st->grid_rows_sub && !it.box->own_rows.empty()) continue;
        float have = row_gap * float(it.row_span - 1);
        for (int i = it.row; i < it.row + it.row_span; ++i) have += rows[i].base;
        distribute(rows, it.row, it.row_span, it.h - have);
    }
    float rows_total;
    if (content_h_def < 0 && min_h > 0) {   // min-height gives the rows room to stretch
        float have = row_gap * float(std::max(0, m.n_rows - 1));
        for (auto& x : rows) have += x.base;
        if (min_h > have) {
            const bool stretch_rows = s.align_content == Align::Normal || s.align_content == Align::Stretch;
            resolve_free(rows, min_h, row_gap, stretch_rows);
        }
    }
    if (content_h_def >= 0) {
        const bool stretch_rows = s.align_content == Align::Normal || s.align_content == Align::Stretch;
        resolve_free(rows, content_h_def, row_gap, stretch_rows);
    } else {
        // indefinite height: flexible rows take their content size
        for (auto& t : rows) if (t.flexible) {}
    }
    rows_total = row_gap * float(std::max(0, m.n_rows - 1));
    for (auto& x : rows) rows_total += x.base;

    // ── positions ───────────────────────────────────────────────────────────
    auto distribute_offsets = [&](const std::vector<TrackState>& tr, float gap, float total, float container, Align ac,
                                  std::vector<float>& pos, float& extra_gap) {
        const int n = int(tr.size());
        float lead = 0; extra_gap = 0;
        const float free = container - total;
        if (container >= 0 && free > 0) {
            switch (ac) {
            case Align::Center: lead = free / 2; break;
            case Align::End: case Align::FlexEnd: lead = free; break;
            case Align::SpaceBetween: if (n > 1) extra_gap = free / float(n - 1); break;
            case Align::SpaceAround: extra_gap = free / float(n); lead = extra_gap / 2; break;
            case Align::SpaceEvenly: extra_gap = free / float(n + 1); lead = extra_gap; break;
            default: break;
            }
        }
        pos.resize(size_t(n) + 1);
        float x = lead;
        for (int i = 0; i < n; ++i) { pos[i] = x; x += tr[i].base + gap + extra_gap; }
        pos[n] = x;
    };
    std::vector<float> col_pos, row_pos;
    float cg_extra, rg_extra;
    distribute_offsets(cols, col_gap, cols_total, content_w, s.justify_content, col_pos, cg_extra);
    distribute_offsets(rows, row_gap, rows_total, content_h_def, s.align_content, row_pos, rg_extra);

    // ── place items ─────────────────────────────────────────────────────────
    for (GItem& it : m.items) {
        Box& k = *it.box;
        const ComputedStyle& ks = *k.st;
        const float ax = col_pos[it.col], ay = row_pos[it.row];
        const float aw = span_w(it);
        float ah = row_gap * float(it.row_span - 1);
        for (int i = it.row; i < it.row + it.row_span; ++i) ah += rows[i].base;
        const Align ja = ks.justify_self != Align::Auto ? ks.justify_self : s.justify_items;
        Align aa = ks.align_self != Align::Auto ? ks.align_self : s.align_items;
        const bool repl = k.kind == Box::Kind::Replaced;
        const bool stretch_w = (ja == Align::Normal || ja == Align::Stretch) && ks.width.is_auto && !repl && !k.mar_l_auto && !k.mar_r_auto;
        const bool stretch_h = (aa == Align::Normal || aa == Align::Stretch) && ks.height.is_auto && !repl && !k.mar_t_auto && !k.mar_b_auto;
        if (ks.grid_rows_sub) { k.sub_rows.clear(); sub_tracks(k, ks.row_gap_set, ks.row_gap.resolve(ah), row_gap, it.row, it.row_span, rows, k.sub_rows, k.sub_rgap, k.padding.t + k.border.t, k.padding.b + k.border.b); }
        Constraint cc; cc.cb_w = aw; cc.cb_h = ah; cc.bfc = nullptr;
        if (stretch_w) cc.forced_w = std::max(0.f, aw - k.margin.h());
        else { cc.shrink = true; cc.avail_w = aw; }
        if (stretch_h) cc.forced_h = std::max(0.f, ah - k.margin.v());
        layout_box(env, k, cc);
        // alignment inside the area
        float x = k.margin.l, y = k.margin.t;
        const float free_w = aw - k.outer_w(), free_h = ah - k.outer_h();
        if (k.mar_l_auto || k.mar_r_auto) {
            const float f = std::max(0.f, aw - k.w - (k.mar_l_auto ? 0 : k.margin.l) - (k.mar_r_auto ? 0 : k.margin.r));
            x = (k.mar_l_auto && k.mar_r_auto) ? f / 2 : (k.mar_l_auto ? f : k.margin.l);
        } else if (!stretch_w) {
            if (ja == Align::Center) x += free_w / 2;
            else if (ja == Align::End || ja == Align::FlexEnd) x += free_w;
        }
        if (k.mar_t_auto || k.mar_b_auto) {
            const float f = std::max(0.f, ah - k.h - (k.mar_t_auto ? 0 : k.margin.t) - (k.mar_b_auto ? 0 : k.margin.b));
            y = (k.mar_t_auto && k.mar_b_auto) ? f / 2 : (k.mar_t_auto ? f : k.margin.t);
        } else if (!stretch_h) {
            if (aa == Align::Center) y += free_h / 2;
            else if (aa == Align::End || aa == Align::FlexEnd) y += free_h;
        }
        k.x = ax + x; k.y = ay + y;
    }
    b.own_rows.clear();
    for (const auto& t : rows) b.own_rows.push_back(t.base);
    return content_h_def >= 0 ? content_h_def : rows_total;
}

Intrinsic grid_intrinsic(Env& env, Box& b) {
    Model m = build_model(env, b, 0);
    const ComputedStyle& s = *b.st;
    Intrinsic r;
    std::vector<float> mn(size_t(std::max(1, m.n_cols)), 0.f), mx(size_t(std::max(1, m.n_cols)), 0.f);
    for (GItem& it : m.items) {
        resolve_edges(*it.box, 0);
        const Intrinsic in = outer_intrinsic(env, *it.box);
        if (it.col_span == 1 && it.col >= 0) { mn[it.col] = std::max(mn[it.col], in.min); mx[it.col] = std::max(mx[it.col], in.max); }
    }
    for (int i = 0; i < m.n_cols; ++i) {
        const Track& t = m.cols[i];
        float lo = mn[i], hi = mx[i];
        if (t.kind == Track::Kind::Len && !t.len.has_pct()) lo = hi = t.len.px;
        else if (t.kind == Track::Kind::MinMax && t.min_kind == 0 && !t.min_len.has_pct()) { lo = std::max(lo, t.min_len.px); }
        r.min += lo; r.max += hi;
    }
    const float gap = s.column_gap.px * float(std::max(0, m.n_cols - 1));
    r.min += gap; r.max += gap;
    return r;
}

} // namespace luxium::layout
