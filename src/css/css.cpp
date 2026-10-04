#include "css/css.hpp"

#include <algorithm>
#include <cctype>

namespace luxium::css {

namespace {

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Strips /* ... */ comments.
std::string strip_comments(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (i + 1 < s.size() && s[i] == '/' && s[i + 1] == '*') {
            size_t e = s.find("*/", i + 2);
            if (e == std::string::npos) break;
            i = e + 2;
            continue;
        }
        out += s[i++];
    }
    return out;
}

// One compound selector: sequences of tag / .class / #id.  The '.', '#'
// markers are part of the word: `section.oscuro` must flush "section" and
// start ".oscuro", or the class silently becomes a second tag and `.oculto`
// stops hiding anything.
Compound parse_compound(const std::string& text) {
    Compound c;
    std::string word;
    auto flush = [&] {
        if (word.empty()) return;
        switch (word[0]) {
        case '.': c.classes.push_back(word.substr(1)); break;
        case '#': c.id = word.substr(1); break;
        default:
            if (c.tag.empty()) c.tag = lower(word);
            break;
        }
        word.clear();
    };
    for (char ch : text) {
        const bool special = ch == '.' || ch == '#';
        if (special && !word.empty()) flush();
        if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_' ||
            ch == '*' || special) {
            word += ch;
        } else {
            flush();
        }
    }
    flush();
    return c;
}

// A full selector: compounds separated by whitespace (descendant only).
Chain parse_chain(const std::string& text) {
    Chain chain;
    std::string word;
    for (char ch : text) {
        if (std::isspace(static_cast<unsigned char>(ch))) {
            if (!word.empty()) { chain.push_back(parse_compound(word)); word.clear(); }
        } else {
            word += ch;
        }
    }
    if (!word.empty()) chain.push_back(parse_compound(word));
    return chain;
}

} // namespace

bool Compound::matches(const dom::Node& el) const {
    if (!tag.empty() && tag != "*" && el.name != tag) return false;
    if (!id.empty() && el.attr_or("id") != id) return false;
    for (const auto& cls : classes) {
        // class="a b" matches either
        bool has = false;
        for (const auto& [k, v] : el.attrs)
            if (k == "class") {
                size_t start = 0;
                while (start <= v.size()) {
                    size_t e = v.find(' ', start);
                    size_t end = (e == std::string::npos) ? v.size() : e;
                    if (v.compare(start, end - start, cls) == 0) { has = true; break; }
                    if (e == std::string::npos) break;
                    start = e + 1;
                }
                break;
            }
        if (!has) return false;
    }
    return true;
}

bool chain_matches(const Chain& chain, const dom::Node& el) {
    if (chain.empty()) return false;
    if (!chain.back().matches(el)) return false;
    // Walk the rest leftwards through ANY ancestors (descendant semantics).
    size_t i = chain.size() - 1;
    const dom::Node* n = &el;
    while (i-- > 0) {
        const dom::Node* p = n->parent;
        while (p && !chain[i].matches(*p)) p = p->parent;
        if (!p) return false;
        n = p;
    }
    return true;
}

int Rule::match(const dom::Node& el) const {
    int best = -1;
    for (const auto& chain : selectors) {
        if (chain_matches(chain, el)) {
            int s = 0;
            for (const auto& c : chain) s += c.specificity();
            best = std::max(best, s);
        }
    }
    return best;
}

std::vector<std::pair<std::string, std::string>> parse_decls(const std::string& text) {
    std::vector<std::pair<std::string, std::string>> out;
    size_t i = 0;
    while (i < text.size()) {
        size_t semi = text.find(';', i);
        size_t end = (semi == std::string::npos) ? text.size() : semi;
        size_t colon = text.find(':', i);
        if (colon < end) {
            std::string prop = lower(trim(text.substr(i, colon - i)));
            std::string value = trim(text.substr(colon + 1, end - colon - 1));
            if (!prop.empty() && !value.empty()) out.emplace_back(prop, value);
        }
        if (semi == std::string::npos) break;
        i = semi + 1;
    }
    return out;
}

std::vector<Rule> parse_stylesheet(const std::string& raw) {
    const std::string text = strip_comments(raw);
    std::vector<Rule> out;

    size_t i = 0;
    while (i < text.size()) {
        // skip leading whitespace
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
        if (i >= text.size()) break;

        if (text[i] == '@') {   // at-rule: skip to ';' or the matching block
            size_t j = i, depth = 0;
            while (j < text.size()) {
                if (text[j] == '{') ++depth;
                if (text[j] == '}') { --depth; if (depth <= 0) { ++j; break; } }
                if (text[j] == ';' && depth == 0) { ++j; break; }
                ++j;
            }
            i = j;
            continue;
        }

        size_t brace = text.find('{', i);
        if (brace == std::string::npos) break;
        size_t close = text.find('}', brace);
        if (close == std::string::npos) close = text.size();

        Rule rule;
        // selector list: comma-separated chains
        std::string sel_text = trim(text.substr(i, brace - i));
        size_t a = 0;
        while (a <= sel_text.size()) {
            size_t comma = sel_text.find(',', a);
            size_t e = (comma == std::string::npos) ? sel_text.size() : comma;
            std::string one = sel_text.substr(a, e - a);
            Chain chain = parse_chain(trim(one));
            if (!chain.empty()) rule.selectors.push_back(std::move(chain));
            if (comma == std::string::npos) break;
            a = comma + 1;
        }
        rule.decls = parse_decls(text.substr(brace + 1, close - brace - 1));
        if (!rule.selectors.empty() && !rule.decls.empty()) out.push_back(std::move(rule));

        i = (close < text.size()) ? close + 1 : text.size();
    }
    return out;
}

} // namespace luxium::css
