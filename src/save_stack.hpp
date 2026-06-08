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
// Entry point: `Grammar::save` (declared in grammar.hpp,
// defined in save_stack.cpp). The Grammar's top_ drives the
// walk unless an explicit `start` NodeId is passed (for
// multi-top-rule grammars like LEF+DEF in one file).

#pragma once

#include <rawast/node.hpp>
#include <rawast/value.hpp>

#include <tl/expected.hpp>

#include <ostream>

// No public declarations needed — the save entry point is
// `Grammar::save` (declared in include/rawast/grammar.hpp),
// defined directly in src/save_stack.cpp. Helper functions in
// this translation unit live in an anonymous namespace.
