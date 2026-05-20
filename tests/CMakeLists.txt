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
    test_linter.cpp
    test_callbacks.cpp
    test_pretty.cpp
)

# Tests reference grammar files via relative path from the test working
# directory; set the working directory so "grammars/json.json" resolves
# from the project root regardless of where ctest is invoked from.
set_target_properties(rawast_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)
set_tests_properties(${doctest_tests} PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

target_link_libraries(rawast_tests
    PRIVATE
        rawast::rawast
        doctest::doctest
)

target_compile_features(rawast_tests PRIVATE cxx_std_20)

include(${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake)
doctest_discover_tests(rawast_tests)
