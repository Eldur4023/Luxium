// Entry point of the layout engine: DOM -> box tree -> geometry -> display list.

#include <layout/box.hpp>

#include <render/layout.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace luxium::render {

namespace {

LayoutResult layout_once(const dom::Node& doc, Font& font, float vw, float vh,
                         const std::vector<style::Rule>& rules, const std::string& base_dir,
                         const std::function<bool(const dom::Node&, style::ComputedStyle&, int)>& hook) {
    using namespace luxium::layout;
    style::Resolver res;
    res.set_viewport(vw, vh);
    res.set_author_rules(&rules);
    res.post_hook = hook;
    const auto T = [] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); };
    const bool timing = std::getenv("LUXIUM_TIMING") != nullptr;
    double t0 = T();
    auto tree = build_tree(doc, res, base_dir);
    const double t_build = T() - t0; t0 = T();
    Env env{font, res};
    env.vw = vw; env.vh = vh; env.base_dir = base_dir;

    Box& root = *tree->root;
    Constraint c;
    c.cb_w = vw; c.cb_h = vh;
    c.bfc = nullptr;
    root.x = root.y = 0;
    layout_box(env, root, c);
    finalize_positions(env, *tree);
    const double t_layout = T() - t0; t0 = T();

    // document height: the root box and anything that overflows it
    float bottom = root.ay + root.h;
    for (Box* k : tree->order)
        if (k->kind == Box::Kind::Block || k->kind == Box::Kind::Replaced) {
            if (k->st->position == style::Position::Fixed) continue;
            bottom = std::max(bottom, k->ay + k->h);
        }

    if (std::getenv("LUXIUM_DEBUG_TREE")) {
        for (Box* k : tree->order) {
            if (k->kind == Box::Kind::Text) continue;
            int d = 0;
            for (Box* p = k->parent; p; p = p->parent) ++d;
            std::fprintf(stderr, "%*s%s%s kind=%d ax=%.1f ay=%.1f w=%.1f h=%.1f x=%.1f y=%.1f\n", d * 2, "",
                         k->node ? k->node->name.c_str() : "#anon", k->anonymous ? "(anon)" : "", int(k->kind),
                         k->ax, k->ay, k->w, k->h, k->x, k->y);
            for (const auto& l : k->lines)
                std::fprintf(stderr, "%*s  line y=%.1f h=%.1f base=%.1f frags=%zu\n", d * 2, "", l.y, l.h, l.baseline, l.frags.size());
        }
    }

    LayoutResult out;
    out.content_height = bottom;
    PaintOutput po;
    paint_tree(env, *tree, vw, std::max(bottom, vh), po);
    out.canvas_bg = po.canvas_bg;
    out.list = std::move(po.main);
    out.fixed = std::move(po.fixed);
    for (auto& h : po.hits) {
        HitBox hb{h.node, h.x, h.y, h.w, h.h, h.button, h.cursor, h.ctl, h.tx, h.ty, h.th, h.px, h.face, h.password,
                  h.clip_x0, h.clip_y0, h.clip_x1, h.clip_y1, h.max_sx, h.max_sy, h.sb_x, h.sb_y, h.sb_w, h.sb_h, h.sticky, h.y};
        out.hits.push_back(hb);
    }
    for (auto& h : po.areas) {
        HitBox hb{h.node, h.x, h.y, h.w, h.h, false, h.cursor, -1, 0, 0, 0, 0, 0, false,
                  h.clip_x0, h.clip_y0, h.clip_x1, h.clip_y1, h.max_sx, h.max_sy, h.sb_x, h.sb_y, h.sb_w, h.sb_h, h.sticky, h.y};
        out.areas.push_back(hb);
    }
    for (auto& b : po.boxes) out.boxes.push_back({b.name, b.x, b.y, b.w, b.h});
    if (timing) std::fprintf(stderr, "  build(style+boxes) %.2f  layout %.2f  paint %.2f ms\n", t_build, t_layout, T() - t0);
    out.media = std::move(po.media);
    out.sticky = std::move(po.sticky);
    for (auto& w : po.words) out.words.push_back({w.text, w.x, w.y, w.w, w.h});
    return out;
}

} // namespace

constexpr float kScrollbarW = 15.f;   // Chromium's classic scrollbar

LayoutResult layout(const dom::Node& root, Font& font, float viewport_w,
                    const std::vector<style::Rule>& page_css,
                    const std::string& base_dir, float vp_y, float vp_h,
                    std::function<bool(const dom::Node&, style::ComputedStyle&, int)> style_hook,
                    bool assume_gutter) {
    const float vh = vp_h - vp_y;
    // The vertical scrollbar takes its gutter from the layout width.  Whether one is needed is only known after
    // laying out, so the previous layout's answer is the first guess (usually right: one pass instead of two).
    LayoutResult out = layout_once(root, font, assume_gutter ? viewport_w - kScrollbarW : viewport_w, vh, page_css, base_dir, style_hook);
    out.gutter = assume_gutter;
    if (out.content_height > vh && !assume_gutter) {
        out = layout_once(root, font, viewport_w - kScrollbarW, vh, page_css, base_dir, style_hook);
        out.gutter = true;
    } else if (out.content_height <= vh && assume_gutter) {
        out = layout_once(root, font, viewport_w, vh, page_css, base_dir, style_hook);
        out.gutter = false;
    }
    return out;
}

const dom::Node* hit_test(const LayoutResult& l, float x, float y) {
    for (auto it = l.hits.rbegin(); it != l.hits.rend(); ++it)
        if (x >= it->x && x <= it->x + it->w && y >= it->y && y <= it->y + it->h && it->in_clip(x, y)) return it->node;
    return nullptr;
}

} // namespace luxium::render
