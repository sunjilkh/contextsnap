# ContextSnap — root build script
#
# Layout:
#   contextsnap_core        static library: core + storage + ipc + privacy + browser + HAL
#   contextsnapd            background daemon
#   contextsnap             CLI client
#   contextsnap-native-host browser native messaging bridge
#   contextsnap-gui         optional Qt 6 / QML client (frontend/qt)
#
# Everything except SQLite is optional. See docs/build.md.

cmake_minimum_required(VERSION 3.24)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")

# vcpkg toolchain auto-detection must happen before project().
include(VcpkgBootstrap)

project(
  contextsnap
  VERSION 0.1.0
  DESCRIPTION "Local-first digital context-state snapshot platform"
  HOMEPAGE_URL "https://github.com/contextsnap/contextsnap"
  LANGUAGES CXX)

if(APPLE)
  enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

include(CTest)
include(GNUInstallDirs)
include(ContextSnapOptions)
include(CompilerWarnings)
include(Platform)
include(Dependencies)

# ---------------------------------------------------------------------------
# Generated version header
# ---------------------------------------------------------------------------
set(CONTEXTSNAP_GENERATED_INCLUDE_DIR "${CMAKE_BINARY_DIR}/generated/include")

find_program(GIT_EXECUTABLE git)
set(CONTEXTSNAP_GIT_SHA "unknown")
if(GIT_EXECUTABLE AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" rev-parse --short=12 HEAD
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    OUTPUT_VARIABLE CONTEXTSNAP_GIT_SHA
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
endif()

configure_file("${CMAKE_SOURCE_DIR}/include/contextsnap/version.hpp.in"
               "${CONTEXTSNAP_GENERATED_INCLUDE_DIR}/contextsnap/version.hpp" @ONLY)

# ---------------------------------------------------------------------------
# Targets
# ---------------------------------------------------------------------------
add_subdirectory(src)

if(CONTEXTSNAP_BUILD_GUI)
  add_subdirectory(frontend/qt)
endif()

if(BUILD_TESTING AND CONTEXTSNAP_BUILD_TESTS)
  add_subdirectory(tests)
endif()

if(CONTEXTSNAP_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()

# ---------------------------------------------------------------------------
# Install & package
# ---------------------------------------------------------------------------
install(DIRECTORY include/contextsnap
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
        FILES_MATCHING
        PATTERN "*.hpp")
install(DIRECTORY "${CONTEXTSNAP_GENERATED_INCLUDE_DIR}/contextsnap"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(DIRECTORY sql/ DESTINATION "${CMAKE_INSTALL_DATADIR}/contextsnap/sql")
install(FILES LICENSE-MIT LICENSE-APACHE README.md
        DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/contextsnap")

if(UNIX AND NOT APPLE)
  install(FILES packaging/linux/contextsnapd.service
          DESTINATION "${CMAKE_INSTALL_LIBDIR}/systemd/user")
  install(FILES packaging/linux/contextsnap.desktop
          DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
  install(FILES packaging/linux/org.contextsnap.ContextSnap.metainfo.xml
          DESTINATION "${CMAKE_INSTALL_DATADIR}/metainfo")
endif()

set(CPACK_PACKAGE_NAME contextsnap)
set(CPACK_PACKAGE_VENDOR "The ContextSnap Authors")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE-MIT")
set(CPACK_STRIP_FILES ON)
include(CPack)

message(STATUS "")
message(STATUS "ContextSnap ${PROJECT_VERSION} (${CONTEXTSNAP_GIT_SHA})")
message(STATUS "  platform backend : ${CONTEXTSNAP_PLATFORM_NAME}")
message(STATUS "  gui              : ${CONTEXTSNAP_BUILD_GUI}")
message(STATUS "  tests            : ${CONTEXTSNAP_BUILD_TESTS}")
message(STATUS "  protobuf ipc     : ${CONTEXTSNAP_WITH_PROTOBUF}")
message(STATUS "  flatbuffers      : ${CONTEXTSNAP_WITH_FLATBUFFERS}")
message(STATUS "  sqlcipher        : ${CONTEXTSNAP_WITH_SQLCIPHER}")
message(STATUS "  lz4 (firefox)    : ${CONTEXTSNAP_WITH_LZ4}")
message(STATUS "")
