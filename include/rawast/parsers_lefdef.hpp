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

// LEF58_-prefixed name parser. Same identifier shape as
// LefdefIdentifierParser but FAILS unless the input starts with the
// literal `LEF58_` prefix. Lets a grammar discriminate the
// `PROPERTY LEF58_TYPE "..."` / `PROPERTY LEF58_SPACING "..."`
// family of clauses from generic `PROPERTY name value ;` clauses
// without a closed-keyword Choice — the prefix is the
// discriminator. Returns the full matched name including the
// `LEF58_` prefix (e.g. `"LEF58_TYPE"`), so save unparse just
// emits the string back verbatim.
//
// Use case: a downstream consumer iterating `layer.lef58` finds
// every LEF58_* vendor-extension property as a typed sub-object
// rather than filtering `layer.properties` by name string.
class LefdefLef58NameParser final : public Parser {
public:
    LefdefLef58NameParser();
    ParseResult parse(StreamReader& sr) override;
    SaveResult  unparse(const Value& value) const override;
};

// Retired: `LefdefUntilEndextParser`. The "consume raw bytes until
// a specific keyword" pattern is now a grammar-level primitive (`*`
// followed by a Key literal); see docs/rawast-format.md §4.5a and
// the BEGINEXT_BLOCK rule in lef.rawast.

// Register the "lefdef" parser group in the global registry. Grammars
// declare `use: lefdef` to pull in identifier + line_comment.
// Idempotent — safe to call from multiple init paths.
void register_lefdef_parser_group();

} // namespace rawast
