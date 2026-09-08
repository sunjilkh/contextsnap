// contextsnap — the command line client.
//
// Everything the CLI does goes through the daemon over IPC, so the binary stays
// tiny and there is exactly one implementation of capture/restore. `save`,
// `list` and friends start contextsnapd on demand (connect_or_spawn_daemon).
//
// Output rules: human output on stdout, diagnostics on stderr, `--json` prints
// a single machine-readable document and nothing else. Exit codes: 0 success,
// 1 runtime error, 2 usage error, 3 daemon unreachable.

#include <contextsnap/core/config.hpp>
#include <contextsnap/core/logging.hpp>
#include <contextsnap/core/snapshot_manager.hpp>
#include <contextsnap/ipc/client.hpp>
#include <contextsnap/storage/serialization.hpp>
#include <contextsnap/version.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <numeric>
#include <string>
#include <vector>

namespace {

using contextsnap::core::json::Value;

constexpr int kExitOk = 0;
constexpr int kExitError = 1;
constexpr int kExitUsage = 2;
constexpr int kExitNoDaemon = 3;

struct Args {
    std::string command;
    std::vector<std::string> positional;
    std::map<std::string, std::string> options;
    bool json{false};

    [[nodiscard]] bool has(const std::string& name) const {
        return options.find(name) != options.end();
    }
    [[nodiscard]] std::string value(const std::string& name,
                                    const std::string& fallback = {}) const {
        const auto found = options.find(name);
        return found == options.end() ? fallback : found->second;
    }
    [[nodiscard]] std::size_t number(const std::string& name, std::size_t fallback) const {
        const auto found = options.find(name);
        if (found == options.end() || found->second.empty()) {
            return fallback;
        }
        return static_cast<std::size_t>(std::strtoull(found->second.c_str(), nullptr, 10));
    }
};

/// Flags that never take a value; everything else consumes the next token
/// (or the text after `=`).
bool is_boolean_flag(const std::string& name) {
    static const std::vector<std::string> flags = {
        "json",     "no-tabs",  "dry-run", "latest", "tree",    "force",
        "favorite", "quiet",    "verbose", "help",   "version", "no-launch",
        "no-cursor", "no-lazy", "pretty"};
    return std::find(flags.begin(), flags.end(), name) != flags.end();
}

Args parse_args(int argc, char** argv) {
    Args args;
    for (int index = 1; index < argc; ++index) {
        std::string token = argv[index];
        if (token.rfind("--", 0) == 0) {
            token.erase(0, 2);
            std::string name = token;
            std::string inline_value;
            const std::size_t equals = token.find('=');
            if (equals != std::string::npos) {
                name = token.substr(0, equals);
                inline_value = token.substr(equals + 1);
            }
            if (is_boolean_flag(name)) {
                args.options[name] = inline_value.empty() ? "true" : inline_value;
            } else if (!inline_value.empty()) {
                args.options[name] = inline_value;
            } else if (index + 1 < argc) {
                args.options[name] = argv[++index];
            } else {
                args.options[name] = "";
            }
        } else if (args.command.empty()) {
            args.command = token;
        } else {
            args.positional.push_back(token);
        }
    }
    args.json = args.has("json");
    return args;
}

std::vector<std::string> split_list(const std::string& text) {
    std::vector<std::string> parts;
    std::string current;
    for (const char character : text) {
        if (character == ',') {
            if (!current.empty()) {
                parts.push_back(current);
                current.clear();
            }
        } else {
            current.push_back(character);
        }
    }
    if (!current.empty()) {
        parts.push_back(current);
    }
    return parts;
}

void print_usage() {
    std::cout
        << "contextsnap " << contextsnap::kVersion << " — snapshot and restore your "
        << "digital context\n\n"
        << "USAGE\n  contextsnap <command> [options]\n\n"
        << "COMMANDS\n"
        << "  save                  Capture the current desktop state\n"
        << "      --name <text>     Human readable name (default: generated)\n"
        << "      --tags a,b        Comma separated tags\n"
        << "      --no-tabs         Skip the browser round-trip\n"
        << "  list                  List stored snapshots\n"
        << "      --limit <n>       Rows to print (default 20)\n"
        << "      --tag <text>      Filter by tag\n"
        << "  show <id>             Print one snapshot\n"
        << "      --tree            Window/tab tree instead of a summary\n"
        << "  restore <id>          Restore a snapshot\n"
        << "      --latest          Restore the newest snapshot\n"
        << "      --only <sel>      app:code,tab:*.figma.com,window:<id>\n"
        << "      --dry-run         Print the plan without touching windows\n"
        << "      --no-launch       Do not start missing applications\n"
        << "      --no-cursor       Leave the pointer alone\n"
        << "  delete <id>           Remove a snapshot\n"
        << "  export <id>           Write a snapshot document\n"
        << "      --format json|flatbuffers   Output format (default json)\n"
        << "      --out <file>      Target file (default stdout)\n"
        << "  import <file>         Read a snapshot document\n"
        << "      --rename <text>   Store under a different name\n"
        << "  diff <a> <b>          Compare two snapshots\n"
        << "  daemon status         Show daemon health\n"
        << "  doctor                Check permissions and capabilities\n"
        << "  bench capture         Time repeated captures\n"
        << "      --iterations <n>  Repetitions (default 10)\n\n"
        << "GLOBAL OPTIONS\n"
        << "  --endpoint <path>     IPC endpoint (default from config)\n"
        << "  --daemon <path>       contextsnapd binary used for auto-start\n"
        << "  --json                Machine readable output\n"
        << "  --verbose             Debug logging on stderr\n"
        << "  --version, --help\n";
}

std::string format_local(contextsnap::core::Timestamp timestamp) {
    const std::time_t seconds =
        std::chrono::system_clock::to_time_t(std::chrono::time_point_cast<
            std::chrono::system_clock::duration>(timestamp));
    std::tm parts{};
#if defined(_WIN32)
    localtime_s(&parts, &seconds);
#else
    localtime_r(&seconds, &parts);
#endif
    std::ostringstream out;
    out << std::put_time(&parts, "%Y-%m-%d %H:%M");
    return out.str();
}

std::string join(const std::vector<std::string>& values, const char* separator) {
    std::string out;
    for (const std::string& value : values) {
        if (!out.empty()) {
            out += separator;
        }
        out += value;
    }
    return out;
}

void report(const contextsnap::core::Error& error) {
    std::cerr << "contextsnap: " << error.to_string() << "\n";
}

/// `--only app:code,tab:*.figma.com` -> selector list.
std::vector<contextsnap::core::RestoreSelector> parse_selectors(const std::string& text) {
    std::vector<contextsnap::core::RestoreSelector> selectors;
    for (const std::string& entry : split_list(text)) {
        auto parsed = contextsnap::core::parse_selector(entry);
        if (!parsed) {
            std::cerr << "contextsnap: ignoring selector '" << entry << "' ("
                      << parsed.error().message << ")\n";
            continue;
        }
        selectors.push_back(parsed.value());
    }
    return selectors;
}

contextsnap::core::RestoreOptions restore_options(const Args& args,
                                                  const contextsnap::core::Config& config) {
    contextsnap::core::RestoreOptions options = config.restore_options();
    if (args.has("only")) {
        options.selectors = parse_selectors(args.value("only"));
    }
    if (args.has("dry-run")) {
        options.dry_run = true;
    }
    if (args.has("no-launch")) {
        options.launch_missing_apps = false;
    }
    if (args.has("no-cursor")) {
        options.restore_cursor = false;
    }
    if (args.has("no-lazy")) {
        options.lazy_load_tabs = false;
    }
    return options;
}

int command_save(contextsnap::ipc::Client& client, const Args& args,
                 const contextsnap::core::Config& config) {
    contextsnap::core::CaptureOptions options = config.capture_options();
    options.name = args.value("name");
    options.tags = split_list(args.value("tags"));
    if (args.has("no-tabs")) {
        options.include_tabs = false;
    }

    auto snapshot = client.capture(options);
    if (!snapshot) {
        report(snapshot.error());
        return kExitError;
    }
    const contextsnap::core::Snapshot& value = snapshot.value();
    if (args.json) {
        std::cout << contextsnap::storage::to_json(value).dump(2) << "\n";
        return kExitOk;
    }
    std::cout << "Saved " << value.metadata.id << " \"" << value.metadata.name << "\"\n"
              << "  " << value.windows.size() << " windows, " << value.tab_count()
              << " tabs, " << value.monitors.size() << " monitors in "
              << value.metadata.capture_duration_ms << " ms\n";
    return kExitOk;
}

int command_list(contextsnap::ipc::Client& client, const Args& args) {
    auto snapshots = client.list(args.number("limit", 20), args.number("offset", 0),
                                 args.value("tag"));
    if (!snapshots) {
        report(snapshots.error());
        return kExitError;
    }
    if (args.json) {
        contextsnap::core::json::Array array;
        for (const auto& summary : snapshots.value()) {
            array.emplace_back(contextsnap::storage::to_json(summary));
        }
        std::cout << Value(std::move(array)).dump(2) << "\n";
        return kExitOk;
    }
    if (snapshots.value().empty()) {
        std::cout << "No snapshots yet. Run `contextsnap save`.\n";
        return kExitOk;
    }
    std::cout << std::left << std::setw(28) << "ID" << std::setw(17) << "CREATED"
              << std::setw(6) << "WIN" << std::setw(6) << "TABS" << "NAME\n";
    for (const auto& summary : snapshots.value()) {
        std::cout << std::left << std::setw(28) << summary.id << std::setw(17)
                  << format_local(summary.created_at) << std::setw(6) << summary.window_count
                  << std::setw(6) << summary.tab_count
                  << (summary.favorite ? "* " : "") << summary.name;
        if (!summary.tags.empty()) {
            std::cout << "  [" << join(summary.tags, ",") << "]";
        }
        std::cout << "\n";
    }
    return kExitOk;
}

int command_show(contextsnap::ipc::Client& client, const Args& args) {
    if (args.positional.empty()) {
        std::cerr << "contextsnap show: missing snapshot id\n";
        return kExitUsage;
    }
    auto snapshot = client.get(args.positional.front());
    if (!snapshot) {
        report(snapshot.error());
        return kExitError;
    }
    const contextsnap::core::Snapshot& value = snapshot.value();
    if (args.json) {
        std::cout << contextsnap::storage::to_json(value).dump(2) << "\n";
        return kExitOk;
    }
    std::cout << value.metadata.name << "  (" << value.metadata.id << ")\n"
              << "  captured " << format_local(value.metadata.created_at) << " on "
              << value.metadata.host_name << " — " << value.metadata.os_version << "\n"
              << "  " << value.monitors.size() << " monitors, " << value.windows.size()
              << " windows, " << value.tab_count() << " tabs\n";
    if (!args.has("tree")) {
        return kExitOk;
    }
    for (const auto& monitor : value.monitors) {
        std::cout << "  monitor " << monitor.id << " " << monitor.bounds.width << "x"
                  << monitor.bounds.height << " @" << monitor.dpi << "dpi"
                  << (monitor.primary ? " (primary)" : "") << "\n";
    }
    for (const auto& window : value.windows) {
        std::cout << "  window " << window.process.app_id << "  " << window.frame.width << "x"
                  << window.frame.height << "+" << window.frame.x << "+" << window.frame.y
                  << "  " << contextsnap::core::to_string(window.state) << "\n"
                  << "    " << window.title << "\n";
        for (const auto& tab : window.tabs) {
            std::cout << "      " << (tab.active ? "*" : " ") << (tab.pinned ? "p" : " ") << " "
                      << tab.url << "\n";
        }
    }
    return kExitOk;
}

int command_restore(contextsnap::ipc::Client& client, const Args& args,
                    const contextsnap::core::Config& config) {
    std::string id = args.positional.empty() ? "" : args.positional.front();
    if (args.has("latest")) {
        auto snapshots = client.list(1, 0);
        if (!snapshots) {
            report(snapshots.error());
            return kExitError;
        }
        if (snapshots.value().empty()) {
            std::cerr << "contextsnap restore: there are no snapshots\n";
            return kExitError;
        }
        id = snapshots.value().front().id;
    }
    if (id.empty()) {
        std::cerr << "contextsnap restore: missing snapshot id (or use --latest)\n";
        return kExitUsage;
    }

    const contextsnap::core::RestoreOptions options = restore_options(args, config);
    if (options.dry_run) {
        auto plan = client.plan_restore(id, options);
        if (!plan) {
            report(plan.error());
            return kExitError;
        }
        if (args.json) {
            std::cout << contextsnap::storage::to_json(plan.value()).dump(2) << "\n";
            return kExitOk;
        }
        std::cout << "Plan for " << plan.value().snapshot_id << " (~"
                  << plan.value().estimated_duration_ms << " ms)\n";
        int step_number = 1;
        for (const auto& step : plan.value().steps) {
            std::cout << "  " << step_number++ << ". " << step.description
                      << (step.skipped ? "  [skipped: " + step.skip_reason + "]" : "") << "\n";
        }
        for (const std::string& warning : plan.value().warnings) {
            std::cout << "  ! " << warning << "\n";
        }
        return kExitOk;
    }

    auto report_result = client.restore(id, options);
    if (!report_result) {
        report(report_result.error());
        return kExitError;
    }
    if (args.json) {
        std::cout << contextsnap::storage::to_json(report_result.value()).dump(2) << "\n";
        return kExitOk;
    }
    const auto& result = report_result.value();
    std::cout << "Restored " << result.windows_restored << " windows, " << result.tabs_restored
              << " tabs, launched " << result.apps_launched << " apps in " << result.duration_ms
              << " ms\n";
    for (const std::string& warning : result.warnings) {
        std::cout << "  ! " << warning << "\n";
    }
    for (const std::string& error : result.errors) {
        std::cerr << "  x " << error << "\n";
    }
    return result.fully_successful() ? kExitOk : kExitError;
}

int command_delete(contextsnap::ipc::Client& client, const Args& args) {
    if (args.positional.empty()) {
        std::cerr << "contextsnap delete: missing snapshot id\n";
        return kExitUsage;
    }
    for (const std::string& id : args.positional) {
        if (const auto status = client.remove(id); !status) {
            report(status.error());
            return kExitError;
        }
        std::cout << "Deleted " << id << "\n";
    }
    return kExitOk;
}

int command_export(contextsnap::ipc::Client& client, const Args& args) {
    if (args.positional.empty()) {
        std::cerr << "contextsnap export: missing snapshot id\n";
        return kExitUsage;
    }
    Value params = Value::object();
    params.set("snapshot_id", Value(args.positional.front()));
    params.set("format", Value(args.value("format", "json")));
    auto result = client.call(contextsnap::ipc::Method::ExportSnapshot, std::move(params));
    if (!result) {
        report(result.error());
        return kExitError;
    }
    const Value* document = result.value().find("document");
    const std::string payload = document == nullptr ? result.value().dump(2)
                                                    : document->as_string();
    const std::string out = args.value("out");
    if (out.empty()) {
        std::cout << payload << "\n";
        return kExitOk;
    }
    std::ofstream file(out, std::ios::binary);
    if (!file) {
        std::cerr << "contextsnap export: cannot write " << out << "\n";
        return kExitError;
    }
    file << payload;
    std::cout << "Wrote " << out << " (" << payload.size() << " bytes)\n";
    return kExitOk;
}

int command_import(contextsnap::ipc::Client& client, const Args& args) {
    if (args.positional.empty()) {
        std::cerr << "contextsnap import: missing file\n";
        return kExitUsage;
    }
    std::ifstream file(args.positional.front(), std::ios::binary);
    if (!file) {
        std::cerr << "contextsnap import: cannot read " << args.positional.front() << "\n";
        return kExitError;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();

    Value params = Value::object();
    params.set("document", Value(buffer.str()));
    params.set("format", Value(args.value("format", "json")));
    if (args.has("rename")) {
        params.set("rename_to", Value(args.value("rename")));
    }
    auto result = client.call(contextsnap::ipc::Method::ImportSnapshot, std::move(params));
    if (!result) {
        report(result.error());
        return kExitError;
    }
    const Value* id = result.value().find("snapshot_id");
    std::cout << "Imported " << (id == nullptr ? "snapshot" : id->as_string()) << "\n";
    return kExitOk;
}

/// Structural diff: apps and tab hosts added/removed between two snapshots.
int command_diff(contextsnap::ipc::Client& client, const Args& args) {
    if (args.positional.size() < 2) {
        std::cerr << "contextsnap diff: needs two snapshot ids\n";
        return kExitUsage;
    }
    auto left = client.get(args.positional[0]);
    auto right = client.get(args.positional[1]);
    if (!left) {
        report(left.error());
        return kExitError;
    }
    if (!right) {
        report(right.error());
        return kExitError;
    }

    const auto app_ids = [](const contextsnap::core::Snapshot& snapshot) {
        std::vector<std::string> ids;
        for (const auto& window : snapshot.windows) {
            if (std::find(ids.begin(), ids.end(), window.process.app_id) == ids.end()) {
                ids.push_back(window.process.app_id);
            }
        }
        std::sort(ids.begin(), ids.end());
        return ids;
    };
    const std::vector<std::string> before = app_ids(left.value());
    const std::vector<std::string> after = app_ids(right.value());

    std::vector<std::string> added;
    std::vector<std::string> removed;
    std::set_difference(after.begin(), after.end(), before.begin(), before.end(),
                        std::back_inserter(added));
    std::set_difference(before.begin(), before.end(), after.begin(), after.end(),
                        std::back_inserter(removed));

    std::cout << left.value().metadata.name << " -> " << right.value().metadata.name << "\n"
              << "  windows " << left.value().windows.size() << " -> "
              << right.value().windows.size() << "\n"
              << "  tabs    " << left.value().tab_count() << " -> "
              << right.value().tab_count() << "\n";
    for (const std::string& app : added) {
        std::cout << "  + " << app << "\n";
    }
    for (const std::string& app : removed) {
        std::cout << "  - " << app << "\n";
    }
    return kExitOk;
}

int command_health(contextsnap::ipc::Client& client, const Args& args, bool doctor) {
    auto health = client.health();
    if (!health) {
        report(health.error());
        return kExitNoDaemon;
    }
    if (args.json) {
        std::cout << health.value().dump(2) << "\n";
        return kExitOk;
    }
    const Value& value = health.value();
    const auto text = [&](const char* key) {
        const Value* found = value.find(key);
        return found == nullptr ? std::string{} : found->as_string();
    };
    const auto flag = [&](const char* key) {
        const Value* found = value.find(key);
        return found != nullptr && found->as_bool();
    };

    std::cout << "contextsnapd " << text("version") << " on " << text("platform") << " ("
              << text("session_type") << ")\n"
              << "  endpoint       " << text("endpoint") << "\n"
              << "  database       " << (flag("database_ok") ? "ok" : "FAILED") << " ("
              << (value.find("snapshot_count") == nullptr
                      ? 0
                      : value.find("snapshot_count")->as_int())
              << " snapshots, "
              << (value.find("database_bytes") == nullptr
                      ? 0
                      : value.find("database_bytes")->as_int() / 1024)
              << " KiB)\n"
              << "  browser bridge "
              << (flag("browser_bridge_connected") ? "connected" : "not connected") << "\n";

    if (const Value* missing = value.find("missing_permissions");
        missing != nullptr && missing->is_array()) {
        for (const Value& entry : missing->as_array()) {
            std::cout << "  ! missing permission: " << entry.as_string() << "\n";
        }
    }
    if (const Value* warnings = value.find("warnings");
        warnings != nullptr && warnings->is_array()) {
        for (const Value& entry : warnings->as_array()) {
            std::cout << "  ! " << entry.as_string() << "\n";
        }
    }
    if (doctor) {
        if (const Value* limitations = value.find("limitations");
            limitations != nullptr && limitations->is_array()) {
            for (const Value& entry : limitations->as_array()) {
                std::cout << "  - " << entry.as_string() << "\n";
            }
        }
        std::cout << "\nRun `contextsnap save --no-tabs` to test capture without the browser.\n";
    }
    return flag("database_ok") ? kExitOk : kExitError;
}

int command_bench(contextsnap::ipc::Client& client, const Args& args,
                  const contextsnap::core::Config& config) {
    const std::size_t iterations = args.number("iterations", 10);
    contextsnap::core::CaptureOptions options = config.capture_options();
    options.include_tabs = !args.has("no-tabs");
    options.automatic = true;
    options.name = "benchmark";

    std::vector<double> samples;
    samples.reserve(iterations);
    for (std::size_t index = 0; index < iterations; ++index) {
        const auto started = std::chrono::steady_clock::now();
        auto snapshot = client.capture(options);
        if (!snapshot) {
            report(snapshot.error());
            return kExitError;
        }
        const auto elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started);
        samples.push_back(elapsed.count());
        if (const auto status = client.remove(snapshot.value().metadata.id); !status) {
            report(status.error());
        }
    }
    std::sort(samples.begin(), samples.end());
    const double total = std::accumulate(samples.begin(), samples.end(), 0.0);
    std::cout << std::fixed << std::setprecision(1) << "capture x" << iterations
              << "  min " << samples.front() << " ms  p50 " << samples[samples.size() / 2]
              << " ms  max " << samples.back() << " ms  mean "
              << total / static_cast<double>(samples.size()) << " ms\n";
    return kExitOk;
}

}  // namespace

