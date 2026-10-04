#pragma once
// A CSS subset, exactly as much as the layout engine can honor (M3 of
// docs/ROADMAP.md):
//
//   selectors:  tag, .class, #id, descendant combinator, comma lists
//   properties: color, background-color, font-size, font-weight,
//               font-family (mono detection), line-height, text-align,
//               display, margin*, padding*
//   cascade:    specificity (id=100, class=10, tag=1), then source order;
//               inheritance per property (color/size/line-height/family/
//               align/weight flow down, boxes do not)
//
// Unknown properties and malformed rules are skipped, not fatal: a page's
// CSS must degrade, the way every real browser behaves.  At-rules (@media,
// @import...) are skipped to the end of their block.
//
// The browser's own default look is ITSELF a stylesheet parsed with this
// parser (layout.cpp's kUAStylesheet) -- the design table and the page CSS
// go through the same code, which is why a page rule like `h1 { color: X }`
// beats the built-in one without special cases: it just has higher
// specificity.

#include <dom/node.hpp>

#include <string>
#include <utility>
#include <vector>

namespace luxium::css {

// One compound selector: `div.card#main` -> tag="div", classes={card}, id=main.
struct Compound {
    std::string tag;                  // "" or "*" = any tag
    std::vector<std::string> classes;
    std::string id;

    int specificity() const {
        return (id.empty() ? 0 : 100) + static_cast<int>(classes.size()) * 10 +
               (tag.empty() || tag == "*" ? 0 : 1);
    }
    bool matches(const dom::Node& el) const;
};

// A selector as a descendant chain, left to right: `nav a span`.
using Chain = std::vector<Compound>;

struct Rule {
    std::vector<Chain> selectors;
    std::vector<std::pair<std::string, std::string>> decls;   // prop, value

    // Highest specificity among the chains that match, or -1 if none does.
    int match(const dom::Node& el) const;
};

// Parses a whole stylesheet.  Never fails: broken rules are skipped.
std::vector<Rule> parse_stylesheet(const std::string& text);

// Parses an inline style="" attribute (declarations only).
std::vector<std::pair<std::string, std::string>> parse_decls(const std::string& text);

// Does a descendant chain match `el` (last compound on el, the rest on
// ancestors, any depth)?
bool chain_matches(const Chain& chain, const dom::Node& el);

} // namespace luxium::css
