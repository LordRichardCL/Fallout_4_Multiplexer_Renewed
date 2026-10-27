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
    std::vector<std::filesystem::path> pluginFiles;   // every current output (written or unchanged)
    std::vector<std::string> writtenOutputs;          // content changed: file rewritten
    std::vector<std::string> unchangedOutputs;        // identical to the file on disk: left untouched
    std::vector<std::string> removedOutputs;          // outputs of the previous build that are no longer needed
    std::filesystem::path formMap;
    std::filesystem::path archiveIni;
    std::filesystem::path disableList;
    std::filesystem::path pluginsTxt;
    std::filesystem::path summary;
    std::vector<std::filesystem::path> archiveLoaders; // empty ESL plugins keeping compiled plugins' archives loading
};

// Stage Export: writes the output plugins and everything needed to switch
// the load order over to them.
class Exporter {
public:
    // previousOutputs: generated plugins of the last build in this folder; ones no longer produced are deleted.
    static ExportResult write(const Session& session, const CompiledOutput& compiled, const ExportOptions& options,
                              const std::vector<std::string>& previousOutputs = {});

    // Default output folder: an MO2 mod folder for MO2 users, otherwise Data.
    static std::filesystem::path defaultOutputDirectory(const Session& session);

    // Updates the MO2 profile's plugins.txt / loadorder.txt (with backups):
    // compiled plugins are disabled and the outputs enabled at the end.
    static std::vector<std::filesystem::path> applyToProfile(const Session& session, const CompiledOutput& compiled,
                                                             const std::filesystem::path& pluginsTxt);

    static std::string proposedPluginsTxt(const Session& session, const CompiledOutput& compiled);

    // The SResourceArchiveList2 value the build needs: the current value, without archives an
    // earlier build in `outputDirectory` registered but this one no longer needs, plus the
    // archives of compiled plugins.
    static std::vector<std::string> archiveList(const Session& session, const CompiledOutput& compiled,
                                                const std::filesystem::path& outputDirectory);

    // Writes archiveList() into Fallout4Custom.ini in the folder the game reads its INIs from
    // (the MO2 profile when it uses profile-local INIs), after a timestamped backup. Every other
    // line is kept. Returns the files written (empty when nothing needed to change).
    static std::vector<std::filesystem::path> applyArchiveIni(const Session& session, const CompiledOutput& compiled,
                                                              const std::filesystem::path& outputDirectory);

    // Sets [section] key=value in INI text, keeping every other line, comment and line ending.
    static std::string setIniValue(const std::string& text, const std::string& section, const std::string& key,
                                   const std::string& value);
};

} // namespace fo4mx
