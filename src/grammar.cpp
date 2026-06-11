#include <rawast/grammar.hpp>
#include <rawast/parsers.hpp>
#include <rawast/parsers_registry.hpp>

#include "save_stack.hpp"

#include "frame.hpp"

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <ostream>
#include <set>
#include <sstream>
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
NodeId Grammar::new_raw()      { return allocate_(NodeKind::Raw); }

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

// --- First-byte precomputation ------------------------------------------
//
// For each node, compute the set of input first-bytes its first
// non-nullable terminal can begin with. Used by parse_from to skip
// optional / choice-alt frame pushes when the input first byte
// can't match. Recursive; we memoize during the walk.
namespace {

struct FirstByteResult {
    std::bitset<256> bytes;
    bool nullable = false;
    bool known    = true;  // false ⇒ "any byte possible, fall back"
};

// Mark "every byte possible" — used for Parse nodes whose set
// we don't try to derive analytically.
inline FirstByteResult any_byte_result() {
    FirstByteResult r;
    r.bytes.set();
    r.nullable = false;
    r.known    = false;
    return r;
}

FirstByteResult compute_node_first_bytes(
        const Grammar& g, NodeId id,
        std::vector<int>& state, std::vector<FirstByteResult>& cache) {
    if (!id.valid()) {
        FirstByteResult r;
        r.nullable = true;
        return r;
    }
    std::size_t i = id.value();
    if (state[i] == 2) return cache[i];
    if (state[i] == 1) {
        // Cycle — treat as "unknown" so the parse driver falls back.
        return any_byte_result();
    }
    state[i] = 1;

    FirstByteResult result;
    // Track the ORIGINAL node's is_optional flag — for a Ref like
    // `?<RULE>`, the optional sits on the Ref, not on RULE's body.
    // An optional Ref is nullable: when the input's next byte
    // isn't in its first-byte set, the optional can SKIP, and the
    // sequence continues to the NEXT child. Without this, a rule
    // like `?<VIS>, ?<LIFE>, <TYPE>` would have a first-byte set
    // limited to VIS's (l, p) because VIS isn't itself nullable —
    // the engine's peek-and-skip would then wrongly exclude inputs
    // starting with TYPE keywords (i, w, r, ...).
    const Node& orig = g.node(id);
    const bool orig_optional = orig.is_optional;
    NodeId resolved = g.resolve_ref(id);
    const Node& n = g.node(resolved);

    switch (n.kind) {
    case NodeKind::Value:
        // Doesn't consume input.
        result.nullable = true;
        break;
    case NodeKind::Key: {
        // Single literal token. First byte is the first char of the
        // key (if non-empty). Otherwise nullable.
        if (auto sv = std::dynamic_pointer_cast<StringValue>(n.value)) {
            if (!sv->data().empty()) {
                result.bytes.set(static_cast<unsigned char>(sv->data().front()));
                result.known = true;
            } else {
                result.nullable = true;
            }
        } else {
            result = any_byte_result();
        }
        break;
    }
    case NodeKind::Parse: {
        // Some standard terminal parsers have well-defined first-byte
        // sets that we can hardcode here — `int`/`uint`/`float`
        // accept only `[0-9+-]` (plus `.` for float), and `string`
        // requires a leading `"`. Knowing those lets the engine
        // skip ?<X> optionals like `?<STEP_PATTERN>` (4 int/float
        // arguments) when the input cursor is on `;` or any other
        // non-numeric byte. Tested on the LEF corpus: profiles
        // showed STEP_PATTERN burning 4.7% of parse time at 100%
        // fail rate, almost entirely from `;`-position dispatches.
        //
        // Bare `identifier` / `qualified_identifier` are NOT hardcoded
        // because they may resolve to either std (strict
        // `[A-Za-z_]…`) or lefdef (permissive — accepts dots,
        // hyphens, brackets, digits) — taking the union would be
        // too permissive to optimize, and being stricter than the
        // active parser would wrongly skip valid input. Leave them
        // as "any" until the bare-alias resolution is deterministic
        // at compute time.
        std::string pname;
        if (auto sv = std::dynamic_pointer_cast<StringValue>(n.value)) {
            pname = sv->data();
        }
        auto set_digit_signs = [](std::bitset<256>& b, bool with_plus,
                                   bool with_dot) {
            b.set(static_cast<unsigned char>('-'));
            if (with_plus) b.set(static_cast<unsigned char>('+'));
            if (with_dot)  b.set(static_cast<unsigned char>('.'));
            for (char c = '0'; c <= '9'; ++c) {
                b.set(static_cast<unsigned char>(c));
            }
        };
        if (pname == "int" || pname == "std.int") {
            set_digit_signs(result.bytes, /*with_plus=*/false,
                            /*with_dot=*/false);
            result.known = true;
        } else if (pname == "uint" || pname == "std.uint") {
            for (char c = '0'; c <= '9'; ++c) {
                result.bytes.set(static_cast<unsigned char>(c));
            }
            result.known = true;
        } else if (pname == "float" || pname == "std.float") {
            set_digit_signs(result.bytes, /*with_plus=*/true,
                            /*with_dot=*/true);
            result.known = true;
        } else if (pname == "string" || pname == "std.string") {
            result.bytes.set(static_cast<unsigned char>('"'));
            result.known = true;
        } else {
            result = any_byte_result();
        }
        break;
    }
    case NodeKind::Raw:
        // Raw consumes anything until a stop literal; first byte
        // unconstrained.
        result = any_byte_result();
        break;
    case NodeKind::Choice: {
        // Union of children's first-byte sets. Nullable iff any
        // alternative is nullable.
        for (NodeId c : n.children) {
            auto sub = compute_node_first_bytes(g, c, state, cache);
            if (!sub.known) {
                result = any_byte_result();
                break;
            }
            result.bytes |= sub.bytes;
            if (sub.nullable) result.nullable = true;
        }
        break;
    }
    case NodeKind::Sequence: {
        // Walk children until we hit a non-nullable one. Children
        // before that contribute their first-bytes to the union.
        // If all children nullable, the sequence is nullable.
        result.nullable = true;
        bool ok = true;
        for (NodeId c : n.children) {
            const Node& cn = g.node(g.resolve_ref(c));
            if (cn.kind == NodeKind::Value) continue;
            auto sub = compute_node_first_bytes(g, c, state, cache);
            if (!sub.known) {
                result = any_byte_result();
                ok = false;
                break;
            }
            result.bytes |= sub.bytes;
            if (!sub.nullable) {
                result.nullable = false;
                break;
            }
        }
        if (!ok) break;
        break;
    }
    case NodeKind::Repeat: {
        // `repeat <X>` is nullable (0 iterations possible) — its
        // first-byte set is X's first-byte set.
        result.nullable = true;
        const std::size_t item_idx = n.has_separator ? 1 : 0;
        if (item_idx < n.children.size()) {
            auto sub = compute_node_first_bytes(
                g, n.children[item_idx], state, cache);
            if (!sub.known) {
                result = any_byte_result();
                result.nullable = true;
            } else {
                result.bytes |= sub.bytes;
            }
        }
        break;
    }
    case NodeKind::Ref:
        // Should have been resolved above.
        result = any_byte_result();
        break;
    }

    // An optional original node is nullable — its first-byte set
    // is the union with "anything that comes after." The Sequence
    // walker that called us uses `nullable` to decide whether to
    // continue accumulating next-child bytes; making this true for
    // any optional fixes the `?<VIS>, ?<LIFE>, <TYPE>` first-byte
    // computation.
    if (orig_optional) result.nullable = true;

    state[i] = 2;
    cache[i] = result;
    return result;
}

} // namespace

