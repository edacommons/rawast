#pragma once

#include <rawast/parser.hpp>

#include <memory>
#include <string>

namespace rawast {

class Grammar;

// --- "scope" terminal parsers -------------------------------------------
//
// Generic depth-balanced consumption between paired delimiters. The
// parser consumes the OPENER, captures the body, then consumes the
// matching CLOSER. The captured body is returned as a StringValue
// (whitespace preserved verbatim; ignore-set skipping is bypassed).
// Designed for use with the `:subparse="RULE"` binding so the body
// re-parses through the host grammar's expression rule.
//
// Grammars use the dotted form to reference these:
//
//   scope.paren    — `(...)` body
//   scope.bracket  — `[...]` body
//   scope.brace    — `{...}` body
//   scope.angle    — `<...>` body
//
// Self-contained contract:
//
//   * The parser is invoked at the position of the OPENER. It consumes
//     the opener, runs the depth-balancing loop, then consumes the
//     matching closer. If either the opener or closer is missing, the
//     parser fails and the stream is rewound to the start.
//   * Inside the body, the parser tracks the depth of ALL FOUR bracket
//     pairs simultaneously (`()`, `[]`, `{}`, `<>`) so a closer
//     embedded in a nested different-shape bracket doesn't terminate
//     the capture early. `(a, {b, c}, [d])` correctly captures
//     ` a, {b, c}, [d] ` when called via `scope.paren`.
//   * Save emits `Start + body + Stop` — the grammar doesn't need
//     separate Keys for the opener/closer.
//
// Why a dedicated `scope` group: these are completely language-
// agnostic. The existing `sv_balanced_braces` / `sv_balanced_brackets`
// duplicate the depth logic but leave opener/closer for the outer
// rule to handle — the scope group encapsulates the whole pair so
// grammar usage is one parser invocation instead of three nodes
// (Key + parser + Key).
//
// `sv_balanced_arg` is NOT promoted to this group: it has the SV-
// specific "stop at top-level comma" behavior for splitting function-
// like macro arg lists. That's an SV macro-args concern, not a
// generic scope concern.

// Template implementation of all four scope parsers. The Start/Stop
// template parameters specify the opener/closer byte pair the parser
// claims; the depth-balancing loop tracks all four bracket pairs
// uniformly so nested brackets of different shapes don't terminate
// early.
template <char Start, char Stop>
class ScopeParser final : public Parser {
public:
    explicit ScopeParser(std::string name) : Parser(std::move(name)) {}

    ParseResult parse(StreamReader& sr) override;
    SaveResult  unparse(const Value& value) const override;
};

// Type aliases for the four standard pairs. Definitions live in
// parsers_scope.cpp via the template's explicit instantiation.
using ScopeParenParser   = ScopeParser<'(', ')'>;
using ScopeBracketParser = ScopeParser<'[', ']'>;
using ScopeBraceParser   = ScopeParser<'{', '}'>;
using ScopeAngleParser   = ScopeParser<'<', '>'>;

// Register the "scope" parser group in the global registry. Grammars
// declare `use: scope` to pull in `scope.paren`, `scope.bracket`,
// `scope.brace`, `scope.angle`. Idempotent — safe to call from
// multiple init paths.
void register_scope_parser_group();

} // namespace rawast
