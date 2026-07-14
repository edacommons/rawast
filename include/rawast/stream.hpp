#pragma once

#include <cstddef>
#include <istream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace rawast {

class StreamReader;  // defined below

// Public handoff type for parser input. The canonical type passed to
// Grammar::parse and produced by Preprocessor::preprocess. Owns the
// backing (istream + StreamReader cursor + lifetime ownership of any
// upstream state); engine consumers use `reader()` to drive the parse.
//
// All input producers go through Stream factories or are constructed
// directly by Preprocessor::preprocess(). All input consumers
// (Grammar::parse, Grammar::parse_from) take Stream&. There is no
// separate "wrap an istream / StreamReader / string" entry — every
// parse goes through Stream.
class Stream {
public:
    // Construct a Stream backed by an in-memory string.
    static Stream from_string(std::string source);

    // Construct a Stream that reads from a file path. The file handle
    // is owned by the Stream — closed when the Stream is destroyed.
    // Throws std::runtime_error on open failure.
    static Stream from_file(const std::string& path);

    // Construct from an arbitrary istream + an owner that keeps any
    // upstream backing-state alive (e.g. Preprocessor::preprocess()
    // passes its expanded string buffer here so the istream can read
    // from it after preprocess() returns).
    Stream(std::unique_ptr<std::istream> is,
           std::shared_ptr<void> owner = {});

    Stream(Stream&&) noexcept;
    Stream& operator=(Stream&&) noexcept;
    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;
    ~Stream();

    // Engine-facing cursor. Holds the mark/accept/reject machinery the
    // parse driver uses. Stable across moves of the Stream because
    // the underlying istream is heap-allocated (unique_ptr) — moving
    // the Stream doesn't relocate the istream object.
    StreamReader& reader() noexcept { return *reader_; }

private:
    std::unique_ptr<std::istream> is_;
    std::unique_ptr<StreamReader> reader_;
    std::shared_ptr<void> owner_;  // type-erased backing-state owner
};

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

    // Line-directive support. A grammar token bound with the engine
    // annotation `#linenum` calls set_line() with its integer value;
    // `#filename` calls set_file(). This lets a `\`line N "f" 0`-style
    // directive re-sync the reader's line counter so downstream error
    // positions track the ORIGINAL source, not the preprocessed stream.
    //
    // set_line is a literal setter (line_ = n). The line-directive
    // convention ("the line FOLLOWING the directive is N") is applied
    // by the caller, which passes N-1: the directive line's own
    // terminating newline then bumps line_ from N-1 to N. This is
    // robust to trailing blank lines (each counts), matching `\`line.
    void set_line(std::size_t n) noexcept { line_ = n; column_ = 1; }
    void set_file(std::string f) { current_file_ = std::move(f); }
    const std::string& current_file() const noexcept { return current_file_; }

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
    std::string current_file_;
    std::vector<MarkData> marks_;

    void advance_position(char c) noexcept;
};

} // namespace rawast
