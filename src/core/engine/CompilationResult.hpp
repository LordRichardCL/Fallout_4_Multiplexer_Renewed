#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace fo4mx {

class Session;
struct CompiledOutput;
struct ExportResult;
struct ValidationReport;

// What one compile + export produced, in the terms the steps after it need (deployment to a mod
// manager, ghosting, the summary shown to the user). Built from the pipeline's results; the
// compiler itself does not know about it.
struct CompilationResult {
    struct Plugin {
        std::string name;
        std::string reason; // why it is in its list (empty for included plugins)
    };

    bool success = false; // compiled, validated and exported
    std::vector<std::filesystem::path> outputPlugins; // the plugins written into the package
    std::filesystem::path packageRoot;                // "Multiplexer Output"
    std::filesystem::path dataFolder;                 // "Multiplexer Output\Data"
    std::filesystem::path zipPath;                    // "Multiplexer Output.zip", when written
    std::filesystem::path disableList;                // Multiplexer_DisablePlugins.txt

    std::vector<Plugin> selected; // active plugins in scope (not base game, Creation Club or generated)
    std::vector<Plugin> included; // compiled into the outputs
    std::vector<Plugin> excluded; // in scope but not compiled by decision (retained, ineligible)
    std::vector<Plugin> skipped;  // not active in the load order
    std::vector<Plugin> failed;   // kept out automatically by validation, or unreadable
    std::vector<std::string> warnings;
    std::vector<std::string> errors;

    // True only for a plugin this successful build compiled into its outputs: the one test every
    // later step (disable, ghost) must pass before touching a plugin.
    bool compiledSuccessfully(const std::string& pluginName) const;
};

// `compiled`, `exported` and `validation` may be null (not run, or failed).
CompilationResult makeCompilationResult(const Session& session, const CompiledOutput* compiled,
                                        const ExportResult* exported, const ValidationReport* validation);

} // namespace fo4mx
