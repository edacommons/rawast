#pragma once

#include <rawast/parser.hpp>

#include <string>

namespace rawast {

// Match a literal token byte-by-byte. Produces a StringValue of the matched
// token on success. The engine driver may discard the returned value when
// the surrounding Node is a structural marker (Key with no value child).
class KeyParser final : public Parser {
    std::string token_;
    bool        strict_;
public:
    explicit KeyParser(std::string token, bool strict = false);
    ParseResult parse(StreamReader& sr) override;
    SaveResult  unparse(const Value& value) const override;
};

// Optional leading '-' followed by one or more digits. Produces an IntValue.
class IntParser final : public Parser {
public:
    IntParser();
    ParseResult parse(StreamReader& sr) override;
    SaveResult  unparse(const Value& value) const override;
};

// One or more digits. Produces a UIntValue.
class UIntParser final : public Parser {
public:
    UIntParser();
    ParseResult parse(StreamReader& sr) override;
    SaveResult  unparse(const Value& value) const override;
};

// Floating-point with optional sign, optional fractional part, optional
// exponent. Must contain a '.' or 'e'/'E' to qualify (a bare integer would
// be parsed by IntParser instead). Produces a RealValue.
class FloatParser final : public Parser {
public:
    FloatParser();
    ParseResult parse(StreamReader& sr) override;
    SaveResult  unparse(const Value& value) const override;
};

// One or more consecutive whitespace characters. Produces a StringValue
// containing the matched run.
class WhitespaceParser final : public Parser {
public:
    WhitespaceParser();
    ParseResult parse(StreamReader& sr) override;
};

// Zero or more horizontal whitespace bytes (space, tab). CRUCIALLY
// does NOT consume newlines — newlines are preserved for the outer
// rule to handle. Always succeeds (may match zero bytes).
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
    ParseResult parse(StreamReader& sr) override;
    SaveResult  unparse(const Value& value) const override;
};

// Identifier in the C-family sense: starts with letter or underscore,
// continues with letter / digit / underscore. Produces a StringValue
// holding the matched identifier text.
//
// Note: this parser does NOT enforce "not a reserved keyword" — that's
// a grammar-design concern. Order grammar alternatives so any
// reserved-word Key matches come before the catch-all Parse(identifier).
class IdentifierParser final : public Parser {
    std::string extra_lead_;    // additional chars valid at first position
    std::string extra_cont_;    // additional chars valid in continuation
public:
    IdentifierParser();
    // Parameterised: extra characters that can appear in identifiers
    // beyond the C-family default. Use to teach the parser about
    // format-specific identifier chars discovered at parse time — e.g.
    // LEF's DIVIDERCHAR "/" becomes an extra continuation char after
    // the preamble parses (wired through Grammar::replace_parser from
    // an on_rule_complete callback).
    IdentifierParser(std::string extra_lead, std::string extra_cont);
    ParseResult parse(StreamReader& sr) override;
    SaveResult  unparse(const Value& value) const override;
};

// `ident('.' ident)*` — a dotted, group-qualified name. Used by the
// meta-grammar to parse parser references in either bare (`int`) or
// group-qualified (`gdsii.header`) form as a single StringValue, so the
// resulting dict shape is flat (`{type: "gdsii.header"}`) and the parser
// registry's dotted-alias lookup resolves it directly.
class QualifiedIdentifierParser final : public Parser {
public:
    QualifiedIdentifierParser();
    ParseResult parse(StreamReader& sr) override;
    SaveResult  unparse(const Value& value) const override;
};

// Double-quoted string. Backslash escape sequences are preserved verbatim
// in the output (this is the pass-through mode from the prototype — the
// engine does not interpret escapes; higher-level code can if needed).
// Produces a StringValue containing the contents between the quotes; the
// quotes themselves are consumed but not included in the value.
class DoubleQuoteStringParser final : public Parser {
public:
    DoubleQuoteStringParser();
    ParseResult parse(StreamReader& sr) override;
    SaveResult  unparse(const Value& value) const override;
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
    ParseResult parse(StreamReader& sr) override;
    SaveResult  unparse(const Value& value) const override;
};

// `//` line comment, consumed up to but not including the line terminator
// (so the trailing newline survives for the line-tracking machinery).
// Produces a StringValue containing the full comment text including the
// leading `//`.
//
// Adding this parser to a grammar's ignore list turns the grammar into
// one that tolerates `//` line comments anywhere whitespace would be
// allowed — the JSON-with-comments (JSONC) pattern, with zero changes to
// the grammar tree.
class LineCommentParser final : public Parser {
public:
    LineCommentParser();
    ParseResult parse(StreamReader& sr) override;
};

// `/* ... */` block comment. Spans multiple lines. Produces a StringValue
// containing the full comment text including the `/*` and `*/` delimiters.
//
// Same usage pattern as LineCommentParser: add to the ignore list to
// enable block comments wherever whitespace would be tolerated.
class BlockCommentParser final : public Parser {
public:
    BlockCommentParser();
    ParseResult parse(StreamReader& sr) override;
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
