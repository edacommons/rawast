#pragma once

#include <rawast/parser.hpp>

#include <string>

namespace rawast {

// Match a literal token byte-by-byte. walk() accepts the token, fills
// accum_ with the matched bytes; default value() returns it as a
// StringValue. The engine driver may discard the returned value when
// the surrounding Node is a structural marker (Key with no value child).
class KeyParser final : public Parser {
    std::string token_;
    bool        strict_;
    std::string fb_spec_;
public:
    explicit KeyParser(std::string token, bool strict = false);
    WalkResult walk(StreamReader& sr) override;
    SaveResult unparse(const Value& value) const override;
    std::string_view first_bytes() const override { return fb_spec_; }
};

// Optional leading '-' followed by one or more digits. accum_ holds the
// matched digit string (with optional sign); value() converts to IntValue.
class IntParser final : public Parser {
public:
    IntParser();
    WalkResult walk(StreamReader& sr) override;
    ValuePtr   value() const override;
    SaveResult unparse(const Value& value) const override;
    std::string_view first_bytes() const override {
        return INC_CHAR("-") INC_RANGE("0", "9");
    }
};

// One or more digits. accum_ holds the digit string; value() → UIntValue.
class UIntParser final : public Parser {
public:
    UIntParser();
    WalkResult walk(StreamReader& sr) override;
    ValuePtr   value() const override;
    SaveResult unparse(const Value& value) const override;
    std::string_view first_bytes() const override {
        return INC_RANGE("0", "9");
    }
};

// Floating-point with optional sign, optional fractional part, optional
// exponent. Must contain a '.' or 'e'/'E' to qualify (a bare integer would
// be parsed by IntParser instead). accum_ holds the matched text;
// value() → RealValue.
class FloatParser final : public Parser {
public:
    FloatParser();
    WalkResult walk(StreamReader& sr) override;
    ValuePtr   value() const override;
    SaveResult unparse(const Value& value) const override;
    std::string_view first_bytes() const override {
        return INC_CHAR("+") INC_CHAR("-") INC_CHAR(".") INC_RANGE("0", "9");
    }
};

// One or more consecutive whitespace characters. accum_ holds the run;
// default value() returns it as a StringValue.
class WhitespaceParser final : public Parser {
public:
    WhitespaceParser();
    WalkResult walk(StreamReader& sr) override;
    std::string_view first_bytes() const override {
        return INC_CHAR(" ") INC_CHAR("\t") INC_CHAR("\n") INC_CHAR("\r");
    }
};

// Zero or more horizontal whitespace bytes (space, tab). CRUCIALLY
// does NOT consume newlines — newlines are preserved for the outer
// rule to handle. Always succeeds (may match zero bytes). value()
// returns an empty StringValue (the original spacing isn't recoverable).
//
// Used by line-aware grammars whose rules have to treat newlines as
// structural terminators while still tolerating spaces/tabs between
// tokens. The standard `whitespace` parser would swallow the newline
// and break line-awareness.
//
// On save, emits a single space — the original spacing isn't
// recoverable (0+), and a single space is the canonical safe choice.
class LinespaceParser final : public Parser {
public:
    LinespaceParser();
    WalkResult walk(StreamReader& sr) override;
    ValuePtr   value() const override;
    SaveResult unparse(const Value& value) const override;
    std::string_view first_bytes() const override {
        return INC_CHAR(" ") INC_CHAR("\t");
    }
};

// Identifier in the C-family sense: starts with letter or underscore,
// continues with letter / digit / underscore. accum_ holds the matched
// identifier text; default value() returns it as a StringValue.
//
// Note: this parser does NOT enforce "not a reserved keyword" — that's
// a grammar-design concern. Order grammar alternatives so any
// reserved-word Key matches come before the catch-all Parse(identifier).
class IdentifierParser final : public Parser {
    std::string extra_lead_;    // additional chars valid at first position
    std::string extra_cont_;    // additional chars valid in continuation
    mutable std::string fb_spec_;
public:
    IdentifierParser();
    // Parameterised: extra characters that can appear in identifiers
    // beyond the C-family default. Use to teach the parser about
    // format-specific identifier chars at construction time.
    IdentifierParser(std::string extra_lead, std::string extra_cont);
    WalkResult walk(StreamReader& sr) override;
    SaveResult unparse(const Value& value) const override;
    std::string_view first_bytes() const override;
};

// `ident('.' ident)*` — a dotted, group-qualified name. accum_ holds
// the full dotted string; default value() returns it as a StringValue.
class QualifiedIdentifierParser final : public Parser {
public:
    QualifiedIdentifierParser();
    WalkResult walk(StreamReader& sr) override;
    SaveResult unparse(const Value& value) const override;
    std::string_view first_bytes() const override {
        return INC_RANGE("A", "Z") INC_RANGE("a", "z") INC_CHAR("_");
    }
};

// Double-quoted string. Backslash escape sequences are preserved verbatim
// in the output (pass-through mode — the engine does not interpret
// escapes; higher-level code can if needed). accum_ holds the contents
// between the quotes (the quotes themselves are consumed but not
// included); default value() returns it as a StringValue.
class DoubleQuoteStringParser final : public Parser {
public:
    DoubleQuoteStringParser();
    WalkResult walk(StreamReader& sr) override;
    SaveResult unparse(const Value& value) const override;
    std::string_view first_bytes() const override { return INC_CHAR("\""); }
};

// `'...'`-delimited string. Mirror of DoubleQuoteStringParser but uses
// single-quote delimiters; backslash-escape pass-through is the same.
// Registered in the std group as `single_quote_string` — the name
// describes the syntactic form, not any usage convention. The rawast
// meta-grammar uses this parser in KEY_EXPR to recognise single-quote
// literals as strict (word-bounded) Key nodes, but other grammars can
// reuse the parser for any purpose that wants a `'...'` string token.
class SingleQuoteStringParser final : public Parser {
public:
    SingleQuoteStringParser();
    WalkResult walk(StreamReader& sr) override;
    SaveResult unparse(const Value& value) const override;
    std::string_view first_bytes() const override { return INC_CHAR("'"); }
};

// `//` line comment, consumed up to but not including the line terminator
// (so the trailing newline survives for the line-tracking machinery).
// accum_ holds the full comment text including the leading `//`; default
// value() returns it as a StringValue.
//
// Adding this parser to a grammar's ignore list turns the grammar into
// one that tolerates `//` line comments anywhere whitespace would be
// allowed — the JSON-with-comments (JSONC) pattern, with zero changes to
// the grammar tree.
class LineCommentParser final : public Parser {
public:
    LineCommentParser();
    WalkResult walk(StreamReader& sr) override;
    std::string_view first_bytes() const override { return INC_CHAR("/"); }
};

// `// …` line comment that STOPS at a line-continuation `\<newline>` — it
// consumes to end of line like line_comment, but if the line ends in a
// `\` immediately before the newline, the comment ends BEFORE that `\`,
// leaving `\<newline>` for a continuation rule to consume. This is the
// macro-body rule: `\`define M // cmt \<newline> body` — the comment is
// `// cmt`, and the macro CONTINUES to `body`. A bare `\` not before a
// newline stays regular comment content.
class LineCommentContParser final : public Parser {
public:
    LineCommentContParser();
    WalkResult walk(StreamReader& sr) override;
    std::string_view first_bytes() const override { return INC_CHAR("/"); }
};

// `/* ... */` block comment. Spans multiple lines. accum_ holds the full
// comment text including the `/*` and `*/` delimiters; default value()
// returns it as a StringValue.
//
// Same usage pattern as LineCommentParser: add to the ignore list to
// enable block comments wherever whitespace would be tolerated.
class BlockCommentParser final : public Parser {
public:
    BlockCommentParser();
    WalkResult walk(StreamReader& sr) override;
    std::string_view first_bytes() const override { return INC_CHAR("/"); }
};

// Register the `std` parser group. Idempotent — safe to call from
// multiple init paths. Provides:
//
//   std.int            (IntParser)
//   std.float          (FloatParser)
//   std.identifier     (IdentifierParser, default char set)
//   std.string         (DoubleQuoteStringParser)
//   std.whitespace     (WhitespaceParser)
//   std.line_comment   (LineCommentParser)
//   std.block_comment  (BlockCommentParser)
//
// Each is also addressable bare (`int`, `whitespace`, ...) when no
// other active group declares the same name. Grammars opt in by
// adding `"use": ["std"]` (JSON form) or `use: std` (.rawast form).
void register_std_parser_group();

} // namespace rawast
