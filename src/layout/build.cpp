// DOM + styles -> box tree: display:none/contents, generated content,
// replaced elements, list markers, and the anonymous-box fix-ups CSS requires
// (inline runs inside blocks, flex/grid items, table structure).

#include <layout/box.hpp>

#include <algorithm>
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
    void build_children(const dom::Node& el, const ComputedStyle& st, Box& into, int* list_counter, int list_step) {
        for (const auto& ch : el.children) {
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
        const bool inl = pst->display == Display::Inline;
        auto b = make(inl ? Box::Kind::Inline : Box::Kind::Block, &el, pst, true);
        if (!pst->content.empty()) {
            auto t = make(Box::Kind::Text, nullptr, pst, true);
            t->text = pst->content;
            adopt(*b, std::move(t));
        }
        adopt(into, std::move(b));
    }

    void load_replaced(Box& b, const dom::Node& el) {
        const std::string src = el.attr_or("src");
        if (src.empty() || src.starts_with("http://") || src.starts_with("https://") || src.starts_with("data:")) {
            b.broken_img = true;
            return;
        }
        std::string path = src;
        if (!path.starts_with('/')) path = base_dir + path;
        const render::ImageBitmap* bm = render::load_image(path);
        if (!bm) { b.broken_img = true; return; }
        // cached forever by load_image: a non-owning shared_ptr is enough
        b.img = std::shared_ptr<const render::ImageBitmap>(bm, [](const render::ImageBitmap*) {});
        b.nat_w = static_cast<float>(bm->w);
        b.nat_h = static_cast<float>(bm->h);
    }

    void build_element(const dom::Node& el, const ComputedStyle& parent_st, Box& into,
                       int* list_counter, int list_step) {
        ComputedStyle cs = res.compute(el, &parent_st);
        if (cs.display == Display::None) return;
        const ComputedStyle* st = keep(std::move(cs));

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
        if (tag == "img") {
            b = make(Box::Kind::Replaced, &el, st);
            load_replaced(*b, el);
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
            case LS::Decimal: mk->text = std::to_string(n) + ". "; break;
            case LS::LowerAlpha: case LS::UpperAlpha: {
                std::string a;
                int v = n;
                if (v <= 0) a = std::to_string(v);
                else while (v > 0) { a.insert(a.begin(), char((st->list_style == LS::LowerAlpha ? 'a' : 'A') + (v - 1) % 26)); v = (v - 1) / 26; }
                mk->text = a + ". ";
                break;
            }
            case LS::LowerRoman: case LS::UpperRoman: {
                static const std::pair<int, const char*> r[] = {{1000, "m"}, {900, "cm"}, {500, "d"}, {400, "cd"}, {100, "c"}, {90, "xc"},
                                                              {50, "l"}, {40, "xl"}, {10, "x"}, {9, "ix"}, {5, "v"}, {4, "iv"}, {1, "i"}};
                std::string out;
                int v = n;
                if (v > 0 && v < 4000) for (auto& [val, sym] : r) while (v >= val) { out += sym; v -= val; }
                else out = std::to_string(n);
                if (st->list_style == LS::UpperRoman) for (char& c : out) c = char(std::toupper(c));
                mk->text = out + ". ";
                break;
            }
            }
            if (mk) adopt(*b, std::move(mk));
        }

        build_pseudo(el, *st, *b, style::Pseudo::Before);
        build_children(el, *st, *b, my_counter, my_step);
        build_pseudo(el, *st, *b, style::Pseudo::After);
        fix_children(*b);
        adopt(into, std::move(b));
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
    return tree;
}

} // namespace luxium::layout
