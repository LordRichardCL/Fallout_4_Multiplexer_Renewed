#pragma once

#include "engine/Session.hpp"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace fo4mx {

struct CompiledOutput;

// Data\<plugin>.ini: game settings Fallout 4 reads for each active plugin, in load order (a later one's
// value for the same setting wins). A compiled plugin is switched off, so its settings are carried into
// Data\<output>.ini, which the game reads while that output is loaded; the outputs load after every plugin
// that stays, so a plugin is compiled only when every setting keeps the final value it has today.
namespace plugin_ini {

struct Entry {
    std::string section, key, value; // as written (the game compares section and key without case)
};

// The plugin's INI in the Data folder (loose), or an empty path.
std::filesystem::path fileOf(const Session& session, PluginIndex plugin);
std::vector<Entry> read(const std::filesystem::path& file);

// Compiled plugins whose settings would end with another final value (the outputs load last), with why.
// `compiled[p]` is 1 for each compiled plugin.
std::map<PluginIndex, std::string> conflicts(const Session& session, const std::vector<uint8_t>& compiled);

// Data\<output>.ini for the compiled plugins' settings, in load order ("" when none has an INI), and the
// plugins it carries.
std::string merged(const Session& session, const std::vector<PluginIndex>& compiled, std::vector<std::string>* from = nullptr);

constexpr const char* kTag = "; Written by Fallout 4 Multiplexer Renewed";

} // namespace plugin_ini
} // namespace fo4mx
