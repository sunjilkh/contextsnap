// Frame encoding and request/response mapping. The same framing is used by the
// daemon socket and by the browser native messaging host, so a bug here breaks
// both transports at once.
#include "framework/microtest.hpp"

#include <contextsnap/ipc/protocol.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace ipc = contextsnap::ipc;
namespace core = contextsnap::core;
using contextsnap::core::json::Value;

namespace {

std::string as_text(const std::vector<std::uint8_t>& bytes) {
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

}  // namespace

TEST("frames carry a 4-byte little-endian length prefix") {
    auto frame = ipc::encode_frame("hello");
    CHECK_OK(frame);
    CHECK_EQ(frame.value().size(), std::size_t(9));
    CHECK_EQ(static_cast<int>(frame.value()[0]), 5);
    CHECK_EQ(static_cast<int>(frame.value()[1]), 0);
    CHECK_EQ(static_cast<int>(frame.value()[2]), 0);
    CHECK_EQ(static_cast<int>(frame.value()[3]), 0);
    CHECK_EQ(as_text(frame.value()).substr(4), std::string("hello"));
}

TEST("oversized payloads are rejected before allocation") {
    CHECK_ERR(ipc::encode_frame("0123456789", 4));
}

TEST("decode_frame waits for the complete payload") {
    auto frame = ipc::encode_frame("{\"a\":1}");
    CHECK_OK(frame);
    const std::string bytes = as_text(frame.value());

    auto partial = ipc::decode_frame(std::string_view(bytes).substr(0, 5));
    CHECK_OK(partial);
    CHECK_FALSE(partial.value().has_value());

    auto complete = ipc::decode_frame(bytes);
    CHECK_OK(complete);
    CHECK(complete.value().has_value());
    CHECK_EQ(complete.value()->payload, std::string("{\"a\":1}"));
    CHECK_EQ(complete.value()->consumed, bytes.size());
}

TEST("FrameReader reassembles frames split across reads") {
    auto first = ipc::encode_frame("one");
    auto second = ipc::encode_frame("two");
    CHECK_OK(first);
    CHECK_OK(second);
    const std::string stream = as_text(first.value()) + as_text(second.value());

    ipc::FrameReader reader;
    reader.feed(std::string_view(stream).substr(0, 5));
    auto pending = reader.next();
    CHECK_OK(pending);
    CHECK_FALSE(pending.value().has_value());

    reader.feed(std::string_view(stream).substr(5));
    auto one = reader.next();
    CHECK_OK(one);
    CHECK(one.value().has_value());
    CHECK_EQ(*one.value(), std::string("one"));

    auto two = reader.next();
    CHECK_OK(two);
    CHECK(two.value().has_value());
    CHECK_EQ(*two.value(), std::string("two"));

    auto empty = reader.next();
    CHECK_OK(empty);
    CHECK_FALSE(empty.value().has_value());
    CHECK_EQ(reader.buffered_bytes(), std::size_t(0));
}

TEST("method names survive a round-trip") {
    CHECK_EQ(ipc::method_from_string(ipc::to_string(ipc::Method::CaptureSnapshot)),
             ipc::Method::CaptureSnapshot);
    CHECK_EQ(ipc::method_from_string(ipc::to_string(ipc::Method::PlanRestore)),
             ipc::Method::PlanRestore);
    CHECK_EQ(ipc::method_from_string("not_a_method"), ipc::Method::Unknown);
}

TEST("requests serialise symmetrically") {
    ipc::Request request;
    request.id = 42;
    request.method = ipc::Method::GetSnapshot;
    request.params = Value::object();
    request.params.set("snapshot_id", Value(std::string("01J000000000000000000000AB")));

    auto decoded = ipc::Request::from_json(request.to_json());
    CHECK_OK(decoded);
    CHECK_EQ(decoded.value().id, std::uint64_t(42));
    CHECK_EQ(decoded.value().method, ipc::Method::GetSnapshot);
    CHECK_EQ(decoded.value().params.find("snapshot_id")->as_string(),
             std::string("01J000000000000000000000AB"));
}

TEST("responses preserve success and failure detail") {
    const ipc::Response ok = ipc::Response::success(42, Value(std::string("done")));
    auto ok_decoded = ipc::Response::from_json(ok.to_json());
    CHECK_OK(ok_decoded);
    CHECK(ok_decoded.value().ok);
    CHECK_EQ(ok_decoded.value().id, std::uint64_t(42));
    CHECK_EQ(ok_decoded.value().error_code, core::ErrorCode::Ok);

    const ipc::Response bad =
        ipc::Response::failure(43, core::err::not_found("no such snapshot", "test"));
    auto bad_decoded = ipc::Response::from_json(bad.to_json());
    CHECK_OK(bad_decoded);
    CHECK_FALSE(bad_decoded.value().ok);
    CHECK_EQ(bad_decoded.value().id, std::uint64_t(43));
    CHECK_EQ(bad_decoded.value().error_code, core::ErrorCode::NotFound);
    CHECK(bad_decoded.value().error_message.find("no such snapshot") != std::string::npos);
}

TEST("events round-trip and stay distinguishable from responses") {
    ipc::Event event;
    event.type = ipc::EventType::RestoreProgress;
    event.payload = Value::object();
    event.payload.set("percent", Value(static_cast<std::int64_t>(60)));

    const Value document = event.to_json();
    CHECK(document.contains("event"));

    auto decoded = ipc::Event::from_json(document);
    CHECK_OK(decoded);
    CHECK_EQ(decoded.value().type, ipc::EventType::RestoreProgress);
    CHECK_EQ(decoded.value().payload.find("percent")->as_int(), 60);
}

TEST("garbage frames are rejected as protocol errors") {
    auto decoded = ipc::Response::from_json(Value(std::string("not an object")));
    CHECK_ERR(decoded);
    CHECK_EQ(decoded.error().code, core::ErrorCode::ProtocolError);
}

MICROTEST_MAIN()
