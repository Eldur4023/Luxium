// DOM + styles -> box tree: display:none/contents, generated content,
// replaced elements, list markers, and the anonymous-box fix-ups CSS requires
// (inline runs inside blocks, flex/grid items, table structure).

#include <layout/box.hpp>
#include <style/values.hpp>
#include <render/svg.hpp>
#include <media/own_media.hpp>

#include <algorithm>
#include <cmath>
#include <cctype>

namespace luxium::layout {

using style::Float;
using style::Position;

bool Box::is_atomic_inline() const {
    if (!st) return false;
    if (kind == Kind::Block)
        return st->display == Display::InlineBlock || st->display == Display::InlineFlex ||
               st->display == Display::InlineGrid || st->display == Display::InlineTable;
    if (kind == Kind::Replaced)
        return st->display == Display::Inline || st->display == Display::InlineBlock;
    return false;
}

bool Box::inline_level() const {
    switch (kind) {
    case Kind::Text: case Kind::Inline: case Kind::Break: case Kind::Marker: return true;
    default: return is_atomic_inline();
    }
}

ComputedStyle inherited_style(const ComputedStyle& parent, Display d) {
    ComputedStyle s = style::inherited_style(parent);
    s.display = d;
    return s;
}

namespace {

bool is_table_internal(Display d) {
    switch (d) {
    case Display::TableRow: case Display::TableCell: case Display::TableRowGroup:
    case Display::TableHeaderGroup: case Display::TableFooterGroup: case Display::TableColumn:
    case Display::TableColumnGroup: case Display::TableCaption: return true;
    default: return false;
    }
}
bool is_row_group(Display d) {
    return d == Display::TableRowGroup || d == Display::TableHeaderGroup || d == Display::TableFooterGroup;
}

bool blank(const std::string& t) {
    return t.find_first_not_of(" \t\r\n\f") == std::string::npos;
}

struct Builder {
    const style::Resolver& res;
    Tree& tree;
    std::string base_dir;

    const ComputedStyle* keep(ComputedStyle s) {
        tree.styles.push_back(std::move(s));
        return &tree.styles.back();
    }

    std::unique_ptr<Box> make(Box::Kind k, const dom::Node* n, const ComputedStyle* st, bool anon = false) {
        auto b = std::make_unique<Box>();
        b->kind = k; b->node = n; b->st = st; b->anonymous = anon;
        return b;
    }

    std::unique_ptr<Box> anon_block(const ComputedStyle& parent, Display d = Display::Block) {
        return make(Box::Kind::Block, nullptr, keep(inherited_style(parent, d)), true);
    }

    void adopt(Box& parent, std::unique_ptr<Box> child) {
        child->parent = &parent;
        parent.kids.push_back(std::move(child));
    }

    // ── text and elements ──────────────────────────────────────────────────
    // ── CSS counters (CSS Lists 3 "inherit counters"): every element has a set of counters, built from its
    // parent's set plus its preceding sibling's counters whose names the parent's set lacks; values are shared
    // by identity, so an increment is seen by every set that holds the same counter.
    struct Counter { std::string name; int value; const dom::Node* scope; };   // scope: parent of the creating element
    using CSet = std::vector<std::shared_ptr<Counter>>;
    CSet  cs_root;
    CSet* cs_parent = &cs_root;   // the set of the element whose children are being built
    CSet  cs_prev;                // the set of the preceding sibling (a ::before counts as the first child)

    static Counter* last_named(CSet& set, const std::string& n) {
        for (auto it = set.rbegin(); it != set.rend(); ++it) if ((*it)->name == n) return it->get();
        return nullptr;
    }
    // The set of an element (or pseudo, scope = its originating element) with its own reset/increment/set applied.
    CSet counters_begin(const ComputedStyle& st, const dom::Node* scope) {
        CSet mine = *cs_parent;
        for (const auto& c : cs_prev)
            if (!last_named(mine, c->name)) mine.push_back(c);
        auto instantiate = [&](const std::string& n, int v) {
            for (auto it = mine.rbegin(); it != mine.rend(); ++it)
                if ((*it)->name == n) { if ((*it)->scope == scope) mine.erase(std::next(it).base()); break; }   // own or a sibling's: replaced
            mine.push_back(std::make_shared<Counter>(Counter{n, v, scope}));
        };
        for (const auto& [n, v] : st.counter_reset) instantiate(n, v);
        for (const auto& [n, v] : st.counter_inc) {
            if (!last_named(mine, n)) instantiate(n, 0);
            last_named(mine, n)->value += v;
        }
        for (const auto& [n, v] : st.counter_set) {
            if (!last_named(mine, n)) instantiate(n, 0);
            last_named(mine, n)->value = v;
        }
        return mine;
    }

