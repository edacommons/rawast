#include <rawast/parsers_sv.hpp>
#include <rawast/parsers_registry.hpp>
#include <rawast/stream.hpp>
#include <rawast/value.hpp>

#include <cctype>
#include <charconv>
#include <cstdint>
#include <memory>
#include <string>
#include <system_error>

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

// Digit class predicates for SV number bases.
bool is_dec_digit(char c) { return c >= '0' && c <= '9'; }
bool is_hex_digit(char c) {
    return is_dec_digit(c)
        || (c >= 'a' && c <= 'f')
        || (c >= 'A' && c <= 'F');
}
bool is_oct_digit(char c) { return c >= '0' && c <= '7'; }
bool is_bin_digit(char c) { return c == '0' || c == '1'; }
bool is_xz_digit(char c) {
    return c == 'x' || c == 'X' || c == 'z' || c == 'Z' || c == '?';
}

// Helper: consume `[_<digit>]+` returning the concatenated digit
// string (with underscores stripped).
//
// `allow_xz` controls whether x/z/`?` digits are accepted alongside
// the base-specific digits. The LRM permits x/z only in the digit
// portion of a *sized/based* literal — never in an unsized decimal
// integer (where `42z` is `42` followed by identifier `z`, not a
// malformed integer).
template <typename Pred>
std::string consume_digits(StreamReader& sr, Pred digit_pred, bool allow_xz) {
    std::string out;
    while (auto c = sr.peek()) {
        if (*c == '_') { sr.get(); continue; }
        if (digit_pred(*c) || (allow_xz && is_xz_digit(*c))) {
            out.push_back(*c);
            sr.get();
        } else {
            break;
        }
    }
    return out;
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

// --- SvNumberParser -----------------------------------------------------

SvNumberParser::SvNumberParser() : Parser("sv_number") {}

ParseResult SvNumberParser::parse(StreamReader& sr) {
    sr.mark();
    const Position start = sr.position();

    // Three syntactic paths from the first byte:
    //   '`': starts an unsized based number ('h, 'b, 'o, 'd, 's[h|b|o|d])
    //   digit: integer / real / sized-based / time
    //
    // We greedily consume the integer prefix, then dispatch:
    //   - followed by `'`  → sized based number, prefix was the size
    //   - followed by `.`  → real (continue lexing fraction/exp)
    //   - followed by `e`/`E` → real with exponent only
    //   - followed by alpha (`ns`, `ps`, `s`...) → time literal
    //   - otherwise          → plain integer

    auto first = sr.peek();
    if (!first) {
        sr.reject();
        return tl::unexpected(ParseError{start, "expected number, got EOF"});
    }

    auto emit_based = [&](const std::string& size_digits)
        -> ParseResult
    {
        // Already at `'`. Consume `'`, optional `s`/`S`, base char,
        // then digits.
        sr.get();
        bool is_signed = false;
        auto sc = sr.peek();
        if (sc && (*sc == 's' || *sc == 'S')) {
            is_signed = true;
            sr.get();
        }
        auto bc = sr.peek();
        if (!bc) {
            sr.reject();
            return tl::unexpected(ParseError{
                start, "expected base character after '\\''"});
        }
        char base_char;
        switch (*bc) {
        case 'b': case 'B': base_char = 'b'; break;
        case 'o': case 'O': base_char = 'o'; break;
        case 'd': case 'D': base_char = 'd'; break;
        case 'h': case 'H': base_char = 'h'; break;
        default:
            sr.reject();
            return tl::unexpected(ParseError{
                start, "unknown number base (expected b/o/d/h)"});
        }
        sr.get();

        std::string digits;
        switch (base_char) {
        case 'b': digits = consume_digits(sr, is_bin_digit, true); break;
        case 'o': digits = consume_digits(sr, is_oct_digit, true); break;
        case 'd': digits = consume_digits(sr, is_dec_digit, true); break;
        case 'h': digits = consume_digits(sr, is_hex_digit, true); break;
        }
        if (digits.empty()) {
            sr.reject();
            return tl::unexpected(ParseError{
                start, "expected digits after base specifier"});
        }

        auto d = std::make_shared<DictValue>();
        d->data()["kind"]   = make_string("based");
        if (size_digits.empty()) {
            d->data()["size"] = null_value();
        } else {
            std::int64_t size_val = 0;
            auto [_p, ec] = std::from_chars(
                size_digits.data(),
                size_digits.data() + size_digits.size(), size_val);
            if (ec != std::errc{}) {
                sr.reject();
                return tl::unexpected(ParseError{
                    start, "based number size out of range"});
            }
            d->data()["size"] = make_int(size_val);
        }
        d->data()["signed"] = is_signed ? true_value() : false_value();
        d->data()["base"]   = make_string(std::string(1, base_char));
        d->data()["value"]  = make_string(std::move(digits));
        sr.accept();
        return d;
    };

    // Path 1: unsized based number — input starts with `'`.
    if (*first == '\'') {
        return emit_based(std::string());
    }

    // Path 2: starts with a digit. Consume the integer prefix; this
    // is either the whole integer or the size of a sized-based form.
    if (!is_dec_digit(*first)) {
        sr.reject();
        return tl::unexpected(ParseError{start, "expected number"});
    }
    const std::string int_digits = consume_digits(sr, is_dec_digit, false);

    auto next = sr.peek();
    if (next && *next == '\'') {
        return emit_based(int_digits);
    }

    // Real number? `.` or `e`/`E` after the integer prefix.
    bool is_real = false;
    std::string num_text = int_digits;
    if (next && *next == '.') {
        // Peek past the `.` — must be followed by a digit for a real
        // (so `1.` isn't a real, it's `1` followed by `.`).
        sr.mark();
        sr.get();
        auto after_dot = sr.peek();
        if (after_dot && is_dec_digit(*after_dot)) {
            num_text.push_back('.');
            while (auto c = sr.peek()) {
                if (!is_dec_digit(*c) && *c != '_') break;
                if (*c != '_') num_text.push_back(*c);
                sr.get();
            }
            is_real = true;
            sr.accept();
        } else {
            sr.reject();
        }
    }
    auto exp_peek = sr.peek();
    if (exp_peek && (*exp_peek == 'e' || *exp_peek == 'E')) {
        // Distinguish exponent from time unit / identifier start.
        // After `e`/`E`, must be `[+-]?[0-9]+`.
        sr.mark();
        num_text.push_back(*exp_peek);
        sr.get();
        auto sign = sr.peek();
        if (sign && (*sign == '+' || *sign == '-')) {
            num_text.push_back(*sign);
            sr.get();
        }
        bool any_exp_digit = false;
        while (auto c = sr.peek()) {
            if (!is_dec_digit(*c)) break;
            num_text.push_back(*c);
            sr.get();
            any_exp_digit = true;
        }
        if (!any_exp_digit) {
            // Roll back the exponent attempt; the `e` was a time unit
            // prefix or identifier start.
            sr.reject();
            num_text.pop_back();   // unstack the 'e' we appended
        } else {
            is_real = true;
            sr.accept();
        }
    }

    // Real result: parse as double.
    if (is_real) {
        // std::from_chars for double isn't universally available in
        // older libc++; use strtod with locale-independent C locale
        // (the parser only writes ASCII digits + sign + dot + e).
        double dval = std::strtod(num_text.c_str(), nullptr);
        auto d = std::make_shared<DictValue>();
        d->data()["kind"]  = make_string("real");
        d->data()["value"] = make_real(dval);

        // Real values may also carry a time unit immediately after.
        // Check for that before emitting.
        auto tu_peek = sr.peek();
        if (tu_peek && std::isalpha(static_cast<unsigned char>(*tu_peek))) {
            std::string unit;
            while (auto c = sr.peek()) {
                if (!std::isalpha(static_cast<unsigned char>(*c))) break;
                unit.push_back(*c);
                sr.get();
            }
            if (unit == "s" || unit == "ms" || unit == "us" || unit == "ns"
                || unit == "ps" || unit == "fs") {
                d->data()["kind"] = make_string("time");
                d->data()["unit"] = make_string(std::move(unit));
            } else {
                // Not a time unit — but we consumed the chars. This is
                // a parser error (real number followed by garbage).
                // Reject the whole thing; the grammar can try a
                // different alternative.
                sr.reject();
                return tl::unexpected(ParseError{
                    start, "unexpected suffix after number: '" + unit + "'"});
            }
        }
        sr.accept();
        return d;
    }

    // Plain integer — check for time unit suffix first.
    auto tu_peek = sr.peek();
    if (tu_peek && std::isalpha(static_cast<unsigned char>(*tu_peek))) {
        // Peek the alpha run.
        std::string unit;
        sr.mark();
        while (auto c = sr.peek()) {
            if (!std::isalpha(static_cast<unsigned char>(*c))) break;
            unit.push_back(*c);
            sr.get();
        }
        if (unit == "s" || unit == "ms" || unit == "us" || unit == "ns"
            || unit == "ps" || unit == "fs") {
            sr.accept();
            std::int64_t ival = 0;
            auto [_p, ec] = std::from_chars(
                int_digits.data(), int_digits.data() + int_digits.size(),
                ival);
            if (ec != std::errc{}) {
                sr.reject();
                return tl::unexpected(ParseError{
                    start, "integer out of range"});
            }
            auto d = std::make_shared<DictValue>();
            d->data()["kind"]  = make_string("time");
            d->data()["value"] = make_real(static_cast<double>(ival));
            d->data()["unit"]  = make_string(std::move(unit));
            sr.accept();
            return d;
        }
        // Not a time unit — restore: the alpha run was actually the
        // start of an identifier / keyword that follows the integer.
        sr.reject();
    }

    // Plain unsized integer.
    std::int64_t ival = 0;
    auto [_p, ec] = std::from_chars(
        int_digits.data(), int_digits.data() + int_digits.size(), ival);
    if (ec != std::errc{}) {
        sr.reject();
        return tl::unexpected(ParseError{start, "integer out of range"});
    }
    auto d = std::make_shared<DictValue>();
    d->data()["kind"]  = make_string("integer");
    d->data()["value"] = make_int(ival);
    sr.accept();
    return d;
}

SaveResult SvNumberParser::unparse(const Value& value) const {
    auto dv = dynamic_cast<const DictValue*>(&value);
    if (!dv) {
        return tl::unexpected(SaveError{
            "SvNumberParser::unparse expects DictValue"});
    }
    auto kind_it = dv->data().find("kind");
    if (kind_it == dv->data().end()) {
        return tl::unexpected(SaveError{
            "SvNumberParser::unparse: missing 'kind' field"});
    }
    auto kind_sv = std::dynamic_pointer_cast<StringValue>(kind_it->second);
    if (!kind_sv) {
        return tl::unexpected(SaveError{
            "SvNumberParser::unparse: 'kind' must be a string"});
    }
    const std::string& kind = kind_sv->data();

    if (kind == "integer") {
        auto vit = dv->data().find("value");
        if (vit == dv->data().end()) {
            return tl::unexpected(SaveError{
                "SvNumberParser::unparse: integer needs 'value'"});
        }
        auto iv = std::dynamic_pointer_cast<IntValue>(vit->second);
        if (!iv) {
            return tl::unexpected(SaveError{
                "SvNumberParser::unparse: integer 'value' must be int"});
        }
        return std::to_string(iv->data());
    }

    if (kind == "real") {
        auto vit = dv->data().find("value");
        if (vit == dv->data().end()) {
            return tl::unexpected(SaveError{
                "SvNumberParser::unparse: real needs 'value'"});
        }
        auto rv = std::dynamic_pointer_cast<RealValue>(vit->second);
        if (!rv) {
            return tl::unexpected(SaveError{
                "SvNumberParser::unparse: real 'value' must be real"});
        }
        // std::to_string default precision is 6; for round-trip we
        // want at least 17 significant digits. Use snprintf with %g.
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.17g", rv->data());
        return std::string(buf);
    }

    if (kind == "based") {
        std::string out;
        auto size_it = dv->data().find("size");
        if (size_it != dv->data().end()) {
            if (auto iv = std::dynamic_pointer_cast<IntValue>(size_it->second)) {
                out = std::to_string(iv->data());
            }
        }
        out.push_back('\'');
        auto signed_it = dv->data().find("signed");
        if (signed_it != dv->data().end()) {
            if (auto bv = std::dynamic_pointer_cast<BoolValue>(signed_it->second)) {
                if (bv->data()) out.push_back('s');
            }
        }
        auto base_it = dv->data().find("base");
        if (base_it == dv->data().end()) {
            return tl::unexpected(SaveError{
                "SvNumberParser::unparse: based number needs 'base'"});
        }
        auto bv = std::dynamic_pointer_cast<StringValue>(base_it->second);
        if (!bv) {
            return tl::unexpected(SaveError{
                "SvNumberParser::unparse: 'base' must be a string"});
        }
        out += bv->data();
        auto val_it = dv->data().find("value");
        if (val_it == dv->data().end()) {
            return tl::unexpected(SaveError{
                "SvNumberParser::unparse: based number needs 'value'"});
        }
        auto val_sv = std::dynamic_pointer_cast<StringValue>(val_it->second);
        if (!val_sv) {
            return tl::unexpected(SaveError{
                "SvNumberParser::unparse: based 'value' must be a string"});
        }
        out += val_sv->data();
        return out;
    }

    if (kind == "time") {
        auto vit = dv->data().find("value");
        auto uit = dv->data().find("unit");
        if (vit == dv->data().end() || uit == dv->data().end()) {
            return tl::unexpected(SaveError{
                "SvNumberParser::unparse: time needs 'value' and 'unit'"});
        }
        auto uv = std::dynamic_pointer_cast<StringValue>(uit->second);
        if (!uv) {
            return tl::unexpected(SaveError{
                "SvNumberParser::unparse: time 'unit' must be a string"});
        }
        // Value can be int or real; emit accordingly.
        if (auto iv = std::dynamic_pointer_cast<IntValue>(vit->second)) {
            return std::to_string(iv->data()) + uv->data();
        }
        if (auto rv = std::dynamic_pointer_cast<RealValue>(vit->second)) {
            char buf[64];
            std::snprintf(buf, sizeof buf, "%.17g", rv->data());
            return std::string(buf) + uv->data();
        }
        return tl::unexpected(SaveError{
            "SvNumberParser::unparse: time 'value' must be int or real"});
    }

    return tl::unexpected(SaveError{
        "SvNumberParser::unparse: unknown kind '" + kind + "'"});
}

// --- Group registration -------------------------------------------------

namespace {

ParserGroup make_sv_group() {
    ParserGroup g;
    g.name = "sv";
    g.parsers = {
        // Only the SystemVerilog-specific terminals — string literals
        // and comments use std.string / std.line_comment / std.block_comment.
        ParserSpec{"sv_identifier", []() {
            return std::make_unique<SvIdentifierParser>();
        }},
        ParserSpec{"sv_number", []() {
            return std::make_unique<SvNumberParser>();
        }},
    };
    return g;
}

} // namespace

void register_sv_parser_group() {
    register_parser_group(make_sv_group());
}

} // namespace rawast
