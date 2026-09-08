#include <contextsnap/privacy/sanitizer.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <unordered_map>

namespace contextsnap::privacy {
namespace {

std::string lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool contains_ci(const std::vector<std::string>& haystack, std::string_view needle) {
    const std::string lowered = lower(needle);
    return std::any_of(haystack.begin(), haystack.end(),
                       [&](const std::string& item) { return lower(item) == lowered; });
}

bool any_glob(const std::vector<std::string>& patterns, std::string_view text) {
    if (patterns.empty()) {
        return true;  // No constraint == matches everything.
    }
    return std::any_of(patterns.begin(), patterns.end(),
                       [&](const std::string& pattern) { return glob_match(pattern, text); });
}

}  // namespace

std::string ParsedUrl::to_string() const {
    std::string out;
    if (!scheme.empty()) {
        out += scheme + "://";
    }
    if (!userinfo.empty()) {
        out += userinfo + "@";
    }
    out += host;
    if (!port.empty()) {
        out += ":" + port;
    }
    out += path;
    for (std::size_t i = 0; i < query.size(); ++i) {
        out += (i == 0 ? "?" : "&") + query[i].first;
        if (!query[i].second.empty()) {
            out += "=" + query[i].second;
        }
    }
    if (!fragment.empty()) {
        out += "#" + fragment;
    }
    return out;
}

std::string ParsedUrl::origin() const {
    std::string out = scheme.empty() ? std::string() : scheme + "://";
    out += host;
    if (!port.empty()) {
        out += ":" + port;
    }
    return out;
}

Result<ParsedUrl> parse_url(std::string_view url) {
    if (url.empty()) {
        return core::err::invalid("empty url", "privacy.parse_url");
    }
    ParsedUrl parsed;
    std::string_view rest = url;

    const std::size_t scheme_end = rest.find("://");
    if (scheme_end != std::string_view::npos) {
        parsed.scheme = lower(rest.substr(0, scheme_end));
        rest.remove_prefix(scheme_end + 3);
    } else if (const std::size_t colon = rest.find(':');
               colon != std::string_view::npos && rest.substr(0, colon).find('/') == std::string_view::npos) {
        // Opaque schemes: about:blank, chrome://newtab handled above, mailto:.
        parsed.scheme = lower(rest.substr(0, colon));
        parsed.path = std::string(rest.substr(colon + 1));
        return parsed;
    } else {
        return core::err::invalid("url has no scheme", "privacy.parse_url");
    }

    const std::size_t authority_end = rest.find_first_of("/?#");
    std::string_view authority = rest.substr(0, authority_end);
    rest = authority_end == std::string_view::npos ? std::string_view{} : rest.substr(authority_end);

    if (const std::size_t at = authority.rfind('@'); at != std::string_view::npos) {
        parsed.userinfo = std::string(authority.substr(0, at));
        authority.remove_prefix(at + 1);
    }
    if (const std::size_t colon = authority.rfind(':');
        colon != std::string_view::npos && authority.find(']') == std::string_view::npos) {
        parsed.port = std::string(authority.substr(colon + 1));
        authority = authority.substr(0, colon);
    }
    parsed.host = lower(authority);

    if (const std::size_t hash = rest.find('#'); hash != std::string_view::npos) {
        parsed.fragment = std::string(rest.substr(hash + 1));
        rest = rest.substr(0, hash);
    }
    if (const std::size_t question = rest.find('?'); question != std::string_view::npos) {
        std::string_view query = rest.substr(question + 1);
        parsed.path = std::string(rest.substr(0, question));
        while (!query.empty()) {
            const std::size_t amp = query.find('&');
            std::string_view pair = query.substr(0, amp);
            const std::size_t equals = pair.find('=');
            if (equals == std::string_view::npos) {
                parsed.query.emplace_back(std::string(pair), std::string());
            } else {
                parsed.query.emplace_back(std::string(pair.substr(0, equals)),
                                          std::string(pair.substr(equals + 1)));
            }
            if (amp == std::string_view::npos) {
                break;
            }
            query.remove_prefix(amp + 1);
        }
    } else {
        parsed.path = std::string(rest);
    }
    return parsed;
}

bool glob_match(std::string_view pattern, std::string_view text) noexcept {
    // Iterative matcher with backtracking: O(n*m) worst case, no recursion, no
    // allocation — it runs once per tab per rule.
    std::size_t p = 0;
    std::size_t t = 0;
    std::size_t star = std::string_view::npos;
    std::size_t match = 0;
    const auto fold = [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    };

    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || fold(pattern[p]) == fold(text[t]))) {
            ++p;
            ++t;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            match = t;
        } else if (star != std::string_view::npos) {
            p = star + 1;
            t = ++match;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') {
        ++p;
    }
    return p == pattern.size();
}