    std::string counters_text(const ComputedStyle& st, CSet& mine, const dom::Node* scope) {
        std::string out;
        for (const auto& it : st.content_items) {
            if (it.kind == style::ContentItem::Kind::Str) { out += it.str; continue; }
            std::vector<int> vals;
            for (const auto& c : mine) if (c->name == it.name) vals.push_back(c->value);
            if (vals.empty()) { mine.push_back(std::make_shared<Counter>(Counter{it.name, 0, scope})); vals.push_back(0); }   // counter() of nothing instantiates it
            if (it.kind == style::ContentItem::Kind::Counter) out += style::counter_text(vals.back(), it.style);
            else for (size_t i = 0; i < vals.size(); ++i) { if (i) out += it.sep; out += style::counter_text(vals[i], it.style); }
        }
        return out;
    }

    void build_children(const dom::Node& el, const ComputedStyle& st, Box& into, int* list_counter, int list_step) {
        const bool closed_details = el.name == "details" && !el.has_attr("open");
        bool summary_done = false;
        for (const auto& ch : el.children) {
            if (closed_details) {   // only the first <summary> of a closed <details> renders
                if (!ch->is_element() || ch->name != "summary" || summary_done) continue;
                summary_done = true;
            }
            if (ch->type == dom::NodeType::Text) {
                if (ch->text.empty()) continue;
                auto t = make(Box::Kind::Text, ch.get(), &st);
                t->text = ch->text;
                adopt(into, std::move(t));
            } else if (ch->is_element()) {
                build_element(*ch, st, into, list_counter, list_step);
            }
        }
    }

    // generated content for ::before / ::after
    void build_pseudo(const dom::Node& el, const ComputedStyle& st, Box& into, style::Pseudo which) {
        ComputedStyle ps = res.compute(el, &st, which);
        if (ps.display == Display::None) return;
        const ComputedStyle* pst = keep(std::move(ps));
        CSet mine = counters_begin(*pst, &el);   // the pseudo is a child of its element
        std::string text = pst->content_items.empty() ? pst->content : counters_text(*pst, mine, &el);
        cs_prev = std::move(mine);
        const bool inl = pst->display == Display::Inline;
        auto b = make(inl ? Box::Kind::Inline : Box::Kind::Block, &el, pst, true);
        if (!text.empty()) {
            auto t = make(Box::Kind::Text, nullptr, pst, true);
            t->text = std::move(text);
            adopt(*b, std::move(t));
        }
        adopt(into, std::move(b));
    }

    static std::string trim_ws(const std::string& s) {
        const size_t a = s.find_first_not_of(" \t\r\n"), z = s.find_last_not_of(" \t\r\n");
        return a == std::string::npos ? std::string() : s.substr(a, z - a + 1);
    }

    // `sizes` -> the slot's width in CSS px: the first entry whose media condition holds (or the last, bare one).
    float slot_width(const std::string& sizes) const {
        const float vw = res.viewport_w();
        size_t from = 0;
        while (from <= sizes.size()) {
            size_t comma = sizes.find(',', from);   // (a lazy split: a comma inside a media condition is rare)
            if (comma == std::string::npos) comma = sizes.size();
            std::string ent = trim_ws(sizes.substr(from, comma - from));
            from = comma + 1;
            if (ent.empty()) continue;
            std::string cond;
            if (ent[0] == '(') {   // (media-condition) <length>
                int depth = 0; size_t i = 0;
                for (; i < ent.size(); ++i) { if (ent[i] == '(') ++depth; else if (ent[i] == ')' && --depth == 0) break; }
                cond = ent.substr(0, i + 1);
                ent = trim_ws(ent.substr(std::min(i + 1, ent.size())));
                if (!style::media_matches(cond, vw, res.viewport_h())) continue;
            }
            char* end = nullptr;
            const float n = std::strtof(ent.c_str(), &end);
            const std::string unit = end ? std::string(end) : "";
            if (unit == "vw") return n * vw / 100.f;
            if (unit == "px" || unit.empty()) return n;
            if (unit == "em" || unit == "rem") return n * 16.f;
            return vw;   // calc() and friends: the viewport
        }
        return vw;
    }

