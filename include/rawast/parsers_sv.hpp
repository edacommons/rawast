#pragma once

#include <rawast/parser.hpp>

namespace rawast {

class Grammar;

// --- SystemVerilog terminal parsers ---
//
// Implementations follow IEEE 1800-2017 lexical rules (which preserve
// and extend the IEEE 1364-2001 Verilog rules — Verilog is a subset
// of SystemVerilog since the 2009 unification). The parsers cover the
// full SV lexical surface; the grammar layer decides which subset of
// SystemVerilog the parser accepts.
//
// Each parser is stateless across `parse()` calls — its state lives
// in the StreamReader. The parser group registers each parser under
// both a bare name (e.g. `sv_identifier`) and a dotted alias
// (`sv.sv_identifier`) — see parsers_registry.cpp.

// SystemVerilog identifier parser. Recognises three forms per §5.6:
//
//   simple    [a-zA-Z_][a-zA-Z_0-9$]*       e.g. `clk`, `data_in`, `i$1`
//   escaped   \<chars-until-whitespace>     e.g. `\foo bar` → name "foo bar"
//                                           (the leading `\` and the
//                                            terminating whitespace are NOT
//                                            part of the identifier)
//   system    $<simple-identifier>          e.g. `$display`, `$bits`
//
// Returns the matched identifier as a StringValue (without the `\`
// for escaped form, with the `$` retained for system names so the
// grammar can dispatch on the leading byte).
class SvIdentifierParser final : public Parser {
public:
    SvIdentifierParser();
    ParseResult parse(StreamReader& sr) override;
    SaveResult  unparse(const Value& value) const override;
};

// SystemVerilog based-number digit-run parser. Consumes the digit
// portion that follows a base specifier in a sized or unsized based
// literal — the `FF` of `8'hFF`, the `0101` of `4'b0101`, the `xxxx`
// of `4'bxxxx`, the `??` of `8'h??`. Accepts any of `[0-9a-fA-FxXzZ?]`
// plus underscores (LRM permits x/z/`?` in any base; validity per
// actual base is the host's concern). Underscores are stripped on
// the way in.
//
// This is the ONLY genuinely Verilog-specific number-lexing concern;
// everything else (plain int, real, time literal) composes naturally
// from `std.int` / `std.float` + grammar Choice. See the BASED_NUM
// rule in `grammars/systemverilog.rawast`.
//
// Returns a StringValue holding the digit run with underscores
// removed (so `8'h1_0_0_0` parses to `"1000"`).
class SvBasedDigitsParser final : public Parser {
public:
    SvBasedDigitsParser();
    ParseResult parse(StreamReader& sr) override;
    SaveResult  unparse(const Value& value) const override;
};

// Consume one argument of a function-like macro call: bytes up to
// the next top-level `,` or `)`, with nested `()` properly balanced.
// `(a, foo(x, y), c)` yields three args: `a`, ` foo(x, y)`, ` c`.
//
// Depth-tracked in the parser itself (it counts `(`/`)`) rather than
// expressed via grammar recursion. Grammar recursion would also
// solve nesting, but only yields each arg as a tree of (text +
// nested-paren) chunks the host has to flatten. The depth-tracking
// scan returns each arg as a single string — simpler IR for the
// most common host use case (textual macro expansion).
//
// Returns the matched arg text. Always succeeds — for an empty arg
// (e.g. `MACRO()` or `MACRO(,)`), returns an empty string.
//
// Future generalization: a `text_until_delim` parser configurable
// with stop/depth chars would cover macro args, brace-balanced
// blocks, bracket-balanced array refs, etc. Saved for whenever a
// second use case lands.
class SvBalancedArgParser final : public Parser {
public:
    SvBalancedArgParser();
    ParseResult parse(StreamReader& sr) override;
    SaveResult  unparse(const Value& value) const override;
};

// Consume a line of text, terminating at the next un-escaped newline.
// Handles the standard `\` line-continuation: a backslash IMMEDIATELY
// before a newline causes the parser to include both and keep going,
// allowing multi-line `define` bodies.
//
// Trailing horizontal whitespace is trimmed. The terminating newline
// is NOT consumed — left in the stream for the outer rule's ignore
// policy to handle on the next iteration.
//
// Used together with `std.linespace` for the body of `define
// directives where rawast's `*` raw-consume can't be applied (it
// requires a literal Key terminator, not a Parse like `newline`).
class SvLineTextParser final : public Parser {
public:
    SvLineTextParser();
    ParseResult parse(StreamReader& sr) override;
    SaveResult  unparse(const Value& value) const override;
};

// Note: SystemVerilog strings and comments are handled by the `std`
// group's existing parsers:
//
//   std.string         — `"…"` with backslash escapes (pass-through,
//                        preserves source form for round-trip)
//   std.line_comment   — `// …` to end of line
//   std.block_comment  — `/* … */` block
//
// These match the SV LRM §5.4 and §5.9 surface forms exactly, so
// the SV grammar declares `use: std, sv` and references them via
// `std.string`, `std.line_comment`, `std.block_comment` (or bare
// names where unambiguous).

// Register the "sv" parser group in the global registry.
// Grammars declare `use: sv` to pull in `sv_identifier` and
// `sv_number`. Idempotent — safe to call from multiple init paths.
void register_sv_parser_group();

} // namespace rawast
