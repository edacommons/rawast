#pragma once

#include <rawast/node.hpp>
#include <rawast/value.hpp>

#include <string>
#include <vector>

namespace rawast {

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
//   stop           Closing-condition literal. The byte-scan terminates
//                  when this literal matches at the cursor. For Raw
//                  nodes this is the pre-resolved next-sibling literal
//                  (populated by `resolve_raw_stops` at load time); for
//                  Scope nodes it's the user's `stop="…"` attribute.
//
//                  Empty stop with no parent-resolution path is an
//                  error at run time; sibling-stop on Scope lands in
//                  Phase 3.
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
    std::string stop;
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
};

} // namespace rawast
