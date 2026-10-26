#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>

namespace fo4mx {

// Case-insensitive INI reader (Bethesda and Mod Organizer style). Later
// loads override earlier values, mirroring Fallout4.ini -> Fallout4Custom.ini.
class IniFile {
public:
    bool load(const std::filesystem::path& path);
    void loadFromString(const std::string& text);

    std::optional<std::string> get(const std::string& section, const std::string& key) const;
    std::string get(const std::string& section, const std::string& key, const std::string& def) const;
    void set(const std::string& section, const std::string& key, const std::string& value);
    bool empty() const { return values_.empty(); }

private:
    std::map<std::string, std::map<std::string, std::string>> values_; // lowercase section/key
};

} // namespace fo4mx
