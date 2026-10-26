#include "util/Json.hpp"

#include <cmath>
#include <cctype>
#include <cstdio>
#include <stdexcept>

namespace fo4mx::json {

namespace {
const Value& nullValue()
{
    static const Value v;
    return v;
}
const Array& emptyArray()
{
    static const Array a;
    return a;
}
const Object& emptyObject()
{
    static const Object o;
    return o;
}

void appendUtf8(std::string& out, uint32_t cp)
{
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

class Parser {
public:
    explicit Parser(std::string_view s) : s_(s) {}

    Value parseDocument()
    {
        skipBom();
        Value v = parseValue(0);
        skipWs();
        if (pos_ != s_.size())
            fail("trailing characters");
        return v;
    }

private:
    [[noreturn]] void fail(const char* what) const
    {
        throw std::runtime_error(std::string("JSON parse error at offset ") + std::to_string(pos_) + ": " + what);
    }

    void skipBom()
    {
        if (s_.size() >= 3 && static_cast<uint8_t>(s_[0]) == 0xEF && static_cast<uint8_t>(s_[1]) == 0xBB &&
            static_cast<uint8_t>(s_[2]) == 0xBF)
            pos_ = 3;
    }

    void skipWs()
    {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r'))
            ++pos_;
    }

    bool consume(char c)
    {
        skipWs();
        if (pos_ < s_.size() && s_[pos_] == c) {
            ++pos_;
            return true;
        }
        return false;
    }

    Value parseValue(int depth)
    {
        if (depth > 256)
            fail("nesting too deep");
        skipWs();
        if (pos_ >= s_.size())
            fail("unexpected end");
        char c = s_[pos_];
        if (c == '{')
            return parseObject(depth);
        if (c == '[')
            return parseArray(depth);
        if (c == '"')
            return Value(parseString());
        if (s_.compare(pos_, 4, "true") == 0) {
            pos_ += 4;
            return Value(true);
        }
        if (s_.compare(pos_, 5, "false") == 0) {
            pos_ += 5;
            return Value(false);
        }
        if (s_.compare(pos_, 4, "null") == 0) {
            pos_ += 4;
            return Value();
        }
        return parseNumber();
    }

    Value parseObject(int depth)
    {
        ++pos_;
        Object obj;
        if (consume('}'))
            return Value(std::move(obj));
        do {
            skipWs();
            if (pos_ >= s_.size() || s_[pos_] != '"')
                fail("expected key");
            std::string key = parseString();
            if (!consume(':'))
                fail("expected ':'");
            obj.emplace_back(std::move(key), parseValue(depth + 1));
        } while (consume(','));
        if (!consume('}'))
            fail("expected '}'");
        return Value(std::move(obj));
    }

    Value parseArray(int depth)
    {
        ++pos_;
        Array arr;
        if (consume(']'))
            return Value(std::move(arr));
        do {
            arr.push_back(parseValue(depth + 1));
        } while (consume(','));
        if (!consume(']'))
            fail("expected ']'");
        return Value(std::move(arr));
    }

    uint32_t hex4()
    {
        if (pos_ + 4 > s_.size())
            fail("bad unicode escape");
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            char c = s_[pos_++];
            v <<= 4;
            if (c >= '0' && c <= '9')
                v |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f')
                v |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                v |= static_cast<uint32_t>(c - 'A' + 10);
            else
                fail("bad hex digit");
        }
        return v;
    }

    std::string parseString()
    {
        ++pos_; // opening quote
        std::string out;
        while (true) {
            if (pos_ >= s_.size())
                fail("unterminated string");
            char c = s_[pos_++];
            if (c == '"')
                break;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (pos_ >= s_.size())
                fail("bad escape");
            char e = s_[pos_++];
            switch (e) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                uint32_t cp = hex4();
                if (cp >= 0xD800 && cp <= 0xDBFF && pos_ + 6 <= s_.size() && s_[pos_] == '\\' && s_[pos_ + 1] == 'u') {
                    pos_ += 2;
                    uint32_t low = hex4();
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                }
                appendUtf8(out, cp);
                break;
            }
            default: fail("bad escape");
            }
        }
        return out;
    }

