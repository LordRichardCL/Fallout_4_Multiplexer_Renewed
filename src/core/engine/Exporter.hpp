#pragma once

#include "engine/CompilationEngine.hpp"
#include "engine/Session.hpp"

#include <filesystem>
#include <unordered_map>
#include <vector>

namespace fo4mx {

class FileTransaction;

struct ExportOptions {
    std::filesystem::path outputDirectory;
    bool writeEmptyOutputs = false;
    bool compressLargeRecords = false;
    // Files a compiled plugin's archive wins today but would lose once its archives load from the
    // INI list: written loose into the output folder (Multiplexer_ArchiveWinners.tsv lists them).
    bool archiveWinnerCopies = true;
    // Also write "Multiplexer Output.zip" next to Data: one archive any mod manager installs.
    bool writeZip = true;
};

struct ExportResult {
    std::filesystem::path directory;   // the package's Data folder (or the output folder chosen)
    std::filesystem::path packageRoot; // "Multiplexer Output": Data, the zip and the notes for the user
    std::filesystem::path zipPath;     // "Multiplexer Output.zip" (Data\ at its root), when written
    std::vector<std::filesystem::path> pluginFiles;   // every current output (written or unchanged)
    std::vector<std::string> writtenOutputs;          // content changed: file rewritten
    std::vector<std::string> unchangedOutputs;        // identical to the file on disk: left untouched
    std::vector<std::string> removedOutputs;          // outputs of the previous build that are no longer needed
    std::filesystem::path formMap;
    std::filesystem::path archiveIni;
    std::filesystem::path disableList;
    std::filesystem::path pluginsTxt;
    std::vector<std::filesystem::path> archiveLoaders; // empty ESL plugins keeping compiled plugins' archives loading
    std::vector<std::filesystem::path> archiveLoaderFiles; // the F4SE archive loader and its list, when used
    std::vector<std::filesystem::path> seqFiles;       // seq\<output>.seq for start-game-enabled quests
    // Voice files of compiled new responses, copied as sound\voice\<output>\<voice type>\<new FormID>_<n>.*
    // (Multiplexer_Voices.tsv lists them; copies a later build no longer needs are removed).
    size_t voiceFiles = 0;
    size_t voiceFilesUnchanged = 0;
    size_t faceGenFiles = 0; // FaceGen meshes and face textures of compiled new NPCs (Multiplexer_FaceGen.tsv)
    std::filesystem::path voiceManifest;
    size_t archiveWinnerFiles = 0;
    // The Diagnostic Recorder, when prepared (Pipeline): its folder, or why it was not written.
    std::filesystem::path recorderFolder;
    // The export's own folder: its name ("Multiplexer Output DDMMYY HHMMSS"; empty when written straight
    // into the game's Data folder), the folder holding the exports, and whether "Multiplexer
    // Latest.txt" now names it.
    std::string runName;
    std::filesystem::path outputRoot;
    bool latestRecorded = false;
    std::string recorderProblem;
};

// Stage Export: writes the output plugins and everything needed to switch
// the load order over to them.
class Exporter {
public:
    // previousOutputs: generated plugins of the last build in this folder; ones no longer produced are deleted.
    static ExportResult write(const Session& session, const CompiledOutput& compiled, const ExportOptions& options,
                              const std::vector<std::string>& previousOutputs = {});

    // The loose copies an earlier export wrote to keep archive winners (path -> content hash).
    static std::unordered_map<std::string, uint64_t> archiveWinnerCopies(const std::filesystem::path& outputDir);

    // Default output folder: "<game folder>\Multiplexer Output\Data", for every mod manager.
    static std::filesystem::path defaultOutputDirectory(const Session& session);
    // The folder holding Data, the zip and the notes: the output folder's parent when it is named Data.
    static std::filesystem::path packageRoot(const std::filesystem::path& outputDirectory);
    // Zips the package's Data folder into "Multiplexer Output.zip" (after every file is written).
    static std::filesystem::path writePackageZip(ExportResult& result,
                                                 const std::vector<std::filesystem::path>& neverPackage = {});

    // Updates the MO2 profile's plugins.txt / loadorder.txt (with backups):
    // compiled plugins are disabled and the outputs enabled at the end. All or nothing.
    static std::vector<std::filesystem::path> applyToProfile(const Session& session, const CompiledOutput& compiled,
                                                             const std::filesystem::path& pluginsTxt);

    // The same changes added to a transaction, so they apply (or roll back) together with others.
    static void planProfile(FileTransaction& tx, const Session& session, const CompiledOutput& compiled,
                            const std::filesystem::path& pluginsTxt);

    static std::string proposedPluginsTxt(const Session& session, const CompiledOutput& compiled);

    // The SResourceArchiveList2 value the build needs: the current value, without archives an
    // earlier build in `outputDirectory` registered but this one no longer needs, plus the
    // archives of compiled plugins.
    // Archives an earlier build added to the INI list (Multiplexer_RegisteredArchives.txt in the
    // output folder), lowercase.
    static std::vector<std::string> registeredArchives(const std::filesystem::path& outputDirectory);
    // Fallout 4 reads SResourceArchiveList2 into a 1,024-byte buffer (GetPrivateProfileString): a
    // longer value is cut there and every archive past the cut silently never loads (scripts, meshes
    // and textures of compiled plugins go missing). The value this app writes stays within it.
    static constexpr size_t kArchiveListLimit = 1023;
    // The F4SE archive loader and the list it supplies, in the package's Data\F4SE\Plugins.
    static constexpr const char* kArchiveLoaderDll = "MultiplexerArchiveLoader.dll";
    static constexpr const char* kArchiveManifest = "MultiplexerArchives.txt";
    static std::vector<std::string> archiveList(const Session& session, const CompiledOutput& compiled,
                                                const std::filesystem::path& outputDirectory);

    // Writes archiveList() into Fallout4Custom.ini in the folder the game reads its INIs from
    // (the MO2 profile when it uses profile-local INIs), after a timestamped backup. Every other
    // line is kept. Returns the files written (empty when nothing needed to change).
    static std::vector<std::filesystem::path> applyArchiveIni(const Session& session, const CompiledOutput& compiled,
                                                              const std::filesystem::path& outputDirectory);
    // The same changes added to a transaction (the INI and the output folder's record of them).
    static void planArchiveIni(FileTransaction& tx, const Session& session, const CompiledOutput& compiled,
                               const std::filesystem::path& outputDirectory);

    // Sets [section] key=value in INI text, keeping every other line, comment and line ending.
    static std::string setIniValue(const std::string& text, const std::string& section, const std::string& key,
                                   const std::string& value);
};

} // namespace fo4mx
