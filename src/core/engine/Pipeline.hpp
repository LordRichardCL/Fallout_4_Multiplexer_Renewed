#pragma once

#include "assets/AssetScanner.hpp"
#include "db/Database.hpp"
#include "engine/AssetTracer.hpp"
#include "engine/AutoMerger.hpp"
#include "engine/BuildCache.hpp"
#include "engine/ChangeDetector.hpp"
#include "engine/CompilationEngine.hpp"
#include "engine/ConflictEngine.hpp"
#include "engine/DependencyEngine.hpp"
#include "engine/Exporter.hpp"
#include "engine/PluginScanner.hpp"
#include "engine/Session.hpp"
#include "engine/ValidationEngine.hpp"

#include <filesystem>
#include <memory>
#include <optional>

namespace fo4mx {

struct PipelineConfig {
    InstallConfig install;
    ScanOptions scan;
    AssetScanOptions assets;
    ConflictOptions conflicts;
    AnalysisOptions analysis;
    CompileOptions compile;
    ExportOptions exportOptions;
    std::filesystem::path databasePath; // empty = no database
    std::filesystem::path reportDirectory;
    bool useCache = true;               // incremental scans / analysis / compilation
    std::filesystem::path cachePath;    // default: next to the database
    bool traceAssets = true;            // resolve the assets compiled records use
    // When validation finds records that cannot be written safely, keep their source plugins out of
    // compilation (and those records out of the patch) and compile again, instead of blocking export.
    bool autoIsolate = true;
    // Mod Organizer 2 with the output folder inside the mods folder: replace compiled plugins that
    // have archives with archive loaders so their archives keep their priority (no INI change).
    bool archiveLoaders = true;
    // Record definitions to use instead of the runtime profile's (tests, trial catalogues). Not owned.
    const RecordDefinitions* definitions = nullptr;
};

// Runs the compiler stages in order and keeps every intermediate result:
// Scan -> Analyse -> (Resolve, Merge, Rewrite, Compile) -> Validate -> Export.
class Pipeline {
public:
    explicit Pipeline(PipelineConfig config);
    ~Pipeline();

    PipelineConfig& config() { return config_; }
    Session& session() { return *session_; }
    const Session& session() const { return *session_; }

    void scan(ProgressSink& progress = nullProgress());
    void analyse(ProgressSink& progress = nullProgress());
    // Resolve, merge, rewrite, compile and validate in memory. Returns false on critical validation failures.
    bool compile(ProgressSink& progress = nullProgress());
    // Writes plugins and side files, re-validates them and writes reports.
    ExportResult exportOutputs(ProgressSink& progress = nullProgress());
    std::vector<std::filesystem::path> writeReports(const std::filesystem::path& directory);
    // Decodes every record of the supported types and reports layouts that look wrong.
    DefinitionCheck checkDefinitions(const std::vector<Signature>& types = {}, ProgressSink& progress = nullProgress());

    // Compares the mod space with the last exported build without a full scan.
    ChangeReport checkForChanges(ProgressSink& progress = nullProgress());
    const ChangeReport& changes() const { return changes_; }
    const AssetTrace& assetTrace() const { return assetTrace_; }
    const std::vector<ArchivePriorityChange>& archivePriorityChanges() const { return archiveChanges_; }
    BuildCache* cache() { return cache_.get(); }
    uint64_t currentSettingsHash() const;

    bool scanned() const { return session_->scanned; }
    bool analysed() const { return conflicts_.has_value(); }
    bool compiled() const { return compiled_.has_value(); }

    const ConflictReport* conflicts() const { return conflicts_ ? &*conflicts_ : nullptr; }
    const DependencyReport* dependencies() const { return dependencies_ ? &*dependencies_ : nullptr; }
    const CompiledOutput* compiledOutput() const { return compiled_ ? &*compiled_ : nullptr; }
    const ValidationReport* validation() const { return validation_ ? &*validation_ : nullptr; }
    const std::optional<ExportResult>& exportResult() const { return exported_; }
    // Advanced Users Auto-Merger results (null when it is off or before Analyse).
    const AutoMergeReport* autoMerge() const { return autoMerge_ ? &*autoMerge_ : nullptr; }
    db::Database* database() { return db_ ? db_.get() : nullptr; }

    // Re-evaluate eligibility after the user changes exclusions (no rescan needed).
    void refreshEligibility();

private:
    void openDatabase();
    void openCache();
    ChangeReport finishChangeReport(const std::optional<InstallSnapshot>& build, InstallSnapshot current,
                                    uint64_t settings);

    PipelineConfig config_;
    std::unique_ptr<Session> session_;
    std::unique_ptr<db::Database> db_;
    std::optional<ConflictReport> conflicts_;
    std::optional<DependencyReport> dependencies_;
    std::optional<CompiledOutput> compiled_;
    std::optional<ValidationReport> validation_;
    std::vector<ArchivePriorityChange> archiveChanges_;
    std::optional<ExportResult> exported_;
    std::optional<AutoMergeReport> autoMerge_;
    std::unique_ptr<BuildCache> cache_;
    AssetTrace assetTrace_;
    ChangeReport changes_;
};

} // namespace fo4mx
