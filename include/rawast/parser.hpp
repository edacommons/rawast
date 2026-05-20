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

// Terminal-parser interface.
//
// Contract: parse() marks the stream on entry; on success it accepts the
// mark (the stream stays advanced) and returns the produced ValuePtr; on
// failure it rejects the mark (the stream is rewound to its entry position)
// and returns an unexpected ParseError.
class Parser {
    std::string name_;
public:
    explicit Parser(std::string name) noexcept : name_(std::move(name)) {}
    virtual ~Parser() = default;

    Parser(const Parser&) = delete;
    Parser& operator=(const Parser&) = delete;

    const std::string& name() const noexcept { return name_; }

    virtual ParseResult parse(StreamReader& sr) = 0;
};

} // namespace rawast