    // srcset="a.jpg 1x, b.jpg 2x" / "a.jpg 480w, b.jpg 800w" (+ sizes): the file to fetch for this display.
    std::string pick_srcset(const std::string& srcset, const std::string& sizes) const {
        struct Cand { std::string url; float density; };
        std::vector<Cand> cands;
        const float slot = sizes.empty() ? res.viewport_w() : slot_width(sizes);
        size_t from = 0;
        while (from < srcset.size()) {
            size_t comma = srcset.find(',', from);
            // a data: URL holds a comma of its own: a candidate ends at a comma followed by whitespace
            while (comma != std::string::npos && comma + 1 < srcset.size() && !std::isspace(static_cast<unsigned char>(srcset[comma + 1])) &&
                   srcset.compare(from, 5, "data:") == 0)
                comma = srcset.find(',', comma + 1);
            if (comma == std::string::npos) comma = srcset.size();
            const std::string ent = trim_ws(srcset.substr(from, comma - from));
            from = comma + 1;
            if (ent.empty()) continue;
            const size_t sp = ent.find_last_of(" \t");
            std::string url = ent, desc;
            if (sp != std::string::npos && (ent.back() == 'x' || ent.back() == 'w')) { url = trim_ws(ent.substr(0, sp)); desc = ent.substr(sp + 1); }
            float d = 1.f;
            if (!desc.empty()) {
                const float n = std::strtof(desc.c_str(), nullptr);
                if (n > 0) d = desc.back() == 'w' ? n / std::max(1.f, slot) : n;
            }
            cands.push_back({url, d});
        }
        if (cands.empty()) return {};
        const float want = render::image_pixel_ratio();
        const Cand* best = nullptr;
        for (const auto& c : cands)   // the smallest file that is sharp enough, else the sharpest there is
            if (c.density >= want && (!best || c.density < best->density)) best = &c;
        if (!best) for (const auto& c : cands) if (!best || c.density > best->density) best = &c;
        return best->url;
    }

    static bool image_type_known(std::string t) {
        for (char& c : t) c = char(std::tolower(static_cast<unsigned char>(c)));
        if (t.empty()) return true;
        for (const char* k : {"image/png", "image/jpeg", "image/jpg", "image/webp", "image/gif", "image/svg+xml", "image/bmp", "image/x-icon", "image/vnd.microsoft.icon", "image/avif"})
            if (t == k) return true;
        return false;
    }

    // The URL an <img> draws: the first <picture><source> that matches (media, type), else srcset, else src.
    std::string image_source(const dom::Node& el) const {
        const dom::Node* pic = el.parent && el.parent->is_element("picture") ? el.parent : nullptr;
        if (pic) {
            for (const auto& c : pic->children) {
                if (c.get() == &el) break;
                if (!c->is_element("source")) continue;
                const std::string media = c->attr_or("media");
                if (!media.empty() && !style::media_matches(media, res.viewport_w(), res.viewport_h())) continue;
                if (!image_type_known(c->attr_or("type"))) continue;
                const std::string url = pick_srcset(c->attr_or("srcset"), c->attr_or("sizes"));
                if (!url.empty()) return url;
            }
        }
        if (el.has_attr("srcset")) {
            const std::string url = pick_srcset(el.attr_or("srcset"), el.attr_or("sizes"));
            if (!url.empty()) return url;
        }
        return el.attr_or("src");
    }

    void load_replaced(Box& b, const dom::Node& el) {
        const std::string src = el.name == "img" ? image_source(el) : el.attr_or("src");
        if (src.empty()) {
            b.broken_img = true;
            return;
        }
        const render::ImageBitmap* bm = render::load_image(render::resolve_image(base_dir, src));
        if (!bm) { b.broken_img = true; return; }
        // cached forever by load_image: a non-owning shared_ptr is enough
        b.img = std::shared_ptr<const render::ImageBitmap>(bm, [](const render::ImageBitmap*) {});
        b.nat_w = static_cast<float>(bm->w);
        b.nat_h = static_cast<float>(bm->h);
    }

    // <video>/<audio>: a replaced box whose picture the shell patches in.  Size:
    // the video's own once the player knows it, else the poster's, else 300x150
    // (audio: 300x54 controls bar, and nothing without `controls`).
    void load_media(Box& b, const dom::Node& el) {
        const bool audio = el.name == "audio";
        b.media = true;
        b.media_controls = el.has_attr("controls");
        media::own::Elem e;
        std::string src = el.attr_or("src");
        if (src.empty())
            for (const auto& c : el.children)
                if (c->is_element() && c->name == "source" && !c->attr_or("src").empty()) { src = c->attr_or("src"); break; }
        if (src.starts_with("file://")) src = src.substr(7);
        // Same resolution as images: "/api/x" and "//host/x" on an http page are relative to the page's origin.
        if (!src.empty()) src = render::resolve_image(base_dir, src);
        if (src.empty()) b.media_controls = false;   // Chromium: no source, no controls (a blank black box)
        e.local = !(src.starts_with("http://") || src.starts_with("https://"));
        e.src = src;
        e.autoplay = el.has_attr("autoplay");
        e.loop = el.has_attr("loop");
        e.muted = el.has_attr("muted");
        const media::own::Info in = media::own::attach(&el, e);
        const std::string poster = el.attr_or("poster");
        if (!audio && !poster.empty() && !poster.starts_with("data:")) {
            if (const render::ImageBitmap* bm = render::load_image(render::resolve_image(base_dir, poster)))
                b.img = std::shared_ptr<const render::ImageBitmap>(bm, [](const render::ImageBitmap*) {});
        }
        if (audio) { b.nat_w = 300; b.nat_h = 54; }
        else if (in.w > 0 && in.h > 0) { b.nat_w = in.w; b.nat_h = in.h; }
        else if (b.img) { b.nat_w = float(b.img->w); b.nat_h = float(b.img->h); }
        else { b.nat_w = 300; b.nat_h = 150; b.no_ratio = true; }   // nothing loaded: no intrinsic ratio either
    }

