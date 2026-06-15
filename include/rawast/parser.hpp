#pragma once

#include <rawast/stream.hpp>
#include <rawast/value.hpp>

#include <tl/expected.hpp>

#include <string>
#include <utility>

namespace rawast {

// Error produced by a terminal-parser failure. The position is captured at
// the point the parser was entered, not where it gave up — that's typically
// the most useful position for the engine's max-progress diagnostic.
struct ParseError {
    Position position;
    std::string message;
};

using ParseResult = tl::expected<ValuePtr, ParseError>;

// Error produced when emitting (the inverse of parsing). No position
// concept — save is value-tree-driven, not stream-driven.
struct SaveError {
    std::string message;
};

using SaveResult = tl::expected<std::string, SaveError>;

using WalkResult = tl::expected<void, ParseError>;

// Terminal-parser interface, astrw-style split.
//
// Three primitives, each with a clear contract:
//   walk(sr)  — scan bytes from `sr`, accumulating the matched text into
//               `accum_`. On success the stream stays advanced; on
//               failure the stream is rewound to its entry position
//               (parsers do this via StreamReader::mark/accept/reject).
//               Pure virtual: every parser implements this.
//   value()   — convert `accum_` into a typed ValuePtr. Default returns
//               the accumulator as a StringValue. Override for typed
//               parsers (IntParser → IntValue, FloatParser → FloatValue,
//               etc.) and for parsers that carry typed accumulation
//               state in additional members.
//   reset()   — clear accumulated state before the next walk. Default
//               clears `accum_`; override if the parser holds extra
//               state.
//
// parse(sr) is the legacy single-shot entry: `reset(); walk(sr); value();`.
// The engine's standard Parse-node dispatch calls parse() so the default
// path is unchanged. The `scope { ... }` driver, by contrast, calls only
// `reset() + walk()` on each INNER — it never asks for value(), avoiding
// the per-step typed-Value allocation when scanning a bracketed region.
//
// unparse() is the inverse: given a Value, produce the text that would
// parse back into it. Default implementation returns SaveError; terminals
// that only make sense in one direction (comment parsers, whitespace) can
// leave the default in place.
class Parser {
    std::string name_;
protected:
    // Astrw-style accumulator. Walk fills this; value() converts it;
    // reset() clears it. Parsers that need typed accumulation (e.g.
    // SignedIntParser holding the sign separately) declare their own
    // members and override reset() / value() accordingly — accum_ is
    // shared infrastructure, not mandatory state.
    std::string accum_;

public:
    explicit Parser(std::string name) noexcept : name_(std::move(name)) {}
    virtual ~Parser() = default;

    Parser(const Parser&) = delete;
    Parser& operator=(const Parser&) = delete;

    const std::string& name() const noexcept { return name_; }

    // Scan bytes, accumulate into `accum_` (or parser-specific state).
    // Pure virtual — every Parser subclass implements this.
    virtual WalkResult walk(StreamReader& sr) = 0;

    // Convert accumulated state → typed ValuePtr. Default: StringValue
    // of accum_. Override for typed parsers (Int, Float, …) or for
    // parsers whose value isn't textual (Padding → IntValue count,
    // comments → null_value(), …).
    virtual ValuePtr value() const { return make_string(accum_); }

    // Raw matched-text accessor. Default returns `accum_`. Parsers
    // that don't store the matched text in `accum_` (LinespaceParser,
    // typed-state parsers like GdsiiPaddingParser) return empty —
    // `scope { ... }`'s scan loop falls back to capturing raw bytes
    // via the pre/post-walk position delta in that case.
    virtual const std::string& text_value() const { return accum_; }

    // Clear accumulated state. Default clears `accum_`. Override if
    // the parser carries extra typed state (GdsiiPaddingParser::count_,
    // GdsiiRecordParser::decoded_, …).
    virtual void reset() { accum_.clear(); }

    // Single-shot entry: `reset(); walk(sr); return value();`. The
    // standard Parse-node engine dispatch calls this; the scope
    // driver calls reset() + walk() directly and skips value() when
    // it just needs byte advance. Left virtual for ABI consistency
    // and override flexibility, but no production override remains.
    virtual ParseResult parse(StreamReader& sr) {
        reset();
        auto wr = walk(sr);
        if (!wr) return tl::unexpected(wr.error());
        return value();
    }

    virtual SaveResult unparse(const Value& /*value*/) const {
        return tl::unexpected(SaveError{
            "parser '" + name_ + "' does not implement unparse"});
    }
};

} // namespace rawast
