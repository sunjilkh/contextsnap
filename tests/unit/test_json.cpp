// The in-tree JSON implementation is load-bearing: it encodes snapshots, the
// IPC frames and the native messaging protocol. These tests pin the behaviour
// the rest of the codebase relies on.
#include "framework/microtest.hpp"

#include <contextsnap/core/json.hpp>

#include <string>

using contextsnap::core::json::Value;
namespace json = contextsnap::core::json;

TEST("objects round-trip through parse and dump") {
    Value document = Value::object();
    document.set("name", Value(std::string("Deep work")));
    document.set("windows", Value(static_cast<std::int64_t>(7)));
    document.set("favorite", Value(true));

    json::Array tags;
    tags.emplace_back(Value(std::string("focus")));
    tags.emplace_back(Value(std::string("writing")));
    document.set("tags", Value(std::move(tags)));

    auto parsed = json::parse(document.dump());
    CHECK_OK(parsed);
    CHECK_EQ(parsed.value().find("name")->as_string(), std::string("Deep work"));
    CHECK_EQ(parsed.value().find("windows")->as_int(), 7);
    CHECK(parsed.value().find("favorite")->as_bool());
    CHECK_EQ(parsed.value().find("tags")->as_array().size(), std::size_t(2));
}

TEST("missing members are reported through find") {
    auto parsed = json::parse("{\"a\":1}");
    CHECK_OK(parsed);
    CHECK(parsed.value().find("a") != nullptr);
    CHECK(parsed.value().find("b") == nullptr);
    CHECK(parsed.value().contains("a"));
    CHECK_FALSE(parsed.value().contains("b"));
}

TEST("strings with control characters and unicode survive a round-trip") {
    const std::string awkward = "tab\there \"quoted\" \\ backslash \n newline \u2014 em dash";
    Value document = Value::object();
    document.set("text", Value(awkward));

    auto parsed = json::parse(document.dump());
    CHECK_OK(parsed);
    CHECK_EQ(parsed.value().find("text")->as_string(), awkward);
}

TEST("at_path walks nested documents") {
    auto parsed = json::parse(
        "{\"snapshot\":{\"windows\":[{\"title\":\"editor\"},{\"title\":\"browser\"}]}}");
    CHECK_OK(parsed);
    const Value* title = parsed.value().at_path("snapshot.windows.1.title");
    CHECK(title != nullptr);
    CHECK_EQ(title->as_string(), std::string("browser"));
    CHECK(parsed.value().at_path("snapshot.missing.0") == nullptr);
}

TEST("numbers keep integer precision") {
    auto parsed = json::parse("{\"handle\":140737488355328,\"scale\":1.5}");
    CHECK_OK(parsed);
    CHECK_EQ(parsed.value().find("handle")->as_int(), 140737488355328LL);
    CHECK_NEAR(parsed.value().find("scale")->as_double(), 1.5, 1e-9);
}

TEST("arrays accept mixed values and grow with push_back") {
    Value array = Value::array();
    array.push_back(Value(static_cast<std::int64_t>(1)));
    array.push_back(Value(std::string("two")));
    array.push_back(Value(false));
    CHECK_EQ(array.size(), std::size_t(3));

    auto parsed = json::parse(array.dump());
    CHECK_OK(parsed);
    CHECK(parsed.value().is_array());
    CHECK_EQ(parsed.value().as_array().size(), std::size_t(3));
    CHECK_EQ((parsed.value().as_array())[1].as_string(), std::string("two"));
}

TEST("malformed documents fail instead of throwing") {
    CHECK_ERR(json::parse("{\"unterminated\": "));
    CHECK_ERR(json::parse("[1, 2,]"));
    CHECK_ERR(json::parse(""));
    CHECK_ERR(json::parse("{'single':'quotes'}"));
}

TEST("accessors return defaults instead of throwing on type mismatch") {
    auto parsed = json::parse("{\"text\":\"hello\"}");
    CHECK_OK(parsed);
    const Value* text = parsed.value().find("text");
    CHECK(text != nullptr);
    CHECK(text->is_string());
    CHECK_EQ(text->as_int(), 0);
    CHECK_EQ(text->as_int(42), 42);
    CHECK_FALSE(text->as_bool());
}

TEST("pretty printing is stable and re-parsable") {
    Value document = Value::object();
    document.set("nested", Value::object());
    document.set("count", Value(static_cast<std::int64_t>(2)));

    const std::string pretty = document.dump(2);
    CHECK(pretty.find('\n') != std::string::npos);
    CHECK_OK(json::parse(pretty));
}

MICROTEST_MAIN()