    static std::string lower_attr(const dom::Node& el, const char* k) {
        std::string v = el.attr_or(k);
        for (char& c : v) c = char(std::tolower(static_cast<unsigned char>(c)));
        return v;
    }
    static int int_attr(const dom::Node& el, const char* k, int def) {
        const std::string v = el.attr_or(k);
        const int n = v.empty() ? 0 : std::atoi(v.c_str());
        return n > 0 ? n : def;
    }

    // <input>/<textarea>/<select>.  Button-like inputs become a block with a text child (like <button>);
    // everything else a replaced box carrying a Control.  False: not handled here (falls through to a plain box).
    bool build_control(const dom::Node& el, const ComputedStyle* st, Box& into) {
        const std::string tag = el.name;
        auto c = std::make_shared<Control>();
        c->disabled = el.has_attr("disabled");
        c->readonly = el.has_attr("readonly");
        c->placeholder = el.attr_or("placeholder");
        if (!c->placeholder.empty()) {
            const ComputedStyle ph = res.compute(el, st, style::Pseudo::Placeholder);
            if (ph.color != st->color) c->ph_color = ph.color;
        }
        if (tag == "input") {
            const std::string ty = lower_attr(el, "type");
            if (ty == "hidden") return true;
            if (ty == "submit" || ty == "button" || ty == "reset") {
                auto b = make(Box::Kind::Block, &el, st);
                auto t = make(Box::Kind::Text, nullptr, st, true);
                t->text = el.has_attr("value") ? el.attr_or("value") : ty == "submit" ? "Submit" : ty == "reset" ? "Reset" : "";
                if (!t->text.empty()) adopt(*b, std::move(t));
                adopt(into, std::move(b));
                return true;
            }
            c->text = el.attr_or("value");
            c->size = int_attr(el, "size", 20);
            if (ty == "checkbox" || ty == "radio") { c->type = ty == "radio" ? Control::Type::Radio : Control::Type::Checkbox; c->checked = el.has_attr("checked"); }
            else if (ty == "range") {
                c->type = Control::Type::Range;
                const double lo = el.has_attr("min") ? std::atof(el.attr_or("min").c_str()) : 0, hi = el.has_attr("max") ? std::atof(el.attr_or("max").c_str()) : 100;
                const double v = el.has_attr("value") ? std::atof(el.attr_or("value").c_str()) : (lo + hi) / 2;
                c->frac = hi > lo ? float(std::clamp((v - lo) / (hi - lo), 0.0, 1.0)) : 0.f;
            }
            else if (ty == "file") { c->type = Control::Type::File; c->text = el.attr_or("data-files"); }
            else if (ty == "color") {
                c->type = Control::Type::Color;
                uint32_t col = 0xFF000000u; bool cur = false;
                if (!style::parse_color(el.has_attr("value") ? el.attr_or("value") : std::string("#000000"), col, &cur)) col = 0xFF000000u;
                c->color = col | 0xFF000000u;
            }
            else if (ty == "password") c->type = Control::Type::Password;
        } else if (tag == "textarea") {
            c->type = Control::Type::Textarea;
            c->text = el.text_content();
            if (c->text.starts_with("\n")) c->text.erase(0, 1);
            c->cols = int_attr(el, "cols", 20);
            c->rows = int_attr(el, "rows", 2);
        } else {
            c->type = Control::Type::Select;
            c->multi = el.has_attr("multiple");
            int i = 0;
            auto scan = [&](auto&& self, const dom::Node& n) -> void {
                for (const auto& ch : n.children) {
                    if (!ch->is_element()) continue;
                    if (ch->name == "option") {
                        std::string label = ch->has_attr("label") ? ch->attr_or("label") : ch->text_content();
                        c->options.push_back(label);
                        if (ch->has_attr("selected")) c->selected = i;
                        ++i;
                    } else if (ch->name == "optgroup") self(self, *ch);
                }
            };
            scan(scan, el);
            if (c->selected < int(c->options.size())) c->text = c->options.empty() ? "" : c->options[c->selected];
        }
        auto b = make(Box::Kind::Replaced, &el, st);
        b->ctl = std::move(c);
        adopt(into, std::move(b));
        return true;
    }

