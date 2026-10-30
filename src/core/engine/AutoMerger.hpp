#pragma once

#include "engine/CompilationEngine.hpp"
#include "engine/Session.hpp"

#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fo4mx {

// Advanced Users Auto-Merger (opt-in, CompileOptions::autoMerge).
//
// Plugins whose file name appears in scripts or config files are normally kept out of compilation:
// those files would keep pointing at a plugin that is no longer loaded. With the Auto-Merger on, each
// such file is checked, and a plugin whose files can all be relinked safely is compiled. After the
// compilation, relinked copies are written into the output folder, where they override the originals:
//   - scripts (.pex): Game.GetFormFromFile(id, "Plugin.esp") gets the output plugin and the record's
//     new FormID; other mentions of the plugin name point at the output that holds most of its records;
//   - MCM config.json: "Plugin.esp|FormID" pairs are rewritten, other mentions renamed;
//   - other configs (RobCo Patcher INIs, ...): "Plugin.esp|FormID" pairs are rewritten. A config that
//     names the plugin without a FormID is not changed and keeps the plugin out (its use is unknown).
// Cases that cannot be relinked keep their plugin out, with the reason: scripts that do not read back
// exactly, GetFormFromFile with a FormID or file name computed at run time, F4SE DLLs (binary) and
// Transfer Settlements blueprints (saved settlement data).
enum class RelinkKind : uint8_t { Script, McmConfig, PairConfig, Unsupported };
const char* relinkKindName(RelinkKind k);

struct RelinkFile {
    std::string path; // data-relative, lowercase
    std::string mod;  // mod providing the winning copy
    RelinkKind kind = RelinkKind::Unsupported;
    std::vector<PluginIndex> plugins; // plugins it names that could be compiled
    std::string problem;              // why it cannot be relinked (empty: it can)
    size_t formCalls = 0;             // GetFormFromFile calls naming those plugins
    size_t pairs = 0;                 // "Plugin|FormID" pairs
    size_t mentions = 0;              // other mentions of the plugin names
    // After relinking:
    bool written = false;
    size_t changedCalls = 0, changedPairs = 0, renamed = 0;
    std::vector<std::string> notes;
};

struct AutoMergeReport {
    bool enabled = false;
    std::vector<RelinkFile> files;
    std::map<PluginIndex, std::vector<size_t>> filesOf; // plugin -> indices into files
    std::set<PluginIndex> relinkable;                    // every file naming it can be relinked
    std::vector<std::filesystem::path> written;          // relinked copies of the last export
    std::vector<std::string> errors;                     // relinking failures at export
};

class AutoMerger {
public:
    // Checks every script/config that names a plugin (Session::nameReferences).
    static AutoMergeReport analyse(const Session& session);
    // Writes relinked copies of the files that name compiled plugins into `outputDir`, and removes
    // copies a previous export wrote that are no longer needed (listed in Multiplexer_Relinked.txt).
    static void relink(const Session& session, const CompiledOutput& compiled, const std::filesystem::path& outputDir,
                       AutoMergeReport& report);
};

} // namespace fo4mx
