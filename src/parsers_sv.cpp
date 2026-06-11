#include <rawast/parsers_sv.hpp>
#include <rawast/parsers_registry.hpp>
#include <rawast/stream.hpp>
#include <rawast/value.hpp>

#include <cctype>
#include <memory>
#include <string>

namespace rawast {

namespace {

// Identifier character classes per IEEE 1800-2017 §5.6.
bool is_simple_id_start(char c) {
    unsigned char uc = static_cast<unsigned char>(c);
    return std::isalpha(uc) || c == '_';
}
bool is_simple_id_cont(char c) {
    unsigned char uc = static_cast<unsigned char>(c);
    return std::isalnum(uc) || c == '_' || c == '$';
}

// "Whitespace" for terminating an escaped identifier per §5.6.1 is
// "any white space" — space, tab, newline, carriage return, form feed.
bool is_escaped_id_terminator(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

// Digit class predicates. Hex covers all base-specific bases — the
// SvBasedDigitsParser is permissive (validity per actual base is the
// host's concern); x/z/`?` are LRM extensions for "unknown" bits.
bool is_dec_digit(char c) { return c >= '0' && c <= '9'; }
bool is_hex_digit(char c) {
    return is_dec_digit(c)
        || (c >= 'a' && c <= 'f')
        || (c >= 'A' && c <= 'F');
}
bool is_xz_digit(char c) {
    return c == 'x' || c == 'X' || c == 'z' || c == 'Z' || c == '?';
}

} // namespace

// --- SvIdentifierParser -------------------------------------------------

SvIdentifierParser::SvIdentifierParser() : Parser("sv_identifier") {}

ParseResult SvIdentifierParser::parse(StreamReader& sr) {
    sr.mark();
    const Position start = sr.position();

    auto first = sr.peek();
    if (!first) {
        sr.reject();
        return tl::unexpected(ParseError{start, "expected identifier, got EOF"});
    }

    std::string out;

    // System task / function: `$<simple-identifier>`
    if (*first == '$') {
        out.push_back('$');
        sr.get();
        auto next = sr.peek();
        if (!next || !is_simple_id_start(*next)) {
            // Bare `$` is not a system name. The grammar likely wanted
            // a different token; reject so the engine can try alts.
            sr.reject();
            return tl::unexpected(ParseError{
                start,
                "expected identifier after '$' for system task/function name"});
        }
        while (auto c = sr.peek()) {
            if (!is_simple_id_cont(*c)) break;
            out.push_back(*c);
            sr.get();
        }
        sr.accept();
        return make_string(std::move(out));
    }

    // Escaped identifier: `\<chars-until-whitespace>`
    if (*first == '\\') {
        sr.get();   // consume the backslash; it's not part of the name
        bool any_chars = false;
        while (auto c = sr.peek()) {
            if (is_escaped_id_terminator(*c)) break;
            out.push_back(*c);
            sr.get();
            any_chars = true;
        }
        if (!any_chars) {
            sr.reject();
            return tl::unexpected(ParseError{
                start, "escaped identifier '\\' must be followed by name chars"});
        }
        // The terminating whitespace is NOT consumed — it's part of
        // the ignore set or a structural separator.
        sr.accept();
        return make_string(std::move(out));
    }

    // Simple identifier: `[a-zA-Z_][a-zA-Z_0-9$]*`
    if (!is_simple_id_start(*first)) {
        sr.reject();
        return tl::unexpected(ParseError{
            start, "expected identifier (letter, '_', '\\', or '$')"});
    }
    out.push_back(*first);
    sr.get();
    while (auto c = sr.peek()) {
        if (!is_simple_id_cont(*c)) break;
        out.push_back(*c);
        sr.get();
    }
    sr.accept();
    return make_string(std::move(out));
}

SaveResult SvIdentifierParser::unparse(const Value& value) const {
    auto sv = dynamic_cast<const StringValue*>(&value);
    if (!sv) {
        return tl::unexpected(SaveError{
            "SvIdentifierParser::unparse expects StringValue"});
    }
    const std::string& s = sv->data();
    if (s.empty()) {
        return tl::unexpected(SaveError{
            "SvIdentifierParser::unparse: empty identifier"});
    }
    // Already-prefixed system name: emit as-is.
    if (s[0] == '$') return s;
    // Detect whether it needs the escaped form: any char that isn't
    // a simple-id char triggers escape. Simple-id rule: first must be
    // letter/`_`, rest must be alnum/`_`/`$`.
    bool needs_escape = !is_simple_id_start(s[0]);
    if (!needs_escape) {
        for (std::size_t i = 1; i < s.size(); ++i) {
            if (!is_simple_id_cont(s[i])) { needs_escape = true; break; }
        }
    }
    if (needs_escape) {
        // Escaped form: `\<name><whitespace>` — emit with a trailing
        // space so the next token isn't part of the name. The caller
        // (the surrounding grammar's save) will follow with appropriate
        // structural content; this trailing space is structurally
        // necessary per §5.6.1.
        return "\\" + s + " ";
    }
    return s;
}

// --- SvBasedDigitsParser ------------------------------------------------
//
// Consumes the digit-run portion of a based literal: `FF` in `8'hFF`,
// `0101` in `4'b0101`, `xxxx` in `4'bxxxx`, `??` in `8'h??`. The
// alphabet is the union of all base-specific digits — the lexer is
// permissive per LRM. Validity per actual base (hex digit in a binary
// literal etc.) is the host's concern; we just emit the raw run.

SvBasedDigitsParser::SvBasedDigitsParser() : Parser("sv_based_digits") {}

ParseResult SvBasedDigitsParser::parse(StreamReader& sr) {
    sr.mark();
    const Position start = sr.position();
    std::string out;
    while (auto c = sr.peek()) {
        if (*c == '_') { sr.get(); continue; }   // strip underscores
        const bool is_digit =
            is_hex_digit(*c)                     // 0-9, a-f, A-F
            || is_xz_digit(*c);                  // x, X, z, Z, ?
        if (!is_digit) break;
        out.push_back(*c);
        sr.get();
    }
    if (out.empty()) {
        sr.reject();
        return tl::unexpected(ParseError{
            start, "expected based-number digits"});
    }
    sr.accept();
    return make_string(std::move(out));
}

SaveResult SvBasedDigitsParser::unparse(const Value& v) const {
    if (auto sv = dynamic_cast<const StringValue*>(&v)) return sv->data();
    return tl::unexpected(SaveError{
        "SvBasedDigitsParser::unparse expects StringValue"});
}

// --- SvBalancedArgParser ------------------------------------------------

SvBalancedArgParser::SvBalancedArgParser()
    : Parser("sv_balanced_arg") {}

ParseResult SvBalancedArgParser::parse(StreamReader& sr) {
    sr.mark();
    std::string out;
    int depth = 0;
    while (auto c = sr.peek()) {
        // At depth 0, `,` and `)` end this argument. Inside nested
        // `()` (depth > 0), they're regular content.
        if (depth == 0 && (*c == ',' || *c == ')')) break;
        if (*c == '(')      ++depth;
        else if (*c == ')') --depth;
        out.push_back(*c);
        sr.get();
    }
    sr.accept();
    return make_string(std::move(out));
}

SaveResult SvBalancedArgParser::unparse(const Value& v) const {
    if (auto sv = dynamic_cast<const StringValue*>(&v)) return sv->data();
    return tl::unexpected(SaveError{
        "SvBalancedArgParser::unparse expects StringValue"});
}

// --- SvBalancedBracesParser ---------------------------------------------

SvBalancedBracesParser::SvBalancedBracesParser()
    : Parser("sv_balanced_braces") {}

ParseResult SvBalancedBracesParser::parse(StreamReader& sr) {
    sr.mark();
    std::string out;
    // Track depth of all three bracket kinds. Stop at depth-0
    // closing `}` (left unconsumed for the next sibling Key to
    // match). Constraint blocks and class body literals are
    // commonly nested with mixed brackets — track all of them
    // so commas inside `{}` or `[]` stay part of the captured
    // text instead of terminating early.
    int brace_depth = 0;
    int paren_depth = 0;
    int brack_depth = 0;
    while (auto c = sr.peek()) {
        // At outermost level, a closing `}` ends the captured
        // section. Leave it unconsumed.
        if (brace_depth == 0 && paren_depth == 0
            && brack_depth == 0 && *c == '}') {
            break;
        }
        if      (*c == '{') ++brace_depth;
        else if (*c == '}') --brace_depth;
        else if (*c == '(') ++paren_depth;
        else if (*c == ')') --paren_depth;
        else if (*c == '[') ++brack_depth;
        else if (*c == ']') --brack_depth;
        out.push_back(*c);
        sr.get();
    }
    sr.accept();
    return make_string(std::move(out));
}

SaveResult SvBalancedBracesParser::unparse(const Value& v) const {
    if (auto sv = dynamic_cast<const StringValue*>(&v)) return sv->data();
    return tl::unexpected(SaveError{
        "SvBalancedBracesParser::unparse expects StringValue"});
}

// --- SvBalancedBracketsParser -------------------------------------------

SvBalancedBracketsParser::SvBalancedBracketsParser()
    : Parser("sv_balanced_brackets") {}

ParseResult SvBalancedBracketsParser::parse(StreamReader& sr) {
    sr.mark();
    std::string out;
    // Track `[]`, `()`, and `{}` depth. Stop at `]` at outermost
    // depth. Used for array index suffixes like `[bit[31:0]]` where
    // the inner bracket is part of the type expression, not the
    // outer suffix terminator.
    int brack_depth = 0;
    int paren_depth = 0;
    int brace_depth = 0;
    while (auto c = sr.peek()) {
        if (brack_depth == 0 && paren_depth == 0
            && brace_depth == 0 && *c == ']') {
            break;
        }
        if      (*c == '[') ++brack_depth;
        else if (*c == ']') --brack_depth;
        else if (*c == '(') ++paren_depth;
        else if (*c == ')') --paren_depth;
        else if (*c == '{') ++brace_depth;
        else if (*c == '}') --brace_depth;
        out.push_back(*c);
        sr.get();
    }
    sr.accept();
    return make_string(std::move(out));
}

SaveResult SvBalancedBracketsParser::unparse(const Value& v) const {
    if (auto sv = dynamic_cast<const StringValue*>(&v)) return sv->data();
    return tl::unexpected(SaveError{
        "SvBalancedBracketsParser::unparse expects StringValue"});
}

// --- SvLineTextParser ---------------------------------------------------

SvLineTextParser::SvLineTextParser() : Parser("sv_line_text") {}

ParseResult SvLineTextParser::parse(StreamReader& sr) {
    sr.mark();
    std::string out;
    bool prev_backslash = false;
    while (auto c = sr.peek()) {
        if (*c == '\n') {
            if (prev_backslash) {
                // Line continuation: include the newline, keep going.
                out.push_back(*c);
                sr.get();
                prev_backslash = false;
                continue;
            }
            // Real end of line — leave the newline unconsumed so the
            // outer rule's ignore policy can handle it on the next
            // iteration.
            break;
        }
        prev_backslash = (*c == '\\');
        out.push_back(*c);
        sr.get();
    }
    sr.accept();
    // Trim trailing horizontal whitespace. Don't trim newlines kept
    // from line continuations — those carry semantic structure.
    while (!out.empty()
           && (out.back() == ' ' || out.back() == '\t')) {
        out.pop_back();
    }
    return make_string(std::move(out));
}

SaveResult SvLineTextParser::unparse(const Value& v) const {
    if (auto sv = dynamic_cast<const StringValue*>(&v)) return sv->data();
    return tl::unexpected(SaveError{
        "SvLineTextParser::unparse expects StringValue"});
}

// --- Group registration -------------------------------------------------

namespace {

ParserGroup make_sv_group() {
    ParserGroup g;
    g.name = "sv";
    g.parsers = {
        // Only the SystemVerilog-specific terminals — strings and
        // comments use std.string / std.line_comment / std.block_comment;
        // plain integers and reals use std.int / std.float; based-
        // number digit runs use sv_based_digits + grammar composition.
        ParserSpec{"sv_identifier", []() {
            return std::make_unique<SvIdentifierParser>();
        }},
        ParserSpec{"sv_based_digits", []() {
            return std::make_unique<SvBasedDigitsParser>();
        }},
        ParserSpec{"sv_line_text", []() {
            return std::make_unique<SvLineTextParser>();
        }},
        ParserSpec{"sv_balanced_arg", []() {
            return std::make_unique<SvBalancedArgParser>();
        }},
        ParserSpec{"sv_balanced_braces", []() {
            return std::make_unique<SvBalancedBracesParser>();
        }},
        ParserSpec{"sv_balanced_brackets", []() {
            return std::make_unique<SvBalancedBracketsParser>();
        }},
    };
    return g;
}

} // namespace

void register_sv_parser_group() {
    register_parser_group(make_sv_group());
}

} // namespace rawast
