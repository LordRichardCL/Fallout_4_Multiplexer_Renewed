#include "engine/PostCompilation.hpp"

#include "engine/AutoMerger.hpp"
#include "engine/CompilationEngine.hpp"
#include "engine/Exporter.hpp"
#include "engine/Mo2Support.hpp"
#include "engine/OutputRuns.hpp"
#include "engine/Session.hpp"
#include "util/FileTransaction.hpp"
#include "util/Text.hpp"
#include "vfs/GameInstall.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <unordered_map>
#include <unordered_set>

namespace fo4mx {

namespace fs = std::filesystem;

namespace {

std::string slurp(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool sameFile(const fs::path& a, const fs::path& b)
{
    std::error_code ec;
    if (!fs::is_regular_file(a, ec) || !fs::is_regular_file(b, ec))
        return false;
    if (fs::file_size(a, ec) != fs::file_size(b, ec))
        return false;
    return slurp(a) == slurp(b);
}

std::string norm(const fs::path& p)
{
    std::string g = toLower(pathToUtf8(p.lexically_normal().generic_u8string()));
    while (g.size() > 1 && g.back() == '/')
        g.pop_back();
    return g;
}

bool isLight(const PluginInfo& p)
{
    return p.kind == PluginKind::ESL || p.kind == PluginKind::LightESP || p.kind == PluginKind::LightESM;
}

// A plugin that stays in the game's load order after switching over.
bool staysActive(const PluginInfo& p, const CompiledOutput& c)
{
    return p.active && !p.generated && (!c.isCompiled(p.index) || c.hasArchiveLoader(p.index));
}

} // namespace

DeploymentCheck PostCompilation::check(const Session& session, const CompiledOutput& c, const CompilationResult& result,
                                       const std::string& assumeEnabledMod)
{
    DeploymentCheck d;
    const auto kind = session.install.config.kind;

    if (!result.success)
        d.blockers.push_back("this build did not finish successfully, so nothing may be switched over to it");

    // Load-order limits after switching over.
    for (const auto& p : session.plugins) {
        if (p.generated || !p.active)
            continue;
        if (c.isCompiled(p.index)) {
            if (c.hasArchiveLoader(p.index))
                ++d.lightPluginsAfter; // replaced by an empty ESL of the same name
            continue;
        }
        ++(isLight(p) ? d.lightPluginsAfter : d.fullPluginsAfter);
    }
    for (const auto& op : c.outputs)
        if (!op.records.empty())
            ++(op.light ? d.lightPluginsAfter : d.fullPluginsAfter);
    if (d.fullPluginsAfter > kMaxFullPlugins)
        d.blockers.push_back(std::to_string(d.fullPluginsAfter) + " full plugins would be active; Fallout 4 has " +
                             std::to_string(kMaxFullPlugins) +
                             " slots, and plugins past them take another plugin's index (crashes, wrong records)");
    if (d.lightPluginsAfter > kMaxLightPlugins)
        d.blockers.push_back(std::to_string(d.lightPluginsAfter) + " light plugins would be active; Fallout 4 has " +
                             std::to_string(kMaxLightPlugins) + " light slots");

    // Outputs the game can see. Under MO2: through the profile's enabled mods (read fresh).
    if (kind == InstallKind::ModOrganizer2 && !session.install.profilePath.empty())
        d.missingOutputs = mo2::outputsNotVisible(session, result.outputPlugins, assumeEnabledMod);
    if (kind != InstallKind::ModOrganizer2 && !session.install.dataPath.empty()) {
        const bool writtenIntoData = norm(result.dataFolder) == norm(session.install.dataPath);
        for (const auto& f : result.outputPlugins) {
            if (writtenIntoData)
                continue;
            if (!sameFile(f, session.install.dataPath / f.filename()))
                d.missingOutputs.push_back(pathToUtf8(f.filename()));
        }
    }
    {
        if (!d.missingOutputs.empty()) {
            const std::string what = std::to_string(d.missingOutputs.size()) +
                                     " output plugin(s) are not installed in the game's Data folder yet (or differ "
                                     "from this build, first: " + d.missingOutputs.front() + "): install " +
                                     (kind == InstallKind::ModOrganizer2 ? "Multiplexer Output.zip as a mod and enable it"
                                                                         : "Multiplexer Output.zip") +
                                     " first";
            // Apply to Game switches plugins.txt itself, which must never disable the compiled
            // plugins while the game cannot see their replacement.
            d.blockers.push_back(what);
        }
    }

    // The Auto-Merger's rewritten scripts and MCM configs: the copy the game loads must be ours. Under
    // Vortex the original mod's file can win the conflict, and MCM then looks for "Plugin.esl|F99" in a
    // plugin that is switched off (the menu is empty or its settings do nothing).
    if (d.missingOutputs.empty() && !result.dataFolder.empty()) {
        std::vector<fs::path> copies;
        for (const auto& [rel, hash] : AutoMerger::readManifest(result.dataFolder)) {
            (void)hash;
            if (auto p = findPathCaseInsensitive(result.dataFolder, rel); !p.empty())
                copies.push_back(p);
        }
        std::sort(copies.begin(), copies.end());
        if (kind == InstallKind::ModOrganizer2 && !session.install.profilePath.empty())
            d.relinkedNotLoaded = mo2::outputsNotVisible(session, copies, assumeEnabledMod, result.dataFolder);
        else if (kind != InstallKind::ModOrganizer2 && !session.install.dataPath.empty() &&
                 norm(result.dataFolder) != norm(session.install.dataPath))
            for (const auto& f : copies) {
                const auto rel = pathToUtf8(f.lexically_relative(result.dataFolder));
                const auto installed = findPathCaseInsensitive(session.install.dataPath, rel);
                if (installed.empty() || !sameFile(f, installed))
                    d.relinkedNotLoaded.push_back(rel);
            }
        // The F4SE archive loader and its list: without them the compiled plugins' archives past the 1,023
        // characters the game reads would never load.
        if (c.f4seArchiveLoader) {
            std::vector<fs::path> loader;
            for (const char* name : {Exporter::kArchiveLoaderDll, Exporter::kArchiveManifest})
                if (auto p = findPathCaseInsensitive(result.dataFolder, std::string("F4SE\\Plugins\\") + name); !p.empty())
                    loader.push_back(p);
            if (loader.size() < 2) {
                d.archiveLoaderNotLoaded.push_back("F4SE\\Plugins (missing from the build)");
            } else if (kind == InstallKind::ModOrganizer2 && !session.install.profilePath.empty()) {
                d.archiveLoaderNotLoaded = mo2::outputsNotVisible(session, loader, assumeEnabledMod, result.dataFolder);
            } else if (kind != InstallKind::ModOrganizer2 && !session.install.dataPath.empty() &&
                       norm(result.dataFolder) != norm(session.install.dataPath)) {
                for (const auto& f : loader) {
                    const auto rel = pathToUtf8(f.lexically_relative(result.dataFolder));
                    const auto installed = findPathCaseInsensitive(session.install.dataPath, rel);
                    if (installed.empty() || !sameFile(f, installed))
                        d.archiveLoaderNotLoaded.push_back(rel);
                }
            }
            if (!d.archiveLoaderNotLoaded.empty())
                d.blockers.push_back("the F4SE archive loader of this build is not installed in the game's Data folder yet "
                                     "(first: " + d.archiveLoaderNotLoaded.front() + "): install the package first, or "
                                     "the compiled plugins' archives past the first 1,023 characters of the list would "
                                     "never load");
            d.warnings.push_back("this build loads compiled plugins' archives through the F4SE archive loader: always "
                                 "start the game through F4SE (f4se_loader.exe, or your mod manager's F4SE entry)");
        }
        if (!d.relinkedNotLoaded.empty()) {
            std::string how;
            if (kind == InstallKind::Vortex)
                how = "in Vortex, open Mods, resolve the file conflicts so the output mod loads after the mods it "
                      "conflicts with (or always wins), then deploy";
            else if (kind == InstallKind::ModOrganizer2)
                how = "give the output mod a higher priority than the mods it conflicts with";
            else
                how = "install the package again, overwriting the existing files";
            d.blockers.push_back(std::to_string(d.relinkedNotLoaded.size()) +
                                 " script(s) or MCM config(s) rewritten by this build are not the copies the game "
                                 "loads (first: " + d.relinkedNotLoaded.front() +
                                 "): another mod's original wins and still names a compiled plugin, so MCM menus and "
                                 "scripts would look for records that are gone. " + how);
        }
    }

    // Compiled plugins still enabled in the plugins.txt the game (or the profile) uses.
    if (!session.install.pluginsFile.empty()) {
        std::unordered_set<std::string> enabled;
        for (const auto& e : parsePluginsTxt(slurp(session.install.pluginsFile)))
            if (e.active)
                enabled.insert(toLower(e.name));
        for (auto pi : c.compiledPlugins) {
            const auto& p = session.plugins[pi];
            if (enabled.count(p.key) && !c.hasArchiveLoader(pi))
                d.stillEnabled.push_back(p.name);
        }
        if (!d.stillEnabled.empty())
            d.warnings.push_back(std::to_string(d.stillEnabled.size()) +
                                 " compiled plugin(s) are still enabled in " +
                                 pathToUtf8(session.install.pluginsFile.filename()) +
                                 ": the game would load their records twice, once from them and once from the outputs");
    }
    return d;
}

std::string PostCompilation::instructions(const Session& session, const CompilationResult& r, const DeploymentCheck& d)
{
    const auto kind = session.install.config.kind;
    std::string t;
    t += "Fallout 4 Multiplexer Renewed: switching your game over to this build\n";
    t += "======================================================================\n\n";
    t += "Mod manager (from Settings): " + std::string(installKindName(kind)) + "\n";
    t += "Compiled plugins: " + std::to_string(r.included.size()) + " (listed in Multiplexer_DisablePlugins.txt)\n";
    t += "Output plugins: " + std::to_string(r.outputPlugins.size()) + "\n";
    t += "Kept in the load order: " + std::to_string(r.excluded.size() + r.failed.size()) + "\n";
    t += "Active plugins after switching: " + std::to_string(d.fullPluginsAfter) + " full (limit " +
         std::to_string(kMaxFullPlugins) + "), " + std::to_string(d.lightPluginsAfter) + " light\n\n";

    if (!d.blockers.empty()) {
        t += "DO NOT SWITCH OVER YET\n";
        for (const auto& b : d.blockers)
            t += "  - " + b + "\n";
        t += "\n";
    }
    if (!d.relinkedNotLoaded.empty()) {
        t += "Rewritten scripts and MCM configs the game does not load yet (another mod's copy wins)\n";
        for (size_t i = 0; i < d.relinkedNotLoaded.size() && i < 50; ++i)
            t += "  - " + d.relinkedNotLoaded[i] + "\n";
        if (d.relinkedNotLoaded.size() > 50)
            t += "  ... and " + std::to_string(d.relinkedNotLoaded.size() - 50) + " more\n";
        t += "\n";
    }
    if (!d.warnings.empty()) {
        t += "Check first\n";
        for (const auto& w : d.warnings)
            t += "  - " + w + "\n";
        t += "\n";
    }

    t += "Steps\n-----\n";
    switch (kind) {
    case InstallKind::ModOrganizer2:
        t += "1. Install the package as a mod: in Mod Organizer 2, Install a new mod from an archive and pick\n"
             "   \"Multiplexer Output.zip\" (Apply to Game can copy it into MO2's downloads folder first). Keep it at\n"
             "   the bottom of the left pane so it wins every file conflict. If your output folder is itself a\n"
             "   folder in MO2's mods folder, Apply to Game can enable that mod for you instead.\n"
             "2. Close Mod Organizer 2 (it rewrites the profile's plugins.txt and modlist.txt when it closes).\n"
             "3. In Multiplexer, Compilation page: Apply to Game. It disables the compiled plugins in the profile,\n"
             "   enables the outputs at the end and keeps the compiled mods' archives loading (backups first).\n"
             "   The mods themselves stay enabled: their meshes, textures and scripts are still used.\n";
        break;
    case InstallKind::Vortex:
        t += "1. In Multiplexer, Compilation page: Apply to Game. With your Vortex downloads folder set (Settings),\n"
             "   it copies \"Multiplexer Output.zip\" there; or drag the zip onto Vortex's Mods page yourself.\n"
             "2. In Vortex: install and enable the package, let it win every file conflict (load after all other\n"
             "   mods) and Deploy. Do not disable the original mods: their files are still used.\n"
             "3. Close Vortex.\n"
             "4. In Multiplexer: Apply to Game again. Once the outputs are deployed and Vortex is closed, it disables\n"
             "   the compiled plugins in plugins.txt, enables the outputs at the end and keeps the compiled mods'\n"
             "   archives loading (every file backed up first).\n"
             "5. Start Vortex and check its Plugins page: the plugins in Multiplexer_DisablePlugins.txt are disabled\n"
             "   and the outputs enabled. Do not enable the compiled plugins again.\n"
             "   (Without Multiplexer: disable the listed plugins and enable the outputs in Vortex's Plugins page.)\n";
        break;
    case InstallKind::Standalone:
        t += "1. Extract \"Multiplexer Output.zip\" into the game folder (its Data folder merges with the game's),\n"
             "   or copy the contents of this export's Data folder into the game's Data folder.\n"
             "2. In Multiplexer, Compilation page: Apply to Game. After a summary and your confirmation it disables\n"
             "   the compiled plugins in plugins.txt, enables the outputs at the end and keeps the compiled mods'\n"
             "   archives loading. Every file is backed up first.\n"
             "3. Optional, in the same step: ghost the compiled plugins (rename Example.esp to Example.esp.ghost) so\n"
             "   no tool enables them again by accident.\n";
        break;
    }
    t += "4. Keep the original mods installed: only their plugins are switched off; meshes, textures, archives,\n"
         "   scripts and sounds are still used.\n"
         "5. Start a new game. Saves made before switching refer to the old plugins.\n\n";

    t += "Undoing it\n----------\n";
    t += "- Every file this app changes is backed up beside it as <file>.multiplexer-backup-<date>-<time>; copy a\n"
         "  backup over the file to restore it.\n"
         "- A ghosted plugin is restored by renaming Example.esp.ghost back to Example.esp.\n"
         "- Disable the output plugins and enable the plugins in Multiplexer_DisablePlugins.txt again.\n";
    return t;
}

std::vector<GhostCheck> PostCompilation::ghostChecks(const Session& session, const CompiledOutput& c,
                                                     const CompilationResult& r, const DeploymentCheck& d)
{
    std::vector<GhostCheck> out;
    const auto kind = session.install.config.kind;

    // Masters of plugins that stay in the load order (and of the outputs) must stay loadable.
    std::unordered_map<std::string, std::string> neededBy; // lowercase master -> a plugin needing it
    for (const auto& p : session.plugins) {
        if (!staysActive(p, c))
            continue;
        for (const auto& m : p.header.masters)
            neededBy.emplace(toLower(m.name), p.name);
    }
    for (const auto& op : c.outputs)
        for (const auto& m : op.masterNames)
            neededBy.emplace(toLower(m), op.name);

    std::string global;
    if (kind == InstallKind::ModOrganizer2)
        global = "Mod Organizer 2 keeps disabled plugins switched off; ghosting is not used there";
    else if (!r.success)
        global = "the build did not finish successfully";
    else if (!d.blockers.empty())
        global = d.blockers.front();
    else if (!d.missingOutputs.empty())
        global = "the outputs are not installed in the game's Data folder yet";

    for (auto pi : c.compiledPlugins) {
        const auto& p = session.plugins[pi];
        GhostCheck g;
        g.plugin = p.name;
        g.file = p.path;
        g.ghost = p.path;
        if (!p.ghosted)
            g.ghost += std::string(kGhostSuffix);
        std::error_code ec;
        if (!global.empty())
            g.problem = global;
        else if (!r.compiledSuccessfully(p.name))
            g.problem = "not compiled successfully by this build";
        else if (p.ghosted)
            g.problem = "already ghosted";
        else if (p.generated || p.baseGame || p.creationClub || p.implicit)
            g.problem = "a base game, Creation Club or generated plugin";
        else if (c.hasArchiveLoader(pi))
            g.problem = "replaced by an archive loader of the same name";
        else if (kind == InstallKind::Vortex && session.install.vortexOwners.count(p.key))
            g.problem = "deployed by Vortex (" + session.install.vortexOwners.at(p.key) +
                        "): disable it in Vortex's Plugins page instead, or Vortex would restore it";
        else if (norm(p.path.parent_path()) != norm(session.install.dataPath))
            g.problem = "not directly in the game's Data folder (" + pathToUtf8(p.path.parent_path()) + ")";
        else if (!fs::is_regular_file(p.path, ec))
            g.problem = "the file is missing";
        else if (fs::file_size(p.path, ec) != p.fileSize)
            g.problem = "the file changed since the scan: scan and compile again";
        else if (fs::exists(g.ghost, ec))
            g.problem = pathToUtf8(g.ghost.filename()) + " already exists";
        else if (auto it = neededBy.find(p.key); it != neededBy.end())
            g.problem = "still a master of " + it->second + ", which stays in the load order";
        out.push_back(std::move(g));
    }
    return out;
}

ApplyPlan PostCompilation::planApply(FileTransaction& tx, const Session& session, const CompiledOutput& c,
                                     const CompilationResult& result, const fs::path& recordDirectory, bool ghost)
{
    ApplyOptions o;
    o.ghost = ghost;
    return planApply(tx, session, c, result, recordDirectory, o);
}

ApplyPlan PostCompilation::planApply(FileTransaction& tx, const Session& session, const CompiledOutput& c,
                                     const CompilationResult& result, const fs::path& recordDirectory,
                                     const ApplyOptions& o)
{
    ApplyPlan plan;
    const auto kind = session.install.config.kind;
    const bool enableMod = kind == InstallKind::ModOrganizer2 && !o.enableOutputMod.empty();
    plan.check = check(session, c, result, enableMod ? o.enableOutputMod : std::string());
    if (kind == InstallKind::Vortex && o.switchLoadOrder && o.managerRunning)
        plan.check.blockers.push_back("Vortex is running: close it first. Vortex keeps plugins.txt in step with its "
                                      "Plugins page and would overwrite the change");
    if (kind == InstallKind::ModOrganizer2 && o.switchLoadOrder && o.managerRunning)
        plan.check.blockers.push_back("Mod Organizer 2 is running: close it first. It writes the profile's plugins.txt "
                                      "and modlist.txt when it closes and would overwrite the change");
    plan.ghosts = ghostChecks(session, c, result, plan.check);

    // The package into Vortex's downloads folder: the step that makes the outputs installable.
    if (!o.zipTo.empty()) {
        std::error_code ec;
        if (!result.success)
            plan.zipProblem = "the build did not finish successfully";
        else if (result.zipPath.empty() || !fs::is_regular_file(result.zipPath, ec))
            plan.zipProblem = "Multiplexer Output.zip was not written by this export";
        else if (fs::exists(o.zipTo, ec) && !o.replaceZip)
            plan.zipProblem = pathToUtf8(o.zipTo.filename()) + " is already in the downloads folder";
        else {
            tx.copy(result.zipPath, o.zipTo, "the package, for Vortex to install", o.replaceZip);
            plan.copiesZip = true;
        }
    }
    if (!plan.check.blockers.empty())
        return plan; // nothing else is planned: the caller shows the blockers

    // The load order: the MO2 profile, or the plugins.txt the game reads (Standalone, and Vortex
    // while it is closed: Vortex reads plugins.txt back when it starts).
    if (enableMod) {
        // The new export's mod on; earlier exports' mods off (their folders stay as they are).
        const std::string keep = o.enableOutputMod;
        mo2::planEnableMod(tx, session.install.profilePath, o.enableOutputMod, [keep](const std::string& mod) {
            return output::isRunName(mod) && !iequals(mod, keep);
        });
        plan.enablesMod = true;
    }
    if (o.switchLoadOrder && !session.install.pluginsFile.empty()) {
        Exporter::planProfile(tx, session, c, session.install.pluginsFile);
        plan.writesLoadOrder = true;
    }
    // The record of archives added to the INI is kept beside the exports, never inside one: a finished
    // export is never changed (its instructions were written when it was exported).
    Exporter::planArchiveIni(tx, session, c, recordDirectory);
    if (o.ghost && kind != InstallKind::ModOrganizer2) {
        planGhosting(tx, plan.ghosts);
        for (const auto& g : plan.ghosts)
            plan.ghosted += g.ok() ? 1 : 0;
    }
    return plan;
}

void PostCompilation::planGhosting(FileTransaction& tx, const std::vector<GhostCheck>& checks)
{
    for (const auto& g : checks)
        if (g.ok())
            tx.rename(g.file, g.ghost, "ghost " + g.plugin + " (compiled into the outputs)");
}

} // namespace fo4mx
