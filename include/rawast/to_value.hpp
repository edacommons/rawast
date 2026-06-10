#pragma once

#include <rawast/grammar.hpp>
#include <rawast/value.hpp>

namespace rawast {

// Walk an in-memory `Grammar` and produce the dict / array / scalar
// Value tree in the same shape the loader's `populate()` consumes.
// The inverse of `load_json_grammar_into()` (and of the meta-grammar's
// parse direction).
//
// Same code form for both directions:
//
//   text/.rawast  ──parse──▶  Value tree  ──populate──▶  Grammar
//                             │                          │
//                             │                          │
//                             ◄──── to_value(grammar) ───┘
//
// The returned dict can be passed back through `load_json_grammar_into()`
// to reconstruct an equivalent Grammar. Round-trip invariant:
//
//   Grammar g1 = ...;
//   ValuePtr v = to_value(g1);
//   Grammar g2;  // configured with same parser groups
//   load_json_grammar_into(g2, *v);
//   // g1 and g2 are structurally equivalent.
//
// Note on parser groups: `to_value` emits a `use:` array derived from
// the dotted aliases registered in the parser registry (every group
// member is registered under both bare and "group.bare" names). If
// the source grammar registered parsers without going through a
// parser group, the consumer of the emitted value will need to
// register those parsers separately before calling
// `load_json_grammar_into()`.
//
// Used by:
//   * `rawast cppgen` (issue #2) — emit C++ source that reconstructs
//     the Value tree via DictValue/ArrayValue make calls, then loads
//     it via `load_json_grammar_into()`.
//   * `.jast` writer (M2 roadmap) — serialise the grammar half of the
//     bundle as the dict shape.
//   * Python `grammar.to_dict()` API for inspection / debugging.
//   * Loader round-trip self-consistency tests.
ValuePtr to_value(const Grammar& g);

} // namespace rawast
