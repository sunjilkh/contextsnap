#include <cstdlib>
#include <contextsnap/core/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace contextsnap::core::json {
namespace {

void append_utf8(std::string& out, std::uint32_t code_point) {
    if (code_point <= 0x7F) {
        out.push_back(static_cast<char>(code_point));
    } else if (code_point <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0u | (code_point >> 6)));
        out.push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
    } else if (code_point <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0u | (code_point >> 12)));
        out.push_back(static_cast<char>(0x80u | ((code_point >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
    } else {
        out.push_back(static_cast<char>(0xF0u | (code_point >> 18)));
        out.push_back(static_cast<char>(0x80u | ((code_point >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((code_point >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
    }
}

std::string format_double(double value) {
    if (std::isnan(value) || std::isinf(value)) {
        return "null";  // RFC 8259 has no representation for these.
    }
    char buffer[40];
    // 17 significant digits round-trip an IEEE-754 double exactly.
    const int written = std::snprintf(buffer, sizeof(buffer), "%.17g", value);
    if (written <= 0) {
        return "0";
    }
    std::string text(buffer, static_cast<std::size_t>(written));
    // Prefer the shortest representation that still round-trips.
    for (int precision = 1; precision < 17; ++precision) {
        char candidate[40];
        const int len = std::snprintf(candidate, sizeof(candidate), "%.*g", precision, value);
        if (len > 0 && std::strtod(candidate, nullptr) == value) {
            text.assign(candidate, static_cast<std::size_t>(len));
            break;
        }
    }
    return text;
}

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    Result<Value> parse() {
        skip_whitespace();
        auto value = parse_value(0);
        if (!value) {
            return value;
        }
        skip_whitespace();
        if (pos_ != text_.size()) {
            return fail("trailing characters after top-level value");
        }
        return value;
    }

private:
    static constexpr int kMaxDepth = 128;  // Guards against stack exhaustion.

    Error fail(std::string message) const {
        return err::invalid(message + " at offset " + std::to_string(pos_), "json.parse");
    }

    void skip_whitespace() {
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    bool consume(char expected) {
        if (pos_ < text_.size() && text_[pos_] == expected) {
            ++pos_;
            return true;
        }
        return false;
    }

    bool starts_with(std::string_view literal) const {
        return text_.compare(pos_, literal.size(), literal) == 0;
    }

    Result<Value> parse_value(int depth) {
        if (depth > kMaxDepth) {
            return fail("maximum nesting depth exceeded");
        }
        skip_whitespace();
        if (pos_ >= text_.size()) {
            return fail("unexpected end of input");
        }
        switch (text_[pos_]) {
            case '{':
                return parse_object(depth);
            case '[':
                return parse_array(depth);
            case '"': {
                auto text = parse_string();
                if (!text) {
                    return Result<Value>(text.error());
                }
                return Value(text.value());
            }
            case 't':
                if (starts_with("true")) {
                    pos_ += 4;
                    return Value(true);
                }
                return fail("invalid literal");
            case 'f':
                if (starts_with("false")) {
                    pos_ += 5;
                    return Value(false);
                }
                return fail("invalid literal");
            case 'n':
                if (starts_with("null")) {
                    pos_ += 4;
                    return Value();
                }
                return fail("invalid literal");
            default:
                return parse_number();
        }
    }

    Result<Value> parse_object(int depth) {
        ++pos_;  // '{'
        Object object;
        skip_whitespace();
        if (consume('}')) {
            return Value(std::move(object));
        }
        while (true) {
            skip_whitespace();
            if (pos_ >= text_.size() || text_[pos_] != '"') {
                return fail("expected object key");
            }
            auto key = parse_string();
            if (!key) {
                return Result<Value>(key.error());
            }
            skip_whitespace();
            if (!consume(':')) {
                return fail("expected ':' after object key");
            }
            auto value = parse_value(depth + 1);
            if (!value) {
                return value;
            }
            object.emplace_back(key.value(), value.value());
            skip_whitespace();
            if (consume(',')) {
                continue;
            }
            if (consume('}')) {
                break;
            }
            return fail("expected ',' or '}' in object");
        }
        return Value(std::move(object));
    }

    Result<Value> parse_array(int depth) {
        ++pos_;  // '['
        Array array;
        skip_whitespace();
        if (consume(']')) {
            return Value(std::move(array));
        }
        while (true) {
            auto value = parse_value(depth + 1);
            if (!value) {
                return value;
            }
            array.push_back(value.value());
            skip_whitespace();
            if (consume(',')) {
                continue;
            }
            if (consume(']')) {
                break;
            }
            return fail("expected ',' or ']' in array");
        }
        return Value(std::move(array));
    }

    Result<std::string> parse_string() {
        ++pos_;  // opening quote
        std::string out;
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c == '"') {
                ++pos_;
                return out;
            }
            if (c == '\\') {
                ++pos_;
                if (pos_ >= text_.size()) {
                    return fail("unterminated escape sequence");
                }
                const char escape = text_[pos_++];
                switch (escape) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        auto code = parse_hex4();
                        if (!code) {
                            return Result<std::string>(code.error());
                        }
                        std::uint32_t code_point = code.value();
                        // Combine UTF-16 surrogate pairs.
                        if (code_point >= 0xD800 && code_point <= 0xDBFF && pos_ + 1 < text_.size() &&
                            text_[pos_] == '\\' && text_[pos_ + 1] == 'u') {
                            const std::size_t saved = pos_;
                            pos_ += 2;
                            auto low = parse_hex4();
                            if (low && low.value() >= 0xDC00 && low.value() <= 0xDFFF) {
                                code_point = 0x10000u + ((code_point - 0xD800u) << 10) +
                                             (low.value() - 0xDC00u);
                            } else {
                                pos_ = saved;
                            }
                        }
                        append_utf8(out, code_point);
                        break;
                    }
                    default:
                        return fail("invalid escape character");
                }
                continue;
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                return fail("unescaped control character in string");
            }
            out.push_back(c);
            ++pos_;
        }
        return fail("unterminated string");
    }

    Result<std::uint32_t> parse_hex4() {
        if (pos_ + 4 > text_.size()) {
            return fail("truncated \\u escape");
        }
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[pos_++];
            value <<= 4;
            if (c >= '0' && c <= '9') {
                value |= static_cast<std::uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                value |= static_cast<std::uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                value |= static_cast<std::uint32_t>(c - 'A' + 10);
            } else {
                return fail("invalid hex digit in \\u escape");
            }
        }
        return value;
    }

    Result<Value> parse_number() {
        const std::size_t start = pos_;
        if (pos_ < text_.size() && (text_[pos_] == '-' || text_[pos_] == '+')) {
            ++pos_;
        }
        bool is_integer = true;
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c >= '0' && c <= '9') {
                ++pos_;
            } else if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
                is_integer = false;
                ++pos_;
            } else {
                break;
            }
        }
        if (pos_ == start) {
            return fail("invalid number");
        }
        const std::string literal(text_.substr(start, pos_ - start));
        try {
            if (is_integer) {
                std::size_t consumed = 0;
                const long long parsed = std::stoll(literal, &consumed);
                if (consumed == literal.size()) {
                    return Value(static_cast<std::int64_t>(parsed));
                }
            }
            return Value(std::stod(literal));
        } catch (const std::exception&) {
            return fail("number out of range");
        }
    }

    std::string_view text_;
    std::size_t pos_{0};
};

