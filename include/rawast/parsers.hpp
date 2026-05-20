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
};

// Optional leading '-' followed by one or more digits. Produces an IntValue.
class IntParser final : public Parser {
public:
    IntParser();
    ParseResult parse(StreamReader& sr) override;
};

// One or more digits. Produces a UIntValue.
class UIntParser final : public Parser {
public:
    UIntParser();
    ParseResult parse(StreamReader& sr) override;
};

// Floating-point with optional sign, optional fractional part, optional
// exponent. Must contain a '.' or 'e'/'E' to qualify (a bare integer would
// be parsed by IntParser instead). Produces a RealValue.
class FloatParser final : public Parser {
public:
    FloatParser();
    ParseResult parse(StreamReader& sr) override;
};

// One or more consecutive whitespace characters. Produces a StringValue
// containing the matched run.
class WhitespaceParser final : public Parser {
public:
    WhitespaceParser();
    ParseResult parse(StreamReader& sr) override;
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
};

} // namespace rawast
