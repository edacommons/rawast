#include <doctest/doctest.h>

#include <rawast/version.hpp>

#include <string_view>

TEST_CASE("version header constants are present and well-formed") {
    CHECK(rawast::VERSION_MAJOR == 0);
    CHECK(rawast::VERSION_MINOR == 1);
    CHECK(rawast::VERSION_PATCH == 0);

    std::string_view v{rawast::VERSION};
    CHECK_FALSE(v.empty());
    CHECK(v == "0.1.0");
}
