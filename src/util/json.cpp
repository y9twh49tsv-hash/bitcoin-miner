#include "util/json.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace azd::util {
namespace {

const Json& nullValue() {
    static const Json kNull;
    return kNull;
}

struct Parser {
    const std::string& text;
    size_t pos = 0;
    std::string error;
    int depth = 0;

    static constexpr int kMaxDepth = 64;

    explicit Parser(const std::string& t) : text(t) {}

    void skipWhitespace() {
        while (pos < text.size()) {
            const char c = text[pos];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos;
            } else {
                break;
            }
        }
    }

    bool fail(const std::string& message) {
        if (error.empty()) {
            error = message + " at offset " + std::to_string(pos);
        }
        return false;
    }

    bool parseValue(Json& out) {
        if (depth >= kMaxDepth) return fail("nesting too deep");
        skipWhitespace();
        if (pos >= text.size()) return fail("unexpected end of input");

        switch (text[pos]) {
            case '{': return parseObject(out);
            case '[': return parseArray(out);
            case '"': {
                std::string s;
                if (!parseString(s)) return false;
                out = Json(s);
                return true;
            }
            case 't':
                if (text.compare(pos, 4, "true") == 0) {
                    pos += 4;
                    out = Json(true);
                    return true;
                }
                return fail("invalid literal");
            case 'f':
                if (text.compare(pos, 5, "false") == 0) {
                    pos += 5;
                    out = Json(false);
                    return true;
                }
                return fail("invalid literal");
            case 'n':
                if (text.compare(pos, 4, "null") == 0) {
                    pos += 4;
                    out = Json::null();
                    return true;
                }
                return fail("invalid literal");
            default: return parseNumber(out);
        }
    }

    bool parseNumber(Json& out) {
        const size_t start = pos;
        if (pos < text.size() && (text[pos] == '-' || text[pos] == '+')) ++pos;
        bool any = false;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
            ++pos;
            any = true;
        }
        if (pos < text.size() && text[pos] == '.') {
            ++pos;
            while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
                ++pos;
                any = true;
            }
        }
        if (any && pos < text.size() && (text[pos] == 'e' || text[pos] == 'E')) {
            ++pos;
            if (pos < text.size() && (text[pos] == '-' || text[pos] == '+')) ++pos;
            while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') ++pos;
        }
        if (!any) return fail("invalid number");

        out = Json(std::strtod(text.substr(start, pos - start).c_str(), nullptr));
        return true;
    }

    bool parseString(std::string& out) {
        if (pos >= text.size() || text[pos] != '"') return fail("expected string");
        ++pos;
        std::string result;
        while (true) {
            if (pos >= text.size()) return fail("unterminated string");
            const char c = text[pos++];
            if (c == '"') break;
            if (c != '\\') {
                result.push_back(c);
                continue;
            }
            if (pos >= text.size()) return fail("unterminated escape");
            const char esc = text[pos++];
            switch (esc) {
                case '"': result.push_back('"'); break;
                case '\\': result.push_back('\\'); break;
                case '/': result.push_back('/'); break;
                case 'b': result.push_back('\b'); break;
                case 'f': result.push_back('\f'); break;
                case 'n': result.push_back('\n'); break;
                case 'r': result.push_back('\r'); break;
                case 't': result.push_back('\t'); break;
                case 'u': {
                    if (pos + 4 > text.size()) return fail("truncated \\u escape");
                    unsigned code = 0;
                    for (int i = 0; i < 4; ++i) {
                        const char h = text[pos + static_cast<size_t>(i)];
                        code <<= 4;
                        if (h >= '0' && h <= '9') code |= static_cast<unsigned>(h - '0');
                        else if (h >= 'a' && h <= 'f') code |= static_cast<unsigned>(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') code |= static_cast<unsigned>(h - 'A' + 10);
                        else return fail("invalid \\u escape");
                    }
                    pos += 4;
                    // Encode as UTF-8. Surrogate pairs are passed through as
                    // replacement characters; Stratum traffic is ASCII hex.
                    if (code < 0x80) {
                        result.push_back(static_cast<char>(code));
                    } else if (code < 0x800) {
                        result.push_back(static_cast<char>(0xC0 | (code >> 6)));
                        result.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                    } else {
                        result.push_back(static_cast<char>(0xE0 | (code >> 12)));
                        result.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                        result.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                    }
                    break;
                }
                default: return fail("invalid escape");
            }
        }
        out.swap(result);
        return true;
    }

    bool parseArray(Json& out) {
        ++pos; // '['
        ++depth;
        Json arr = Json::array();
        skipWhitespace();
        if (pos < text.size() && text[pos] == ']') {
            ++pos;
            --depth;
            out = std::move(arr);
            return true;
        }
        while (true) {
            Json element;
            if (!parseValue(element)) return false;
            arr.push(std::move(element));
            skipWhitespace();
            if (pos >= text.size()) return fail("unterminated array");
            if (text[pos] == ',') {
                ++pos;
                continue;
            }
            if (text[pos] == ']') {
                ++pos;
                break;
            }
            return fail("expected ',' or ']'");
        }
        --depth;
        out = std::move(arr);
        return true;
    }

    bool parseObject(Json& out) {
        ++pos; // '{'
        ++depth;
        Json obj = Json::object();
        skipWhitespace();
        if (pos < text.size() && text[pos] == '}') {
            ++pos;
            --depth;
            out = std::move(obj);
            return true;
        }
        while (true) {
            skipWhitespace();
            std::string key;
            if (!parseString(key)) return false;
            skipWhitespace();
            if (pos >= text.size() || text[pos] != ':') return fail("expected ':'");
            ++pos;
            Json value;
            if (!parseValue(value)) return false;
            obj.set(key, std::move(value));
            skipWhitespace();
            if (pos >= text.size()) return fail("unterminated object");
            if (text[pos] == ',') {
                ++pos;
                continue;
            }
            if (text[pos] == '}') {
                ++pos;
                break;
            }
            return fail("expected ',' or '}'");
        }
        --depth;
        out = std::move(obj);
        return true;
    }
};

} // namespace