    void build_element(const dom::Node& el, const ComputedStyle& parent_st, Box& into,
                       int* list_counter, int list_step) {
        ComputedStyle cs = res.compute(el, &parent_st);
        if (cs.display == Display::None) return;
        const ComputedStyle* st = keep(std::move(cs));

        CSet mine = counters_begin(*st, el.parent);
        struct CountersCtx {
            Builder* b; CSet* parent; CSet after_me;
            ~CountersCtx() { b->cs_parent = parent; b->cs_prev = std::move(after_me); }
        } counters_ctx{this, cs_parent, mine};
        cs_parent = &mine;
        cs_prev.clear();

        if (st->display == Display::Contents) {
            build_pseudo(el, *st, into, style::Pseudo::Before);
            build_children(el, *st, into, list_counter, list_step);
            build_pseudo(el, *st, into, style::Pseudo::After);
            return;
        }

        const std::string& tag = el.name;
        std::unique_ptr<Box> b;
        if (tag == "br") {
            b = make(Box::Kind::Break, &el, st);
            adopt(into, std::move(b));
            return;
        }
        if (tag == "video" || (tag == "audio" && el.has_attr("controls"))) {
            b = make(Box::Kind::Replaced, &el, st);
            load_media(*b, el);
            adopt(into, std::move(b));
            return;
        }
        if (tag == "audio") return;   // no controls: nothing to show
        if (tag == "input" || tag == "textarea" || tag == "select") {
            if (build_control(el, st, into)) return;
        }
        if (tag == "progress" || tag == "meter") {
            auto c = std::make_shared<Control>();
            auto num = [&](const char* k, double def) { return el.has_attr(k) ? std::atof(el.attr_or(k).c_str()) : def; };
            if (tag == "progress") {
                c->type = Control::Type::Progress;
                const double mx = std::max(1e-9, num("max", 1));
                c->indeterminate = !el.has_attr("value");
                c->frac = float(std::clamp(num("value", 0) / mx, 0.0, 1.0));
            } else {
                c->type = Control::Type::Meter;
                const double lo = num("min", 0), hi = std::max(lo, num("max", 1)), v = std::clamp(num("value", 0), lo, hi);
                const double low = std::clamp(num("low", lo), lo, hi), high = std::clamp(num("high", hi), lo, hi);
                const double opt = std::clamp(num("optimum", (lo + hi) / 2), lo, hi);
                c->frac = hi > lo ? float((v - lo) / (hi - lo)) : 0.f;
                // optimum in the low / middle / high third decides which end is "good"
                const int region_v = v < low ? 0 : v > high ? 2 : 1, region_o = opt < low ? 0 : opt > high ? 2 : 1;
                c->meter_state = region_v == region_o ? 0 : std::abs(region_v - region_o) == 1 ? 1 : 2;
            }
            auto b = make(Box::Kind::Replaced, &el, st);
            b->ctl = std::move(c);
            adopt(into, std::move(b));
            return;
        }
        if (tag == "img") {
            b = make(Box::Kind::Replaced, &el, st);
            load_replaced(*b, el);
            adopt(into, std::move(b));
            return;
        }
        if (tag == "object" || tag == "embed") {   // a picture: like <img>; anything else shows the element's fallback content
            std::string src = el.attr_or(tag == "object" ? "data" : "src");
            std::string type = lower_attr(el, "type");
            std::string path = src.substr(0, src.find_first_of("?#"));
            for (char& c : path) c = char(std::tolower(static_cast<unsigned char>(c)));
            auto ends = [&](const char* e) { const std::string x = e; return path.size() >= x.size() && path.compare(path.size() - x.size(), x.size(), x) == 0; };
            const bool image = type.starts_with("image/") || ends(".png") || ends(".jpg") || ends(".jpeg") || ends(".gif") ||
                               ends(".webp") || ends(".svg") || ends(".bmp") || ends(".ico") || ends(".avif");
            if (image && !src.empty()) {
                b = make(Box::Kind::Replaced, &el, st);
                const render::ImageBitmap* bm = render::load_image(render::resolve_image(base_dir, src));
                if (bm) {
                    b->img = std::shared_ptr<const render::ImageBitmap>(bm, [](const render::ImageBitmap*) {});
                    b->nat_w = float(bm->w); b->nat_h = float(bm->h);
                } else b->broken_img = true;
                adopt(into, std::move(b));
                return;
            }
        }
        if (tag == "iframe") {   // no browsing context: the frame's box (300x150 unless sized), empty
            b = make(Box::Kind::Replaced, &el, st);
            b->nat_w = 300; b->nat_h = 150; b->no_ratio = true;
            adopt(into, std::move(b));
            return;
        }
        if (tag == "svg") {   // inline SVG: a replaced box drawn by the vector rasteriser
            b = make(Box::Kind::Replaced, &el, st);
            if (auto doc = render::svg::from_dom(el, st->color)) {
                float w, h;
                render::svg::intrinsic_size(*doc, w, h);
                auto bm = std::make_shared<render::ImageBitmap>();
                bm->w = std::max(1, int(std::lround(w))); bm->h = std::max(1, int(std::lround(h)));
                bm->svg = doc;   // no pixels of its own: the painter rasterises at the placed size
                b->img = bm;
                b->nat_w = w; b->nat_h = h;
            } else b->broken_img = true;
            adopt(into, std::move(b));
            return;
        }
        const bool inline_box = st->display == Display::Inline;
        b = make(inline_box ? Box::Kind::Inline : Box::Kind::Block, &el, st);

        // <td colspan rowspan>
        if (st->display == Display::TableCell) {
            b->col_span = std::max(1, std::atoi(el.attr_or("colspan", "1").c_str()));
            b->row_span = std::max(1, std::atoi(el.attr_or("rowspan", "1").c_str()));
        }

        // list numbering
        int counter = 0, step = 1;
        int* my_counter = list_counter;
        int my_step = list_step;
        if (tag == "ol" || tag == "ul" || tag == "menu") {
            const bool rev = el.has_attr("reversed");
            step = rev ? -1 : 1;
            int start = 1;
            if (el.has_attr("start")) start = std::atoi(el.attr_or("start").c_str());
            else if (rev) {
                int n = 0;
                for (const auto& c : el.children) if (c->is_element() && c->name == "li") ++n;
                start = n;
            }
            counter = start;
            my_counter = &counter;
            my_step = step;
        }
        if (st->display == Display::ListItem) {
            if (el.has_attr("value") && my_counter) *my_counter = std::atoi(el.attr_or("value").c_str());
            const int n = my_counter ? *my_counter : 1;
            if (my_counter) *my_counter += my_step;
            auto mk = make(Box::Kind::Marker, &el, st, true);
            using LS = style::ListStyle;
            switch (st->list_style) {
            case LS::None: mk.reset(); break;
            case LS::Disc: mk->marker_shape = 1; break;
            case LS::Circle: mk->marker_shape = 2; break;
            case LS::Square: mk->marker_shape = 3; break;
            default: mk->text = style::counter_text(n, st->list_style) + ". "; break;
            }
            if (mk && !st->list_image.empty()) {   // list-style-image: the picture instead of the bullet (or the number)
                if (const render::ImageBitmap* bm = render::load_image(render::resolve_image(base_dir, st->list_image))) {
                    mk->marker_shape = 0;
                    mk->text.clear();
                    mk->img = std::shared_ptr<const render::ImageBitmap>(bm, [](const render::ImageBitmap*) {});
                    mk->nat_w = float(bm->w); mk->nat_h = float(bm->h);
                }
            }
            if (mk) adopt(*b, std::move(mk));
        }

        build_pseudo(el, *st, *b, style::Pseudo::Before);
        build_children(el, *st, *b, my_counter, my_step);
        build_pseudo(el, *st, *b, style::Pseudo::After);
        if (res.pseudo_matches(el, style::Pseudo::FirstLetter) && st->display != Display::Inline && st->display != Display::None) first_letter(el, *st, *b);
        if (res.pseudo_matches(el, style::Pseudo::FirstLine) && st->display != Display::Inline && st->display != Display::None) {
            ComputedStyle fs = res.compute(el, st, style::Pseudo::FirstLine);
            if (fs.display != Display::None) b->first_line_st = keep(std::move(fs));
        }
        fix_children(*b);
        adopt(into, std::move(b));
    }

