# Auto-selects the vcpkg toolchain when VCPKG_ROOT is exported and the caller
# has not already supplied a toolchain file. Must be included before project().

if(DEFINED CMAKE_TOOLCHAIN_FILE)
  return()
endif()

if(DEFINED ENV{VCPKG_ROOT} AND EXISTS "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
  set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
      CACHE STRING "vcpkg toolchain file")
  message(STATUS "ContextSnap: using vcpkg toolchain at $ENV{VCPKG_ROOT}")
else()
  message(STATUS "ContextSnap: no vcpkg toolchain detected, using system packages")
endif()

# Manifest features are mapped from ContextSnap options by the caller, e.g.
#   cmake --preset release -DVCPKG_MANIFEST_FEATURES="gui;sqlcipher"
