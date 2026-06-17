#pragma once

#include <rawast/node.hpp>
#include <rawast/value.hpp>

#include <string>
#include <vector>

namespace rawast {

class Parser;

// Configuration for the engine's unified byte-scan routine. Both the
// `*` Raw primitive and the `scope { … }` grammar form materialise to
// `walk_scan` invocations with different ScanConfig values — they're
// facets of the same underlying operation.
//
// The byte-scan operation: starting at the stream cursor, consume bytes
// until the stop literal matches at the cursor. INNERs (atomic-span
// recognisers) are tried at each step before falling through to
// raw-byte consumption — when an INNER matches, its matched bytes are
// folded into the captured payload (string container) or its typed
// value is appended (array container), and the scan continues.
//
// Field semantics:
//
//   start          Optional opening delimiter. Matched at scan entry;
//                  empty means the scan starts immediately at the
//                  caller's cursor (the Raw / sibling-stop case).
//
//   stops          The set of literal byte strings that terminate the
//                  byte-scan when any of them matches at the cursor.
//                  Populated from Node.stops (filled by the load-time
//                  resolver in src/loader.cpp). Tried in order; the
//                  first that matches wins.
//
//                  Single-stop case (the historical default —
//                  `sequence { OPEN, scope { ... }, CLOSE }`) → one
//                  entry, the next-sibling Key literal.
//                  Multi-stop case (`sequence { OPEN, repeat scope { ... }
//                  separator SEP, CLOSE }`) → two entries, SEP and CLOSE.
//
//                  Empty stops with no parent-resolution path is an
//                  error at run time.
//
//   start_strict /
//   stop_strict    Word-boundary check on the matching delimiter —
//                  same semantic as Key's `strict` flag. Set by the
//                  single-quote surface forms `start='X'` / `stop='X'`.
//
//   inners         Atomic-span recognisers. For each byte the scan
//                  does not consume as start or stop, it tries each
//                  INNER in order; the first match contributes the
//                  matched bytes (string mode) or its value (array
//                  mode) before the scan continues. Empty for Raw.
//
//   container      Output shape. Container::None → StringValue payload
//                  (the concatenated body bytes). Container::Array →
//                  ArrayValue of segments (one per INNER match plus
//                  StringValue runs for raw-byte gaps).
//
//   subparse_start When valid, the captured payload is re-parsed
//                  through the named rule and the resulting structured
//                  value replaces the raw payload. Same semantic as
//                  the existing `#subparse` annotation.
struct ScanConfig {
    std::string start;
    std::vector<std::string> stops;
    bool start_strict = false;
    bool stop_strict  = false;
    std::vector<NodeId> inners;
    Container container = Container::None;
    NodeId subparse_start;

    // When true, the stop literal is consumed when matched (Scope's
    // semantic — the close delimiter is part of the scope's span).
    // When false, the stop literal is left unconsumed for the next
    // sibling in the surrounding sequence to match (Raw's semantic —
    // `*` is followed by a Key sibling that owns the stop bytes).
    bool consume_stop = true;

    // Caller's active ignore policy at the scope/raw call site. INNER
    // rules that subparse via walk_scan must inherit this — otherwise
    // an INNER without its own `ignore` declaration loses the policy
    // that was active when the scope was entered, and predictive
    // optional checks see raw whitespace instead of the eaten policy.
    // nullptr → no inheritance (treated as empty).
    const std::vector<Parser*>* caller_ignore = nullptr;
};

} // namespace rawast
