// Internal (non-installed) header for the contextsnapd process.
//
// The daemon owns every long-lived resource: the platform backend, the SQLite
// repository, the browser bridge and the two IPC listeners. Everything else in
// the project (CLI, GUI, native host) is a client of this process.
#pragma once

#include <contextsnap/browser/bridge.hpp>
#include <contextsnap/core/config.hpp>
#include <contextsnap/core/snapshot_manager.hpp>
#include <contextsnap/hal/platform.hpp>
#include <contextsnap/ipc/server.hpp>
#include <contextsnap/ipc/transport.hpp>
#include <contextsnap/privacy/sanitizer.hpp>
#include <contextsnap/storage/snapshot_repository.hpp>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace contextsnap::daemon {

struct DaemonOptions {
    core::Config config{};
    /// Overrides config.ipc.socket_path when non-empty.
    std::string endpoint;
    /// Second listener used by contextsnap-native-host. Defaults to
    /// `endpoint + "-browser"`.
    std::string browser_endpoint;
    /// Runs periodic/idle captures. Disabled by `--no-scheduler`.
    bool scheduler{true};
    /// Forces the null HAL backend (also honours CONTEXTSNAP_HEADLESS).
    bool headless{false};
};

/// Appends the browser-listener suffix to an IPC endpoint. Works for both Unix
/// socket paths and Windows pipe names.
[[nodiscard]] std::string browser_endpoint_for(const std::string& endpoint);

class Daemon {
public:
    explicit Daemon(DaemonOptions options);
    ~Daemon();

    Daemon(const Daemon&) = delete;
    Daemon& operator=(const Daemon&) = delete;

    /// Opens the database, builds the platform backend and starts listening.
    [[nodiscard]] core::Status start();

    /// Blocks until stop() is called (from a signal handler or the Shutdown RPC).
    void run();

    void stop();

    [[nodiscard]] bool running() const noexcept { return running_.load(); }

    [[nodiscard]] core::SnapshotManager& manager() { return *manager_; }

    [[nodiscard]] const std::string& endpoint() const noexcept { return endpoint_; }

private:
    void register_handlers();

    /// Periodic capture, idle capture and retention pruning.
    void scheduler_loop();

    /// Accepts contextsnap-native-host connections and pumps browser frames.
    void browser_loop();
    void serve_browser_host(std::unique_ptr<ipc::Connection> connection,
                            const std::string& connection_id);

    void publish(ipc::EventType type, core::json::Value payload);

    DaemonOptions options_;
    std::string endpoint_;
    std::string browser_endpoint_;

    std::shared_ptr<hal::Platform> platform_;
    std::shared_ptr<storage::SnapshotRepository> repository_;
    std::shared_ptr<browser::Bridge> bridge_;
    std::shared_ptr<privacy::Sanitizer> sanitizer_;
    std::unique_ptr<core::SnapshotManager> manager_;
    std::unique_ptr<ipc::Server> server_;

    std::unique_ptr<ipc::Listener> browser_listener_;
    std::thread browser_thread_;
    std::vector<std::thread> browser_workers_;
    std::mutex browser_mutex_;

    std::thread scheduler_thread_;
    std::mutex wake_mutex_;
    std::condition_variable wake_;
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> host_counter_{0};
    core::Timestamp last_capture_{};
};

}  // namespace contextsnap::daemon
