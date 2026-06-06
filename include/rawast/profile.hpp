#pragma once

#include <rawast/node.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rawast {

// Per-rule profiling counters collected during a `parse_from` call.
// One entry per node that was entered at least once; named rules get
// their `rule_name` populated, anonymous (inline) nodes get an empty
// string. Times are wall-clock nanoseconds measured at frame
// push/pop.
//
// `total_ns` is the sum of all elapsed times the frame for this node
// spent on the parse stack — including time spent in nested child
// frames. Self-time (excluding children) is not tracked in this MVP;
// add it when the inclusive numbers point at something that needs
// breaking down further.
//
// `fail_count` is the number of times a frame for this node was
// popped due to a parse failure (either a clean rewind on a
// `backtrack`-marked Choice, or a propagated failure that bubbled
// past this frame). High failure counts on a rule indicate either
// genuine ambiguity in the grammar or a Choice ordering that lets
// alternatives try-and-rewind on every input.
//
// `max_depth` is the deepest the parse stack got while this node's
// frame was active. Useful for catching grammars that recurse
// unexpectedly deep on certain inputs.
struct ProfileEntry {
    NodeId        node_id;
    std::string   rule_name;
    std::uint64_t entry_count = 0;
    std::uint64_t fail_count  = 0;
    std::uint64_t total_ns    = 0;
    std::uint64_t max_depth   = 0;
};

// Full report produced by a single profile-enabled parse_from call.
// Entries are unordered; sort with `top()` or by hand.
struct ProfileReport {
    std::vector<ProfileEntry> entries;
    // Total elapsed wall-clock for the parse_from call itself.
    std::uint64_t total_ns      = 0;
    // Total number of frames pushed during the parse (including
    // nested re-entries of the same rule).
    std::uint64_t total_frames  = 0;
    // Deepest parse-stack depth observed.
    std::uint64_t max_depth     = 0;

    // Sort entries by `total_ns` descending and return the top `n`
    // (or all entries if n is zero or greater than entries.size()).
    // Returned by value so the caller can hold onto a stable
    // snapshot.
    std::vector<ProfileEntry> top_by_time(std::size_t n) const;

    // Same shape, sorted by `entry_count` descending.
    std::vector<ProfileEntry> top_by_count(std::size_t n) const;
};

} // namespace rawast
