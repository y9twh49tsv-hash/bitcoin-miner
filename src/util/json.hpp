#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace azd::util {

/// Minimal, dependency-free JSON value.
///
/// Written by hand rather than vendoring a library so the project builds with
/// nothing but a C++20 compiler and CMake on both the cloud container and a
/// fresh Windows box. It covers exactly what Stratum V1, the config file and
/// the local API need: objects, arrays, strings, numbers, booleans, null.
class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Json() : type_(Type::Null) {}
    Json(bool v) : type_(Type::Bool), bool_(v) {}
    Json(double v) : type_(Type::Number), number_(v) {}
    Json(int v) : type_(Type::Number), number_(static_cast<double>(v)) {}
    Json(int64_t v) : type_(Type::Number), number_(static_cast<double>(v)) {}
    Json(uint64_t v) : type_(Type::Number), number_(static_cast<double>(v)) {}
    Json(uint32_t v) : type_(Type::Number), number_(static_cast<double>(v)) {}
    Json(const char* v) : type_(Type::String), string_(v ? v : "") {}
    Json(std::string v) : type_(Type::String), string_(std::move(v)) {}

    static Json array() {
        Json j;
        j.type_ = Type::Array;
        return j;
    }
    static Json object() {
        Json j;
        j.type_ = Type::Object;
        return j;
    }
    static Json null() { return Json(); }

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isBool() const { return type_ == Type::Bool; }
    bool isNumber() const { return type_ == Type::Number; }
    bool isString() const { return type_ == Type::String; }
    bool isArray() const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }

    /// Accessors with defaults -- never throw, so malformed pool traffic can
    /// never take the miner down.
    bool asBool(bool fallback = false) const;
    double asNumber(double fallback = 0.0) const;
    int64_t asInt(int64_t fallback = 0) const;
    uint32_t asUInt32(uint32_t fallback = 0) const;
    std::string asString(const std::string& fallback = std::string()) const;

    /// Array access.
    size_t size() const;
    const Json& at(size_t index) const;

    /// Object access. Returns a static null value when the key is absent.
    bool has(const std::string& key) const;
    const Json& operator[](const std::string& key) const;

    /// Mutation.
    void push(Json value);
    void set(const std::string& key, Json value);

    /// Ordered key list (objects preserve insertion order so serialized output
    /// is stable and diffable).
    const std::vector<std::string>& keys() const { return keyOrder_; }

    /// Serialize. `indent > 0` pretty-prints.
    std::string dump(int indent = 0) const;

    /// Parse. Returns false and sets `error` on malformed input.
    static bool parse(const std::string& text, Json& out, std::string* error = nullptr);

private:
    void dumpTo(std::string& out, int indent, int depth) const;

    Type type_;
    bool bool_ = false;
    double number_ = 0.0;
    std::string string_;
    std::vector<Json> array_;
    std::map<std::string, Json> object_;
    std::vector<std::string> keyOrder_;
};

/// Escape a string as a JSON string literal (including the surrounding quotes).
std::string jsonEscape(const std::string& in);

} // namespace azd::util
