// Minimal JSON value/parser/serialiser (control-plane messages and --json output only).
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <variant>
#include <vector>

namespace mtport {

class Json {
public:
    using Array = std::vector<Json>;
    using Object = std::map<std::string, Json>;

    Json() = default;  // null
    Json(std::nullptr_t) {}
    Json(bool b) : v_(b) {}
    Json(int v) : v_(static_cast<int64_t>(v)) {}
    Json(unsigned v) : v_(static_cast<int64_t>(v)) {}
    Json(int64_t v) : v_(v) {}
    Json(uint64_t v) : v_(static_cast<int64_t>(v)) {}
    Json(double v) : v_(v) {}
    Json(const char* s) : v_(std::string(s)) {}
    Json(std::string s) : v_(std::move(s)) {}
    Json(Array a) : v_(std::move(a)) {}
    Json(Object o) : v_(std::move(o)) {}

    static Json object() { return Json(Object{}); }
    static Json array() { return Json(Array{}); }

    bool isNull() const { return std::holds_alternative<std::monostate>(v_); }
    bool isObject() const { return std::holds_alternative<Object>(v_); }
    bool isArray() const { return std::holds_alternative<Array>(v_); }
    bool isString() const { return std::holds_alternative<std::string>(v_); }
    bool isNumber() const { return std::holds_alternative<int64_t>(v_) || std::holds_alternative<double>(v_); }
    bool isBool() const { return std::holds_alternative<bool>(v_); }

    // Object access: operator[] inserts (builder use); find() is read-only and returns nullptr if absent.
    Json& operator[](const std::string& key);
    const Json* find(const std::string& key) const;
    void push(Json v);

    std::string asString(const std::string& fallback = {}) const;
    int64_t asInt(int64_t fallback = 0) const;
    double asDouble(double fallback = 0.0) const;
    bool asBool(bool fallback = false) const;
    const Array& items() const;
    const Object& members() const;

    std::string dump() const;  // compact
    // Throws std::runtime_error on malformed input, depth > 32 or input > maxBytes.
    static Json parse(const std::string& text, size_t maxBytes = 1 << 20);

private:
    std::variant<std::monostate, bool, int64_t, double, std::string, Array, Object> v_;
};

}  // namespace mtport
