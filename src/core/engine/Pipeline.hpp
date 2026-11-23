#pragma once

#include "engine/BuildComparison.hpp"
#include "diagnostics/SaveGame.hpp"
#include "engine/PatchPlan.hpp"
#include "assets/AssetScanner.hpp"
#include "db/Database.hpp"
#include "engine/AssetTracer.hpp"
#include "engine/AutoMerger.hpp"
#include "engine/CacheVersions.hpp"
#include "engine/InactivePreview.hpp"
#include "engine/BuildRecord.hpp"
#include "engine/BuildCache.hpp"
#include "engine/ChangeDetector.hpp"
#include "engine/CompilationEngine.hpp"
#include "engine/ConflictEngine.hpp"
#include "engine/DependencyEngine.hpp"
#include "engine/Exporter.hpp"
#include "engine/PluginScanner.hpp"
#include "engine/OutputRuns.hpp"
#include "engine/Session.hpp"
#include "engine/ValidationEngine.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>

namespace fo4mx {

// A callback held in the configuration. Configurations are compared (to decide whether a compilation can be
// reused), so it compares by whether one is set; the ones used here run at export time only.
template <class Signature>
struct ConfigHook : std::function<Signature> {
    using std::function<Signature>::function;
    using std::function<Signature>::operator=;
    bool operator==(const ConfigHook& other) const { return bool(*this) == bool(other); }
};

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
    // The build cache components' versions (tests simulate a component's code changing with another value).
    CacheVersions cacheVersions = CacheVersions::current();
    // The user's choices in the compatibility patch plan (JSON); default: next to the database.
    std::filesystem::path patchPlanDecisions;
    bool traceAssets = true;            // resolve the assets compiled records use
    // When validation finds records that cannot be written safely, keep their source plugins out of
    // compilation (and those records out of the patch) and compile again, instead of blocking export.
    bool autoIsolate = true;
    // Mod Organizer 2 with the output folder inside the mods folder: replace compiled plugins that
    // have archives with archive loaders so their archives keep their priority (no INI change).
    bool archiveLoaders = true;
    // Keep a plugin out of compilation when its archives, loaded from the INI list after switching
    // over, would no longer serve files they serve today (other archives would win them).
    bool keepArchivePriority = true;
    // Instead of keeping it out, keep each such file's winner by writing it loose into the output
    // folder (a loose file wins over every archive, as the archive did); a plugin needing more copies
    // than the limit is kept out.
    bool archiveWinnerCopies = true;
    size_t archiveWinnerCopyLimit = 2000;
    // The optional Diagnostic Recorder (F4SE plugin): when on, export puts it, ready to install, in
    // "Multiplexer Output\Diagnostic Recorder" (never inside the package's Data folder).
    bool diagnosticRecorder = false;
    std::filesystem::path diagnosticRecorderDll; // MultiplexerDiagnosticRecorder.dll (next to the application)
    // The F4SE archive loader (MultiplexerArchiveLoader.dll, next to the application): when on, compiled
    // plugins' archives are not limited by the 1,023 characters the game reads of SResourceArchiveList2.
    // Export ships the DLL and the full list (Data\F4SE\Plugins\MultiplexerArchives.txt) in the package;
    // the game must be started through F4SE. Only on Fallout 4 1.10.163, where it was tested.
    bool f4seArchiveLoader = false;
    std::filesystem::path archiveLoaderDll;
    // Editor IDs of the base-game masters from an independent tool (next to the application): the
    // Definition Check compares this app's reading of those plugins with it. Optional.
    std::filesystem::path formIdReference;
    // Tests: runs after the export's new folder is created, before the export writes anything.
    ConfigHook<void(const std::filesystem::path& outputDirectory)> beforeExportWrite;
    // Record definitions to use instead of the runtime profile's (tests, trial catalogues). Not owned.
    const RecordDefinitions* definitions = nullptr;
    bool operator==(const PipelineConfig&) const = default;
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
    // The last compile() kept the compilation before it (FormID changes accepted, nothing else changed).
    bool reusedCompile() const { return reusedCompile_; }
    // Writes plugins and side files, re-validates them and writes reports.
    ExportResult exportOutputs(ProgressSink& progress = nullProgress());
    // Written in the package folder when an export fails (removed by the next successful one).
    static constexpr const char* kBuildFailedMarker = "MULTIPLEXER_BUILD_FAILED.txt";
    std::vector<std::filesystem::path> writeReports(const std::filesystem::path& directory);
    // Decodes every record of the supported types and reports layouts that look wrong.
    DefinitionCheck checkDefinitions(const std::vector<Signature>& types = {}, ProgressSink& progress = nullProgress());

