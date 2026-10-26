#pragma once

#include "assets/AssetScanner.hpp"
#include "db/Database.hpp"
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

    bool scanned() const { return session_->scanned; }
    bool analysed() const { return conflicts_.has_value(); }
    bool compiled() const { return compiled_.has_value(); }

    const ConflictReport* conflicts() const { return conflicts_ ? &*conflicts_ : nullptr; }
    const DependencyReport* dependencies() const { return dependencies_ ? &*dependencies_ : nullptr; }
    const CompiledOutput* compiledOutput() const { return compiled_ ? &*compiled_ : nullptr; }
    const ValidationReport* validation() const { return validation_ ? &*validation_ : nullptr; }
    const std::optional<ExportResult>& exportResult() const { return exported_; }
    db::Database* database() { return db_ ? db_.get() : nullptr; }

    // Re-evaluate eligibility after the user changes exclusions (no rescan needed).
    void refreshEligibility();

private:
    void openDatabase();

    PipelineConfig config_;
    std::unique_ptr<Session> session_;
    std::unique_ptr<db::Database> db_;
    std::optional<ConflictReport> conflicts_;
    std::optional<DependencyReport> dependencies_;
    std::optional<CompiledOutput> compiled_;
    std::optional<ValidationReport> validation_;
    std::optional<ExportResult> exported_;
};

} // namespace fo4mx
