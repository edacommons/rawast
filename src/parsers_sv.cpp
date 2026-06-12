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

// --- SvPpTextLineParser -------------------------------------------------

SvPpTextLineParser::SvPpTextLineParser() : Parser("sv_pp_text_line") {}

ParseResult SvPpTextLineParser::parse(StreamReader& sr) {
    sr.mark();

    // EOF is a failure — let the caller's repeat detect end-of-input
    // through some other channel rather than this parser returning
    // empty strings forever.
    auto first = sr.peek();
    if (!first) {
        sr.reject();
        return tl::unexpected(ParseError{
            sr.position(), "sv_pp_text_line: at EOF"});
    }

    // Terminator-directive lookahead. Only triggered when the cursor
    // is at `\``; cheap fall-through for plain text lines.
    if (*first == '`') {
        sr.mark();
        sr.get();  // consume the backtick
        std::string kw;
        while (auto c = sr.peek()) {
            unsigned char uc = static_cast<unsigned char>(*c);
            if (!std::isalpha(uc)) break;
            kw.push_back(*c);
            sr.get();
        }
        // Word boundary: next byte must not extend an identifier (and
        // EOF qualifies). Identifier-continuation bytes are letters,
        // digits, `_`, `$` per IEEE 1800-2017 §5.6.
        bool at_boundary = true;
        if (auto c = sr.peek()) {
            unsigned char uc = static_cast<unsigned char>(*c);
            if (std::isalnum(uc) || *c == '_' || *c == '$') {
                at_boundary = false;
            }
        }
        sr.reject();   // unwind the lookahead — caller's mark still live

        if (at_boundary && (kw == "endif" || kw == "else")) {
            sr.reject();
            return tl::unexpected(ParseError{
                sr.position(),
                "sv_pp_text_line: at \`" + kw + " terminator"});
        }
    }

    // Consume up to and including the next newline (or to EOF if no
    // more newlines remain in the stream).
    std::string out;
    while (auto c = sr.peek()) {
        out.push_back(*c);
        sr.get();
        if (*c == '\n') break;
    }
    sr.accept();
    return make_string(std::move(out));
}

SaveResult SvPpTextLineParser::unparse(const Value& v) const {
    if (auto sv = dynamic_cast<const StringValue*>(&v)) return sv->data();
    return tl::unexpected(SaveError{
        "SvPpTextLineParser::unparse expects StringValue"});
}

// --- SvEolParser --------------------------------------------------------

SvEolParser::SvEolParser() : Parser("sv_eol") {}

ParseResult SvEolParser::parse(StreamReader& sr) {
    sr.mark();

    // Tolerate a trailing line comment on the same line — `\`endif`,
    // `\`define`, `\`else` etc. are frequently followed by `// note`.
    // Spaces/tabs before the comment, the comment itself (up to but
    // not including the newline), then the actual newline.
    std::string out;
    while (auto c = sr.peek()) {
        if (*c != ' ' && *c != '\t') break;
        sr.get();
    }
    auto first = sr.peek();
    if (!first) {
        sr.reject();
        return tl::unexpected(ParseError{
            sr.position(), "sv_eol: at EOF"});
    }
    // Optional `// …` line comment — consume up to (but not
    // including) the next newline.
    if (*first == '/' ) {
        sr.mark();
        sr.get();
        auto next = sr.peek();
        if (next && *next == '/') {
            sr.get();
            while (auto c = sr.peek()) {
                if (*c == '\n' || *c == '\r') break;
                sr.get();
            }
            sr.accept();
            first = sr.peek();
        } else {
            // Not a line comment after all — rewind the lookahead.
            sr.reject();
            first = sr.peek();
        }
    }
    if (!first) {
        sr.reject();
        return tl::unexpected(ParseError{
            sr.position(), "sv_eol: at EOF"});
    }
    if (*first == '\r') {
        out.push_back(*first);
        sr.get();
        if (auto next = sr.peek(); next && *next == '\n') {
            out.push_back(*next);
            sr.get();
        }
        sr.accept();
        return make_string(std::move(out));
    }
    if (*first == '\n') {
        out.push_back(*first);
        sr.get();
        sr.accept();
        return make_string(std::move(out));
    }
    sr.reject();
    return tl::unexpected(ParseError{
        sr.position(), "sv_eol: expected newline"});
}

SaveResult SvEolParser::unparse(const Value& v) const {
    if (auto sv = dynamic_cast<const StringValue*>(&v)) return sv->data();
    return tl::unexpected(SaveError{
        "SvEolParser::unparse expects StringValue"});
}

// --- SvPpMacroNameParser ------------------------------------------------

SvPpMacroNameParser::SvPpMacroNameParser() : Parser("sv_pp_macro_name") {}

ParseResult SvPpMacroNameParser::parse(StreamReader& sr) {
    sr.mark();
    auto first = sr.peek();
    if (!first || !is_simple_id_start(*first)) {
        sr.reject();
        return tl::unexpected(ParseError{
            sr.position(), "sv_pp_macro_name: expected identifier start"});
    }
    std::string name;
    name.push_back(*first);
    sr.get();
    while (auto c = sr.peek()) {
        if (!is_simple_id_cont(*c)) break;
        name.push_back(*c);
        sr.get();
    }
    // Preprocessor terminator keywords — refuse to match them as
    // a macro name so the enclosing `\`ifdef`/`\`ifndef` rule can
    // claim the `\`endif` / `\`else` directive instead.
    if (name == "endif" || name == "else") {
        sr.reject();
        return tl::unexpected(ParseError{
            sr.position(),
            "sv_pp_macro_name: \`" + name + " is a terminator, not a macro"});
    }
    sr.accept();
    return make_string(std::move(name));
}

