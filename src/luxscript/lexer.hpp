#pragma once
// Lux Script lexer skeleton for Luxium (M0).
//
// Implements the lexical part of ../Lux/LUX_SCRIPT-GRAMMAR.md Part I: `#`
// comments, indentation blocks (SPACES ONLY -- a tab is an error, per §1),
// INDENT/DEDENT tokens, no Newline inside brackets, the '.' continuation
// line, and int/float/string literals including triple-quoted strings.
//
// This is a placeholder: M2 vendors Lux's own lexer/parser/VM into
// src/luxscript/ and deletes this file. It exists so the M0 pipeline (HTML →
// script extraction → lex) is testable before the vendoring.
//
// Token kinds are coarse on purpose (Keyword/Op carry their spelling in
// `text`); the vendored frontend replaces them with its own fine-grained set.

#include <string>
#include <vector>

namespace luxium::lux {

enum class Tok {
    Ident,     // user identifier (keywords are Tok::Keyword with their spelling)
    Int,       // integer literal
    Float,     // float literal (fraction and/or exponent)
    String,    // decoded string value
    Keyword,   // spelling in text
    Op,        // spelling in text
    Newline,   // logical end of line (suppressed inside brackets)
    Indent,    // block opens
    Dedent,    // block closes
    EndOfFile, // always last; pending Dedents emitted before it
};

struct Token {
    Tok         kind;
    std::string text;  // raw spelling, or the decoded value for String
    int         line = 0;
    int         col  = 0;  // 1-based
};

struct LexResult {
    std::vector<Token>        tokens;  // ends with EndOfFile even after errors
    std::vector<std::string>  errors;  // "line L: message"
};

LexResult lex(std::string_view source);

const char* to_string(Tok kind);

} // namespace luxium::lux
