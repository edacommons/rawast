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

// Helper for the unparse half: pull a StringValue out of a Value or
// return a SaveError keyed by the calling terminal's name.
SaveResult expect_string(const Value& v, const std::string& who) {
    auto sv = dynamic_cast<const StringValue*>(&v);
    if (!sv) {
        return tl::unexpected(SaveError{
            "tcl." + who + "::unparse expects StringValue"});
    }
    return sv->data();
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

SaveResult TclNewlineParser::unparse(const Value& /*v*/) const {
    // Structural separator with no payload. Emit "\n" unconditionally —
    // the parse side returned null_value, so nothing is bound from the
    // AST; save just emits the byte the parser would have consumed.
    return std::string{"\n"};
}

// --- TclCommandSepParser ------------------------------------------------

TclCommandSepParser::TclCommandSepParser() : Parser("command_sep") {}

ParseResult TclCommandSepParser::parse(StreamReader& sr) {
    sr.mark();
    const Position start = sr.position();
    auto c = sr.peek();
    if (!c || (*c != '\n' && *c != ';')) {
        sr.reject();
        return tl::unexpected(ParseError{start, "expected '\\n' or ';'"});
    }
    sr.get();
    sr.accept();
    return null_value();
}

SaveResult TclCommandSepParser::unparse(const Value& /*v*/) const {
    // Canonicalize to "\n" on save. The AST doesn't track which form
    // was used at parse time; save picks the most common.
    return std::string{"\n"};
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

SaveResult TclCommentParser::unparse(const Value& v) const {
    // Parse stores the body without the leading `#` and without the
    // trailing newline (the newline is left for tcl.newline to consume
    // via SEP). Save mirrors that: emit `#` + body. The structural
    // newline comes back from the surrounding SEP_RUN.
    auto body = expect_string(v, "comment");
    if (!body) return body;
    return "#" + *body;
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

SaveResult TclBraceGroupParser::unparse(const Value& v) const {
    // Parse stores the body between the outer braces with `\{`/`\}`
    // escapes preserved verbatim. Save just wraps it back up.
    auto body = expect_string(v, "brace_group");
    if (!body) return body;
    return "{" + *body + "}";
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

SaveResult TclQuotedStringParser::unparse(const Value& v) const {
    // Parse stores the body between `"..."` with backslash escapes
    // preserved verbatim. When the binding carries #subparse, the
    // engine first re-serialises the sub-tree through that rule
    // (save_stack.cpp:1191) and hands the resulting bytes here as
    // a StringValue, so this code path is identical in both cases.
    auto body = expect_string(v, "quoted_string");
    if (!body) return body;
    return "\"" + *body + "\"";
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

SaveResult TclBracketSubParser::unparse(const Value& v) const {
    // Parse stores the body between `[...]`. Same subparse re-entry
    // contract as TclQuotedStringParser::unparse — the engine collapses
    // the nested SCRIPT sub-tree to bytes before calling this.
    auto body = expect_string(v, "bracket_sub");
    if (!body) return body;
    return "[" + *body + "]";
}

// --- TclBareWordParser -------------------------------------------------

TclBareWordParser::TclBareWordParser() : Parser("bare_word") {}

ParseResult TclBareWordParser::parse(StreamReader& sr) {
    sr.mark();
    const Position start = sr.position();
    std::string word;
    while (true) {
        auto c = sr.peek();
        if (!c) break;
        // Backslash escape: per Dodekalogue rule 9, `\<X>` inside a
        // word is interpreted at evaluation time but at parse time
        // both bytes are part of the word, regardless of whether X
        // would otherwise be a word-boundary char. Without this,
        // `MACC_\{CONFIG\}` would split at the unescaped-looking
        // `{` boundary. WORD_SEGMENTS re-parses captured content and
        // ESCAPE_SEG handles the `\X` form.
        if (*c == '\\') {
            sr.get();           // consume `\`
            auto next = sr.peek();
            if (!next) {
                // Trailing backslash at EOF — preserve as part of
                // the word and let WORD_SEGMENTS raise the error
                // there if it matters.
                word.push_back('\\');
                break;
            }
            word.push_back('\\');
            word.push_back(*next);
            sr.get();           // consume the escaped byte
            continue;
        }
        if (is_bare_word_stop(*c)) break;
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

SaveResult TclBareWordParser::unparse(const Value& v) const {
    // No delimiters: bare word is raw bytes verbatim. Same subparse
    // contract as the bracketed variants — by the time we get here the
    // sub-tree (if any) has already been collapsed.
    return expect_string(v, "bare_word");
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

SaveResult TclExpandMarkerParser::unparse(const Value& /*v*/) const {
    // Structural marker; AST carries `expand=true` on EXPAND_WORD as
    // the actual flag. Emit the canonical "{*}" so the next byte
    // (a non-whitespace word start) re-triggers the parser on
    // re-parse.
    return std::string{"{*}"};
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

SaveResult TclVarNameParser::unparse(const Value& v) const {
    // Pick bare or braced form based on the name bytes alone. Bare
    // form (e.g. `$x`) re-parses iff every byte is a valid bare var
    // name char and the name is non-empty. Braced form (e.g. `${x}`)
    // works for any other name (spaces, sigils, empty).
    //
    // Known limitation: a bare-safe name immediately followed by
    // another bare var-name char in the next segment (e.g. the AST
    // pattern var=`x` then literal `_suffix`, originally written
    // `${x}_suffix`) re-parses as the single var name `x_suffix`.
    // We can't detect this from inside unparse — context isn't
    // available. Always-braced would fix this case but breaks parse:
    // BARE_WORD stops at `{`, so `${x}` outside an obvious var
    // context gets parsed as `$` + `{x}` (DOLLAR_LITERAL + BRACE_WORD)
    // instead of VAR_SEG. The bare-by-default policy wins on the
    // corpus.
    auto body = expect_string(v, "var_name");
    if (!body) return body;
    bool bare_safe = !body->empty();
    for (char c : *body) {
        if (!is_var_name_char(c)) { bare_safe = false; break; }
    }
    return bare_safe ? *body : ("{" + *body + "}");
}

// (TclUntilParenParser retired — superseded by the engine's `*`
// raw-consume primitive. See header note.)

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

SaveResult TclEscapeParser::unparse(const Value& v) const {
    // Parse stores the two-byte sequence `\` + char verbatim. Save
    // emits the stored bytes as-is; no extra delimiters needed.
    return expect_string(v, "escape");
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

SaveResult TclLiteralRunParser::unparse(const Value& v) const {
    // Verbatim literal run between substitution markers — emit bytes
    // as stored. Parse already excluded the structural chars (`$`,
    // `[`, `\`, `"`), so the body can't accidentally re-trigger them
    // on a round-trip.
    return expect_string(v, "literal_run");
}

// --- Group registration ------------------------------------------------

namespace {

ParserGroup make_tcl_group() {
    ParserGroup g;
    g.name = "tcl";
    g.parsers = {
        ParserSpec{"hspace",        []{ return std::make_unique<TclHspaceParser>(); }},
        ParserSpec{"newline",       []{ return std::make_unique<TclNewlineParser>(); }},
        ParserSpec{"command_sep",   []{ return std::make_unique<TclCommandSepParser>(); }},
        ParserSpec{"comment",       []{ return std::make_unique<TclCommentParser>(); }},
        ParserSpec{"brace_group",   []{ return std::make_unique<TclBraceGroupParser>(); }},
        ParserSpec{"quoted_string", []{ return std::make_unique<TclQuotedStringParser>(); }},
        ParserSpec{"bracket_sub",   []{ return std::make_unique<TclBracketSubParser>(); }},
        ParserSpec{"bare_word",     []{ return std::make_unique<TclBareWordParser>(); }},
        ParserSpec{"expand_marker", []{ return std::make_unique<TclExpandMarkerParser>(); }},
        ParserSpec{"var_name",      []{ return std::make_unique<TclVarNameParser>(); }},
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
