#include "engine/CompilationResult.hpp"

#include "engine/CompilationEngine.hpp"
#include "engine/Exporter.hpp"
#include "engine/Session.hpp"
#include "engine/ValidationEngine.hpp"
#include "util/Text.hpp"

#include <unordered_set>

namespace fo4mx {

bool CompilationResult::compiledSuccessfully(const std::string& pluginName) const
{
    if (!success)
        return false;
    for (const auto& p : included)
        if (iequals(p.name, pluginName))
            return true;
    return false;
}

CompilationResult makeCompilationResult(const Session& session, const CompiledOutput* compiled,
                                        const ExportResult* exported, const ValidationReport* validation)
{
    CompilationResult r;
    std::unordered_set<PluginIndex> compiledSet;
    if (compiled)
        compiledSet.insert(compiled->compiledPlugins.begin(), compiled->compiledPlugins.end());

    for (const auto& p : session.plugins) {
        if (p.generated)
            continue;
        const bool inScope = !p.baseGame && !p.creationClub && !p.implicit;
        if (!p.active) {
            if (inScope)
                r.skipped.push_back({p.name, p.ghosted ? "ghosted" : "not active in the load order"});
            continue;
        }
        if (!inScope)
            continue;
        r.selected.push_back({p.name, {}});
        if (compiledSet.count(p.index)) {
            r.included.push_back({p.name, {}});
            continue;
        }
        if (auto it = session.autoRetained.find(p.key); it != session.autoRetained.end()) {
            r.failed.push_back({p.name, "kept out automatically: " + it->second});
            continue;
        }
        if (!p.error.empty()) {
            r.failed.push_back({p.name, p.error});
            continue;
        }
        std::string reason = eligibilityName(p.eligibility);
        if (!p.reasons.empty())
            reason += ": " + p.reasons.front();
        r.excluded.push_back({p.name, reason});
    }

    if (compiled) {
        size_t errors = 0, warnings = 0;
        for (const auto& i : compiled->issues) {
            if (i.severity == Severity::Error)
                ++errors;
            else if (i.severity == Severity::Warning)
                ++warnings;
        }
        if (errors)
            r.errors.push_back(std::to_string(errors) + " compilation error(s) (see the Compilation Report)");
        if (warnings)
            r.warnings.push_back(std::to_string(warnings) + " compilation warning(s) (see the Compilation Report)");
    } else {
        r.errors.push_back("nothing was compiled");
    }
    if (validation) {
        if (validation->errors)
            r.errors.push_back(std::to_string(validation->errors) + " validation error(s) (see the Validation Report)");
        if (validation->warnings)
            r.warnings.push_back(std::to_string(validation->warnings) +
                                 " validation warning(s) (see the Validation Report)");
    }
    if (exported) {
        r.outputPlugins = exported->pluginFiles;
        r.packageRoot = exported->packageRoot;
        r.dataFolder = exported->directory;
        r.zipPath = exported->zipPath;
        r.disableList = exported->disableList;
    } else {
        r.errors.push_back("the outputs were not exported");
    }
    r.success = compiled && !compiled->empty() && exported && !(validation && validation->critical()) &&
                !r.outputPlugins.empty();
    return r;
}

} // namespace fo4mx
