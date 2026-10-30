#pragma once

#include "engine/CompilationEngine.hpp"
#include "engine/Session.hpp"

#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace fo4mx {

// Advanced Users Auto-Merger (opt-in, CompileOptions::autoMerge).
//
// The ReferenceScanner classifies every place a plugin's file name appears. With the Auto-Merger on,
// a plugin whose references are all Safe or Rewritable is compiled, and exactly its Rewritable
// references are changed in copies written into the output folder, where they override the originals:
//   - scripts (.pex): Game.GetFormFromFile(<constant>, "Plugin.esp") gets the output plugin and the
//     record's new FormID; Game.IsPluginInstalled("Plugin.esp") gets the output plugin. Each use is
//     pointed at its own string, so a string the script uses elsewhere is never renamed;
//   - configs: "Plugin.esp|FormID" pairs get the output plugin and the new FormID; MCM
//     "pluginRequirements" entries get the output plugin.
// Nothing else in a file changes. Rewrites are planned and checked at compile time (plan()): the copy
// is scanned again, every expected change must be there, every other reference must be as it was and
// every other byte identical. A file that fails keeps its plugins out and the compilation runs again
// (fail closed). Export (relink()) is a transaction: all copies are written beside their targets and
// only then moved into place, earlier copies are restored if any step fails, and every change is
// recorded with its old and new value in Multiplexer_Relinked.tsv. The user's mods are never changed.
enum class RelinkKind : uint8_t { Script, McmConfig, PairConfig, Unsupported };
const char* relinkKindName(RelinkKind k);

// One change to one reference.
struct RelinkChange {
    PluginIndex plugin = kInvalidPlugin;
    std::string location; // "Object.State.Function", "line 3"
    std::string context;  // "Game.GetFormFromFile(00000812)", "Plugin|FormID pair", ...
    std::string before;   // "ModS.esp|812"
    std::string after;    // "Dummy_Plugin_01.esp|801"
};

struct RelinkFile {
    std::string path; // data-relative, lowercase
    std::string mod;  // mod providing the winning copy
    RelinkKind kind = RelinkKind::Unsupported;
    std::vector<PluginIndex> plugins;   // plugins it names
    std::vector<size_t> references;     // indices into Session::nameReferences
    ReferenceClass cls = ReferenceClass::Safe; // most restrictive class among them
    std::string problem;                // why it blocks its plugins (empty: only safe or rewritable)
    size_t formCalls = 0;               // rewritable GetFormFromFile calls
    size_t pairs = 0;                   // rewritable "Plugin|FormID" pairs
    size_t mentions = 0;                // rewritable presence checks (IsPluginInstalled, MCM requirements)
    // Plan (compile time):
    bool planned = false;               // a copy is to be written
    std::vector<RelinkChange> changes;
    std::vector<uint8_t> output;
    uint64_t originalHash = 0, outputHash = 0;
    std::string validation;             // why the planned copy failed its checks (empty: passed)
    // Export:
    bool written = false;
    size_t changedCalls = 0, changedPairs = 0, renamed = 0;
    std::vector<std::string> notes;
};

struct AutoMergeReport {
    bool enabled = false;
    std::vector<RelinkFile> files;
    std::map<PluginIndex, std::vector<size_t>> filesOf; // plugin -> indices into files
    std::set<PluginIndex> relinkable;                    // only safe or rewritable references
    std::map<PluginIndex, std::string> keptOut;          // compiled, then kept out by a failed check
    std::vector<std::filesystem::path> written;          // relinked copies of the last export
    std::vector<std::string> errors;                     // failures at export (nothing was changed)
};

class AutoMerger {
public:
    static constexpr const char* kManifest = "Multiplexer_Relinked.tsv";

    // Groups the classified references (Session::nameReferences) per file.
    static AutoMergeReport analyse(const Session& session);

    // Plans and checks the copies for a compilation. Returns the plugins whose files failed a check,
    // with the reason: they must be kept out and the compilation run again.
    // A file already in `outputDir` that the manifest does not list as an earlier copy fails the plan.
    static std::map<PluginIndex, std::string> plan(const Session& session, const CompiledOutput& compiled,
                                                   const std::filesystem::path& outputDir, AutoMergeReport& report);

    // Writes the planned copies into `outputDir` as one transaction, removes copies an earlier export
    // wrote that are no longer needed, and writes the manifest.
    static void relink(const Session& session, const std::filesystem::path& outputDir, AutoMergeReport& report);

    // Copies an earlier export wrote into `dir`: data-relative path -> content hash (0: not recorded).
    static std::unordered_map<std::string, uint64_t> readManifest(const std::filesystem::path& dir);

    // Removes every copy the manifest lists (unless changed since) and the manifest.
    static size_t rollback(const std::filesystem::path& outputDir);
};

} // namespace fo4mx
