#include "util/IniFile.hpp"

#include "util/Text.hpp"

#include <fstream>
#include <sstream>

namespace fo4mx {

bool IniFile::load(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    std::stringstream ss;
    ss << in.rdbuf();
    loadFromString(ss.str());
    return true;
}

void IniFile::loadFromString(const std::string& text)
{
    std::string section;
    std::istringstream in(text);
    std::string line;
    bool first = true;
    while (std::getline(in, line)) {
        if (first) {
            first = false;
            if (line.size() >= 3 && static_cast<uint8_t>(line[0]) == 0xEF && static_cast<uint8_t>(line[1]) == 0xBB &&
                static_cast<uint8_t>(line[2]) == 0xBF)
                line.erase(0, 3);
        }
        std::string t = trim(line);
        if (t.empty() || t[0] == ';' || t[0] == '#')
            continue;
        if (t.front() == '[' && t.back() == ']') {
            section = toLower(trim(std::string_view(t).substr(1, t.size() - 2)));
            continue;
        }
        auto eq = t.find('=');
        if (eq == std::string::npos)
            continue;
        values_[section][toLower(trim(std::string_view(t).substr(0, eq)))] = trim(std::string_view(t).substr(eq + 1));
    }
}

std::optional<std::string> IniFile::get(const std::string& section, const std::string& key) const
{
    auto s = values_.find(toLower(section));
    if (s == values_.end())
        return std::nullopt;
    auto k = s->second.find(toLower(key));
    if (k == s->second.end())
        return std::nullopt;
    return k->second;
}

std::string IniFile::get(const std::string& section, const std::string& key, const std::string& def) const
{
    auto v = get(section, key);
    return v ? *v : def;
}

void IniFile::set(const std::string& section, const std::string& key, const std::string& value)
{
    values_[toLower(section)][toLower(key)] = value;
}

} // namespace fo4mx
