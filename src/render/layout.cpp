#include "render/layout.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cstdlib>

namespace luxium::render {

namespace {

// ── The UA stylesheet: the browser's default look, as CSS ───────────────────
// It goes through the same parser as page CSS; a page rule wins purely by
// specificity, no special cases.  Properties that inherit (color, size,
// line-height, family, align) are deliberately absent from the tags that
// should inherit them.
// Hoja UA = los defaults de Blink (html.css), para que una pagina sin CSS
// se VEA IGUAL que en Chromium.  Desviaciones conscientes: los botones
// mantienen un aspecto propio (en Chromium son widgets nativos) y no
// modelamos font-style: italic (no hay cara oblicua cargada).
constexpr std::string_view kUAStylesheet = R"css(
body       { margin: 8px; font-size: 16px; line-height: normal;
             color: #000000; background-color: #ffffff; }
h1 { font-size: 2em;    font-weight: bold; margin: 0.67em 0; }
h2 { font-size: 1.5em;  font-weight: bold; margin: 0.83em 0; }
h3 { font-size: 1.17em; font-weight: bold; margin: 1em 0; }
h4 { font-size: 1em;    font-weight: bold; margin: 1.33em 0; }
h5 { font-size: 0.83em; font-weight: bold; margin: 1.67em 0; }
h6 { font-size: 0.67em; font-weight: bold; margin: 2.33em 0; }
p          { margin: 1em 0; }
blockquote { margin: 1em 40px; }
ul, ol     { margin: 1em 0; padding-left: 40px; }
li         { margin: 0; }
th         { font-weight: bold; text-align: center; }
pre        { font-family: monospace; font-size: 13px; margin: 1em 0; }
code, kbd, samp, tt { font-family: monospace; font-size: 13px; }
a          { color: #0000EE; text-decoration: underline; }
b, strong  { font-weight: bold; }
u          { text-decoration: underline; }
small      { font-size: 0.83em; }
big        { font-size: 1.17em; }
button     { font-size: 13.33px; color: #000000; }
)css";

constexpr std::string_view kSkipped[] = {
    "head", "title", "script", "style", "meta", "link", "template",
};
constexpr std::string_view kBlocks[] = {
    "body", "html", "h1", "h2", "h3", "p", "ul", "ol", "li", "pre",
    "blockquote", "button", "div", "hr", "br", "section",
};

bool in_list(std::string_view name, const std::string_view* list, size_t n) {
    return std::find(list, list + n, name) != list + n;
}

// ── computed style ──────────────────────────────────────────────────────────

// degradado de background: linear/radial con paradas (t<0 = repartir uniforme)
struct GradDef {
    bool  radial = false;
    float angle = 180.f;    // 0deg = hacia arriba, 90deg = a la derecha (CSS)
    float cx = 0.5f, cy = 0.5f;
    std::vector<std::pair<uint32_t, float>> stops;
};

struct Style {
    // text face (what a word needs)
    float    size  = 16.f;
    bool     bold  = false;
    bool     mono  = false;
    uint32_t color = 0xFF000000;
    float    lh    = 1.17f;  // "normal" de DejaVu, como Blink
    uint32_t pill  = 0;      // inline background (code); painted per word
    bool     underline = false;
    // boxes
    float    mt = 0, mb = 0, ml = 0;
    float    pt = 0, pr = 0, pb = 0, pl = 0;
    uint32_t block_bg = 0;
    bool     has_block_bg = false;
    int      align = 0;      // 0 left, 1 center, 2 right
    bool     display_none = false;
    // width/min/max: contenido (content-box salvo box-sizing:border-box);
    // los % se resuelven en layout contra el containing block
    bool     w_set = false;    float w_val = 0;    bool w_pct = false;
    bool     minw_set = false; float minw_val = 0; bool minw_pct = false;
    bool     maxw_set = false; float maxw_val = 0; bool maxw_pct = false;
    // height fija en px (% con padre de altura auto = auto, como Chromium)
    bool     h_set = false;    float h_val = 0;
    bool     ml_auto = false, mr_auto = false;
    // borde uniforme (subset: solo el estilo solid se pinta)
    float    border_w = 0;
    uint32_t border_color = 0xFF000000;
    bool     border_box = false;
    // background con degradado (linear/radial-gradient con paradas de color)
    GradDef  bg_grad;
    bool     has_grad = false;
    // flexbox nivel 1
    bool     flex = false, dir_column = false, wrap = false;
    int      justify = 0;      // 0 start, 1 center, 2 end, 3 between, 4 around
    int      falign = 0;       // 0 stretch, 1 start, 2 center, 3 end (align-items)
    int      falign_self = -1; // -1 = usar align-items del contenedor
    int      falign_content = 0; // 0 start/stretch, 3 space-between
    int      order = 0;
    float    grow = 0, shrink = 1, gap = 0;
    bool     basis_set = false; float basis_val = 0; bool basis_pct = false;
};

uint32_t parse_color(const std::string& v, uint32_t fallback) {
    std::string s;
    for (char ch : v)
        if (!std::isspace(static_cast<unsigned char>(ch))) s += static_cast<char>(std::tolower(ch));
    if (!s.empty() && s[0] == '#') s.erase(0, 1);
    if (s.size() == 6) {
        unsigned long rgb = 0;
        try { rgb = std::stoul(s, nullptr, 16); } catch (...) { return fallback; }
        return 0xFF000000u | static_cast<uint32_t>(rgb);
    }
    if (s.size() == 3) {
        unsigned long rgb = 0;
        try { rgb = std::stoul(s + s, nullptr, 16); } catch (...) { return fallback; }
        return 0xFF000000u | static_cast<uint32_t>(rgb);
    }
    static const std::array<std::pair<std::string_view, uint32_t>, 12> kNames{{
        {"white", 0xFFFFFFFF}, {"black", 0xFF000000}, {"red", 0xFFCC2E2E},
        {"green", 0xFF2E8B3A}, {"blue", 0xFF2F6FED}, {"gray", 0xFF808690},
        {"grey", 0xFF808690}, {"orange", 0xFFE88A2E}, {"yellow", 0xFFE8C22E},
        {"purple", 0xFF8A4FBF}, {"pink", 0xFFE86AA6}, {"brown", 0xFF8A5A2E},
    }};
    for (const auto& [n, c] : kNames)
        if (s == n) return c;
    return fallback;
}

float parse_len(const std::string& v, float parent_size, float fallback) {
    std::string s;
    for (char ch : v)
        if (!std::isspace(static_cast<unsigned char>(ch))) s += static_cast<char>(std::tolower(ch));
    try {
        if (s.ends_with("px"))  return std::stof(s);
        if (s.ends_with("em"))  return std::stof(s) * parent_size;
        if (s.ends_with("rem")) return std::stof(s) * 16.f;
        if (s.ends_with("%"))   return std::stof(s) / 100.f * parent_size;
        if (!s.empty() && (std::isdigit(s[0]) || s[0] == '.' || s[0] == '-'))
            return std::stof(s);
    } catch (...) {}
    return fallback;
}

// tokeniza "1px auto 2px" en minusculas y sin espacios
std::vector<std::string> css_tokens(const std::string& v) {
    std::vector<std::string> out;
    std::string cur;
    for (char ch : v) {
        if (std::isspace(static_cast<unsigned char>(ch))) {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
        } else cur += static_cast<char>(std::tolower(ch));
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// lo mismo partiendo por un separador dado (p.ej. las comas de un degradado)
std::vector<std::string> css_tokens_sep(const std::string& v, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char ch : v) {
        if (ch == sep) {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
        } else if (!std::isspace(static_cast<unsigned char>(ch))) {
            cur += static_cast<char>(std::tolower(ch));
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// border / border-width: <width> | <style> | <color> en cualquier orden.
// Solo pintamos solid; los demas estilos se aceptan y se dibujan igual.
void parse_border(Style& s, const std::string& v, bool shorthand) {
    for (const auto& t : css_tokens(v)) {
        if (t == "solid" || t == "none" || t == "hidden" || t == "dotted" ||
            t == "dashed" || t == "double" || t == "groove" || t == "ridge" ||
            t == "inset" || t == "outset" || t == "thin" || t == "medium" ||
            t == "thick")
            continue;   // el estilo del borde: lo pintamos como solid
        if (shorthand) {
            if (const uint32_t c = parse_color(t, 0); c) {
                s.border_color = c;
                continue;
            }
        }
        const float w = parse_len(t, s.size, -1);
        if (w > 0) s.border_w = w;
    }
}

// colapso de margenes CSS2.1: max(positivos) + min(negativos)
float collapse(float a, float b) {
    if (a >= 0 && b >= 0) return std::max(a, b);
    if (a <= 0 && b <= 0) return std::min(a, b);
    return std::max(a, b) + std::min(a, b);
}

// linear-gradient(90deg, #a, #b) / radial-gradient(circle, #a, #b) /
// radial-gradient(ellipse at top, ...)  — subset: paradas de color sin
// posiciones explicitas (se reparten uniformes).
GradDef parse_grad(const std::string& v) {
    GradDef g;
    g.radial = v.starts_with("radial-gradient");
    const size_t p0 = v.find('(');
    const size_t p1 = v.rfind(')');
    if (p0 == std::string::npos || p1 == std::string::npos || p1 <= p0) return g;
    const auto toks = css_tokens_sep(v.substr(p0 + 1, p1 - p0 - 1), ',');
    if (toks.empty()) return g;

    size_t first = 0;
    if (!g.radial) {
        const std::string a = toks[0];
        float ang = 180.f;
        if (a.ends_with("deg")) {
            try { ang = std::stof(a); } catch (...) {}
        } else if (a.starts_with("to")) {
            if (a.find("right") != std::string::npos) ang = 90.f;
            else if (a.find("left") != std::string::npos) ang = 270.f;
            else if (a.find("top") != std::string::npos) ang = 0.f;
            else ang = 180.f;   // to bottom
        }
        g.angle = ang;
        first = 1;
    } else {
        for (size_t k = 0; k < toks.size(); ++k) {
            const std::string t = toks[k];
            if (t.find("circle") == 0 || t.find("ellipse") == 0) {
                if (t.find("at top") != std::string::npos) g.cy = 0.f;
                if (t.find("at bottom") != std::string::npos) g.cy = 1.f;
                if (t.find("at left") != std::string::npos) g.cx = 0.f;
                if (t.find("at right") != std::string::npos) g.cx = 1.f;
                first = k + 1;
            } else break;
        }
    }

    for (size_t k = first; k < toks.size(); ++k) {
        const auto parts = css_tokens(toks[k]);
        if (parts.empty()) continue;
        const uint32_t c = parse_color(parts[0], 0);
        if (!c) continue;
        float t = -1.f;
        if (parts.size() > 1 && parts[1].ends_with('%')) {
            try { t = std::stof(parts[1]) / 100.f; } catch (...) { t = -1.f; }
        }
        g.stops.push_back({c, t});
    }
    if (g.stops.size() >= 2) {
        g.stops.front().second = 0.f;
        g.stops.back().second = 1.f;
        const float n = static_cast<float>(g.stops.size() - 1);
        for (size_t k = 1; k + 1 < g.stops.size(); ++k)
            if (g.stops[k].second < 0)
                g.stops[k].second = static_cast<float>(k) / n;
        return g;
    }
    g.stops.clear();
    return g;
}

struct StyleResolver {
    std::vector<css::Rule> ua;
    std::vector<css::Rule> page;
    float viewport = 1180.f;

    explicit StyleResolver(const std::string& page_css, float vw = 1180.f)
        : viewport(vw) {
        ua   = css::parse_stylesheet(std::string(kUAStylesheet));
        page = css::parse_stylesheet(page_css);
    }

    // One declaration onto a style.  `parent` is only for relative units.
    void apply(Style& s, const Style& parent, const std::string& prop,
               const std::string& v) const {
        if (prop == "color")                    s.color = parse_color(v, s.color);
        else if (prop == "background-color" || prop == "background") {
            if (v.starts_with("linear-gradient") || v.starts_with("radial-gradient")) {
                s.bg_grad = parse_grad(v);
                s.has_grad = s.bg_grad.stops.size() >= 2;
            } else {
                // color plano (shorthand o background-color)
                const uint32_t c = parse_color(v, 0);
                if (c) {
                    s.block_bg = c;
                    s.has_block_bg = true;
                    if (!s.mono) s.pill = c;   // inline pills, too (code)
                }
            }
        }
        else if (prop == "width" || prop == "min-width" || prop == "max-width") {
            bool* set; float* val; bool* pct;
            if (prop == "width")        { set = &s.w_set;   val = &s.w_val;   pct = &s.w_pct; }
            else if (prop == "min-width") { set = &s.minw_set; val = &s.minw_val; pct = &s.minw_pct; }
            else                          { set = &s.maxw_set; val = &s.maxw_val; pct = &s.maxw_pct; }
            std::string k;
            for (char ch : v)
                if (!std::isspace(static_cast<unsigned char>(ch)))
                    k += static_cast<char>(std::tolower(ch));
            if (k == "auto") { *set = false; }   // width:auto = llenar
            else if (k.size() && k.back() == '%') {
                k.pop_back();
                *set = true; *pct = true; *val = std::max(0.f, parse_len(k, s.size, 0));
            } else { *set = true; *pct = false; *val = parse_len(k, s.size, 0); }
        }
        else if (prop == "height") {
            if (v.ends_with("px")) {
                s.h_set = true;
                s.h_val = std::max(0.f, parse_len(v, s.size, 0));
            }
            // % y otras unidades: con padre de altura auto equivalen a auto
        }
        else if (prop == "box-sizing")
            s.border_box = v.find("border") != std::string::npos;
        else if (prop == "border" || prop == "border-width")
            parse_border(s, v, prop == "border");
        else if (prop == "border-color") {
            const uint32_t c = parse_color(v, 0);
            if (c) s.border_color = c;
        }
        else if (prop == "font-size")           s.size = std::max(6.f, parse_len(v, parent.size, s.size));
        else if (prop == "font-weight")         s.bold = (v.find("bold") != std::string::npos ||
                                                          v == "700" || v == "800" || v == "900");
        else if (prop == "font-family")         s.mono = v.find("mono") != std::string::npos;
        else if (prop == "line-height") {
            if (v == "normal")                  s.lh = 1.17f;   // metrica DejaVu
            else if (v.find("px") != std::string::npos) s.lh = parse_len(v, s.size, s.lh) / s.size;
            else { try { s.lh = std::stof(v); } catch (...) {} }
        }
        else if (prop == "text-align") {
            if (v == "center") s.align = 1;
            else if (v == "right") s.align = 2;
            else s.align = 0;
        }
        else if (prop == "text-decoration")     s.underline = v.find("underline") != std::string::npos;
        else if (prop == "display") {
            s.display_none = (v == "none");
            s.flex = (v.find("flex") != std::string::npos);
        }
        else if (prop == "flex-direction") s.dir_column = v.find("column") != std::string::npos;
        else if (prop == "flex-wrap")      s.wrap = v.find("wrap") != std::string::npos &&
                                                    v.find("nowrap") == std::string::npos;
        else if (prop == "gap" || prop == "column-gap") s.gap = parse_len(v, s.size, 0);
        else if (prop == "row-gap")         s.gap = parse_len(v, s.size, 0);
        else if (prop == "order")           { try { s.order = std::stoi(v); } catch (...) {} }
        else if (prop == "justify-content") {
            if (v.find("center") != std::string::npos)        s.justify = 1;
            else if (v.find("flex-end") != std::string::npos ||
                     v.find("end") != std::string::npos)      s.justify = 2;
            else if (v.find("space-between") != std::string::npos) s.justify = 3;
            else if (v.find("space-around") != std::string::npos)  s.justify = 4;
            else s.justify = 0;
        }
        else if (prop == "align-items" || prop == "align-self" || prop == "align-content") {
            int val = 0;
            if (v.find("center") != std::string::npos)              val = 2;
            else if (v.find("flex-end") != std::string::npos ||
                     v.find("end") != std::string::npos)            val = 3;
            else if (v.find("flex-start") != std::string::npos ||
                     v.find("start") != std::string::npos)          val = 1;
            else if (v.find("space-between") != std::string::npos)  val = 3;
            if (prop == "align-items")  s.falign = val;
            if (prop == "align-content") s.falign_content = val;
            if (prop == "align-self")   s.falign_self = val;   // "auto" no llega: -1
        }
        else if (prop == "flex-grow")   { try { s.grow = std::stof(v); } catch (...) {} }
        else if (prop == "flex-shrink") { try { s.shrink = std::stof(v); } catch (...) {} }
        else if (prop == "flex-basis") {
            if (v.find("auto") != std::string::npos) { s.basis_set = false; }
            else {
                s.basis_set = true;
                s.basis_pct = v.find('%') != std::string::npos;
                s.basis_val = std::max(0.f, parse_len(v, s.size, 0));
            }
        }
        else if (prop == "flex") {
            // flex: grow | grow shrink | grow shrink basis | auto | none
            s.grow = 1; s.shrink = 1; s.basis_set = false;
            const auto toks = css_tokens(v);
            if (toks.size() == 1 && toks[0] == "none") { s.grow = 0; s.shrink = 0; s.basis_set = true; s.basis_val = 0; }
            else {
                for (size_t k = 0; k < toks.size() && k < 3; ++k) {
                    const std::string& t = toks[k];
                    if (t == "auto") { s.grow = 1; s.shrink = 1; s.basis_set = false; }
                    else if (t.find('%') != std::string::npos || t.ends_with("px")) {
                        s.basis_set = true; s.basis_pct = t.find('%') != std::string::npos;
                        s.basis_val = std::max(0.f, parse_len(t, s.size, 0));
                    } else {
                        try { const float f = std::stof(t);
                              if (k == 0) s.grow = f; else s.shrink = f;
                        } catch (...) {}
                    }
                }
            }
        }
        else if (prop == "margin") {
            // con soporte de auto: los dos laterales auto -> centrar
            const auto toks = css_tokens(v);
            if (toks.empty() || toks.size() > 4) return;
            float vals[4] = {0, 0, 0, 0};
            bool autos[4] = {false, false, false, false};
            for (int i = 0; i < static_cast<int>(toks.size()); ++i) {
                if (toks[i] == "auto") autos[i] = true;
                else vals[i] = parse_len(toks[i], s.size, 0);
            }
            const float t_ = vals[0];
            const float r_ = toks.size() > 1 ? vals[1] : t_;
            const float b_ = toks.size() > 2 ? vals[2] : t_;
            const float l_ = toks.size() > 3 ? vals[3] : r_;
            const bool rA = toks.size() > 1 ? autos[1] : autos[0];
            const bool lA = toks.size() > 3 ? autos[3]
                                            : (toks.size() > 1 ? autos[1] : autos[0]);
            s.mt = t_; s.mb = b_; s.ml = l_;
            s.ml_auto = lA; s.mr_auto = rA;
        }
        else if (prop == "padding") {
            float t, r, b, l;
            if (!sides(v, s.size, t, r, b, l)) return;   // em contra el propio size
            s.pt = t; s.pr = r; s.pb = b; s.pl = l;
        }
        else if (prop == "margin-top")    s.mt = parse_len(v, s.size, s.mt);
        else if (prop == "margin-bottom") s.mb = parse_len(v, s.size, s.mb);
        else if (prop == "margin-left")   s.ml = parse_len(v, s.size, s.ml);
        else if (prop == "margin-right")  { /* vertical layout never uses it */ }
        else if (prop == "padding-top")    s.pt = parse_len(v, s.size, s.pt);
        else if (prop == "padding-bottom") s.pb = parse_len(v, s.size, s.pb);
        else if (prop == "padding-left")   s.pl = parse_len(v, s.size, s.pl);
        else if (prop == "padding-right")  s.pr = parse_len(v, s.size, s.pr);
    }

    // `1px` / `1px 2px` / ... -> t r b l.  `base` resuelve los em (margins y
    // paddings en em van contra el font-size DEL PROPIO elemento, como Blink).
    // `auto` aun no: se queda en 0.
    static bool sides(const std::string& v, float base, float& t, float& r,
                      float& b, float& l) {
        std::vector<float> vals;
        std::string cur;
        for (char ch : v) {
            if (std::isspace(static_cast<unsigned char>(ch))) {
                if (!cur.empty()) { vals.push_back(parse_len(cur, base, 0)); cur.clear(); }
            } else cur += ch;
        }
        if (!cur.empty()) vals.push_back(parse_len(cur, base, 0));
        if (vals.empty() || vals.size() > 4) return false;
        t = vals[0];
        r = vals.size() > 1 ? vals[1] : t;
        b = vals.size() > 2 ? vals[2] : t;
        l = vals.size() > 3 ? vals[3] : r;
        return true;
    }

    // The cascade, in CSS order: UA rules (specificity, order), then page
    // rules, then the style="" attribute.
    Style compute(const dom::Node& el, const Style& parent) const {
        Style s;
        s.size = parent.size; s.bold = parent.bold; s.mono = parent.mono;
        s.color = parent.color; s.lh = parent.lh; s.align = parent.align;

        auto fold = [&](const std::vector<css::Rule>& rules) {
            std::vector<std::pair<int, const css::Rule*>> matched;
            for (const auto& r : rules) {
                if (!r.media.empty() && !css::media_matches(r.media, viewport))
                    continue;   // @media que no aplica a este viewport
                int spec = r.match(el);
                if (spec >= 0) matched.push_back({spec, &r});
            }
            std::stable_sort(matched.begin(), matched.end(),
                             [](auto& a, auto& b) { return a.first < b.first; });
            for (auto& [spec, r] : matched)
                for (const auto& [prop, val] : r->decls) apply(s, parent, prop, val);
        };
        fold(ua);
        fold(page);

        for (const auto& [prop, val] : css::parse_decls(el.attr_or("style")))
            apply(s, parent, prop, val);
        return s;
    }
};

// ── layout context ──────────────────────────────────────────────────────────

struct WordPending {
    float x = 0;            // from the line's left edge (alignment shifts it)
    Word  word;
    uint32_t color = 0;
    uint32_t pill = 0;      // inline background (code)
    float    lh = 0;        // for the pill's height
    bool     underline = false;
};

struct Ctx {
    Font&         font;
    LayoutResult& out;
    const StyleResolver* res = nullptr;

    float y      = 0;
    float pend   = 0;   // margen inferior pendiente de colapsar (CSS collapsing)
    float line_w = 0;
    float line_h = 0;
    bool  line_has_atom = false;   // a button made the line not centerable

    // The line being built belongs to ONE block: its left edge (margins
    // included) and its available width.  flush() needs both -- without
    // line_left, every margin-left in the document silently disappears and
    // list bullets land on top of their first letter.
    float line_left  = 0;
    float line_avail = 0;

    int   align = 0;               // of the block being laid out
    std::vector<WordPending> pending;

    // Runs glued together: `<i>Lux</i>,` is ONE word visually -- there is no
    // space in the source between the i's text and the comma, so no word gap
    // may appear at that boundary.  last_space is the gap the previous word
    // added; glue_next takes it back off the line for the first glued word.
    float last_space  = 0;
    bool  glue_next   = false;

    void flush() {
        const float off = (align == 1 && !line_has_atom) ? std::max(0.f, (line_avail - line_w) / 2.f)
                        : (align == 2 && !line_has_atom) ? std::max(0.f, line_avail - line_w)
                                                         : 0.f;
        if (std::getenv("LUXIUM_DEBUG_WORDS"))
            for (const auto& p : pending)
                std::fprintf(stderr, "word '%s' w=%.1f x=%.1f\n", p.word.text.c_str(),
                             p.word.width, p.x);
        for (auto& p : pending) {
            const float fx = line_left + p.x + off;
            if (p.pill)   // the word's pill shifts WITH the word (and centers with it)
                out.shapes.push_back({ShapeCmd::Kind::RRect, fx - 3, y + 1.5f,
                                      p.word.width + 6, p.lh - 3, 3, p.pill});
            if (p.underline)
                out.shapes.push_back({ShapeCmd::Kind::Rect, fx,
                                      y + p.lh - 2, p.word.width, 1, p.color});
            out.words.push_back({fx, y, p.color, std::move(p.word)});
        }
        pending.clear();
        y += line_h;
        line_w = 0;
        line_h = 0;
        line_has_atom = false;
    }

    void emit(const Word& w, float x_from_left, const Style& s) {
        if (line_w == 0) line_h = s.size * s.lh;
        // The pill, if any, is emitted at flush() with the word's FINAL x
        // (alignment included) -- emitting it here would paint it at the
        // line-relative x, floating in the page margin.
        pending.push_back({x_from_left, w, s.color, s.pill, s.size * s.lh, s.underline});
    }
};

void emit_word(Ctx& c, const Word& w, float left, float avail, const Style& s) {
    const float space = s.size * 0.30f;
    if (c.line_w > 0 && c.line_w + w.width > avail) c.flush();
    const float x = c.line_w == 0 ? 0 : c.line_w;
    c.line_w = x + w.width + space;
    c.line_h = std::max(c.line_h, s.size * s.lh);
    c.last_space = space;
    c.emit(w, x, s);
}

void inline_text(Ctx& c, std::string_view text, const Style& s, float left, float avail) {
    if (text.empty()) return;
    c.font.set_size(s.size);
    // A new run whose first character is not a space GLUES to the previous
    // run's last word: take the pending word gap back off the line.
    const bool first_glues = c.glue_next && c.line_w > 0 && text.front() != ' ';
    if (first_glues) c.line_w = std::max(0.f, c.line_w - c.last_space);
    for (const Word& w : shape_words(c.font, text, s.bold, s.mono))
        emit_word(c, w, left, avail, s);
    c.glue_next = !std::isspace(static_cast<unsigned char>(text.back()));
}

// Inline subtree (b/i/code/a/span): flows with the open line, each element
// taking its own computed style for its own text.
void layout_inline_children(Ctx& c, const dom::Node& el, const Style& el_style,
                            float left, float avail) {
    for (const auto& child : el.children) {
        if (child->type == dom::NodeType::Text) {
            inline_text(c, child->text, el_style, left, avail);
        } else if (child->is_element()) {
            Style cs = c.res->compute(*child, el_style);
            layout_inline_children(c, *child, cs, left, avail);
        }
    }
}

std::string flatten_text(const dom::Node& el) {
    std::string out;
    for (const auto& c : el.children) {
        if (c->type == dom::NodeType::Text) {
            if (!out.empty()) out += ' ';
            out += c->text;
        } else if (c->is_element()) {
            if (!out.empty()) out += ' ';
            out += flatten_text(*c);
        }
    }
    return out;
}

void layout_block(Ctx& c, const dom::Node& el, float left, float avail,
                  const Style& parent);

void layout_children(Ctx& c, const dom::Node& el, float left, float avail,
                     const Style& el_style) {
    for (const auto& child : el.children) {
        if (child->type == dom::NodeType::Text) {
            c.y += c.pend;   // texto real: el margen pendiente se aplica
            c.pend = 0;
            inline_text(c, child->text, el_style, left, avail);
        } else if (child->is_element()) {
            layout_block(c, *child, left, avail, el_style);
        }
    }
}

// ── flexbox (nivel 1): row/column, wrap, justify, align, gap, order ─────────

// pase en seco: cuanto ocupa el subarbol con ese ancho disponible
void measure_subtree(Ctx& c, const dom::Node& el, const Style& s,
                     float avail, float& out_w, float& out_h) {
    LayoutResult dummy;
    Ctx m{c.font, dummy};
    m.res = c.res;
    m.line_left  = s.pl;
    m.line_avail = std::max(1.f, avail - s.pl - s.pr);
    m.y = s.pt;
    layout_children(m, el, s.pl, m.line_avail, s);
    m.flush();
    float right = s.pl;
    for (const auto& sh : dummy.shapes)
        right = std::max(right, sh.x + sh.w);
    for (const auto& wd : dummy.words)
        right = std::max(right, wd.x + wd.word.width);
    out_w = right + s.pr;
    out_h = std::max(s.pt, m.y + std::max(0.f, m.pend)) + s.pb;
}

void layout_block(Ctx& c, const dom::Node& el, float left, float avail,
                  const Style& parent);

// render de UN item flex en (x, y) con contenido w y cross fija h_fix (0=auto)
void layout_flex_item(Ctx& c, const dom::Node& el, const Style& st,
                      float x, float y, float w, float h_fix) {
    const size_t bg_mark = c.out.shapes.size();
    c.y = y + st.pt + st.border_w;
    c.line_left  = x + st.border_w + st.pl;
    c.line_avail = std::max(1.f, w);
    c.align = st.align;
    c.glue_next = false;
    layout_children(c, el, c.line_left, c.line_avail, st);
    c.flush();
    c.align = 0;
    float hh = st.h_set ? st.h_val : (c.y + st.pb + st.border_w) - y;
    if (h_fix > 0) hh = h_fix + st.pt + st.pb + 2 * st.border_w;
    hh = std::max(hh, 1.f);
    const float box_w = w + 2 * st.pl + 2 * st.border_w;
    // fondo/borde del item: encima del fondo del contenedor, bajo los hijos
    if (st.has_grad && st.bg_grad.stops.size() >= 2) {
        ShapeCmd bg;
        bg.kind = ShapeCmd::Kind::Rect;
        bg.x = x;
        bg.y = y;
        bg.w = box_w;
        bg.h = hh;
        bg.grad = true;
        bg.grad_radial = st.bg_grad.radial;
        for (const auto& gs : st.bg_grad.stops) bg.stops.push_back({gs.first, gs.second});
        if (!bg.grad_radial) {
            const float rad = st.bg_grad.angle * 3.14159265f / 180.f;
            bg.gdx = std::sin(rad);
            bg.gdy = -std::cos(rad);
            bg.glen = std::max(1.f, std::fabs(bg.w * bg.gdx) +
                                    std::fabs(bg.h * bg.gdy));
        }
        c.out.shapes.insert(c.out.shapes.begin() + bg_mark, std::move(bg));
    } else if (st.has_block_bg) {
        c.out.shapes.insert(c.out.shapes.begin() + bg_mark,
                            {ShapeCmd::Kind::RRect, x, y, box_w, hh, 4,
                             st.block_bg});
    }
    if (st.border_w > 0) {
        c.out.shapes.push_back({ShapeCmd::Kind::Rect, x, y,
                                box_w, st.border_w, st.border_color});
        c.out.shapes.push_back({ShapeCmd::Kind::Rect, x, y + hh - st.border_w,
                                box_w, st.border_w, st.border_color});
        c.out.shapes.push_back({ShapeCmd::Kind::Rect, x, y,
                                st.border_w, hh, st.border_color});
        c.out.shapes.push_back({ShapeCmd::Kind::Rect, x + box_w - st.border_w, y,
                                st.border_w, hh, st.border_color});
    }
    c.y = y + hh;
}

void layout_flex(Ctx& c, const dom::Node& el, float left, float avail,
                 const Style& parent) {
    const Style s = c.res->compute(el, parent);
    if (s.display_none) return;
    if (in_list(el.name, kSkipped, std::size(kSkipped))) return;

    const bool trapped = s.pt > 0 || s.border_w > 0 || s.h_set;
    if (trapped) { c.y += collapse(c.pend, s.mt); c.pend = 0; }
    else { c.pend = collapse(c.pend, s.mt); }
    const float box_y = trapped ? c.y : c.y + c.pend;

    c.flush();
    c.glue_next = false;

    const float inner_left = left + s.ml + s.border_w + s.pl;
    const float inner_w    = std::max(1.f, avail - s.ml - s.pl - s.pr);
    const float inner_top  = c.y + s.pt;
    const bool  cross_fixed = s.h_set;
    const float cross_h = s.h_set ? std::max(0.f, s.h_val - s.pt - s.pb) : 0.f;
    const size_t bg_mark = c.out.shapes.size();

    struct Item {
        const dom::Node* el = nullptr;
        Style st;
        float basis = 0, grow = 0, shrink = 1, w = 0, h = 0;
        int order = 0;
    };
    std::vector<Item> items;
    for (const auto& ch : el.children) {
        if (!ch->is_element()) continue;
        if (in_list(ch->name, kSkipped, std::size(kSkipped))) continue;
        Item it;
        it.el = ch.get();
        it.st = c.res->compute(*ch, s);
        if (it.st.display_none) continue;
        it.grow = it.st.grow;
        it.shrink = it.st.shrink;
        it.order = it.st.order;
        items.push_back(std::move(it));
    }
    std::stable_sort(items.begin(), items.end(),
                     [](const Item& a, const Item& b) { return a.order < b.order; });
    const int n = static_cast<int>(items.size());

    if (!s.dir_column) {
        for (auto& it : items) {
            if (it.st.basis_set)
                it.basis = it.st.basis_pct ? it.st.basis_val / 100.f * inner_w
                                           : it.st.basis_val;
            else if (it.st.w_set)
                it.basis = it.st.w_pct ? it.st.w_val / 100.f * inner_w : it.st.w_val;
            else {
                float mw = 0, mh = 0;
                measure_subtree(c, *it.el, it.st, inner_w, mw, mh);
                it.basis = std::max(1.f, mw);
            }
            it.basis = std::max(1.f, it.basis);
        }
    } else {
        for (auto& it : items) {
            const int eff = it.st.falign_self >= 0 ? it.st.falign_self : s.falign;
            const bool stretch = eff == 0;
            float witem = inner_w;
            if (!stretch) {
                float mw = 0, mh = 0;
                measure_subtree(c, *it.el, it.st, inner_w, mw, mh);
                witem = std::max(1.f, mw);
            }
            it.w = witem;
            float mw = 0, mh = 0;
            measure_subtree(c, *it.el, it.st, witem, mw, mh);
            if (it.st.h_set)          it.basis = it.st.h_val;
            else if (it.st.basis_set) it.basis = it.st.basis_pct
                                              ? it.st.basis_val / 100.f * std::max(1.f, cross_h)
                                              : it.st.basis_val;
            else                      it.basis = std::max(1.f, mh);
        }
    }

    if (!s.dir_column) {
        // ── lineas (row): una o varias con wrap ──
        std::vector<std::vector<int>> lines(1);
        {
            float used = 0;
            for (int i = 0; i < n; ++i) {
                if (s.wrap && !lines.back().empty() &&
                    used + items[i].basis > inner_w + 0.5f) {
                    lines.push_back({});
                    used = 0;
                }
                lines.back().push_back(i);
                used += items[i].basis + s.gap;
            }
        }

        // 1) anchuras por linea (grow/shrink, min/max) ...
        std::vector<float> line_crosses(lines.size(), 0.f);
        std::vector<float> line_tops(lines.size(), inner_top);
        for (size_t li = 0; li < lines.size(); ++li) {
            const auto& line = lines[li];
            const int cnt = static_cast<int>(line.size());
            float sum_basis = 0, sum_grow = 0, sum_sb = 0;
            for (int i : line) {
                sum_basis += items[i].basis;
                sum_grow += items[i].grow;
                sum_sb += items[i].shrink * items[i].basis;
            }
            float free = inner_w - sum_basis - s.gap * std::max(0, cnt - 1);
            for (int i : line) {
                float wi = items[i].basis;
                if (free > 0 && sum_grow > 0 && items[i].grow > 0)
                    wi = items[i].basis + free * items[i].grow / sum_grow;
                else if (free < 0 && sum_sb > 0 && items[i].shrink > 0)
                    wi = items[i].basis + free * (items[i].shrink * items[i].basis) / sum_sb;
                if (items[i].st.minw_set)
                    wi = std::max(wi, items[i].st.minw_pct
                                       ? items[i].st.minw_val / 100.f * inner_w
                                       : items[i].st.minw_val);
                if (items[i].st.maxw_set)
                    wi = std::min(wi, items[i].st.maxw_pct
                                       ? items[i].st.maxw_val / 100.f * inner_w
                                       : items[i].st.maxw_val);
                items[i].w = std::max(1.f, wi);
            }
        }
        // 2) alturas al ancho final + posiciones de linea (align-content)
        for (size_t li = 0; li < lines.size(); ++li) {
            for (const int i : lines[li]) {
                float mw = 0, mh = 0;
                measure_subtree(c, *items[i].el, items[i].st, items[i].w, mw, mh);
                items[i].h = mh;
                line_crosses[li] = std::max(line_crosses[li], mh);
            }
            if (cross_fixed && lines.size() == 1) line_crosses[li] = cross_h;
        }
        if (cross_fixed && lines.size() > 1) {
            float used_c = s.gap * static_cast<float>(lines.size() - 1);
            for (float lc : line_crosses) used_c += lc;
            const float free_c = std::max(0.f, cross_h - used_c);
            if (s.falign_content == 3) {
                for (size_t li = 1; li < lines.size(); ++li)
                    line_tops[li] = line_tops[li - 1] + line_crosses[li - 1] +
                                    s.gap + free_c / static_cast<float>(lines.size() - 1);
            }
        }

        // 3) justify + render
        for (size_t li = 0; li < lines.size(); ++li) {
            const auto& line = lines[li];
            const int cnt = static_cast<int>(line.size());
            const float line_top = line_tops[li];
            const float line_cross = line_crosses[li];
            float used2 = s.gap * std::max(0, cnt - 1);
            for (int i : line) used2 += items[i].w;
            float free2 = std::max(0.f, inner_w - used2);
            float cur = inner_left;
            if (s.justify == 1) cur += free2 / 2.f;
            else if (s.justify == 2) cur += free2;
            else if (s.justify == 4) cur += free2 / (cnt + 1);
            float between = 0.f;
            if (s.justify == 3 && cnt > 1) between = free2 / (cnt - 1);
            if (s.justify == 4) between = free2 / (cnt + 1) * 2.f;

            for (int k = 0; k < cnt; ++k) {
                const int i = line[k];
                const int eff = items[i].st.falign_self >= 0
                                  ? items[i].st.falign_self : s.falign;
                float y_i = line_top;
                float h_fix = 0;
                if (cross_fixed) {
                    if (eff == 0 && !items[i].st.h_set) h_fix = cross_h;
                    if (eff == 2) y_i = line_top + (cross_h - items[i].h) / 2.f;
                    else if (eff == 3) y_i = line_top + cross_h - items[i].h;
                } else {
                    if (eff == 2) y_i = line_top + (line_cross - items[i].h) / 2.f;
                    else if (eff == 3) y_i = line_top + line_cross - items[i].h;
                }
                layout_flex_item(c, *items[i].el, items[i].st, cur, y_i,
                                 items[i].w, h_fix);
                cur += items[i].w + s.gap + between;
            }
        }
        float rows_sum = s.gap * static_cast<float>(std::max(0, static_cast<int>(lines.size()) - 1));
        for (float lc : line_crosses) rows_sum += lc;
        c.y = inner_top + (cross_fixed ? cross_h : rows_sum);
    } else {
        // ── column: el eje principal es vertical ──
        float total = 0;
        for (const auto& it : items) total += it.basis;
        total += s.gap * std::max(0, n - 1);
        const float target = cross_fixed ? cross_h : total;
        float free = target - total;
        float sum_grow = 0;
        for (const auto& it : items) sum_grow += it.grow;
        if (free > 0 && sum_grow > 0)
            for (auto& it : items)
                if (it.grow > 0) it.basis += free * it.grow / sum_grow;

        float used = 0;
        for (const auto& it : items) used += it.basis;
        used += s.gap * std::max(0, n - 1);
        float free2 = std::max(0.f, target - used);
        float cur = inner_top;
        if (s.justify == 1) cur += free2 / 2.f;
        else if (s.justify == 2) cur += free2;
        float between = 0.f;
        if (s.justify == 3 && n > 1) between = free2 / (n - 1);
        if (s.justify == 4) between = free2 / (n + 1) * 2.f;

        for (int k = 0; k < n; ++k) {
            auto& it = items[k];
            const int eff = it.st.falign_self >= 0 ? it.st.falign_self : s.falign;
            float x_i = inner_left;
            if (eff == 2) x_i = inner_left + (inner_w - it.w) / 2.f;
            else if (eff == 3) x_i = inner_left + inner_w - it.w;
            const bool stretch = eff == 0;
            const bool fixed_h = it.st.h_set || it.st.basis_set ||
                                 (free > 0 && it.grow > 0);
            layout_flex_item(c, *it.el, it.st, x_i, cur,
                             stretch ? inner_w : it.w,
                             fixed_h ? it.basis : 0.f);
            cur += (fixed_h ? it.basis : it.h) + s.gap + between;
        }
        c.y = inner_top + target;
    }

    // ── fondo/borde del contenedor: ANTES de las formas de los items
    const float box_left  = left + s.ml;
    const float box_width = avail - s.ml;
    const float box_h = (c.y + s.pb + s.border_w) - box_y;
    if (s.has_grad && s.bg_grad.stops.size() >= 2) {
        ShapeCmd g;
        g.kind = ShapeCmd::Kind::Rect;
        g.x = box_left; g.y = box_y; g.w = box_width; g.h = box_h;
        g.grad = true;
        g.grad_radial = s.bg_grad.radial;
        for (const auto& gs : s.bg_grad.stops) g.stops.push_back({gs.first, gs.second});
        if (!g.grad_radial) {
            const float rad = s.bg_grad.angle * 3.14159265f / 180.f;
            g.gdx = std::sin(rad);
            g.gdy = -std::cos(rad);
            g.glen = std::max(1.f, std::fabs(box_width * g.gdx) +
                                   std::fabs(box_h * g.gdy));
        }
        c.out.shapes.insert(c.out.shapes.begin() + bg_mark, std::move(g));
    } else if (s.has_block_bg) {
        c.out.shapes.insert(c.out.shapes.begin() + bg_mark,
                            {ShapeCmd::Kind::RRect, box_left, box_y,
                             box_width, box_h, 4, s.block_bg});
    }
    if (s.border_w > 0) {
        c.out.shapes.push_back({ShapeCmd::Kind::Rect, box_left, box_y,
                                box_width, s.border_w, s.border_color});
        c.out.shapes.push_back({ShapeCmd::Kind::Rect, box_left,
                                box_y + box_h - s.border_w, box_width,
                                s.border_w, s.border_color});
        c.out.shapes.push_back({ShapeCmd::Kind::Rect, box_left, box_y,
                                s.border_w, box_h, s.border_color});
        c.out.shapes.push_back({ShapeCmd::Kind::Rect, box_left + box_width - s.border_w,
                                box_y, s.border_w, box_h, s.border_color});
    }

    c.y += s.mb;
    c.y += s.pb + s.border_w;
}

void layout_block(Ctx& c, const dom::Node& el, float left, float avail,
                  const Style& parent) {
    if (el.type != dom::NodeType::Element) return;
    const Style s = c.res->compute(el, parent);
    if (s.display_none) return;   // the subtree goes with it

    const std::string& tag = el.name;
    if (in_list(tag, kSkipped, std::size(kSkipped))) return;
    c.font.set_size(s.size);

    if (s.flex) {   // contenedor flex: motor propio (nivel 1)
        layout_flex(c, el, left, avail, parent);
        return;
    }

    if (tag == "br") {
        c.y += c.pend;
        c.pend = 0;
        c.flush();
        c.y += s.size * 0.35f;
        return;
    }
    if (tag == "hr") {
        c.y += c.pend;
        c.pend = 0;
        c.flush();
        c.y += 10;
        c.out.shapes.push_back({ShapeCmd::Kind::RRect, left, c.y, avail, 1, 0.5f,
                                0xFFE3E6EA});
        c.y += 22;
        return;
    }

    const bool block = in_list(tag, kBlocks, std::size(kBlocks));

    // ── button: an inline-block atom ─────────────────────────────────────
    if (tag == "button") {
        c.y += c.pend;
        c.pend = 0;
        const float padx = 16, pady = 9;
        c.font.set_size(s.size);
        auto words = shape_words(c.font, flatten_text(el), s.bold, s.mono);
        float content_w = 0;
        for (const auto& w : words) content_w += w.width + s.size * 0.30f;
        content_w -= s.size * 0.30f;
        const float bw = content_w + 2 * padx;
        const float bh = s.size * s.lh + 2 * pady;

        if (c.line_w > 0 && c.line_w + bw + 10 > avail) c.flush();
        if (c.line_w == 0) c.line_h = bh;
        const float bx = left + c.line_w;
        const float by = c.y;

        c.out.shapes.push_back({ShapeCmd::Kind::RRect, bx, by, bw, bh, 7,
                                s.has_block_bg ? s.block_bg : 0xFFEFEFEF});
        float wx = bx + padx;
        for (const auto& w : words) {
            c.out.words.push_back({wx, by + pady, s.color, w});
            wx += w.width + s.size * 0.30f;
        }
        c.out.hits.push_back({&el, bx, by, bw, bh, true});

        c.line_w += bw + 10;
        c.line_h = std::max(c.line_h, bh);
        c.line_has_atom = true;
        return;
    }

    if (!block) {   // inline element
        layout_inline_children(c, el, s, left, avail);
        return;
    }

    // ── plain block: width, min/max, margenes auto, padding, borde
    //    uniforme y background (el subset del box model que hoy pintamos) ──
    // TOP: mi margin-top colapsa con lo pendiente; sin padding/border arriba
    // el margen de mi primer hijo ESCAPA a traves de mi (y el mio con el de
    // fuera: el body con margin 8 y un h1 de 21.44 arrancan a 21.44, como
    // Chromium).  Con altura fija nada escapa: la caja contiene.
    const bool trapped = s.pt > 0 || s.border_w > 0 || s.h_set;
    if (trapped) {
        c.y += collapse(c.pend, s.mt);
        c.pend = 0;
    } else {
        c.pend = collapse(c.pend, s.mt);
    }
    const float box_y = trapped ? c.y : c.y + c.pend;

    c.flush();
    c.align = s.align;
    c.glue_next = false;

    const float avail_content = avail - s.ml - s.pl - s.pr;
    float w = avail_content;
    auto resolve_w = [&](bool set, float v, bool pct) {
        if (!set) return;
        w = pct ? v / 100.f * avail : v;   // % del containing block
    };
    resolve_w(s.w_set, s.w_val, s.w_pct);
    if (s.minw_set)
        w = std::max(w, s.minw_pct ? s.minw_val / 100.f * avail : s.minw_val);
    if (s.maxw_set)
        w = std::min(w, s.maxw_pct ? s.maxw_val / 100.f * avail : s.maxw_val);
    if (s.border_box) w -= 2 * s.pl + 2 * s.border_w;
    w = std::max(1.f, w);

    // margenes auto: los dos -> centrar; solo el izquierdo -> absorbe el hueco
    float ml = s.ml;
    if (s.ml_auto || s.mr_auto) {
        const float free = std::max(0.f, avail - (s.pl + w + s.pr));
        ml = (s.ml_auto && s.mr_auto) ? free / 2.f : (s.ml_auto ? free : s.ml);
    }

    const float box_left  = left + ml;
    const float box_width = w + 2 * s.pl + 2 * s.border_w;
    const float inner_left  = box_left + s.border_w + s.pl;
    const float inner_avail = w;
    c.y += s.pt + s.border_w;
    const float y_content_top = c.y;   // para height fija
    // las lineas de este bloque se miden y pintan DENTRO de su caja
    c.line_left  = inner_left;
    c.line_avail = inner_avail;

    const bool li        = tag == "li";
    const bool clickable = el.has_attr("id") || tag == "a";
    // el fondo del bloque entra en el vector ANTES de las formas de los
    // hijos: si no, el bg del body (blanco, se emite al terminar) tapa todo
    const size_t bg_mark = c.out.shapes.size();

    if (li) {
        c.out.shapes.push_back({ShapeCmd::Kind::Circle,
                                inner_left - 13, c.y + s.size * s.lh * 0.5f - 0.5f,
                                7, 7, 3.5f, 0xFF6B7280});
    }

    if (tag == "pre") {
        const float lh  = s.size * s.lh;
        float max_w = 0;
        const std::string text = el.text_content();
        size_t start = 0;
        while (start <= text.size()) {
            size_t nl  = text.find('\n', start);
            size_t end = (nl == std::string::npos) ? text.size() : nl;
            std::string line = text.substr(start, end - start);
            while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                line.pop_back();
            float lw = 0;
            for (const Word& w : shape_words(c.font, line, s.bold, s.mono)) {
                c.out.words.push_back({inner_left + lw, c.y, s.color, w});
                lw += w.width + s.size * 0.30f;
            }
            max_w = std::max(max_w, lw);
            c.y += lh;
            if (nl == std::string::npos) break;
            start = nl + 1;
        }
        if (s.has_block_bg)
            c.out.shapes.insert(c.out.shapes.begin() + bg_mark,
                                {ShapeCmd::Kind::RRect, box_left, box_y,
                                 box_width, c.y - box_y, 6, s.block_bg});
        if (s.pb > 0 || s.border_w > 0) {
            c.y += c.pend;
            c.pend = s.mb;
        } else {
            c.pend = collapse(c.pend, s.mb);
        }
        return;
    }

    if (clickable)
        c.out.hits.push_back({&el, inner_left, c.y, inner_avail, 0, false});

    layout_children(c, el, inner_left, inner_avail, s);
    c.flush();
    c.align = parent.align;

    if (clickable && !c.out.hits.empty() && c.out.hits.back().node == &el)
        c.out.hits.back().h = std::max(c.y - c.out.hits.back().y, s.size * 1.2f);

    if (s.h_set)   // altura fija: el contenido que sobra se pinta igual pero
        c.y = y_content_top + s.h_val;   // la caja no crece
    const float box_h = (c.y + s.pb + s.border_w) - box_y;
    if (s.has_grad && s.bg_grad.stops.size() >= 2) {
        // degradado: Rect relleno por pixel (el recorte de esquinas, luego)
        ShapeCmd g;
        g.kind = ShapeCmd::Kind::Rect;
        g.x = box_left;
        g.y = box_y;
        g.w = box_width;
        g.h = box_h;
        g.grad = true;
        g.grad_radial = s.bg_grad.radial;
        for (const auto& gs : s.bg_grad.stops) g.stops.push_back({gs.first, gs.second});
        if (!g.grad_radial) {
            const float rad = s.bg_grad.angle * 3.14159265f / 180.f;
            g.gdx = std::sin(rad);
            g.gdy = -std::cos(rad);
            g.glen = std::max(1.f, std::fabs(box_width * g.gdx) +
                                   std::fabs(box_h * g.gdy));
        } else {
            g.gcx = box_left + s.bg_grad.cx * box_width;
            g.gcy = box_y + s.bg_grad.cy * box_h;
            g.grx = 1.f;
            g.gry = 1.f;
            const float xs[2] = {box_left, box_left + box_width};
            const float ys[2] = {box_y, box_y + box_h};
            for (const float xx : xs)
                for (const float yy : ys) {
                    g.grx = std::max(g.grx, std::fabs(xx - g.gcx));
                    g.gry = std::max(g.gry, std::fabs(yy - g.gcy));
                }
        }
        c.out.shapes.insert(c.out.shapes.begin() + bg_mark, std::move(g));
    } else if (s.has_block_bg) {   // the box paints before every word
        c.out.shapes.insert(c.out.shapes.begin() + bg_mark,
                            {ShapeCmd::Kind::RRect, box_left, box_y,
                             box_width, box_h, 4, s.block_bg});
    }
    if (s.border_w > 0) {   // los 4 lados, solid
        c.out.shapes.push_back({ShapeCmd::Kind::Rect, box_left, box_y,
                                box_width, s.border_w, s.border_color});
        c.out.shapes.push_back({ShapeCmd::Kind::Rect, box_left,
                                box_y + box_h - s.border_w, box_width,
                                s.border_w, s.border_color});
        c.out.shapes.push_back({ShapeCmd::Kind::Rect, box_left, box_y,
                                s.border_w, box_h, s.border_color});
        c.out.shapes.push_back({ShapeCmd::Kind::Rect,
                                box_left + box_width - s.border_w, box_y,
                                s.border_w, box_h, s.border_color});
    }

    if (s.h_set) {
        c.y = y_content_top + s.h_val + s.pb + s.border_w;
        c.pend = s.mb;   // los hijos quedan contenidos: sin escape
    } else if (s.pb > 0 || s.border_w > 0) {
        c.y += c.pend;      // lo pendiente de los hijos no atraviesa el padding
        c.pend = s.mb;
    } else {
        c.pend = collapse(c.pend, s.mb);   // escapa hacia abajo
    }
}

} // namespace

LayoutResult layout(const dom::Node& root, Font& font, float viewport_w,
                    const std::string& page_css) {
    LayoutResult out;
    Ctx c{font, out};
    StyleResolver res(page_css, viewport_w);
    c.res = &res;

    // Chromium: el body aporta el margen (8px por defecto) y el contenido
    // mide el ancho completo menos esos margenes.  Fuera la columna de
    // lectura disenada: la paridad manda.
    c.line_left  = 0.f;
    c.line_avail = viewport_w;

    Style root_style;   // the inherited-defaults root: browser defaults
    root_style.size = 16; root_style.color = 0xFF000000; root_style.lh = 1.17f;

    layout_children(c, root, 0.f, viewport_w, root_style);
    c.flush();
    c.y += std::max(c.pend, 0.f);

    out.content_height = c.y + 8.f;
    return out;
}

const dom::Node* hit_test(const LayoutResult& l, float x, float y) {
    for (auto it = l.hits.rbegin(); it != l.hits.rend(); ++it) {
        if (x >= it->x && x <= it->x + it->w && y >= it->y && y <= it->y + it->h)
            return it->node;
    }
    return nullptr;
}

} // namespace luxium::render