void serialize_into(const Value& value, std::string& out, int indent, int depth);

void write_indent(std::string& out, int indent, int depth) {
    if (indent < 0) {
        return;
    }
    out.push_back('\n');
    out.append(static_cast<std::size_t>(indent * depth), ' ');
}

void serialize_into(const Value& value, std::string& out, int indent, int depth) {
    switch (value.type()) {
        case Type::Null:
            out += "null";
            return;
        case Type::Bool:
            out += value.as_bool() ? "true" : "false";
            return;
        case Type::Int:
            out += std::to_string(value.as_int());
            return;
        case Type::Double:
            out += format_double(value.as_double());
            return;
        case Type::String:
            out.push_back('"');
            out += escape(value.as_string());
            out.push_back('"');
            return;
        case Type::Array: {
            const Array& array = value.as_array();
            if (array.empty()) {
                out += "[]";
                return;
            }
            out.push_back('[');
            bool first = true;
            for (const Value& item : array) {
                if (!first) {
                    out.push_back(',');
                }
                first = false;
                write_indent(out, indent, depth + 1);
                serialize_into(item, out, indent, depth + 1);
            }
            write_indent(out, indent, depth);
            out.push_back(']');
            return;
        }
        case Type::Object: {
            const Object& object = value.as_object();
            if (object.empty()) {
                out += "{}";
                return;
            }
            out.push_back('{');
            bool first = true;
            for (const auto& [key, member] : object) {
                if (!first) {
                    out.push_back(',');
                }
                first = false;
                write_indent(out, indent, depth + 1);
                out.push_back('"');
                out += escape(key);
                out += indent < 0 ? "\":" : "\": ";
                serialize_into(member, out, indent, depth + 1);
            }
            write_indent(out, indent, depth);
            out.push_back('}');
            return;
        }
    }
}

const Array& empty_array() {
    static const Array kEmpty;
    return kEmpty;
}

const Object& empty_object() {
    static const Object kEmpty;
    return kEmpty;
}

}  // namespace

Type Value::type() const noexcept {
    switch (storage_.index()) {
        case 0: return Type::Null;
        case 1: return Type::Bool;
        case 2: return Type::Int;
        case 3: return Type::Double;
        case 4: return Type::String;
        case 5: return Type::Array;
        default: return Type::Object;
    }
}

