// Privacy is a correctness property here: a snapshot that leaks an OAuth token
// into a local database is a bug, not a nuisance.
#include "framework/microtest.hpp"

#include <contextsnap/privacy/sanitizer.hpp>

#include <string>

namespace privacy = contextsnap::privacy;

TEST("URLs are parsed into their components") {
    auto parsed = privacy::parse_url("https://user:secret@app.example.com:8443/a/b?x=1#frag");
    CHECK_OK(parsed);
    CHECK_EQ(parsed.value().scheme, std::string("https"));
    CHECK_EQ(parsed.value().host, std::string("app.example.com"));
    CHECK(parsed.value().path.find("/a/b") == std::size_t(0));
}

TEST("glob matching handles hosts and paths") {
    CHECK(privacy::glob_match("*.example.com", "docs.example.com"));
    CHECK_FALSE(privacy::glob_match("*.example.com", "example.com"));
    CHECK(privacy::glob_match("https://mail.*", "https://mail.google.com/u/0"));
    CHECK(privacy::glob_match("*", "anything"));
}

TEST("shannon entropy separates words from secrets") {
    const double word = privacy::shannon_entropy("documentation");
    const double secret = privacy::shannon_entropy("eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9");
    CHECK(secret > word);
}

TEST("OAuth material is stripped from query strings") {
    privacy::Sanitizer sanitizer;
    const privacy::SanitizeResult result = sanitizer.sanitize_url(
        "https://app.example.com/callback?code=abc123&access_token=ya29.a0AfB_byC&state=xyz");

    CHECK(result.modified);
    CHECK(result.url.find("access_token") == std::string::npos);
    CHECK(result.url.find("ya29.a0AfB_byC") == std::string::npos);
    CHECK_FALSE(result.applied_rule_ids.empty());
}

TEST("session identifiers never reach disk") {
    privacy::Sanitizer sanitizer;
    const privacy::SanitizeResult result =
        sanitizer.sanitize_url("https://intranet.example.com/dashboard?PHPSESSID=9f8a7b6c5d4e3f2a1b");
    CHECK(result.modified);
    CHECK(result.url.find("9f8a7b6c5d4e3f2a1b") == std::string::npos);
}

TEST("tracking parameters are removed without touching the page") {
    privacy::Sanitizer sanitizer;
    const privacy::SanitizeResult result = sanitizer.sanitize_url(
        "https://blog.example.com/posts/local-first?utm_source=hn&utm_medium=social&page=2");
    CHECK(result.modified);
    CHECK(result.url.find("utm_source") == std::string::npos);
    CHECK(result.url.find("utm_medium") == std::string::npos);
    CHECK(result.url.find("/posts/local-first") != std::string::npos);
    CHECK(result.url.find("page=2") != std::string::npos);
}

TEST("pre-signed storage links are treated as credentials") {
    privacy::Sanitizer sanitizer;
    const privacy::SanitizeResult result = sanitizer.sanitize_url(
        "https://bucket.s3.amazonaws.com/report.pdf?X-Amz-Credential=AKIAIOSFODNN7EXAMPLE"
        "&X-Amz-Signature=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    CHECK(result.modified);
    CHECK(result.url.find("X-Amz-Signature") == std::string::npos);
}

TEST("embedded credentials in the authority are dropped") {
    privacy::Sanitizer sanitizer;
    const privacy::SanitizeResult result =
        sanitizer.sanitize_url("https://admin:hunter2@router.local/status");
    CHECK(result.modified);
    CHECK(result.url.find("hunter2") == std::string::npos);
}

TEST("ordinary URLs are left byte-for-byte identical") {
    privacy::Sanitizer sanitizer;
    const std::string url = "https://en.wikipedia.org/wiki/Local-first_software";
    const privacy::SanitizeResult result = sanitizer.sanitize_url(url);
    CHECK_FALSE(result.modified);
    CHECK_EQ(result.url, url);
}

TEST("denylisted domains drop the tab entirely") {
    privacy::SanitizerOptions options;
    options.domain_denylist.push_back("*.bank.example");
    privacy::Sanitizer sanitizer(options);

    CHECK(sanitizer.is_host_blocked("login.bank.example"));
    CHECK_FALSE(sanitizer.is_host_blocked("example.com"));

    const privacy::SanitizeResult result =
        sanitizer.sanitize_url("https://login.bank.example/accounts");
    CHECK(result.excluded);
}

MICROTEST_MAIN()
