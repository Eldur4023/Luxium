#include "luxscript/lexer.hpp"

#include <cctype>

namespace luxium::lux {

namespace {

// Grammar §2: keywords. Server-only contexts still lex as keywords -- the
// browser rejects them at parse time (M2), not lex time.
const char* const kKeywords[] = {
    "import", "class", "fn", "app", "group", "endpoint", "on", "error", "origins",
    "get", "post", "put", "patch", "delete", "any", "sse", "ws",
    "if", "else", "elif", "while", "for", "in", "return", "require", "try",
    "catch", "break", "continue", "validate", "and", "or", "not",
    "true", "false", "null", "this", "void", "spa",
};

bool is_keyword(std::string_view s) {
    for (const char* k : kKeywords)
        if (s == k) return true;
    return false;
}

bool ident_start(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool ident_char(char c)  { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }
bool is_digit(char c)    { return c >= '0' && c <= '9'; }

struct Lexer {
    std::string_view src;
    LexResult        out;
    size_t           pos = 0;
    int              line = 1;
    int              col = 1;
    std::vector<int> indents{0};
    int              bracket_depth = 0;
    bool             at_line_start = true;

    bool eof() const { return pos >= src.size(); }
    char peek(size_t ahead = 0) const {
        size_t i = pos + ahead;
        return i < src.size() ? src[i] : '\0';
    }
    void advance() {
        if (pos < src.size()) {
            if (src[pos] == '\n') { ++line; col = 1; }
            else ++col;
            ++pos;
        }
    }

    void error(std::string msg) {
        out.errors.push_back("line " + std::to_string(line) + ": " + std::move(msg));
    }
    void push(Tok kind, int ln, int cl, std::string text = {}) {
        out.tokens.push_back({kind, std::move(text), ln, cl});
    }
    void push(Tok kind, int ln, int cl, char c) { push(kind, ln, cl, std::string(1, c)); }

    // Decodes the body of a string literal (escape handling; unknown escapes
    // keep both characters -- matches Lux's forgiving behavior).
    std::string decode(std::string_view raw, int ln) {
        std::string s;
        for (size_t i = 0; i < raw.size(); ++i) {
            char c = raw[i];
            if (c != '\\') { s.push_back(c); continue; }
            if (++i >= raw.size()) { s.push_back('\\'); break; }
            switch (raw[i]) {
            case 'n': s.push_back('\n'); break;
            case 't': s.push_back('\t'); break;
            case 'r': s.push_back('\r'); break;
            case '0': s.push_back('\0'); break;
            case '\\': s.push_back('\\'); break;
            case '"': s.push_back('"'); break;
            default:
                error("unknown escape '\\" + std::string(1, raw[i]) + "' in string");
                s.push_back('\\');
                s.push_back(raw[i]);
                (void)ln;
                break;
            }
        }
        return s;
    }

    // Consumes the line if it is blank or comment-only (neither may produce
    // Newline or affect indentation -- grammar §1). Returns true if consumed.
    bool skip_blank_or_comment_line() {
        size_t save = pos;
        int save_line = line, save_col = col;
        while (!eof() && peek() == ' ') advance();
        if (eof() || peek() == '\n' || peek() == '#') {
            while (!eof() && peek() != '\n') advance();
            if (!eof()) advance();  // the newline itself
            return true;
        }
        pos = save; line = save_line; col = save_col;
        return false;
    }

    // Handles indentation at a logical line start. A line whose first
    // non-space character is '.' continues the previous one (grammar §1).
    void handle_line_start() {
        int w = 0;
        while (!eof() && peek() == ' ') { ++w; advance(); }
        if (!eof() && peek() == '\t') {
            error("tabs are not valid indentation (spaces only)");
            while (!eof() && peek() == '\t') { ++w; advance(); }
        }
        if (!eof() && peek() == '.') return;  // continuation: no indent tokens

        int top = indents.back();
        if (w > top) {
            indents.push_back(w);
            push(Tok::Indent, line, 1);
        } else if (w < top) {
            while (indents.size() > 1 && indents.back() > w) {
                indents.pop_back();
                push(Tok::Dedent, line, 1);
            }
            if (indents.back() != w)
                error("unindent does not match any outer indentation level");
        }
    }

    void lex_number() {
        int ln = line, cl = col;
        size_t start = pos;
        while (!eof() && is_digit(peek())) advance();
        bool is_float = false;
        if (peek() == '.' && is_digit(peek(1))) {
            is_float = true;
            advance();
            while (!eof() && is_digit(peek())) advance();
        }
        if (peek() == 'e' || peek() == 'E') {
            size_t save = pos;
            advance();
            if (peek() == '+' || peek() == '-') advance();
            if (is_digit(peek())) {
                is_float = true;
                while (!eof() && is_digit(peek())) advance();
            } else {
                pos = save;  // not an exponent after all
            }
        }
        push(is_float ? Tok::Float : Tok::Int, ln, cl, std::string(src.substr(start, pos - start)));
    }

    void lex_string() {
        int ln = line, cl = col;
        advance();  // opening quote
        if (peek(0) == '"' && peek(1) == '"') {  // triple-quoted (§5)
            advance(); advance();
            size_t start = pos;
            while (!eof()) {
                if (peek() == '\\') { advance(); advance(); continue; }  // escaped char
                if (peek(0) == '"' && peek(1) == '"' && peek(2) == '"') break;
                advance();
            }
            if (eof()) {
                error("unterminated triple-quoted string");
            } else {
                std::string value = decode(src.substr(start, pos - start), ln);
                advance(); advance(); advance();
                push(Tok::String, ln, cl, std::move(value));
            }
            return;
        }
        size_t start = pos;
        while (!eof() && peek() != '"' && peek() != '\n') {
            if (peek() == '\\') advance();  // escaped char: consume it whole
            advance();
        }
        if (eof() || peek() == '\n') {
            error("unterminated string literal");
            return;
        }
        std::string value = decode(src.substr(start, pos - start), ln);
        advance();  // closing quote
        push(Tok::String, ln, cl, std::move(value));
    }

    void lex_ident() {
        int ln = line, cl = col;
        size_t start = pos;
        while (!eof() && ident_char(peek())) advance();
        std::string word(src.substr(start, pos - start));
        push(is_keyword(word) ? Tok::Keyword : Tok::Ident, ln, cl, std::move(word));
    }

    bool match2(char a, char b) const { return peek(0) == a && peek(1) == b; }

    void lex_op() {
        int ln = line, cl = col;
        char c = peek();
        // two-character operators first
        for (auto [a, b] : {std::pair{'=', '='}, {'!', '='}, {'<', '='}, {'>', '='},
                            {'/', '/'}, {'*', '*'}, {'-', '>'}}) {
            if (match2(a, b)) {
                std::string text{a, b};
                advance(); advance();
                push(Tok::Op, ln, cl, std::move(text));
                return;
            }
        }
        std::string text{c};
        advance();
        if (c == '(' || c == '[' || c == '{') ++bracket_depth;
        if (c == ')' || c == ']' || c == '}') bracket_depth = std::max(0, bracket_depth - 1);
        push(Tok::Op, ln, cl, std::move(text));
    }

    LexResult run() {
        while (!eof()) {
            if (at_line_start && bracket_depth == 0) {
                if (skip_blank_or_comment_line()) continue;  // still at line start
                handle_line_start();
                at_line_start = false;  // '.' continuation lines lex as Op('.'))
                if (eof()) break;
            }
            if (at_line_start && bracket_depth > 0) at_line_start = false;

            char c = peek();
            if (c == '\n') {
                int ln = line;
                advance();
                if (bracket_depth == 0) {
                    push(Tok::Newline, ln, 1);
                    at_line_start = true;
                }
                continue;
            }
            if (c == ' ') { advance(); continue; }
            if (c == '#') {  // trailing comment: the newline still ends the line
                while (!eof() && peek() != '\n') advance();
                continue;
            }
            if (c == '"') { lex_string(); continue; }
            if (is_digit(c)) { lex_number(); continue; }
            if (ident_start(c)) { lex_ident(); continue; }

            bool known_op = c == '+' || c == '-' || c == '*' || c == '/' || c == '%' ||
                            c == '=' || c == '<' || c == '>' || c == '!' ||
                            c == '(' || c == ')' || c == '[' || c == ']' ||
                            c == '{' || c == '}' || c == ',' || c == ':' || c == '.';
            if (known_op) { lex_op(); continue; }

            error(std::string("unexpected character '") + c + "'");
            advance();
        }
        while (indents.size() > 1) {
            indents.pop_back();
            push(Tok::Dedent, line, 1);
        }
        push(Tok::EndOfFile, line, col);
        return std::move(out);
    }
};

} // namespace

LexResult lex(std::string_view source) {
    Lexer l;
    l.src = source;
    return l.run();
}

const char* to_string(Tok kind) {
    switch (kind) {
    case Tok::Ident:     return "Ident";
    case Tok::Int:       return "Int";
    case Tok::Float:     return "Float";
    case Tok::String:    return "String";
    case Tok::Keyword:   return "Keyword";
    case Tok::Op:        return "Op";
    case Tok::Newline:   return "Newline";
    case Tok::Indent:    return "Indent";
    case Tok::Dedent:    return "Dedent";
    case Tok::EndOfFile: return "EndOfFile";
    }
    return "?";
}

} // namespace luxium::lux