bool Json::asBool(bool fallback) const {
    if (type_ == Type::Bool) return bool_;
    if (type_ == Type::Number) return number_ != 0.0;
    return fallback;
}

double Json::asNumber(double fallback) const {
    if (type_ == Type::Number) return number_;
    if (type_ == Type::Bool) return bool_ ? 1.0 : 0.0;
    if (type_ == Type::String && !string_.empty()) {
        char* end = nullptr;
        const double v = std::strtod(string_.c_str(), &end);
        if (end && *end == '\0') return v;
    }
    return fallback;
}

int64_t Json::asInt(int64_t fallback) const {
    if (type_ == Type::Number) return static_cast<int64_t>(number_);
    if (type_ == Type::String) {
        char* end = nullptr;
        const long long v = std::strtoll(string_.c_str(), &end, 10);
        if (end && *end == '\0' && !string_.empty()) return static_cast<int64_t>(v);
    }
    if (type_ == Type::Bool) return bool_ ? 1 : 0;
    return fallback;
}

uint32_t Json::asUInt32(uint32_t fallback) const {
    if (type_ == Type::Null) return fallback;
    const int64_t v = asInt(static_cast<int64_t>(fallback));
    if (v < 0) return fallback;
    return static_cast<uint32_t>(v);
}

std::string Json::asString(const std::string& fallback) const {
    if (type_ == Type::String) return string_;
    return fallback;
}

size_t Json::size() const {
    if (type_ == Type::Array) return array_.size();
    if (type_ == Type::Object) return object_.size();
    return 0;
}

const Json& Json::at(size_t index) const {
    if (type_ != Type::Array || index >= array_.size()) return nullValue();
    return array_[index];
}

bool Json::has(const std::string& key) const {
    return type_ == Type::Object && object_.find(key) != object_.end();
}

const Json& Json::operator[](const std::string& key) const {
    if (type_ != Type::Object) return nullValue();
    const auto it = object_.find(key);
    if (it == object_.end()) return nullValue();
    return it->second;
}

void Json::push(Json value) {
    if (type_ != Type::Array) {
        type_ = Type::Array;
        array_.clear();
    }
    array_.push_back(std::move(value));
}

void Json::set(const std::string& key, Json value) {
    if (type_ != Type::Object) {
        type_ = Type::Object;
        object_.clear();
        keyOrder_.clear();
    }
    if (object_.find(key) == object_.end()) keyOrder_.push_back(key);
    object_[key] = std::move(value);
}

std::string jsonEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 2);
    out.push_back('"');
    for (unsigned char c : in) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
    return out;
}

void Json::dumpTo(std::string& out, int indent, int depth) const {
    const std::string pad = indent > 0 ? std::string(static_cast<size_t>(indent * (depth + 1)), ' ')
                                       : std::string();
    const std::string padEnd =
        indent > 0 ? std::string(static_cast<size_t>(indent * depth), ' ') : std::string();
    const char* newline = indent > 0 ? "\n" : "";

    switch (type_) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += bool_ ? "true" : "false"; break;
        case Type::Number: {
            if (!std::isfinite(number_)) {
                out += "0";
                break;
            }
            if (number_ == static_cast<double>(static_cast<int64_t>(number_)) &&
                std::fabs(number_) < 9.0e15) {
                out += std::to_string(static_cast<int64_t>(number_));
            } else {
                char buf[40];
                std::snprintf(buf, sizeof(buf), "%.10g", number_);
                out += buf;
            }
            break;
        }
        case Type::String: out += jsonEscape(string_); break;
        case Type::Array: {
            if (array_.empty()) {
                out += "[]";
                break;
            }
            out += "[";
            out += newline;
            for (size_t i = 0; i < array_.size(); ++i) {
                out += pad;
                array_[i].dumpTo(out, indent, depth + 1);
                if (i + 1 < array_.size()) out += ",";
                out += newline;
            }
            out += padEnd;
            out += "]";
            break;
        }
        case Type::Object: {
            if (keyOrder_.empty()) {
                out += "{}";
                break;
            }
            out += "{";
            out += newline;
            for (size_t i = 0; i < keyOrder_.size(); ++i) {
                out += pad;
                out += jsonEscape(keyOrder_[i]);
                out += indent > 0 ? ": " : ":";
                object_.at(keyOrder_[i]).dumpTo(out, indent, depth + 1);
                if (i + 1 < keyOrder_.size()) out += ",";
                out += newline;
            }
            out += padEnd;
            out += "}";
            break;
        }
    }
}

std::string Json::dump(int indent) const {
    std::string out;
    dumpTo(out, indent, 0);
    return out;
}

bool Json::parse(const std::string& text, Json& out, std::string* error) {
    Parser parser(text);
    Json value;
    if (!parser.parseValue(value)) {
        if (error) *error = parser.error;
        return false;
    }
    parser.skipWhitespace();
    if (parser.pos != text.size()) {
        if (error) *error = "trailing characters at offset " + std::to_string(parser.pos);
        return false;
    }
    out = std::move(value);
    return true;
}

} // namespace azd::util