    // ::first-letter: the first letter of the block's first formatted line (with the punctuation around it) becomes
    // a box of its own that takes the pseudo-element's style.  A float:left drop cap works the same way.
    static size_t first_letter_len(const std::string& t) {
        auto cp_len = [&](size_t i) { const unsigned char c = (unsigned char)t[i]; return c < 0x80 ? 1u : c < 0xE0 ? 2u : c < 0xF0 ? 3u : 4u; };
        auto cp = [&](size_t i) { size_t n = cp_len(i); uint32_t v = n == 1 ? (unsigned char)t[i] : (unsigned char)t[i] & (0xFF >> (n + 1));
                                  for (size_t k = 1; k < n && i + k < t.size(); ++k) v = (v << 6) | ((unsigned char)t[i + k] & 0x3F); return v; };
        auto punct = [](uint32_t c) { return (c < 0x80 && std::ispunct((int)c)) || (c >= 0x2018 && c <= 0x201F) || c == 0xAB || c == 0xBB || c == 0xBF || c == 0xA1; };
        size_t i = 0;
        while (i < t.size() && punct(cp(i))) i += cp_len(i);
        if (i >= t.size()) return 0;
        i += cp_len(i);
        while (i < t.size() && punct(cp(i))) i += cp_len(i);
        return std::min(i, t.size());
    }
    bool split_first_letter(Box& parent, const ComputedStyle& fl) {
        for (size_t i = 0; i < parent.kids.size(); ++i) {
            Box& k = *parent.kids[i];
            if (k.kind == Box::Kind::Marker || (k.out_of_flow() && k.kind != Box::Kind::Text)) continue;
            if (k.kind == Box::Kind::Text) {
                const size_t a = k.text.find_first_not_of(" \t\r\n");
                if (a == std::string::npos) continue;
                const size_t n = first_letter_len(k.text.substr(a));
                if (n == 0) return true;
                const bool blk = fl.float_ != style::Float::None || fl.display == Display::Block;
                auto fb = make(blk ? Box::Kind::Block : Box::Kind::Inline, parent.node, &fl, true);
                auto lt = make(Box::Kind::Text, nullptr, &fl, true);
                lt->text = k.text.substr(a, n);
                adopt(*fb, std::move(lt));
                k.text = k.text.substr(a + n);
                fb->parent = &parent;
                parent.kids.insert(parent.kids.begin() + long(i), std::move(fb));
                return true;
            }
            if (k.kind == Box::Kind::Inline || (k.kind == Box::Kind::Block && !k.is_float())) {
                if (split_first_letter(k, fl)) return true;
                if (k.kind == Box::Kind::Block) return true;   // the first line is inside it, even when nothing matched
                continue;
            }
            return true;   // replaced, <br>, float...: no first letter
        }
        return false;
    }
    void first_letter(const dom::Node& el, const ComputedStyle& st, Box& b) {
        ComputedStyle fs = res.compute(el, &st, style::Pseudo::FirstLetter);
        if (fs.display == Display::None) return;
        split_first_letter(b, *keep(std::move(fs)));
    }

