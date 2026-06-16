#include <rawast/loader.hpp>
#include <rawast/parsers.hpp>
#include <rawast/parsers_registry.hpp>
#include <rawast/preprocessor.hpp>

#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <utility>

namespace rawast {

namespace {

// Helpers --------------------------------------------------------------

tl::expected<std::string, std::string>
dict_string(const DictValue& d, const std::string& key) {
    auto it = d.data().find(key);
    if (it == d.data().end()) {
        std::string keys;
        for (const auto& [k, _] : d.data()) {
            if (!keys.empty()) keys += ", ";
            keys += k;
        }
        return tl::unexpected("missing field '" + key + "' (have: [" + keys + "])");
    }
    auto sv = std::dynamic_pointer_cast<StringValue>(it->second);
    if (!sv) return tl::unexpected("field '" + key + "' is not a string");
    return sv->data();
}

std::optional<std::string>
dict_string_opt(const DictValue& d, const std::string& key) {
    auto it = d.data().find(key);
    if (it == d.data().end()) return std::nullopt;
    auto sv = std::dynamic_pointer_cast<StringValue>(it->second);
    if (!sv) return std::nullopt;
    return sv->data();
}

bool dict_bool(const DictValue& d, const std::string& key) {
    auto it = d.data().find(key);
    if (it == d.data().end()) return false;
    if (!it->second || it->second->type() != ValueType::Bool) return false;
    return std::dynamic_pointer_cast<BoolValue>(it->second)->data();
}

ValuePtr dict_value(const DictValue& d, const std::string& key) {
    auto it = d.data().find(key);
    if (it == d.data().end()) return nullptr;
    return it->second;
}

// Interpret C-style escape sequences in a literal string. The grammar
// loader uses this on `tail` strings (and any other text users embed in
// the grammar file) because the engine's DoubleQuoteStringParser
// preserves backslash sequences verbatim — \n is the two characters
// \ + n, not a newline. We unescape at load time so the save direction
// can emit the intended bytes.
std::string unescape(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] != '\\' || i + 1 == raw.size()) {
            out.push_back(raw[i]);
            continue;
        }
        char next = raw[i + 1];
        switch (next) {
        case 'n':  out.push_back('\n'); ++i; break;
        case 't':  out.push_back('\t'); ++i; break;
        case 'r':  out.push_back('\r'); ++i; break;
        case '0':  out.push_back('\0'); ++i; break;
        case '\\': out.push_back('\\'); ++i; break;
        case '"':  out.push_back('"');  ++i; break;
        default:   out.push_back(raw[i]); break;   // unknown escape: pass through
        }
    }
    return out;
}

// One parsed binding entry — extracted from either the canonical array
// form or the legacy dict form. Empty `name` (with value "@") is the
// var-binding sentinel; the loader sets is_name on the surrounding expr.
struct BindingEntry {
    std::string name;
    ValuePtr    value;
    // True when the binding used the `:#name=...` engine-reserved
    // namespace. The loader dispatches on the bare `name` to engine
    // directives (#subparse, #role, #field) and rejects unknown
    // reserved names. Plain `:name=...` bindings always carry false.
    bool        reserved = false;
};

// Extract binding entries from a `bindings` field value. Accepts:
//   - Array of {name, value} dicts (canonical form, preserves order).
//     A missing `name` field is treated as empty (var sentinel).
//   - Dict of name→value pairs (deprecated legacy form, std::map order).
//   - nullptr (no bindings field) → returns an empty vector.
tl::expected<std::vector<BindingEntry>, std::string>
extract_bindings(const ValuePtr& bindings_val) {
    std::vector<BindingEntry> entries;
    if (!bindings_val) return entries;

    if (auto arr = std::dynamic_pointer_cast<ArrayValue>(bindings_val)) {
        for (const auto& e : arr->data()) {
            auto ed = std::dynamic_pointer_cast<DictValue>(e);
            if (!ed) {
                return tl::unexpected(
                    "bindings: array entry must be a dict {name, value}");
            }
            std::string name;
            if (auto nit = ed->data().find("name");
                nit != ed->data().end()) {
                auto ns = std::dynamic_pointer_cast<StringValue>(nit->second);
                if (!ns) {
                    return tl::unexpected(
                        "bindings: entry 'name' must be a string");
                }
                name = ns->data();
            }
            // `list_append: true` flag (set by the meta-grammar when the
            // surface syntax was `:name[]=@`) is folded back into the
            // name as a `[]` suffix; the dict-assembly loop in frame.cpp
            // strips the suffix and appends to a list under the base name.
            if (auto lit = ed->data().find("list_append");
                lit != ed->data().end()) {
                auto bv = std::dynamic_pointer_cast<BoolValue>(lit->second);
                if (bv && bv->data()) {
                    name.append("[]");
                }
            }
            ValuePtr value;
            if (auto vit = ed->data().find("value");
                vit != ed->data().end()) {
                value = vit->second;
            }
            bool reserved = false;
            if (auto rit = ed->data().find("reserved");
                rit != ed->data().end()) {
                auto bv = std::dynamic_pointer_cast<BoolValue>(rit->second);
                if (bv && bv->data()) reserved = true;
            }
            entries.push_back({std::move(name), std::move(value), reserved});
        }
        return entries;
    }

    if (auto dict = std::dynamic_pointer_cast<DictValue>(bindings_val)) {
        for (const auto& [name, value] : dict->data()) {
            entries.push_back({name, value});
        }
        return entries;
    }

    return tl::unexpected("bindings: must be an array or dict");
}

// Apply pretty-print attributes (depth/indent flags + tail string) read
// from a dict (either a top-level node body or an ITEM wrapper) to a
// built Node. Universal — callable on any node kind.
void apply_pretty_attrs(Grammar& g, NodeId target, const DictValue& d) {
    auto bool_field = [&](const std::string& key) {
        auto it = d.data().find(key);
        if (it == d.data().end()) return false;
        if (!it->second || it->second->type() != ValueType::Bool) return false;
        return std::dynamic_pointer_cast<BoolValue>(it->second)->data();
    };
    if (bool_field("indent"))  g.set_indent(target);
    if (bool_field("tab"))     g.set_tab(target);
    if (bool_field("space"))   g.set_space(target);
    if (bool_field("newline")) g.set_newline(target);
    auto t = d.data().find("tail");
    if (t != d.data().end()) {
        if (auto sv = std::dynamic_pointer_cast<StringValue>(t->second)) {
            g.set_tail(target, unescape(sv->data()));
        }
    }
}

