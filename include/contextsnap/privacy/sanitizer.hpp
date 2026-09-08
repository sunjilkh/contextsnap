// URL and credential sanitization.
//
// Everything that reaches persistent storage passes through here first. The
// rule set is declarative so users can extend it in config.toml without
// recompiling, and every rule is covered by tests/unit/test_sanitizer.cpp with
// real-world URL samples.
//
// Threat model: a snapshot database is a high-value target precisely because it
// aggregates a user's whole working context. Tokens embedded in URLs (OAuth
// callbacks, password reset links, pre-signed S3 URLs, Zoom join links) are the
// most common way secrets leak into such a store.
#pragma once

#include <contextsnap/core/result.hpp>
#include <contextsnap/core/types.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace contextsnap::privacy {

using core::Result;

enum class RuleAction : std::uint8_t {
    RemoveParameter,  ///< Drop the query parameter entirely.
    MaskParameter,    ///< Keep the key, replace the value with "[redacted]".
    TruncatePath,     ///< Keep scheme+host, drop the path (reset links).
    DropUrl,          ///< Store only the origin.
    ExcludeTab,       ///< Do not persist this tab at all.
};

struct Rule {
    std::string id;
    std::string description;
    RuleAction action{RuleAction::RemoveParameter};
    std::vector<std::string> parameters;     ///< Case-insensitive parameter names.
    std::vector<std::string> host_patterns;  ///< Glob patterns; empty == any host.
    std::vector<std::string> path_patterns;
    bool enabled{true};
};

struct SanitizeResult {
    std::string url;
    bool modified{false};
    bool excluded{false};  ///< Caller must drop the tab.
    std::vector<std::string> applied_rule_ids;
};

struct SanitizerOptions {
    bool strip_all_query_params{false};
    bool strip_fragments{true};
    bool strip_userinfo{true};      ///< https://user:pass@host -> https://host
    bool detect_high_entropy{true}; ///< Catch unknown token parameter names.
    double entropy_threshold{3.5};  ///< Shannon bits/char over base64-ish values.
    std::size_t entropy_min_length{20};
    std::vector<std::string> extra_sensitive_params;
    std::vector<std::string> domain_denylist;
    std::vector<std::string> domain_allowlist;
};

/// Parsed URL. Deliberately tolerant: browser-supplied URLs are already valid,
/// and session-file recovery can produce odd ones we must not crash on.
struct ParsedUrl {
    std::string scheme;
    std::string userinfo;
    std::string host;
    std::string port;
    std::string path;
    std::vector<std::pair<std::string, std::string>> query;
    std::string fragment;

    [[nodiscard]] std::string to_string() const;
    [[nodiscard]] std::string origin() const;
};

[[nodiscard]] Result<ParsedUrl> parse_url(std::string_view url);

/// Case-insensitive glob with `*` and `?`, used for hosts, paths and app ids.
[[nodiscard]] bool glob_match(std::string_view pattern, std::string_view text) noexcept;

/// Shannon entropy in bits per character.
[[nodiscard]] double shannon_entropy(std::string_view text) noexcept;

class Sanitizer {
public:
    explicit Sanitizer(SanitizerOptions options = {});

    /// The built-in rule set: OAuth/OIDC parameters, session ids, pre-signed
    /// storage URLs, password reset paths, meeting passcodes, ad trackers.
    [[nodiscard]] static std::vector<Rule> default_rules();

    void add_rule(Rule rule);
    void set_rules(std::vector<Rule> rules);
    [[nodiscard]] const std::vector<Rule>& rules() const noexcept { return rules_; }

    [[nodiscard]] SanitizeResult sanitize_url(std::string_view url) const;

    /// Applies sanitize_url() to every tab, dropping excluded ones, and clears
    /// window titles that leak content when the owning app is on the denylist.
    void sanitize_snapshot(core::Snapshot& snapshot) const;

    /// True when the host is denied outright (allowlist/denylist evaluation).
    [[nodiscard]] bool is_host_blocked(std::string_view host) const noexcept;

    [[nodiscard]] const SanitizerOptions& options() const noexcept { return options_; }

private:
    [[nodiscard]] bool looks_like_secret(std::string_view key, std::string_view value) const;

    SanitizerOptions options_;
    std::vector<Rule> rules_;
};

}  // namespace contextsnap::privacy
