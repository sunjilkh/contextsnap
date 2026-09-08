// Chooses the HAL backend for this build/session. Keeping the decision in one
// translation unit means the daemon, CLI and tests all agree on it.
#include <contextsnap/hal/null_platform.hpp>
#include <contextsnap/hal/platform.hpp>

#include <contextsnap/core/logging.hpp>

#include <cstdlib>
#include <string>

namespace contextsnap::hal {

#if defined(_WIN32)
[[nodiscard]] Result<std::shared_ptr<Platform>> create_windows_platform();
#elif defined(__APPLE__)
[[nodiscard]] Result<std::shared_ptr<Platform>> create_macos_platform();
#elif defined(__linux__)
[[nodiscard]] Result<std::shared_ptr<Platform>> create_linux_platform();
#endif

namespace {

bool env_flag(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr) {
        return false;
    }
    const std::string text(value);
    return text == "1" || text == "true" || text == "yes";
}

}  // namespace

Result<std::shared_ptr<Platform>> Platform::create() {
    // CONTEXTSNAP_HEADLESS is what CI and `ctest` set: it keeps every capture
    // and restore path exercised without a display server.
    if (env_flag("CONTEXTSNAP_HEADLESS")) {
        core::log::info("using the headless null platform (CONTEXTSNAP_HEADLESS)");
        return create_null_platform(sample_null_state());
    }

#if defined(_WIN32)
    return create_windows_platform();
#elif defined(__APPLE__)
    return create_macos_platform();
#elif defined(__linux__)
    return create_linux_platform();
#else
    core::log::warn("unsupported platform; falling back to the null backend");
    return create_null_platform();
#endif
}

}  // namespace contextsnap::hal
