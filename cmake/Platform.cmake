# Detects the target platform and exposes CONTEXTSNAP_PLATFORM_* variables plus
# the contextsnap::platform_config interface target with the right defines.

add_library(contextsnap_platform_config INTERFACE)
add_library(contextsnap::platform_config ALIAS contextsnap_platform_config)

if(WIN32)
  set(CONTEXTSNAP_PLATFORM_NAME "windows")
  set(CONTEXTSNAP_PLATFORM_WINDOWS ON)
  target_compile_definitions(
    contextsnap_platform_config
    INTERFACE CONTEXTSNAP_PLATFORM_WINDOWS=1
              WIN32_LEAN_AND_MEAN
              NOMINMAX
              UNICODE
              _UNICODE
              _WIN32_WINNT=0x0A00 # Windows 10
              NTDDI_VERSION=0x0A000007)
elseif(APPLE)
  set(CONTEXTSNAP_PLATFORM_NAME "macos")
  set(CONTEXTSNAP_PLATFORM_MACOS ON)
  target_compile_definitions(contextsnap_platform_config INTERFACE CONTEXTSNAP_PLATFORM_MACOS=1)
elseif(UNIX)
  set(CONTEXTSNAP_PLATFORM_NAME "linux")
  set(CONTEXTSNAP_PLATFORM_LINUX ON)
  target_compile_definitions(contextsnap_platform_config INTERFACE CONTEXTSNAP_PLATFORM_LINUX=1)
else()
  message(FATAL_ERROR "ContextSnap supports Windows, macOS and Linux only")
endif()

target_compile_definitions(
  contextsnap_platform_config
  INTERFACE CONTEXTSNAP_PLATFORM_NAME="${CONTEXTSNAP_PLATFORM_NAME}"
            CONTEXTSNAP_DEFAULT_SOCKET_NAME="${CONTEXTSNAP_DEFAULT_SOCKET_NAME}"
            CONTEXTSNAP_NATIVE_HOST_ID="${CONTEXTSNAP_NATIVE_HOST_ID}")

if(CONTEXTSNAP_PLATFORM_WINDOWS)
  target_compile_definitions(contextsnap_platform_config
                             INTERFACE CONTEXTSNAP_DEFAULT_PIPE_NAME="${CONTEXTSNAP_DEFAULT_PIPE_NAME}")
endif()

# Threads are required on every platform.
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
target_link_libraries(contextsnap_platform_config INTERFACE Threads::Threads)

message(STATUS "ContextSnap platform backend: ${CONTEXTSNAP_PLATFORM_NAME}")
