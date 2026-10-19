cmake_minimum_required(VERSION 3.20)
get_filename_component(DOLPHIN_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(CMAKE_USER_MAKE_RULES_OVERRIDE "${DOLPHIN_SOURCE_DIR}/CMake/FlagsOverride.cmake")
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")
project(DolphinDependencies LANGUAGES C CXX)

# Keep the dependency build independent of Dolphin's application sources. Use
# the same pinned submodules and wrappers as a normal bundled build.
set(DOLPHIN_BUILD_DEPENDENCY_BUNDLE ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_INSTALL_LIBDIR lib CACHE STRING "" FORCE)
set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
set(CMAKE_CONFIGURATION_TYPES Release CACHE STRING "" FORCE)
list(APPEND CMAKE_MODULE_PATH "${DOLPHIN_SOURCE_DIR}/CMake")
include(DolphinDisableWarningsMSVC)
find_package(Threads REQUIRED)

# Compiler versions and the selected Windows SDK are not reliably stored in
# CMakeCache.txt. Include them explicitly in the Actions cache fingerprint.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/bundle-toolchain.txt"
  "CMake=${CMAKE_VERSION}\n"
  "System=${CMAKE_SYSTEM_NAME};${CMAKE_SYSTEM_VERSION};${CMAKE_SYSTEM_PROCESSOR}\n"
  "C=${CMAKE_C_COMPILER};${CMAKE_C_COMPILER_ID};${CMAKE_C_COMPILER_VERSION}\n"
  "CXX=${CMAKE_CXX_COMPILER};${CMAKE_CXX_COMPILER_ID};${CMAKE_CXX_COMPILER_VERSION}\n"
  "Generator=${CMAKE_GENERATOR};${CMAKE_GENERATOR_PLATFORM};${CMAKE_GENERATOR_TOOLSET}\n"
  "WindowsSDK=${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}\n"
  "macOS=${CMAKE_OSX_SYSROOT};${CMAKE_OSX_ARCHITECTURES};${CMAKE_OSX_DEPLOYMENT_TARGET}\n")

if(MSVC)
  if(CMAKE_GENERATOR MATCHES "Visual Studio")
    add_compile_options(/MP)
  endif()
  add_compile_options(/utf-8 /GR- /EHsc /Gy /Oi /GS- /Zc:inline
    /Zc:__cplusplus,enumTypes,externConstexpr,preprocessor,templateScope,throwingNew
    /volatile:iso /fp:precise /experimental:deterministic)
else()
  add_compile_definitions(_DEFAULT_SOURCE _FILE_OFFSET_BITS=64 _LARGEFILE_SOURCE)
  add_compile_options(-fno-exceptions -fno-strict-aliasing
    -fvisibility=hidden -fvisibility-inlines-hidden)
  if(APPLE)
    add_compile_definitions(FMT_EXCEPTIONS=0)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
      add_compile_options(-mssse3 -march=core2)
    endif()
  endif()
endif()

set(BUILD_SHARED_LIBS OFF)
set(FMT_INSTALL ON CACHE BOOL "" FORCE)
set(FMT_TEST OFF CACHE BOOL "" FORCE)
set(GLSLANG_ENABLE_INSTALL ON CACHE BOOL "" FORCE)
set(SDL_INSTALL ON CACHE BOOL "" FORCE)
set(SKIP_GLSLANG_INSTALL OFF)
set(SDL_DISABLE_INSTALL OFF)
set(SDL_INSTALL_CMAKEDIR_ROOT lib/cmake/SDL3 CACHE STRING "" FORCE)
set(LibUSB_TYPE Bundled)

foreach(dependency fmt glslang libusb SDL SFML mbedtls)
  add_subdirectory("${DOLPHIN_SOURCE_DIR}/Externals/${dependency}" "${dependency}")
endforeach()

# Dolphin's small SFML wrapper and libusb do not have install/export rules.
# Export precisely those targets, including their transitive link dependencies.
foreach(target sfml-network sfml-system usb mbedcrypto mbedx509 mbedtls)
  set_property(TARGET ${target} PROPERTY INTERFACE_INCLUDE_DIRECTORIES
    "$<INSTALL_INTERFACE:include>")
endforeach()
if(WIN32)
  target_link_libraries(sfml-network PUBLIC ws2_32)
endif()
install(TARGETS sfml-network sfml-system usb mbedcrypto mbedx509 mbedtls
  EXPORT DolphinDependenciesTargets ARCHIVE DESTINATION lib)
install(EXPORT DolphinDependenciesTargets DESTINATION lib/cmake/DolphinDependencies)
install(DIRECTORY "${DOLPHIN_SOURCE_DIR}/Externals/SFML/SFML/include/SFML"
  DESTINATION include)
install(FILES "${DOLPHIN_SOURCE_DIR}/Externals/libusb/libusb/libusb/libusb.h"
  DESTINATION include)
install(DIRECTORY "${DOLPHIN_SOURCE_DIR}/LICENSES" DESTINATION share/dolphin-dependencies)

include(CMakePackageConfigHelpers)
configure_package_config_file(DolphinDependenciesConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/DolphinDependenciesConfig.cmake"
  INSTALL_DESTINATION lib/cmake/DolphinDependencies)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/DolphinDependenciesConfig.cmake"
  DESTINATION lib/cmake/DolphinDependencies)