    // ── anonymous box fix-ups ──────────────────────────────────────────────
    static bool block_level_in_flow(const Box& c) {
        if (c.out_of_flow() || c.kind == Box::Kind::Marker) return false;
        return !c.inline_level();
    }

    // Wraps runs of children[b..e) satisfying `pred` in anonymous boxes of display `d`.
    template <class Pred>
    void wrap_runs(Box& parent, Display d, Pred pred, bool drop_blank_runs) {
        std::vector<std::unique_ptr<Box>> out;
        std::unique_ptr<Box> cur;
        auto flush = [&] {
            if (!cur) return;
            bool all_blank = true;
            for (auto& k : cur->kids)
                if (!(k->kind == Box::Kind::Text && blank(k->text))) { all_blank = false; break; }
            if (!(drop_blank_runs && all_blank)) { cur->parent = &parent; out.push_back(std::move(cur)); }
            cur.reset();
        };
        for (auto& k : parent.kids) {
            if (pred(*k)) {
                if (!cur) cur = anon_block(*parent.st, d);
                k->parent = cur.get();
                cur->kids.push_back(std::move(k));
            } else {
                flush();
                out.push_back(std::move(k));
            }
        }
        flush();
        parent.kids = std::move(out);
        for (auto& k : parent.kids) k->parent = &parent;
    }

    void fix_children(Box& b) {
        const Display d = b.st->display;
        // inline box with block children: promote to a block container (approximation of block-in-inline)
        if (b.kind == Box::Kind::Inline) {
            bool has_block = false;
            for (auto& c : b.kids) if (block_level_in_flow(*c)) { has_block = true; break; }
            if (has_block) {
                auto* s = const_cast<ComputedStyle*>(b.st);
                s->display = Display::Block;
                b.kind = Box::Kind::Block;
            }
        }
        if (b.kind != Box::Kind::Block) return;

        if (d == Display::Table || d == Display::InlineTable) { fix_table(b); return; }
        if (d == Display::TableRow) {
            wrap_runs(b, Display::TableCell, [](const Box& c) { return c.st->display != Display::TableCell && !c.out_of_flow(); }, true);
            return;
        }
        if (is_row_group(d)) {
            wrap_runs(b, Display::TableRow, [](const Box& c) { return c.st->display != Display::TableRow && !c.out_of_flow(); }, true);
            for (auto& c : b.kids) if (c->anonymous && c->st->display == Display::TableRow) fix_children(*c);
            return;
        }
        // stray table-internal children in a non-table parent -> anonymous table
        {
            bool stray = false;
            for (auto& c : b.kids)
                if (c->own_style() && is_table_internal(c->st->display) && c->st->display != Display::TableCaption) { stray = true; break; }
            if (stray) {
                wrap_runs(b, Display::Table,
                          [](const Box& c) { return c.own_style() && is_table_internal(c.st->display) && c.st->display != Display::TableCaption; }, false);
                for (auto& c : b.kids) if (c->anonymous && c->st->display == Display::Table) fix_table(*c);
            }
        }
        if (d == Display::Flex || d == Display::InlineFlex || d == Display::Grid || d == Display::InlineGrid) {
            // every contiguous run of inline-level content becomes one anonymous flex/grid item
            wrap_runs(b, Display::Block,
                      [](const Box& c) { return c.kind != Box::Kind::Marker && !c.out_of_flow() && c.inline_level() && !c.is_atomic_inline(); }, true);
            // atomic inlines (inline-block) are blockified items by the cascade already
            return;
        }
        // block container: mixing inline and block children -> wrap inline runs
        bool has_block = false;
        for (auto& c : b.kids) if (block_level_in_flow(*c)) { has_block = true; break; }
        if (has_block)
            wrap_runs(b, Display::Block,
                      [](const Box& c) { return c.kind != Box::Kind::Marker && (c.inline_level() || (c.out_of_flow() && false)); }, true);
    }

