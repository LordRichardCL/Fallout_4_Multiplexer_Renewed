#include "util/Text.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace fo4mx {

namespace {
char lowerChar(char c)
{
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}
} // namespace

std::string toLower(std::string_view s)
{
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), lowerChar);
    return out;
}

bool iequals(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (lowerChar(a[i]) != lowerChar(b[i]))
            return false;
    return true;
}

bool iendsWith(std::string_view s, std::string_view suffix)
{
    return s.size() >= suffix.size() && iequals(s.substr(s.size() - suffix.size()), suffix);
}

bool istartsWith(std::string_view s, std::string_view prefix)
{
    return s.size() >= prefix.size() && iequals(s.substr(0, prefix.size()), prefix);
}

std::string trim(std::string_view s)
{
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b])))
        ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
        --e;
    return std::string(s.substr(b, e - b));
}

std::vector<std::string> split(std::string_view s, char delimiter, bool trimParts)
{
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t end = s.find(delimiter, start);
        if (end == std::string_view::npos)
            end = s.size();
        auto part = s.substr(start, end - start);
        out.push_back(trimParts ? trim(part) : std::string(part));
        start = end + 1;
    }
    return out;
}

std::string join(const std::vector<std::string>& parts, std::string_view separator)
{
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i)
            out += separator;
        out += parts[i];
    }
    return out;
}

std::string normalizeAssetPath(std::string_view path)
{
    std::string out;
    out.reserve(path.size());
    for (char c : path) {
        if (c == '/')
            c = '\\';
        if (c == '\\' && !out.empty() && out.back() == '\\')
            continue;
        out.push_back(lowerChar(c));
    }
    while (!out.empty() && out.front() == '\\')
        out.erase(out.begin());
    if (out.rfind("data\\", 0) == 0)
        out.erase(0, 5);
    return out;
}

std::string formatFormId(uint32_t formId)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%08X", formId);
    return buf;
}

std::string formatCount(uint64_t value)
{
    std::string digits = std::to_string(value);
    std::string out;
    int n = 0;
    for (auto it = digits.rbegin(); it != digits.rend(); ++it) {
        if (n && n % 3 == 0)
            out.push_back(',');
        out.push_back(*it);
        ++n;
    }
    std::reverse(out.begin(), out.end());
    return out;
}

std::string formatBytes(uint64_t bytes)
{
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double v = static_cast<double>(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 4) {
        v /= 1024.0;
        ++u;
    }
    char buf[32];
    if (u == 0)
        std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
    else
        std::snprintf(buf, sizeof(buf), "%.1f %s", v, units[u]);
    return buf;
}

std::string pathToUtf8(const std::filesystem::path& p)
{
    auto u8 = p.u8string();
    return std::string(u8.begin(), u8.end());
}

std::filesystem::path utf8ToPath(std::string_view s)
{
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

std::filesystem::path findChildCaseInsensitive(const std::filesystem::path& dir, std::string_view name)
{
    std::error_code ec;
    auto direct = dir / utf8ToPath(name);
    if (std::filesystem::exists(direct, ec))
        return direct;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (iequals(pathToUtf8(entry.path().filename()), name))
            return entry.path();
    }
    return {};
}

std::string sanitizeFileName(std::string_view s)
{
    std::string out;
    for (char c : s) {
        if (c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' || c == '?' ||
            c == '*' || static_cast<unsigned char>(c) < 32)
            out.push_back('_');
        else
            out.push_back(c);
    }
    return out;
}

std::string htmlEscape(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&#39;"; break;
        default: out.push_back(c);
        }
    }
    return out;
}

uint64_t fnv1a64(const void* data, size_t size, uint64_t seed)
{
    const auto* p = static_cast<const uint8_t*>(data);
    uint64_t h = seed;
    for (size_t i = 0; i < size; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

} // namespace fo4mx
