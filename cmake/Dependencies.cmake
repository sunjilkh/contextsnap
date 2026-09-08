# Resolves third-party dependencies. Only SQLite is mandatory; everything else
# is gated behind a CONTEXTSNAP_WITH_* option and degrades to an in-tree
# implementation when disabled.

add_library(contextsnap_deps INTERFACE)
add_library(contextsnap::deps ALIAS contextsnap_deps)

# --- SQLite / SQLCipher ------------------------------------------------------
if(CONTEXTSNAP_WITH_SQLCIPHER)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SQLCIPHER REQUIRED IMPORTED_TARGET sqlcipher)
  target_link_libraries(contextsnap_deps INTERFACE PkgConfig::SQLCIPHER)
  target_compile_definitions(contextsnap_deps INTERFACE CONTEXTSNAP_WITH_SQLCIPHER=1
                                                        SQLITE_HAS_CODEC=1)
else()
  find_package(SQLite3 3.35 REQUIRED)
  target_link_libraries(contextsnap_deps INTERFACE SQLite::SQLite3)
endif()

# --- Optional: protobuf IPC codec -------------------------------------------
if(CONTEXTSNAP_WITH_PROTOBUF)
  find_package(Protobuf CONFIG REQUIRED)
  target_link_libraries(contextsnap_deps INTERFACE protobuf::libprotobuf)
  target_compile_definitions(contextsnap_deps INTERFACE CONTEXTSNAP_WITH_PROTOBUF=1)
endif()

# --- Optional: FlatBuffers export -------------------------------------------
if(CONTEXTSNAP_WITH_FLATBUFFERS)
  find_package(Flatbuffers CONFIG REQUIRED)
  target_link_libraries(contextsnap_deps INTERFACE flatbuffers::flatbuffers)
  target_compile_definitions(contextsnap_deps INTERFACE CONTEXTSNAP_WITH_FLATBUFFERS=1)
endif()

# --- Optional: OpenSSL (export encryption, KDF) ------------------------------
if(CONTEXTSNAP_WITH_OPENSSL OR CONTEXTSNAP_WITH_SQLCIPHER)
  find_package(OpenSSL REQUIRED COMPONENTS Crypto)
  target_link_libraries(contextsnap_deps INTERFACE OpenSSL::Crypto)
  target_compile_definitions(contextsnap_deps INTERFACE CONTEXTSNAP_WITH_OPENSSL=1)
endif()

# --- Optional: lz4 (Firefox mozlz4 recovery files) ---------------------------
if(CONTEXTSNAP_WITH_LZ4)
  find_package(lz4 CONFIG QUIET)
  if(lz4_FOUND)
    target_link_libraries(contextsnap_deps INTERFACE lz4::lz4)
  else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LZ4 REQUIRED IMPORTED_TARGET liblz4)
    target_link_libraries(contextsnap_deps INTERFACE PkgConfig::LZ4)
  endif()
  target_compile_definitions(contextsnap_deps INTERFACE CONTEXTSNAP_WITH_LZ4=1)
endif()

# --- Platform SDK libraries --------------------------------------------------
if(CONTEXTSNAP_PLATFORM_WINDOWS)
  target_link_libraries(
    contextsnap_deps
    INTERFACE user32
              shell32
              ole32
              oleaut32
              dwmapi
              shcore
              advapi32
              psapi
              version
              crypt32)
elseif(CONTEXTSNAP_PLATFORM_MACOS)
  find_library(COCOA_LIBRARY Cocoa REQUIRED)
  find_library(APPKIT_LIBRARY AppKit REQUIRED)
  find_library(COREGRAPHICS_LIBRARY CoreGraphics REQUIRED)
  find_library(COREFOUNDATION_LIBRARY CoreFoundation REQUIRED)
  find_library(APPLICATIONSERVICES_LIBRARY ApplicationServices REQUIRED)
  find_library(SECURITY_LIBRARY Security REQUIRED)
  target_link_libraries(
    contextsnap_deps
    INTERFACE ${COCOA_LIBRARY} ${APPKIT_LIBRARY} ${COREGRAPHICS_LIBRARY} ${COREFOUNDATION_LIBRARY}
              ${APPLICATIONSERVICES_LIBRARY} ${SECURITY_LIBRARY})
elseif(CONTEXTSNAP_PLATFORM_LINUX)
  find_package(PkgConfig REQUIRED)
  if(CONTEXTSNAP_LINUX_X11)
    pkg_check_modules(XCB IMPORTED_TARGET xcb xcb-ewmh xcb-icccm xcb-randr xcb-xfixes)
    if(XCB_FOUND)
      target_link_libraries(contextsnap_deps INTERFACE PkgConfig::XCB)
      target_compile_definitions(contextsnap_deps INTERFACE CONTEXTSNAP_LINUX_X11=1)
    else()
      message(WARNING "xcb development packages not found — X11 backend disabled")
      set(CONTEXTSNAP_LINUX_X11 OFF CACHE BOOL "" FORCE)
    endif()
  endif()
  if(CONTEXTSNAP_LINUX_WAYLAND)
    pkg_check_modules(WAYLAND IMPORTED_TARGET wayland-client)
    if(WAYLAND_FOUND)
      target_link_libraries(contextsnap_deps INTERFACE PkgConfig::WAYLAND)
      target_compile_definitions(contextsnap_deps INTERFACE CONTEXTSNAP_LINUX_WAYLAND=1)
    else()
      message(WARNING "wayland-client not found — Wayland backend disabled")
      set(CONTEXTSNAP_LINUX_WAYLAND OFF CACHE BOOL "" FORCE)
    endif()
  endif()
  # D-Bus is used for XDG Desktop Portals and session bus notifications.
  pkg_check_modules(DBUS IMPORTED_TARGET dbus-1)
  if(DBUS_FOUND)
    target_link_libraries(contextsnap_deps INTERFACE PkgConfig::DBUS)
    target_compile_definitions(contextsnap_deps INTERFACE CONTEXTSNAP_WITH_DBUS=1)
  endif()
endif()