    // Compares the mod space with the last exported build without a full scan.
    ChangeReport checkForChanges(ProgressSink& progress = nullProgress());
    const ChangeReport& changes() const { return changes_; }
    // The build cache in use (null without one): its components' state and this run's counters.
    const BuildCache* cache() const { return cache_.get(); }
    // What each inactive plugin would bring if enabled (empty unless ScanOptions::previewInactive).
    const std::vector<InactivePreviewRow>& inactivePreview() const { return inactivePreview_; }
    const AssetTrace& assetTrace() const { return assetTrace_; }
    const std::vector<ArchivePriorityChange>& archivePriorityChanges() const { return archiveChanges_; }
    BuildCache* cache() { return cache_.get(); }
    uint64_t currentSettingsHash() const;

    bool scanned() const { return session_->scanned; }
    bool analysed() const { return conflicts_.has_value(); }
    bool compiled() const { return compiled_.has_value(); }

    const ConflictReport* conflicts() const { return conflicts_ ? &*conflicts_ : nullptr; }
    // The reviewable compatibility patch plan, built after Analyse from the conflicts and saved choices.
    const PatchPlan* patchPlan() const { return plan_ ? &*plan_ : nullptr; }
    const PlanDecisions& planDecisions() const { return planDecisions_; }
    std::filesystem::path planDecisionsPath() const;
    // Records the user's choice for one change (nullopt: back to the default), saves it and rebuilds the plan
    // (pointers into the previous plan are invalidated).
    void setPlanDecision(FormKey record, const std::string& changeId, std::optional<PlanAction> action);
    // Accepts the automatic choice for the changes that wait for one (of `record`, or of every record): each is
    // recorded as "keep the winner's value", which is what compiling does with an undecided change, so the build
    // stays the same and the records no longer wait. Saved once; returns how many changes were decided.
    size_t acceptAutomatic(std::optional<FormKey> record = std::nullopt);
    const DependencyReport* dependencies() const { return dependencies_ ? &*dependencies_ : nullptr; }
    const CompiledOutput* compiledOutput() const { return compiled_ ? &*compiled_ : nullptr; }
    const ValidationReport* validation() const { return validation_ ? &*validation_ : nullptr; }
    // For the Validation Report: the compile validation plus the latest definition check (its
    // findings are never errors). Null when neither has run.
    const ValidationReport* reportValidation() const;
    const std::optional<ExportResult>& exportResult() const { return exported_; }
    // Advanced Users Auto-Merger results (null when it is off or before Analyse).
    const AutoMergeReport* autoMerge() const { return autoMerge_ ? &*autoMerge_ : nullptr; }
    // This compile compared with the previous build (when there is one).
    const BuildComparison* comparison() const { return comparison_ ? &*comparison_ : nullptr; }
    // What a save is checked against: the load order after this compile when there is one, else now.
    diagnostics::SaveCheckInput saveCheckInput() const;

    // "Ready to test?": what the testing guide asks for before Apply to Game, each Pass, Warn or Fail with why.
    // `save`: a save to check against the build (the newest by default; none checked when empty).
    struct ReadinessItem {
        enum class State { Pass, Warn, Fail } state = State::Pass;
        std::string title;
        std::string detail;
    };
    struct Readiness {
        std::vector<ReadinessItem> items;
        bool ready() const
        {
            for (const auto& i : items)
                if (i.state == ReadinessItem::State::Fail)
                    return false;
            return true;
        }
        std::string format() const;
    };
    Readiness readiness(const std::filesystem::path& save) const;
    db::Database* database() { return db_ ? db_.get() : nullptr; }

