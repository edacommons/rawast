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

// SystemVerilog numeric literal parser. Recognises all forms per §5.7:
//
//   unsized integer       42, 1_000_000 (underscores allowed except as
//                         first/last char; they're discarded)
//
//   sized based integer   8'hFF, 4'b0101, 16'd42, 32'o777
//                         signed forms: 8'shFF, 4'sb01
//                         x/z digits:    4'bxxxx, 8'h?? (? = z)
//
//   unsized based integer 'h42 (default 32-bit width per LRM)
//
//   real number           42.5, 1.5e10, 1.5E-3, 1e10
//
//   time literal          42ns, 1.5us, 100ps (number + time unit)
//
// Returns a DictValue with a `kind` discriminator:
//
//   {"kind": "integer", "value": <int64>}
//   {"kind": "real",    "value": <double>}
//   {"kind": "based",   "size": <int|null>, "signed": <bool>,
//                       "base": "b"|"o"|"d"|"h", "value": "<digit-string>"}
//   {"kind": "time",    "value": <double>, "unit": "s"|"ms"|"us"|"ns"|"ps"|"fs"}
//
// The digit string for "based" is kept as text (rather than converted
// to int) because: (a) x/z digits aren't representable as integers,
// (b) widths up to 2^31 bits are legal per LRM and can overflow int64.
// The host can convert to bit-pattern when needed.
class SvNumberParser final : public Parser {
public:
    SvNumberParser();
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
