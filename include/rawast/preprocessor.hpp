#pragma once

#include <rawast/node.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace rawast {

// String-form name for a PpRole enumerator. Matches the lowercase
// identifier used in `:#role="..."` grammar bindings. PpRole::None
// returns an empty string (it has no surface form). Total — covers
// every enumerator.
std::string_view to_string(PpRole role) noexcept;

// Parse a string into a PpRole. Accepts the lowercase identifiers
// listed in node.hpp's PpRole declaration. Returns std::nullopt for
// any unknown name; the loader maps that to a clear user error.
std::optional<PpRole> parse_pp_role(std::string_view name) noexcept;

} // namespace rawast
