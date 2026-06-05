#pragma once

#include <rawast/parser.hpp>

namespace rawast {

class Grammar;

// LEF/DEF identifier parser. Greedy consume of one-or-more characters
// that are not whitespace nor one of `; ( ) " + -`. Returns the matched
// run as a StringValue. The `#` character is allowed inside identifiers
// — line-comment skipping is handled by lefdef_line_comment as an
// ignore-only parser, and the rawast ignore round only fires between
// top-level parser invocations (i.e. after whitespace, never mid-token),
// so the spec's "comment only when preceded by whitespace" rule falls
// out naturally without explicit lookback.
//
// Bus-bit characters (`[ ]` by default) and the divider character (`/`
// by default) are treated as ordinary identifier characters; the
// application layer interprets `M1[7:0]` or `inst/pin` semantically.
class LefdefIdentifierParser final : public Parser {
public:
    LefdefIdentifierParser();
    ParseResult parse(StreamReader& sr) override;
    SaveResult  unparse(const Value& value) const override;
};

// LEF/DEF line-comment parser. Matches `#` then everything up to (and
// including) the next newline. Intended for use only on a grammar's
// `ignore:` list; unparse is not implemented (comments are not
// reconstructible from value trees).
class LefdefLineCommentParser final : public Parser {
public:
    LefdefLineCommentParser();
    ParseResult parse(StreamReader& sr) override;
};

// LEF/DEF `BEGINEXT ... ENDEXT` body parser. Consumes the raw text
// between the BEGINEXT opener (already matched by the surrounding
// grammar) and the next `ENDEXT` keyword (NOT consumed — the grammar
// matches it as a structural keyword after this parser returns).
// Returns the captured content as a StringValue, including embedded
// whitespace and newlines. Used by lef.rawast's BEGINEXT_BLOCK rule
// to round-trip vendor extension blocks losslessly.
class LefdefUntilEndextParser final : public Parser {
public:
    LefdefUntilEndextParser();
    ParseResult parse(StreamReader& sr) override;
    SaveResult  unparse(const Value& value) const override;
};

// Register the "lefdef" parser group in the global registry. Grammars
// declare `use: lefdef` to pull in identifier + line_comment.
// Idempotent — safe to call from multiple init paths.
void register_lefdef_parser_group();

} // namespace rawast