// Forward declaration: populate a placeholder Node (already allocated and
// possibly already registered as a named rule) from a grammar body Value.
tl::expected<void, std::string>
populate(Grammar& g, NodeId target, const Value& body);

// Allocate a fresh placeholder Node and populate it from a body. Used
// for inline (anonymous) sub-items in items arrays / repeat children /
// repeat separators.
tl::expected<NodeId, std::string>
build_inline(Grammar& g, const Value& body) {
    NodeId id = g.new_sequence();   // placeholder kind, populate() mutates it
    auto r = populate(g, id, body);
    if (!r) return tl::unexpected(r.error());
    return id;
}

// Build a single child Node from either an ITEM-wrapper dict (the
// .rawast loader emits `{"expr": <X>, "type": "bare"|"var", ...}` shapes
// for postfix-attr-bearing items) or a direct grammar body (legacy JSON
// form). Used for repeat-item and repeat-separator positions where only
// one child is allowed (so the "binding" wrapper — which expands to
// two children — is rejected here).
tl::expected<NodeId, std::string>
build_item(Grammar& g, const Value& val) {
    if (auto dv = dynamic_cast<const DictValue*>(&val)) {
        auto expr_it = dv->data().find("expr");

        // New multi-binding form: {expr:<X>, bindings:{...}?}. Bindings
        // field is optional (defaults to empty). Only ONE child is
        // allowed at a repeat-item / repeat-separator position, so
        // non-empty bindings (which would emit name markers as siblings)
        // are rejected here unless they only set is_name (var sentinel
        // or legacy {type:"var"}).
        //
        // Skip this path if the dict has a recognised legacy `type`
        // field — the legacy handler below owns it.
        bool has_legacy_type = false;
        if (auto type_it = dv->data().find("type");
            type_it != dv->data().end()) {
            if (auto sv = std::dynamic_pointer_cast<StringValue>(type_it->second)) {
                const std::string& t = sv->data();
                if (t == "bare" || t == "var" || t == "binding"
                    || t == "binding_const") {
                    has_legacy_type = true;
                }
            }
        }
        if (expr_it != dv->data().end() && !has_legacy_type) {
            auto entries_r = extract_bindings(dict_value(*dv, "bindings"));
            if (!entries_r) return tl::unexpected(entries_r.error());
            const auto& entries = *entries_r;
            if (!expr_it->second) {
                return tl::unexpected("multi-binding: null expr");
            }
            bool only_var = false;
            if (entries.empty()) {
                // bare
            } else if (entries.size() == 1) {
                const auto& e = entries[0];
                if (e.name.empty()) {
                    only_var = true;   // empty-name = var sentinel
                }
                if (!only_var) {
                    return tl::unexpected(
                        "binding wrapper not allowed as repeat item or "
                        "separator (would produce two children where one is "
                        "required)");
                }
            } else {
                return tl::unexpected(
                    "binding wrapper not allowed as repeat item or "
                    "separator (would produce two children where one is "
                    "required)");
            }
            auto child_r = build_inline(g, *expr_it->second);
            if (!child_r) return tl::unexpected(child_r.error());
            if (only_var) g.set_name(*child_r);
            apply_pretty_attrs(g, *child_r, *dv);
            return *child_r;
        }

        auto type_it = dv->data().find("type");
        if (type_it != dv->data().end() && expr_it != dv->data().end()) {
            auto type_sv = std::dynamic_pointer_cast<StringValue>(type_it->second);
            if (type_sv) {
                const std::string& t = type_sv->data();
                if (t == "bare" || t == "var") {
                    if (!expr_it->second) {
                        return tl::unexpected(t + " wrapper: null expr");
                    }
                    auto child_r = build_inline(g, *expr_it->second);
                    if (!child_r) return tl::unexpected(child_r.error());
                    if (t == "var") g.set_name(*child_r);
                    apply_pretty_attrs(g, *child_r, *dv);
                    return *child_r;
                }
                if (t == "binding") {
                    return tl::unexpected(
                        "binding wrapper not allowed as repeat item or separator "
                        "(would produce two children where one is required)");
                }
                // Other type values are normal grammar bodies; fall through.
            }
        }
    }
    // EXPR-flattened ITEM dict: the meta-grammar emits the EXPR's
    // fields directly into the ITEM dict (no separate `expr` field),
    // with `bindings` alongside. If bindings are present, build the
    // expr inline AND wrap it in a Sequence carrying the binding's
    // name marker. Lets `repeat <X>:foo[]=@` actually fire the
    // binding — previously the bindings field was silently dropped
    // because neither legacy nor multi-binding path matched the
    // EXPR-flattened shape.
    if (auto dv = dynamic_cast<const DictValue*>(&val)) {
        auto bindings_it = dv->data().find("bindings");
        if (bindings_it != dv->data().end()) {
            auto entries_r = extract_bindings(bindings_it->second);
            if (!entries_r) return tl::unexpected(entries_r.error());
            const auto& entries = *entries_r;
            if (!entries.empty()) {
                auto child_r = build_inline(g, val);
                if (!child_r) return tl::unexpected(child_r.error());
                NodeId wrapper = g.new_sequence();
                for (const auto& e : entries) {
                    if (e.name.empty()) {
                        // Var binding — set is_name on the expr, no
                        // sibling Value node emitted.
                        g.set_name(*child_r);
                        continue;
                    }
                    auto sv = std::dynamic_pointer_cast<StringValue>(e.value);
                    const bool is_at = sv && sv->data() == "@";
                    if (is_at) {
                        NodeId vn = g.new_value(make_string(e.name));
                        g.set_name(vn);
                        g.node(wrapper).children.push_back(vn);
                    } else {
                        // const binding: emit Value-name + Value-const
                        // AFTER the expr (binding_const ordering).
                    }
                }
                g.node(wrapper).children.push_back(*child_r);
                for (const auto& e : entries) {
                    auto sv = std::dynamic_pointer_cast<StringValue>(e.value);
                    const bool is_at = sv && sv->data() == "@";
                    const bool is_var = e.name.empty();
                    if (is_at || is_var) continue;
                    NodeId vn = g.new_value(make_string(e.name));
                    g.set_name(vn);
                    g.node(wrapper).children.push_back(vn);
                    NodeId vc = g.new_value(e.value ? e.value : null_value());
                    g.node(wrapper).children.push_back(vc);
                }
                apply_pretty_attrs(g, wrapper, *dv);
                return wrapper;
            }
        }
    }
    return build_inline(g, val);
}

