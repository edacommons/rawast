#pragma once

#include <string_view>

namespace rawast {

inline constexpr int VERSION_MAJOR = 0;
inline constexpr int VERSION_MINOR = 1;
inline constexpr int VERSION_PATCH = 1;

// Matches the Python package version in pyproject.toml. The
// numeric MAJOR/MINOR/PATCH constants above intentionally don't
// carry the PEP 440 pre-release suffix (`a1`); they're for C++
// callers that want to compare versions numerically and don't
// model alpha/beta/rc tiers. Use the VERSION string when you
// need the full identifier.
inline constexpr std::string_view VERSION = "0.1.1";

} // namespace rawast
