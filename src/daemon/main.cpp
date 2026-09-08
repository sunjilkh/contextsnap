// contextsnapd entry point: argument parsing, logging setup, signal handling.
//
// The daemon is deliberately boring: it never forks by default (service
// managers prefer foreground processes), and every long-lived resource lives in
// daemon::Daemon so shutdown is a single stop() call.

#include "daemon.hpp"

#include <contextsnap/core/logging.hpp>
#include <contextsnap/version.hpp>

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace {

std::atomic<contextsnap::daemon::Daemon*> g_daemon{nullptr};

extern "C" void handle_signal(int signal_number) {
    // Only async-signal-safe work here: flip the flag and let run() return.
    if (contextsnap::daemon::Daemon* daemon = g_daemon.load()) {
        daemon->stop();
    }
    (void)signal_number;
}

void print_usage() {
    std::cout << "contextsnapd " << contextsnap::kVersion << " \u2014 ContextSnap daemon\n\n"
              << "USAGE\n  contextsnapd [options]\n\n"
              << "OPTIONS\n"
              << "  --foreground        Stay attached to the terminal (default)\n"
              << "  --detached          Detach from the controlling terminal (POSIX)\n"
              << "  --endpoint <path>   IPC socket/pipe (default from config)\n"
              << "  --config <file>     Alternate config.toml\n"
              << "  --database <file>   Alternate SQLite file\n"
              << "  --log-level <lvl>   trace|debug|info|warn|error|off\n"
              << "  --log-file <file>   Write logs to a rotating file\n"
              << "  --no-scheduler      Disable periodic/idle captures\n"
              << "  --headless          Force the null window backend\n"
              << "  --version, --help\n";
}

std::string take_value(int argc, char** argv, int& index, const char* flag) {
    if (index + 1 >= argc) {
        std::cerr << "contextsnapd: " << flag << " needs a value\n";
        std::exit(2);
    }
    return argv[++index];
}

}  // namespace

int main(int argc, char** argv) {
    contextsnap::daemon::DaemonOptions options;
    std::string config_path;
    std::string database_override;
    std::string log_level;
    std::string log_file;
    bool detach = false;

    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--help" || argument == "-h") {
            print_usage();
            return 0;
        }
        if (argument == "--version") {
            std::cout << contextsnap::version_string() << "\n";
            return 0;
        }
        if (argument == "--foreground") {
            detach = false;
        } else if (argument == "--detached" || argument == "--daemonize") {
            detach = true;
        } else if (argument == "--endpoint") {
            options.endpoint = take_value(argc, argv, index, "--endpoint");
        } else if (argument == "--browser-endpoint") {
            options.browser_endpoint = take_value(argc, argv, index, "--browser-endpoint");
        } else if (argument == "--config") {
            config_path = take_value(argc, argv, index, "--config");
        } else if (argument == "--database") {
            database_override = take_value(argc, argv, index, "--database");
        } else if (argument == "--log-level") {
            log_level = take_value(argc, argv, index, "--log-level");
        } else if (argument == "--log-file") {
            log_file = take_value(argc, argv, index, "--log-file");
        } else if (argument == "--no-scheduler") {
            options.scheduler = false;
        } else if (argument == "--headless") {
            options.headless = true;
        } else {
            std::cerr << "contextsnapd: unknown option " << argument << "\n";
            print_usage();
            return 2;
        }
    }

    auto loaded = contextsnap::core::Config::load(config_path);
    if (!loaded) {
        std::cerr << "contextsnapd: " << loaded.error().to_string() << "\n";
        return 1;
    }
    options.config = loaded.value();
    if (!database_override.empty()) {
        options.config.storage.database_path = database_override;
    }
    if (!log_level.empty()) {
        options.config.log_level = contextsnap::core::log::level_from_string(log_level);
    }
    if (!log_file.empty()) {
        options.config.log_file = log_file;
    }
    if (options.headless) {
#if defined(_WIN32)
        _putenv_s("CONTEXTSNAP_HEADLESS", "1");
#else
        ::setenv("CONTEXTSNAP_HEADLESS", "1", 1);
#endif
    }
    if (const auto valid = options.config.validate(); !valid) {
        std::cerr << "contextsnapd: " << valid.error().to_string() << "\n";
        return 1;
    }

    contextsnap::core::log::set_level(options.config.log_level);
    if (!options.config.log_file.empty()) {
        contextsnap::core::log::set_log_file(options.config.log_file);
    }
    contextsnap::core::log::add_global_field(
        contextsnap::core::log::field("component", std::string("daemon")));

#if !defined(_WIN32)
    if (detach) {
        // Classic double-fork so the daemon survives its parent shell.
        const pid_t first = ::fork();
        if (first < 0) {
            std::cerr << "contextsnapd: fork failed\n";
            return 1;
        }
        if (first > 0) {
            return 0;
        }
        ::setsid();
        const pid_t second = ::fork();
        if (second < 0) {
            return 1;
        }
        if (second > 0) {
            ::_exit(0);
        }
        ::close(STDIN_FILENO);
    }
    std::signal(SIGPIPE, SIG_IGN);  // Writes to dead clients must not kill us.
    std::signal(SIGHUP, SIG_IGN);
#else
    if (detach) {
        contextsnap::core::log::debug("--detached is a no-op on Windows");
    }
#endif

    contextsnap::daemon::Daemon daemon(options);
    g_daemon.store(&daemon);
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    if (const auto status = daemon.start(); !status) {
        std::cerr << "contextsnapd: " << status.error().to_string() << "\n";
        g_daemon.store(nullptr);
        return 1;
    }
    daemon.run();
    g_daemon.store(nullptr);
    return 0;
}
