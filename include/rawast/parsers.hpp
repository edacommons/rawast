#pragma once

#include <rawast/parser.hpp>

#include <string>

namespace rawast {

// Match a literal token byte-by-byte. Produces a StringValue of the matched
// token on success. The engine driver may discard the returned value when
// the surrounding Node is a structural marker (Key with no value child).
class KeyParser final : public Parser {
    std::string token_;
public:
    explicit KeyParser(std::string token);
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

// Identifier in the C-family sense: starts with letter or underscore,
// continues with letter / digit / underscore. Produces a StringValue
// holding the matched identifier text.
//
// Note: this parser does NOT enforce "not a reserved keyword" — that's
// a grammar-design concern. Order grammar alternatives so any
// reserved-word Key matches come before the catch-all Parse(identifier).
class IdentifierParser final : public Parser {
public:
    IdentifierParser();
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

} // namespace rawast
