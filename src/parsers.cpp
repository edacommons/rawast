#include <rawast/parsers.hpp>

#include <cctype>
#include <charconv>
#include <string>
#include <system_error>
#include <utility>

namespace rawast {

// KeyParser ---------------------------------------------------------------

KeyParser::KeyParser(std::string token)
    : Parser(token), token_(std::move(token)) {}

ParseResult KeyParser::parse(StreamReader& sr) {
    sr.mark();
    const Position start = sr.position();

    for (char expected : token_) {
        auto c = sr.get();
        if (!c || *c != expected) {
            sr.reject();
            return tl::unexpected(ParseError{
                start, "expected literal '" + token_ + "'"});
        }
    }
    sr.accept();
    return make_string(token_);
}

// IntParser ---------------------------------------------------------

IntParser::IntParser() : Parser("int") {}

ParseResult IntParser::parse(StreamReader& sr) {
    sr.mark();
    const Position start = sr.position();

    std::string digits;
    auto first = sr.peek();
    if (first && *first == '-') {
        digits.push_back('-');
        sr.get();
    }

    bool seen_digit = false;
    while (true) {
        auto c = sr.peek();
        if (!c || !std::isdigit(static_cast<unsigned char>(*c))) break;
        digits.push_back(*c);
        sr.get();
        seen_digit = true;
    }

    if (!seen_digit) {
        sr.reject();
        return tl::unexpected(ParseError{start, "expected integer"});
    }

    sr.accept();
    std::int64_t result;
    auto [ptr, ec] = std::from_chars(digits.data(),
                                     digits.data() + digits.size(),
                                     result);
    if (ec != std::errc{}) {
        return tl::unexpected(ParseError{start, "integer out of range"});
    }
    return make_int(result);
}

// UIntParser --------------------------------------------------------------

UIntParser::UIntParser() : Parser("uint") {}

ParseResult UIntParser::parse(StreamReader& sr) {
    sr.mark();
    const Position start = sr.position();

    std::string digits;
    while (true) {
        auto c = sr.peek();
        if (!c || !std::isdigit(static_cast<unsigned char>(*c))) break;
        digits.push_back(*c);
        sr.get();
    }

    if (digits.empty()) {
        sr.reject();
        return tl::unexpected(ParseError{start, "expected unsigned integer"});
    }

    sr.accept();
    std::uint64_t result;
    auto [ptr, ec] = std::from_chars(digits.data(),
                                     digits.data() + digits.size(),
                                     result);
    if (ec != std::errc{}) {
        return tl::unexpected(ParseError{start, "unsigned integer out of range"});
    }
    return make_uint(result);
}

// FloatParser -------------------------------------------------------------

FloatParser::FloatParser() : Parser("float") {}

ParseResult FloatParser::parse(StreamReader& sr) {
    sr.mark();
    const Position start = sr.position();

    std::string digits;
    bool has_dot          = false;
    bool has_exp          = false;
    bool has_digit_before = false;
    bool has_digit_after  = false;
    bool has_digit_in_exp = false;

    // Optional sign.
    auto c = sr.peek();
    if (c && *c == '-') {
        digits.push_back('-');
        sr.get();
    }

    // Integer part.
    while (true) {
        c = sr.peek();
        if (!c || !std::isdigit(static_cast<unsigned char>(*c))) break;
        digits.push_back(*c);
        sr.get();
        has_digit_before = true;
    }

    // Optional fractional part.
    c = sr.peek();
    if (c && *c == '.') {
        digits.push_back('.');
        sr.get();
        has_dot = true;
        while (true) {
            c = sr.peek();
            if (!c || !std::isdigit(static_cast<unsigned char>(*c))) break;
            digits.push_back(*c);
            sr.get();
            has_digit_after = true;
        }
    }

    if (!has_digit_before && !has_digit_after) {
        sr.reject();
        return tl::unexpected(ParseError{start, "expected floating-point number"});
    }

    // Optional exponent.
    c = sr.peek();
    if (c && (*c == 'e' || *c == 'E')) {
        digits.push_back(*c);
        sr.get();
        has_exp = true;

        c = sr.peek();
        if (c && (*c == '+' || *c == '-')) {
            digits.push_back(*c);
            sr.get();
        }
        while (true) {
            c = sr.peek();
            if (!c || !std::isdigit(static_cast<unsigned char>(*c))) break;
            digits.push_back(*c);
            sr.get();
            has_digit_in_exp = true;
        }
        if (!has_digit_in_exp) {
            sr.reject();
            return tl::unexpected(ParseError{
                start, "expected exponent digits after 'e' / 'E'"});
        }
    }

    // To qualify as float, the input must carry a fractional part or an
    // exponent. A bare integer would have been matched by IntParser.
    if (!has_dot && !has_exp) {
        sr.reject();
        return tl::unexpected(ParseError{
            start, "not a floating-point number (no '.' or exponent)"});
    }

    sr.accept();
    double result;
    auto [ptr, ec] = std::from_chars(digits.data(),
                                     digits.data() + digits.size(),
                                     result);
    if (ec != std::errc{}) {
        return tl::unexpected(ParseError{start, "floating-point conversion failed"});
    }
    return make_real(result);
}

// WhitespaceParser --------------------------------------------------------

WhitespaceParser::WhitespaceParser() : Parser("whitespace") {}

ParseResult WhitespaceParser::parse(StreamReader& sr) {
    sr.mark();
    const Position start = sr.position();

    std::string spaces;
    while (true) {
        auto c = sr.peek();
        if (!c || !std::isspace(static_cast<unsigned char>(*c))) break;
        spaces.push_back(*c);
        sr.get();
    }

    if (spaces.empty()) {
        sr.reject();
        return tl::unexpected(ParseError{start, "expected whitespace"});
    }

    sr.accept();
    return make_string(std::move(spaces));
}

// DoubleQuoteStringParser -------------------------------------------------

DoubleQuoteStringParser::DoubleQuoteStringParser() : Parser("string") {}

ParseResult DoubleQuoteStringParser::parse(StreamReader& sr) {
    sr.mark();
    const Position start = sr.position();

    auto open = sr.get();
    if (!open || *open != '"') {
        sr.reject();
        return tl::unexpected(ParseError{start, "expected opening '\"'"});
    }

    std::string contents;
    bool escaped = false;
    while (true) {
        auto ch = sr.get();
        if (!ch) {
            sr.reject();
            return tl::unexpected(ParseError{start, "unterminated string"});
        }
        if (escaped) {
            contents.push_back(*ch);
            escaped = false;
        } else if (*ch == '\\') {
            contents.push_back(*ch);
            escaped = true;
        } else if (*ch == '"') {
            break;
        } else {
            contents.push_back(*ch);
        }
    }

    sr.accept();
    return make_string(std::move(contents));
}

} // namespace rawast
