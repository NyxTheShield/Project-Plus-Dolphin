cmake_minimum_required(VERSION 3.20)
project(DolphinDependencySmoke LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 23)
# Exercise the same find modules that Dolphin uses when importing the bundle.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/../..")
find_package(DolphinDependencies REQUIRED CONFIG
  PATHS "${DOLPHIN_DEPENDENCY_PREFIX}/lib/cmake/DolphinDependencies" NO_DEFAULT_PATH)
add_executable(dependency-smoke main.cpp)
target_link_libraries(dependency-smoke PRIVATE fmt::fmt glslang::glslang
  SDL3::SDL3 sfml-network MbedTLS::mbedcrypto LibUSB::LibUSB)
enable_testing()
add_test(NAME dependency-smoke COMMAND dependency-smoke)
