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
    src/builder.cpp
    src/accessor.cpp
    src/grammar.cpp
    src/save_stack.cpp
    src/loader.cpp
    src/to_value.cpp
    src/linter.cpp
    src/profile.cpp
    src/parsers_gdsii.cpp
    src/parsers_lefdef.cpp
    src/parsers_sv.cpp
    src/parsers_tcl.cpp
    src/parsers_registry.cpp
    src/preprocessor.cpp
    src/preprocessor_eval.cpp
)
add_library(rawast::rawast ALIAS rawast)

target_include_directories(rawast
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# tl::expected is header-only and appears in rawast's PUBLIC return types
# (grammar.hpp etc.). Link it at BUILD time only — the installed/exported
# rawast target does not depend on the tl-expected CMake target; instead the
# single tl/expected.hpp header is bundled into rawast's install include tree
# (see the install block below), so a downstream find_package(rawast)
# consumer needs nothing beyond rawast itself.
target_link_libraries(rawast PUBLIC $<BUILD_INTERFACE:tl::expected>)

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
# Install / export — make rawast consumable as a generic engine library
# via find_package(rawast). A downstream project links rawast::rawast,
# includes the public seam headers (builder.hpp / accessor.hpp /
# grammar.hpp / stream.hpp / value.hpp / node.hpp), implements its own
# {Builder, Accessor} representation, and drives it through parse_into /
# save_from / convert. Concrete representations (arenas, columnar stores,
# host structures) live in the CONSUMING project — not here.
# -----------------------------------------------------------------------
set(_rawast_is_top OFF)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(_rawast_is_top ON)
endif()
option(RAWAST_INSTALL
    "Generate install + export rules for downstream find_package(rawast)"
    ${_rawast_is_top})

if(RAWAST_INSTALL AND NOT SKBUILD)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    install(TARGETS rawast
        EXPORT rawastTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

    # Public seam headers.
    install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/rawast
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        FILES_MATCHING PATTERN "*.hpp")

    # Bundle the header-only tl::expected so <tl/expected.hpp> (used in
    # rawast's public headers) resolves from rawast's own include tree.
    install(FILES ${tl-expected_SOURCE_DIR}/include/tl/expected.hpp
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/tl)

    install(EXPORT rawastTargets
        FILE rawastTargets.cmake
        NAMESPACE rawast::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rawast)

    configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/rawastConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/rawastConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rawast)
    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/rawastConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion)
    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/rawastConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/rawastConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rawast)
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

    nanobind_add_module(_rawast NB_STATIC python/src/native.cc)
    target_link_libraries(_rawast PRIVATE rawast::rawast)
    target_compile_features(_rawast PRIVATE cxx_std_17)

    install(TARGETS _rawast LIBRARY DESTINATION rawast)

    # Bundle the canonical grammars/ directory into the wheel under
    # rawast/grammars/ so Grammar(name) and grammar_path(name) resolve
    # at runtime in installed wheels (editable installs use the
    # python/rawast/grammars symlink to the same files in the repo).
    install(DIRECTORY ${CMAKE_SOURCE_DIR}/grammars/
            DESTINATION rawast/grammars
            FILES_MATCHING PATTERN "*.json" PATTERN "*.rawast")
endif()
