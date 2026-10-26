#pragma once

#include "engine/CompilationEngine.hpp"
#include "engine/Session.hpp"

#include <filesystem>
#include <vector>

namespace fo4mx {

struct ExportOptions {
    std::filesystem::path outputDirectory;
    bool writeEmptyOutputs = false;
    bool compressLargeRecords = false;
};

struct ExportResult {
    std::filesystem::path directory;
    std::vector<std::filesystem::path> pluginFiles;
    std::filesystem::path formMap;
    std::filesystem::path archiveIni;
    std::filesystem::path disableList;
    std::filesystem::path pluginsTxt;
    std::filesystem::path summary;
};

// Stage Export: writes the output plugins and everything needed to switch
// the load order over to them.
class Exporter {
public:
    static ExportResult write(const Session& session, const CompiledOutput& compiled, const ExportOptions& options);

    // Default output folder: an MO2 mod folder for MO2 users, otherwise Data.
    static std::filesystem::path defaultOutputDirectory(const Session& session);

    // Updates the MO2 profile's plugins.txt / loadorder.txt (with backups):
    // compiled plugins are disabled and the outputs enabled at the end.
    static std::vector<std::filesystem::path> applyToProfile(const Session& session, const CompiledOutput& compiled,
                                                             const std::filesystem::path& pluginsTxt);

    static std::string proposedPluginsTxt(const Session& session, const CompiledOutput& compiled);
};

} // namespace fo4mx
