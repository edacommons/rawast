#pragma once

#include <rawast/grammar.hpp>
#include <rawast/value.hpp>

#include <tl/expected.hpp>

#include <string>
#include <string_view>

namespace rawast {

// Load grammar rules from a JSON-grammar Value tree into an existing
// Grammar. The Grammar must already have its terminal parsers registered
// (the loader doesn't know which parsers the target grammar will need).
//
// The tree shape is the standard JSON-grammar schema:
//
//   { "start": "RULE_NAME",
//     "RULE_1": { "type": "sequence", "container": "array",
//                 "items": [ ... ] },
//     "RULE_2": "RULE_3",
//     ...
//   }
//
// Item / body shapes inside the tree:
//   "X"                                  -- bare string: Ref if X is
//                                           a registered rule name,
//                                           otherwise Key with literal X.
//   {"type":"sequence",   ...}
//   {"type":"choice",     ...}
//   {"type":"repeat",     ...}
//   {"type":"key",   "key":"X", "value":?}
//   {"type":"value", "value": <constant>, "var": ?}
//   {"type":"ref",   "name": "X"}        -- explicit form of bare-string Ref
//   {"type":"optional", "expr": <expr>}  -- transparent wrapper that sets
//                                           is_optional on the wrapped expr
//   {"type":"<parser-name>", "var":?}    -- Parse using a registered parser.
//
// Optional fields (apply to any node kind unless noted):
//   "container":  "array" | "dict"   (on sequence / choice)
//   "var":        true               (on parse / value)
//   "value":      any constant       (on key / value)
//   "separator":  <item>             (on repeat)
//   "item":       <item>             (required, on repeat)
//   "items":      [ <item>, ... ]    (required, on sequence / choice)
//   "optional":   true               (any node)
//
// On success the Grammar is populated; on failure returns a human-
// readable error message.
tl::expected<void, std::string> load_json_grammar_into(
    Grammar& g, const Value& tree);

// Convenience: parse JSON-grammar text into an Value tree using a
// fresh internal JSON grammar, then load that tree into `g`.
tl::expected<void, std::string> load_json_grammar_from_string(
    Grammar& g, std::string_view content);

// Read a file and load its contents as JSON grammar.
tl::expected<void, std::string> load_json_grammar_from_file(
    Grammar& g, const std::string& path);

} // namespace rawast
