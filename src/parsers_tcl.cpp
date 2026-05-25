#include <rawast/parsers_tcl.hpp>
#include <rawast/parsers_registry.hpp>
#include <rawast/stream.hpp>
#include <rawast/value.hpp>

#include <cctype>
#include <memory>
#include <string>

namespace rawast {

namespace {

// Identifier-like chars for the bare form of variable names — Tcl allows
// namespace-qualified names like `ns::var`, so `:` is included.
bool is_var_name_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == ':';
}

// Bare-word stop chars (Dodekalogue rule 3 word separators + the
// structural delimiters that start other word flavours).
bool is_bare_word_stop(char c) {
    switch (c) {
    case ' ': case '\t': case '\n': case '\r':
    case ';': case '[': case ']': case '{': case '}': case '"':
        return true;
    default:
        return false;
    }
}

// Word-internals stop chars: `$` (variable substitution), `[` (command
// substitution), `\` (escape), `"` (closing quote — only relevant inside
// quoted strings; harmless for bare words because they can't contain `"`).
bool is_literal_run_stop(char c) {
    switch (c) {
    case '$': case '[': case '\\': case '"':
        return true;
    default:
        return false;
    }
}

} // namespace

// --- TclHspaceParser ----------------------------------------------------

TclHspaceParser::TclHspaceParser() : Parser("hspace") {}

ParseResult TclHspaceParser::parse(StreamReader& sr) {
    sr.mark();
    const Position start = sr.position();

    std::size_t consumed = 0;
    while (true) {
        auto c = sr.peek();
        if (!c) break;

        if (*c == ' ' || *c == '\t') {
            sr.get();
            ++consumed;
            continue;
        }
        // Line continuation: `\` immediately followed by `\n`. Consume
        // both as one unit. If `\` is followed by anything else (or
        // EOF), do NOT consume — it's an escape sequence inside a word,
        // not whitespace.
        if (*c == '\\') {
            sr.mark();
            sr.get();   // consume `\`
            auto next = sr.peek();
            if (next && *next == '\n') {
                sr.get();
                sr.accept();
                consumed += 2;
                continue;
            }
            sr.reject();   // rewind the lone `\`
            break;
        }
        break;
    }

    if (consumed == 0) {
        sr.reject();
        return tl::unexpected(ParseError{start, "expected horizontal whitespace"});
    }
    sr.accept();
    return null_value();   // ignore-list parsers don't produce values
}

// --- TclNewlineParser ---------------------------------------------------

TclNewlineParser::TclNewlineParser() : Parser("newline") {}

ParseResult TclNewlineParser::parse(StreamReader& sr) {
    sr.mark();
    const Position start = sr.position();
    auto c = sr.peek();
    if (!c || *c != '\n') {
        sr.reject();
        return tl::unexpected(ParseError{start, "expected newline"});
    }
    sr.get();
    sr.accept();
    return null_value();
}

// --- TclCommentParser ---------------------------------------------------

TclCommentParser::TclCommentParser() : Parser("comment") {}

ParseResult TclCommentParser::parse(StreamReader& sr) {
    sr.mark();
    const Position start = sr.position();
    auto first = sr.peek();
    if (!first || *first != '#') {
        sr.reject();
        return tl::unexpected(ParseError{start, "expected '#' comment"});
    }
    sr.get();   // consume `#`

    std::string body;
    while (true) {
        auto c = sr.peek();
        if (!c) break;          // EOF ends the comment
        if (*c == '\n') break;  // newline is the structural terminator;
                                // leave it for tcl.newline to consume
        body.push_back(*c);
        sr.get();
    }
    sr.accept();
    return make_string(std::move(body));
}

// --- TclBraceGroupParser ------------------------------------------------

TclBraceGroupParser::TclBraceGroupParser() : Parser("brace_group") {}

ParseResult TclBraceGroupParser::parse(StreamReader& sr) {
    sr.mark();
    const Position start = sr.position();
    auto first = sr.peek();
    if (!first || *first != '{') {
        sr.reject();
        return tl::unexpected(ParseError{start, "expected '{'"});
    }
    sr.get();   // consume opening `{`

    std::string content;
    int depth = 1;
    while (true) {
        auto c = sr.peek();
        if (!c) {
            sr.reject();
            return tl::unexpected(ParseError{start, "unterminated brace group"});
        }
        char ch = *c;
        if (ch == '\\') {
            // Escape: consume `\` + next byte verbatim; depth unchanged.
            sr.get();
            content.push_back('\\');
            auto next = sr.get();
            if (!next) {
                sr.reject();
                return tl::unexpected(ParseError{
                    start, "unterminated escape in brace group"});
            }
            content.push_back(*next);
            continue;
        }
        if (ch == '{') {
            ++depth;
            content.push_back(ch);
            sr.get();
            continue;
        }
        if (ch == '}') {
            --depth;
            sr.get();
            if (depth == 0) {
                sr.accept();
                return make_string(std::move(content));
            }
            content.push_back(ch);
            continue;
        }
        content.push_back(ch);
        sr.get();
    }
}