SaveResult SvPpMacroNameParser::unparse(const Value& v) const {
    if (auto sv = dynamic_cast<const StringValue*>(&v)) return sv->data();
    return tl::unexpected(SaveError{
        "SvPpMacroNameParser::unparse expects StringValue"});
}

// --- SvSystemNameParser -------------------------------------------------

SvSystemNameParser::SvSystemNameParser() : Parser("sv_system_name") {}

ParseResult SvSystemNameParser::parse(StreamReader& sr) {
    sr.mark();
    auto first = sr.peek();
    if (!first || *first != '$') {
        sr.reject();
        return tl::unexpected(ParseError{
            sr.position(), "sv_system_name: expected '$'"});
    }
    std::string out;
    out.push_back(*first);
    sr.get();
    // First identifier segment is required.
    auto next = sr.peek();
    if (!next || !is_simple_id_start(*next)) {
        sr.reject();
        return tl::unexpected(ParseError{
            sr.position(),
            "sv_system_name: expected identifier start after '$'"});
    }
    out.push_back(*next);
    sr.get();
    while (auto c = sr.peek()) {
        if (!is_simple_id_cont(*c)) break;
        out.push_back(*c);
        sr.get();
    }
    // Additional `$identifier` segments (e.g. `$value$plusargs`).
    while (auto c = sr.peek()) {
        if (*c != '$') break;
        sr.mark();
        sr.get();   // consume the '$'
        auto seg_start = sr.peek();
        if (!seg_start || !is_simple_id_start(*seg_start)) {
            // Not actually a sub-segment — rewind the '$' for the
            // outer parser to handle.
            sr.reject();
            break;
        }
        out.push_back('$');
        out.push_back(*seg_start);
        sr.get();
        while (auto cc = sr.peek()) {
            if (!is_simple_id_cont(*cc)) break;
            out.push_back(*cc);
            sr.get();
        }
        sr.accept();
    }
    sr.accept();
    return make_string(std::move(out));
}

SaveResult SvSystemNameParser::unparse(const Value& v) const {
    if (auto sv = dynamic_cast<const StringValue*>(&v)) return sv->data();
    return tl::unexpected(SaveError{
        "SvSystemNameParser::unparse expects StringValue"});
}

// --- SvQualifiedTypeParser ----------------------------------------------

SvQualifiedTypeParser::SvQualifiedTypeParser()
    : Parser("sv_qualified_type") {}

ParseResult SvQualifiedTypeParser::parse(StreamReader& sr) {
    sr.mark();
    auto first = sr.peek();
    if (!first || !is_simple_id_start(*first)) {
        sr.reject();
        return tl::unexpected(ParseError{
            sr.position(),
            "sv_qualified_type: expected identifier start"});
    }
    std::string out;
    out.push_back(*first);
    sr.get();
    while (auto c = sr.peek()) {
        if (!is_simple_id_cont(*c)) break;
        out.push_back(*c);
        sr.get();
    }
    // Optional `::name` suffix. The `::` only commits if a valid
    // identifier-start follows, so non-qualified names rewind cleanly
    // for the surrounding rule to handle whatever comes next.
    if (auto c = sr.peek(); c && *c == ':') {
        sr.mark();
        sr.get();   // first ':'
        auto c2 = sr.peek();
        if (c2 && *c2 == ':') {
            sr.get();   // second ':'
            auto seg_start = sr.peek();
            if (seg_start && is_simple_id_start(*seg_start)) {
                out.append("::");
                out.push_back(*seg_start);
                sr.get();
                while (auto cc = sr.peek()) {
                    if (!is_simple_id_cont(*cc)) break;
                    out.push_back(*cc);
                    sr.get();
                }
                sr.accept();
            } else {
                sr.reject();  // not a valid qualifier — rewind
            }
        } else {
            sr.reject();  // single ':' — rewind
        }
    }
    sr.accept();
    return make_string(std::move(out));
}

SaveResult SvQualifiedTypeParser::unparse(const Value& v) const {
    if (auto sv = dynamic_cast<const StringValue*>(&v)) return sv->data();
    return tl::unexpected(SaveError{
        "SvQualifiedTypeParser::unparse expects StringValue"});
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
        ParserSpec{"sv_pp_text_line", []() {
            return std::make_unique<SvPpTextLineParser>();
        }},
        ParserSpec{"sv_eol", []() {
            return std::make_unique<SvEolParser>();
        }},
        ParserSpec{"sv_pp_macro_name", []() {
            return std::make_unique<SvPpMacroNameParser>();
        }},
        ParserSpec{"sv_system_name", []() {
            return std::make_unique<SvSystemNameParser>();
        }},
        ParserSpec{"sv_qualified_type", []() {
            return std::make_unique<SvQualifiedTypeParser>();
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
