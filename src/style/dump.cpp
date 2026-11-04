#include "style/dump.hpp"

#include <cmath>
#include <cstdio>
#include <functional>

namespace luxium::style {

namespace {

std::string num(float v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.4f", v);
    std::string s = buf;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    if (s == "-0") s = "0";
    return s;
}
std::string px(float v) { return num(v) + "px"; }

std::string color(uint32_t c) {
    const unsigned a = c >> 24, r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
    if (a == 255) return "rgb(" + std::to_string(r) + ", " + std::to_string(g) + ", " + std::to_string(b) + ")";
    return "rgba(" + std::to_string(r) + ", " + std::to_string(g) + ", " + std::to_string(b) + ", " +
           num(std::round(a / 255.f * 1000.f) / 1000.f) + ")";
}

std::string len(const Len& l) {
    if (l.is_auto) return "auto";
    if (l.has_pct() && l.px == 0) return num(l.pct) + "%";
    return px(l.px);
}

const char* display_name(Display d) {
    switch (d) {
    case Display::None: return "none"; case Display::Contents: return "contents";
    case Display::Block: return "block"; case Display::Inline: return "inline";
    case Display::InlineBlock: return "inline-block"; case Display::Flex: return "flex";
    case Display::InlineFlex: return "inline-flex"; case Display::Grid: return "grid";
    case Display::InlineGrid: return "inline-grid"; case Display::Table: return "table";
    case Display::InlineTable: return "inline-table"; case Display::TableRow: return "table-row";
    case Display::TableCell: return "table-cell"; case Display::TableRowGroup: return "table-row-group";
    case Display::TableHeaderGroup: return "table-header-group"; case Display::TableFooterGroup: return "table-footer-group";
    case Display::TableColumn: return "table-column"; case Display::TableColumnGroup: return "table-column-group";
    case Display::TableCaption: return "table-caption"; case Display::ListItem: return "list-item";
    case Display::FlowRoot: return "flow-root";
    }
    return "inline";
}

const char* bstyle(BorderStyle b) {
    static const char* n[] = {"none", "hidden", "solid", "dashed", "dotted", "double", "groove", "ridge", "inset", "outset"};
    return n[static_cast<int>(b)];
}

const char* align_name(Align a) {
    static const char* n[] = {"auto", "normal", "stretch", "start", "end", "center", "flex-start", "flex-end",
                              "baseline", "space-between", "space-around", "space-evenly"};
    return n[static_cast<int>(a)];
}

using Getter = std::function<std::string(const ComputedStyle&, bool& include)>;

const std::vector<std::pair<std::string, Getter>>& table() {
    static const std::vector<std::pair<std::string, Getter>> t = [] {
        std::vector<std::pair<std::string, Getter>> v;
        auto add = [&](const std::string& n, Getter g) { v.emplace_back(n, std::move(g)); };
        add("color", [](const ComputedStyle& s, bool&) { return color(s.color); });
        add("background-color", [](const ComputedStyle& s, bool&) { return color(s.bg_color); });
        add("font-size", [](const ComputedStyle& s, bool&) { return px(s.font_size); });
        add("font-weight", [](const ComputedStyle& s, bool&) { return std::to_string(s.font_weight); });
        add("font-style", [](const ComputedStyle& s, bool&) { return std::string(s.italic ? "italic" : "normal"); });
        add("line-height", [](const ComputedStyle& s, bool&) {
            return s.lh_mode == 0 ? std::string("normal") : px(s.line_px()); });
        add("direction", [](const ComputedStyle& s, bool&) { return std::string(s.rtl ? "rtl" : "ltr"); });
        add("text-align", [](const ComputedStyle& s, bool&) {
            static const char* n[] = {"start", "end", "left", "right", "center", "justify"};
            return std::string(n[static_cast<int>(s.text_align)]); });
        add("text-decoration-line", [](const ComputedStyle& s, bool&) {
            std::string r;
            if (s.underline) r += "underline";
            if (s.overline) r += std::string(r.empty() ? "" : " ") + "overline";
            if (s.line_through) r += std::string(r.empty() ? "" : " ") + "line-through";
            return r.empty() ? std::string("none") : r; });
        add("text-transform", [](const ComputedStyle& s, bool&) {
            static const char* n[] = {"none", "uppercase", "lowercase", "capitalize"};
            return std::string(n[static_cast<int>(s.text_transform)]); });
        add("letter-spacing", [](const ComputedStyle& s, bool&) {
            return s.letter_spacing == 0 ? std::string("normal") : px(s.letter_spacing); });
        add("word-spacing", [](const ComputedStyle& s, bool&) { return px(s.word_spacing); });
        add("white-space", [](const ComputedStyle& s, bool&) {
            static const char* n[] = {"normal", "nowrap", "pre", "pre-wrap", "pre-line", "break-spaces"};
            return std::string(n[static_cast<int>(s.white_space)]); });
        add("display", [](const ComputedStyle& s, bool&) { return std::string(display_name(s.display)); });
        add("position", [](const ComputedStyle& s, bool&) {
            static const char* n[] = {"static", "relative", "absolute", "fixed", "sticky"};
            return std::string(n[static_cast<int>(s.position)]); });
        add("float", [](const ComputedStyle& s, bool&) {
            static const char* n[] = {"none", "left", "right"}; return std::string(n[static_cast<int>(s.float_)]); });
        add("clear", [](const ComputedStyle& s, bool&) {
            static const char* n[] = {"none", "left", "right", "both"}; return std::string(n[static_cast<int>(s.clear)]); });
        add("box-sizing", [](const ComputedStyle& s, bool&) {
            return std::string(s.box_sizing == BoxSizing::Border ? "border-box" : "content-box"); });
        add("overflow-x", [](const ComputedStyle& s, bool&) {
            static const char* n[] = {"visible", "hidden", "scroll", "auto", "clip"};
            return std::string(n[static_cast<int>(s.overflow_x)]); });
        add("overflow-y", [](const ComputedStyle& s, bool&) {
            static const char* n[] = {"visible", "hidden", "scroll", "auto", "clip"};
            return std::string(n[static_cast<int>(s.overflow_y)]); });
        add("visibility", [](const ComputedStyle& s, bool&) {
            static const char* n[] = {"visible", "hidden", "collapse"}; return std::string(n[static_cast<int>(s.visibility)]); });
        add("opacity", [](const ComputedStyle& s, bool&) { return num(s.opacity); });
        add("z-index", [](const ComputedStyle& s, bool&) { return s.z_auto ? std::string("auto") : std::to_string(s.z_index); });
        const char* sides[] = {"top", "right", "bottom", "left"};
        for (int i = 0; i < 4; ++i) {
            const std::string sd = sides[i];
            add("border-" + sd + "-width", [i](const ComputedStyle& s, bool&) { return px(s.border_w[i]); });
            add("border-" + sd + "-style", [i](const ComputedStyle& s, bool&) { return std::string(bstyle(s.border_style[i])); });
            add("border-" + sd + "-color", [i](const ComputedStyle& s, bool&) { return color(s.border_color[i]); });
            add("margin-" + sd, [i](const ComputedStyle& s, bool& inc) {
                inc = !s.margin[i].is_auto && !s.margin[i].has_pct(); return px(s.margin[i].px); });
            add("padding-" + sd, [i](const ComputedStyle& s, bool& inc) {
                inc = !s.padding[i].has_pct(); return px(s.padding[i].px); });
        }
        const char* corners[] = {"top-left", "top-right", "bottom-right", "bottom-left"};
        for (int i = 0; i < 4; ++i)
            add(std::string("border-") + corners[i] + "-radius", [i](const ComputedStyle& s, bool&) { return s.radius_ell && len(s.radiusv[i]) != len(s.radius[i]) ? len(s.radius[i]) + " " + len(s.radiusv[i]) : len(s.radius[i]); });
        add("max-width", [](const ComputedStyle& s, bool&) { return s.max_width.is_auto ? std::string("none") : len(s.max_width); });
        add("flex-grow", [](const ComputedStyle& s, bool&) { return num(s.flex_grow); });
        add("flex-shrink", [](const ComputedStyle& s, bool&) { return num(s.flex_shrink); });
        add("flex-basis", [](const ComputedStyle& s, bool&) { return len(s.flex_basis); });
        add("order", [](const ComputedStyle& s, bool&) { return std::to_string(s.order); });
        add("flex-direction", [](const ComputedStyle& s, bool&) {
            static const char* n[] = {"row", "row-reverse", "column", "column-reverse"};
            return std::string(n[static_cast<int>(s.flex_direction)]); });
        add("flex-wrap", [](const ComputedStyle& s, bool&) {
            static const char* n[] = {"nowrap", "wrap", "wrap-reverse"}; return std::string(n[static_cast<int>(s.flex_wrap)]); });
        add("justify-content", [](const ComputedStyle& s, bool&) { return std::string(align_name(s.justify_content)); });
        add("align-items", [](const ComputedStyle& s, bool&) { return std::string(align_name(s.align_items)); });
        add("align-self", [](const ComputedStyle& s, bool&) { return std::string(align_name(s.align_self)); });
        add("list-style-type", [](const ComputedStyle& s, bool&) {
            static const char* n[] = {"disc", "circle", "square", "decimal", "lower-alpha", "upper-alpha",
                                      "lower-roman", "upper-roman", "none", "decimal-leading-zero"};
            return std::string(n[static_cast<int>(s.list_style)]); });
        return v;
    }();
    return t;
}

void walk(const dom::Node& n, const Resolver& r, const ComputedStyle* parent, std::string& out, bool& first) {
    ComputedStyle cs;
    const ComputedStyle* mine = parent;
    if (n.is_element()) {
        cs = r.compute(n, parent);
        mine = &cs;
        if (n.has_attr("data-t")) {
            out += first ? "" : ",";
            first = false;
            out += "{\"t\":\"" + n.attr_or("data-t") + "\",\"cs\":{";
            bool f2 = true;
            for (const auto& [name, g] : table()) {
                bool inc = true;
                const std::string v = g(cs, inc);
                if (!inc) continue;
                out += (f2 ? "\"" : ",\"") + name + "\":\"" + v + "\"";
                f2 = false;
            }
            out += "}}";
        }
    }
    for (const auto& ch : n.children) walk(*ch, r, mine, out, first);
}

} // namespace

const std::vector<std::string>& probe_properties() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> v;
        for (const auto& [n, g] : table()) v.push_back(n);
        return v;
    }();
    return names;
}

std::string dump_styles_json(const dom::Node& root, const Resolver& r) {
    std::string out = "[";
    bool first = true;
    walk(root, r, nullptr, out, first);
    return out + "]\n";
}

} // namespace luxium::style
