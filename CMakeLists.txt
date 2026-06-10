cmake_minimum_required(VERSION 3.20)

project(rawast
    # CMake's project(VERSION ...) only accepts numeric components
    # (up to 4 dot-separated integers), so it can't carry PEP 440
    # pre-release suffixes. The actual user-visible version lives in
    # pyproject.toml (Python wheel) and include/rawast/version.hpp
    # (C++ string `rawast::VERSION`); both may carry an `aN` / `bN`
    # / `rcN` tier the CMake VERSION here cannot.
    VERSION 0.1.0
    DESCRIPTION "Data-driven predictive PEG parser engine and .jast container format"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Default build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        Debug Release RelWithDebInfo MinSizeRel)
endif()

option(RAWAST_BUILD_TESTS "Build tests" ON)
option(RAWAST_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

# Some upstream dependencies (doctest, tl::expected) still declare very old
# cmake_minimum_required values that modern CMake refuses outright. Tell CMake
# to apply at least the 3.5 policy set when configuring those sub-projects.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

include(FetchContent)

# tl::expected — header-only error type.
FetchContent_Declare(tl-expected
    GIT_REPOSITORY https://github.com/TartanLlama/expected.git
    GIT_TAG v1.1.0
    GIT_SHALLOW ON
)
set(EXPECTED_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(tl-expected)

# Library target.
add_library(rawast STATIC
    src/version.cpp
    src/value.cpp
    src/node.cpp
    src/stream.cpp
    src/parsers.cpp
    src/pool.cpp
    src/frame.cpp
    src/grammar.cpp
    src/save_stack.cpp
    src/loader.cpp
    src/to_value.cpp
    src/linter.cpp
    src/profile.cpp
    src/parsers_gdsii.cpp
    src/parsers_lefdef.cpp
    src/parsers_tcl.cpp
    src/parsers_sv.cpp
    src/parsers_registry.cpp
)
add_library(rawast::rawast ALIAS rawast)

target_include_directories(rawast
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(rawast PUBLIC tl::expected)

target_compile_features(rawast PUBLIC cxx_std_17)

if(RAWAST_WARNINGS_AS_ERRORS)
    if(MSVC)
        target_compile_options(rawast PRIVATE /W4 /WX)
    else()
        target_compile_options(rawast PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
else()
    if(MSVC)
        target_compile_options(rawast PRIVATE /W4)
    else()
        target_compile_options(rawast PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()

if(RAWAST_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# -----------------------------------------------------------------------
# Python binding (built only when invoked via scikit-build-core).
# -----------------------------------------------------------------------
if(SKBUILD)
    set_property(TARGET rawast PROPERTY POSITION_INDEPENDENT_CODE ON)

    find_package(Python REQUIRED COMPONENTS Interpreter Development.Module)

    FetchContent_Declare(nanobind
        GIT_REPOSITORY https://github.com/wjakob/nanobind.git
        GIT_TAG v2.4.0
        GIT_SHALLOW ON
    )
    FetchContent_MakeAvailable(nanobind)

    nanobind_add_module(_native NB_STATIC python/src/native.cc)
    target_link_libraries(_native PRIVATE rawast::rawast)
    target_compile_features(_native PRIVATE cxx_std_17)

    install(TARGETS _native LIBRARY DESTINATION rawast)

    # Bundle the canonical grammars/ directory into the wheel under
    # rawast/grammars/ so Grammar(name) and grammar_path(name) resolve
    # at runtime in installed wheels (editable installs use the
    # python/rawast/grammars symlink to the same files in the repo).
    install(DIRECTORY ${CMAKE_SOURCE_DIR}/grammars/
            DESTINATION rawast/grammars
            FILES_MATCHING PATTERN "*.json" PATTERN "*.rawast")
endif()
