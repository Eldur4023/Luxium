// Entry point of the layout engine: DOM -> box tree -> geometry -> display list.

#include <layout/box.hpp>

#include <render/layout.hpp>
#include <render/images.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <fstream>
#include <optional>
#include <sstream>

namespace luxium::render {

namespace {

std::string trim_ws(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n"), z = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, z - a + 1);
}
std::string unquote(std::string s) {
    s = trim_ws(s);
    if (s.size() >= 2 && (s.front() == '"' || s.front() == '\'') && s.back() == s.front()) s = s.substr(1, s.size() - 2);
    return s;
}
// "a, b(c, d), e" -> ["a", "b(c, d)", "e"]
std::vector<std::string> split_top(const std::string& s) {
    std::vector<std::string> out;
    int depth = 0; char q = 0; size_t from = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (q) { if (c == q) q = 0; continue; }
        if (c == '"' || c == '\'') q = c;
        else if (c == '(') ++depth;
        else if (c == ')') --depth;
        else if (c == ',' && depth == 0) { out.push_back(s.substr(from, i - from)); from = i + 1; }
    }
    out.push_back(s.substr(from));
    return out;
}

// @font-face rules -> faces of `font`.  Sources are tried in order (like a browser): local() names an installed
// font, url() a WOFF2/WOFF/TrueType/OpenType file (a format() hint rules out the rest).  A file still on its way
// over the network is asked for again at the next layout (the shell relayouts when it lands).
void load_web_fonts(Font& font, const std::vector<style::Rule>& rules, const std::string& base_dir) {
    for (const style::Rule& r : rules) {
        if (!r.font_face()) continue;
        std::string family, src, weight = "400", fstyle = "normal";
        for (const auto& d : r.decls) {
            if (d.name == "font-family") family = unquote(d.value);
            else if (d.name == "src") src = d.value;
            else if (d.name == "font-weight") weight = trim_ws(d.value);
            else if (d.name == "font-style") fstyle = trim_ws(d.value);
        }
        if (family.empty() || src.empty()) continue;
        int w = 400;
        if (weight == "bold") w = 700;
        else if (weight != "normal") {
            try { w = std::stoi(weight); } catch (...) {}
            const size_t sp = weight.find(' ');   // a range (variable font): the middle of it names the face
            if (sp != std::string::npos) { try { w = (w + std::stoi(weight.substr(sp + 1))) / 2; } catch (...) {} }
        }
        const bool italic = fstyle.starts_with("italic") || fstyle.starts_with("oblique");
        for (const std::string& entry0 : split_top(src)) {
            const std::string entry = trim_ws(entry0);
            if (entry.starts_with("local(")) {
                const size_t e = entry.rfind(')');
                if (e != std::string::npos && font.add_local_alias(family, w, italic, unquote(entry.substr(6, e - 6)))) break;
                continue;
            }
            if (!entry.starts_with("url(")) continue;
            size_t close = std::string::npos;
            int depth = 0; char q = 0;
            for (size_t i = 3; i < entry.size(); ++i) {
                const char c = entry[i];
                if (q) { if (c == q) q = 0; continue; }
                if (c == '"' || c == '\'') q = c;
                else if (c == '(') ++depth;
                else if (c == ')' && --depth == 0) { close = i; break; }
            }
            if (close == std::string::npos) continue;
            const std::string url = unquote(entry.substr(4, close - 4));
            const std::string rest = entry.substr(close + 1);
            if (rest.find("format(") != std::string::npos &&
                rest.find("woff2") == std::string::npos && rest.find("woff") == std::string::npos &&
                rest.find("truetype") == std::string::npos && rest.find("opentype") == std::string::npos) continue;   // eot, svg...
            const std::string path = render::resolve_image(base_dir, url);
            const std::string key = path + "#" + family + "#" + std::to_string(w) + (italic ? "i" : "n");
            if (font.has_web_face(key)) break;
            std::string bytes;
            const int st = render::fetch_resource(path, bytes);
            if (st == 0) break;   // on its way: not this source's turn to fail over
            if (st > 0 && font.add_web_face(family, w, italic, bytes, key)) break;
        }
    }
}

LayoutResult layout_once(const dom::Node& doc, Font& font, float vw, float vh,
                         const std::vector<style::Rule>& rules, const std::string& base_dir,
                         const std::function<bool(const dom::Node&, style::ComputedStyle&, int)>& hook) {
    using namespace luxium::layout;
    load_web_fonts(font, rules, base_dir);
    style::Resolver res;
    res.set_viewport(vw, vh);
    res.set_author_rules(&rules);
    res.post_hook = hook;
    const auto T = [] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); };
    const bool timing = std::getenv("LUXIUM_TIMING") != nullptr;
    double t0 = T();
    std::unique_ptr<Tree> tree;
    std::optional<Env> env_slot;
    double t_build = 0, t_layout = 0;
    // Container queries: styles depend on container sizes that only layout knows. Lay out, measure the query
    // containers, restyle against them, and repeat until the sizes settle (pages without containers: one pass).
    std::unordered_map<const dom::Node*, style::Resolver::Container> conts;
    for (int pass = 0; pass < 4; ++pass) {
        tree = build_tree(doc, res, base_dir);
        t_build += T() - t0; t0 = T();
        env_slot.emplace(font, res);
        Env& e = *env_slot;
        e.vw = vw; e.vh = vh; e.base_dir = base_dir;
        Box& r = *tree->root;
        Constraint c;
        c.cb_w = vw; c.cb_h = vh;
        c.bfc = nullptr;
        r.x = r.y = 0;
        layout_box(e, r, c);
        finalize_positions(e, *tree);
        t_layout += T() - t0; t0 = T();
        std::unordered_map<const dom::Node*, style::Resolver::Container> now;
        for (Box* k : tree->order)
            if (k->own_style() && k->node && k->st->container_type && !k->anonymous)
                now[k->node] = {std::max(0.f, k->content_w()), std::max(0.f, k->content_h()), k->st->container_type, k->st->container_name};
        auto same = [&] {
            if (now.size() != conts.size()) return false;
            for (const auto& [n, c2] : now) {
                auto it = conts.find(n);
                if (it == conts.end() || std::fabs(it->second.w - c2.w) > 0.01f || std::fabs(it->second.h - c2.h) > 0.01f || it->second.type != c2.type || it->second.names != c2.names) return false;
            }
            return true;
        };
        if (same()) break;
        conts = std::move(now);
        res.set_containers(conts);
    }
    Env& env = *env_slot;
    Box& root = *tree->root;

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
    out.containers = conts;
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
