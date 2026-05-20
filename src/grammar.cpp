#include <rawast/grammar.hpp>
#include <rawast/parsers.hpp>

#include "frame.hpp"

#include <cassert>
#include <memory>
#include <string>
#include <utility>

namespace rawast {

// -------------------------------------------------------------------------
// Node allocation and builders
// -------------------------------------------------------------------------

NodeId Grammar::allocate_(NodeKind kind) {
    NodeId id{nodes_.size()};
    nodes_.emplace_back();
    nodes_.back().kind = kind;
    return id;
}

NodeId Grammar::new_choice()   { return allocate_(NodeKind::Choice); }
NodeId Grammar::new_sequence() { return allocate_(NodeKind::Sequence); }
NodeId Grammar::new_repeat()   { return allocate_(NodeKind::Repeat); }

NodeId Grammar::new_ref(std::string name) {
    NodeId id = allocate_(NodeKind::Ref);
    nodes_[id.value()].value = make_string(std::move(name));
    return id;
}

NodeId Grammar::new_key(std::string token) {
    NodeId id = allocate_(NodeKind::Key);
    nodes_[id.value()].value = make_string(std::move(token));
    return id;
}

NodeId Grammar::new_parse(std::string parser_name) {
    NodeId id = allocate_(NodeKind::Parse);
    nodes_[id.value()].value = make_string(std::move(parser_name));
    return id;
}

NodeId Grammar::new_value(ValuePtr v) {
    NodeId id = allocate_(NodeKind::Value);
    nodes_[id.value()].value = std::move(v);
    return id;
}

NodeId Grammar::add_ref(NodeId parent, std::string name) {
    NodeId child = new_ref(std::move(name));
    nodes_[parent.value()].children.push_back(child);
    return child;
}

NodeId Grammar::add_key(NodeId parent, std::string token) {
    NodeId child = new_key(std::move(token));
    nodes_[parent.value()].children.push_back(child);
    return child;
}

NodeId Grammar::add_parse(NodeId parent, std::string parser_name) {
    NodeId child = new_parse(std::move(parser_name));
    nodes_[parent.value()].children.push_back(child);
    return child;
}

NodeId Grammar::add_value(NodeId parent, ValuePtr v) {
    NodeId child = new_value(std::move(v));
    nodes_[parent.value()].children.push_back(child);
    return child;
}

NodeId Grammar::add_choice(NodeId parent) {
    NodeId child = new_choice();
    nodes_[parent.value()].children.push_back(child);
    return child;
}

NodeId Grammar::add_sequence(NodeId parent) {
    NodeId child = new_sequence();
    nodes_[parent.value()].children.push_back(child);
    return child;
}

NodeId Grammar::add_repeat(NodeId parent) {
    NodeId child = new_repeat();
    nodes_[parent.value()].children.push_back(child);
    return child;
}

void Grammar::set_optional(NodeId id) {
    nodes_[id.value()].is_optional = true;
}

void Grammar::set_name(NodeId id) {
    nodes_[id.value()].is_name = true;
}

void Grammar::set_container(NodeId id, Container c) {
    nodes_[id.value()].container = c;
}

void Grammar::set_separator(NodeId parent, NodeId sep) {
    Node& p = nodes_[parent.value()];
    if (p.has_separator && !p.children.empty()) {
        p.children[0] = sep;
    } else {
        p.children.insert(p.children.begin(), sep);
        p.has_separator = true;
    }
}

// -------------------------------------------------------------------------
// Registries and accessors
// -------------------------------------------------------------------------

void Grammar::register_rule(std::string name, NodeId node) {
    named_rules_[std::move(name)] = node;
}

void Grammar::register_parser(std::unique_ptr<Parser> p) {
    std::string name = p->name();
    parsers_[std::move(name)] = std::move(p);
}

void Grammar::add_ignore(std::string parser_name) {
    auto it = parsers_.find(parser_name);
    assert(it != parsers_.end());
    ignore_.push_back(it->second.get());
}

void Grammar::set_top(NodeId node) {
    top_ = node;
}

const Node& Grammar::node(NodeId id) const noexcept {
    return nodes_[id.value()];
}

Node& Grammar::node(NodeId id) noexcept {
    return nodes_[id.value()];
}

NodeId Grammar::resolve_ref(NodeId id) const {
    while (id.valid() && nodes_[id.value()].kind == NodeKind::Ref) {
        const auto& v = nodes_[id.value()].value;
        auto sv = std::dynamic_pointer_cast<StringValue>(v);
        assert(sv);
        auto it = named_rules_.find(sv->data());
        assert(it != named_rules_.end());
        id = it->second;
    }
    return id;
}

Parser* Grammar::parser(const std::string& name) const {
    auto it = parsers_.find(name);
    return it == parsers_.end() ? nullptr : it->second.get();
}

// -------------------------------------------------------------------------
// Load driver — trampolined recursive descent
// -------------------------------------------------------------------------

namespace {

void run_ignore(const Grammar& g, StreamReader& sr) {
    for (Parser* p : g.ignore()) {
        // Discard result; the ignore list is allowed to fail (no whitespace
        // is fine). We just consume whatever matches.
        (void)p->parse(sr);
    }
}

// Push a NodeId onto the stack, resolving Ref chains.
void push_node(std::vector<Frame>& stack, const Grammar& g, NodeId id) {
    NodeId resolved = g.resolve_ref(id);
    stack.emplace_back(g, resolved);
}

// Update max_progress if the new error is further along.
void note_progress(ParseError& max_progress, const ParseError& err) {
    if (err.position.bytes > max_progress.position.bytes) {
        max_progress = err;
    }
}

} // namespace

tl::expected<ValuePtr, ParseError> Grammar::parse(StreamReader& sr) const {
    ValuePool pool;
    return parse(sr, pool);
}

tl::expected<ValuePtr, ParseError> Grammar::parse(StreamReader& sr, ValuePool& pool) const {
    std::vector<Frame> stack;
    ParseError max_progress{sr.position(), "no parse attempted"};
    ValuePtr result_value;

    // Advance after a child has just completed successfully. Pops frames
    // until we either find one with more work to do, or reach the top.
    auto advance_after_child = [&]() {
        while (!stack.empty()) {
            Frame& top = stack.back();
            bool more = false;
            switch (top.kind()) {
            case NodeKind::Choice:
                // Choice's child succeeded -> choice itself is done.
                more = false;
                break;
            case NodeKind::Sequence:
            case NodeKind::Repeat:
                more = top.step_next();
                break;
            default:
                break;
            }
            if (more) return;

            // Pop this frame, finish, pass values up.
            Frame popped = std::move(stack.back());
            stack.pop_back();
            popped.finish(pool);
            if (stack.empty()) {
                result_value = popped.result();
                return;
            }
            popped.pass_values_to(stack.back());
            // Loop to advance the new top frame.
        }
    };

    // Handle a parse failure: walk back to the nearest Choice with more
    // alternatives, or to a Repeat (which terminates gracefully on
    // failure), or to an optional level (which treats failure as empty).
    auto handle_failure = [&](const ParseError& err) {
        note_progress(max_progress, err);
        while (!stack.empty()) {
            Frame popped = std::move(stack.back());
            stack.pop_back();

            if (popped.is_optional()) {
                // Treat as success-with-empty.
                if (!stack.empty()) {
                    advance_after_child();
                }
                return;
            }

            if (popped.kind() == NodeKind::Repeat) {
                // Iteration ended -- accept what we collected so far.
                popped.finish(pool);
                if (stack.empty()) {
                    result_value = popped.result();
                    return;
                }
                popped.pass_values_to(stack.back());
                advance_after_child();
                return;
            }

            if (popped.kind() == NodeKind::Choice) {
                if (popped.step_next()) {
                    stack.push_back(std::move(popped));
                    return;
                }
                // Else: choice exhausted -- propagate the failure further.
            }
            // Sequence/Key/Parse: propagate by continuing to pop.
        }
        // Stack drained without recovery -- top-level parse fails.
    };

    push_node(stack, *this, top_);

    while (!stack.empty() && !result_value) {
        Frame& top = stack.back();
        switch (top.kind()) {

        case NodeKind::Key: {
            run_ignore(*this, sr);
            const Node& n = nodes_[top.node_id().value()];
            auto sv = std::dynamic_pointer_cast<StringValue>(n.value);
            assert(sv);
            KeyParser p(sv->data());
            auto r = p.parse(sr);
            if (r) {
                // Key matched. Don't push the literal as a value -- the
                // Key node may have Value-kind children that already
                // populated emitted_ with the constants to flow up.
                Frame popped = std::move(stack.back());
                stack.pop_back();
                popped.finish(pool);
                if (stack.empty()) {
                    result_value = popped.result();
                    break;
                }
                popped.pass_values_to(stack.back());
                advance_after_child();
            } else {
                handle_failure(r.error());
            }
            break;
        }

        case NodeKind::Parse: {
            run_ignore(*this, sr);
            const Node& n = nodes_[top.node_id().value()];
            auto sv = std::dynamic_pointer_cast<StringValue>(n.value);
            assert(sv);
            Parser* p = parser(sv->data());
            assert(p);
            auto r = p->parse(sr);
            if (r) {
                // Intern the produced primitive so identical content
                // (same string, same int, etc.) shares a canonical
                // ValuePtr across the entire parse.
                ValuePtr canonical = pool.intern(*r);
                top.add_value(canonical, top.is_name());
                Frame popped = std::move(stack.back());
                stack.pop_back();
                popped.finish(pool);
                if (stack.empty()) {
                    result_value = popped.result();
                    break;
                }
                popped.pass_values_to(stack.back());
                advance_after_child();
            } else {
                handle_failure(r.error());
            }
            break;
        }

        case NodeKind::Choice:
        case NodeKind::Sequence:
        case NodeKind::Repeat: {
            if (top.has_current()) {
                push_node(stack, *this, top.current_child());
            } else {
                // No children left to process -- this frame is done.
                Frame popped = std::move(stack.back());
                stack.pop_back();
                popped.finish(pool);
                if (stack.empty()) {
                    result_value = popped.result();
                    break;
                }
                popped.pass_values_to(stack.back());
                advance_after_child();
            }
            break;
        }

        default:
            // Ref absorbed by resolve_ref at push time; Value absorbed by
            // Frame constructor. Neither should appear as a top frame.
            assert(false && "unreachable: Ref or Value at top of parse stack");
            handle_failure(ParseError{sr.position(), "internal: unexpected node kind"});
            break;
        }
    }

    if (result_value) {
        return result_value;
    }
    return tl::unexpected(max_progress);
}

// -------------------------------------------------------------------------
// JSON grammar — first working grammar, built in code
// -------------------------------------------------------------------------

Grammar make_json_grammar() {
    Grammar g;

    g.register_parser(std::make_unique<IntParser>());
    g.register_parser(std::make_unique<FloatParser>());
    g.register_parser(std::make_unique<DoubleQuoteStringParser>());
    g.register_parser(std::make_unique<WhitespaceParser>());
    g.add_ignore("whitespace");

    // VALUE: choice of primitives and containers.
    NodeId value = g.new_choice();
    g.register_rule("VALUE", value);
    g.add_ref(value, "STRUCT");
    g.add_ref(value, "LIST");
    g.add_parse(value, "string");
    // float must come before int because float requires '.' or 'e' so a
    // bare integer falls through to the int branch; the order here is the
    // predictive-PEG ordering described in §2.4.
    g.add_parse(value, "float");
    g.add_parse(value, "int");

    NodeId k_null  = g.add_key(value, "null");
    g.add_value(k_null, null_value());
    NodeId k_true  = g.add_key(value, "true");
    g.add_value(k_true, true_value());
    NodeId k_false = g.add_key(value, "false");
    g.add_value(k_false, false_value());

    // LIST: [ item, item, ... ]
    NodeId list = g.new_sequence();
    g.register_rule("LIST", list);
    g.set_container(list, Container::Array);
    g.add_key(list, "[");
    NodeId list_rep = g.add_repeat(list);
    g.add_key(list, "]");
    g.set_separator(list_rep, g.new_key(","));
    g.add_ref(list_rep, "VALUE");

    // STRUCT: { "key": value, ... }
    NodeId structure = g.new_sequence();
    g.register_rule("STRUCT", structure);
    g.set_container(structure, Container::Dict);
    g.add_key(structure, "{");
    NodeId struct_rep = g.add_repeat(structure);
    g.add_key(structure, "}");
    g.set_separator(struct_rep, g.new_key(","));
    NodeId pair = g.add_sequence(struct_rep);
    NodeId name = g.add_parse(pair, "string");
    g.set_name(name);
    g.add_key(pair, ":");
    g.add_ref(pair, "VALUE");

    g.set_top(g.new_ref("VALUE"));

    return g;
}

} // namespace rawast
