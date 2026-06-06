#pragma once

#include <rawast/node.hpp>
#include <rawast/parser.hpp>
#include <rawast/pool.hpp>
#include <rawast/stream.hpp>
#include <rawast/value.hpp>

#include <tl/expected.hpp>

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace rawast {

// A complete grammar plus its terminal-parser registry and the list of
// parsers to run between tokens (whitespace, comments). Owns the arena
// of Nodes that make up the grammar tree, the named-rule registry, the
// parsers themselves, and the ignore list. Both parse and save direction
// drive against this object.
class Grammar {
public:
    Grammar() = default;

    Grammar(const Grammar&) = delete;
    Grammar& operator=(const Grammar&) = delete;
    Grammar(Grammar&&) = default;
    Grammar& operator=(Grammar&&) = default;

    // --- Node allocation -----------------------------------------------

    NodeId new_choice();
    NodeId new_sequence();
    NodeId new_repeat();
    // Raw-consume node: scans bytes until its stop literal matches at
    // cursor. The stop literal is the immediate next sibling under a
    // Sequence parent (required to be a Key node with a string Value);
    // the loader resolves it after grammar-load completes and stashes
    // the literal on `value`.
    NodeId new_raw();
    NodeId new_ref(std::string name);
    NodeId new_key(std::string token);
    NodeId new_parse(std::string parser_name);
    NodeId new_value(ValuePtr v);

    // --- Builder convenience: allocate child + attach to parent -------

    NodeId add_ref(NodeId parent, std::string name);
    NodeId add_key(NodeId parent, std::string token);
    NodeId add_parse(NodeId parent, std::string parser_name);
    NodeId add_value(NodeId parent, ValuePtr v);
    NodeId add_choice(NodeId parent);
    NodeId add_sequence(NodeId parent);
    NodeId add_repeat(NodeId parent);

    // --- Mutators on existing nodes ------------------------------------

    void set_optional(NodeId id);
    void set_name(NodeId id);
    void set_container(NodeId id, Container c);
    void set_separator(NodeId parent, NodeId sep);
    void set_backtrack(NodeId id);
    void set_fixed_schema(NodeId id);
    void set_min(NodeId id, std::uint32_t m);  // Repeat-only: minimum iteration count.

    // Save-side pretty-print modifiers; see Node fields for semantics.
    void set_indent(NodeId id);    // depth+1 for this Node's scope
    void set_tab(NodeId id);       // emit depth × indent_step before content
    void set_space(NodeId id);     // emit " " after content
    void set_newline(NodeId id);   // emit "\n" at the end
    void set_tail(NodeId id, std::string s);  // emit s after content (escape-interpreted)

    // Grammar-wide indent step. Default "  " (two spaces). Used by the
    // save direction whenever a Node with `tab` (indent_emit) fires.
    void set_indent_step(std::string s) { indent_step_ = std::move(s); }
    const std::string& indent_step() const noexcept { return indent_step_; }

    // --- Registries -----------------------------------------------------

    void register_rule(std::string name, NodeId node);
    void register_parser(std::unique_ptr<Parser> p);
    // Register a parser under an arbitrary key (e.g. a dotted alias
    // like "std.int") rather than the parser's own ::name(). Used by
    // the parser-group machinery to give one logical terminal two
    // addressable forms: bare and "group.local".
    void register_parser_alias(std::string key, std::unique_ptr<Parser> p);
    void add_ignore(std::string parser_name);

    // Rule-local ignore override. When the parse driver enters the rule
    // named `rule_name` (via Ref resolution), it temporarily uses the
    // listed parsers as the active ignore set, restoring the previous
    // set on rule exit. Parsers must be registered via `use:` before
    // they can appear in an override list. Empty list = "ignore
    // nothing for this rule and its callees" — used e.g. for word-
    // internal contexts where whitespace is literal.
    //
    // Set by the .rawast `ignore <RULE>: …` top-level declaration.
    void add_rule_ignore(std::string rule_name, std::vector<std::string> parser_names);

    // Look up the override for a rule; returns nullptr if none.
    const std::vector<Parser*>* rule_ignore(NodeId rule_node) const;

    // Record a pending subparse target. The rule name is resolved by
    // resolve_subparse_refs() after all rules are loaded — necessary
    // because the named rule may be defined later in the grammar
    // source than the item that references it.
    void set_pending_subparse(NodeId target, std::string rule_name);

    // Resolve all pending subparse targets. Called by the loader at
    // the end of grammar load. Errors if a referenced rule wasn't
    // defined.
    tl::expected<void, std::string> resolve_subparse_refs();

    // --- Mid-parse hooks: rule callbacks and parser replacement -------
    //
    // Some formats (LEF/DEF, Liberty, Verilog) have preambles that
    // declare how later content should be tokenised — e.g. LEF's
    //   DIVIDERCHAR "/" ;
    //   BUSBITCHARS "[]" ;
    // After parsing the declaration, the identifier terminal needs to
    // accept those characters for the rest of the file. The callback +
    // replace_parser pair lets a host program react to a completed rule
    // and swap a parser in the registry; the driver picks up the new
    // parser for subsequent input.
    //
    // Callbacks fire on COMMIT, not on tentative match. If a callback's
    // rule completes inside a backtracking Choice that is later
    // rejected, the callback never fires. The driver queues callbacks
    // under active stream marks and flushes them on accept, discards on
    // reject. This is what makes the mechanism safe: a parser swap in a
    // rejected branch would otherwise corrupt the rest of the parse.
    using RuleCallback = std::function<void(const ValuePtr&)>;
    void on_rule_complete(const std::string& rule_name, RuleCallback cb);

    // Add or replace a terminal parser by name. The new parser owns its
    // own state; callers transferring lookup tables (char classes, etc.)
    // do so via the parser's constructor. Marked const because the
    // parser registry is mutable — replace_parser is intended to be
    // called from within a rule callback during parse().
    void replace_parser(std::unique_ptr<Parser> p) const;

    void set_top(NodeId node);
    NodeId top() const noexcept { return top_; }

    // --- Read-only accessors -------------------------------------------

    const Node& node(NodeId id) const noexcept;
    Node& node(NodeId id) noexcept;

    // Follow Ref chains and return the underlying non-Ref node id.
    NodeId resolve_ref(NodeId id) const;

    // Rule registry access.
    bool   has_rule(const std::string& name) const noexcept;
    NodeId rule_id(const std::string& name) const noexcept;

    // Arena introspection — every NodeId in [0, node_count()) refers to
    // an allocated Node. Used by the grammar linter and other tooling
    // that needs to walk every Node regardless of reachability from top.
    std::size_t node_count() const noexcept { return nodes_.size(); }

    // Iterate over named rules. Each entry is (rule_name, NodeId).
    // The map is in insertion order — std::map sorts by name.
    const std::map<std::string, NodeId>& named_rules() const noexcept {
        return named_rules_;
    }

    Parser* parser(const std::string& name) const;
    const std::vector<Parser*>& ignore() const noexcept { return ignore_; }

    // --- Driver: read direction ----------------------------------------

    // Parse an input stream against this grammar. On success returns the
    // root ValuePtr of the resulting tree. On failure returns the
    // max-progress ParseError (the deepest position the parser reached
    // before giving up — the most informative failure for diagnostics).
    //
    // The no-pool overload creates an internal pool that lives for the
    // duration of the parse and is discarded on return; primitive
    // interning happens but is not visible to the caller.
    //
    // The pool-aware overload uses the caller-provided pool. After parse
    // returns, the caller owns the pool and can query its back-references
    // (find_containers_of) to do value search across the produced tree.
    tl::expected<ValuePtr, ParseError> parse(StreamReader& sr) const;
    tl::expected<ValuePtr, ParseError> parse(StreamReader& sr, ValuePool& pool) const;
    // Parse from an explicit start node — used by the subparse hook to
    // re-enter the engine on an item's captured string with a different
    // entry rule. The same grammar is reused; only the starting point
    // changes.
    tl::expected<ValuePtr, ParseError> parse_from(
            StreamReader& sr, ValuePool& pool, NodeId start) const;

    // Convenience: parse from a rule name. Looks up the named rule's
    // body NodeId via the registry; fails if the rule doesn't exist.
    // Lets application code re-parse arbitrary strings through any
    // rule in the grammar without depending on the subparse hook:
    //
    //   auto result = g.parse_from(sr, "EXPR");
    //
    // Equivalent to `parse_from(sr, pool, g.rule_id("EXPR"))` with an
    // internally-allocated pool plus an explicit error on missing rule.
    tl::expected<ValuePtr, ParseError> parse_from(
            StreamReader& sr, const std::string& start_name) const;
    tl::expected<ValuePtr, ParseError> parse_from(
            StreamReader& sr, ValuePool& pool, const std::string& start_name) const;

    // --- Driver: save direction ----------------------------------------

    // Emit `value` as text to `out` using this grammar.
    //
    // `pretty` controls the save-side pretty-print attributes:
    //   true  (default) — emit `tab` (indent), `newline`, and apply
    //                     `indent` depth bumps. The grammar's full
    //                     formatting takes effect.
    //   false           — skip tab/indent/newline; keep `space` and
    //                     `tail` (those may be required for round-trip
    //                     parsing — e.g. space between two adjacent
    //                     identifiers, or tail strings used as
    //                     line-continuation markers).
    //
    // Choice dispatch is by value-shape: a DictValue picks the alternative
    // that produces a Container::Dict; an ArrayValue picks Container::Array;
    // a primitive picks the matching Parse alternative by parser-name
    // convention ("int" / "uint" / "float" / "string"); null/true/false
    // pick the Key alternative whose Value-child constant identity-matches.
    tl::expected<void, SaveError> save(std::ostream& out, ValuePtr value,
                                       bool pretty = true) const;

private:
    std::vector<Node> nodes_;
    std::map<std::string, NodeId> named_rules_;
    // mutable: replace_parser() swaps entries during parse, called from
    // RuleCallbacks that fire inside the const parse() driver.
    mutable std::map<std::string, std::unique_ptr<Parser>> parsers_;
    // ignore_ is rebuilt from parsers_ on every replace_parser() call so
    // a swap stays consistent if the swapped parser was on the ignore
    // list. mutable for the same reason as parsers_.
    mutable std::vector<Parser*> ignore_;
    // Insertion order of ignore_ — replace_parser preserves it.
    std::vector<std::string> ignore_names_;
    // Per-rule ignore overrides. Key: rule name (also stored as
    // resolved NodeId after grammar load for O(1) lookup during
    // parse). Value: parser names; the parsers themselves are resolved
    // at lookup time so a replace_parser() swap is honoured.
    std::map<std::string, std::vector<std::string>> rule_ignore_names_;
    // Resolved view: rule NodeId → parser-pointer list. Built lazily
    // on first parse(); invalidated by replace_parser().
    mutable std::map<std::size_t, std::vector<Parser*>> rule_ignore_resolved_;
    mutable bool rule_ignore_dirty_ = true;
    // Pending subparse targets — populated by the loader as items
    // with `:subparse="RULE"` bindings are processed, resolved en
    // masse by resolve_subparse_refs() at end of grammar load.
    std::vector<std::pair<NodeId, std::string>> pending_subparse_;
    // Rule-completion callbacks, keyed by the rule's body NodeId
    // (the post-Ref-resolution arena id, which is also Frame::node_id()).
    std::map<std::size_t, std::vector<RuleCallback>> callbacks_by_node_;
    // Indent step for save-direction pretty-print (default two spaces).
    std::string indent_step_ = "  ";
    NodeId top_;

    NodeId allocate_(NodeKind kind);
};

// The first working grammar — JSON. Constructed entirely in code via the
// Grammar builder API. The engine self-hosts once this works: subsequent
// grammars are loaded from `.json` files using this grammar to parse them.
Grammar make_json_grammar();

} // namespace rawast
