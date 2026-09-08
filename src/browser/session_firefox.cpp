// Firefox fallback: sessionstore-backups/recovery.jsonlz4 is a mozlz4 file --
// the magic string "mozLz40\0", a little-endian uint32 with the decompressed
// size, and then a raw LZ4 block. The block decoder below is intentionally
// small and dependency-free so the fallback works without linking liblz4.
#include <cstdlib>
#include <contextsnap/browser/session_parsers.hpp>

#include <contextsnap/core/json.hpp>

#include <cstring>
#include <fstream>
#include <iterator>

namespace contextsnap::browser::session {
namespace {

constexpr char kMagic[] = "mozLz40";

/// Minimal LZ4 block decompressor (sequence = token, literals, match).
bool lz4_block_decompress(const std::uint8_t* input, std::size_t input_size, std::string& out,
                          std::size_t expected_size) {
    out.clear();
    out.reserve(expected_size);
    std::size_t position = 0;
    while (position < input_size) {
        const std::uint8_t token = input[position++];
        std::size_t literal_length = token >> 4;
        if (literal_length == 15) {
            std::uint8_t extra = 0;
            do {
                if (position >= input_size) return false;
                extra = input[position++];
                literal_length += extra;
            } while (extra == 255);
        }
        if (position + literal_length > input_size) return false;
        out.append(reinterpret_cast<const char*>(input + position), literal_length);
        position += literal_length;
        if (position == input_size) {
            break;  // Last sequence contains literals only.
        }
        if (position + 2 > input_size) return false;
        const std::size_t offset = static_cast<std::size_t>(input[position]) |
                                   (static_cast<std::size_t>(input[position + 1]) << 8);
        position += 2;
        if (offset == 0 || offset > out.size()) return false;
        std::size_t match_length = token & 0x0F;
        if (match_length == 15) {
            std::uint8_t extra = 0;
            do {
                if (position >= input_size) return false;
                extra = input[position++];
                match_length += extra;
            } while (extra == 255);
        }
        match_length += 4;  // Minimum match length.
        const std::size_t start = out.size() - offset;
        for (std::size_t i = 0; i < match_length; ++i) {
            out.push_back(out[start + i]);  // Overlapping copies are legal in LZ4.
        }
    }
    return true;
}

void append_tabs(const core::json::Value& tab_array, BrowserWindow& window) {
    if (!tab_array.is_array()) {
        return;
    }
    std::int32_t index = 0;
    for (const core::json::Value& tab : tab_array.as_array()) {
        const core::json::Value* entries = tab.find("entries");
        if (entries == nullptr || !entries->is_array() ||
            entries->as_array().empty()) {
            continue;
        }
        // "index" is 1-based and points at the visible history entry.
        const core::json::Value* index_value = tab.find("index");
        std::size_t entry_index = entries->as_array().size() - 1;
        if (index_value != nullptr && index_value->as_int() > 0) {
            const std::size_t candidate = static_cast<std::size_t>(index_value->as_int()) - 1;
            if (candidate < entries->as_array().size()) {
                entry_index = candidate;
            }
        }
        const core::json::Value& entry = (entries->as_array())[entry_index];

        core::TabInfo info;
        info.index = index++;
        info.id = info.index;
        if (const core::json::Value* url = entry.find("url"); url != nullptr) {
            info.url = url->as_string();
        }
        if (const core::json::Value* title = entry.find("title"); title != nullptr) {
            info.title = title->as_string();
        }
        if (const core::json::Value* pinned = tab.find("pinned"); pinned != nullptr) {
            info.pinned = pinned->as_bool();
        }
        if (const core::json::Value* hidden = tab.find("hidden"); hidden != nullptr) {
            info.discarded = hidden->as_bool();
        }
        if (const core::json::Value* cookies = tab.find("userContextId");
            cookies != nullptr && cookies->as_int() > 0) {
            info.cookie_store_id = "firefox-container-" + std::to_string(cookies->as_int());
        }
        if (const core::json::Value* scroll = tab.find("scroll"); scroll != nullptr) {
            if (const core::json::Value* y = scroll->find("scrollY"); y != nullptr) {
                info.scroll_y = static_cast<std::int32_t>(y->as_double());
            }
        }
        if (!info.url.empty()) {
            window.tabs.push_back(std::move(info));
        }
    }
}

}  // namespace

Result<std::string> decompress_mozlz4(const std::vector<std::uint8_t>& input) {
    constexpr std::size_t kHeaderSize = 8 + 4;  // magic (with NUL) + size
    if (input.size() < kHeaderSize || std::memcmp(input.data(), kMagic, 7) != 0) {
        return core::err::protocol("not a mozlz4 file", "browser.firefox");
    }
    const std::size_t expected = static_cast<std::size_t>(input[8]) |
                                 (static_cast<std::size_t>(input[9]) << 8) |
                                 (static_cast<std::size_t>(input[10]) << 16) |
                                 (static_cast<std::size_t>(input[11]) << 24);
    std::string out;
    if (!lz4_block_decompress(input.data() + kHeaderSize, input.size() - kHeaderSize, out,
                              expected)) {
        return core::err::protocol("mozlz4 payload is truncated or corrupt", "browser.firefox");
    }
    if (expected != 0 && out.size() != expected) {
        return core::err::protocol("mozlz4 size mismatch (expected " + std::to_string(expected) +
                                       ", got " + std::to_string(out.size()) + ")",
                                   "browser.firefox");
    }
    return out;
}

Result<std::vector<BrowserWindow>> parse_firefox_recovery(std::string_view json_text,
                                                          const std::string& profile) {
    auto document = core::json::parse(json_text);
    if (!document) {
        return document.error();
    }
    const core::json::Value* windows = document.value().find("windows");
    if (windows == nullptr || !windows->is_array()) {
        return core::err::protocol("recovery file has no windows array", "browser.firefox");
    }
    const core::json::Value* selected = document.value().find("selectedWindow");
    const std::int64_t selected_window = selected == nullptr ? 1 : selected->as_int();

    std::vector<BrowserWindow> result;
    std::int64_t ordinal = 0;
    for (const core::json::Value& entry : windows->as_array()) {
        ++ordinal;
        BrowserWindow window;
        window.browser = core::BrowserKind::Firefox;
        window.profile = profile;
        window.extension_window_id = ordinal;
        window.focused = ordinal == selected_window;
        if (const core::json::Value* privacy = entry.find("isPrivate"); privacy != nullptr) {
            window.incognito = privacy->as_bool();
        }
        // Firefox stores geometry as strings in "sizemode"/"width"/"height".
        if (const core::json::Value* mode = entry.find("sizemode"); mode != nullptr) {
            window.state = mode->as_string();
        }
        auto read_int = [&entry](const char* key, std::int32_t& target) {
            if (const core::json::Value* value = entry.find(key); value != nullptr) {
                target = value->is_string() ? static_cast<std::int32_t>(std::atoi(
                                                  value->as_string().c_str()))
                                            : static_cast<std::int32_t>(value->as_int());
            }
        };
        read_int("screenX", window.frame.x);
        read_int("screenY", window.frame.y);
        read_int("width", window.frame.width);
        read_int("height", window.frame.height);

        if (const core::json::Value* tabs = entry.find("tabs"); tabs != nullptr) {
            append_tabs(*tabs, window);
        }
        if (const core::json::Value* selected_tab = entry.find("selected");
            selected_tab != nullptr) {
            const std::size_t active = static_cast<std::size_t>(selected_tab->as_int());
            if (active >= 1 && active <= window.tabs.size()) {
                window.tabs[active - 1].active = true;
            }
        }
        if (!window.tabs.empty()) {
            result.push_back(std::move(window));
        }
    }
    if (result.empty()) {
        return core::err::not_found("recovery file had no restorable tabs", "browser.firefox");
    }
    return result;
}

Result<std::vector<BrowserWindow>> read_firefox_session(const std::string& path,
                                                        const std::string& profile) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return core::err::io("cannot open " + path, "browser.firefox");
    }
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(stream)),
                                          std::istreambuf_iterator<char>());
    if (bytes.size() >= 7 && std::memcmp(bytes.data(), kMagic, 7) == 0) {
        auto text = decompress_mozlz4(bytes);
        if (!text) {
            return text.error();
        }
        return parse_firefox_recovery(text.value(), profile);
    }
    // Older builds (and `sessionstore.js`) store plain JSON.
    return parse_firefox_recovery(
        std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), profile);
}

}  // namespace contextsnap::browser::session
