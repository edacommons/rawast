#pragma once

#include <rawast/node.hpp>
#include <rawast/parser.hpp>
#include <rawast/pool.hpp>
#include <rawast/profile.hpp>
#include <rawast/stream.hpp>
#include <rawast/value.hpp>

#include <tl/expected.hpp>

#include <bitset>
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
    void set_negative(NodeId id);
    void set_name(NodeId id);
    void set_container(NodeId id, Container c);
    void set_separator(NodeId parent, NodeId sep);
    void set_backtrack(NodeId id);
    void set_fixed_schema(NodeId id);
    // `#opchain` reserved flag — when a rule body carries this
    // annotation, parse-side post-processing compacts always-wrap
    // `{lhs, tail:[{op,rhs},...]}` shapes into `{op, args[]}` (same-op
    // runs collapse flat, mixed-op boundaries nest). Save-side
    // reverses the transform before normal dispatch. Shape-based —
    // the engine doesn't track which subtree node produced which dict;
    // it walks the AST under the marked rule and applies the
    // transform wherever the shape matches.
    void set_opchain(NodeId id);
    bool has_opchain(NodeId id) const noexcept;
    // Walk the Ref chain starting at `id` and return true if any
    // node along the way carries the opchain flag. resolve_ref jumps
    // to the chain's end and misses intermediate Refs (e.g. the
    // `start:` top_ Ref → EXPR Ref → ADD body where EXPR carries
    // the flag).
    bool has_opchain_in_chain(NodeId id) const noexcept;
    // Key-only: opt the KeyParser into word-boundary strict matching at
    // parse time. The literal still matches byte-by-byte; the strict
    // flag additionally requires the byte after the match to be non-word
    // or EOF. See `Node.strict` in node.hpp. No-op for non-Key nodes.
    void set_strict(NodeId id);
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

    // Grammar-wide ignore parser names, in insertion order. Used by
    // `to_value` to round-trip the ignore configuration.
    const std::vector<std::string>& ignore_names() const noexcept {
        return ignore_names_;
    }

    // Per-rule ignore overrides, keyed by rule name. Used by `to_value`.
    const std::map<std::string, std::vector<std::string>>& rule_ignore_names() const noexcept {
        return rule_ignore_names_;
    }

    // Inferred parser-group names from registered dotted aliases.
    // Walks the parser registry looking for "X.Y" entries; "X" becomes
    // a group name. Used by `to_value` to reconstruct the `use:`
    // directive. Returns sorted, deduplicated names.
    std::vector<std::string> parser_groups() const;

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
    //
    // `require_full_consume` (default true) enforces that the stream is
    // exhausted after the start rule completes — the standard contract
    // for a top-level parse or subparse. Set false for sub-invocations
    // that may legitimately consume only a prefix (the byte-scan INNER
    // trial path in walk_scan, for example).
    //
    // `initial_ignore` seeds the parse driver's ignore_stack with the
    // caller's active policy. Used by walk_scan when subparsing an
    // INNER rule from within a scope/raw scan — without it the INNER
    // would lose any ignore policy inherited from the calling context
    // (e.g. PP_FILE's `ignore linespace`), and predictive checks at
    // optional boundaries would see raw whitespace. nullptr means
    // "no seed" — the parse starts with an empty ignore_stack and
    // falls back to the grammar's default ignore set.
    tl::expected<ValuePtr, ParseError> parse_from(
            StreamReader& sr, ValuePool& pool, NodeId start,
            bool require_full_consume = true,
            const std::vector<Parser*>* initial_ignore = nullptr) const;

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
                                       bool pretty = true,
                                       NodeId start = {}) const;

    // --- Performance: peek-and-skip optional/choice optimization ------
    //
    // For each Node, precompute the set of input first-bytes its
    // first non-nullable terminal can begin with. When the parse
    // driver is about to push a frame for an *optional* Ref (`?<X>`
    // pattern) or a Choice alternative, it peeks the next input byte
    // (after ignore-skip) and bails immediately if the byte isn't in
    // the node's first-byte set. Saves the frame push + key-parse
    // attempt + rewind that would otherwise burn cycles on shapes
    // without MASK_MOD / ITERATE_PREFIX / etc.
    //
    // Computed by `compute_first_bytes()`, called once at end of
    // grammar load. Idempotent — safe to call multiple times.
    //
    // The bitset has a "wildcard" semantic when `first_bytes_known_`
    // is false for that NodeId: the engine falls back to the normal
    // push-and-try path (correctness preserved when the analysis
    // can't determine a specific set).
    void compute_first_bytes() const;
    bool first_byte_might_match(NodeId id, unsigned char byte) const noexcept {
        if (id.value() >= first_bytes_known_.size()) return true;
        if (!first_bytes_known_[id.value()])         return true;
        return first_bytes_[id.value()].test(byte);
    }

    // --- Profiling -----------------------------------------------------
    //
    // When enabled, `parse_from` collects per-node entry / failure /
    // wall-clock-time counters and stashes them on the Grammar. Read
    // back via `last_profile_report()`. Zero overhead when disabled
    // — one bool check per Frame push/pop. The MVP doesn't profile
    // the save direction; add that hook when the parse-side numbers
    // point at a specific area worth chasing.
    //
    // Enable/disable is a runtime flag (no recompile needed); the
    // flag is sticky until cleared. The CLI exposes this via
    // `rawast parse --profile [--profile-top=N|all]`.
    void profile_enable(bool yes = true) const noexcept { profile_enabled_ = yes; }
    bool profile_enabled() const noexcept { return profile_enabled_; }
    const ProfileReport& last_profile_report() const noexcept {
        return last_profile_report_;
    }

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

    // Profiling — mutable so `parse_from(...) const` can write the
    // report back to the Grammar. profile_enabled_ is the runtime
    // toggle (see profile_enable()).
    mutable bool          profile_enabled_ = false;
    mutable ProfileReport last_profile_report_;

    // Precomputed per-Node first-byte sets. Indexed by NodeId.value().
    // first_bytes_known_[i] = true means first_bytes_[i] is the exact
    // set of input first-bytes the node can begin with; false means
    // "unknown — fall back to push-and-try". 256 bits = 32 bytes per
    // entry; reasonable cost for the speed-up. Mutable so
    // compute_first_bytes() can populate from a `const` Grammar
    // method (called lazily by parse_from on first use).
    mutable std::vector<std::bitset<256>> first_bytes_;
    mutable std::vector<bool>             first_bytes_known_;
    // Strict CONTENT first-byte set — what bytes the node's body
    // actually starts with, ignoring nullability. `first_bytes_`
    // (above) is set-all when the node is nullable so the Choice
    // peek-and-skip never wrongly rejects a nullable alternative.
    // `should_skip_optional` uses THIS strict set: an `?<X>` whose
    // content can't start at the next byte can be cleanly skipped
    // even if X is nullable (skipping is equivalent to matching
    // empty in that case).
    mutable std::vector<std::bitset<256>> strict_first_bytes_;
    // Per-NodeId flag: this node is at an `?<...>` use-site optional
    // position (its own is_optional is set, OR it's a Ref into a
    // chain where some link carries is_optional). The parse-loop
    // peek-and-skip check uses this as a fast O(1) guard before
    // doing the run_ignore + peek work; non-optional pushes (the
    // common case) early-return without touching the input stream.
    // Computed alongside first_bytes_.
    mutable std::vector<bool>             is_optional_chain_;
    mutable bool                          first_bytes_computed_ = false;

    // Per-NodeId resolved Ref target. For Ref nodes, this is the
    // final-target NodeId after walking the Ref chain (so `resolve_ref`
    // becomes a single field read instead of a loop with map lookups
    // and dynamic_pointer_cast on every visit). For non-Ref nodes,
    // the entry mirrors the node's own id (resolve_ref is a no-op).
    // Populated lazily by ensure_refs_resolved_(); the save engine
    // is the hot consumer.
    mutable std::vector<NodeId> resolved_refs_;
    mutable bool                resolved_refs_computed_ = false;
    void ensure_refs_resolved_() const;

    NodeId allocate_(NodeKind kind);
};

// The first working grammar — JSON. Constructed entirely in code via the
// Grammar builder API. The engine self-hosts once this works: subsequent
// grammars are loaded from `.json` files using this grammar to parse them.
Grammar make_json_grammar();

} // namespace rawast
