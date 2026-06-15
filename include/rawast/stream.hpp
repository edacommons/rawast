#pragma once

#include <cstddef>
#include <istream>
#include <optional>
#include <string>
#include <vector>

namespace rawast {

// Snapshot of a position in the input stream. 1-based line/column,
// 0-based absolute byte offset.
struct Position {
    std::size_t bytes  = 0;
    std::size_t line   = 1;
    std::size_t column = 1;
};

// Stream-backed input with nested mark/accept/reject checkpoints.
//
// While at least one mark is live, consumed bytes are retained so that
// reject() can rewind. When all marks have been accepted or rejected,
// the consumed prefix is trimmed permanently. This makes backtracking
// inside terminal parsers cheap while keeping memory bounded over very
// long inputs.
class StreamReader {
public:
    explicit StreamReader(std::istream& is) noexcept : is_(is) {}

    StreamReader(const StreamReader&) = delete;
    StreamReader& operator=(const StreamReader&) = delete;

    // Read the next byte, advancing the cursor. Returns nullopt at EOF.
    std::optional<char> get();

    // Look at the next byte without advancing. Returns nullopt at EOF.
    std::optional<char> peek();

    // True iff get() / peek() would return nullopt.
    bool eof();

    // Save the current position as a checkpoint.
    void mark();

    // Pop the most recent checkpoint, committing the current position.
    void accept();

    // Pop the most recent checkpoint, rewinding the cursor to it.
    void reject();

    std::size_t mark_count() const noexcept { return marks_.size(); }
    Position position() const noexcept;

    // Return bytes from absolute position `start` up to (but not
    // including) the current cursor. Used by the `scope { ... }`
    // driver to capture an INNER's matched-text span verbatim,
    // including delimiters the INNER parser may strip from its own
    // value (e.g. DoubleQuoteStringParser drops the outer `"`s).
    //
    // Contract: caller MUST hold a live mark covering `start` —
    // otherwise the retained buffer has been trimmed past `start`
    // and the call returns an empty / truncated string. The scope
    // driver holds its entry mark for the whole scope-body span,
    // so this is always satisfied there.
    std::string bytes_from(const Position& start) const;

private:
    struct MarkData {
        std::size_t pos;
        std::size_t line;
        std::size_t column;
    };

    std::istream& is_;
    std::string buf_;
    std::size_t pos_      = 0;
    std::size_t consumed_ = 0;
    std::size_t line_     = 1;
    std::size_t column_   = 1;
    std::vector<MarkData> marks_;

    void advance_position(char c) noexcept;
};

} // namespace rawast