// --- TclQuotedStringParser ---------------------------------------------

TclQuotedStringParser::TclQuotedStringParser() : Parser("quoted_string") {}

ParseResult TclQuotedStringParser::parse(StreamReader& sr) {
    sr.mark();
    const Position start = sr.position();
    auto first = sr.peek();
    if (!first || *first != '"') {
        sr.reject();
        return tl::unexpected(ParseError{start, "expected '\"'"});
    }
    sr.get();   // consume opening `"`

    std::string content;
    while (true) {
        auto c = sr.peek();
        if (!c) {
            sr.reject();
            return tl::unexpected(ParseError{start, "unterminated quoted string"});
        }
        char ch = *c;
        if (ch == '\\') {
            sr.get();
            content.push_back('\\');
            auto next = sr.get();
            if (!next) {
                sr.reject();
                return tl::unexpected(ParseError{
                    start, "unterminated escape in quoted string"});
            }
            content.push_back(*next);
            continue;
        }
        if (ch == '"') {
            sr.get();
            sr.accept();
            return make_string(std::move(content));
        }
        content.push_back(ch);
        sr.get();
    }
}

// --- TclBracketSubParser -----------------------------------------------

TclBracketSubParser::TclBracketSubParser() : Parser("bracket_sub") {}

ParseResult TclBracketSubParser::parse(StreamReader& sr) {
    sr.mark();
    const Position start = sr.position();
    auto first = sr.peek();
    if (!first || *first != '[') {
        sr.reject();
        return tl::unexpected(ParseError{start, "expected '['"});
    }
    sr.get();   // consume opening `[`

    std::string content;
    int bracket_depth = 1;
    int brace_depth   = 0;
    bool in_string    = false;

    while (true) {
        auto c = sr.peek();
        if (!c) {
            sr.reject();
            return tl::unexpected(ParseError{
                start, "unterminated [...] command substitution"});
        }
        char ch = *c;

        // Escapes apply in all sub-contexts (Tcl rule 9).
        if (ch == '\\') {
            sr.get();
            content.push_back('\\');
            auto next = sr.get();
            if (!next) {
                sr.reject();
                return tl::unexpected(ParseError{
                    start, "unterminated escape in bracket-sub"});
            }
            content.push_back(*next);
            continue;
        }

        if (in_string) {
            content.push_back(ch);
            sr.get();
            if (ch == '"') in_string = false;
            continue;
        }
        if (brace_depth > 0) {
            content.push_back(ch);
            sr.get();
            if (ch == '{') ++brace_depth;
            else if (ch == '}') --brace_depth;
            continue;
        }

        // Normal bracket-sub context.
        if (ch == '"') {
            in_string = true;
            content.push_back(ch);
            sr.get();
            continue;
        }
        if (ch == '{') {
            brace_depth = 1;
            content.push_back(ch);
            sr.get();
            continue;
        }
        if (ch == '[') {
            ++bracket_depth;
            content.push_back(ch);
            sr.get();
            continue;
        }
        if (ch == ']') {
            --bracket_depth;
            sr.get();
            if (bracket_depth == 0) {
                sr.accept();
                return make_string(std::move(content));
            }
            content.push_back(ch);
            continue;
        }
        content.push_back(ch);
        sr.get();
    }
}

// --- TclBareWordParser -------------------------------------------------

TclBareWordParser::TclBareWordParser() : Parser("bare_word") {}

ParseResult TclBareWordParser::parse(StreamReader& sr) {
    sr.mark();
    const Position start = sr.position();
    std::string word;
    while (true) {
        auto c = sr.peek();
        if (!c || is_bare_word_stop(*c)) break;
        word.push_back(*c);
        sr.get();
    }
    if (word.empty()) {
        sr.reject();
        return tl::unexpected(ParseError{start, "expected bare word"});
    }
    sr.accept();
    return make_string(std::move(word));
}

// --- TclExpandMarkerParser ---------------------------------------------

TclExpandMarkerParser::TclExpandMarkerParser() : Parser("expand_marker") {}

ParseResult TclExpandMarkerParser::parse(StreamReader& sr) {
    sr.mark();
    const Position start = sr.position();

    auto c1 = sr.get();
    auto c2 = sr.get();
    auto c3 = sr.get();
    if (!c1 || !c2 || !c3 || *c1 != '{' || *c2 != '*' || *c3 != '}') {
        sr.reject();
        return tl::unexpected(ParseError{start, "expected '{*}'"});
    }
    // Must be followed by a non-whitespace, non-special character — `{*}`
    // alone (e.g. as a list element) is just an ordinary brace word.
    auto next = sr.peek();
    if (!next || is_bare_word_stop(*next)) {
        sr.reject();
        return tl::unexpected(ParseError{
            start, "'{*}' must be followed by a non-whitespace character"});
    }
    sr.accept();
    return null_value();
}