// Append each entry of an `items` array as a child of `target`. Recognises
// the .rawast binding-wrapper shapes:
//   {"expr":<X>, "type":"bare"}          -> unwrap; build child from <X>
//   {"expr":<X>, "type":"var"}           -> unwrap; build child; set_name
//   {"expr":<X>, "type":"binding", "name":"N"}
//                                         -> expand inline into two children:
//                                            (1) a Value-kind Node holding
//                                                the constant string "N",
//                                                flagged is_name=true;
//                                            (2) the build of <X>.
// Other shapes (bare strings, "sequence"/"choice"/etc. dicts) flow through
// to build_inline as before.
tl::expected<void, std::string>
append_items_array(Grammar& g, NodeId target, const ValuePtr& items_val,
                   const std::string& kind_label) {
    if (!items_val) {
        return tl::unexpected(kind_label + ": missing 'items'");
    }
    auto arr = std::dynamic_pointer_cast<ArrayValue>(items_val);
    if (!arr) return tl::unexpected(kind_label + ": 'items' is not an array");
    for (const auto& item : arr->data()) {
        if (!item) return tl::unexpected(kind_label + ": null item in 'items'");

        // Detect binding-wrapper shape — two forms accepted:
        //   * Wrapper: {"expr": <X>, "bindings": [...]}   — EXPR nested.
        //   * Flat:    {"type": ..., "items": ..., "bindings": [...]}
        //              EXPR's fields flatten into the item dict at top
        //              level (the rawast.rawast design draft shape).
        // Either way, the item-level "bindings" field expands into
        // Value-name + Value-const sibling pairs around the expr.
        //
        // Skip this path if the dict has a recognised legacy `type`
        // field value (bare/var/binding/binding_const) — those are
        // handled by the legacy handler below. The new flat form uses
        // structural type values (sequence/choice/key/parse/...) which
        // don't collide with the legacy markers.
        if (auto item_dict = std::dynamic_pointer_cast<DictValue>(item)) {
            auto expr_it = item_dict->data().find("expr");
            bool has_legacy_type = false;
            if (auto type_it = item_dict->data().find("type");
                type_it != item_dict->data().end()) {
                if (auto sv = std::dynamic_pointer_cast<StringValue>(type_it->second)) {
                    const std::string& t = sv->data();
                    if (t == "bare" || t == "var" || t == "binding"
                        || t == "binding_const") {
                        has_legacy_type = true;
                    }
                }
            }

            // Flat form detection: no `expr` field, but a structural
            // `type` field is present (sequence/choice/key/parse/etc.).
            const bool is_flat = (expr_it == item_dict->data().end())
                                 && item_dict->data().count("type")
                                 && !has_legacy_type;

            // Take the wrapper path when EITHER an `expr` field is
            // present (wrapper form) OR the item is a flat-form item.
            if ((expr_it != item_dict->data().end() && !has_legacy_type)
                || is_flat) {
                auto entries_r = extract_bindings(
                    dict_value(*item_dict, "bindings"));
                if (!entries_r) return tl::unexpected(entries_r.error());
                const auto& entries = *entries_r;
                // For wrapper form: expr is in the `expr` field.
                // For flat form:    expr IS the item dict itself
                //                   (populate() ignores `bindings`).
                ValuePtr expr_source = is_flat ? item : expr_it->second;
                if (!expr_source) {
                    return tl::unexpected("multi-binding: null expr");
                }

                bool set_var = false;
                std::string subparse_name;
                PpRole pp_role = PpRole::None;
                std::vector<std::pair<std::string, ValuePtr>> at_bindings;
                std::vector<std::pair<std::string, ValuePtr>> const_bindings;

                for (const auto& e : entries) {
                    // Empty-name = var sentinel from :=@ syntax.
                    if (e.name.empty()) {
                        set_var = true;
                        continue;
                    }
                    // Engine-reserved annotations (`:#name=...`). The
                    // `reserved` flag is set by the meta-grammar's
                    // RESERVED_VALUE_BIND rule. Dispatch on the bare
                    // name; reject unknown reserved names with a clear
                    // valid-values list.
                    if (e.reserved) {
                        if (e.name == "subparse") {
                            auto sv = std::dynamic_pointer_cast<StringValue>(e.value);
                            if (!sv) {
                                return tl::unexpected(
                                    "#subparse: value must be a string naming a rule");
                            }
                            subparse_name = sv->data();
                            continue;
                        }
                        if (e.name == "role") {
                            auto sv = std::dynamic_pointer_cast<StringValue>(e.value);
                            if (!sv) {
                                return tl::unexpected(
                                    "#role: value must be a string");
                            }
                            auto parsed = parse_pp_role(sv->data());
                            if (!parsed) {
                                return tl::unexpected(
                                    "#role: unknown role '" + sv->data() +
                                    "' (valid: define, undef, ifdef, ifndef, "
                                    "if, elsif, else, endif, include, "
                                    "macro_use, paste, stringify, text)");
                            }
                            pp_role = *parsed;
                            continue;
                        }
                        if (e.name == "field") {
                            // Field-name override; validated here, wired
                            // when the preprocessor walker lands.
                            auto sv = std::dynamic_pointer_cast<StringValue>(e.value);
                            if (!sv) {
                                return tl::unexpected(
                                    "#field: value must be a string");
                            }
                            continue;
                        }
                        return tl::unexpected(
                            "unknown engine annotation '#" + e.name +
                            "'; valid: #subparse, #role, #field");
                    }
                    // Catch unmigrated `:subparse="RULE"` (no `#` prefix)
                    // since 0.2.0 — the directive moved to the reserved
                    // namespace. Silent demotion to a value binding
                    // would break the runtime contract without a hint.
                    if (e.name == "subparse") {
                        return tl::unexpected(
                            "':subparse=' is no longer recognized — "
                            "use ':#subparse=' (engine-reserved namespace, since 0.2.0)");
                    }
                    // Value "@" means "bind to expr's parsed value".
                    if (auto sv = std::dynamic_pointer_cast<StringValue>(e.value)) {
                        if (sv->data() == "@") {
                            at_bindings.push_back({e.name, e.value});
                            continue;
                        }
                    }
                    const_bindings.push_back({e.name, e.value});
                }

                if (at_bindings.size() > 1) {
                    return tl::unexpected(
                        "multi-binding: at most one '@' binding per item "
                        "(would need multiple consumers to pair with)");
                }

                // Build the expr first so we can detect is_optional and
                // scope the bindings inside the same optional unit.
                auto expr_child = build_inline(g, *expr_source);
                if (!expr_child) return tl::unexpected(expr_child.error());
                if (set_var) g.set_name(*expr_child);
                // For wrapper form, pretty attrs live on the wrapper.
                // For flat form, populate() already applied them to the
                // expr node (since item dict IS the expr source).
                if (!is_flat) apply_pretty_attrs(g, *expr_child, *item_dict);
                // Stash the subparse target name; resolved after all
                // rules are loaded (the named rule may come later).
                if (!subparse_name.empty()) {
                    g.set_pending_subparse(*expr_child, subparse_name);
                }
                if (pp_role != PpRole::None) {
                    g.node(*expr_child).pp_role = pp_role;
                }

                // Conditional bindings: when the expr is optional AND
                // there are any bindings (at-binding `:name=@` or
                // const-binding `:name=val`), wrap the whole
                // [@-bindings, expr, const-bindings] block in a new
                // sequence and move is_optional onto the wrapper.
                //
                // BOTH binding kinds need the wrap when the surrounding
                // optional may skip:
                //
                //   * Const bindings emit unconditionally as siblings, so
                //     without the wrap they'd fire even when the optional
                //     rewinds. (Pre-existing reason for the wrap.)
                //
                //   * At-bindings leave a V-name marker in the parent's
                //     emitted_ buffer. If the optional is skipped, the
                //     V-name stays orphaned — the NEXT emitted value
                //     pairs with it, corrupting the AST. The previous
                //     "next field's V-name overwrites" assumption fails
                //     whenever the next sibling is a Key with `:@`
                //     (which emits a value before its own bindings'
                //     V-names) or any other emitter that produces a
                //     value without a preceding V-name. Symptom: a
                //     skipped `?<VIS>:visibility=@` paired with a
                //     following `'function':@:type="…"` produces
                //     `{visibility: "function"}` instead of leaving
                //     visibility absent.
                const bool wrap_optional =
                    g.node(*expr_child).is_optional
                    && (!at_bindings.empty() || !const_bindings.empty());
                // Choice target: a binding-wrapped item must be ONE
                // alternative (not split into V-name + expr siblings,
                // which the Choice would treat as separate alts). Wrap
                // in an unnamed Sequence so the bindings stay grouped
                // with their expr inside a single alt.
                const bool wrap_for_choice =
                    !wrap_optional
                    && g.node(target).kind == NodeKind::Choice
                    && (!at_bindings.empty() || !const_bindings.empty());
                // Repeat target: same reason. A `repeat <X>:foo[]=@`
                // would otherwise add Value-name + Ref as two children
                // of the Repeat node, but Repeat only iterates its
                // child[0] (the item) — extra children never fire,
                // so the binding silently never appends. Wrap so the
                // binding sticks with its expr inside a single
                // item-slot.
                const bool wrap_for_repeat =
                    !wrap_optional && !wrap_for_choice
                    && g.node(target).kind == NodeKind::Repeat
                    && (!at_bindings.empty() || !const_bindings.empty());
                NodeId append_to = target;
                NodeId wrapper_id;
                if (wrap_optional) {
                    wrapper_id = g.new_sequence();
                    g.set_optional(wrapper_id);
                    g.node(*expr_child).is_optional = false;
                    append_to = wrapper_id;
                } else if (wrap_for_choice || wrap_for_repeat) {
                    wrapper_id = g.new_sequence();
                    append_to = wrapper_id;
                }

                // (1) @-binding name marker BEFORE expr.
                for (auto& [name, _] : at_bindings) {
                    NodeId vn = g.new_value(make_string(name));
                    g.set_name(vn);
                    g.node(append_to).children.push_back(vn);
                }

                // (1a) Auto-emit for Keys: when a Key has at-bindings
                // (`'X':name=@`) but no explicit `emit: true` flag,
                // the binding's `@` reference would resolve to nothing
                // (Key without a Value-child emits no value), leaving
                // the V-name marker orphaned. Auto-add the Value-child
                // so the Key's literal text becomes the binding's
                // value — same effect as the `'X':@:name=@` form, just
                // without requiring the author to write the redundant
                // `:@`. Idempotent: if emit was already set, the
                // Value-child is already there and we don't add a
                // duplicate.
                if (!at_bindings.empty()) {
                    // Inspect expr's kind and (if Key) extract its key
                    // text BEFORE calling g.new_value() — `new_value`
                    // appends to `nodes_`, which may reallocate the
                    // backing vector and invalidate every `Node&`
                    // reference we held. After the allocation, re-
                    // fetch the node by NodeId (vector index) to push
                    // the new Value-child onto its children list.
                    NodeKind kind = g.node(*expr_child).kind;
                    ValuePtr key_value = g.node(*expr_child).value;
                    bool children_empty = g.node(*expr_child).children.empty();
                    if (kind == NodeKind::Key && key_value && children_empty) {
                        if (auto sv = std::dynamic_pointer_cast<StringValue>(key_value)) {
                            NodeId vc = g.new_value(make_string(sv->data()));
                            // Re-fetch through Grammar::node(id) AFTER
                            // new_value — same NodeId, but the backing
                            // storage may have moved.
                            g.node(*expr_child).children.push_back(vc);
                        }
                    }
                }

                // (2) The expr.
                g.node(append_to).children.push_back(*expr_child);

                // (3) Const bindings AFTER expr.
                for (auto& [name, value] : const_bindings) {
                    NodeId vn = g.new_value(make_string(name));
                    g.set_name(vn);
                    g.node(append_to).children.push_back(vn);
                    NodeId vc = g.new_value(value ? value : null_value());
                    g.node(append_to).children.push_back(vc);
                }

                if (wrap_optional || wrap_for_choice || wrap_for_repeat) {
                    g.node(target).children.push_back(wrapper_id);
                }

                continue;
            }

            auto type_it = item_dict->data().find("type");
            if (type_it != item_dict->data().end() &&
                expr_it != item_dict->data().end()) {
                auto type_sv = std::dynamic_pointer_cast<StringValue>(
                    type_it->second);
                if (type_sv) {
                    const std::string& t = type_sv->data();
                    if (t == "bare") {
                        if (!expr_it->second) {
                            return tl::unexpected("bare wrapper: null expr");
                        }
                        auto child_r = build_inline(g, *expr_it->second);
                        if (!child_r) return tl::unexpected(child_r.error());
                        apply_pretty_attrs(g, *child_r, *item_dict);
                        g.node(target).children.push_back(*child_r);
                        continue;
                    }
                    if (t == "var") {
                        if (!expr_it->second) {
                            return tl::unexpected("var wrapper: null expr");
                        }
                        auto child_r = build_inline(g, *expr_it->second);
                        if (!child_r) return tl::unexpected(child_r.error());
                        g.set_name(*child_r);
                        apply_pretty_attrs(g, *child_r, *item_dict);
                        g.node(target).children.push_back(*child_r);
                        continue;
                    }
                    if (t == "binding") {
                        auto name_it = item_dict->data().find("name");
                        if (name_it == item_dict->data().end()) {
                            return tl::unexpected("binding wrapper: missing 'name'");
                        }
                        auto name_sv = std::dynamic_pointer_cast<StringValue>(
                            name_it->second);
                        if (!name_sv) {
                            return tl::unexpected("binding wrapper: 'name' must be string");
                        }
                        if (!expr_it->second) {
                            return tl::unexpected("binding wrapper: null expr");
                        }
                        // (1) Value-kind name marker.
                        NodeId name_child = g.new_value(make_string(name_sv->data()));
                        g.set_name(name_child);
                        g.node(target).children.push_back(name_child);
                        // (2) the wrapped expression. Pretty-print attrs
                        // on the binding apply to the value side (not the
                        // name marker).
                        auto child_r = build_inline(g, *expr_it->second);
                        if (!child_r) return tl::unexpected(child_r.error());
                        apply_pretty_attrs(g, *child_r, *item_dict);
                        g.node(target).children.push_back(*child_r);
                        continue;
                    }
                    if (t == "binding_const") {
                        // EXPR:name=<literal> form. The EXPR is parsed
                        // (input is consumed, its side effect matters —
                        // matching a discriminator record etc.), but its
                        // produced value is discarded. The literal
                        // becomes the value-side of the (name, literal)
                        // pair emitted into the surrounding dict catcher.
                        auto name_it  = item_dict->data().find("name");
                        auto value_it = item_dict->data().find("value");
                        if (name_it == item_dict->data().end()) {
                            return tl::unexpected("binding_const: missing 'name'");
                        }
                        if (value_it == item_dict->data().end()) {
                            return tl::unexpected("binding_const: missing 'value'");
                        }
                        auto name_sv = std::dynamic_pointer_cast<StringValue>(
                            name_it->second);
                        if (!name_sv) {
                            return tl::unexpected("binding_const: 'name' must be string");
                        }
                        if (!value_it->second) {
                            return tl::unexpected("binding_const: null 'value'");
                        }
                        if (!expr_it->second) {
                            return tl::unexpected("binding_const: null expr");
                        }
                        // (1) Build the EXPR — input parsing still
                        // happens (discriminator record gets matched);
                        // the produced Value is dropped at the
                        // dict-catcher stage by the trailing constant.
                        auto child_r = build_inline(g, *expr_it->second);
                        if (!child_r) return tl::unexpected(child_r.error());
                        apply_pretty_attrs(g, *child_r, *item_dict);
                        g.node(target).children.push_back(*child_r);
                        // (2) Value-kind name marker.
                        NodeId name_child = g.new_value(make_string(name_sv->data()));
                        g.set_name(name_child);
                        g.node(target).children.push_back(name_child);
                        // (3) Value-kind constant value.
                        NodeId value_child = g.new_value(value_it->second);
                        g.node(target).children.push_back(value_child);
                        continue;
                    }
                    // Other type values ("sequence", "choice", "key", ...)
                    // fall through to build_inline below.
                }
            }
        }

        auto child_r = build_inline(g, *item);
        if (!child_r) return tl::unexpected(child_r.error());
        g.node(target).children.push_back(*child_r);
    }
    return {};
}

