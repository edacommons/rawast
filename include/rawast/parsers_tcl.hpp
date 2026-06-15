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
// is present at the current position. value() returns null (ignore-list
// terminal — no payload).
class TclHspaceParser final : public Parser {
public:
    TclHspaceParser();
    WalkResult walk(StreamReader& sr) override;
    ValuePtr   value() const override;
};

// A single `\n` byte. Used as a structural command separator.
// value() returns null (no payload).
class TclNewlineParser final : public Parser {
public:
    TclNewlineParser();
    WalkResult walk(StreamReader& sr) override;
    ValuePtr   value() const override;
    // Emits "\n" unconditionally. The parse side returns null_value
    // (so nothing is captured into the AST), but a SEP choice in the
    // grammar may dispatch this alternative on save; without unparse
    // the dispatch would fail.
    SaveResult unparse(const Value& value) const override;
};

// Command separator that accepts either `\n` or `;`. Per Dodekalogue
// rule 11, both bytes terminate a command. The parse side returns
// null_value (no AST capture — the form distinction is dropped);
// save emits canonical `\n`. Eliminates the SEP-choice save dispatch
// problem (a Choice with no discriminator can't pick an alt).
class TclCommandSepParser final : public Parser {
public:
    TclCommandSepParser();
    WalkResult walk(StreamReader& sr) override;
    ValuePtr   value() const override;
    SaveResult unparse(const Value& value) const override;
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
    WalkResult walk(StreamReader& sr) override;
    SaveResult unparse(const Value& value) const override;
};

// `{...}` with brace-depth counting. Returns the inner content
// (without the outer braces) as a StringValue. `\{` and `\}` are
// preserved verbatim and do NOT count toward nesting depth.
class TclBraceGroupParser final : public Parser {
public:
    TclBraceGroupParser();
    WalkResult walk(StreamReader& sr) override;
    SaveResult unparse(const Value& value) const override;
};

// `"..."` with backslash escapes. Returns the inner content
// (without the outer quotes) as a StringValue. Backslash + any-char
// is preserved verbatim and does not terminate the string.
class TclQuotedStringParser final : public Parser {
public:
    TclQuotedStringParser();
    WalkResult walk(StreamReader& sr) override;
    SaveResult unparse(const Value& value) const override;
};

// `[...]` with bracket nesting. Inside, `"..."` and `{...}` are
// respected so `]` inside them doesn't close the outer bracket-sub.
// Returns the inner content (without the outer brackets) as a
// StringValue.
class TclBracketSubParser final : public Parser {
public:
    TclBracketSubParser();
    WalkResult walk(StreamReader& sr) override;
    SaveResult unparse(const Value& value) const override;
};

// Maximal run of non-special chars. Specials are whitespace (space,
// tab, newline), `;`, `[`, `]`, `{`, `}`, `"`. Returns the run as a
// StringValue; fails on a zero-length match (so the structural
// grammar can fall through to other word flavours).
class TclBareWordParser final : public Parser {
public:
    TclBareWordParser();
    WalkResult walk(StreamReader& sr) override;
    SaveResult unparse(const Value& value) const override;
};

// `{*}` followed by non-whitespace (Dodekalogue rule 5 — argument
// expansion). Consumes `{*}` only if the next byte is non-whitespace
// and non-special; emits NullValue (purely structural). The grammar
// uses this as a leading marker on EXPAND_WORD.
class TclExpandMarkerParser final : public Parser {
public:
    TclExpandMarkerParser();
    WalkResult walk(StreamReader& sr) override;
    ValuePtr   value() const override;
    // Emits "{*}" unconditionally. The parse side returned null_value
    // (the marker is purely structural — the AST encodes its presence
    // via the bound `expand=true` flag on EXPAND_WORD), but save
    // dispatch still routes some value through here.
    SaveResult unparse(const Value& value) const override;
};

// Variable name — either `name` (bare) or `{name}` (braced). For the
// bare form, accepts `[A-Za-z0-9_:]+`. For the braced form, accepts
// anything except `}` until the matching `}`. Returns the name as a
// StringValue (without `${` and `}` for the braced form).
class TclVarNameParser final : public Parser {
public:
    TclVarNameParser();
    WalkResult walk(StreamReader& sr) override;
    // Note: the parser accepts both bare `name` and braced `{name}` forms
    // but stores only the bare name; unparse picks the form by checking
    // whether every byte is a valid bare-form char.
    SaveResult unparse(const Value& value) const override;
};

// Retired: `TclUntilParenParser`. Replaced by the grammar-level
// `*` raw-consume primitive (engine commit e865727); VAR_INDEX in
// tcl.rawast now uses `*:index=@:#subparse="WORD_SEGMENTS"` with `)`
// as the stop literal in the surrounding sequence. See
// docs/rawast-format.md §4.5a-1.

// Backslash + one byte. Returns the two-byte escape sequence as a
// StringValue. Fails on lone backslash at end-of-input. Used inside
// WORD_SEGMENTS to split escape sequences out from literal runs.
class TclEscapeParser final : public Parser {
public:
    TclEscapeParser();
    WalkResult walk(StreamReader& sr) override;
    SaveResult unparse(const Value& value) const override;
};

// Run of chars that aren't `$`, `[`, `\`, or `"`. At least one char;
// fails on a zero-length match. Used inside WORD_SEGMENTS as the
// catch-all literal-text segment between substitutions / escapes.
class TclLiteralRunParser final : public Parser {
public:
    TclLiteralRunParser();
    WalkResult walk(StreamReader& sr) override;
    SaveResult unparse(const Value& value) const override;
};

// Register the "tcl" parser group in the global registry. Grammars
// declare `use: tcl` to pull in the terminals above. Idempotent.
void register_tcl_parser_group();

} // namespace rawast
