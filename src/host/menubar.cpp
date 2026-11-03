#include "host/menubar.hpp"

#include <render/text.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>

namespace luxium::host {

namespace {

// ── minimal JSON tree (the menu spec is nested, unlike the flat protocol objects) ───────────────────────────
struct JV {
    enum T { Null, Bool, Num, Str, List, Dict } t = Null;
    bool b = false;
    std::string s;
    std::vector<JV> l;
    std::map<std::string, JV> d;
};

struct P {
    const std::string& s;
    size_t i = 0;
    int depth = 0;
    void ws() { while (i < s.size() && std::isspace((unsigned char)s[i])) ++i; }
    bool str(std::string& out) {
        if (i >= s.size() || s[i] != '"') return false;
        for (++i; i < s.size() && s[i] != '"'; ++i) {
            if (s[i] != '\\') { out += s[i]; continue; }
            if (++i >= s.size()) return false;
            switch (char e = s[i]) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'u': {
                if (i + 4 >= s.size()) return false;
                unsigned cp = (unsigned)std::strtoul(s.substr(i + 1, 4).c_str(), nullptr, 16);
                i += 4;
                if (cp >= 0xD800 && cp < 0xDC00 && i + 6 < s.size() && s[i + 1] == '\\' && s[i + 2] == 'u') {
                    unsigned lo = (unsigned)std::strtoul(s.substr(i + 3, 4).c_str(), nullptr, 16);
                    i += 6;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                if (cp < 0x80) out += (char)cp;
                else if (cp < 0x800) { out += (char)(0xC0 | cp >> 6); out += (char)(0x80 | (cp & 0x3F)); }
                else if (cp < 0x10000) { out += (char)(0xE0 | cp >> 12); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
                else { out += (char)(0xF0 | cp >> 18); out += (char)(0x80 | ((cp >> 12) & 0x3F)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
                break;
            }
            default: out += e;
            }
        }
        if (i >= s.size()) return false;
        ++i;
        return true;
    }
    bool val(JV& v) {
        ws();
        if (i >= s.size() || ++depth > 32) return false;
        struct G { int& d; ~G() { --d; } } g{depth};
        const char c = s[i];
        if (c == '"') { v.t = JV::Str; return str(v.s); }
        if (c == '[' || c == '{') {
            const bool dict = c == '{';
            v.t = dict ? JV::Dict : JV::List;
            ++i;
            ws();
            if (i < s.size() && s[i] == (dict ? '}' : ']')) { ++i; return true; }
            for (;;) {
                ws();
                std::string k;
                if (dict) {
                    if (!str(k)) return false;
                    ws();
                    if (i >= s.size() || s[i] != ':') return false;
                    ++i;
                }
                JV e;
                if (!val(e)) return false;
                if (dict) v.d[k] = std::move(e); else v.l.push_back(std::move(e));
                ws();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == (dict ? '}' : ']')) { ++i; return true; }
                return false;
            }
        }
        if (s.compare(i, 4, "true") == 0)  { v.t = JV::Bool; v.b = true;  i += 4; return true; }
        if (s.compare(i, 5, "false") == 0) { v.t = JV::Bool; v.b = false; i += 5; return true; }
        if (s.compare(i, 4, "null") == 0)  { i += 4; return true; }
        size_t j = i;   // number: not used by the spec, skipped
        while (j < s.size() && (std::isdigit((unsigned char)s[j]) || std::strchr("+-.eE", s[j]))) ++j;
        if (j == i) return false;
        v.t = JV::Num;
        i = j;
        return true;
    }
};

// GTK accelerator syntax ("<Control><Shift>s", "F5", "<Alt>Left") -> SDL key + modifier mask.
bool parse_accel(const std::string& a, SDL_Keycode& key, unsigned& mods, std::string& text) {
    mods = 0;
    size_t i = 0;
    while (i < a.size() && a[i] == '<') {
        size_t e = a.find('>', i);
        if (e == std::string::npos) return false;
        std::string m = a.substr(i + 1, e - i - 1);
        for (auto& c : m) c = (char)std::tolower((unsigned char)c);
        if (m == "control" || m == "ctrl" || m == "primary") mods |= KMOD_CTRL;
        else if (m == "shift") mods |= KMOD_SHIFT;
        else if (m == "alt" || m == "mod1") mods |= KMOD_ALT;
        else if (m == "super" || m == "meta" || m == "mod4") mods |= KMOD_GUI;
        else return false;
        i = e + 1;
    }
    std::string name = a.substr(i);
    if (name.empty()) return false;
    static const std::map<std::string, const char*> alias = {
        {"Page_Up", "PageUp"}, {"Page_Down", "PageDown"}, {"BackSpace", "Backspace"}, {"space", "Space"},
        {"plus", "+"}, {"minus", "-"}, {"comma", ","}, {"period", "."}, {"slash", "/"}, {"equal", "="},
        {"Escape", "Escape"}, {"Return", "Return"}, {"KP_Enter", "Return"}};
    if (name.size() == 1) key = (SDL_Keycode)std::tolower((unsigned char)name[0]);
    else {
        auto it = alias.find(name);
        key = SDL_GetKeyFromName(it != alias.end() ? it->second : name.c_str());
    }
    if (key == SDLK_UNKNOWN) return false;
    text.clear();
    if (mods & KMOD_CTRL) text += "Ctrl+";
    if (mods & KMOD_ALT) text += "Alt+";
    if (mods & KMOD_SHIFT) text += "Shift+";
    if (mods & KMOD_GUI) text += "Super+";
    std::string kn = SDL_GetKeyName(key);
    if (kn.size() == 1) kn[0] = (char)std::toupper((unsigned char)kn[0]);
    text += kn;
    return true;
}

bool build(const JV& list, std::vector<MenuItem>& out, bool top) {
    if (list.t != JV::List) return false;
    for (const JV& e : list.l) {
        MenuItem it;
        if (e.t == JV::Str) {
            if (e.s == "-" && !top) { it.sep = true; out.push_back(std::move(it)); }
            continue;
        }
        if (e.t != JV::Dict) continue;
        auto get = [&](const char* k) -> const JV* { auto f = e.d.find(k); return f == e.d.end() ? nullptr : &f->second; };
        if (const JV* v = get("label"); v && v->t == JV::Str) it.label = v->s;
        if (const JV* v = get("items"); v && v->t == JV::List) build(*v, it.items, false);
        const bool has_items = get("items") && get("items")->t == JV::List;
        if (!has_items) {   // a leaf: action, accelerator and check state (a submenu's own are ignored, as in GTK)
            if (const JV* v = get("action"); v && v->t == JV::Str) it.action = v->s;
            if (const JV* v = get("checked"); v && v->t == JV::Bool) { it.checkable = true; it.checked = v->b; }
            if (const JV* v = get("accel"); v && v->t == JV::Str && !v->s.empty())
                if (!parse_accel(v->s, it.key, it.mods, it.accel_text)) { it.key = 0; it.mods = 0; it.accel_text.clear(); }
        }
        out.push_back(std::move(it));
    }
    return true;
}

constexpr int kRow = 24, kSepH = 9, kPadX = 12, kFont = 13;

MenuItem* find_accel(std::vector<MenuItem>& v, const SDL_Keysym& k) {
    const unsigned m = k.mod & (KMOD_CTRL | KMOD_SHIFT | KMOD_ALT | KMOD_GUI);
    // KMOD_CTRL etc. are L|R pairs: "either side" matches either bit.
    auto norm = [](unsigned x) { return (x & KMOD_CTRL ? KMOD_CTRL : 0) | (x & KMOD_SHIFT ? KMOD_SHIFT : 0) |
                                        (x & KMOD_ALT ? KMOD_ALT : 0) | (x & KMOD_GUI ? KMOD_GUI : 0); };
    for (auto& it : v) {
        if (it.sub()) { if (MenuItem* r = find_accel(it.items, k)) return r; continue; }
        if (it.key && it.key == k.sym && norm(it.mods) == norm(m)) return &it;
    }
    return nullptr;
}

} // namespace

bool MenuBar::set_spec(const std::string& json) {
    JV root;
    P p{json};
    if (!p.val(root) || root.t != JV::List) return false;
    std::vector<MenuItem> next;
    build(root, next, true);
    top_ = std::move(next);
    path_.clear();
    hover_top_ = -1;
    dirty_ = true;
    return true;
}

float MenuBar::text_w(const std::string& s) const {
    if (!font_ || s.empty()) return 0;
    auto* f = const_cast<render::Font*>(font_);
    f->set_size(kFont);
    return f->shape(s, false, false).width;
}

void MenuBar::layout_top() {
    topx_.assign(top_.size(), 0);
    topw_.assign(top_.size(), 0);
    int x = 4;
    for (size_t i = 0; i < top_.size(); ++i) {
        topx_[i] = x;
        topw_[i] = (int)text_w(top_[i].label) + 2 * kPadX;
        x += topw_[i];
    }
}

std::vector<MenuBar::Panel> MenuBar::panels(int win_w) const {
    std::vector<Panel> out;
    if (path_.empty() || path_[0] < 0 || path_[0] >= (int)top_.size()) return out;
    const std::vector<MenuItem>* items = &top_[path_[0]].items;
    int x = path_[0] < (int)topx_.size() ? topx_[path_[0]] : 0, y = kBarH;
    for (size_t d = 0; items && !items->empty(); ++d) {
        Panel pn{items, x, y, 0, 0, {}};
        float wmax = 0;
        int yy = 0;
        for (const auto& it : *items) {
            pn.rowy.push_back(yy);
            yy += it.sep ? kSepH : kRow;
            if (it.sep) continue;
            wmax = std::max(wmax, text_w(it.label) + (it.accel_text.empty() ? 0 : text_w(it.accel_text) + 28) + (it.sub() ? 18 : 0));
        }
        pn.h = yy + 6;
        pn.w = (int)wmax + 2 * kPadX + 22;   // 22: check column
        if (pn.x + pn.w > win_w) pn.x = std::max(0, win_w - pn.w);
        out.push_back(pn);
        const MenuItem* next = nullptr;
        if (d + 1 < path_.size() && path_[d + 1] >= 0 && path_[d + 1] < (int)items->size() && (*items)[path_[d + 1]].sub()) {
            next = &(*items)[path_[d + 1]];
            x = pn.x + pn.w - 2;
            y = pn.y + 3 + pn.rowy[path_[d + 1]];
        }
        items = next ? &next->items : nullptr;
    }
    return out;
}

MenuItem* MenuBar::item_at(const std::vector<int>& p, size_t depth) {
    // panel `depth` shows the children of: top_[p[0]] followed by the items p[1..depth]
    if (p.empty() || p[0] < 0 || p[0] >= (int)top_.size()) return nullptr;
    MenuItem* cur = &top_[p[0]];
    for (size_t k = 1; k <= depth; ++k) {
        if (k >= p.size() || p[k] < 0 || p[k] >= (int)cur->items.size()) return nullptr;
        cur = &cur->items[p[k]];
    }
    return cur;
}

void MenuBar::activate(MenuItem& it, MenuFire& f) {
    if (it.checkable) it.checked = !it.checked;
    f.fired = !it.action.empty();
    f.action = it.action;
    f.checkable = it.checkable;
    f.checked = it.checked;
    dirty_ = true;
}

bool MenuBar::on_motion(int x, int y, int win_w) {
    if (top_.empty()) return false;
    layout_top();
    int ht = -1;
    if (y >= 0 && y < kBarH)
        for (size_t i = 0; i < top_.size(); ++i)
            if (x >= topx_[i] && x < topx_[i] + topw_[i]) ht = (int)i;
    if (ht != hover_top_) { hover_top_ = ht; dirty_ = true; }
    if (!open()) return y >= 0 && y < kBarH;
    if (ht >= 0 && ht != path_[0]) { path_ = {ht}; dirty_ = true; return true; }
    const auto ps = panels(win_w);
    for (size_t d = ps.size(); d-- > 0;) {
        const Panel& pn = ps[d];
        if (x < pn.x || x >= pn.x + pn.w || y < pn.y || y >= pn.y + pn.h) continue;
        for (size_t r = 0; r < pn.items->size(); ++r) {
            const int ry = pn.y + 3 + pn.rowy[r], rh = (*pn.items)[r].sep ? kSepH : kRow;
            if (y < ry || y >= ry + rh || (*pn.items)[r].sep) continue;
            if (path_.size() != d + 2 || path_[d + 1] != (int)r) { path_.resize(d + 1); path_.push_back((int)r); dirty_ = true; }
            break;
        }
        return true;
    }
    return true;   // open menu grabs the pointer
}

bool MenuBar::on_button(int x, int y, bool down, int win_w, MenuFire& f) {
    if (top_.empty()) return false;
    if (!down) { bool s = swallow_up_; swallow_up_ = false; return s; }
    layout_top();
    if (y >= 0 && y < kBarH) {
        for (size_t i = 0; i < top_.size(); ++i)
            if (x >= topx_[i] && x < topx_[i] + topw_[i]) {
                if (open() && path_[0] == (int)i) close(); else { path_ = {(int)i}; dirty_ = true; }
                swallow_up_ = true;
                return true;
            }
        if (!open()) return true;   // empty part of the bar: nothing under it but the bar
    }
    if (!open()) return false;
    swallow_up_ = true;
    const auto ps = panels(win_w);
    for (size_t d = ps.size(); d-- > 0;) {
        const Panel& pn = ps[d];
        if (x < pn.x || x >= pn.x + pn.w || y < pn.y || y >= pn.y + pn.h) continue;
        for (size_t r = 0; r < pn.items->size(); ++r) {
            const int ry = pn.y + 3 + pn.rowy[r];
            if (y < ry || y >= ry + ((*pn.items)[r].sep ? kSepH : kRow) || (*pn.items)[r].sep) continue;
            path_.resize(d + 1);
            path_.push_back((int)r);
            MenuItem* it = item_at(path_, d + 1);
            if (it && !it->sub()) { activate(*it, f); close(); }
            else dirty_ = true;
            return true;
        }
        return true;
    }
    close();   // click outside: close and swallow it
    return true;
}

// Moves the highlight in the deepest panel by `dir`, skipping separators.
bool MenuBar::step(int dir) {
    size_t depth = 0;
    while (item_at(path_, depth + 1) && item_at(path_, depth + 1)->sub()) ++depth;
    if (path_.size() == depth + 1) path_.push_back(dir > 0 ? -1 : 0);   // nothing highlighted yet
    const auto& items = item_at(path_, depth)->items;
    if (items.empty()) return true;
    int r = path_.back();
    for (size_t n = 0; n < items.size(); ++n) {
        r = (r + dir + (int)items.size()) % (int)items.size();
        if (!items[r].sep) break;
    }
    path_.back() = r;
    dirty_ = true;
    return true;
}

bool MenuBar::on_key(const SDL_Keysym& k, bool repeat, MenuFire& f) {
    if (top_.empty()) return false;
    if (open()) {
        switch (k.sym) {
        case SDLK_ESCAPE:
            if (path_.size() > 1) path_.pop_back(); else close();
            dirty_ = true;
            return true;
        case SDLK_DOWN: return step(1);
        case SDLK_UP:   return step(-1);
        case SDLK_LEFT:
            if (path_.size() > 2) { path_.pop_back(); dirty_ = true; }
            else { path_ = {(path_[0] + (int)top_.size() - 1) % (int)top_.size()}; dirty_ = true; }
            return true;
        case SDLK_RIGHT: {
            MenuItem* it = path_.size() > 1 ? item_at(path_, path_.size() - 1) : nullptr;
            if (it && it->sub()) { path_.push_back(0); dirty_ = true; }
            else if (path_.size() <= 2) { path_ = {(path_[0] + 1) % (int)top_.size()}; dirty_ = true; }
            return true;
        }
        case SDLK_RETURN: case SDLK_KP_ENTER: {
            MenuItem* it = path_.size() > 1 ? item_at(path_, path_.size() - 1) : nullptr;
            if (it && it->sub()) { path_.push_back(0); dirty_ = true; }
            else if (it) { activate(*it, f); close(); }
            return true;
        }
        default: break;
        }
    }
    if (repeat) return false;
    if (MenuItem* it = find_accel(top_, k)) { activate(*it, f); close(); return true; }
    if (k.sym == SDLK_F10 && !(k.mod & (KMOD_CTRL | KMOD_ALT | KMOD_SHIFT))) {   // F10 opens the first menu, as in GTK
        if (open()) close(); else { path_ = {0}; dirty_ = true; }
        return true;
    }
    return open();
}

void MenuBar::draw(uint32_t* fb, int W, int H) {
    if (top_.empty() || !font_ || W <= 0 || H <= 0) return;
    layout_top();
    auto* font = const_cast<render::Font*>(font_);
    const uint32_t bg = dark_ ? 0xFF2B2B31 : 0xFFEFEFF1, fg = dark_ ? 0xFFE6E6EA : 0xFF1F1F23,
                   dim = dark_ ? 0xFF8E8E98 : 0xFF6A6A72, hi = dark_ ? 0xFF45454F : 0xFFD3D8E4,
                   line = dark_ ? 0xFF3C3C44 : 0xFFC4C4CA, pbg = dark_ ? 0xFF303037 : 0xFFFAFAFB;
    auto rect = [&](int x, int y, int w, int h, uint32_t c) {
        for (int yy = std::max(0, y); yy < std::min(H, y + h); ++yy)
            for (int xx = std::max(0, x); xx < std::min(W, x + w); ++xx) fb[(size_t)yy * W + xx] = c;
    };
    auto text = [&](const std::string& s, float x, int ytop, int rowh, uint32_t c) {
        if (s.empty()) return;
        font->set_size(kFont);
        render::Word w;
        w.text = s;
        w.shaped = font->shape(s, false, false);
        w.width = w.shaped.width;
        w.size = kFont;
        w.ascent = font->ascent();
        render::draw_word(std::span<uint32_t>(fb, (size_t)W * H), W, H, *font, w, x, ytop + (rowh - font->line_height()) / 2.f, c);
    };
    rect(0, 0, W, kBarH, bg);
    rect(0, kBarH - 1, W, 1, line);
    for (size_t i = 0; i < top_.size(); ++i) {
        const bool on = (open() && path_[0] == (int)i) || (!open() && hover_top_ == (int)i);
        if (on) rect(topx_[i], 2, topw_[i], kBarH - 4, hi);
        text(top_[i].label, (float)(topx_[i] + kPadX), 0, kBarH - 1, fg);
    }
    const auto ps = panels(W);
    for (size_t d = 0; d < ps.size(); ++d) {
        const Panel& pn = ps[d];
        rect(pn.x - 1, pn.y - 1, pn.w + 2, pn.h + 2, line);
        rect(pn.x, pn.y, pn.w, pn.h, pbg);
        const int sel = d + 1 < path_.size() ? path_[d + 1] : -1;
        for (size_t r = 0; r < pn.items->size(); ++r) {
            const MenuItem& it = (*pn.items)[r];
            const int ry = pn.y + 3 + pn.rowy[r];
            if (it.sep) { rect(pn.x + 6, ry + kSepH / 2, pn.w - 12, 1, line); continue; }
            if ((int)r == sel) rect(pn.x + 2, ry, pn.w - 4, kRow, hi);
            if (it.checkable && it.checked) text("\xE2\x9C\x93", (float)(pn.x + 6), ry, kRow, fg);
            text(it.label, (float)(pn.x + 22 + kPadX / 2), ry, kRow, fg);
            if (it.sub()) text("\xE2\x96\xB8", (float)(pn.x + pn.w - 18), ry, kRow, fg);
            else if (!it.accel_text.empty()) text(it.accel_text, (float)pn.x + pn.w - kPadX - text_w(it.accel_text), ry, kRow, dim);
        }
    }
}

} // namespace luxium::host
