// CSS tables: cell grid (colspan/rowspan), column widths (auto and fixed
// layout), row heights, vertical-align, border-spacing, and the collapsed
// borders model with conflict resolution.

#include <layout/box.hpp>

#include <algorithm>
#include <cmath>
#include <map>

namespace luxium::layout {

using style::BorderStyle;

namespace {

struct Cell {
    Box* box = nullptr;
    int r = 0, c = 0, rs = 1, cs = 1;
    float min = 0, max = 0;       // border-box min/max-content widths
    float spec = -1;              // specified border-box width (px)
    float pct = -1;               // specified percentage
};

struct Row { Box* box = nullptr; Box* group = nullptr; };

struct Model {
    int rows = 0, cols = 0;
    std::vector<Cell> cells;
    std::vector<Row> row_list;
    std::vector<Box*> captions;
    std::vector<float> col_fixed;     // from <col>: border-box width or -1
    std::vector<Box*> groups;         // row groups in order
};

bool is_group(Display d) {
    return d == Display::TableRowGroup || d == Display::TableHeaderGroup || d == Display::TableFooterGroup;
}

Model build_model(Box& t) {
    Model m;
    std::vector<Box*> head, body, foot;
    std::vector<std::pair<Box*, Box*>> body_rows;   // (row, group)
    auto rows_of = [&](Box* g, std::vector<Row>& out) {
        for (auto& k : g->kids) if (k->st->display == Display::TableRow) out.push_back({k.get(), g});
    };
    std::vector<Row> hr, br, fr;
    for (auto& up : t.kids) {
        Box& k = *up;
        const Display d = k.st->display;
        if (k.is_abs()) continue;
        if (d == Display::TableCaption) m.captions.push_back(&k);
        else if (d == Display::TableColumn || d == Display::TableColumnGroup) {
            if (d == Display::TableColumn) {
                float w = -1;
                if (!k.st->width.is_auto && !k.st->width.has_pct()) w = k.st->width.px;
                m.col_fixed.push_back(w);
            } else for (auto& c : k.kids) {
                float w = -1;
                if (c->st->display == Display::TableColumn && !c->st->width.is_auto && !c->st->width.has_pct()) w = c->st->width.px;
                m.col_fixed.push_back(w);
            }
        } else if (d == Display::TableHeaderGroup) { m.groups.push_back(&k); rows_of(&k, hr); }
        else if (d == Display::TableFooterGroup) { m.groups.push_back(&k); rows_of(&k, fr); }
        else if (d == Display::TableRowGroup) { m.groups.push_back(&k); rows_of(&k, br); }
        else if (d == Display::TableRow) br.push_back({&k, nullptr});
    }
    m.row_list = hr;
    m.row_list.insert(m.row_list.end(), br.begin(), br.end());
    m.row_list.insert(m.row_list.end(), fr.begin(), fr.end());
    m.rows = int(m.row_list.size());

    // place cells into the slot grid
    std::vector<std::vector<char>> occ(size_t(m.rows));
    auto taken = [&](int r, int c) { return r < m.rows && c < int(occ[r].size()) && occ[r][c]; };
    for (int r = 0; r < m.rows; ++r) {
        int c = 0;
        for (auto& up : m.row_list[r].box->kids) {
            Box& cell = *up;
            if (cell.st->display != Display::TableCell) continue;
            while (taken(r, c)) ++c;
            Cell ce; ce.box = &cell; ce.r = r; ce.c = c;
            ce.cs = std::max(1, cell.col_span); ce.rs = std::max(1, std::min(cell.row_span, m.rows - r));
            for (int rr = r; rr < r + ce.rs; ++rr) {
                if (int(occ[rr].size()) < c + ce.cs) occ[rr].resize(size_t(c + ce.cs), 0);
                for (int cc = c; cc < c + ce.cs; ++cc) occ[rr][cc] = 1;
            }
            c += ce.cs;
            m.cols = std::max(m.cols, c);
            m.cells.push_back(ce);
        }
    }
    m.cols = std::max(m.cols, int(m.col_fixed.size()));
    m.col_fixed.resize(size_t(m.cols), -1.f);
    return m;
}

int style_rank(BorderStyle s) {
    switch (s) {
    case BorderStyle::Double: return 7; case BorderStyle::Solid: return 6; case BorderStyle::Dashed: return 5;
    case BorderStyle::Dotted: return 4; case BorderStyle::Ridge: return 3; case BorderStyle::Outset: return 2;
    case BorderStyle::Groove: return 1; case BorderStyle::Inset: return 0; default: return -1;
    }
}

struct Border { float w = 0; uint32_t color = 0; BorderStyle style = BorderStyle::None; bool hidden = false; };

Border pick(const Border& a, const Border& b) {
    if (a.hidden) return a;
    if (b.hidden) return b;
    if (a.style == BorderStyle::None || a.w <= 0) return b;
    if (b.style == BorderStyle::None || b.w <= 0) return a;
    if (a.w != b.w) return a.w > b.w ? a : b;
    return style_rank(a.style) >= style_rank(b.style) ? a : b;
}

Border border_of(const ComputedStyle& s, int side) {
    Border b;
    b.style = s.border_style[side];
    b.hidden = b.style == BorderStyle::Hidden;
    b.w = (b.style == BorderStyle::None || b.hidden) ? 0.f : (s.border_w[side] > 0 ? std::max(1.f, std::floor(s.border_w[side])) : 0.f);
    b.color = s.border_color[side];
    return b;
}

// cells' min/max widths
void measure_cells(Env& env, Model& m, bool collapse) {
    for (Cell& ce : m.cells) {
        Box& b = *ce.box;
        resolve_edges(b, 0);
        const float bp = b.border.h() + b.padding.h();   // collapsed cells carry their half borders
        const Intrinsic in = intrinsic_widths(env, b);
        ce.min = in.min + bp; ce.max = in.max + bp;
        const style::Len& w = b.st->width;
        if (!w.is_auto && !w.has_pct()) {
            float v = w.px;
            if (b.st->box_sizing == style::BoxSizing::Content) v += bp;
            ce.spec = v;
            ce.max = std::max(ce.min, v);
            ce.min = std::max(ce.min, collapse ? 0.f : 0.f);
        } else if (w.has_pct()) ce.pct = w.pct;
    }
}

struct Cols { std::vector<float> min, max, w; std::vector<float> pct; std::vector<char> fixed; };

Cols column_sizes(Model& m) {
    Cols c;
    const size_t n = size_t(m.cols);
    c.min.assign(n, 0); c.max.assign(n, 0); c.pct.assign(n, -1); c.fixed.assign(n, 0);
    for (size_t i = 0; i < n; ++i) if (m.col_fixed[i] >= 0) { c.min[i] = c.max[i] = m.col_fixed[i]; c.fixed[i] = 1; }
    // single-span cells first
    for (const Cell& ce : m.cells) {
        if (ce.cs != 1) continue;
        c.min[ce.c] = std::max(c.min[ce.c], ce.min);
        c.max[ce.c] = std::max(c.max[ce.c], ce.max);
        if (ce.pct >= 0) c.pct[ce.c] = std::max(c.pct[ce.c], ce.pct);
        if (ce.spec >= 0) c.fixed[ce.c] = 1;
    }
    // spanning cells: distribute what the columns don't already provide
    for (const Cell& ce : m.cells) {
        if (ce.cs == 1) continue;
        float have_min = 0, have_max = 0;
        for (int i = ce.c; i < ce.c + ce.cs; ++i) { have_min += c.min[i]; have_max += c.max[i]; }
        const float add_min = std::max(0.f, ce.min - have_min), add_max = std::max(0.f, ce.max - have_max);
        for (int i = ce.c; i < ce.c + ce.cs; ++i) {
            const float share = have_max > 0 ? c.max[i] / have_max : 1.f / float(ce.cs);
            c.min[i] += add_min * share;
            c.max[i] += add_max * share;
        }
    }
    for (size_t i = 0; i < n; ++i) c.max[i] = std::max(c.max[i], c.min[i]);
    return c;
}

// Assigns widths for a given available column area `avail`.
void assign_widths(Cols& c, float avail, bool table_width_set) {
    const size_t n = c.min.size();
    c.w.assign(n, 0);
    float sum_min = 0, sum_max = 0;
    for (size_t i = 0; i < n; ++i) { sum_min += c.min[i]; sum_max += c.max[i]; }
    if (avail <= sum_min + 0.001f) { c.w = c.min; return; }
    // percentage columns take their share first
    std::vector<float> w(n, -1);
    float pct_total = 0, pct_px = 0;
    for (size_t i = 0; i < n; ++i) if (c.pct[i] >= 0) { pct_total += c.pct[i]; }
    if (pct_total > 0) {
        const float scale = pct_total > 100 ? 100.f / pct_total : 1.f;
        for (size_t i = 0; i < n; ++i)
            if (c.pct[i] >= 0) { w[i] = std::max(c.min[i], avail * c.pct[i] * scale / 100.f); pct_px += w[i]; }
    }
    float rest_min = 0, rest_max = 0;
    for (size_t i = 0; i < n; ++i) if (w[i] < 0) { rest_min += c.min[i]; rest_max += c.max[i]; }
    const float rest_avail = avail - pct_px;
    if (rest_avail <= rest_min) { for (size_t i = 0; i < n; ++i) c.w[i] = w[i] >= 0 ? w[i] : c.min[i]; return; }
    if (rest_avail >= rest_max) {
        // extra space: distributed over auto columns in proportion to max width
        float extra = rest_avail - rest_max;
        float weight = 0;
        for (size_t i = 0; i < n; ++i) if (w[i] < 0 && !c.fixed[i]) weight += std::max(1.f, c.max[i]);
        bool use_fixed = false;
        if (weight <= 0 || !table_width_set) { if (weight <= 0) { use_fixed = true; for (size_t i = 0; i < n; ++i) if (w[i] < 0) weight += std::max(1.f, c.max[i]); } }
        for (size_t i = 0; i < n; ++i) {
            if (w[i] >= 0) { c.w[i] = w[i]; continue; }
            float v = c.max[i];
            if (extra > 0 && weight > 0 && (use_fixed || !c.fixed[i])) v += extra * std::max(1.f, c.max[i]) / weight;
            c.w[i] = v;
        }
        return;
    }
    // between min and max: columns with a specified width keep it while the others can still
    // shrink toward their minimum; only when that is not enough does everything scale
    float fix_max = 0, auto_min = 0, auto_max = 0;
    for (size_t i = 0; i < n; ++i) {
        if (w[i] >= 0) continue;
        if (c.fixed[i]) fix_max += c.max[i]; else { auto_min += c.min[i]; auto_max += c.max[i]; }
    }
    if (fix_max > 0 && auto_max > auto_min && rest_avail >= fix_max + auto_min) {
        const float t = (rest_avail - fix_max - auto_min) / (auto_max - auto_min);
        for (size_t i = 0; i < n; ++i) {
            if (w[i] >= 0) c.w[i] = w[i];
            else if (c.fixed[i]) c.w[i] = c.max[i];
            else c.w[i] = c.min[i] + (c.max[i] - c.min[i]) * std::min(1.f, t);
        }
        return;
    }
    const float t = rest_max > rest_min ? (rest_avail - rest_min) / (rest_max - rest_min) : 0.f;
    for (size_t i = 0; i < n; ++i) c.w[i] = w[i] >= 0 ? w[i] : c.min[i] + (c.max[i] - c.min[i]) * t;
}

float first_baseline(const Box& b) {
    const float inset = b.border.t + b.padding.t;
    if (!b.lines.empty()) return inset + b.lines.front().y + b.lines.front().baseline;
    for (const auto& k : b.kids) {
        if (k->kind != Box::Kind::Block || k->out_of_flow()) continue;
        const float kb = first_baseline(*k);
        if (kb >= 0) return inset + k->y + kb;
    }
    return -1;
}

} // namespace

Intrinsic table_intrinsic(Env& env, Box& b) {
    Model m = build_model(b);
    const bool collapse = b.st->border_collapse;
    measure_cells(env, m, collapse);
    Cols c = column_sizes(m);
    const float sp = collapse ? 0.f : b.st->border_spacing_h;
    Intrinsic r;
    for (int i = 0; i < m.cols; ++i) { r.min += c.min[i]; r.max += c.max[i]; }
    const float extra = sp * float(m.cols + 1);
    r.min += extra; r.max += extra;
    for (Box* cap : m.captions) { const Intrinsic ci = outer_intrinsic(env, *cap); r.min = std::max(r.min, ci.min); r.max = std::max(r.max, ci.max); }
    return r;
}

void layout_table(Env& env, Box& b, const Constraint& c) {
    const ComputedStyle& s = *b.st;
    resolve_edges(b, c.cb_w);
    const bool collapse = s.border_collapse;
    Model m = build_model(b);
    const float sp_h = collapse ? 0.f : s.border_spacing_h, sp_v = collapse ? 0.f : s.border_spacing_v;

    // ── collapsed border model: edge widths ────────────────────────────────
    // vertical edges v[r][c] (c in 0..cols), horizontal edges h[r][c] (r in 0..rows)
    std::vector<std::vector<Border>> vedge, hedge;
    std::vector<std::vector<Cell*>> grid;
    if (collapse && m.rows > 0 && m.cols > 0) {
        vedge.assign(size_t(m.rows), std::vector<Border>(size_t(m.cols + 1)));
        hedge.assign(size_t(m.rows + 1), std::vector<Border>(size_t(m.cols)));
        grid.assign(size_t(m.rows), std::vector<Cell*>(size_t(m.cols), nullptr));
        for (Cell& ce : m.cells)
            for (int r = ce.r; r < ce.r + ce.rs; ++r) for (int cc = ce.c; cc < ce.c + ce.cs; ++cc) grid[r][cc] = &ce;
        auto set_v = [&](int r, int cidx, const Border& bd) { vedge[r][cidx] = pick(vedge[r][cidx], bd); };
        auto set_h = [&](int r, int cidx, const Border& bd) { hedge[r][cidx] = pick(hedge[r][cidx], bd); };
        for (Cell& ce : m.cells) {
            const ComputedStyle& cs = *ce.box->st;
            for (int r = ce.r; r < ce.r + ce.rs; ++r) { set_v(r, ce.c, border_of(cs, 3)); set_v(r, ce.c + ce.cs, border_of(cs, 1)); }
            for (int cc = ce.c; cc < ce.c + ce.cs; ++cc) { set_h(ce.r, cc, border_of(cs, 0)); set_h(ce.r + ce.rs, cc, border_of(cs, 2)); }
        }
        // rows and the table itself contribute at the outer edges
        for (int r = 0; r < m.rows; ++r) {
            const ComputedStyle& rs = *m.row_list[r].box->st;
            for (int cc = 0; cc < m.cols; ++cc) { if (r == 0) set_h(0, cc, border_of(rs, 0)); if (r == m.rows - 1) set_h(m.rows, cc, border_of(rs, 2)); }
            set_v(r, 0, border_of(rs, 3)); set_v(r, m.cols, border_of(rs, 1));
        }
        for (int cc = 0; cc < m.cols; ++cc) { set_h(0, cc, border_of(s, 0)); set_h(m.rows, cc, border_of(s, 2)); }
        for (int r = 0; r < m.rows; ++r) { set_v(r, 0, border_of(s, 3)); set_v(r, m.cols, border_of(s, 1)); }
        // each cell gets half of every shared edge
        for (Cell& ce : m.cells) {
            Box& cb = *ce.box;
            cb.border_collapsed = true;
            resolve_edges(cb, 0);
            const float l = vedge[ce.r][ce.c].w, r = vedge[ce.r][ce.c + ce.cs].w;
            const float t = hedge[ce.r][ce.c].w, bt = hedge[ce.r + ce.rs][ce.c].w;
            cb.coll_border = Edges{t / 2, r / 2, bt / 2, l / 2};
            cb.border = cb.coll_border;
        }
    }
    // the table's own border in the collapsed model: half of the outermost edges
    if (collapse && m.rows > 0 && m.cols > 0) {
        float lt = 0, rt = 0, tt = 0, bt = 0;
        for (int r = 0; r < m.rows; ++r) { lt = std::max(lt, vedge[r][0].w); rt = std::max(rt, vedge[r][m.cols].w); }
        for (int cc = 0; cc < m.cols; ++cc) { tt = std::max(tt, hedge[0][cc].w); bt = std::max(bt, hedge[m.rows][cc].w); }
        b.border_collapsed = true;
        b.coll_border = Edges{tt / 2, rt / 2, bt / 2, lt / 2};
        b.border = b.coll_border;
        b.padding = Edges{};
    }

    measure_cells(env, m, collapse);
    Cols cols = column_sizes(m);

    // ── table width ─────────────────────────────────────────────────────────
    const float bp_h = b.border.h() + b.padding.h();
    const float spacing_w = sp_h * float(m.cols + 1);
    float table_w;      // border box
    bool width_set = false;
    if (c.forced_w >= 0) { table_w = c.forced_w; width_set = true; }
    else if (!s.width.is_auto && !(s.width.has_pct() && c.cb_w < 0)) {
        float v = resolve_len(s.width, c.cb_w);
        if (s.box_sizing == style::BoxSizing::Content) v += bp_h;
        table_w = v; width_set = true;
    } else {
        float sum_min = 0, sum_max = 0;
        for (int i = 0; i < m.cols; ++i) { sum_min += cols.min[i]; sum_max += cols.max[i]; }
        // captions widen the table
        float cap_w = 0;
        for (Box* cap : m.captions) cap_w = std::max(cap_w, outer_intrinsic(env, *cap).min);
        const float avail = c.shrink && c.avail_w >= 0 ? c.avail_w : c.cb_w;
        const float room = std::max(0.f, avail - b.margin.h()) ;
        const float want = sum_max + spacing_w + bp_h;
        table_w = std::min(want, std::max(room, sum_min + spacing_w + bp_h));
        table_w = std::max(table_w, cap_w + bp_h);
    }
    float sum_min = 0;
    for (int i = 0; i < m.cols; ++i) sum_min += cols.min[i];
    table_w = std::max(table_w, sum_min + spacing_w + bp_h);
    if (!s.max_width.is_auto && !s.max_width.has_pct()) {
        float mw = s.max_width.px; if (s.box_sizing == style::BoxSizing::Content) mw += bp_h;
        table_w = std::max(sum_min + spacing_w + bp_h, std::min(table_w, mw));
    }
    if (s.table_fixed && width_set) {
        // table-layout: fixed -- the first row (or <col>) names the widths, content never moves them; what is
        // not named is shared out equally, and any extra goes evenly to every column
        const size_t n = size_t(m.cols);
        std::vector<float> wv(n, -1);
        for (size_t i = 0; i < n; ++i) if (m.col_fixed[i] >= 0) wv[i] = m.col_fixed[i];
        for (const Cell& ce : m.cells)
            if (ce.r == 0 && ce.cs == 1 && ce.spec >= 0 && wv[ce.c] < 0) wv[ce.c] = ce.spec;
        const float avail_cols = std::max(0.f, table_w - bp_h - spacing_w);
        float named = 0; int autos = 0;
        for (float v : wv) { if (v >= 0) named += v; else ++autos; }
        cols.w.assign(n, 0);
        if (autos > 0) {
            const float each = std::max(0.f, avail_cols - named) / float(autos);
            for (size_t i = 0; i < n; ++i) cols.w[i] = wv[i] >= 0 ? wv[i] : each;
        } else {
            const float extra = n ? std::max(0.f, avail_cols - named) / float(n) : 0.f;
            for (size_t i = 0; i < n; ++i) cols.w[i] = wv[i] + extra;
        }
    } else assign_widths(cols, table_w - bp_h - spacing_w, width_set);
    // the table is exactly as wide as its columns when auto-sized
    if (!width_set) {
        float sum = 0;
        for (int i = 0; i < m.cols; ++i) sum += cols.w[i];
        table_w = sum + spacing_w + bp_h;
    }
    b.w = table_w;

    // column x positions (relative to the table content origin)
    std::vector<float> col_x(size_t(m.cols) + 1);
    {
        float x = sp_h;
        for (int i = 0; i < m.cols; ++i) { col_x[i] = x; x += cols.w[i] + sp_h; }
        col_x[m.cols] = x;
    }
    auto span_w = [&](const Cell& ce) {
        float w = sp_h * float(ce.cs - 1);
        for (int i = ce.c; i < ce.c + ce.cs; ++i) w += cols.w[i];
        return w;
    };

    // ── row heights ─────────────────────────────────────────────────────────
    std::vector<float> row_h(size_t(m.rows), 0.f);
    for (int r = 0; r < m.rows; ++r) {
        const ComputedStyle& rs = *m.row_list[r].box->st;
        if (!rs.height.is_auto && !rs.height.has_pct()) row_h[r] = rs.height.px;
    }
    struct CellRes { float h = 0; float baseline = -1; };
    std::vector<CellRes> res(m.cells.size());
    for (size_t i = 0; i < m.cells.size(); ++i) {
        Cell& ce = m.cells[i];
        Box& cb = *ce.box;
        Constraint cc; cc.cb_w = span_w(ce); cc.cb_h = -1; cc.bfc = nullptr; cc.forced_w = span_w(ce);
        layout_box(env, cb, cc);
        float h = cb.h;
        if (!cb.st->height.is_auto && !cb.st->height.has_pct()) {
            float v = cb.st->height.px;
            if (cb.st->box_sizing == style::BoxSizing::Content) v += cb.border.v() + cb.padding.v();
            h = std::max(h, v);
        }
        res[i].h = h;
        res[i].baseline = first_baseline(cb);
        if (ce.rs == 1) row_h[ce.r] = std::max(row_h[ce.r], h);
    }
    // baseline-aligned cells: rows grow so that baselines meet
    std::vector<float> row_base(size_t(m.rows), 0.f);
    for (size_t i = 0; i < m.cells.size(); ++i) {
        const Cell& ce = m.cells[i];
        if (ce.rs != 1 || ce.box->st->vertical_align != style::VerticalAlign::Baseline) continue;
        const float bl = res[i].baseline >= 0 ? res[i].baseline : res[i].h;
        row_base[ce.r] = std::max(row_base[ce.r], bl);
    }
    for (size_t i = 0; i < m.cells.size(); ++i) {
        const Cell& ce = m.cells[i];
        if (ce.rs != 1 || ce.box->st->vertical_align != style::VerticalAlign::Baseline) continue;
        const float bl = res[i].baseline >= 0 ? res[i].baseline : res[i].h;
        row_h[ce.r] = std::max(row_h[ce.r], row_base[ce.r] - bl + res[i].h);
    }
    // rowspan cells: spread any shortfall over the rows they span
    for (size_t i = 0; i < m.cells.size(); ++i) {
        const Cell& ce = m.cells[i];
        if (ce.rs <= 1) continue;
        float have = sp_v * float(ce.rs - 1);
        for (int r = ce.r; r < ce.r + ce.rs; ++r) have += row_h[r];
        if (res[i].h > have) for (int r = ce.r; r < ce.r + ce.rs; ++r) row_h[r] += (res[i].h - have) / float(ce.rs);
    }
    // collapsed borders add the shared edges' halves to the row geometry already via cell borders

    // a specified table height is a minimum: extra goes to the rows
    float rows_sum = sp_v * float(m.rows + 1);
    for (float h : row_h) rows_sum += h;
    float cap_top = 0, cap_bottom = 0;
    for (Box* cap : m.captions) {
        Constraint cc; cc.cb_w = table_w - bp_h; cc.cb_h = -1; cc.bfc = nullptr;
        cc.forced_w = std::max(0.f, table_w - bp_h - cap->margin.h());
        resolve_edges(*cap, table_w - bp_h);
        cc.forced_w = std::max(0.f, table_w - bp_h - cap->margin.h());
        layout_box(env, *cap, cc);
        const bool bottom = false;   // caption-side: bottom is not modelled
        (bottom ? cap_bottom : cap_top) += cap->outer_h();
    }
    float table_h = rows_sum + b.border.v() + b.padding.v() + cap_top + cap_bottom;
    float spec_h = -1;
    if (c.forced_h >= 0) spec_h = c.forced_h;
    else if (!s.height.is_auto && !(s.height.has_pct() && c.cb_h < 0)) {
        float v = resolve_len(s.height, c.cb_h);
        if (s.box_sizing == style::BoxSizing::Content) v += b.border.v() + b.padding.v();
        spec_h = v;
    }
    if (spec_h > table_h && m.rows > 0) {
        const float extra = spec_h - table_h;
        float tot = 0;
        for (float h : row_h) tot += h;
        for (float& h : row_h) h += tot > 0 ? extra * h / tot : extra / float(m.rows);
        table_h = spec_h;
    }
    b.h = table_h;

    // row y positions
    std::vector<float> row_y(size_t(m.rows) + 1);
    {
        float y = cap_top + sp_v;
        for (int r = 0; r < m.rows; ++r) { row_y[r] = y; y += row_h[r] + sp_v; }
        row_y[m.rows] = y;
    }
    // captions on top
    {
        float y = 0;
        for (Box* cap : m.captions) {
            cap->x = cap->margin.l; cap->y = y + cap->margin.t; cap->lparent = &b;
            y += cap->outer_h();
        }
    }
    // rows, cells and groups: geometry relative to the table content origin
    const float cells_w = col_x[m.cols] - sp_h - col_x[0];
    for (int r = 0; r < m.rows; ++r) {
        Box& rb = *m.row_list[r].box;
        rb.lparent = &b; rb.x = col_x[0]; rb.y = row_y[r]; rb.w = cells_w; rb.h = row_h[r];
        resolve_edges(rb, 0);
        rb.border = Edges{}; rb.padding = Edges{};
    }
    for (Box* g : m.groups) {
        int r0 = -1, r1 = -1;
        for (int r = 0; r < m.rows; ++r) if (m.row_list[r].group == g) { if (r0 < 0) r0 = r; r1 = r; }
        if (r0 < 0) continue;
        g->lparent = &b; g->x = col_x[0]; g->y = row_y[r0]; g->w = cells_w;
        g->h = row_y[r1] + row_h[r1] - row_y[r0];
        g->border = Edges{}; g->padding = Edges{}; g->margin = Edges{};
    }
    for (size_t i = 0; i < m.cells.size(); ++i) {
        Cell& ce = m.cells[i];
        Box& cb = *ce.box;
        cb.lparent = &b;
        float h = sp_v * float(ce.rs - 1);
        for (int r = ce.r; r < ce.r + ce.rs; ++r) h += row_h[r];
        cb.x = col_x[ce.c];
        cb.y = row_y[ce.r];
        const float extra = h - cb.h;
        using VA = style::VerticalAlign;
        float shift = 0;
        switch (cb.st->vertical_align) {
        case VA::Middle: shift = extra / 2; break;
        case VA::Bottom: case VA::TextBottom: shift = extra; break;
        case VA::Baseline: {
            if (ce.rs == 1) {
                const float bl = res[i].baseline >= 0 ? res[i].baseline : res[i].h;
                shift = row_base[ce.r] - bl;
            }
            break;
        }
        default: break;
        }
        if (shift > 0) cb.padding.t += shift;   // moves the content down inside the cell
        cb.h = h;
    }

    // collapsed edge segments for painting (relative to the table border box)
    b.table_edges.clear();
    if (collapse && m.rows > 0 && m.cols > 0) {
        const float ox = b.border.l + b.padding.l, oy = b.border.t + b.padding.t;
        std::vector<float> xs(size_t(m.cols) + 1), ys(size_t(m.rows) + 1);
        float x = 0;
        for (int i = 0; i <= m.cols; ++i) { xs[i] = x; if (i < m.cols) x += cols.w[i]; }
        float y = cap_top;
        for (int r = 0; r <= m.rows; ++r) { ys[r] = y; if (r < m.rows) y += row_h[r]; }
        for (int r = 0; r < m.rows; ++r)
            for (int cc = 0; cc <= m.cols; ++cc) {
                const Border& bd = vedge[r][cc];
                if (bd.w <= 0 || bd.style == BorderStyle::None || bd.hidden) continue;
                Box::Edge e; e.w = bd.w; e.h = row_h[r]; e.color = bd.color; e.style = bd.style;
                e.x = ox + xs[cc] - std::floor(bd.w / 2); e.y = oy + ys[r];
                b.table_edges.push_back(e);
            }
        for (int r = 0; r <= m.rows; ++r)
            for (int cc = 0; cc < m.cols; ++cc) {
                const Border& bd = hedge[r][cc];
                if (bd.w <= 0 || bd.style == BorderStyle::None || bd.hidden) continue;
                Box::Edge e; e.w = cols.w[cc] + (cc == m.cols - 1 ? 0.f : 0.f); e.h = bd.w; e.color = bd.color; e.style = bd.style;
                e.x = ox + xs[cc]; e.y = oy + ys[r] - std::floor(bd.w / 2);
                b.table_edges.push_back(e);
            }
    }
    b.baseline = -1;
}

} // namespace luxium::layout