double shannon_entropy(std::string_view text) noexcept {
    if (text.empty()) {
        return 0.0;
    }
    std::array<std::uint32_t, 256> counts{};
    for (const unsigned char c : text) {
        ++counts[c];
    }
    double entropy = 0.0;
    const double length = static_cast<double>(text.size());
    for (const std::uint32_t count : counts) {
        if (count == 0) {
            continue;
        }
        const double probability = static_cast<double>(count) / length;
        entropy -= probability * std::log2(probability);
    }
    return entropy;
}

Sanitizer::Sanitizer(SanitizerOptions options)
    : options_(std::move(options)), rules_(default_rules()) {}

std::vector<Rule> Sanitizer::default_rules() {
    return {
        Rule{"oauth", "OAuth/OIDC credentials in the query string", RuleAction::RemoveParameter,
             {"access_token", "id_token", "refresh_token", "code", "code_verifier", "client_secret",
              "state", "nonce", "assertion", "authuser"},
             {}, {}, true},
        Rule{"session", "Session and CSRF identifiers", RuleAction::RemoveParameter,
             {"session", "sessionid", "session_id", "sid", "sessid", "phpsessid", "jsessionid",
              "csrf", "csrf_token", "xsrf", "auth", "authorization", "api_key", "apikey", "token",
              "key", "password", "passwd", "pwd", "secret", "signature", "sig"},
             {}, {}, true},
        Rule{"presigned-storage", "Pre-signed object storage URLs", RuleAction::RemoveParameter,
             {"x-amz-signature", "x-amz-credential", "x-amz-security-token", "x-goog-signature",
              "sig", "se", "sp", "sv", "sr", "st", "skoid"},
             {"*.amazonaws.com", "*.blob.core.windows.net", "*.googleapis.com",
              "storage.googleapis.com", "*.r2.cloudflarestorage.com"},
             {}, true},
        Rule{"reset-links", "Password reset and magic-link paths", RuleAction::TruncatePath, {}, {},
             {"*/reset*", "*/verify*", "*/magic*", "*/invite/*", "*/confirm/*", "*/activate/*",
              "*/unsubscribe/*", "*/set-password*"},
             true},
        Rule{"meeting-passcodes", "Video call passcodes embedded in join links",
             RuleAction::RemoveParameter, {"pwd", "passcode", "password", "pw", "k", "tk"},
             {"*.zoom.us", "zoom.us", "teams.microsoft.com", "meet.google.com", "*.webex.com",
              "*.whereby.com"},
             {}, true},
        Rule{"trackers", "Marketing and analytics parameters", RuleAction::RemoveParameter,
             {"utm_source", "utm_medium", "utm_campaign", "utm_term", "utm_content", "utm_id",
              "gclid", "dclid", "fbclid", "msclkid", "mc_eid", "mc_cid", "igshid", "ttclid",
              "_hsenc", "_hsmi", "vero_id", "yclid", "twclid", "ref_src"},
             {}, {}, true},
        Rule{"local-files", "Local file paths reveal directory structure", RuleAction::DropUrl, {},
             {""}, {}, true},
        Rule{"private-browsing", "Never persist banking or password-manager tabs",
             RuleAction::ExcludeTab, {},
             {"*.bank*", "*vault*", "*.1password.com", "*.bitwarden.com", "*lastpass.com"}, {},
             true},
    };
}

void Sanitizer::add_rule(Rule rule) { rules_.push_back(std::move(rule)); }

void Sanitizer::set_rules(std::vector<Rule> rules) { rules_ = std::move(rules); }

bool Sanitizer::is_host_blocked(std::string_view host) const noexcept {
    if (!options_.domain_allowlist.empty()) {
        return !any_glob(options_.domain_allowlist, host);
    }
    return std::any_of(options_.domain_denylist.begin(), options_.domain_denylist.end(),
                       [&](const std::string& pattern) { return glob_match(pattern, host); });
}

bool Sanitizer::looks_like_secret(std::string_view key, std::string_view value) const {
    if (contains_ci(options_.extra_sensitive_params, key)) {
        return true;
    }
    if (!options_.detect_high_entropy || value.size() < options_.entropy_min_length) {
        return false;
    }
    // Long, high-entropy, mostly base64/hex values are almost always tokens.
    const bool token_shaped = std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isalnum(c) != 0 || c == '-' || c == '_' || c == '.' || c == '%' || c == '=';
    });
    return token_shaped && shannon_entropy(value) >= options_.entropy_threshold;
}

