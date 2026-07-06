# Standalone example: consume rawast as an installed library and build a
# custom value representation against its public {Builder, Accessor} seam.
#
#   cmake -S . -B build -DCMAKE_PREFIX_PATH=<rawast-install-prefix>
#   cmake --build build
#   ./build/custom-representation <grammar> <input>
#
# This is intentionally a SEPARATE project (not part of the rawast build):
# it proves rawast is consumable from the outside via find_package.
cmake_minimum_required(VERSION 3.20)
project(rawast-custom-representation CXX)

find_package(rawast REQUIRED)

add_executable(custom-representation main.cpp)
target_link_libraries(custom-representation PRIVATE rawast::rawast)
target_compile_features(custom-representation PRIVATE cxx_std_17)
