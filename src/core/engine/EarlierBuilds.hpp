#pragma once

#include "engine/OutputRuns.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace fo4mx {

class Session;

// What an earlier build compiled. A build's outputs replace the plugins it compiled, which are then
// switched off in plugins.txt (Apply to Game, the mod manager, or by hand). When the next build is
// compiled while they are still switched off, they are still sources: without this record they would
// be read as uninstalled and silently left out of every later build.
struct BuildRecord {
    std::vector<std::string> outputs;  // output plugins of that build
    std::vector<std::string> compiled; // plugins it compiled
    std::string source;                // where the record was read (for reports)
};

namespace earlier {

// Written into every export's Data folder (so it is installed with the outputs) and read back by scans.
inline constexpr const char* kCompiledListFile = "Multiplexer_CompiledPlugins.txt";

std::string formatCompiledList(const std::vector<std::string>& outputs, const std::vector<std::string>& compiled);
// "output: Name.esp" lines name the outputs, other lines the compiled plugins; '#' comments.
std::optional<BuildRecord> parseCompiledList(const std::string& text, std::string source);
std::optional<BuildRecord> readCompiledList(const std::filesystem::path& file);

// Every finished export under `location`, newest first: its Multiplexer_CompiledPlugins.txt, or for
// exports written before it existed, its Multiplexer_DisablePlugins.txt and the plugin files in its
// Data folder. The game's own Data folder (no exports) is read directly.
std::vector<BuildRecord> recordsFromExports(const output::Location& location);

// After the scanner has marked this app's outputs (generated): every output of an earlier build in the
// load order is matched with the record that names it, and the plugins that build compiled become
// sources again (Session::reactivate). The result is kept in Session::earlierBuilds.
void apply(Session& session, const std::vector<BuildRecord>& records);

} // namespace earlier
} // namespace fo4mx
