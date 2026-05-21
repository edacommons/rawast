#pragma once

#include <tl/expected.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace rawast {

class Grammar;
class Parser;

// A "parser group" is a named, namespaced set of terminal parsers a
// grammar opts into via a `use:` directive. Each parser inside group
// `G` is addressable two ways:
//
//   * bare:    "int"          — works when unambiguous
//   * dotted:  "std.int"      — always works, self-documenting
//
// Both names resolve to the same kind of Parser (a separate instance
// per Grammar); replace_parser (used for callback-driven parser
// rewriting in LEF/DEF/Liberty) operates on whichever name the rule
// referenced. Future M1 work: detect bare-name collisions across
// active groups and require qualification when ambiguous.

// Factory for one parser. Called every time the group is applied —
// each Grammar gets fresh instances.
using ParserFactory = std::function<std::unique_ptr<Parser>()>;

struct ParserSpec {
    std::string   local_name;   // e.g. "int", "gds_header"
    ParserFactory factory;
};

struct ParserGroup {
    std::string             name;     // e.g. "std", "gdsii"
    std::vector<ParserSpec> parsers;  // local names within the group
};

// Register a structured group. After it has been applied via
// `apply_parser_group`, every parser is registered on the target
// Grammar under both its bare and dotted names.
void register_parser_group(ParserGroup group);

// --- Legacy API (function-only, opaque to the loader) -------------------
// Kept for any third-party code that still builds groups by hand. New
// groups should use the structured ParserGroup form above.
using ParserGroupRegisterFn = std::function<void(Grammar&)>;
void register_parser_group(std::string name, ParserGroupRegisterFn fn);

// --- Application + introspection ----------------------------------------

// Apply a previously-registered group to `g`. Structured and legacy
// groups are both supported. Returns an error if the group name was
// never registered.
tl::expected<void, std::string>
apply_parser_group(Grammar& g, const std::string& name);

// Diagnostic: is the named group available?
bool parser_group_exists(const std::string& name);

// Return the list of currently-registered group names, sorted.
std::vector<std::string> registered_parser_groups();

// Enumerate the parser local names a structured group contributes.
// Returns an empty vector for legacy (function-only) groups — those
// are opaque to introspection. Used by docs/tooling, not the loader.
std::vector<std::string>
parser_group_local_names(const std::string& group_name);

} // namespace rawast