tl::expected<void, std::string>
populate(Grammar& g, NodeId target, const Value& body) {
    // String body -> Ref (if name registered) or Key (literal).
    if (auto sv = dynamic_cast<const StringValue*>(&body)) {
        Node& n = g.node(target);
        if (g.has_rule(sv->data())) {
            n.kind = NodeKind::Ref;
        } else {
            n.kind = NodeKind::Key;
        }
        n.value = make_string(sv->data());
        return {};
    }

    auto dv = dynamic_cast<const DictValue*>(&body);
    if (!dv) {
        return tl::unexpected("grammar body must be a string or object");
    }

    auto type_r = dict_string(*dv, "type");
    if (!type_r) return tl::unexpected(type_r.error());
    const std::string& type = *type_r;

    // Optional wrapper: transparently sets is_optional on the wrapped expr.
    // {"type":"optional", "expr": <inner-expr>}
    if (type == "optional") {
        auto inner_val = dict_value(*dv, "expr");
        if (!inner_val) {
            return tl::unexpected("optional: missing 'expr' field");
        }
        auto r = populate(g, target, *inner_val);
        if (!r) return tl::unexpected(r.error());
        g.set_optional(target);
        return {};
    }

    // Explicit Ref form: {"type":"ref", "name":"RULE_NAME"}
    if (type == "ref") {
        auto name_r = dict_string(*dv, "name");
        if (!name_r) return tl::unexpected(name_r.error());
        Node& n = g.node(target);
        n.kind = NodeKind::Ref;
        n.value = make_string(*name_r);
        if (dict_bool(*dv, "optional")) g.set_optional(target);
        if (dict_bool(*dv, "negative")) g.set_negative(target);
        return {};
    }

    Node& n = g.node(target);

    // Universal "optional" field applies to any node kind from here on.
    if (dict_bool(*dv, "optional")) {
        g.set_optional(target);
    }

    // Universal "negative" field — surface form `!X` in .rawast. The
    // engine inverts the inner's success/failure at this node (succeed
    // empty if inner fails, fail if inner succeeds) and consumes zero
    // bytes either way. See compute_node_first_bytes and the
    // is_negative branches in parse_from.
    if (dict_bool(*dv, "negative")) {
        g.set_negative(target);
    }

    // "backtrack" field — opt-in structural rewind, currently only
    // meaningful on Choice. Reading it on other kinds is harmless: the
    // flag is set on the Node but the driver only consults it for
    // NodeKind::Choice.
    if (dict_bool(*dv, "backtrack")) {
        g.set_backtrack(target);
    }

    // "fixed_schema" field — force a Sequence container=Dict to push
    // a dict scope (not flatten) so consumers in nested sub-rules can
    // do field-by-name lookups. Only meaningful on Sequence/Dict.
    if (dict_bool(*dv, "fixed_schema")) {
        g.set_fixed_schema(target);
    }

    // Pretty-print attrs (indent/tab/space/newline/tail) — universal,
    // apply to any node kind. Save-side only; parse ignores them.
    apply_pretty_attrs(g, target, *dv);

    // Rule-level engine-reserved bindings. Process `:#role="..."` and
    // `:#field="..."` annotations attached to a rule body itself
    // (as opposed to bindings on a child item, which append_items_array
    // handles). Non-reserved bindings at rule level are ignored as
    // before — they don't have well-defined semantics on a rule body
    // (the wrapping pattern used to emit name markers only applies
    // around expressions inside sequences).
    if (auto rb_bindings = dict_value(*dv, "bindings")) {
        auto entries_r = extract_bindings(rb_bindings);
        if (!entries_r) return tl::unexpected(entries_r.error());
        for (const auto& e : *entries_r) {
            if (!e.reserved) continue;
            if (e.name == "role") {
                auto sv = std::dynamic_pointer_cast<StringValue>(e.value);
                if (!sv) {
                    return tl::unexpected(
                        "#role: value must be a string");
                }
                auto parsed = parse_pp_role(sv->data());
                if (!parsed) {
                    return tl::unexpected(
                        "#role: unknown role '" + sv->data() +
                        "' (valid: define, undef, ifdef, ifndef, if, "
                        "elsif, else, endif, include, macro_use, paste, "
                        "stringify, text)");
                }
                g.node(target).pp_role = *parsed;
                continue;
            }
            if (e.name == "field") {
                auto sv = std::dynamic_pointer_cast<StringValue>(e.value);
                if (!sv) {
                    return tl::unexpected(
                        "#field: value must be a string");
                }
                // Wired when the preprocessor walker lands.
                continue;
            }
            if (e.name == "subparse") {
                // #subparse is meaningful on a Parse-kind child binding
                // (a Parse terminal inside a sequence/rule body). For
                // flat-form items, build_inline calls populate() with
                // the item dict itself — the item's bindings end up
                // here even though append_items_array already wired
                // #subparse at the item level. Skip silently: the
                // binding has already been handled at the item layer
                // and reaching this code path means we're inside a
                // flat-form item, not at a true rule body. The
                // alternative (erroring here) breaks every grammar
                // that uses `<parser>:value=@:#subparse="RULE"` —
                // including tcl.rawast and any future grammar built
                // on the same pattern.
                continue;
            }
            if (e.name == "opchain") {
                // Bare flag — no value expected. Marks the rule body
                // for parse-side always-wrap → {op, args[]} compaction
                // (and save-side reversal). See Grammar::set_opchain.
                if (e.value) {
                    return tl::unexpected(
                        "#opchain: takes no value (use bare `:#opchain`)");
                }
                g.set_opchain(target);
                continue;
            }
            return tl::unexpected(
                "unknown engine annotation '#" + e.name +
                "' on rule body; valid: #role, #field, #opchain");
        }
    }

    if (type == "sequence" || type == "choice" || type == "repeat") {
        if (type == "sequence") n.kind = NodeKind::Sequence;
        else if (type == "choice") n.kind = NodeKind::Choice;
        else                      n.kind = NodeKind::Repeat;

        if (auto c = dict_string_opt(*dv, "container")) {
            if      (*c == "array") n.container = Container::Array;
            else if (*c == "dict")  n.container = Container::Dict;
            else if (*c == "none")  n.container = Container::None;
            else return tl::unexpected("unknown container: '" + *c + "'");
        }
    }

    if (type == "sequence" || type == "choice") {
        // Accept the canonical `value` field (the field name the .rawast
        // meta-grammar emits) alongside the legacy `items` JSON shape.
        auto items_val = dict_value(*dv, "value");
        if (!items_val) items_val = dict_value(*dv, "items");
        return append_items_array(g, target, items_val, type);
    }

    if (type == "scope") {
        // `scope [array] { INNER... }`. Children are exactly the INNERs.
        // `container:"array"` selects segment-array output (each INNER
        // match → its typed value, text gaps → StringValue entries);
        // the default (no container) emits a single concatenated
        // StringValue body. The stop literal is resolved later by
        // `resolve_raw_stops` from the next sibling in the surrounding
        // sequence — scope has no start/stop attributes of its own.
        n.kind = NodeKind::Scope;
        if (auto c = dict_string_opt(*dv, "container")) {
            if      (*c == "array") n.container = Container::Array;
            else if (*c == "none")  n.container = Container::None;
            else return tl::unexpected(
                "scope: container must be 'array' (got '" + *c + "')");
        }
        auto items_val = dict_value(*dv, "value");
        if (!items_val) items_val = dict_value(*dv, "items");
        return append_items_array(g, target, items_val, type);
    }

    if (type == "repeat") {
        if (auto sep_val = dict_value(*dv, "separator")) {
            auto sep_r = build_item(g, *sep_val);
            if (!sep_r) return tl::unexpected(sep_r.error());
            g.set_separator(target, *sep_r);
        }
        // `min`: zero-or-more (default 0) vs one-or-more (`repeat+` → 1).
        if (auto mv = dict_value(*dv, "min")) {
            if (auto iv = std::dynamic_pointer_cast<IntValue>(mv)) {
                if (iv->data() < 0) return tl::unexpected("repeat: 'min' must be non-negative");
                g.set_min(target, static_cast<std::uint32_t>(iv->data()));
            } else {
                return tl::unexpected("repeat: 'min' must be an integer");
            }
        }
        // Accept `value` (canonical) alongside legacy `item` for the
        // repeat's body.
        auto item_val = dict_value(*dv, "value");
        if (!item_val) item_val = dict_value(*dv, "item");
        if (!item_val) return tl::unexpected("repeat: missing 'item'/'value'");
        auto item_r = build_item(g, *item_val);
        if (!item_r) return tl::unexpected(item_r.error());
        g.node(target).children.push_back(*item_r);
        return {};
    }

    if (type == "raw") {
        // `*` in a sequence body. The stop literal is the immediate
        // next sibling in the surrounding Sequence; it's resolved by
        // a post-load pass (see resolve_raw_stops) once all siblings
        // have been built. Leave `n.value` empty here.
        n.kind = NodeKind::Raw;
        return {};
    }

    if (type == "key" || type == "strict_key") {
        n.kind = NodeKind::Key;
        auto key_r = dict_string(*dv, "key");
        if (!key_r) return tl::unexpected(key_r.error());
        n.value = make_string(*key_r);
        // Word-boundary strict matching. The DSL surface forms `"X"` and
        // `'X'` arrive at the loader as `type: "key"` vs `type: "strict_key"`
        // respectively — both produce a Key node, with the strict_key
        // variant additionally setting `Node.strict = true`. JSON-form
        // grammars can also set `"strict": true` directly on a `"key"`
        // node, equivalent to using `'X'` in the DSL.
        n.strict = (type == "strict_key") || dict_bool(*dv, "strict");
        // `"X":@` shorthand — `emit: true` flag means "use the key's own
        // text as the emitted value." Equivalent at runtime to
        // `{key: "X", value: "X"}`.
        if (dict_bool(*dv, "emit")) {
            NodeId val_child = g.new_value(make_string(*key_r));
            g.node(target).children.push_back(val_child);
        } else if (auto val = dict_value(*dv, "value")) {
            NodeId val_child = g.new_value(val);
            g.node(target).children.push_back(val_child);
        }
        return {};
    }

    if (type == "value") {
        // Constant value emission. Used by the .rawast `name=@` desugar
        // to inject fixed dict-key names into a sequence's catcher.
        // With "var": true, the constant is emitted as a name marker.
        n.kind = NodeKind::Value;
        if (auto val = dict_value(*dv, "value")) {
            n.value = val;
        } else {
            return tl::unexpected("value: missing 'value' field");
        }
        if (dict_bool(*dv, "var")) {
            g.set_name(target);
        }
        return {};
    }

    // Ref-to-rule (if the name is a registered rule) or parser-name
    // reference (otherwise). Rules are registered in pass 1 of
    // load_json_grammar_into before any bodies are populated, so
    // has_rule() works at this point regardless of declaration order.
    if (g.has_rule(type)) {
        n.kind = NodeKind::Ref;
        n.value = make_string(type);
        return {};
    }
    n.kind  = NodeKind::Parse;
    // Parser reference: `type` carries either the bare name (`header`)
    // or the group-qualified form (`gdsii.header`) — the meta-grammar's
    // PARSE_EXPR parses both via `qualified_identifier`. The parser
    // registry registers each group member under both bare and dotted
    // aliases, so either form resolves directly.
    n.value = make_string(type);
    if (dict_bool(*dv, "var")) {
        g.set_name(target);
    }
    return {};
}