SanitizeResult Sanitizer::sanitize_url(std::string_view url) const {
    SanitizeResult result{std::string(url), false, false, {}};
    if (url.empty()) {
        return result;
    }

    auto parsed_result = parse_url(url);
    if (!parsed_result) {
        // Unparseable: keep it only if it carries no query string at all.
        result.excluded = url.find('?') != std::string_view::npos;
        result.modified = result.excluded;
        return result;
    }
    ParsedUrl parsed = std::move(parsed_result).value();

    if (parsed.scheme == "file") {
        result.url = "file:///[redacted]";
        result.modified = true;
        result.applied_rule_ids.emplace_back("local-files");
        return result;
    }
    if (is_host_blocked(parsed.host)) {
        result.excluded = true;
        result.modified = true;
        result.applied_rule_ids.emplace_back("domain-policy");
        return result;
    }

    if (options_.strip_userinfo && !parsed.userinfo.empty()) {
        parsed.userinfo.clear();
        result.modified = true;
        result.applied_rule_ids.emplace_back("userinfo");
    }
    if (options_.strip_fragments && !parsed.fragment.empty()) {
        parsed.fragment.clear();
        result.modified = true;
        result.applied_rule_ids.emplace_back("fragment");
    }
    if (options_.strip_all_query_params && !parsed.query.empty()) {
        parsed.query.clear();
        result.modified = true;
        result.applied_rule_ids.emplace_back("strip-all-query");
    }

    for (const Rule& rule : rules_) {
        if (!rule.enabled || rule.id == "local-files") {
            continue;
        }
        if (!rule.host_patterns.empty() && !any_glob(rule.host_patterns, parsed.host)) {
            continue;
        }
        if (!rule.path_patterns.empty() && !any_glob(rule.path_patterns, parsed.path)) {
            continue;
        }

        bool applied = false;
        switch (rule.action) {
            case RuleAction::ExcludeTab:
                result.excluded = true;
                result.modified = true;
                result.applied_rule_ids.push_back(rule.id);
                return result;
            case RuleAction::DropUrl:
                parsed.path.clear();
                parsed.query.clear();
                parsed.fragment.clear();
                applied = true;
                break;
            case RuleAction::TruncatePath:
                if (!parsed.path.empty() && parsed.path != "/") {
                    parsed.path = "/";
                    parsed.query.clear();
                    applied = true;
                }
                break;
            case RuleAction::RemoveParameter:
            case RuleAction::MaskParameter: {
                const std::size_t before = parsed.query.size();
                std::vector<std::pair<std::string, std::string>> kept;
                kept.reserve(before);
                for (auto& [key, value] : parsed.query) {
                    if (!contains_ci(rule.parameters, key)) {
                        kept.emplace_back(key, value);
                        continue;
                    }
                    if (rule.action == RuleAction::MaskParameter) {
                        kept.emplace_back(key, "[redacted]");
                    }
                    applied = true;
                }
                if (applied) {
                    parsed.query = std::move(kept);
                }
                break;
            }
        }
        if (applied) {
            result.modified = true;
            result.applied_rule_ids.push_back(rule.id);
        }
    }

    // Catch-all for parameter names we have never seen before.
    std::vector<std::pair<std::string, std::string>> kept;
    kept.reserve(parsed.query.size());
    for (auto& [key, value] : parsed.query) {
        if (looks_like_secret(key, value)) {
            result.modified = true;
            result.applied_rule_ids.emplace_back("entropy");
            continue;
        }
        kept.emplace_back(key, value);
    }
    parsed.query = std::move(kept);

    result.url = parsed.to_string();
    return result;
}

void Sanitizer::sanitize_snapshot(core::Snapshot& snapshot) const {
    for (core::WindowInfo& window : snapshot.windows) {
        std::vector<core::TabInfo> kept;
        kept.reserve(window.tabs.size());
        for (core::TabInfo& tab : window.tabs) {
            const SanitizeResult sanitized = sanitize_url(tab.url);
            if (sanitized.excluded) {
                continue;  // Dropped entirely: banking, vaults, denylisted hosts.
            }
            if (sanitized.modified) {
                tab.url = sanitized.url;
                tab.sanitized = true;
                // A title can restate the query string ("Reset password - token abc").
                if (tab.title.find("token") != std::string::npos ||
                    tab.title.find("password") != std::string::npos) {
                    tab.title = "[redacted]";
                }
            }
            kept.push_back(std::move(tab));
        }
        window.tabs = std::move(kept);

        // Re-index so restore keeps the original left-to-right ordering.
        for (std::size_t i = 0; i < window.tabs.size(); ++i) {
            window.tabs[i].index = static_cast<std::int32_t>(i);
        }
    }
}

}  // namespace contextsnap::privacy
