// contextsnap-native-host: the browser-facing half of the tab pipeline.
//
// Browsers start this binary themselves (Chrome/Edge via the native messaging
// manifest, Firefox via the same mechanism) and speak the length-prefixed JSON
// protocol over stdio. The host does not touch the database: it relays frames
// to contextsnapd over the local socket, which keeps the trusted code in one
// place and lets the browser process stay unprivileged.
//
//   browser  <-- stdio (4-byte LE length + JSON) -->  contextsnap-native-host
//   contextsnap-native-host  <-- unix socket -->  contextsnapd

#include <contextsnap/browser/native_host.hpp>
#include <contextsnap/core/config.hpp>
#include <contextsnap/core/logging.hpp>
#include <contextsnap/version.hpp>

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

void print_usage() {
    std::cout << "contextsnap-native-host " << contextsnap::kVersion << "\n\n"
              << "Started automatically by the browser. Manual options:\n"
              << "  --endpoint <path>     contextsnapd socket (default: config value)\n"
              << "  --print-manifest      write the native messaging manifest to stdout\n"
              << "  --host-path <path>    absolute path recorded in the manifest\n"
              << "  --extension <id>      allowed extension id (repeatable, Chromium)\n"
              << "  --origin <url>        allowed origin (repeatable, Chromium)\n"
              << "  --verbose             log frames to stderr\n"
              << "  --version, --help\n";
}

std::string next_argument(int argc, char** argv, int& index, const char* flag) {
    if (index + 1 >= argc) {
        std::cerr << "contextsnap-native-host: " << flag << " needs a value\n";
        std::exit(2);
    }
    return argv[++index];
}

}  // namespace

int main(int argc, char** argv) {
    contextsnap::browser::NativeHostOptions options;
    // The daemon keeps a second listener for browser hosts so that native
    // messaging frames never mix with CLI/GUI request frames.
    options.daemon_endpoint =
        contextsnap::core::Config::with_defaults().ipc.socket_path + "-browser";

    bool print_manifest = false;
    std::string host_path = argv[0] == nullptr ? "" : argv[0];
    std::vector<std::string> extensions;
    std::vector<std::string> origins;

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
        if (argument == "--endpoint") {
            options.daemon_endpoint = next_argument(argc, argv, index, "--endpoint");
        } else if (argument == "--print-manifest") {
            print_manifest = true;
        } else if (argument == "--host-path") {
            host_path = next_argument(argc, argv, index, "--host-path");
        } else if (argument == "--extension") {
            extensions.push_back(next_argument(argc, argv, index, "--extension"));
        } else if (argument == "--origin") {
            origins.push_back(next_argument(argc, argv, index, "--origin"));
        } else if (argument == "--verbose") {
            options.verbose = true;
        } else {
            std::cerr << "contextsnap-native-host: unknown option " << argument << "\n";
            return 2;
        }
    }

    if (print_manifest) {
        // Chromium wants chrome-extension:// origins, Firefox wants bare ids;
        // emitting both keys keeps one manifest usable for the installers.
        if (origins.empty()) {
            origins.emplace_back("chrome-extension://REPLACE_WITH_EXTENSION_ID/");
        }
        if (extensions.empty()) {
            extensions.emplace_back("contextsnap@contextsnap.dev");
        }
        std::cout << contextsnap::browser::build_host_manifest(host_path, origins, extensions)
                  << "\n";
        return 0;
    }

    // stdout belongs to the browser protocol from here on: never write logs to it.
    contextsnap::core::log::set_level(options.verbose ? contextsnap::core::log::Level::Debug
                                                      : contextsnap::core::log::Level::Warn);
    contextsnap::core::log::add_global_field(
        contextsnap::core::log::field("component", std::string("native-host")));
    return contextsnap::browser::run_native_host(options);
}
