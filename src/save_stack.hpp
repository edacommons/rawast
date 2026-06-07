// Phase B save engine — stack-navigation rewrite.
//
// Replaces the flat-cursor save (`SaveCursor` + recursive `save_node`)
// with a two-pointer walk: a stack of value-tree positions, threaded
// against the grammar tree node by node. This gives us:
//
//   * Key-based Choice dispatch — an alternative can match because
//     the current dict key equals a literal Key token, not because
//     a Value-kind discriminator child matches.
//   * Wrapped sub-structure descent — a non-consuming Sequence
//     (container=None, optional) with internal Value-name markers
//     can pull fields from the surrounding dict by name.
//   * Catch-all alternatives — a Choice alternative without explicit
//     discriminators is tried last, accepting anything remaining.
//   * Bare-string Ref dispatch — a Choice alternative whose shape is
//     a bare identifier (REF in the meta-grammar) matches when the
//     value at the dispatch point is a string.
//
// The old engine survives as a fallback until B1 is green; once
// shipped grammars round-trip through this engine, the old code is
// deleted.
//
// Entry point: `save_v2(grammar, out, value, pretty)` — same signature
// as `Grammar::save`. The Grammar's top_ drives the walk.

#pragma once

#include <rawast/node.hpp>
#include <rawast/value.hpp>

#include <tl/expected.hpp>

#include <ostream>

namespace rawast {

class Grammar;
struct SaveError;

// `start` is the NodeId of the rule to dispatch from — pass an
// invalid NodeId (the default-constructed one) to fall back to
// `g.top()`. Lets a single grammar carry multiple top-level rules
// (LEF vs DEF in one file) and have the host pick which to save
// against.
tl::expected<void, SaveError>
save_v2(const Grammar& g, std::ostream& out, const ValuePtr& root,
        bool pretty, NodeId start = {});

} // namespace rawast
