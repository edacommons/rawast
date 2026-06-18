#include <rawast/to_value.hpp>

#include <rawast/grammar.hpp>
#include <rawast/node.hpp>
#include <rawast/value.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rawast {

namespace {

// Helpers — keep the emit code readable.

void put(DictValue& d, const std::string& key, ValuePtr v) {
    d.data()[key] = std::move(v);
}

void put_str(DictValue& d, const std::string& key, std::string val) {
    put(d, key, make_string(std::move(val)));
}

void put_bool(DictValue& d, const std::string& key, bool val) {
    put(d, key, val ? true_value() : false_value());
}

void put_int(DictValue& d, const std::string& key, std::int64_t val) {
    put(d, key, make_int(val));
}

// Pretty-print attrs emitted only when set. Tail string is emitted
// raw — the loader's `unescape()` runs on the input side, so the
// stored Node.tail is already the unescaped form. For perfect
// round-trip a re-escape would be needed; leaving raw matches the
// JSON-form convention where the string is the literal value.
void emit_pretty_attrs(DictValue& d, const Node& n) {
    if (n.depth_in)      put_bool(d, "indent", true);
    if (n.indent_emit)   put_bool(d, "tab", true);
    if (n.space_after)   put_bool(d, "space", true);
    if (n.newline_after) put_bool(d, "newline", true);
    if (!n.tail.empty()) put_str(d, "tail", n.tail);
}

ValuePtr node_to_value(const Grammar& g, NodeId id);

// Emit the value subtree for a single Node, using the flat-form shape
// the loader accepts (each child becomes an item dict; binding name-
// markers become explicit `{type:"value", var:true, value:"<name>"}`
// items). This loses the elegant `{expr, bindings}` wrapper form of
// the meta-grammar's output but is structurally faithful and
// uniformly round-trippable.
ValuePtr node_to_value(const Grammar& g, NodeId id) {
    auto d = std::make_shared<DictValue>();
    const Node& n = g.node(id);

    // Universal attrs — apply to every Node kind.
    if (n.is_optional)  put_bool(*d, "optional", true);
    if (n.backtrack)    put_bool(*d, "backtrack", true);
    if (n.fixed_schema) put_bool(*d, "fixed_schema", true);
    emit_pretty_attrs(*d, n);

    switch (n.kind) {

    case NodeKind::Ref: {
        // Refs lower to `{"type": "<rule-name>"}`. Loader checks
        // `g.has_rule(type)` and treats it as a Ref.
        auto sv = std::dynamic_pointer_cast<StringValue>(n.value);
        put_str(*d, "type", sv ? sv->data() : std::string());
        break;
    }

    case NodeKind::Value: {
        put_str(*d, "type", "value");
        put(*d, "value", n.value ? n.value : null_value());
        if (n.is_name) put_bool(*d, "var", true);
        break;
    }

    case NodeKind::Key: {
        auto sv = std::dynamic_pointer_cast<StringValue>(n.value);
        const std::string literal = sv ? sv->data() : std::string();
        put_str(*d, "type", n.strict ? "strict_key" : "key");
        put_str(*d, "key", literal);
        // Loader's populate() for "key" attaches a child Value if
        // `value` or `emit` is present on the dict. Reverse: if the
        // Key has exactly one Value child (no is_name), fold it back.
        if (n.children.size() == 1) {
            const Node& child = g.node(n.children[0]);
            if (child.kind == NodeKind::Value && !child.is_name && child.value) {
                if (auto cv = std::dynamic_pointer_cast<StringValue>(child.value);
                    cv && cv->data() == literal) {
                    put_bool(*d, "emit", true);
                } else {
                    put(*d, "value", child.value);
                }
            }
        }
        // Note: Keys can carry the `is_name` flag (set via binding
        // syntax). The flag lives on the *Key* node in the engine
        // since the binding name-marker is a *separate sibling* in
        // the parent Sequence. So is_name on a Key itself is
        // unexpected; if it occurs, emit `var:true` for round-trip.
        if (n.is_name) put_bool(*d, "var", true);
        break;
    }

    case NodeKind::Parse: {
        auto sv = std::dynamic_pointer_cast<StringValue>(n.value);
        const std::string parser_name = sv ? sv->data() : std::string();
        // Loader uses the `type` field for both bare ("int") and
        // dotted ("std.int") parser names; the parser registry
        // resolves either form.
        put_str(*d, "type", parser_name);
        if (n.is_name) put_bool(*d, "var", true);
        break;
    }

    case NodeKind::Choice:
    case NodeKind::Sequence: {
        put_str(*d, "type", n.kind == NodeKind::Choice ? "choice" : "sequence");
        if      (n.container == Container::Array) put_str(*d, "container", "array");
        else if (n.container == Container::Dict)  put_str(*d, "container", "dict");
        auto items = std::make_shared<ArrayValue>();
        for (NodeId child : n.children) {
            items->data().push_back(node_to_value(g, child));
        }
        put(*d, "value", items);
        break;
    }

    case NodeKind::Scope: {
        // `{type: "scope", container: "array", value: [INNER...]}`.
        // Scope has no start/stop attributes — those come from the
        // surrounding sequence's siblings. Loader's `type == "scope"`
        // branch reads container + INNERs back; `resolve_raw_stops`
        // populates the sibling-driven stop at load time.
        put_str(*d, "type", "scope");
        if (n.container == Container::Array) put_str(*d, "container", "array");
        auto items = std::make_shared<ArrayValue>();
        for (NodeId child : n.children) {
            items->data().push_back(node_to_value(g, child));
        }
        put(*d, "value", items);
        break;
    }

    case NodeKind::Repeat: {
        put_str(*d, "type", "repeat");
        if (n.min > 0) put_int(*d, "min", static_cast<std::int64_t>(n.min));
        // Repeat children layout (see Node.has_separator + Frame
        // construction): if has_separator, children[0] is the
        // separator and children[1] is the body item. Otherwise
        // children[0] is the body item.
        std::size_t body_idx = 0;
        if (n.has_separator && !n.children.empty()) {
            put(*d, "separator", node_to_value(g, n.children[0]));
            body_idx = 1;
        }
        if (n.children.size() > body_idx) {
            put(*d, "value", node_to_value(g, n.children[body_idx]));
        }
        break;
    }

    case NodeKind::Raw: {
        put_str(*d, "type", "raw");
        break;
    }
    }

    return d;
}

} // namespace