    void fix_table(Box& t) {
        // children allowed: caption, colgroup/col, row groups, rows.  Others -> anonymous row(+cell)
        auto allowed = [](const Box& c) {
            const Display d = c.st->display;
            return c.out_of_flow() || d == Display::TableCaption || d == Display::TableColumn ||
                   d == Display::TableColumnGroup || is_row_group(d) || d == Display::TableRow;
        };
        // a run of non-allowed children becomes: anon row > anon cell > run
        std::vector<std::unique_ptr<Box>> out;
        std::unique_ptr<Box> row;
        auto flush = [&] {
            if (!row) return;
            bool all_blank = true;
            for (auto& cell : row->kids) for (auto& k : cell->kids)
                if (!(k->kind == Box::Kind::Text && blank(k->text))) all_blank = false;
            if (!all_blank) { row->parent = &t; out.push_back(std::move(row)); }
            row.reset();
        };
        for (auto& k : t.kids) {
            if (allowed(*k)) {
                flush();
                if (k->st->display == Display::TableRow) fix_children(*k);
                out.push_back(std::move(k));
            } else {
                if (!row) {
                    row = anon_block(*t.st, Display::TableRow);
                    row->kids.push_back(anon_block(*t.st, Display::TableCell));
                    row->kids.back()->parent = row.get();
                }
                Box* cell = row->kids.back().get();
                // text/inline in a cell: normal block-container fix-up happens when we fix the cell
                k->parent = cell;
                cell->kids.push_back(std::move(k));
            }
        }
        flush();
        t.kids = std::move(out);
        for (auto& k : t.kids) {
            k->parent = &t;
            if (k->anonymous && k->st->display == Display::TableRow)
                for (auto& cell : k->kids) fix_children(*cell);
        }
        // groups: rows inside
        for (auto& k : t.kids) if (is_row_group(k->st->display) && !k->anonymous) fix_children(*k);
    }

    void number(Box& b) {
        b.order = static_cast<int>(tree.order.size());
        tree.order.push_back(&b);
        for (auto& k : b.kids) number(*k);
    }
};

} // namespace

std::unique_ptr<Tree> build_tree(const dom::Node& document, const style::Resolver& res,
                                 const std::string& base_dir) {
    auto tree = std::make_unique<Tree>();
    Builder bd{res, *tree, base_dir};
    const dom::Node* html = nullptr;
    for (const auto& ch : document.children)
        if (ch->is_element() && ch->name == "html") { html = ch.get(); break; }
    if (!html) {
        tree->root = bd.make(Box::Kind::Block, nullptr, bd.keep(inherited_style(ComputedStyle{}, Display::Block)), true);
        return tree;
    }
    ComputedStyle hs = res.compute(*html, nullptr);
    if (hs.display == Display::None) hs.display = Display::Block;
    hs.display = Display::Block;
    const ComputedStyle* hst = bd.keep(std::move(hs));
    tree->html_style = hst;
    auto root = bd.make(Box::Kind::Block, html, hst);
    int counter = 1;
    bd.build_pseudo(*html, *hst, *root, style::Pseudo::Before);
    bd.build_children(*html, *hst, *root, &counter, 1);
    bd.build_pseudo(*html, *hst, *root, style::Pseudo::After);
    bd.fix_children(*root);
    tree->root = std::move(root);
    for (auto& k : tree->root->kids)
        if (k->node && k->node->name == "body") { tree->body_box = k.get(); tree->body_style = k->st; }
    bd.number(*tree->root);
    media::own::sweep();   // players of elements that left the tree stop here
    return tree;
}

} // namespace luxium::layout