// Lazy-built JSONC meta-grammar reused across loader calls. The
// in-memory JSON grammar (make_json_grammar) is itself JSONC, so this
// wrapper is just a cached instance used to parse JSON-form grammar
// files (which may carry // and /* */ comments as inline docs).
const Grammar& json_meta_grammar() {
    static const Grammar g = make_json_grammar();
    return g;
}

} // namespace

tl::expected<void, std::string>
load_json_grammar_into(Grammar& g, const Value& tree) {
    auto root = dynamic_cast<const DictValue*>(&tree);
    if (!root) return tl::unexpected("top-level grammar must be a dict");

    // Apply `use:` directives first. The grammar's terminal-parser
    // references (gds_header, int, identifier, ...) need to be
    // resolvable at parse time; registering the parser groups up front
    // means a `use: gdsii` declaration alone is sufficient — no host
    // C++ pre-registration required.
    if (auto use_it = root->data().find("use"); use_it != root->data().end()) {
        auto use_arr = std::dynamic_pointer_cast<ArrayValue>(use_it->second);
        if (!use_arr) {
            return tl::unexpected("'use' field must be an array of group names");
        }
        for (const auto& entry : use_arr->data()) {
            auto sv = std::dynamic_pointer_cast<StringValue>(entry);
            if (!sv) {
                return tl::unexpected("'use' entries must be identifiers");
            }
            auto r = apply_parser_group(g, sv->data());
            if (!r) return tl::unexpected("use: " + r.error());
        }
    }

    // Pass 1: allocate one Node per named rule and register the name. The
    // Node's kind/value/children are filled in pass 2; for now they're
    // placeholders so that build_inline() / populate() can correctly
    // disambiguate bare-string Ref vs. Key by checking has_rule().
    for (const auto& [name, body] : root->data()) {
        if (name == "start" || name == "use") continue;
        NodeId id = g.new_sequence();   // placeholder
        g.register_rule(name, id);
    }

    // Per-rule ignore overrides. A rule body's `rule_ignore` field is an
    // array of parser names that the parse driver pushes as the active
    // ignore set when it enters that rule. Grammar-level "default
    // ignore" is achieved by attaching `rule_ignore` to the start rule —
    // the override propagates to all callees until something pushes
    // its own.
    for (const auto& [name, body] : root->data()) {
        if (name == "start" || name == "use") continue;
        auto body_dict = std::dynamic_pointer_cast<DictValue>(body);
        if (!body_dict) continue;
        auto ri_it = body_dict->data().find("rule_ignore");
        if (ri_it == body_dict->data().end()) continue;
        auto ri_arr = std::dynamic_pointer_cast<ArrayValue>(ri_it->second);
        if (!ri_arr) {
            return tl::unexpected("rule '" + name +
                                  "': rule_ignore must be an array of parser names");
        }
        std::vector<std::string> parsers;
        parsers.reserve(ri_arr->data().size());
        for (const auto& entry : ri_arr->data()) {
            auto sv = std::dynamic_pointer_cast<StringValue>(entry);
            if (!sv) {
                return tl::unexpected("rule '" + name +
                                      "': rule_ignore entries must be parser names");
            }
            parsers.push_back(sv->data());
        }
        g.add_rule_ignore(name, std::move(parsers));
    }

    // Pass 2: populate each named rule's Node from its body.
    for (const auto& [name, body] : root->data()) {
        if (name == "start" || name == "use") continue;
        if (!body) {
            return tl::unexpected("rule '" + name + "' has null body");
        }
        NodeId placeholder = g.rule_id(name);
        if (!placeholder.valid()) {
            return tl::unexpected("internal: rule '" + name +
                                  "' not registered after pass 1");
        }
        auto r = populate(g, placeholder, *body);
        if (!r) {
            return tl::unexpected("rule '" + name + "': " + r.error());
        }
    }

    // Set top entry point. Accepts two shapes:
    //   "start": "RULE_NAME"               (JSON-form: plain string)
    //   "start": {"type": "RULE_NAME"}     (.rawast meta-grammar emission:
    //                                       a flat ref dict)
    auto start_it = root->data().find("start");
    if (start_it == root->data().end()) {
        return tl::unexpected("grammar must define 'start'");
    }
    std::string start_name;
    if (auto sv = std::dynamic_pointer_cast<StringValue>(start_it->second)) {
        start_name = sv->data();
    } else if (auto dv = std::dynamic_pointer_cast<DictValue>(start_it->second)) {
        auto type_r = dict_string(*dv, "type");
        if (!type_r) {
            return tl::unexpected("'start' dict must have 'type' naming a rule");
        }
        start_name = *type_r;
    } else {
        return tl::unexpected("'start' must be a rule name string or {\"type\": <name>} dict");
    }
    if (!g.has_rule(start_name)) {
        return tl::unexpected("'start' references undefined rule '" +
                              start_name + "'");
    }
    g.set_top(g.new_ref(start_name));

    // Resolve any pending `:subparse="RULE"` references collected while
    // walking items — the rule may have been defined later than the
    // referring item, so resolution waits until all rules are
    // registered.
    auto sub_r = g.resolve_subparse_refs();
    if (!sub_r) return tl::unexpected(sub_r.error());

    // Resolve stop literals for every Raw and Scope node by walking
    // every Sequence's children and copying the next sibling's Key
    // literal onto the byte-scan node's `value`. Errors out if the
    // next sibling isn't a Key (or doesn't exist) — the byte-scan
    // engine has no way to know when to stop scanning otherwise.
    //
    // Scope's stop is sibling-driven (same as Raw): the surrounding
    // sequence describes the close delimiter as a Key sibling
    // immediately after the scope item. This unifies scope and Raw
    // structurally — scope is just Raw with INNERs and an optional
    // array container.
    auto kind_label = [](NodeKind k) {
        return k == NodeKind::Raw ? "raw consume (`*`)" : "scope";
    };
    for (std::size_t idx = 0; idx < g.node_count(); ++idx) {
        NodeId nid{idx};
        Node& parent = g.node(nid);
        if (parent.kind != NodeKind::Sequence) continue;
        const auto& kids = parent.children;
        for (std::size_t k = 0; k < kids.size(); ++k) {
            Node& child = g.node(g.resolve_ref(kids[k]));
            if (child.kind != NodeKind::Raw
                && child.kind != NodeKind::Scope) continue;
            const char* label = kind_label(child.kind);
            if (k + 1 >= kids.size()) {
                return tl::unexpected(
                    std::string(label) + " must be followed by a literal "
                    "key in the same sequence; nothing follows here");
            }
            const Node& next = g.node(g.resolve_ref(kids[k + 1]));
            if (next.kind != NodeKind::Key) {
                return tl::unexpected(
                    std::string(label) + " must be followed by a literal "
                    "key in the same sequence; next sibling is not a "
                    "Key node");
            }
            auto stop_sv = std::dynamic_pointer_cast<StringValue>(next.value);
            if (!stop_sv || stop_sv->data().empty()) {
                return tl::unexpected(
                    std::string(label) + " next-sibling Key has no literal "
                    "or empty literal");
            }
            child.value = stop_sv;
        }
    }

    // Validate that every Parse-node references a registered parser.
    // Without this, an unresolved name (typo, wrong group, std parser
    // referenced before the `use:` declaration) is accepted by the
    // loader and only blows up at parse time with a null deref on the
    // parser pointer — a hostile failure mode for grammar authors,
    // since the diagnostic is "segfault" with no context. By the time
    // we reach this point, `use:` groups have been applied and all
    // host-side `register_parser()` calls that ran before load have
    // landed; any remaining unresolved name is a real bug in the
    // grammar text.
    for (std::size_t idx = 0; idx < g.node_count(); ++idx) {
        NodeId nid{idx};
        const Node& n = g.node(nid);
        if (n.kind != NodeKind::Parse) continue;
        auto sv = std::dynamic_pointer_cast<StringValue>(n.value);
        if (!sv) continue;            // shouldn't happen — Parse always has a name
        const std::string& name = sv->data();
        if (g.parser(name) != nullptr) continue;
        return tl::unexpected(
            "unknown parser '" + name + "' referenced by grammar; "
            "either register it on the Grammar before load, or add the "
            "appropriate `use: <group>` line if it comes from a parser "
            "group (e.g. `use: std` for `int` / `identifier` / etc.)");
    }

    return {};
}

