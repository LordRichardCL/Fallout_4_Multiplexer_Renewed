#pragma once

#include <cstdint>
#include <filesystem>
#include <unordered_map>
#include <string>
#include <string_view>
#include <vector>

namespace fo4mx {

std::string toLower(std::string_view s);
bool iequals(std::string_view a, std::string_view b);
bool iendsWith(std::string_view s, std::string_view suffix);
bool istartsWith(std::string_view s, std::string_view prefix);
std::string trim(std::string_view s);
std::vector<std::string> split(std::string_view s, char delimiter, bool trimParts = true);
std::string join(const std::vector<std::string>& parts, std::string_view separator);

// Game-relative asset key: lowercase, backslash separated, no leading "data\".
std::string normalizeAssetPath(std::string_view path);

// "0001A2B3"
std::string formatFormId(uint32_t formId);
// 1234567 -> "1,234,567"
std::string formatCount(uint64_t value);
// 1536 -> "1.5 KB"
std::string formatBytes(uint64_t bytes);

std::string pathToUtf8(const std::filesystem::path& p);
std::filesystem::path utf8ToPath(std::string_view s);

// Case-insensitive lookup of a child entry on case-sensitive file systems.
std::filesystem::path findChildCaseInsensitive(const std::filesystem::path& dir, std::string_view name);
// The same for a relative path ("a\\b/c.txt"), one component at a time. Empty when it does not exist.
std::filesystem::path findPathCaseInsensitive(const std::filesystem::path& root, std::string_view relative);

// The same, for many lookups in a row: each folder is listed at most once (findPathCaseInsensitive lists a
// folder per call when the exact spelling is not there, so a loop over many files in one folder takes
// quadratic time). For read-only checks: files created after a folder was listed are not seen.
class PathLookup {
public:
    std::filesystem::path find(const std::filesystem::path& root, std::string_view relative);

private:
    const std::unordered_map<std::string, std::filesystem::path>& listing(const std::filesystem::path& dir);
    std::unordered_map<std::string, std::unordered_map<std::string, std::filesystem::path>> dirs_;
};

// Strip characters that are illegal in Windows file names.
std::string sanitizeFileName(std::string_view s);

// Escape for HTML text / attribute content.
std::string htmlEscape(std::string_view s);

uint64_t fnv1a64(const void* data, size_t size, uint64_t seed = 0xcbf29ce484222325ULL);

} // namespace fo4mx
