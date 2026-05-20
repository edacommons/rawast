#pragma once

#include <rawast/node.hpp>
#include <rawast/parser.hpp>
#include <rawast/stream.hpp>
#include <rawast/value.hpp>

#include <tl/expected.hpp>

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

    // --- Registries -----------------------------------------------------

    void register_rule(std::string name, NodeId node);
    void register_parser(std::unique_ptr<Parser> p);
    void add_ignore(std::string parser_name);

    void set_top(NodeId node);
    NodeId top() const noexcept { return top_; }

    // --- Read-only accessors -------------------------------------------

    const Node& node(NodeId id) const noexcept;
    Node& node(NodeId id) noexcept;

    // Follow Ref chains and return the underlying non-Ref node id.
    NodeId resolve_ref(NodeId id) const;

    Parser* parser(const std::string& name) const;
    const std::vector<Parser*>& ignore() const noexcept { return ignore_; }

    // --- Driver: read direction ----------------------------------------

    // Parse an input stream against this grammar. On success returns the
    // root ValuePtr of the resulting tree. On failure returns the
    // max-progress ParseError (the deepest position the parser reached
    // before giving up — the most informative failure for diagnostics).
    tl::expected<ValuePtr, ParseError> parse(StreamReader& sr) const;

private:
    std::vector<Node> nodes_;
    std::map<std::string, NodeId> named_rules_;
    std::map<std::string, std::unique_ptr<Parser>> parsers_;
    std::vector<Parser*> ignore_;
    NodeId top_;

    NodeId allocate_(NodeKind kind);
};

// The first working grammar — JSON. Constructed entirely in code via the
// Grammar builder API. The engine self-hosts once this works: subsequent
// grammars are loaded from `.json` files using this grammar to parse them.
Grammar make_json_grammar();

} // namespace rawast
