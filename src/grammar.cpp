#include <rawast/grammar.hpp>
#include <rawast/parsers.hpp>
#include <rawast/parsers_registry.hpp>

#include "save_stack.hpp"

#include "frame.hpp"

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <map>
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

void Grammar::set_fixed_schema(NodeId id) {
    nodes_[id.value()].fixed_schema = true;
}

void Grammar::set_min(NodeId id, std::uint32_t m) {
    nodes_[id.value()].min = m;
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

void Grammar::register_parser_alias(std::string key, std::unique_ptr<Parser> p) {
    parsers_[std::move(key)] = std::move(p);
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

// Push a NodeId onto the stack, resolving Ref chains. If any Ref along
// the chain has is_optional=true (the `?<RULE>` site pattern), propagate
// that flag to the pushed Frame — without it the optional fails-as-
// empty handling in handle_failure would never fire for ref-site
// optionals.
void push_node(std::vector<Frame>& stack, const Grammar& g, NodeId id) {
    NodeId resolved = g.resolve_ref(id);
    stack.emplace_back(g, resolved);
    NodeId cur = id;
    while (cur.valid() && cur.value() != resolved.value()
                       && g.node(cur).kind == NodeKind::Ref) {
        if (g.node(cur).is_optional) {
            stack.back().force_optional();
            break;
        }
        auto sv = std::dynamic_pointer_cast<StringValue>(g.node(cur).value);
        if (!sv) break;
        cur = g.rule_id(sv->data());
    }
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

    // Trace mode: enabled via `RAWAST_TRACE` env var. When set, the
    // parse driver emits per-frame events to stderr with byte/line
    // positions and rule names — useful for grammar debugging
    // ("which rule were you in when you failed?"). Zero cost when
    // off: one env-var read at parse start, one bool check at each
    // instrumentation site. See docs/debugging.md.
    const bool trace_enabled = std::getenv("RAWAST_TRACE") != nullptr;

    // Reverse map NodeId → rule name, built once at parse start when
    // tracing is enabled. Used by node_label() to produce
    // human-readable trace messages. Empty when tracing is off (no
    // allocation cost).
    std::map<std::uint32_t, std::string> node_to_rule;
    if (trace_enabled) {
        for (const auto& [name, id] : named_rules_) {
            node_to_rule[id.value()] = name;
        }
    }

    auto node_label = [&](NodeId id) -> std::string {
        auto it = node_to_rule.find(id.value());
        if (it != node_to_rule.end()) return it->second;
        // Anonymous (inline) node — show kind + node id for context.
        const Node& n = nodes_[id.value()];
        const char* kind = "?";
        switch (n.kind) {
            case NodeKind::Sequence: kind = "seq"; break;
            case NodeKind::Choice:   kind = "choice"; break;
            case NodeKind::Repeat:   kind = "repeat"; break;
            case NodeKind::Key:      kind = "key"; break;
            case NodeKind::Parse:    kind = "parse"; break;
            case NodeKind::Ref:      kind = "ref"; break;
            case NodeKind::Value:    kind = "value"; break;
        }
        return std::string{kind} + "#" + std::to_string(id.value());
    };

    auto trace_pos = [&]() -> std::string {
        auto p = sr.position();
        return "L" + std::to_string(p.line)
             + ":C" + std::to_string(p.column);
    };

    auto trace = [&](const std::string& msg) {
        if (!trace_enabled) return;
        // Indent by stack depth so the trace is visually nested.
        std::cerr << "[" << trace_pos() << "] ";
        for (std::size_t i = 0; i < stack.size(); ++i) std::cerr << "  ";
        std::cerr << msg << "\n";
    };

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

    // Wrap a push_node so optional frames pick up a stream mark on entry.
    // Without this, an optional sub-rule that consumes input then fails
    // mid-parse would leave the stream advanced — the "treat as empty"
    // path in handle_failure would never rewind. Parallel to Choice's
    // mark/reject around alternative attempts.
    auto push_with_optional_mark = [&](NodeId id) {
        push_node(stack, *this, id);
        if (trace_enabled && !stack.empty()) {
            const auto& f = stack.back();
            std::string opt = f.is_optional() ? " ?" : "";
            trace("PUSH " + node_label(f.node_id()) + opt);
        }
        if (!stack.empty() && stack.back().is_optional()
            && !stack.back().has_mark()) {
            sr.mark();
            pending_per_mark.emplace_back();
            stack.back().set_has_mark(true);
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
            // Optional frame completed successfully — accept the entry
            // mark so the stream advances permanently.
            if (popped.is_optional() && popped.has_mark()) {
                on_mark_accept();
                popped.set_has_mark(false);
            }
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
    //
    // Order matters: Choice must be checked BEFORE the is_optional
    // fast-path, otherwise an optional Choice (e.g. `?<X>` where X is
    // itself a choice) would treat the first alt's failure as the whole
    // optional failing — never giving the Choice a chance to try
    // remaining alts.
    auto handle_failure = [&](const ParseError& err) {
        if (trace_enabled) {
            trace("FAIL: " + err.message);
        }
        note_progress(max_progress, err);
        while (!stack.empty()) {
            Frame popped = std::move(stack.back());
            stack.pop_back();
            if (trace_enabled) {
                trace("  unwind " + node_label(popped.node_id()));
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
                    if (trace_enabled) {
                        trace("  retry " + node_label(popped.node_id())
                              + " next-alt");
                    }
                    stack.push_back(std::move(popped));
                    return;
                }
                // Else: choice exhausted -- propagate the failure further.
                // If the choice was optional, fall through to the optional
                // handler below.
            }

            if (popped.is_optional()) {
                // Reject the entry mark to rewind the stream to where
                // the optional started — without this, any consumed
                // input before the inner failure is left dangling.
                if (popped.has_mark()) {
                    on_mark_reject();
                    popped.set_has_mark(false);
                }
                // Treat as success-with-empty.
                if (!stack.empty()) {
                    advance_after_child();
                }
                return;
            }

            if (popped.kind() == NodeKind::Repeat) {
                // `repeat+` (min=1) form: the Repeat itself fails if too
                // few iterations matched. Propagate the failure further up
                // so an enclosing Choice/optional can react.
                if (popped.iter_count() < popped.min()) {
                    continue;
                }
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
            // Sequence/Key/Parse: propagate by continuing to pop.
        }
        // Stack drained without recovery -- top-level parse fails.
    };

    push_with_optional_mark(top_);

    while (!stack.empty() && !parse_finished) {
        Frame& top = stack.back();
        switch (top.kind()) {

        case NodeKind::Key: {
            run_ignore(*this, sr);
            const Node& n = nodes_[top.node_id().value()];
            auto sv = std::dynamic_pointer_cast<StringValue>(n.value);
            assert(sv);
            if (trace_enabled) trace("  key \"" + sv->data() + "\"");
            KeyParser p(sv->data());
            auto r = p.parse(sr);
            if (r) {
                // Key matched. The literal itself is not emitted; any
                // Value-kind children, however, contribute their
                // constants when iterated.
                if (top.has_current()) {
                    push_with_optional_mark(top.current_child());
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
            if (trace_enabled) trace("  parse:" + sv->data());
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
                    push_with_optional_mark(top.current_child());
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
                push_with_optional_mark(top.current_child());
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
                push_with_optional_mark(top.current_child());
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
        // Start rule produced a complete value; require the rest of
        // the stream (modulo trailing ignored terminals) to be empty.
        // Without this check a grammar that matches a prefix would
        // silently succeed, hiding coverage gaps when the file has
        // unmodeled content after the matched portion.
        run_ignore(*this, sr);
        if (!sr.eof()) {
            return tl::unexpected(ParseError{
                sr.position(),
                "unexpected content after start rule completed"
            });
        }
        return result_value;
    }
    return tl::unexpected(max_progress);
}


tl::expected<void, SaveError> Grammar::save(std::ostream& out, ValuePtr value,
                                            bool pretty) const {
    // Single save engine — the stack-navigation walk in
    // src/save_stack.cpp (Phase B). Handles fixed-schema dicts,
    // open-schema dicts, key-based Choice dispatch, wrapped sub-
    // structures, catch-all alternatives, and the self-host case.
    return rawast::save_v2(*this, out, value, pretty);
}

// -------------------------------------------------------------------------
// JSON grammar — first working grammar, built in code
// -------------------------------------------------------------------------

Grammar make_json_grammar() {
    Grammar g;

    // The in-memory JSON grammar is JSONC: it tolerates // line and
    // /* block */ comments anywhere whitespace is allowed. This is a
    // pragmatic default for the host-API grammar used to bootstrap
    // JSON-form grammar files (which typically carry inline docs).
    // File-loaded grammars opt into JSONC explicitly via their
    // top-level "ignore" field.
    //
    // Terminal parsers come from the `std` parser group, registered
    // here through the same mechanism a file-loaded grammar uses via
    // `"use": ["std"]`. Both paths converge on the same parser set.
    register_std_parser_group();
    auto r = apply_parser_group(g, "std");
    (void)r;   // std group always present after the call above
    g.add_ignore("whitespace");
    g.add_ignore("line_comment");
    g.add_ignore("block_comment");

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
