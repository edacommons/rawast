#include <rawast/parsers_registry.hpp>
#include <rawast/grammar.hpp>

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace rawast {

namespace {

// Meyers singleton — the registry map is constructed on first access,
// so static-initialisation order between this TU and any caller's
// static AutoRegister struct doesn't matter.
std::map<std::string, ParserGroupRegisterFn>& registry() {
    static std::map<std::string, ParserGroupRegisterFn> m;
    return m;
}

} // namespace

void register_parser_group(std::string name, ParserGroupRegisterFn fn) {
    registry()[std::move(name)] = std::move(fn);
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
    it->second(g);
    return {};
}

std::vector<std::string> registered_parser_groups() {
    std::vector<std::string> names;
    names.reserve(registry().size());
    for (const auto& [n, _] : registry()) names.push_back(n);
    return names;   // std::map iteration is already sorted
}

} // namespace rawast
