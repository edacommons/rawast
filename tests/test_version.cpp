#include <doctest/doctest.h>

#include <rawast/version.hpp>

#include <string>
#include <string_view>

TEST_CASE("version header constants are present and well-formed") {
    CHECK(rawast::VERSION_MAJOR == 0);
    CHECK(rawast::VERSION_MINOR == 1);
    CHECK(rawast::VERSION_PATCH == 0);

    // VERSION is the full PEP 440-style identifier, which may carry
    // a pre-release suffix (`aN` / `bN` / `rcN`). It must always
    // *start* with `<MAJOR>.<MINOR>.<PATCH>` from the numeric
    // constants above — that's the part the test pins. The optional
    // suffix lets us bump the version string without churning this
    // test on every release.
    std::string_view v{rawast::VERSION};
    CHECK_FALSE(v.empty());

    const std::string numeric_prefix =
        std::to_string(rawast::VERSION_MAJOR) + "." +
        std::to_string(rawast::VERSION_MINOR) + "." +
        std::to_string(rawast::VERSION_PATCH);
    CHECK(v.substr(0, numeric_prefix.size()) == numeric_prefix);
}
