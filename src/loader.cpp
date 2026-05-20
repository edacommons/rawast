#include <rawast/loader.hpp>
#include <rawast/parsers.hpp>

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
        return tl::unexpected("missing field '" + key + "'");
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
        auto type_it = dv->data().find("type");
        auto expr_it = dv->data().find("expr");
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

        // Detect binding-wrapper shape.
        if (auto item_dict = std::dynamic_pointer_cast<DictValue>(item)) {
            auto type_it = item_dict->data().find("type");
            auto expr_it = item_dict->data().find("expr");
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
        return {};
    }

    Node& n = g.node(target);

    // Universal "optional" field applies to any node kind from here on.
    if (dict_bool(*dv, "optional")) {
        g.set_optional(target);
    }

    // "backtrack" field — opt-in structural rewind, currently only
    // meaningful on Choice. Reading it on other kinds is harmless: the
    // flag is set on the Node but the driver only consults it for
    // NodeKind::Choice.
    if (dict_bool(*dv, "backtrack")) {
        g.set_backtrack(target);
    }

    // Pretty-print attrs (indent/tab/space/newline/tail) — universal,
    // apply to any node kind. Save-side only; parse ignores them.
    apply_pretty_attrs(g, target, *dv);

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
        return append_items_array(g, target, dict_value(*dv, "items"), type);
    }

    if (type == "repeat") {
        if (auto sep_val = dict_value(*dv, "separator")) {
            auto sep_r = build_item(g, *sep_val);
            if (!sep_r) return tl::unexpected(sep_r.error());
            g.set_separator(target, *sep_r);
        }
        auto item_val = dict_value(*dv, "item");
        if (!item_val) return tl::unexpected("repeat: missing 'item'");
        auto item_r = build_item(g, *item_val);
        if (!item_r) return tl::unexpected(item_r.error());
        g.node(target).children.push_back(*item_r);
        return {};
    }

    if (type == "key") {
        n.kind = NodeKind::Key;
        auto key_r = dict_string(*dv, "key");
        if (!key_r) return tl::unexpected(key_r.error());
        n.value = make_string(*key_r);
        if (auto val = dict_value(*dv, "value")) {
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

    // Otherwise: it's a parser-name reference.
    n.kind  = NodeKind::Parse;
    n.value = make_string(type);
    if (dict_bool(*dv, "var")) {
        g.set_name(target);
    }
    return {};
}

// Lazy-built JSON+comments meta-grammar reused across loader calls.
// JSONC tolerance (// line and /* block */ comments in the ignore list)
// lets community grammar files in JSON form carry inline documentation.
const Grammar& json_meta_grammar() {
    static const Grammar g = [] {
        Grammar base = make_json_grammar();
        base.register_parser(std::make_unique<LineCommentParser>());
        base.register_parser(std::make_unique<BlockCommentParser>());
        base.add_ignore("line_comment");
        base.add_ignore("block_comment");
        return base;
    }();
    return g;
}

} // namespace

tl::expected<void, std::string>
load_json_grammar_into(Grammar& g, const Value& tree) {
    auto root = dynamic_cast<const DictValue*>(&tree);
    if (!root) return tl::unexpected("top-level grammar must be a dict");

    // Pass 1: allocate one Node per named rule and register the name. The
    // Node's kind/value/children are filled in pass 2; for now they're
    // placeholders so that build_inline() / populate() can correctly
    // disambiguate bare-string Ref vs. Key by checking has_rule().
    for (const auto& [name, body] : root->data()) {
        if (name == "start") continue;
        NodeId id = g.new_sequence();   // placeholder
        g.register_rule(name, id);
    }

    // Pass 2: populate each named rule's Node from its body.
    for (const auto& [name, body] : root->data()) {
        if (name == "start") continue;
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

    // Set top entry point.
    auto start_it = root->data().find("start");
    if (start_it == root->data().end()) {
        return tl::unexpected("grammar must define 'start'");
    }
    auto start_sv = std::dynamic_pointer_cast<StringValue>(start_it->second);
    if (!start_sv) {
        return tl::unexpected("'start' must be a string naming a rule");
    }
    if (!g.has_rule(start_sv->data())) {
        return tl::unexpected("'start' references undefined rule '" +
                              start_sv->data() + "'");
    }
    g.set_top(g.new_ref(start_sv->data()));

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
        Grammar g;
        g.register_parser(std::make_unique<DoubleQuoteStringParser>());
        g.register_parser(std::make_unique<IdentifierParser>());
        g.register_parser(std::make_unique<WhitespaceParser>());
        g.register_parser(std::make_unique<LineCommentParser>());
        g.register_parser(std::make_unique<BlockCommentParser>());
        g.add_ignore("whitespace");
        g.add_ignore("line_comment");
        g.add_ignore("block_comment");
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
