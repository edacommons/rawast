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

// Consume text until the closing `}` at OUTERMOST depth. Tracks
// nesting of `{}`, `()`, AND `[]` so commas, semicolons, and
// closers inside nested brackets stay part of the captured run
// instead of terminating early.
//
// Used for class constraint blocks (where the body has
// `data inside { [0:1000], [10000:20000] }` patterns), enum/
// struct/union bodies with nested `{...}` defaults, and any
// other raw-body capture where the content might contain
// brace-nested expressions.
//
// The closing `}` is NOT consumed — left for the outer rule's
// next sibling Key to match.
class SvBalancedBracesParser final : public Parser {
public:
    SvBalancedBracesParser();
    ParseResult parse(StreamReader& sr) override;
    SaveResult  unparse(const Value& value) const override;
};

// Consume text until the closing `]` at OUTERMOST depth. Tracks
// nesting of `[]`, `()`, and `{}` so commas, semicolons, and
// inner brackets stay part of the captured run. Used for array
// index/dimension suffixes that may contain nested types like
// `[bit[31:0]]` (associative array keyed by a bit vector type)
// where the inner `[31:0]` is part of the key type, not the outer
// suffix terminator.
class SvBalancedBracketsParser final : public Parser {
public:
    SvBalancedBracketsParser();
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

// Consume one line of source up to and including the trailing newline,
// returning the captured run as a StringValue. UNLIKE `sv_line_text`,
// this parser fails immediately when the cursor is at the start of a
// preprocessor terminator directive — `\`endif` or `\`else`. The fail
// is by design: it lets a `PP_FILE: repeat <PP_ITEM>` rule terminate
// cleanly at conditional-compilation boundaries (the outer PP_IFDEF
// then matches its own `\`endif`).
//
// Word boundary check on the terminator: `\`endifx` (some user macro)
// IS consumed, while `\`endif`, `\`endif // …`, `\`endif\n` etc. are
// all rejected. Same for `\`else` vs `\`elsex`.
//
// The captured line keeps its trailing newline so passthrough
// preserves source line structure. EOF without a newline is fine —
// whatever was read is returned.
class SvPpTextLineParser final : public Parser {
public:
    SvPpTextLineParser();
    ParseResult parse(StreamReader& sr) override;
    SaveResult  unparse(const Value& value) const override;
};

// Consume exactly one line terminator: `\n`, `\r\n`, or `\r`. Used
// by the preprocessor grammar to mark the end of single-line
// directives (`\`define`, `\`undef`, etc.) — those rules can't use
// the meta-grammar's `newline` postfix attr (which is a save-side
// pretty-print flag, not a parse-side terminal) and the standard
// `std` parser group doesn't ship an end-of-line parser. Returns
// the consumed terminator bytes as a StringValue so unparse can
// round-trip the original line ending.
//
// Fails at end-of-input — directive lines that aren't newline-
// terminated would otherwise be hidden parse failures; preferring
// a structured fail surfaces them as preprocessor warnings.
class SvEolParser final : public Parser {
public:
    SvEolParser();
    ParseResult parse(StreamReader& sr) override;
    SaveResult  unparse(const Value& value) const override;
};

// SystemVerilog identifier parser specialized for preprocessor
// macro-use sites: reads a `sv_identifier`-shaped name but fails
// on the small set of preprocessor terminator keywords that must
// NOT be matched as macro names — currently `endif` and `else`.
//
// Used by `PP_MACRO_USE` in `sv_preprocessor.rawast`: a `\``
// followed by an identifier is a macro use unless the identifier
// is one of those terminators (in which case the surrounding
// `\`ifdef`/`\`ifndef` rule needs to claim it). Returns the
// matched name as a StringValue.
class SvPpMacroNameParser final : public Parser {
public:
    SvPpMacroNameParser();
    ParseResult parse(StreamReader& sr) override;
    SaveResult  unparse(const Value& value) const override;
};

// SystemVerilog system task / function name including multi-segment
// forms like `$value$plusargs`, `$test$plusargs`. Matches a leading
// `$`, then one or more `$`-separated identifier segments. Returns
// the matched string including all `$` characters. Used by
// SYSTEM_FUNC_CALL / SYSTEM_TASK_CALL in the SV grammar.
class SvSystemNameParser final : public Parser {
public:
    SvSystemNameParser();
    ParseResult parse(StreamReader& sr) override;
    SaveResult  unparse(const Value& value) const override;
};

// A SystemVerilog type name — either a plain identifier or a
// package-qualified one (`name` or `pkg::name`). Returns the matched
// text as a StringValue, including the `::` when present. Used in
// grammar positions where the SV LRM allows `class_scope`/`package_scope`
// prefixes on a type identifier — parameter types, user-type variable
// declarations, ANSI port types, function return types, etc.
//
// Letting one terminal handle both forms collapses what used to need
// two parallel grammar rules (a bare-identifier variant and a separate
// `pkg::name` variant tried before it because PEG choice commits on
// the first match). The terminal optionally consumes the second
// segment, so the surrounding rule writes `sv_qualified_type:type=@`
// once and gets both forms for free.
class SvQualifiedTypeParser final : public Parser {
public:
    SvQualifiedTypeParser();
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
