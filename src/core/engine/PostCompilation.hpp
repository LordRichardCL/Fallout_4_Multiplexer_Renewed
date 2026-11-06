#pragma once

#include "engine/CompilationResult.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace fo4mx {

class Session;
class FileTransaction;
struct CompiledOutput;

// What the game's load order looks like once this build is switched on, checked before anything
// in the user's setup changes.
struct DeploymentCheck {
    // Outputs the game cannot see yet: not in its Data folder, or different from the package's copy.
    // Empty under Mod Organizer 2, whose virtual Data folder is only known to a scan.
    std::vector<std::string> missingOutputs;
    // Compiled plugins enabled in the plugins.txt on disk right now (they must be disabled).
    std::vector<std::string> stillEnabled;
    size_t fullPluginsAfter = 0;  // active full plugins once compiled plugins are off and outputs on
    size_t lightPluginsAfter = 0; // active light plugins (ESL, ESL-flagged) at the same point
    std::vector<std::string> blockers; // reasons nothing may be switched over
    std::vector<std::string> warnings;
};

// Whether one compiled plugin may be ghosted (renamed "<name>.ghost" so the game skips it).
struct GhostCheck {
    std::string plugin;
    std::filesystem::path file;  // the plugin file in Data
    std::filesystem::path ghost; // file + ".ghost"
    std::string problem;         // why not; empty when it may be ghosted
    bool ok() const { return problem.empty(); }
};

// What the user chose for Apply to Game.
struct ApplyOptions {
    bool switchLoadOrder = true; // disable the compiled plugins and enable the outputs in plugins.txt
    bool ghost = false;          // also ghost the compiled plugins that pass every check
    bool managerRunning = false; // the mod manager (Vortex) is running right now (the caller checks)
    std::filesystem::path zipTo; // copy the package zip here (a full file path); empty: no copy
    bool replaceZip = false;     // zipTo exists and the user chose to replace it (it is backed up)
};

// What Apply to Game will do, planned (not yet applied) into a transaction.
struct ApplyPlan {
    DeploymentCheck check;
    bool copiesZip = false;
    std::string zipProblem; // why the zip is not copied although it was asked for
    std::vector<GhostCheck> ghosts; // every compiled plugin, whether or not ghosting was asked for
    bool writesLoadOrder = false;   // plugins.txt (MO2 profile or Standalone) is part of the plan
    size_t ghosted = 0;             // plugins the plan renames to .ghost
};

// The steps after a compile + export that touch the user's setup. Every change is planned into a
// FileTransaction so it is shown before it happens and applied all or nothing.
class PostCompilation {
public:
    static constexpr size_t kMaxFullPlugins = 254;   // load-order slots 00-FD
    static constexpr size_t kMaxLightPlugins = 4096; // FE:000-FE:FFF
    static constexpr const char* kInstructionsFile = "Multiplexer_Instructions.txt";

    static DeploymentCheck check(const Session& session, const CompiledOutput& compiled, const CompilationResult& result);

    // A plain-language guide to switching this build on, for the selected mod manager.
    static std::string instructions(const Session& session, const CompilationResult& result,
                                    const DeploymentCheck& check);

    // Every plugin the build compiled, with whether it may be ghosted and why not.
    static std::vector<GhostCheck> ghostChecks(const Session& session, const CompiledOutput& compiled,
                                               const CompilationResult& result, const DeploymentCheck& check);
    // Plans Apply to Game: the load order (MO2 profile, or plugins.txt for a Standalone install), the
    // archive INI, the instructions file and, when asked, ghosting. Plans nothing when check() finds
    // a blocker.
    // The zip copy (Vortex downloads) is planned even when switching over is blocked: installing the
    // package is the step that unblocks it.
    static ApplyPlan planApply(FileTransaction& tx, const Session& session, const CompiledOutput& compiled,
                               const CompilationResult& result, const std::filesystem::path& outputDirectory,
                               const ApplyOptions& options);
    static ApplyPlan planApply(FileTransaction& tx, const Session& session, const CompiledOutput& compiled,
                               const CompilationResult& result, const std::filesystem::path& outputDirectory,
                               bool ghost);

    // Renames the plugins whose check passed. Never writes ".ghost" names into any load order file.
    static void planGhosting(FileTransaction& tx, const std::vector<GhostCheck>& checks);
};

} // namespace fo4mx
