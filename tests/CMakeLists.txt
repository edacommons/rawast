include(FetchContent)

FetchContent_Declare(doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG v2.4.11
    GIT_SHALLOW ON
)
set(DOCTEST_WITH_TESTS OFF CACHE BOOL "" FORCE)
set(DOCTEST_WITH_MAIN_IN_STATIC_LIB OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(doctest)

add_executable(rawast_tests
    test_main.cpp
    test_version.cpp
    test_value.cpp
    test_node.cpp
    test_stream.cpp
    test_parsers.cpp
    test_grammar.cpp
    test_json.cpp
    test_pool.cpp
    test_save.cpp
    test_loader.cpp
    test_parsers_sv.cpp
    test_to_value.cpp
    test_linter.cpp
    test_callbacks.cpp
    test_pretty.cpp
    test_gdsii.cpp
    test_preprocessor.cpp
    test_neg_lookahead.cpp
    test_scope_parsers.cpp
)

set_target_properties(rawast_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

target_link_libraries(rawast_tests
    PRIVATE
        rawast::rawast
        doctest::doctest
)

target_compile_features(rawast_tests PRIVATE cxx_std_20)

# Tests reference grammar files via relative path (e.g. "grammars/json.json"),
# so the working directory must be the project root. Pass it through
# doctest_discover_tests rather than a post-hoc set_tests_properties on
# ${doctest_tests} — that variable isn't populated until the helper runs.
include(${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake)
doctest_discover_tests(rawast_tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