int main(int argc, char** argv) {
    const Args args = parse_args(argc, argv);
    if (args.has("help") || args.command.empty() || args.command == "help") {
        print_usage();
        return args.command.empty() && !args.has("help") ? kExitUsage : kExitOk;
    }
    if (args.has("version") || args.command == "version") {
        std::cout << contextsnap::version_string() << "\n";
        return kExitOk;
    }
    contextsnap::core::log::set_level(args.has("verbose") ? contextsnap::core::log::Level::Debug
                                                          : contextsnap::core::log::Level::Warn);

    auto config_result = contextsnap::core::Config::load();
    const contextsnap::core::Config config =
        config_result ? config_result.value() : contextsnap::core::Config::with_defaults();

    const std::string endpoint = args.value("endpoint", config.ipc.socket_path);
    const std::string daemon_path = args.value("daemon", "contextsnapd");

    // Every command needs the daemon, so start it once here.
    auto client_result = contextsnap::ipc::connect_or_spawn_daemon(endpoint, daemon_path);
    if (!client_result) {
        std::cerr << "contextsnap: cannot reach contextsnapd at " << endpoint << "\n  "
                  << client_result.error().to_string() << "\n"
                  << "  start it manually with `contextsnapd --foreground`\n";
        return kExitNoDaemon;
    }
    contextsnap::ipc::Client& client = *client_result.value();

    const std::string& command = args.command;
    if (command == "save" || command == "capture") {
        return command_save(client, args, config);
    }
    if (command == "list" || command == "ls") {
        return command_list(client, args);
    }
    if (command == "show" || command == "inspect") {
        return command_show(client, args);
    }
    if (command == "restore") {
        return command_restore(client, args, config);
    }
    if (command == "delete" || command == "rm") {
        return command_delete(client, args);
    }
    if (command == "export") {
        return command_export(client, args);
    }
    if (command == "import") {
        return command_import(client, args);
    }
    if (command == "diff") {
        return command_diff(client, args);
    }
    if (command == "daemon") {
        if (!args.positional.empty() && args.positional.front() != "status") {
            std::cerr << "contextsnap daemon: only `status` is supported\n";
            return kExitUsage;
        }
        return command_health(client, args, false);
    }
    if (command == "doctor") {
        return command_health(client, args, true);
    }
    if (command == "bench") {
        return command_bench(client, args, config);
    }

    std::cerr << "contextsnap: unknown command \"" << command << "\"\n";
    print_usage();
    return kExitUsage;
}