tl::expected<void, std::string>
load_json_grammar_from_string(Grammar& g, std::string_view content) {
    std::istringstream is{std::string{content}};
    StreamReader sr{is};
    auto parsed = json_meta_grammar().parse(sr);
    if (!parsed) {
        return tl::unexpected(
            "failed to parse JSON-grammar text at byte " +
            std::to_string(parsed.error().position.bytes) +
            ", line " + std::to_string(parsed.error().position.line) +
            ", column " + std::to_string(parsed.error().position.column) +
            ": " + parsed.error().message);
    }
    if (!*parsed) return tl::unexpected("JSON-grammar text produced null tree");
    return load_json_grammar_into(g, **parsed);
}

tl::expected<void, std::string>
load_json_grammar_from_file(Grammar& g, const std::string& path) {
    std::ifstream fs{path};
    if (!fs) {
        return tl::unexpected("cannot open grammar file: " + path);
    }
    std::ostringstream buf;
    buf << fs.rdbuf();
    return load_json_grammar_from_string(g, buf.str());
}

// -------------------------------------------------------------------------
// .rawast meta-grammar (lazy-loaded from grammars/rawast.json) and
// public end-to-end loaders that go .rawast text -> Grammar in one call.
// -------------------------------------------------------------------------

namespace {

const tl::expected<Grammar, std::string>& rawast_meta_grammar() {
    static const auto cached = []() -> tl::expected<Grammar, std::string> {
        // The grammar file itself declares `"use": ["std"]` and the
        // ignore list it needs; the loader brings those in via the
        // `use:` directive. Just ensure the std group is registered
        // globally so the use directive can resolve it.
        register_std_parser_group();
        Grammar g;
        auto r = load_json_grammar_from_file(g, "grammars/rawast.json");
        if (!r) {
            return tl::unexpected("rawast meta-grammar load: " + r.error());
        }
        return g;
    }();
    return cached;
}

} // namespace

tl::expected<void, std::string>
load_rawast_grammar_from_string(Grammar& g, std::string_view content) {
    const auto& meta = rawast_meta_grammar();
    if (!meta) return tl::unexpected(meta.error());

    std::istringstream is{std::string{content}};
    StreamReader sr{is};
    auto parsed = meta->parse(sr);
    if (!parsed) {
        return tl::unexpected(
            "failed to parse .rawast text at byte " +
            std::to_string(parsed.error().position.bytes) +
            ", line " + std::to_string(parsed.error().position.line) +
            ", column " + std::to_string(parsed.error().position.column) +
            ": " + parsed.error().message);
    }
    if (!*parsed) return tl::unexpected(".rawast parse produced null tree");
    return load_json_grammar_into(g, **parsed);
}

tl::expected<void, std::string>
load_rawast_grammar_from_file(Grammar& g, const std::string& path) {
    std::ifstream fs{path};
    if (!fs) {
        return tl::unexpected("cannot open .rawast file: " + path);
    }
    std::ostringstream buf;
    buf << fs.rdbuf();
    return load_rawast_grammar_from_string(g, buf.str());
}

} // namespace rawast
