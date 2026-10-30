#pragma once

#include "db/Database.hpp"
#include "engine/AutoMerger.hpp"
#include "engine/AssetTracer.hpp"
#include "engine/ChangeDetector.hpp"
#include "engine/CompilationEngine.hpp"
#include "engine/ConflictEngine.hpp"
#include "engine/DependencyEngine.hpp"
#include "engine/Session.hpp"
#include "engine/ValidationEngine.hpp"
#include "util/Progress.hpp"

#include <optional>
#include <string>
#include <unordered_map>

namespace fo4mx::db {

// Persists engine results so the UI and reports can query them with SQL.
class DatabaseWriter {
public:
    explicit DatabaseWriter(Database& db) : db_(db) {}

    void initSchema();
    void setMeta(const std::string& key, const std::string& value);

    void writeScan(const Session& session, ProgressSink& progress = nullProgress());
    void writeAnalysis(const Session& session, const ConflictReport& conflicts, const DependencyReport& deps,
                       ProgressSink& progress = nullProgress());
    void writeCompilation(const Session& session, const CompiledOutput& compiled);
    void writeValidation(const std::string& stage, const ValidationReport& report, bool clearPrevious);
    // Advanced Users Auto-Merger results (report null: the Auto-Merger is off, tables are cleared).
    void writeAutoMerge(const Session& session, const AutoMergeReport* report, const CompiledOutput* compiled);
    void updatePlugins(const Session& session);
    void writeAssetTrace(const CompiledOutput& compiled, const AssetTrace& trace);

    // Mod space snapshots ("scan", "build") and the latest change report.
    void saveSnapshot(const std::string& kind, const InstallSnapshot& snapshot);
    std::optional<InstallSnapshot> loadSnapshot(const std::string& kind);
    void writeChanges(const ChangeReport& report);
    std::vector<std::string> resolutionStrings();

    // User decisions survive rescans because they are keyed by plugin name.
    void saveResolution(const std::string& originPlugin, uint32_t objectId, const std::string& winnerPlugin);
    void clearResolution(const std::string& originPlugin, uint32_t objectId);
    std::unordered_map<uint64_t, PluginIndex> loadResolutions(const Session& session);

private:
    void writePlugins(const Session& session);
    Database& db_;
};

} // namespace fo4mx::db
