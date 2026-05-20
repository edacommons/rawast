#include <rawast/grammar.hpp>
#include <rawast/parsers.hpp>

#include "frame.hpp"

#include <cassert>
#include <memory>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

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

void Grammar::set_backtrack(NodeId id) {
    nodes_[id.value()].backtrack = true;
}

void Grammar::set_indent(NodeId id)  { nodes_[id.value()].depth_in      = true; }
void Grammar::set_tab(NodeId id)     { nodes_[id.value()].indent_emit   = true; }
void Grammar::set_space(NodeId id)   { nodes_[id.value()].space_after   = true; }
void Grammar::set_newline(NodeId id) { nodes_[id.value()].newline_after = true; }
void Grammar::set_tail(NodeId id, std::string s) {
    nodes_[id.value()].tail = std::move(s);
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
    ignore_names_.push_back(std::move(parser_name));
}

void Grammar::on_rule_complete(const std::string& rule_name, RuleCallback cb) {
    auto it = named_rules_.find(rule_name);
    assert(it != named_rules_.end() && "on_rule_complete: unknown rule");
    callbacks_by_node_[it->second.value()].push_back(std::move(cb));
}

void Grammar::replace_parser(std::unique_ptr<Parser> p) const {
    std::string name = p->name();
    parsers_[name] = std::move(p);
    // If this parser was on the ignore list, refresh the pointer so the
    // driver's ignore loop picks up the new instance.
    for (std::size_t i = 0; i < ignore_names_.size(); ++i) {
        if (ignore_names_[i] == name) {
            ignore_[i] = parsers_[name].get();
        }
    }
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

bool Grammar::has_rule(const std::string& name) const noexcept {
    return named_rules_.find(name) != named_rules_.end();
}

NodeId Grammar::rule_id(const std::string& name) const noexcept {
    auto it = named_rules_.find(name);
    if (it == named_rules_.end()) return NodeId{};
    return it->second;
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
    // Loop until a full pass through the ignore list consumes nothing.
    // This handles arbitrary interleaving of whitespace and comments —
    // e.g. "  // line\n  /* block */\n  " requires multiple cycles
    // because each ignore parser only consumes one contiguous run.
    while (true) {
        const std::size_t before = sr.position().bytes;
        for (Parser* p : g.ignore()) {
            (void)p->parse(sr);
        }
        if (sr.position().bytes == before) break;
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
    bool parse_finished = false;   // true once the top frame pops successfully

    // Rule-callback machinery -------------------------------------------
    //
    // pending_per_mark[i] is the queue of callbacks-to-fire that
    // accumulated while stream mark #i was active. On sr.accept() we
    // pop the back queue and either fire its contents (if no outer
    // mark) or merge them into the new back queue (their fate now
    // depends on the outer mark's resolution). On sr.reject() we pop
    // and discard — callbacks in a rejected branch never fire.
    struct PendingCallback {
        RuleCallback cb;
        ValuePtr     value;
    };
    std::vector<std::vector<PendingCallback>> pending_per_mark;

    auto fire_or_queue = [&](const RuleCallback& cb, ValuePtr v) {
        if (pending_per_mark.empty()) {
            cb(v);
        } else {
            pending_per_mark.back().push_back({cb, std::move(v)});
        }
    };

    auto fire_callbacks_for_frame = [&](Frame& popped) {
        auto it = callbacks_by_node_.find(popped.node_id().value());
        if (it == callbacks_by_node_.end()) return;
        ValuePtr v = popped.result();
        for (const auto& cb : it->second) {
            fire_or_queue(cb, v);
        }
    };

    auto on_mark_accept = [&]() {
        sr.accept();
        if (pending_per_mark.empty()) return;
        auto popped_q = std::move(pending_per_mark.back());
        pending_per_mark.pop_back();
        if (pending_per_mark.empty()) {
            for (auto& p : popped_q) p.cb(p.value);
        } else {
            auto& outer = pending_per_mark.back();
            for (auto& p : popped_q) outer.push_back(std::move(p));
        }
    };

    auto on_mark_reject = [&]() {
        sr.reject();
        if (!pending_per_mark.empty()) {
            pending_per_mark.pop_back();
        }
    };

    // Advance after a child has just completed successfully. Pops frames
    // until we either find one with more work to do, or reach the top.
    auto advance_after_child = [&]() {
        while (!stack.empty()) {
            Frame& top = stack.back();
            bool more = false;
            switch (top.kind()) {
            case NodeKind::Choice:
                // Choice's child succeeded -> choice itself is done. If
                // this Choice was backtracking and had marked the stream
                // for the just-succeeded alternative, accept that mark.
                if (top.has_mark()) {
                    on_mark_accept();
                    top.set_has_mark(false);
                }
                more = false;
                break;
            case NodeKind::Sequence:
            case NodeKind::Repeat:
            case NodeKind::Key:
            case NodeKind::Parse:
                // Key and Parse iterate their (typically Value-kind)
                // children after the terminal succeeds; same iteration
                // model as Sequence.
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
            fire_callbacks_for_frame(popped);
            if (stack.empty()) {
                result_value = popped.result();
                parse_finished = true;
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
                fire_callbacks_for_frame(popped);
                if (stack.empty()) {
                    result_value = popped.result();
                    parse_finished = true;
                    return;
                }
                popped.pass_values_to(stack.back());
                advance_after_child();
                return;
            }

            if (popped.kind() == NodeKind::Choice) {
                // The just-failed alternative was wrapped in a mark by
                // the entry-side code below (only if backtrack was on).
                // Reject it now so the stream rewinds to the position
                // before this alternative was tried.
                if (popped.has_mark()) {
                    on_mark_reject();
                    popped.set_has_mark(false);
                }
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

    while (!stack.empty() && !parse_finished) {
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
                // Key matched. The literal itself is not emitted; any
                // Value-kind children, however, contribute their
                // constants when iterated.
                if (top.has_current()) {
                    push_node(stack, *this, top.current_child());
                } else {
                    Frame popped = std::move(stack.back());
                    stack.pop_back();
                    popped.finish(pool);
                    fire_callbacks_for_frame(popped);
                    if (stack.empty()) {
                        result_value = popped.result();
                        parse_finished = true;
                        break;
                    }
                    popped.pass_values_to(stack.back());
                    advance_after_child();
                }
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
                if (top.has_current()) {
                    push_node(stack, *this, top.current_child());
                } else {
                    Frame popped = std::move(stack.back());
                    stack.pop_back();
                    popped.finish(pool);
                    fire_callbacks_for_frame(popped);
                    if (stack.empty()) {
                        result_value = popped.result();
                        parse_finished = true;
                        break;
                    }
                    popped.pass_values_to(stack.back());
                    advance_after_child();
                }
            } else {
                handle_failure(r.error());
            }
            break;
        }

        case NodeKind::Value: {
            // Frame ctor already pre-seeded emitted_ with this node's
            // constant (honouring is_name). Just pop and bubble up.
            Frame popped = std::move(stack.back());
            stack.pop_back();
            popped.finish(pool);
            fire_callbacks_for_frame(popped);
            if (stack.empty()) {
                result_value = popped.result();
                parse_finished = true;
                break;
            }
            popped.pass_values_to(stack.back());
            advance_after_child();
            break;
        }

        case NodeKind::Choice: {
            if (top.has_current()) {
                // Opt-in: if this Choice is marked as backtracking and we
                // haven't yet issued a stream mark for the current
                // alternative attempt, mark now. The mark will be
                // accepted (on alternative success) or rejected (on
                // alternative failure) before we move on.
                if (top.is_backtrack() && !top.has_mark()) {
                    sr.mark();
                    pending_per_mark.emplace_back();
                    top.set_has_mark(true);
                }
                push_node(stack, *this, top.current_child());
            } else {
                Frame popped = std::move(stack.back());
                stack.pop_back();
                popped.finish(pool);
                fire_callbacks_for_frame(popped);
                if (stack.empty()) {
                    result_value = popped.result();
                    parse_finished = true;
                    break;
                }
                popped.pass_values_to(stack.back());
                advance_after_child();
            }
            break;
        }

        case NodeKind::Sequence:
        case NodeKind::Repeat: {
            if (top.has_current()) {
                push_node(stack, *this, top.current_child());
            } else {
                // No children left to process -- this frame is done.
                Frame popped = std::move(stack.back());
                stack.pop_back();
                popped.finish(pool);
                fire_callbacks_for_frame(popped);
                if (stack.empty()) {
                    result_value = popped.result();
                    parse_finished = true;
                    break;
                }
                popped.pass_values_to(stack.back());
                advance_after_child();
            }
            break;
        }

        default:
            // Ref resolved at push time; should never reach a top frame.
            assert(false && "unreachable: Ref at top of parse stack");
            handle_failure(ParseError{sr.position(), "internal: unexpected node kind"});
            break;
        }
    }

    if (parse_finished) {
        return result_value;
    }
    return tl::unexpected(max_progress);
}

// -------------------------------------------------------------------------
// Save driver — structural mirror of the load driver
// -------------------------------------------------------------------------

namespace {

// A cursor into a sequence of values that flow downward during save.
// Container-Sequences split their value into a child cursor (array
// elements, or dict name-value pairs); Repeat iterates a cursor;
// terminals consume from the cursor.
struct SaveCursor {
    std::vector<Frame::EmittedValue> values;
    std::size_t idx = 0;

    bool has_more() const noexcept { return idx < values.size(); }
    const Frame::EmittedValue& peek() const { return values[idx]; }
    const Frame::EmittedValue& next() { return values[idx++]; }
};

bool alternative_matches(const Grammar& g, const Node& alt, const Value& v);

// Choose the Choice alternative whose grammar shape matches the value.
NodeId choose_alternative(const Grammar& g, const Node& choice_node, const Value& v) {
    for (NodeId child_id : choice_node.children) {
        NodeId resolved = g.resolve_ref(child_id);
        const Node& alt = g.node(resolved);
        if (alternative_matches(g, alt, v)) {
            return child_id;
        }
    }
    return NodeId{};
}

bool alternative_matches(const Grammar& g, const Node& alt, const Value& v) {
    const ValueType vt = v.type();
    switch (alt.kind) {
    case NodeKind::Sequence:
        if (alt.container == Container::Dict  && vt == ValueType::Dict)  return true;
        if (alt.container == Container::Array && vt == ValueType::Array) return true;
        return false;

    case NodeKind::Parse: {
        auto sv = std::dynamic_pointer_cast<StringValue>(alt.value);
        if (!sv) return false;
        const std::string& pn = sv->data();
        if (vt == ValueType::Int    && pn == "int")    return true;
        if (vt == ValueType::UInt   && pn == "uint")   return true;
        if (vt == ValueType::Real   && pn == "float")  return true;
        if (vt == ValueType::String && pn == "string") return true;
        return false;
    }

    case NodeKind::Key: {
        // Match if any Value-kind child has an identity-equal constant.
        // For Phase 5 this covers the JSON null/true/false case where the
        // Value children hold the global singletons.
        for (NodeId child_id : alt.children) {
            NodeId resolved = g.resolve_ref(child_id);
            const Node& vc = g.node(resolved);
            if (vc.kind == NodeKind::Value && vc.value && vc.value.get() == &v) {
                return true;
            }
        }
        return false;
    }

    default:
        return false;
    }
}

tl::expected<void, SaveError> save_node(const Grammar& g, std::ostream& out,
                                        NodeId node_id, SaveCursor& cursor, int depth);

tl::expected<void, SaveError> save_node_body(const Grammar& g, std::ostream& out,
                                              NodeId node_id, SaveCursor& cursor,
                                              int depth) {
    const NodeId resolved = g.resolve_ref(node_id);
    const Node& n = g.node(resolved);

    switch (n.kind) {
    case NodeKind::Key: {
        auto sv = std::dynamic_pointer_cast<StringValue>(n.value);
        if (!sv) return tl::unexpected(SaveError{"Key without literal token"});
        // A Key with Value-kind children is being used as a discriminator
        // (e.g., the null/true/false alternatives in a JSON-value Choice).
        // It consumes the matched value from the cursor on save so that
        // the enclosing Repeat / Sequence advances correctly. A Key
        // without Value-kind children is purely structural ("{", ":")
        // and consumes nothing.
        bool is_discriminator = false;
        for (NodeId child_id : n.children) {
            NodeId rc = g.resolve_ref(child_id);
            if (g.node(rc).kind == NodeKind::Value) {
                is_discriminator = true;
                break;
            }
        }
        if (is_discriminator && cursor.has_more()) {
            cursor.next();
        }
        out << sv->data();
        return {};
    }

    case NodeKind::Value:
        // Discriminator only. No text emitted.
        return {};

    case NodeKind::Parse: {
        if (!cursor.has_more()) {
            return tl::unexpected(SaveError{"Parse node with no value to consume"});
        }
        auto sv = std::dynamic_pointer_cast<StringValue>(n.value);
        if (!sv) return tl::unexpected(SaveError{"Parse node without parser name"});
        Parser* p = g.parser(sv->data());
        if (!p) return tl::unexpected(SaveError{"unknown parser: " + sv->data()});
        const auto& ev = cursor.next();
        if (!ev.value) return tl::unexpected(SaveError{"null value to Parse"});
        auto r = p->unparse(*ev.value);
        if (!r) return tl::unexpected(r.error());
        out << *r;
        return {};
    }

    case NodeKind::Sequence: {
        if (n.container == Container::None) {
            for (NodeId child : n.children) {
                auto r = save_node(g, out, child, cursor, depth);
                if (!r) return r;
            }
            return {};
        }
        // Container-Sequence: consume one value, split into an inner stream.
        if (!cursor.has_more()) {
            return tl::unexpected(SaveError{"container Sequence with no value"});
        }
        const auto& ev = cursor.next();
        SaveCursor inner;
        if (n.container == Container::Array) {
            auto arr = std::dynamic_pointer_cast<ArrayValue>(ev.value);
            if (!arr) return tl::unexpected(SaveError{
                "expected ArrayValue for container=Array sequence"});
            inner.values.reserve(arr->data().size());
            for (const auto& elem : arr->data()) {
                inner.values.push_back({elem, false});
            }
        } else if (n.container == Container::Dict) {
            auto dict = std::dynamic_pointer_cast<DictValue>(ev.value);
            if (!dict) return tl::unexpected(SaveError{
                "expected DictValue for container=Dict sequence"});
            inner.values.reserve(dict->data().size() * 2);
            for (const auto& [name, val] : dict->data()) {
                inner.values.push_back({make_string(name), true});
                inner.values.push_back({val, false});
            }
        }
        for (NodeId child : n.children) {
            auto r = save_node(g, out, child, inner, depth);
            if (!r) return r;
        }
        return {};
    }

    case NodeKind::Repeat: {
        const bool has_sep = n.has_separator && n.children.size() >= 2;
        NodeId sep_id  = has_sep ? n.children[0] : NodeId{};
        NodeId item_id = has_sep ? n.children[1] : (n.children.empty() ? NodeId{} : n.children[0]);
        if (!item_id.valid()) return {};  // empty repeat — nothing to emit
        bool first = true;
        while (cursor.has_more()) {
            if (!first && sep_id.valid()) {
                auto r = save_node(g, out, sep_id, cursor, depth);
                if (!r) return r;
            }
            auto r = save_node(g, out, item_id, cursor, depth);
            if (!r) return r;
            first = false;
        }
        return {};
    }

    case NodeKind::Choice: {
        if (!cursor.has_more()) {
            return tl::unexpected(SaveError{"Choice with no value to dispatch on"});
        }
        const auto& ev = cursor.peek();
        if (!ev.value) return tl::unexpected(SaveError{"null value at Choice"});
        NodeId picked = choose_alternative(g, n, *ev.value);
        if (!picked.valid()) {
            return tl::unexpected(SaveError{
                "no matching grammar alternative for value type"});
        }
        return save_node(g, out, picked, cursor, depth);
    }

    case NodeKind::Ref:
        // Already resolved at entry; unreachable.
        return tl::unexpected(SaveError{"internal: unresolved Ref at save"});
    }
    return {};
}

// Wrapper: applies pretty-print attributes (depth-aware indent, tail, space,
// newline) around the body switch. Order: tab (body_depth × indent_step)
// before content; tail then space then newline after. Both the original
// (possibly Ref) node and the resolved target contribute their attrs in
// onion order.
//
// The depth bump from `indent` (depth_in) happens BEFORE `tab` is emitted,
// so a Node with both flags emits its tab at the bumped depth. This makes
// the common idiom `<PAIR> tab indent` (or `repeat ... tab ... indent`)
// work: each iteration of the indented scope emits its leading indent at
// the new depth.
tl::expected<void, SaveError> save_node(const Grammar& g, std::ostream& out,
                                        NodeId node_id, SaveCursor& cursor,
                                        int depth) {
    if (!node_id.valid()) return {};
    const NodeId resolved = g.resolve_ref(node_id);
    const Node& orig = g.node(node_id);
    const Node& n    = g.node(resolved);
    const bool ref_diff = (resolved.value() != node_id.value());

    int body_depth = depth;
    if (orig.depth_in) ++body_depth;
    if (ref_diff && n.depth_in) ++body_depth;

    auto emit_tab = [&](const Node& node) {
        if (node.indent_emit) {
            for (int i = 0; i < body_depth; ++i) out << g.indent_step();
        }
    };
    auto emit_post = [&](const Node& node) {
        if (!node.tail.empty()) out << node.tail;
        if (node.space_after)   out << ' ';
        if (node.newline_after) out << '\n';
    };

    emit_tab(orig);
    if (ref_diff) emit_tab(n);

    auto r = save_node_body(g, out, node_id, cursor, body_depth);
    if (!r) return r;

    if (ref_diff) emit_post(n);
    emit_post(orig);
    return {};
}

} // namespace

tl::expected<void, SaveError> Grammar::save(std::ostream& out, ValuePtr value) const {
    if (!value) return tl::unexpected(SaveError{"save: null root value"});
    SaveCursor cursor;
    cursor.values.push_back({std::move(value), false});
    return save_node(*this, out, top_, cursor, 0);
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
