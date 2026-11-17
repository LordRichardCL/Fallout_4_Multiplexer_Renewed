#pragma once

#include "engine/ChangeDetector.hpp"
#include "engine/CompilationEngine.hpp"
#include "engine/Exporter.hpp"
#include "engine/Session.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace fo4mx {

// Multiplexer_Build.json, written into every finished export: what a later session of the app needs
// to switch the load order over to this build (Apply to Game) without compiling again, and how to
// check first that the build still fits the load order and that its files are as written.
//
// It holds the install snapshot taken at export (the reference for change detection, kept here as
// well as in the results database, which is cleared when the app itself is updated), the settings
// hash, the compiled plugins, archive loaders and archive list, and every output plugin with its size
// and content hash. Paths are relative to the export's own folder, so a moved export still reopens.
namespace buildrecord {

inline constexpr const char* kFile = "Multiplexer_Build.json";
inline constexpr int kFormat = 1;

struct Output {
    std::string name;
    bool light = false;
    bool patch = false;
    uint32_t newRecords = 0;
    uint32_t overrides = 0;
    std::string file; // relative to the export folder
    uint64_t size = 0;
    uint64_t hash = 0; // fnv1a64 of the whole file
};

struct Record {
    int format = kFormat;
    std::string run;           // the export folder's name when written
    uint64_t settingsHash = 0; // ChangeDetector::settingsHash at export
    InstallSnapshot snapshot;  // the install at export, with the build information
    bool f4seArchiveLoader = false;
    std::vector<std::string> compiled;      // compiled plugins, by name
    std::vector<std::string> archiveLoaders; // compiled plugins replaced by an archive loader
    std::vector<std::string> archivesToRegister;
    std::vector<Output> outputs;
    // Export files, relative to the export folder ("" when not written).
    std::string directory, zip, disableList, recorderFolder;
    std::vector<std::string> archiveLoaderFiles, loaderPlugins;
};

// Content hash of a file (fnv1a64 over every byte); nullopt when it cannot be read.
std::optional<uint64_t> hashFile(const std::filesystem::path& file);

// What a finished export records about itself.
Record make(const Session& session, const CompiledOutput& compiled, const ExportResult& exported,
            const InstallSnapshot& snapshot, uint64_t settingsHash);
void write(const std::filesystem::path& exportFolder, const Record& record);
// Throws std::runtime_error when the file cannot be read or is not a record this app can use.
Record read(const std::filesystem::path& exportFolder);

} // namespace buildrecord

// A build reopened from its export: enough to Apply it, never to export it again.
struct RestoredBuild {
    std::string run;                // the export folder's name
    CompiledOutput compiled;        // restored: compiled plugins, outputs, archive loaders, archive list
    ExportResult exported;          // the export's files where they are now
    std::vector<std::string> notes; // changes since the build that do not affect it
};

} // namespace fo4mx