ValuePtr to_value(const Grammar& g) {
    auto root = std::make_shared<DictValue>();

    // `use:` — derived from registered parser-group prefixes.
    auto groups = g.parser_groups();
    if (!groups.empty()) {
        auto arr = std::make_shared<ArrayValue>();
        for (auto& name : groups) {
            arr->data().push_back(make_string(std::move(name)));
        }
        put(*root, "use", arr);
    }

    // `start:` — resolved from g.top() (typically a Ref to a named rule).
    if (NodeId top = g.top(); top.valid()) {
        const Node& tn = g.node(top);
        if (tn.kind == NodeKind::Ref) {
            if (auto sv = std::dynamic_pointer_cast<StringValue>(tn.value)) {
                put_str(*root, "start", sv->data());
            }
        }
    }

    // Grammar-wide ignore: NOT serialised at the top level. The JSON
    // loader treats every top-level key other than `start` / `use` as
    // a named rule definition, so emitting `"ignore": [...]` would be
    // misread as a rule whose body is an array. Grammar-wide ignore is
    // a host-side concern (the host calls `g.add_ignore(...)` before
    // load); it's NOT serialised in the grammar data.
    //
    // Rule-local overrides ARE serialised, via `rule_ignore` on the
    // rule body — see the rule loop below.

    // Rule bodies, plus per-rule `rule_ignore` attached to the body
    // dict where applicable.
    const auto& rule_ignores = g.rule_ignore_names();
    for (const auto& [name, id] : g.named_rules()) {
        ValuePtr body = node_to_value(g, id);
        if (auto rii = rule_ignores.find(name); rii != rule_ignores.end()) {
            if (auto bd = std::dynamic_pointer_cast<DictValue>(body)) {
                auto arr = std::make_shared<ArrayValue>();
                for (const auto& pname : rii->second) {
                    arr->data().push_back(make_string(pname));
                }
                bd->data()["rule_ignore"] = arr;
            }
        }
        put(*root, name, body);
    }

    return root;
}

} // namespace rawast
