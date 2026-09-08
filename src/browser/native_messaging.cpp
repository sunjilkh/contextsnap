// WebExtension native messaging host.
//
// The browser starts this process and speaks the standard native messaging
// protocol on stdio: a 4-byte little-endian length prefix followed by UTF-8
// JSON. The host relays those frames to the daemon's browser endpoint using
// exactly the same framing, so it stays a thin, auditable pipe.
#include <contextsnap/browser/native_host.hpp>

#include <contextsnap/core/logging.hpp>
#include <contextsnap/ipc/protocol.hpp>
#include <contextsnap/storage/serialization.hpp>
#include <contextsnap/ipc/transport.hpp>
#include <contextsnap/version.hpp>

#include <atomic>
#include <thread>

namespace contextsnap::browser {

Result<ExtensionMessage> ExtensionMessage::from_json(const core::json::Value& value) {
    const core::json::Value* type = value.find("type");
    if (type == nullptr || !type->is_string() || type->as_string().empty()) {
        return core::err::protocol("extension message is missing a string \"type\"",
                                   "browser.native");
    }
    ExtensionMessage message;
    message.type = type->as_string();
    if (const core::json::Value* payload = value.find("payload"); payload != nullptr) {
        message.payload = *payload;
    }
    return message;
}

core::json::Value HostMessage::to_json() const {
    core::json::Value out = core::json::Value::object();
    out.set("type", core::json::Value(type));
    out.set("version", core::json::Value(static_cast<std::int64_t>(kNativeMessagingVersion)));
    out.set("payload", payload);
    return out;
}

Result<std::vector<BrowserWindow>> parse_tabs_message(const core::json::Value& payload) {
    const core::json::Value* windows = payload.find("windows");
    if (windows == nullptr || !windows->is_array()) {
        return core::err::protocol("tabs message has no \"windows\" array", "browser.native");
    }
    const std::string profile = payload.find("profile") == nullptr
                                    ? std::string{}
                                    : payload.find("profile")->as_string();
    const core::BrowserKind browser =
        payload.find("browser") == nullptr
            ? core::BrowserKind::None
            : core::browser_kind_from_string(payload.find("browser")->as_string());

    std::vector<BrowserWindow> result;
    for (const core::json::Value& entry : windows->as_array()) {
        BrowserWindow window;
        window.browser = browser;
        window.profile = profile;
        if (const core::json::Value* id = entry.find("id"); id != nullptr) {
            window.extension_window_id = id->as_int();
        }
        if (const core::json::Value* focused = entry.find("focused"); focused != nullptr) {
            window.focused = focused->as_bool();
        }
        if (const core::json::Value* incognito = entry.find("incognito"); incognito != nullptr) {
            window.incognito = incognito->as_bool();
        }
        if (const core::json::Value* state = entry.find("state"); state != nullptr) {
            window.state = state->as_string();
        }
        window.frame.x = static_cast<std::int32_t>(
            entry.find("left") == nullptr ? 0 : entry.find("left")->as_int());
        window.frame.y = static_cast<std::int32_t>(
            entry.find("top") == nullptr ? 0 : entry.find("top")->as_int());
        window.frame.width = static_cast<std::int32_t>(
            entry.find("width") == nullptr ? 0 : entry.find("width")->as_int());
        window.frame.height = static_cast<std::int32_t>(
            entry.find("height") == nullptr ? 0 : entry.find("height")->as_int());

        const core::json::Value* tabs = entry.find("tabs");
        if (tabs != nullptr && tabs->is_array()) {
            for (const core::json::Value& tab : tabs->as_array()) {
                auto parsed = storage::tab_from_json(tab);
                if (parsed) {
                    window.tabs.push_back(parsed.value());
                }
            }
        }
        result.push_back(std::move(window));
    }
    return result;
}

core::json::Value build_restore_message(const std::vector<BrowserWindow>& windows,
                                        bool lazy_load) {
    core::json::Array window_array;
    for (const BrowserWindow& window : windows) {
        core::json::Value entry = core::json::Value::object();
        entry.set("browser", core::json::Value(std::string(core::to_string(window.browser))));
        entry.set("profile", core::json::Value(window.profile));
        entry.set("incognito", core::json::Value(window.incognito));
        entry.set("state", core::json::Value(window.state));
        entry.set("left", core::json::Value(static_cast<std::int64_t>(window.frame.x)));
        entry.set("top", core::json::Value(static_cast<std::int64_t>(window.frame.y)));
        entry.set("width", core::json::Value(static_cast<std::int64_t>(window.frame.width)));
        entry.set("height", core::json::Value(static_cast<std::int64_t>(window.frame.height)));

        core::json::Array tab_array;
        for (const core::TabInfo& tab : window.tabs) {
            core::json::Value item = core::json::Value::object();
            item.set("url", core::json::Value(tab.url));
            item.set("title", core::json::Value(tab.title));
            item.set("index", core::json::Value(static_cast<std::int64_t>(tab.index)));
            item.set("pinned", core::json::Value(tab.pinned));
            item.set("active", core::json::Value(tab.active));
            item.set("muted", core::json::Value(tab.muted));
            // Inactive tabs are created discarded so a 200-tab restore does not
            // spawn 200 renderer processes.
            item.set("discarded", core::json::Value(lazy_load && !tab.active));
            if (tab.cookie_store_id.has_value()) {
                item.set("cookieStoreId", core::json::Value(*tab.cookie_store_id));
            }
            if (tab.group_title.has_value()) {
                item.set("groupTitle", core::json::Value(*tab.group_title));
            }
            tab_array.push_back(std::move(item));
        }
        entry.set("tabs", core::json::Value(std::move(tab_array)));
        window_array.push_back(std::move(entry));
    }

    core::json::Value payload = core::json::Value::object();
    payload.set("lazyLoad", core::json::Value(lazy_load));
    payload.set("windows", core::json::Value(std::move(window_array)));
    return payload;
}

std::string build_host_manifest(const std::string& host_path,
                                const std::vector<std::string>& allowed_origins,
                                const std::vector<std::string>& allowed_extensions) {
    core::json::Value manifest = core::json::Value::object();
    manifest.set("name", core::json::Value(std::string(kNativeHostId)));
    manifest.set("description",
                 core::json::Value(std::string("ContextSnap native messaging host")));
    manifest.set("path", core::json::Value(host_path));
    manifest.set("type", core::json::Value(std::string("stdio")));
    if (!allowed_origins.empty()) {
        core::json::Array origins;
        for (const std::string& origin : allowed_origins) {
            origins.emplace_back(core::json::Value(origin));
        }
        manifest.set("allowed_origins", core::json::Value(std::move(origins)));
    }
    if (!allowed_extensions.empty()) {
        core::json::Array extensions;
        for (const std::string& extension : allowed_extensions) {
            extensions.emplace_back(core::json::Value(extension));
        }
        manifest.set("allowed_extensions", core::json::Value(std::move(extensions)));
    }
    return manifest.dump(2);
}

namespace {

bool write_bytes(ipc::Connection& connection, std::string_view bytes) {
    std::size_t written = 0;
    while (written < bytes.size()) {
        auto result = connection.write(bytes.substr(written));
        if (!result || result.value() == 0) {
            return false;
        }
        written += result.value();
    }
    return true;
}

bool write_payload(ipc::Connection& connection, const std::string& payload) {
    auto frame = ipc::encode_frame(payload);
    if (!frame) {
        return false;
    }
    return write_bytes(connection,
                       std::string_view(reinterpret_cast<const char*>(frame.value().data()),
                                        frame.value().size()));
}

bool write_frame(ipc::Connection& connection, const core::json::Value& value) {
    return write_payload(connection, value.dump());
}

/// Pumps frames from `from` to `to` until either side closes.
void pump(ipc::Connection& from, ipc::Connection& to, std::atomic<bool>& running,
          std::uint32_t max_bytes, const char* label) {
    ipc::FrameReader reader(max_bytes);
    while (running.load()) {
        auto chunk = from.read_some();
        if (!chunk || chunk.value().empty()) {
            break;
        }
        reader.feed(chunk.value());
        while (true) {
            auto next = reader.next();
            if (!next) {
                core::log::error("native host framing error",
                                 {core::log::field("side", std::string(label)),
                                  core::log::field("error", next.error().to_string())});
                running.store(false);
                return;
            }
            if (!next.value().has_value()) {
                break;
            }
            if (!write_payload(to, *next.value())) {
                running.store(false);
                return;
            }
        }
    }
    running.store(false);
}

}  // namespace

int run_native_host(const NativeHostOptions& options) {
    std::unique_ptr<ipc::Connection> browser = ipc::stdio_connection();
    if (browser == nullptr) {
        return 2;
    }

    auto daemon = ipc::connect(options.daemon_endpoint, options.daemon_timeout);
    if (!daemon) {
        // Tell the extension why nothing will happen; it surfaces this in the
        // popup instead of silently doing nothing.
        HostMessage error;
        error.type = "error";
        error.payload = core::json::Value::object();
        error.payload.set("message",
                          core::json::Value("contextsnapd is not running: " +
                                            daemon.error().message));
        (void)write_frame(*browser, error.to_json());
        return 3;
    }

    HostMessage hello;
    hello.type = "hello";
    hello.payload = core::json::Value::object();
    hello.payload.set("host", core::json::Value(std::string(kNativeHostId)));
    hello.payload.set("version", core::json::Value(std::string(kVersion)));
    hello.payload.set("protocol",
                      core::json::Value(static_cast<std::int64_t>(kNativeMessagingVersion)));
    if (!write_frame(*browser, hello.to_json()) ||
        !write_frame(*daemon.value(), hello.to_json())) {
        return 4;
    }

    std::atomic<bool> running{true};
    std::thread inbound([&] {
        pump(*browser, *daemon.value(), running, options.max_message_bytes, "browser->daemon");
    });
    pump(*daemon.value(), *browser, running, options.max_message_bytes, "daemon->browser");
    running.store(false);
    browser->close();
    daemon.value()->close();
    if (inbound.joinable()) {
        inbound.join();
    }
    return 0;
}

}  // namespace contextsnap::browser