    Value parseNumber()
    {
        size_t start = pos_;
        if (pos_ < s_.size() && (s_[pos_] == '-' || s_[pos_] == '+'))
            ++pos_;
        while (pos_ < s_.size() && (std::isdigit(static_cast<unsigned char>(s_[pos_])) || s_[pos_] == '.' ||
                                    s_[pos_] == 'e' || s_[pos_] == 'E' || s_[pos_] == '-' || s_[pos_] == '+'))
            ++pos_;
        if (start == pos_)
            fail("unexpected character");
        std::string num(s_.substr(start, pos_ - start));
        try {
            return Value(std::stod(num));
        } catch (...) {
            fail("bad number");
        }
    }

    std::string_view s_;
    size_t pos_ = 0;
};

} // namespace

const Array& Value::asArray() const
{
    return isArray() ? *std::get<std::shared_ptr<Array>>(v_) : emptyArray();
}

const Object& Value::asObject() const
{
    return isObject() ? *std::get<std::shared_ptr<Object>>(v_) : emptyObject();
}

Array& Value::array()
{
    if (!isArray())
        v_ = std::make_shared<Array>();
    return *std::get<std::shared_ptr<Array>>(v_);
}

Object& Value::object()
{
    if (!isObject())
        v_ = std::make_shared<Object>();
    return *std::get<std::shared_ptr<Object>>(v_);
}

const Value& Value::operator[](std::string_view key) const
{
    if (!isObject())
        return nullValue();
    for (const auto& [k, v] : asObject())
        if (k == key)
            return v;
    return nullValue();
}

void Value::set(std::string key, Value v)
{
    auto& o = object();
    for (auto& [k, existing] : o)
        if (k == key) {
            existing = std::move(v);
            return;
        }
    o.emplace_back(std::move(key), std::move(v));
}

void Value::push(Value v)
{
    array().push_back(std::move(v));
}

std::string escape(std::string_view s)
{
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                out += buf;
            } else {
                out.push_back(c);
            }
        }
    }
    return out;
}

void Value::dumpTo(std::string& out, int indent, int level) const
{
    auto newline = [&](int lvl) {
        if (indent <= 0)
            return;
        out.push_back('\n');
        out.append(static_cast<size_t>(indent * lvl), ' ');
    };
    if (isNull()) {
        out += "null";
    } else if (isBool()) {
        out += asBool() ? "true" : "false";
    } else if (isNumber()) {
        double d = asNumber();
        if (!std::isfinite(d)) {
            out += "null";
        } else if (d == std::floor(d) && std::fabs(d) < 9.0e15) {
            out += std::to_string(static_cast<long long>(d));
        } else {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.9g", d);
            out += buf;
        }
    } else if (isString()) {
        out.push_back('"');
        out += escape(asString());
        out.push_back('"');
    } else if (isArray()) {
        const auto& a = asArray();
        out.push_back('[');
        for (size_t i = 0; i < a.size(); ++i) {
            if (i)
                out.push_back(',');
            newline(level + 1);
            a[i].dumpTo(out, indent, level + 1);
        }
        if (!a.empty())
            newline(level);
        out.push_back(']');
    } else {
        const auto& o = asObject();
        out.push_back('{');
        for (size_t i = 0; i < o.size(); ++i) {
            if (i)
                out.push_back(',');
            newline(level + 1);
            out.push_back('"');
            out += escape(o[i].first);
            out += indent > 0 ? "\": " : "\":";
            o[i].second.dumpTo(out, indent, level + 1);
        }
        if (!o.empty())
            newline(level);
        out.push_back('}');
    }
}

std::string Value::dump(int indent) const
{
    std::string out;
    dumpTo(out, indent, 0);
    return out;
}

Value Value::parse(std::string_view text)
{
    return Parser(text).parseDocument();
}

} // namespace fo4mx::json
