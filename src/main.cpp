// luxium -- M0 skeleton CLI.
//
//   luxium <page.html> [--html-tokens] [--quiet]
//
// Reads an HTML file, builds the DOM, dumps the tree, and lexes every
// <script> block with the Lux Script lexer skeleton, printing tokens and
// diagnostics. This is the test harness for the two M0 pipelines; the shell
// window replaces it in M1.

#include "dom/node.hpp"
#include "html/parser.hpp"
#include "html/tokenizer.hpp"
#include "luxscript/lexer.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace html = luxium::html;
namespace dom  = luxium::dom;
namespace lux  = luxium::lux;

namespace {

std::string read_file(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void print_usage() {
    std::fprintf(stderr,
                 "usage: luxium <page.html> [--html-tokens] [--quiet]\n"
                 "  --html-tokens  also dump the HTML token stream\n"
                 "  --quiet        DOM tree only (no script tokens)\n");
}

void print_html_tokens(const std::vector<html::Token>& tokens) {
    std::printf("== HTML tokens (%zu) ==\n", tokens.size());
    for (const auto& t : tokens) {
        const char* kind = "?";
        switch (t.type) {
        case html::TokType::Doctype:  kind = "Doctype "; break;
        case html::TokType::StartTag: kind = "StartTag"; break;
        case html::TokType::EndTag:   kind = "EndTag  "; break;
        case html::TokType::Comment:  kind = "Comment "; break;
        case html::TokType::Text:     kind = "Text    "; break;
        }
        std::string head = t.type == html::TokType::Text ||
                                   t.type == html::TokType::Comment ||
                                   t.type == html::TokType::Doctype
                               ? t.text.substr(0, 40)
                               : t.name;
        std::printf("  L%-4d %s %s%s\n", t.line, kind, head.c_str(),
                    t.self_closing ? " (self-closing)" : "");
        for (const auto& [k, v] : t.attrs)
            std::printf("          %s = \"%s\"\n", k.c_str(), v.c_str());
    }
    std::printf("\n");
}

void print_script_tokens(const dom::Node& script, size_t index) {
    std::string src = script.text_content();
    std::printf("== <script> #%zu (line %d) -- LuxScript ==\n", index, script.line);
    if (script.has_attr("src")) {
        std::printf("   external src=\"%s\" (loading lands in M2 -- skipped)\n\n",
                    script.attr_or("src").c_str());
        return;
    }
    if (script.attr_or("type") == "text/javascript" || script.attr_or("type") == "application/javascript") {
        std::printf("   type=javascript: Luxium does not run JavaScript.\n\n");
        return;
    }

    lux::LexResult lx = lux::lex(src);
    for (const auto& tok : lx.tokens) {
        if (tok.kind == lux::Tok::EndOfFile) break;
        std::string shown = tok.text;
        // one-line tokens can hold newlines (triple-quoted strings)
        for (char& c : shown)
            if (c == '\n') c = ' ';
        std::printf("  L%-4d C%-4d %-9s %s\n", tok.line, tok.col, lux::to_string(tok.kind),
                    shown.c_str());
    }
    for (const auto& e : lx.errors) std::printf("  error: %s\n", e.c_str());
    std::printf("  (%zu tokens, %zu errors)\n\n", lx.tokens.size() - 1, lx.errors.size());
}

} // namespace

int main(int argc, char** argv) {
    const char* path = nullptr;
    bool html_tokens = false, quiet = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--html-tokens") html_tokens = true;
        else if (arg == "--quiet") quiet = true;
        else if (arg == "-h" || arg == "--help") { print_usage(); return 0; }
        else if (!path) path = argv[i];
        else { print_usage(); return 2; }
    }
    if (!path) { print_usage(); return 2; }

    std::string source = read_file(path);
    if (source.empty() && !std::ifstream(path)) {
        std::fprintf(stderr, "luxium: cannot read '%s'\n", path);
        return 1;
    }

    if (html_tokens) print_html_tokens(html::tokenize(source));

    html::ParseResult page = html::parse(source);
    for (const auto& e : page.errors)
        std::fprintf(stderr, "html: %s\n", e.c_str());

    page.root->print();
    std::printf("\n");

    if (!quiet) {
        std::vector<dom::Node*> scripts = page.root->find_all("script");
        for (size_t i = 0; i < scripts.size(); ++i) print_script_tokens(*scripts[i], i + 1);
    }

    std::fflush(stdout);
    return 0;
}
