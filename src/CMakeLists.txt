# ContextSnap build graph
#
#   contextsnap_core          static library, all portable logic + the HAL
#   contextsnapd              daemon (IPC server, scheduler, browser endpoint)
#   contextsnap               CLI client
#   contextsnap-native-host   WebExtension native messaging host
#
# Only the platform backend for the host OS is compiled; the null backend is
# always compiled so tests and CI can run without a display server.

set(CONTEXTSNAP_CORE_SOURCES
    core/types.cpp
    core/result.cpp
    core/json.cpp
    core/geometry.cpp
    core/matching.cpp
    core/ulid.cpp
    core/logging.cpp
    core/config.cpp
    core/snapshot_manager.cpp
    core/restore_engine.cpp
    storage/database.cpp
    storage/migrations.cpp
    storage/serialization.cpp
    storage/snapshot_repository.cpp
    privacy/sanitizer.cpp
    privacy/crypto.cpp
    ipc/protocol.cpp
    ipc/server.cpp
    ipc/client.cpp
    browser/bridge.cpp
    browser/native_messaging.cpp
    browser/session_chromium.cpp
    browser/session_firefox.cpp
    hal/common/null_platform.cpp
    hal/common/platform_factory.cpp)

# --- Transport: one implementation per OS family -----------------------------
if(WIN32)
  list(APPEND CONTEXTSNAP_CORE_SOURCES ipc/transport_windows.cpp)
else()
  list(APPEND CONTEXTSNAP_CORE_SOURCES ipc/transport_posix.cpp)
endif()

# --- Platform HAL backend ----------------------------------------------------
if(CONTEXTSNAP_PLATFORM_WINDOWS)
  list(APPEND CONTEXTSNAP_CORE_SOURCES hal/windows/platform_windows.cpp)
elseif(CONTEXTSNAP_PLATFORM_MACOS)
  list(APPEND CONTEXTSNAP_CORE_SOURCES hal/macos/platform_macos.mm)
elseif(CONTEXTSNAP_PLATFORM_LINUX)
  list(APPEND CONTEXTSNAP_CORE_SOURCES hal/linux/platform_linux.cpp)
endif()

add_library(contextsnap_core STATIC ${CONTEXTSNAP_CORE_SOURCES})
add_library(contextsnap::core ALIAS contextsnap_core)

target_include_directories(
  contextsnap_core
  PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
         $<BUILD_INTERFACE:${CONTEXTSNAP_GENERATED_INCLUDE_DIR}>
         $<INSTALL_INTERFACE:include>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(
  contextsnap_core
  PUBLIC contextsnap::deps
  PRIVATE contextsnap::warnings contextsnap::platform_config)

target_compile_features(contextsnap_core PUBLIC cxx_std_20)
set_target_properties(contextsnap_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

# X11 is optional even on Linux: platform_linux.cpp compiles to a Wayland-only
# backend when CONTEXTSNAP_HAS_X11 is 0.
if(CONTEXTSNAP_PLATFORM_LINUX)
  if(CONTEXTSNAP_LINUX_X11 AND TARGET X11::xcb)
    target_compile_definitions(contextsnap_core PRIVATE CONTEXTSNAP_HAS_X11=1)
  else()
    target_compile_definitions(contextsnap_core PRIVATE CONTEXTSNAP_HAS_X11=0)
  endif()
  if(CONTEXTSNAP_LINUX_WAYLAND)
    target_compile_definitions(contextsnap_core PRIVATE CONTEXTSNAP_HAS_WAYLAND=1)
  endif()
endif()

# Generated IDL is optional; the JSON reference implementation always works.
if(TARGET contextsnap_proto)
  add_dependencies(contextsnap_core contextsnap_proto)
endif()
if(TARGET contextsnap_fbs)
  add_dependencies(contextsnap_core contextsnap_fbs)
endif()

# --- contextsnapd ------------------------------------------------------------
if(CONTEXTSNAP_BUILD_DAEMON)
  add_executable(contextsnapd daemon/main.cpp daemon/daemon.cpp)
  target_link_libraries(contextsnapd PRIVATE contextsnap::core contextsnap::warnings
                                             contextsnap::platform_config)
  target_include_directories(contextsnapd PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  install(TARGETS contextsnapd RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# --- contextsnap (CLI) ------------------------------------------------------
if(CONTEXTSNAP_BUILD_CLI)
  add_executable(contextsnap_cli cli/main.cpp)
  set_target_properties(contextsnap_cli PROPERTIES OUTPUT_NAME contextsnap)
  target_link_libraries(contextsnap_cli PRIVATE contextsnap::core contextsnap::warnings
                                                contextsnap::platform_config)
  target_include_directories(contextsnap_cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  install(TARGETS contextsnap_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# --- contextsnap-native-host -------------------------------------------------
if(CONTEXTSNAP_BUILD_NATIVE_HOST)
  add_executable(contextsnap_native_host native_host/main.cpp)
  set_target_properties(contextsnap_native_host PROPERTIES OUTPUT_NAME contextsnap-native-host)
  target_link_libraries(contextsnap_native_host PRIVATE contextsnap::core contextsnap::warnings
                                                        contextsnap::platform_config)
  install(TARGETS contextsnap_native_host RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
