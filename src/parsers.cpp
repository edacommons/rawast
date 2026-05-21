#include <rawast/parsers.hpp>
#include <rawast/parsers_registry.hpp>

#include <array>
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

SaveResult KeyParser::unparse(const Value& /*value*/) const {
    // Literal is fixed; the input value (if any) is ignored.
    return token_;
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

SaveResult IntParser::unparse(const Value& value) const {
    auto iv = dynamic_cast<const IntValue*>(&value);
    if (!iv) {
        return tl::unexpected(SaveError{"IntParser::unparse expects IntValue"});
    }
    return std::to_string(iv->data());
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

SaveResult UIntParser::unparse(const Value& value) const {
    auto uv = dynamic_cast<const UIntValue*>(&value);
    if (!uv) {
        return tl::unexpected(SaveError{"UIntParser::unparse expects UIntValue"});
    }
    return std::to_string(uv->data());
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

SaveResult FloatParser::unparse(const Value& value) const {
    auto rv = dynamic_cast<const RealValue*>(&value);
    if (!rv) {
        return tl::unexpected(SaveError{"FloatParser::unparse expects RealValue"});
    }
    // std::to_chars gives the shortest round-trippable representation.
    std::array<char, 32> buf{};
    auto [ptr, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), rv->data());
    if (ec != std::errc{}) {
        return tl::unexpected(SaveError{"float formatting failed"});
    }
    std::string out{buf.data(), ptr};
    // The FloatParser rejects integer-looking inputs (no '.' or 'e'); make
    // sure the output also satisfies that, by appending ".0" if needed.
    bool has_dot_or_exp = false;
    for (char c : out) {
        if (c == '.' || c == 'e' || c == 'E') { has_dot_or_exp = true; break; }
    }
    if (!has_dot_or_exp) out += ".0";
    return out;
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

SaveResult DoubleQuoteStringParser::unparse(const Value& value) const {
    auto sv = dynamic_cast<const StringValue*>(&value);
    if (!sv) {
        return tl::unexpected(SaveError{
            "DoubleQuoteStringParser::unparse expects StringValue"});
    }
    // Contents already include any backslash escapes from parse (pass-
    // through mode); wrap in quotes.
    return "\"" + sv->data() + "\"";
}

// IdentifierParser --------------------------------------------------------

IdentifierParser::IdentifierParser() : Parser("identifier") {}

IdentifierParser::IdentifierParser(std::string extra_lead, std::string extra_cont)
    : Parser("identifier"),
      extra_lead_(std::move(extra_lead)),
      extra_cont_(std::move(extra_cont)) {}

ParseResult IdentifierParser::parse(StreamReader& sr) {
    sr.mark();
    const Position start = sr.position();

    auto first = sr.peek();
    auto is_lead = [&](char c) {
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') return true;
        return extra_lead_.find(c) != std::string::npos;
    };
    auto is_cont = [&](char c) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') return true;
        return extra_cont_.find(c) != std::string::npos;
    };
    if (!first || !is_lead(*first)) {
        sr.reject();
        return tl::unexpected(ParseError{start, "expected identifier"});
    }

    std::string out;
    out.push_back(*first);
    sr.get();

    while (true) {
        auto c = sr.peek();
        if (!c) break;
        if (is_cont(*c)) {
            out.push_back(*c);
            sr.get();
        } else {
            break;
        }
    }

    sr.accept();
    return make_string(std::move(out));
}

SaveResult IdentifierParser::unparse(const Value& value) const {
    auto sv = dynamic_cast<const StringValue*>(&value);
    if (!sv) {
        return tl::unexpected(SaveError{
            "IdentifierParser::unparse expects StringValue"});
    }
    return sv->data();
}

// LineCommentParser -------------------------------------------------------

LineCommentParser::LineCommentParser() : Parser("line_comment") {}

ParseResult LineCommentParser::parse(StreamReader& sr) {
    sr.mark();
    const Position start = sr.position();

    auto c1 = sr.get();
    if (!c1 || *c1 != '/') {
        sr.reject();
        return tl::unexpected(ParseError{start, "expected '//'"});
    }
    auto c2 = sr.get();
    if (!c2 || *c2 != '/') {
        sr.reject();
        return tl::unexpected(ParseError{start, "expected '//'"});
    }

    std::string body = "//";
    while (true) {
        auto c = sr.peek();
        if (!c || *c == '\n' || *c == '\r') break;
        body.push_back(*c);
        sr.get();
    }

    sr.accept();
    return make_string(std::move(body));
}

// BlockCommentParser ------------------------------------------------------

BlockCommentParser::BlockCommentParser() : Parser("block_comment") {}

ParseResult BlockCommentParser::parse(StreamReader& sr) {
    sr.mark();
    const Position start = sr.position();

    auto c1 = sr.get();
    if (!c1 || *c1 != '/') {
        sr.reject();
        return tl::unexpected(ParseError{start, "expected '/*'"});
    }
    auto c2 = sr.get();
    if (!c2 || *c2 != '*') {
        sr.reject();
        return tl::unexpected(ParseError{start, "expected '/*'"});
    }

    std::string body = "/*";
    char prev = '\0';
    while (true) {
        auto c = sr.get();
        if (!c) {
            sr.reject();
            return tl::unexpected(ParseError{start, "unterminated block comment"});
        }
        body.push_back(*c);
        if (prev == '*' && *c == '/') break;
        prev = *c;
    }

    sr.accept();
    return make_string(std::move(body));
}

// -----------------------------------------------------------------------
// Standard parser group registration
// -----------------------------------------------------------------------

void register_std_parser_group() {
    // Idempotent: register_parser_group(ParserGroup) does
    // insert_or_assign, so repeated calls overwrite with an equivalent
    // group definition. Safe to call from multiple init paths
    // (binding load, host C++ code).
    ParserGroup g;
    g.name = "std";
    g.parsers = {
        {"int",            [] { return std::make_unique<IntParser>(); }},
        {"float",          [] { return std::make_unique<FloatParser>(); }},
        {"identifier",     [] { return std::make_unique<IdentifierParser>(); }},
        {"string",         [] { return std::make_unique<DoubleQuoteStringParser>(); }},
        {"whitespace",     [] { return std::make_unique<WhitespaceParser>(); }},
        {"line_comment",   [] { return std::make_unique<LineCommentParser>(); }},
        {"block_comment",  [] { return std::make_unique<BlockCommentParser>(); }},
    };
    register_parser_group(std::move(g));
}

} // namespace rawast
