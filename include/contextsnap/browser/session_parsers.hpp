// Passive session-file readers — the fallback when no extension is installed.
//
// Chromium family
//   <Profile>/Sessions/Session_<timestamp>   SNSS container: 4-byte magic
//   "SNSS", 4-byte version, then length-prefixed commands. We only decode the
//   handful of command ids that carry navigation entries and tab/window ids.
//   Newer builds may instead keep state in LevelDB (Session Storage) — detected
//   and reported as unsupported rather than mis-parsed.
//
// Firefox family
//   <Profile>/sessionstore-backups/recovery.jsonlz4 — "mozLz40\0" magic,
//   4-byte LE decompressed size, then a raw LZ4 block containing JSON.
//
// Everything here is read-only, never locks the browser's files (a copy is made
// first), and treats input as hostile: fuzzed by tests/fuzz/fuzz_session.cpp.
#pragma once

#include <contextsnap/browser/bridge.hpp>
#include <contextsnap/core/result.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace contextsnap::browser::session {

using core::Result;

struct ProfileLocation {
    BrowserKind browser{BrowserKind::None};
    std::string profile_name;
    std::string profile_path;
    std::string session_file;  ///< Newest session/recovery file found.
    bool readable{false};
};

/// Locates installed browser profiles for the current user on this platform.
[[nodiscard]] std::vector<ProfileLocation> discover_profiles();

/// Decompresses a mozLz4 ("mozLz40\0") buffer. Requires CONTEXTSNAP_WITH_LZ4;
/// otherwise returns Unsupported.
[[nodiscard]] Result<std::string> decompress_mozlz4(const std::vector<std::uint8_t>& input);

/// Parses Firefox recovery JSON (already decompressed) into browser windows.
[[nodiscard]] Result<std::vector<BrowserWindow>> parse_firefox_recovery(std::string_view json_text,
                                                                        const std::string& profile);

/// Reads and parses a Firefox recovery.jsonlz4 file end to end.
[[nodiscard]] Result<std::vector<BrowserWindow>> read_firefox_session(const std::string& path,
                                                                      const std::string& profile);

/// Minimal SNSS command stream reader.
struct SnssCommand {
    std::uint8_t id{0};
    std::vector<std::uint8_t> payload;
};

[[nodiscard]] Result<std::vector<SnssCommand>> parse_snss(const std::vector<std::uint8_t>& data);

/// Reconstructs windows/tabs from a Chromium SNSS session file.
[[nodiscard]] Result<std::vector<BrowserWindow>> read_chromium_session(const std::string& path,
                                                                       BrowserKind browser,
                                                                       const std::string& profile);

/// Chromium "Current Session"/"Last Session" pick plus SNSS/LevelDB detection.
[[nodiscard]] Result<std::string> newest_chromium_session_file(const std::string& profile_path);

/// Captures tabs from every discovered profile. Best effort: unreadable or
/// unsupported profiles become warnings, not failures.
[[nodiscard]] TabCapture capture_from_session_files();

}  // namespace contextsnap::browser::session
