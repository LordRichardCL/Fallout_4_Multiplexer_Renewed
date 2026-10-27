#pragma once

#include "engine/CompilationEngine.hpp"
#include "engine/Exporter.hpp"
#include "engine/Session.hpp"
#include "util/Json.hpp"
#include "util/Progress.hpp"
#include "vfs/GameInstall.hpp"

#include <string>
#include <vector>

namespace fo4mx {

struct ModFingerprint {
    std::string name;
    bool enabled = true;
    int32_t priority = 0;
    uint64_t files = 0;
    uint64_t bytes = 0;
    uint64_t hash = 0; // order-independent hash of (path, size, modified) of every file
};

struct FileFingerprint {
    std::string name;
    std::string mod;
    uint64_t size = 0;
    int64_t modified = 0;
    uint64_t hash = 0; // plugins: whole file; archives and outputs: sampled content
    bool active = false;
    int32_t loadOrder = -1;
    std::string recordTypes; // plugins: "WEAP OMOD COBJ"
};

// State of the mod space at one point in time. The "build" snapshot is taken
// after a successful export and is the reference for change detection.
struct InstallSnapshot {
    int64_t created = 0;
    std::string installKind;
    std::string profile;
    std::vector<ModFingerprint> mods;
    std::vector<FileFingerprint> plugins;
    std::vector<FileFingerprint> archives;
    std::vector<std::string> loadOrder; // active plugins
    uint64_t looseFiles = 0;

    // Build information (only in build snapshots).
    bool isBuild = false;
    std::vector<std::string> compiledPlugins;
    std::vector<std::string> participants; // plugins whose records shaped the output
    std::string outputDirectory;
    std::vector<FileFingerprint> outputs;
    uint64_t settingsHash = 0;

    json::Value toJson() const;
    static InstallSnapshot fromJson(const json::Value& v);
};

enum class BuildStatus : uint8_t { NotBuilt, UpToDate, ChangesDetected, RequiresRebuild };
const char* buildStatusName(BuildStatus s);

struct ChangeItem {
    std::string category; // Mod, Plugin, Archive, Load Order, Output, Settings
    std::string kind;     // Added, Removed, Updated, Enabled, Disabled, Moved, Missing, Modified, Changed
    std::string name;
    std::string detail;
    bool affectsBuild = false;
};

struct ChangeReport {
    BuildStatus status = BuildStatus::NotBuilt;
    int64_t referenceTime = 0;
    int64_t checkedTime = 0;
    std::vector<ChangeItem> items;
    size_t newMods = 0, removedMods = 0, updatedMods = 0;
    size_t addedPlugins = 0, removedPlugins = 0, updatedPlugins = 0;
    size_t archiveChanges = 0, loadOrderChanges = 0;
    std::vector<std::string> affectedRecordTypes;   // record types in changed plugins
    std::vector<std::string> unaffectedRecordTypes; // compiled types no changed plugin touches
    std::vector<std::string> reasons;               // why a rebuild is required
};

// Installation fingerprinting and change detection (the "build system" view).
class ChangeDetector {
public:
    // Full snapshot from a scanned session.
    static InstallSnapshot capture(const Session& session);
    // Adds build information after an export.
    static void addBuild(InstallSnapshot& snapshot, const Session& session, const CompiledOutput& compiled,
                         const ExportResult& exported, uint64_t settingsHash);
    // Fast snapshot without indexing records: resolves the install, walks the
    // virtual Data folder and reads plugin headers. Hashes are only computed
    // for files whose size or date differ from `reference`.
    static InstallSnapshot captureQuick(const InstallConfig& config, const InstallSnapshot* reference,
                                        ProgressSink& progress = nullProgress());
    static ChangeReport compare(const InstallSnapshot* reference, const InstallSnapshot& current);
    // Fingerprints the reference build's generated plugins as they are on disk now.
    static void fingerprintOutputs(const InstallSnapshot& reference, InstallSnapshot& current);

    // Hash of everything besides the mod space that changes the output. User
    // resolutions are hashed by plugin name so the value is stable across sessions.
    static uint64_t settingsHash(const Session& session, const CompileOptions& compile,
                                 const std::vector<std::string>& excluded);
    // Same, with resolutions given as "origin.esp:objectId=winner.esp" strings.
    static uint64_t settingsHash(const CompileOptions& compile, const std::vector<std::string>& excluded,
                                 const std::string& runtimeName, std::vector<std::string> resolutions);
};

} // namespace fo4mx
