#include <rawast/parsers_registry.hpp>
#include <rawast/grammar.hpp>

#include <map>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace rawast {

namespace {

// Each registered group is either a structured ParserGroup
// (enumerable, supports dotted naming) or a legacy registration
// function (opaque, kept for back-compat).
using RegistryEntry = std::variant<ParserGroup, ParserGroupRegisterFn>;

// Meyers singleton — the registry map is constructed on first access,
// so static-initialisation order between this TU and any caller's
// static AutoRegister struct doesn't matter.
std::map<std::string, RegistryEntry>& registry() {
    static std::map<std::string, RegistryEntry> m;
    return m;
}

// Apply a structured group to a Grammar: each parser is instantiated
// twice and registered under both bare and dotted keys. Terminal
// parsers are stateless across `parse()` calls (their state lives in
// the StreamReader), so two instances behave identically. The
// callback-driven `replace_parser` machinery operates on whichever
// key the rule referenced.
void apply_structured(Grammar& g, const ParserGroup& group) {
    for (const ParserSpec& spec : group.parsers) {
        const std::string dotted = group.name + "." + spec.local_name;
        // Bare key: register via the parser's own ::name() (uses the
        // existing register_parser path so name-keyed callers keep
        // working).
        g.register_parser(spec.factory());
        // Dotted key: register the second instance under the explicit
        // alias key.
        g.register_parser_alias(dotted, spec.factory());
    }
}

} // namespace

void register_parser_group(ParserGroup group) {
    std::string key = group.name;
    registry().insert_or_assign(std::move(key), std::move(group));
}

void register_parser_group(std::string name, ParserGroupRegisterFn fn) {
    registry().insert_or_assign(std::move(name), std::move(fn));
}

bool parser_group_exists(const std::string& name) {
    return registry().count(name) > 0;
}

tl::expected<void, std::string>
apply_parser_group(Grammar& g, const std::string& name) {
    auto it = registry().find(name);
    if (it == registry().end()) {
        return tl::unexpected("parser group '" + name + "' not registered");
    }
    if (auto* group = std::get_if<ParserGroup>(&it->second)) {
        apply_structured(g, *group);
    } else if (auto* fn = std::get_if<ParserGroupRegisterFn>(&it->second)) {
        (*fn)(g);
    }
    return {};
}

std::vector<std::string> registered_parser_groups() {
    std::vector<std::string> names;
    names.reserve(registry().size());
    for (const auto& [n, _] : registry()) names.push_back(n);
    return names;   // std::map iteration is already sorted
}

std::vector<std::string>
parser_group_local_names(const std::string& group_name) {
    auto it = registry().find(group_name);
    if (it == registry().end()) return {};
    auto* group = std::get_if<ParserGroup>(&it->second);
    if (!group) return {};   // legacy entry — opaque
    std::vector<std::string> out;
    out.reserve(group->parsers.size());
    for (const auto& p : group->parsers) out.push_back(p.local_name);
    return out;
}

} // namespace rawast