bool Value::is_number() const noexcept {
    return storage_.index() == 2 || storage_.index() == 3;
}

bool Value::as_bool(bool fallback) const noexcept {
    if (const bool* v = std::get_if<bool>(&storage_)) {
        return *v;
    }
    if (const std::int64_t* v = std::get_if<std::int64_t>(&storage_)) {
        return *v != 0;
    }
    return fallback;
}

std::int64_t Value::as_int(std::int64_t fallback) const noexcept {
    if (const std::int64_t* v = std::get_if<std::int64_t>(&storage_)) {
        return *v;
    }
    if (const double* v = std::get_if<double>(&storage_)) {
        return static_cast<std::int64_t>(*v);
    }
    if (const bool* v = std::get_if<bool>(&storage_)) {
        return *v ? 1 : 0;
    }
    return fallback;
}

double Value::as_double(double fallback) const noexcept {
    if (const double* v = std::get_if<double>(&storage_)) {
        return *v;
    }
    if (const std::int64_t* v = std::get_if<std::int64_t>(&storage_)) {
        return static_cast<double>(*v);
    }
    return fallback;
}

std::string Value::as_string(std::string_view fallback) const {
    if (const std::string* v = std::get_if<std::string>(&storage_)) {
        return *v;
    }
    return std::string(fallback);
}

const Array& Value::as_array() const noexcept {
    if (const Array* v = std::get_if<Array>(&storage_)) {
        return *v;
    }
    return empty_array();
}

const Object& Value::as_object() const noexcept {
    if (const Object* v = std::get_if<Object>(&storage_)) {
        return *v;
    }
    return empty_object();
}

const Value* Value::find(std::string_view key) const noexcept {
    const Object* object = std::get_if<Object>(&storage_);
    if (object == nullptr) {
        return nullptr;
    }
    for (const auto& entry : *object) {
        if (entry.first == key) {
            return &entry.second;
        }
    }
    return nullptr;
}

const Value* Value::at_path(std::string_view dotted_path) const noexcept {
    const Value* current = this;
    std::size_t start = 0;
    while (start <= dotted_path.size() && current != nullptr) {
        const std::size_t dot = dotted_path.find('.', start);
        const std::string_view segment = dotted_path.substr(
            start, dot == std::string_view::npos ? std::string_view::npos : dot - start);
        if (segment.empty()) {
            return nullptr;
        }
        if (current->is_array()) {
            // Numeric segments index into arrays: "snapshot.windows.1.title".
            std::size_t index = 0;
            bool numeric = true;
            for (const char c : segment) {
                if (c < '0' || c > '9') {
                    numeric = false;
                    break;
                }
                index = index * 10 + static_cast<std::size_t>(c - '0');
            }
            const Array& items = current->as_array();
            current = (numeric && index < items.size()) ? &items[index] : nullptr;
        } else {
            current = current->find(segment);
        }
        if (dot == std::string_view::npos) {
            break;
        }
        start = dot + 1;
    }
    return current;
}

Value& Value::set(std::string key, Value value) {
    if (!is_object()) {
        storage_ = Object{};
    }
    Object& object = std::get<Object>(storage_);
    for (auto& entry : object) {
        if (entry.first == key) {
            entry.second = std::move(value);
            return *this;
        }
    }
    object.emplace_back(std::move(key), std::move(value));
    return *this;
}

Value& Value::push_back(Value value) {
    if (!is_array()) {
        storage_ = Array{};
    }
    std::get<Array>(storage_).push_back(std::move(value));
    return *this;
}

std::size_t Value::size() const noexcept {
    if (const Array* array = std::get_if<Array>(&storage_)) {
        return array->size();
    }
    if (const Object* object = std::get_if<Object>(&storage_)) {
        return object->size();
    }
    if (const std::string* text = std::get_if<std::string>(&storage_)) {
        return text->size();
    }
    return 0;
}

std::string Value::dump(int indent) const {
    return serialize(*this, indent);
}

Value Value::object(std::initializer_list<std::pair<const char*, Value>> entries) {
    Object object;
    object.reserve(entries.size());
    for (const auto& [key, value] : entries) {
        object.emplace_back(key, value);
    }
    return Value(std::move(object));
}

std::string escape(std::string_view input) {
    std::string out;
    out.reserve(input.size() + 8);
    for (const char c : input) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buffer[7];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x",
                                  static_cast<unsigned int>(static_cast<unsigned char>(c)));
                    out += buffer;
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

std::string serialize(const Value& value, int indent) {
    std::string out;
    out.reserve(256);
    serialize_into(value, out, indent, 0);
    return out;
}

Result<Value> parse(std::string_view text) {
    if (text.empty()) {
        return err::invalid("empty JSON document", "json.parse");
    }
    // Tolerate a UTF-8 BOM: exported files opened in some editors grow one.
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
        text.remove_prefix(3);
    }
    Parser parser(text);
    return parser.parse();
}

}  // namespace contextsnap::core::json
