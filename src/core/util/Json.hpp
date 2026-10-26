#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace fo4mx::json {

class Value;
using Array = std::vector<Value>;
using Object = std::vector<std::pair<std::string, Value>>; // insertion ordered

// Minimal JSON document model: enough for manifests, settings and reports.
class Value {
public:
    Value() : v_(nullptr) {}
    Value(std::nullptr_t) : v_(nullptr) {}
    Value(bool b) : v_(b) {}
    Value(int v) : v_(static_cast<double>(v)) {}
    Value(int64_t v) : v_(static_cast<double>(v)) {}
    Value(uint32_t v) : v_(static_cast<double>(v)) {}
    Value(uint64_t v) : v_(static_cast<double>(v)) {}
    Value(double v) : v_(v) {}
    Value(const char* s) : v_(std::string(s)) {}
    Value(std::string s) : v_(std::move(s)) {}
    Value(std::string_view s) : v_(std::string(s)) {}
    Value(Array a) : v_(std::make_shared<Array>(std::move(a))) {}
    Value(Object o) : v_(std::make_shared<Object>(std::move(o))) {}

    bool isNull() const { return std::holds_alternative<std::nullptr_t>(v_); }
    bool isBool() const { return std::holds_alternative<bool>(v_); }
    bool isNumber() const { return std::holds_alternative<double>(v_); }
    bool isString() const { return std::holds_alternative<std::string>(v_); }
    bool isArray() const { return std::holds_alternative<std::shared_ptr<Array>>(v_); }
    bool isObject() const { return std::holds_alternative<std::shared_ptr<Object>>(v_); }

    bool asBool(bool def = false) const { return isBool() ? std::get<bool>(v_) : def; }
    double asNumber(double def = 0) const { return isNumber() ? std::get<double>(v_) : def; }
    std::string asString(std::string def = {}) const { return isString() ? std::get<std::string>(v_) : def; }
    const Array& asArray() const;
    const Object& asObject() const;
    Array& array();
    Object& object();

    // Object member lookup; returns a null value when absent.
    const Value& operator[](std::string_view key) const;
    void set(std::string key, Value v);
    void push(Value v);

    std::string dump(int indent = 2) const;
    static Value parse(std::string_view text); // throws std::runtime_error

private:
    void dumpTo(std::string& out, int indent, int level) const;
    std::variant<std::nullptr_t, bool, double, std::string, std::shared_ptr<Array>, std::shared_ptr<Object>> v_;
};

std::string escape(std::string_view s);

} // namespace fo4mx::json