void Grammar::compute_first_bytes() const {
    if (first_bytes_computed_) return;
    const std::size_t N = nodes_.size();
    first_bytes_.assign(N, std::bitset<256>{});
    first_bytes_known_.assign(N, false);
    is_optional_chain_.assign(N, false);
    std::vector<int>             state(N, 0);
    std::vector<FirstByteResult> cache(N);
    for (std::size_t i = 0; i < N; ++i) {
        if (state[i] != 0) continue;
        compute_node_first_bytes(*this, NodeId{i}, state, cache);
    }
    for (std::size_t i = 0; i < N; ++i) {
        if (state[i] == 2 && cache[i].known) {
            first_bytes_known_[i] = true;
            // Nullable nodes: any byte is fine (since "skip" is a
            // valid match). Encode as "known" + all bits set so the
            // parse loop's peek check never rejects them.
            if (cache[i].nullable) {
                first_bytes_[i].set();
            } else {
                first_bytes_[i] = cache[i].bytes;
            }
        }
    }
    // Precompute the "is this node in an optional chain?" flag per
    // NodeId. Lets should_skip_optional in parse_from short-circuit
    // in O(1) on the common case (non-optional pushes). A node is in
    // an optional chain if its own is_optional is set, OR it's a Ref
    // and the resolved chain contains an is_optional link.
    for (std::size_t i = 0; i < N; ++i) {
        if (nodes_[i].is_optional) {
            is_optional_chain_[i] = true;
            continue;
        }
        if (nodes_[i].kind != NodeKind::Ref) continue;
        NodeId cur{i};
        bool optional = false;
        while (cur.valid()) {
            const Node& cn = nodes_[cur.value()];
            if (cn.is_optional) { optional = true; break; }
            if (cn.kind != NodeKind::Ref) break;
            auto sv = std::dynamic_pointer_cast<StringValue>(cn.value);
            if (!sv) break;
            if (!has_rule(sv->data())) break;
            NodeId next = rule_id(sv->data());
            if (next.value() == cur.value()) break;
            cur = next;
        }
        is_optional_chain_[i] = optional;
    }
    first_bytes_computed_ = true;
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

void Grammar::set_strict(NodeId id) {
    nodes_[id.value()].strict = true;
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

void Grammar::add_rule_ignore(std::string rule_name,
                              std::vector<std::string> parser_names) {
    rule_ignore_names_[std::move(rule_name)] = std::move(parser_names);
    rule_ignore_dirty_ = true;
}

void Grammar::set_pending_subparse(NodeId target, std::string rule_name) {
    pending_subparse_.emplace_back(target, std::move(rule_name));
}

tl::expected<void, std::string> Grammar::resolve_subparse_refs() {
    for (const auto& [target, rule_name] : pending_subparse_) {
        auto it = named_rules_.find(rule_name);
        if (it == named_rules_.end()) {
            return tl::unexpected("subparse references undefined rule '" +
                                  rule_name + "'");
        }
        node(target).subparse_start = it->second;
    }
    pending_subparse_.clear();
    return {};
}

const std::vector<Parser*>* Grammar::rule_ignore(NodeId rule_node) const {
    if (rule_ignore_dirty_) {
        rule_ignore_resolved_.clear();
        for (const auto& [rule_name, parser_names] : rule_ignore_names_) {
            auto rit = named_rules_.find(rule_name);
            if (rit == named_rules_.end()) continue;
            std::vector<Parser*> ps;
            ps.reserve(parser_names.size());
            for (const auto& pn : parser_names) {
                auto pit = parsers_.find(pn);
                if (pit == parsers_.end()) continue;
                ps.push_back(pit->second.get());
            }
            rule_ignore_resolved_[rit->second.value()] = std::move(ps);
        }
        rule_ignore_dirty_ = false;
    }
    auto it = rule_ignore_resolved_.find(rule_node.value());
    return it == rule_ignore_resolved_.end() ? nullptr : &it->second;
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
    // The same parser may appear in rule-local override lists; rebuild
    // them lazily next time rule_ignore() is called.
    rule_ignore_dirty_ = true;
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
    // Fast path: lookup precomputed resolution table.
    if (resolved_refs_computed_) {
        return id.value() < resolved_refs_.size()
            ? resolved_refs_[id.value()]
            : id;
    }
    // Slow path (called pre-cache, e.g. during loader phases that
    // run before ensure_refs_resolved_).
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

void Grammar::ensure_refs_resolved_() const {
    if (resolved_refs_computed_) return;
    const std::size_t N = nodes_.size();
    resolved_refs_.resize(N);
    for (std::size_t i = 0; i < N; ++i) {
        NodeId id{i};
        // Re-walk via the slow path (which is still active because
        // resolved_refs_computed_ is false). After this loop the
        // fast path takes over.
        while (id.valid() && nodes_[id.value()].kind == NodeKind::Ref) {
            const auto& v = nodes_[id.value()].value;
            auto sv = std::dynamic_pointer_cast<StringValue>(v);
            assert(sv);
            auto it = named_rules_.find(sv->data());
            assert(it != named_rules_.end());
            id = it->second;
        }
        resolved_refs_[i] = id;
    }
    resolved_refs_computed_ = true;
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

std::vector<std::string> Grammar::parser_groups() const {
    // Every parser registered via apply_parser_group is registered TWICE
    // in parsers_: once under the bare name (e.g. "int") and once under
    // the dotted "group.bare" alias (e.g. "std.int"). The presence of the
    // dotted form is the only signal that the parser belongs to a group;
    // ad-hoc parsers registered without a group don't get dotted aliases.
    std::set<std::string> groups;
    for (const auto& [name, _ignored] : parsers_) {
        auto dot = name.find('.');
        if (dot == std::string::npos) continue;
        groups.insert(name.substr(0, dot));
    }
    return {groups.begin(), groups.end()};
}

// -------------------------------------------------------------------------
// Load driver — trampolined recursive descent
// -------------------------------------------------------------------------

namespace {

void run_ignore(StreamReader& sr, const std::vector<Parser*>& ignores) {
    // Loop until a full pass through the ignore list consumes nothing.
    // This handles arbitrary interleaving of whitespace and comments —
    // e.g. "  // line\n  /* block */\n  " requires multiple cycles
    // because each ignore parser only consumes one contiguous run.
    while (true) {
        const std::size_t before = sr.position().bytes;
        for (Parser* p : ignores) {
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
    return parse_from(sr, pool, top_);
}

tl::expected<ValuePtr, ParseError> Grammar::parse_from(
        StreamReader& sr, const std::string& start_name) const {
    ValuePool pool;
    return parse_from(sr, pool, start_name);
}

tl::expected<ValuePtr, ParseError> Grammar::parse_from(
        StreamReader& sr, ValuePool& pool, const std::string& start_name) const {
    auto it = named_rules_.find(start_name);
    if (it == named_rules_.end()) {
        return tl::unexpected(ParseError{
            sr.position(),
            "parse_from: no rule named '" + start_name + "'"});
    }
    return parse_from(sr, pool, it->second);
}

tl::expected<ValuePtr, ParseError> Grammar::parse_from(
        StreamReader& sr, ValuePool& pool, NodeId start) const {
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

    // Profiling mode: enabled via Grammar::profile_enable(). When on,
    // the loop tracks per-Node entry counts, fail counts, wall-clock
    // time spent on the parse stack, and max depth. Results land on
    // `last_profile_report_` at the end of the call; the public API
    // exposes them via Grammar::last_profile_report(). Off by
    // default — one bool check at each instrumentation site.
    const bool profile_enabled = profile_enabled_;
    std::vector<ProfileEntry>                        profile_per_node;
    std::vector<std::chrono::steady_clock::time_point> profile_frame_starts;
    std::vector<NodeId>                               profile_frame_ids;
    std::uint64_t                                     profile_total_frames = 0;
    std::uint64_t                                     profile_max_depth    = 0;
    const auto                                        profile_start_clock =
        std::chrono::steady_clock::now();
    if (profile_enabled) {
        profile_per_node.assign(nodes_.size(), ProfileEntry{});
        for (std::size_t i = 0; i < nodes_.size(); ++i) {
            profile_per_node[i].node_id = NodeId{i};
        }
    }

    // Sync the profile state against the live stack. Called at the
    // top of every loop iteration: detects push events (stack grew
    // since last sync — record start time, bump entry_count, update
    // max_depth) and pop events (stack shrunk — accumulate elapsed
    // wall-clock time for each popped frame back into its NodeId's
    // entry). Idempotent if the stack hasn't changed.
    auto profile_sync = [&]() {
        if (!profile_enabled) return;
        while (profile_frame_starts.size() < stack.size()) {
            std::size_t idx = profile_frame_starts.size();
            profile_frame_starts.push_back(std::chrono::steady_clock::now());
            NodeId nid = stack[idx].node_id();
            profile_frame_ids.push_back(nid);
            ++profile_total_frames;
            std::size_t v = nid.value();
            if (v < profile_per_node.size()) {
                ++profile_per_node[v].entry_count;
                if (stack.size() > profile_per_node[v].max_depth) {
                    profile_per_node[v].max_depth = stack.size();
                }
            }
        }
        if (stack.size() > profile_max_depth) profile_max_depth = stack.size();
        while (profile_frame_starts.size() > stack.size()) {
            auto end = std::chrono::steady_clock::now();
            auto elapsed_ns =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    end - profile_frame_starts.back()).count();
            NodeId nid = profile_frame_ids.back();
            std::size_t v = nid.value();
            if (v < profile_per_node.size()) {
                profile_per_node[v].total_ns += elapsed_ns;
            }
            profile_frame_starts.pop_back();
            profile_frame_ids.pop_back();
        }
    };

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
            case NodeKind::Raw:      kind = "raw"; break;
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

    // Rule-local ignore overrides ---------------------------------------
    //
    // An IgnoreScope captures "stack depth at which this override was
    // pushed". After every pop, the loop discards scopes whose depth
    // exceeds the current stack size — i.e. whose owning frame is gone.
    // The active ignore list is the top of the stack, or the
    // grammar-level default if the stack is empty.
    struct IgnoreScope {
        std::size_t entry_depth;
        const std::vector<Parser*>* parsers;
    };
    std::vector<IgnoreScope> ignore_stack;
    auto current_ignore = [&]() -> const std::vector<Parser*>& {
        return ignore_stack.empty() ? ignore() : *ignore_stack.back().parsers;
    };
    auto trim_ignore_stack = [&]() {
        while (!ignore_stack.empty()
               && ignore_stack.back().entry_depth > stack.size()) {
            ignore_stack.pop_back();
        }
    };

    // Wrap a push_node so optional frames pick up a stream mark on entry.
    // Without this, an optional sub-rule that consumes input then fails
    // mid-parse would leave the stream advanced — the "treat as empty"
    // path in handle_failure would never rewind. Parallel to Choice's
    // mark/reject around alternative attempts.
    // Lazily compute per-Node first-byte sets the first time we need
    // them. Cached on the Grammar across parses.
    auto ensure_first_bytes = [this]() {
        if (!first_bytes_computed_) compute_first_bytes();
    };

    // True if the about-to-be-pushed node is an optional whose
    // first-byte set is known and doesn't include the next input
    // byte. Lets the parse driver skip the push + key-parse + rewind
    // for `?<MASK_MOD>` / `?<ITERATE_PREFIX>` / `?<STEP_PATTERN>` and
    // every other optional clause that doesn't fire on the current
    // input. Profiling showed those three alone burn ~14% of the
    // wall-clock parse time on a real Sky130 / asap7 / gf130bcd
    // corpus, all at 100% fail rate.
    //
    // Conservative: returns false unless the optionality AND the
    // first-byte miss are both confirmed. Anything ambiguous falls
    // back to the normal push-and-try path, preserving correctness.
    // Choice-alt peek-and-skip. Distinct from should_skip_optional:
    // applies to ANY Choice alternative regardless of optional-chain
    // status — at a Choice dispatch site, an alt whose first-byte
    // set is known and doesn't include the next input byte cannot
    // possibly match, so we can step past it without pushing. The
    // big payoff is on 46-alt closed-keyword choices like
    // LAYER_KEYWORD where ~97% of attempts on real LEFs rewind on
    // the first character of the key.
    auto choice_alt_cant_match = [this, &sr, &current_ignore,
                                   &ensure_first_bytes](NodeId alt) -> bool {
        ensure_first_bytes();
        NodeId resolved = resolve_ref(alt);
        if (resolved.value() >= first_bytes_known_.size()) return false;
        if (!first_bytes_known_[resolved.value()])         return false;
        run_ignore(sr, current_ignore());
        auto c = sr.peek();
        if (!c) return false;
        return !first_bytes_[resolved.value()].test(
            static_cast<unsigned char>(*c));
    };

    auto should_skip_optional = [this, &sr, &current_ignore,
                                  &ensure_first_bytes](NodeId id) -> bool {
        // Fast path: precomputed flag tells us in O(1) whether this
        // node is in an `?<...>` use-site optional chain. Non-
        // optional pushes (the vast majority — Sequence children,
        // Choice alts, Repeat items) return immediately without
        // touching the input stream.
        ensure_first_bytes();
        if (id.value() >= is_optional_chain_.size()) return false;
        if (!is_optional_chain_[id.value()])         return false;
        NodeId resolved = resolve_ref(id);
        if (resolved.value() >= first_bytes_known_.size()) return false;
        if (!first_bytes_known_[resolved.value()])         return false;
        // Peek the next byte after ignore-skip. Whitespace skipping
        // is idempotent; if we later push the frame, the inner
        // terminal re-runs ignore harmlessly.
        run_ignore(sr, current_ignore());
        auto c = sr.peek();
        if (!c) return false;   // EOF — let the engine handle
        return !first_bytes_[resolved.value()].test(
            static_cast<unsigned char>(*c));
    };

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
        // Rule-local ignore: if the just-pushed frame's node has an
        // override registered, push it. Lives for the lifetime of the
        // frame and any children — trim_ignore_stack() pops it when
        // the frame goes away.
        if (!stack.empty()) {
            if (const auto* ovr = rule_ignore(stack.back().node_id())) {
                ignore_stack.push_back({stack.size(), ovr});
            }
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
            case NodeKind::Repeat:
                // Repeat case: accept the just-completed iteration's
                // mark (if any) before stepping. Each successful
                // iteration commits its consumption; a fresh mark is
                // pushed when the next iteration starts (in the
                // Repeat case at the main switch). This is what makes
                // a failing iteration roll back to the position
                // BEFORE the iteration started — without it, a Choice
                // inside the body that committed (accepted its own
                // mark) would leave the cursor advanced even if a
                // later required item failed.
                if (top.has_mark()) {
                    on_mark_accept();
                    top.set_has_mark(false);
                }
                more = top.step_next();
                break;
            case NodeKind::Sequence:
            case NodeKind::Key:
            case NodeKind::Parse:
            case NodeKind::Raw:
                // Key, Parse, and Raw iterate their (typically Value-
                // kind) children after the terminal succeeds; same
                // iteration model as Sequence.
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

    // Push `child_id` or, if it's an optional whose first-byte set
    // doesn't include the next input byte, advance the parent's
    // iteration instead. Same observable effect as pushing a
    // doomed-to-fail frame and recovering through the optional
    // fallback path — just skips the frame churn. Use this in
    // place of `push_with_optional_mark` at the dispatch sites
    // where the next child can be optional.
    auto push_or_skip_optional = [&](NodeId child_id) {
        if (should_skip_optional(child_id)) {
            if (stack.empty()) return;
            bool more = stack.back().step_next();
            if (!more) advance_after_child();
        } else {
            push_with_optional_mark(child_id);
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
            if (profile_enabled) {
                std::size_t v = stack.back().node_id().value();
                if (v < profile_per_node.size()) {
                    ++profile_per_node[v].fail_count;
                }
            }
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
                // Reject the failed iteration's mark FIRST — this
                // rolls the cursor back to the position before the
                // iteration started. Without this, partial progress
                // inside the failed iteration body (especially a
                // Choice that committed before a later required item
                // failed) leaves the cursor advanced and breaks the
                // outer rule's continuation.
                if (popped.has_mark()) {
                    on_mark_reject();
                    popped.set_has_mark(false);
                }
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

    push_with_optional_mark(start);
    profile_sync();

    while (!stack.empty() && !parse_finished) {
        // Pop any rule-local ignore scopes whose owning frame has been
        // popped since the previous iteration.
        trim_ignore_stack();
        profile_sync();   // catch push/pop events from the previous iteration
        if (stack.empty() || parse_finished) break;
        Frame& top = stack.back();
        switch (top.kind()) {

        case NodeKind::Raw: {
            // Raw consume: bypass the ignore-set (embedded whitespace
            // and newlines are part of the captured payload), then
            // peek-and-skip until the stop literal matches at the
            // cursor. The literal is NOT consumed — it's left for the
            // next sibling (a Key node) to match in its own iteration.
            const Node& n = nodes_[top.node_id().value()];
            auto sv = std::dynamic_pointer_cast<StringValue>(n.value);
            assert(sv);
            const std::string& stop = sv->data();
            if (trace_enabled) trace("  raw until \"" + stop + "\"");
            sr.mark();
            const Position start = sr.position();
            std::string captured;
            bool found = false;
            while (true) {
                auto c = sr.peek();
                if (!c) break;
                if (*c == stop[0]) {
                    sr.mark();
                    std::string lookahead;
                    for (std::size_t i = 0; i < stop.size(); ++i) {
                        auto k = sr.get();
                        if (!k) break;
                        lookahead.push_back(*k);
                    }
                    sr.reject();
                    if (lookahead == stop) { found = true; break; }
                }
                captured.push_back(*c);
                sr.get();
            }
            if (!found) {
                sr.reject();
                handle_failure(ParseError{
                    start, "raw consume: stop literal '"
                            + stop + "' not found before EOF"});
                break;
            }
            sr.accept();
            ValuePtr produced = make_string(std::move(captured));
            // Subparse hook: same as the Parse node branch — if this
            // Raw node carries a subparse_start, re-enter the engine
            // on the captured string with the named rule as entry.
            // The resulting sub-tree replaces the original string.
            if (n.subparse_start.valid()) {
                auto produced_sv = std::dynamic_pointer_cast<StringValue>(produced);
                if (!produced_sv) {
                    handle_failure(ParseError{
                        sr.position(),
                        "subparse requires a string-valued terminal"});
                    break;
                }
                std::istringstream sub_is(produced_sv->data());
                StreamReader sub_sr(sub_is);
                auto sub_r = parse_from(sub_sr, pool, n.subparse_start);
                if (!sub_r) {
                    handle_failure(ParseError{
                        sr.position(),
                        "subparse: " + sub_r.error().message});
                    break;
                }
                produced = *sub_r;
            }
            produced = pool.intern(produced);
            top.add_value(produced, top.is_name());
            if (top.has_current()) {
                push_or_skip_optional(top.current_child());
            } else {
                Frame popped = std::move(stack.back());
                stack.pop_back();
                // Optional frame's entry mark must be accepted so the
                // stream advance becomes permanent. Without this the
                // mark stays on the StreamReader's mark stack and the
                // NEXT mark-pop (Choice accept or any reject) targets
                // the WRONG mark — subtle and ugly.
                if (popped.is_optional() && popped.has_mark()) {
                    on_mark_accept();
                    popped.set_has_mark(false);
                }
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

        case NodeKind::Key: {
            run_ignore(sr, current_ignore());
            const Node& n = nodes_[top.node_id().value()];
            auto sv = std::dynamic_pointer_cast<StringValue>(n.value);
            assert(sv);
            if (trace_enabled) trace("  key \"" + sv->data() + "\"");
            KeyParser p(sv->data(), n.strict);
            auto r = p.parse(sr);
            if (r) {
                // Key matched. The literal itself is not emitted; any
                // Value-kind children, however, contribute their
                // constants when iterated.
                if (top.has_current()) {
                    push_or_skip_optional(top.current_child());
                } else {
                    Frame popped = std::move(stack.back());
                    stack.pop_back();
                    // See the Raw / Parse cases for why the optional
                    // mark accept is required here.
                    if (popped.is_optional() && popped.has_mark()) {
                        on_mark_accept();
                        popped.set_has_mark(false);
                    }
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
            run_ignore(sr, current_ignore());
            const Node& n = nodes_[top.node_id().value()];
            auto sv = std::dynamic_pointer_cast<StringValue>(n.value);
            assert(sv);
            if (trace_enabled) trace("  parse:" + sv->data());
            Parser* p = parser(sv->data());
            assert(p);
            auto r = p->parse(sr);
            if (r) {
                ValuePtr produced = *r;
                // Subparse hook: if this Parse node carries a
                // subparse_start, re-enter the engine on the produced
                // string with the named rule as entry. Result replaces
                // the original string value. Failure propagates as a
                // parse error with the sub-position adjusted into the
                // outer stream's reference frame (best-effort: we
                // surface the inner message with a prefix and the
                // outer stream position).
                if (n.subparse_start.valid()) {
                    auto produced_sv = std::dynamic_pointer_cast<StringValue>(produced);
                    if (!produced_sv) {
                        handle_failure(ParseError{
                            sr.position(),
                            "subparse requires a string-valued terminal"});
                        break;
                    }
                    std::istringstream sub_is(produced_sv->data());
                    StreamReader sub_sr(sub_is);
                    auto sub_r = parse_from(sub_sr, pool, n.subparse_start);
                    if (!sub_r) {
                        handle_failure(ParseError{
                            sr.position(),
                            "subparse: " + sub_r.error().message});
                        break;
                    }
                    produced = *sub_r;
                }
                // Intern the produced primitive so identical content
                // (same string, same int, etc.) shares a canonical
                // ValuePtr across the entire parse.
                ValuePtr canonical = pool.intern(produced);
                top.add_value(canonical, top.is_name());
                if (top.has_current()) {
                    push_or_skip_optional(top.current_child());
                } else {
                    Frame popped = std::move(stack.back());
                    stack.pop_back();
                    // See the Raw / Key cases for why the optional
                    // mark accept is required here.
                    if (popped.is_optional() && popped.has_mark()) {
                        on_mark_accept();
                        popped.set_has_mark(false);
                    }
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
            // Peek-and-skip alts whose first-byte set can't include
            // the next input byte. Steps past doomed alts in O(1)
            // each — for LAYER_KEYWORD (46 alts) and similar
            // closed-keyword choices, this collapses a 97%
            // try-and-rewind workload into a single peek + lookup.
            while (top.has_current()
                   && choice_alt_cant_match(top.current_child())) {
                top.step_next();
            }
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
                push_or_skip_optional(top.current_child());
            } else if (stack.size() > 1) {
                // Every alt's first-byte set excluded the input. The
                // Choice has nothing more to try; trigger the same
                // failure-recovery path the normal "Choice exhausted"
                // unwind takes. handle_failure walks back to the
                // nearest recoverable level (outer Choice with more
                // alts, or optional, or repeat).
                handle_failure(ParseError{
                    sr.position(),
                    "no choice alternative matched input first byte"});
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

        case NodeKind::Repeat: {
            if (top.has_current()) {
                // Mark the start of each iteration so a failed body
                // can roll back cleanly. Standard PEG repeat
                // semantics: partial progress in a failed iteration
                // must not affect the outer rule. Without this, a
                // Choice inside the iteration body that committed
                // (accepted its own mark) would leave the cursor
                // advanced when a later required item in the body
                // failed — the surrounding rule would then fail too,
                // unable to roll back over the committed Choice.
                if (!top.has_mark()) {
                    sr.mark();
                    pending_per_mark.emplace_back();
                    top.set_has_mark(true);
                }
                push_or_skip_optional(top.current_child());
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

        case NodeKind::Sequence: {
            if (top.has_current()) {
                push_or_skip_optional(top.current_child());
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

    // Drain any remaining frames in the profile counters (e.g. the
    // start frame's elapsed time gets added on the final iteration),
    // then build the public report.
    profile_sync();
    if (profile_enabled) {
        last_profile_report_ = {};
        last_profile_report_.total_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - profile_start_clock).count();
        last_profile_report_.total_frames = profile_total_frames;
        last_profile_report_.max_depth    = profile_max_depth;
        for (std::size_t i = 0; i < profile_per_node.size(); ++i) {
            if (profile_per_node[i].entry_count == 0) continue;
            // Look up the rule name (if this NodeId is a named rule's
            // body). Anonymous (inline) nodes keep an empty rule_name.
            for (const auto& [name, rid] : named_rules_) {
                if (rid.value() == i) {
                    profile_per_node[i].rule_name = name;
                    break;
                }
            }
            last_profile_report_.entries.push_back(std::move(profile_per_node[i]));
        }
    }

    if (parse_finished) {
        // Start rule produced a complete value; require the rest of
        // the stream (modulo trailing ignored terminals) to be empty.
        // Without this check a grammar that matches a prefix would
        // silently succeed, hiding coverage gaps when the file has
        // unmodeled content after the matched portion.
        run_ignore(sr, current_ignore());
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


// Grammar::save is defined in src/save_stack.cpp so the save
// engine (helpers + entry point) stays in one file. See the
// stack-navigation walk for how the implementation handles
// fixed/open-schema dicts, key-based Choice dispatch, wrapped
// sub-structures, catch-all alternatives, and the self-host case.

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
