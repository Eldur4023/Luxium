#include "html/tokenizer.hpp"

#include <algorithm>
#include <cctype>

namespace luxium::html {

namespace {

bool is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
bool is_alnum(char c) {
    return is_alpha(c) || (c >= '0' && c <= '9');
}
bool is_ws(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
}
std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}
int count_lines(std::string_view s) {
    return static_cast<int>(std::count(s.begin(), s.end(), '\n'));
}

constexpr size_t kNpos = std::string_view::npos;

// Case-insensitive find of `needle` starting at `from`.
size_t ifind(std::string_view s, size_t from, std::string_view needle) {
    if (needle.empty() || s.size() < needle.size()) return kNpos;
    for (size_t i = from; i + needle.size() <= s.size(); ++i) {
        size_t j = 0;
        while (j < needle.size() &&
               std::tolower(static_cast<unsigned char>(s[i + j])) ==
                   std::tolower(static_cast<unsigned char>(needle[j])))
            ++j;
        if (j == needle.size()) return i;
    }
    return kNpos;
}

} // namespace

std::vector<Token> tokenize(std::string_view src) {
    std::vector<Token> out;
    const size_t n = src.size();
    size_t i = 0;
    int line = 1;

    std::string text;
    int text_line = 1;
    auto flush_text = [&] {
        if (!text.empty()) {
            Token t;
            t.type = TokType::Text;
            t.text = std::move(text);
            t.line = text_line;
            text.clear();
            out.push_back(std::move(t));
        }
    };

    while (i < n) {
        char c = src[i];

        // Plain data: everything up to the next '<'.
        if (c != '<') {
            if (text.empty()) text_line = line;
            if (c == '\n') ++line;
            text.push_back(c);
            ++i;
            continue;
        }

        // Comments: <!-- ... -->
        if (src.compare(i, 4, "<!--") == 0) {
            flush_text();
            size_t end = src.find("-->", i + 4);
            size_t stop = (end == kNpos) ? n : end;
            Token t;
            t.type  = TokType::Comment;
            t.text  = std::string(src.substr(i + 4, stop - (i + 4)));
            t.line  = line;
            line   += count_lines(src.substr(i, (end == kNpos ? n : end + 3) - i));
            i       = (end == kNpos) ? n : end + 3;
            out.push_back(std::move(t));
            continue;
        }

        // Doctype / bogus markup: <! ... >
        if (src.compare(i, 2, "<!") == 0) {
            flush_text();
            size_t end = src.find('>', i);
            size_t stop = (end == kNpos) ? n : end;
            Token t;
            t.type = TokType::Doctype;
            t.text = std::string(src.substr(i + 2, stop - (i + 2)));
            t.line = line;
            line  += count_lines(src.substr(i, (end == kNpos ? n : end + 1) - i));
            i      = (end == kNpos) ? n : end + 1;
            out.push_back(std::move(t));
            continue;
        }

        // End tag: </name ...>
        if (src.compare(i, 2, "</") == 0 && i + 2 < n && is_alpha(src[i + 2])) {
            flush_text();
            i += 2;
            size_t name_start = i;
            while (i < n && (is_alnum(src[i]) || src[i] == '-')) ++i;
            Token t;
            t.type = TokType::EndTag;
            t.name = lower(src.substr(name_start, i - name_start));
            t.line = line;
            // Skip anything up to '>' (stray attributes on end tags).
            while (i < n && src[i] != '>') {
                if (src[i] == '\n') ++line;
                ++i;
            }
            if (i < n) ++i;  // consume '>'
            line += count_lines(src.substr(name_start, i - name_start));
            out.push_back(std::move(t));
            continue;
        }

        // Start tag: <name attr=...>
        if (i + 1 < n && is_alpha(src[i + 1])) {
            flush_text();
            size_t start = i;
            ++i;
            size_t name_start = i;
            while (i < n && (is_alnum(src[i]) || src[i] == '-')) ++i;
            Token t;
            t.type = TokType::StartTag;
            t.name = lower(src.substr(name_start, i - name_start));
            t.line = line;

            bool rawtext_done = false;
            while (i < n && !rawtext_done) {
                while (i < n && is_ws(src[i])) {
                    if (src[i] == '\n') ++line;
                    ++i;
                }
                if (i >= n) break;
                if (src[i] == '>') { ++i; break; }
                if (src[i] == '/') {
                    ++i;
                    if (i < n && src[i] == '>') { t.self_closing = true; ++i; break; }
                    continue;  // stray '/' inside the tag
                }

                // Attribute name: up to ws, '=', '>' or '/'.
                size_t astart = i;
                while (i < n && !is_ws(src[i]) && src[i] != '=' && src[i] != '>' && src[i] != '/')
                    ++i;
                std::string aname = lower(src.substr(astart, i - astart));
                std::string avalue;
                size_t look = i;
                while (look < n && is_ws(src[look])) ++look;
                if (look < n && src[look] == '=') {
                    i = look + 1;
                    while (i < n && is_ws(src[i])) {
                        if (src[i] == '\n') ++line;
                        ++i;
                    }
                    if (i < n && (src[i] == '"' || src[i] == '\'')) {
                        char q = src[i++];
                        size_t vstart = i;
                        while (i < n && src[i] != q) {
                            if (src[i] == '\n') ++line;
                            ++i;
                        }
                        avalue = std::string(src.substr(vstart, i - vstart));
                        if (i < n) ++i;  // closing quote
                    } else {
                        size_t vstart = i;
                        while (i < n && !is_ws(src[i]) && src[i] != '>')
                            ++i;
                        avalue = std::string(src.substr(vstart, i - vstart));
                    }
                }
                bool dup = std::any_of(t.attrs.begin(), t.attrs.end(),
                                       [&](const auto& kv) { return kv.first == aname; });
                if (!aname.empty() && !dup) t.attrs.emplace_back(std::move(aname), std::move(avalue));
            }
            line += count_lines(src.substr(start, i - start));
            const std::string tag_name = t.name;  // captured before the move below
            const bool is_rawtext = (tag_name == "script" || tag_name == "style");
            out.push_back(std::move(t));

            // Raw-text mode: <script>/<style> swallow everything up to their
            // end tag as one Text token; the end tag itself is consumed by the
            // normal loop above on the next iteration.
            if (!t.self_closing && is_rawtext) {
                std::string closer = "</" + tag_name;
                size_t r = i;
                while (true) {
                    size_t hit = ifind(src, r, closer);
                    if (hit == kNpos) {
                        Token rt;
                        rt.type = TokType::Text;
                        rt.text = std::string(src.substr(i));
                        rt.line = line;
                        line   += count_lines(rt.text);
                        i       = n;
                        out.push_back(std::move(rt));
                        break;
                    }
                    size_t after = hit + closer.size();
                    if (after >= n || is_ws(src[after]) || src[after] == '/' || src[after] == '>') {
                        Token rt;
                        rt.type = TokType::Text;
                        rt.text = std::string(src.substr(i, hit - i));
                        rt.line = line;
                        line   += count_lines(rt.text);
                        i       = hit;
                        out.push_back(std::move(rt));
                        break;
                    }
                    r = hit + 1;  // e.g. "</scriptx" -- keep looking
                }
            }
            continue;
        }

        // Lone '<' is text.
        if (text.empty()) text_line = line;
        text.push_back('<');
        ++i;
    }

    flush_text();
    return out;
}

} // namespace luxium::html
