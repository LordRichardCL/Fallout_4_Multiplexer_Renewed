#pragma once

#include "engine/Session.hpp"
#include "esp/FieldDecoder.hpp"
#include "util/Progress.hpp"

#include <string>
#include <vector>

namespace fo4mx {

struct AnalysisOptions {
    std::vector<std::string> excludedPlugins; // never compile these
    bool compileCreationClub = false;
    unsigned threads = 0;
};

struct DependencyIssue {
    Severity severity = Severity::Warning;
    std::string category; // "Missing Master", "Missing Reference", ...
    PluginIndex plugin = kInvalidPlugin;
    FormKey form;
    std::string message;
};

struct DependencyReport {
    std::vector<DependencyIssue> issues;
    size_t pluginEdges = 0;
    size_t recordEdges = 0;
    size_t danglingReferences = 0;
    size_t eligiblePlugins = 0;
    size_t eligibleRecords = 0;
};

// Stage 3: plugin- and record-level dependency graphs, missing requirements,
// potential breakages, and which plugins can safely be compiled.
class DependencyEngine {
public:
    static DependencyReport analyse(Session& session, const AnalysisOptions& options = {},
                                    ProgressSink& progress = nullProgress());
    static void evaluateEligibility(Session& session, const AnalysisOptions& options, DependencyReport* report = nullptr);

    // Plugins that list `plugin` as a master.
    static std::vector<PluginIndex> dependents(const Session& session, PluginIndex plugin);
};

} // namespace fo4mx
