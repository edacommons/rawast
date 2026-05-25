#pragma once

#include <rawast/parser.hpp>

namespace rawast {

class Grammar;

// -----------------------------------------------------------------------
// Tcl parser group — eleven terminals modelled on Dodekalogue rules
// (the twelve-rule Tcl(n) spec). Used by `grammars/tcl.rawast` via
// `use: tcl`.
//
// All string-returning terminals preserve their content verbatim
// (Tier-1 policy): backslash escapes and substitution markers stay as
// raw text in the returned StringValue. Application layers or sub-
// parses re-interpret as needed.
// -----------------------------------------------------------------------

// Horizontal whitespace + `\<newline>` line continuation. Listed in
// SCRIPT context's ignore list. Greedy: consumes runs of spaces, tabs,
// and backslash-newline pairs as one match. Fails if no horizontal-ws
// is present at the current position.
class TclHspaceParser final : public Parser {
public:
    TclHspaceParser();
    ParseResult parse(StreamReader& sr) override;
};

// A single `\n` byte. Used as a structural command separator.
class TclNewlineParser final : public Parser {
public:
    TclNewlineParser();
    ParseResult parse(StreamReader& sr) override;
};

// `#` followed by everything up to (and including) the next newline.
// Returns the comment body (without the leading `#` and without the
// trailing newline) as a StringValue. The "command-position only"
// constraint of Dodekalogue rule 10 is enforced by the grammar
// (COMMENT alternative tried first inside COMMAND); the terminal
// itself just recognises `#`-to-EOL.
class TclCommentParser final : public Parser {
public:
    TclCommentParser();
    ParseResult parse(StreamReader& sr) override;
};

// `{...}` with brace-depth counting. Returns the inner content
// (without the outer braces) as a StringValue. `\{` and `\}` are
// preserved verbatim and do NOT count toward nesting depth.
class TclBraceGroupParser final : public Parser {
public:
    TclBraceGroupParser();
    ParseResult parse(StreamReader& sr) override;
};

// `"..."` with backslash escapes. Returns the inner content
// (without the outer quotes) as a StringValue. Backslash + any-char
// is preserved verbatim and does not terminate the string.
class TclQuotedStringParser final : public Parser {
public:
    TclQuotedStringParser();
    ParseResult parse(StreamReader& sr) override;
};

// `[...]` with bracket nesting. Inside, `"..."` and `{...}` are
// respected so `]` inside them doesn't close the outer bracket-sub.
// Returns the inner content (without the outer brackets) as a
// StringValue.
class TclBracketSubParser final : public Parser {
public:
    TclBracketSubParser();
    ParseResult parse(StreamReader& sr) override;
};

// Maximal run of non-special chars. Specials are whitespace (space,
// tab, newline), `;`, `[`, `]`, `{`, `}`, `"`. Returns the run as a
// StringValue; fails on a zero-length match (so the structural
// grammar can fall through to other word flavours).
class TclBareWordParser final : public Parser {
public:
    TclBareWordParser();
    ParseResult parse(StreamReader& sr) override;
};

// `{*}` followed by non-whitespace (Dodekalogue rule 5 — argument
// expansion). Consumes `{*}` only if the next byte is non-whitespace
// and non-special; emits NullValue (purely structural). The grammar
// uses this as a leading marker on EXPAND_WORD.
class TclExpandMarkerParser final : public Parser {
public:
    TclExpandMarkerParser();
    ParseResult parse(StreamReader& sr) override;
};

// Variable name — either `name` (bare) or `{name}` (braced). For the
// bare form, accepts `[A-Za-z0-9_:]+`. For the braced form, accepts
// anything except `}` until the matching `}`. Returns the name as a
// StringValue (without `${` and `}` for the braced form).
class TclVarNameParser final : public Parser {
public:
    TclVarNameParser();
    ParseResult parse(StreamReader& sr) override;
};

// Consume until the next unmatched `)`. Used for array indices in
// $arr(idx). Returns the consumed text (without the closing `)`) as
// a StringValue. The structural grammar consumes the closing `)`.
class TclUntilParenParser final : public Parser {
public:
    TclUntilParenParser();
    ParseResult parse(StreamReader& sr) override;
};

// Backslash + one byte. Returns the two-byte escape sequence as a
// StringValue. Fails on lone backslash at end-of-input. Used inside
// WORD_SEGMENTS to split escape sequences out from literal runs.
class TclEscapeParser final : public Parser {
public:
    TclEscapeParser();
    ParseResult parse(StreamReader& sr) override;
};

// Run of chars that aren't `$`, `[`, `\`, or `"`. At least one char;
// fails on a zero-length match. Used inside WORD_SEGMENTS as the
// catch-all literal-text segment between substitutions / escapes.
class TclLiteralRunParser final : public Parser {
public:
    TclLiteralRunParser();
    ParseResult parse(StreamReader& sr) override;
};

// Register the "tcl" parser group in the global registry. Grammars
// declare `use: tcl` to pull in the terminals above. Idempotent.
void register_tcl_parser_group();

} // namespace rawast
