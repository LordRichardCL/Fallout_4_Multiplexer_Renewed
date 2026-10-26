#pragma once

#include "engine/Session.hpp"
#include "util/Progress.hpp"

#include <string>
#include <vector>

namespace fo4mx {

struct AssetScanOptions {
    bool scanScriptsForPluginNames = true;
    bool scanConfigsForPluginNames = true;
    unsigned threads = 0;
};

// Asset Intelligence Engine: builds the complete asset view (loose files and
// BA2 archives in engine load order) and finds plugin-name dependencies.
class AssetScanner {
public:
    static void scan(Session& session, const AssetScanOptions& options = {}, ProgressSink& progress = nullProgress());

    // Archives the engine loads for one plugin ("<name> - Main.ba2" ...).
    static std::vector<std::string> pluginArchiveNames(const PluginInfo& plugin, const std::string& language);

    // Strings in a compiled Papyrus script's string table (Fallout 4, little-endian).
    static std::vector<std::string> pexStrings(const std::vector<uint8_t>& pex);

    // Plugin file names mentioned in free text (".esp/.esm/.esl" tokens), lowercase.
    static std::vector<std::string> pluginNamesInText(std::string_view text);
};

} // namespace fo4mx
