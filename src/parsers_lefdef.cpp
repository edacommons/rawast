#include <rawast/parsers_lefdef.hpp>
#include <rawast/parsers_registry.hpp>
#include <rawast/stream.hpp>
#include <rawast/value.hpp>

#include <memory>
#include <string>

namespace rawast {

namespace {

// Stop characters for identifier consumption. Whitespace stops via the
// !isgraph check; everything below is also a stop:
//   `; ( ) " +`
// `#` is NOT a stop — see header note on comment handling.
// `-` is NOT a stop either — LEF allows hyphens inside identifier
// names (e.g. `Via1Array-0`, `SD_GATE-Array`). DEF's `-` record
// separator (`- name + NET ...`) is consumed as a literal token by
// the structural grammar BEFORE the identifier parser is invoked
// on `name`, so the identifier parser never sees the leading `-`.
bool is_identifier_stop(char c) {
    switch (c) {
    case ' ': case '\t': case '\n': case '\r':
    case ';': case '(': case ')': case '"':
    case '+':
        return true;
    default:
        return false;
    }
}

} // namespace

// --- LefdefIdentifierParser ---------------------------------------------

LefdefIdentifierParser::LefdefIdentifierParser() : Parser("identifier") {}

ParseResult LefdefIdentifierParser::parse(StreamReader& sr) {
    sr.mark();
    const Position start = sr.position();

    std::string ident;
    while (true) {
        auto c = sr.peek();
        if (!c || is_identifier_stop(*c)) break;
        ident.push_back(*c);
        sr.get();
    }

    if (ident.empty()) {
        sr.reject();
        return tl::unexpected(ParseError{start, "expected LEF/DEF identifier"});
    }

    sr.accept();
    return make_string(std::move(ident));
}

SaveResult LefdefIdentifierParser::unparse(const Value& value) const {
    auto sv = dynamic_cast<const StringValue*>(&value);
    if (!sv) {
        return tl::unexpected(SaveError{
            "LefdefIdentifierParser::unparse expects StringValue"});
    }
    return sv->data();
}

// --- LefdefLineCommentParser --------------------------------------------

LefdefLineCommentParser::LefdefLineCommentParser() : Parser("line_comment") {}

ParseResult LefdefLineCommentParser::parse(StreamReader& sr) {
    sr.mark();
    const Position start = sr.position();

    auto first = sr.peek();
    if (!first || *first != '#') {
        sr.reject();
        return tl::unexpected(ParseError{start, "expected '#' line comment"});
    }

    // Consume `#` and everything up to and including the newline (or EOF).
    while (true) {
        auto c = sr.get();
        if (!c) break;
        if (*c == '\n') break;
    }

    sr.accept();
    return null_value();
}

// --- Group registration -------------------------------------------------
//
// The previous `until_endext` raw-text terminal parser was retired
// when the engine grew the `*` grammar-level primitive (commit
// after 2048494). Grammars that need "consume raw bytes until X"
// behaviour now write `*:body=@, "X" newline` in the body itself.

ParserGroup make_lefdef_group() {
    ParserGroup g;
    g.name = "lefdef";
    g.parsers = {
        ParserSpec{"identifier",   []() {
            return std::make_unique<LefdefIdentifierParser>();
        }},
        ParserSpec{"line_comment", []() {
            return std::make_unique<LefdefLineCommentParser>();
        }},
    };
    return g;
}

void register_lefdef_parser_group() {
    register_parser_group(make_lefdef_group());
}

} // namespace rawast
