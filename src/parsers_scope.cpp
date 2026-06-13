#include <rawast/parsers_scope.hpp>
#include <rawast/parsers_registry.hpp>
#include <rawast/stream.hpp>
#include <rawast/value.hpp>

#include <memory>
#include <string>

namespace rawast {

// --- ScopeParser template implementation --------------------------------

template <char Start, char Stop>
ParseResult ScopeParser<Start, Stop>::parse(StreamReader& sr) {
    sr.mark();
    // Opener must be present at the cursor. If it isn't, the parser
    // fails cleanly — the surrounding rule's other alternatives can
    // still try. Without this check the parser would silently capture
    // arbitrary text up to any same-shape closer it finds later.
    auto first = sr.peek();
    if (!first || *first != Start) {
        sr.reject();
        return tl::unexpected(ParseError{
            sr.position(),
            std::string{"expected '"} + Start + "' for scope opener"});
    }
    sr.get();   // consume the opener

    // Capture body bytes, tracking depth of all four bracket pairs so
    // a closer of one shape doesn't terminate a capture of a different
    // shape. Stop at the matching `Stop` at outermost depth.
    std::string out;
    int paren_depth = 0;
    int brack_depth = 0;
    int brace_depth = 0;
    int angle_depth = 0;
    while (auto c = sr.peek()) {
        if (paren_depth == 0 && brack_depth == 0
            && brace_depth == 0 && angle_depth == 0
            && *c == Stop) {
            break;
        }
        if      (*c == '(') ++paren_depth;
        else if (*c == ')') --paren_depth;
        else if (*c == '[') ++brack_depth;
        else if (*c == ']') --brack_depth;
        else if (*c == '{') ++brace_depth;
        else if (*c == '}') --brace_depth;
        else if (*c == '<') ++angle_depth;
        else if (*c == '>') --angle_depth;
        out.push_back(*c);
        sr.get();
    }

    // Closer must be present where the loop stopped. EOF without a
    // matching closer is a malformed scope — fail with the cursor
    // rewound to the original opener position so an outer choice can
    // try other alternatives.
    auto closer = sr.peek();
    if (!closer || *closer != Stop) {
        sr.reject();
        return tl::unexpected(ParseError{
            sr.position(),
            std::string{"unterminated scope: expected '"} + Stop
                + "' to match opener '" + Start + "'"});
    }
    sr.get();   // consume the closer

    sr.accept();
    return make_string(std::move(out));
}

template <char Start, char Stop>
SaveResult ScopeParser<Start, Stop>::unparse(const Value& v) const {
    auto sv = dynamic_cast<const StringValue*>(&v);
    if (!sv) {
        return tl::unexpected(SaveError{
            std::string{"ScopeParser<"} + Start + "," + Stop
                + ">::unparse expects StringValue"});
    }
    // Round-trip the opener and closer the parser consumed at parse
    // time. Save emits `Start + body + Stop`, restoring the bracketed
    // form whether or not the grammar's outer rule remembers the
    // delimiters.
    std::string out;
    out.reserve(sv->data().size() + 2);
    out.push_back(Start);
    out.append(sv->data());
    out.push_back(Stop);
    return out;
}

// Explicit instantiations for the four standard pairs. The header
// declares only the templates; the linker resolves these instantiations
// from this translation unit.
template class ScopeParser<'(', ')'>;
template class ScopeParser<'[', ']'>;
template class ScopeParser<'{', '}'>;
template class ScopeParser<'<', '>'>;

// --- Group registration -------------------------------------------------

namespace {

ParserGroup make_scope_group() {
    ParserGroup g;
    g.name = "scope";
    g.parsers = {
        ParserSpec{"paren", []() {
            return std::make_unique<ScopeParenParser>("scope.paren");
        }},
        ParserSpec{"bracket", []() {
            return std::make_unique<ScopeBracketParser>("scope.bracket");
        }},
        ParserSpec{"brace", []() {
            return std::make_unique<ScopeBraceParser>("scope.brace");
        }},
        ParserSpec{"angle", []() {
            return std::make_unique<ScopeAngleParser>("scope.angle");
        }},
    };
    return g;
}

} // namespace

void register_scope_parser_group() {
    register_parser_group(make_scope_group());
}

} // namespace rawast
