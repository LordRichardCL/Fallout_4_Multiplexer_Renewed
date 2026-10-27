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

// A record type that keeps plugins out of compilation because it is not supported yet.
struct TypeBlocker {
    Signature sig;
    std::string name;
    size_t plugins = 0;     // plugins containing the type (otherwise eligible or not)
    size_t records = 0;     // records of the type in those plugins
    size_t soleBlocker = 0; // plugins for which it is the only unsupported type
    size_t unlocks = 0;     // plugins that would become eligible if only this type were supported
};

struct DependencyReport {
    std::vector<DependencyIssue> issues;
    size_t pluginEdges = 0;
    size_t recordEdges = 0;
    size_t danglingReferences = 0;
    size_t eligiblePlugins = 0;
    size_t eligibleRecords = 0;
    // Unsupported record types, most plugins unlocked first.
    std::vector<TypeBlocker> blockers;
    size_t blockedByTypes = 0;  // active, otherwise compilable plugins held back by record types
    size_t unlockedByAll = 0;   // plugins that supporting every blocking type would unlock
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
