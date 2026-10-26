#pragma once

#include "db/Database.hpp"
#include "engine/CompilationEngine.hpp"
#include "engine/ConflictEngine.hpp"
#include "engine/DependencyEngine.hpp"
#include "engine/Session.hpp"
#include "engine/ValidationEngine.hpp"
#include "util/Progress.hpp"

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
    void updatePlugins(const Session& session);

    // User decisions survive rescans because they are keyed by plugin name.
    void saveResolution(const std::string& originPlugin, uint32_t objectId, const std::string& winnerPlugin);
    void clearResolution(const std::string& originPlugin, uint32_t objectId);
    std::unordered_map<uint64_t, PluginIndex> loadResolutions(const Session& session);

private:
    void writePlugins(const Session& session);
    Database& db_;
};

} // namespace fo4mx::db