    // Re-evaluate eligibility after the user changes exclusions (no rescan needed).
    void refreshEligibility();

    // The latest finished export, reopened after a scan when it still fits (restoreLastBuild): enough to
    // Apply it without compiling again. Null when there is none, or it does not fit (restoreProblem says
    // why, restoreDetails lists what changed).
    const RestoredBuild* restoredBuild() const { return restored_ ? &*restored_ : nullptr; }
    const std::string& restoreProblem() const { return restoreProblem_; }
    const std::vector<std::string>& restoreDetails() const { return restoreDetails_; }
    // Checks the latest finished export against the scanned load order and its own files, and reopens it
    // when everything still matches (scan() calls it). Returns whether a build was reopened.
    bool restoreLastBuild();

    // The build Apply to Game works on: this session's compiled and exported build, else the reopened one.
    struct ApplySource {
        const CompiledOutput* compiled = nullptr;
        const ExportResult* exported = nullptr;
        const ValidationReport* validation = nullptr;
        bool restored = false;
        explicit operator bool() const { return compiled && exported; }
    };
    ApplySource applySource() const;

    // The folder holding the exports (where Apply to Game records what it changed, for Switch back).
    std::filesystem::path outputRoot() const;

private:
    // This app's own files that stay open while it runs (results database, build cache): never moved
    // to an output backup and never packaged, even when they sit in the output folder.
    std::vector<std::filesystem::path> workingFiles() const;
    // Where exports go (the Output folder setting), the latest finished export's folder of build
    // records (FormID map, copy lists; empty: none), and where the next export will write.
    output::Location outputLocation() const;
    std::filesystem::path previousBuildDirectory() const;
    std::filesystem::path pendingOutputDirectory() const;
    // The Mod Organizer 2 mod the next export writes into (archive loaders and master placeholders are
    // written only there); empty, with the reason in `off`, when there is none.
    std::string loaderMod(std::string* off) const;
    // The analysis options, with whether master placeholders are possible for the next export.
    AnalysisOptions analysisOptions() const;
    void markBuildFailed(const std::filesystem::path& directory, const std::filesystem::path& packageRoot,
                         const std::vector<std::filesystem::path>& written, const std::string& reason);

    void openDatabase();
    void openCache();
    ChangeReport finishChangeReport(const std::optional<InstallSnapshot>& build, InstallSnapshot current,
                                    uint64_t settings);

    PipelineConfig config_;
    std::unique_ptr<Session> session_;
    std::unique_ptr<db::Database> db_;
    std::optional<ConflictReport> conflicts_;
    std::optional<PatchPlan> plan_;
    PlanDecisions planDecisions_;
    void writeReviewState(); // ConflictReview, from the plan
    std::optional<DependencyReport> dependencies_;
    std::optional<CompiledOutput> compiled_;
    std::optional<ValidationReport> validation_;
    mutable std::optional<ValidationReport> definitionCheck_; // latest definition check's findings
    std::optional<ValidationReport> candidateCheck_;          // this analysis's check of the Candidate types
    mutable std::optional<ValidationReport> reportValidation_;
    std::vector<ArchivePriorityChange> archiveChanges_;
    std::optional<ExportResult> exported_;
    std::optional<RestoredBuild> restored_;
    std::string restoreProblem_;
    std::vector<std::string> restoreDetails_;
    std::optional<AutoMergeReport> autoMerge_;
    std::optional<BuildComparison> comparison_;
    std::unique_ptr<BuildCache> cache_;
    AssetTrace assetTrace_;
    ChangeReport changes_;
    // The configuration of the last compilation (acceptance of FormID changes left out), cleared by
    // anything else that shapes a compilation; and the FormID Stability error that stopped it, if one did.
    std::optional<PipelineConfig> lastCompileInputs_;
    std::optional<size_t> formIdGate_;
    bool reusedCompile_ = false;
    std::string formIdGateText_;
    void prepareCompileConfig();
    PipelineConfig compileInputs() const;
    bool finishCompile();
    std::vector<InactivePreviewRow> inactivePreview_;
};

} // namespace fo4mx