// --- TclVarNameParser --------------------------------------------------

TclVarNameParser::TclVarNameParser() : Parser("var_name") {}

ParseResult TclVarNameParser::parse(StreamReader& sr) {
    sr.mark();
    const Position start = sr.position();

    auto first = sr.peek();
    if (!first) {
        sr.reject();
        return tl::unexpected(ParseError{start, "expected variable name"});
    }

    if (*first == '{') {
        // Braced form: ${name} — name can contain anything except `}`.
        // No nesting (per Tcl spec).
        sr.get();   // consume `{`
        std::string name;
        while (true) {
            auto c = sr.peek();
            if (!c) {
                sr.reject();
                return tl::unexpected(ParseError{
                    start, "unterminated ${...} variable reference"});
            }
            if (*c == '}') {
                sr.get();
                sr.accept();
                return make_string(std::move(name));
            }
            name.push_back(*c);
            sr.get();
        }
    }

    // Bare form: [A-Za-z0-9_:]+
    std::string name;
    while (true) {
        auto c = sr.peek();
        if (!c || !is_var_name_char(*c)) break;
        name.push_back(*c);
        sr.get();
    }
    if (name.empty()) {
        sr.reject();
        return tl::unexpected(ParseError{start, "expected variable name"});
    }
    sr.accept();
    return make_string(std::move(name));
}

// --- TclUntilParenParser -----------------------------------------------

TclUntilParenParser::TclUntilParenParser() : Parser("until_paren") {}

ParseResult TclUntilParenParser::parse(StreamReader& sr) {
    sr.mark();
    const Position start = sr.position();
    std::string content;
    while (true) {
        auto c = sr.peek();
        if (!c) {
            sr.reject();
            return tl::unexpected(ParseError{
                start, "unterminated array index (expected ')')"});
        }
        if (*c == ')') break;
        content.push_back(*c);
        sr.get();
    }
    sr.accept();
    return make_string(std::move(content));
}

// --- TclEscapeParser ---------------------------------------------------

TclEscapeParser::TclEscapeParser() : Parser("escape") {}

ParseResult TclEscapeParser::parse(StreamReader& sr) {
    sr.mark();
    const Position start = sr.position();
    auto c1 = sr.peek();
    if (!c1 || *c1 != '\\') {
        sr.reject();
        return tl::unexpected(ParseError{start, "expected '\\'"});
    }
    sr.get();   // consume `\`
    auto c2 = sr.get();
    if (!c2) {
        sr.reject();
        return tl::unexpected(ParseError{
            start, "unterminated escape sequence at end of input"});
    }
    std::string esc;
    esc.push_back('\\');
    esc.push_back(*c2);
    sr.accept();
    return make_string(std::move(esc));
}

// --- TclLiteralRunParser -----------------------------------------------

TclLiteralRunParser::TclLiteralRunParser() : Parser("literal_run") {}

ParseResult TclLiteralRunParser::parse(StreamReader& sr) {
    sr.mark();
    const Position start = sr.position();
    std::string run;
    while (true) {
        auto c = sr.peek();
        if (!c || is_literal_run_stop(*c)) break;
        run.push_back(*c);
        sr.get();
    }
    if (run.empty()) {
        sr.reject();
        return tl::unexpected(ParseError{start, "expected literal run"});
    }
    sr.accept();
    return make_string(std::move(run));
}

// --- Group registration ------------------------------------------------

namespace {

ParserGroup make_tcl_group() {
    ParserGroup g;
    g.name = "tcl";
    g.parsers = {
        ParserSpec{"hspace",        []{ return std::make_unique<TclHspaceParser>(); }},
        ParserSpec{"newline",       []{ return std::make_unique<TclNewlineParser>(); }},
        ParserSpec{"comment",       []{ return std::make_unique<TclCommentParser>(); }},
        ParserSpec{"brace_group",   []{ return std::make_unique<TclBraceGroupParser>(); }},
        ParserSpec{"quoted_string", []{ return std::make_unique<TclQuotedStringParser>(); }},
        ParserSpec{"bracket_sub",   []{ return std::make_unique<TclBracketSubParser>(); }},
        ParserSpec{"bare_word",     []{ return std::make_unique<TclBareWordParser>(); }},
        ParserSpec{"expand_marker", []{ return std::make_unique<TclExpandMarkerParser>(); }},
        ParserSpec{"var_name",      []{ return std::make_unique<TclVarNameParser>(); }},
        ParserSpec{"until_paren",   []{ return std::make_unique<TclUntilParenParser>(); }},
        ParserSpec{"escape",        []{ return std::make_unique<TclEscapeParser>(); }},
        ParserSpec{"literal_run",   []{ return std::make_unique<TclLiteralRunParser>(); }},
    };
    return g;
}

} // namespace

void register_tcl_parser_group() {
    register_parser_group(make_tcl_group());
}

} // namespace rawast
