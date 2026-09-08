# User-facing build options. Keep defaults conservative: a plain
# `cmake -S . -B build` must succeed with nothing but a compiler and SQLite.

include(CMakeDependentOption)

option(CONTEXTSNAP_BUILD_TESTS "Build the unit/integration test suite" ON)
option(CONTEXTSNAP_BUILD_BENCHMARKS "Build capture/restore microbenchmarks" OFF)
option(CONTEXTSNAP_BUILD_GUI "Build the Qt 6 / QML desktop client" OFF)
option(CONTEXTSNAP_BUILD_CLI "Build the contextsnap command line client" ON)
option(CONTEXTSNAP_BUILD_NATIVE_HOST "Build the browser native messaging host" ON)

option(CONTEXTSNAP_WITH_PROTOBUF "Use the protobuf codec for IPC (JSON codec otherwise)" OFF)
option(CONTEXTSNAP_WITH_FLATBUFFERS "Enable FlatBuffers snapshot export" OFF)
option(CONTEXTSNAP_WITH_SQLCIPHER "Encrypt the snapshot database with SQLCipher" OFF)
option(CONTEXTSNAP_WITH_OPENSSL "Enable OpenSSL-backed export encryption/KDF" OFF)
option(CONTEXTSNAP_WITH_LZ4 "Enable Firefox mozlz4 session parsing" OFF)

cmake_dependent_option(CONTEXTSNAP_LINUX_X11 "Build the X11 backend" ON "UNIX;NOT APPLE" OFF)
cmake_dependent_option(CONTEXTSNAP_LINUX_WAYLAND "Build the Wayland backend" ON "UNIX;NOT APPLE" OFF)

option(CONTEXTSNAP_ENABLE_SANITIZERS "Enable ASan/UBSan in Debug builds" OFF)
option(CONTEXTSNAP_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(CONTEXTSNAP_ENABLE_CLANG_TIDY "Run clang-tidy as part of the build" OFF)
option(CONTEXTSNAP_ENABLE_COVERAGE "Instrument for coverage reporting" OFF)

# Runtime defaults compiled into the binaries; overridable at runtime by config.
set(CONTEXTSNAP_DEFAULT_SOCKET_NAME "contextsnapd.sock"
    CACHE STRING "Unix domain socket file name inside the runtime directory")
set(CONTEXTSNAP_DEFAULT_PIPE_NAME "\\\\.\\pipe\\contextsnapd"
    CACHE STRING "Windows named pipe path")
set(CONTEXTSNAP_NATIVE_HOST_ID "com.contextsnap.native_host"
    CACHE STRING "Browser native messaging host identifier")

if(CONTEXTSNAP_ENABLE_CLANG_TIDY)
  find_program(CLANG_TIDY_EXE NAMES clang-tidy REQUIRED)
  set(CMAKE_CXX_CLANG_TIDY "${CLANG_TIDY_EXE}" "--warnings-as-errors=*")
endif()
