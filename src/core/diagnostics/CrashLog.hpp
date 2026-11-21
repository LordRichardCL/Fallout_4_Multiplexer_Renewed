#pragma once

#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace fo4mx {
struct FormMapping;
}

namespace fo4mx::diagnostics {

// Buffout 4 crash logs (Documents\My Games\Fallout4\F4SE\Crash-*.log) read against a Multiplexer build: a
// crash log names the output plugins and their FormIDs (Dummy_Plugin_03.esp, 0x0A0012AB), which say nothing
// about the mod behind them. Every FormID the log names is read with the log's own PLUGINS list (the load
// order the game had) and, where it is a record an output defines, translated through the build's FormID map
// to the original mod, object ID, Editor ID and type. Records of other plugins that an output or a
// redirected copy changes are marked as such.
struct CrashFormRef {
    std::string formId;     // as the log writes it (8 hex digits)
    std::string plugin;     // the plugin the FormID belongs to, from the log's PLUGINS list
    uint32_t objectId = 0;  // within that plugin
    std::string where;      // the line the log gives it under (register or stack slot)
    std::string type;       // FormType (kREFR, ...)
    std::string name;       // Name: "..."
    std::string modifiedBy; // Modified by: ... (as written)
    bool inOutput = false;  // defined by an output plugin of the build
    std::string source;     // the original plugin, when translated
    uint32_t sourceObjectId = 0;
    std::string editorId;
    std::string signature;
    bool changedByOutput = false; // a record of another plugin that an output changes
    bool redirected = false;      // the plugin is a redirected copy (Master Redirection)
};

struct CrashReport {
    bool recognised = false; // a Buffout 4 crash log
    std::string exception;   // Unhandled exception ... line
    std::string game;        // Fallout 4 v...
    size_t pluginsListed = 0;
    std::vector<std::string> outputsLoaded; // output plugins in the log's PLUGINS list
    std::vector<CrashFormRef> refs;          // every FormID named, first mention first
    size_t translated = 0;
    size_t untranslated = 0; // in an output, but not in this build's FormID map (another build?)
};

// `outputs`: the build's output plugin names (lowercase or not); `redirected`: its redirected copies.
CrashReport translateCrashLog(std::string_view text, const std::vector<FormMapping>& formMap,
                              const std::set<std::string>& outputs, const std::set<std::string>& redirected);
std::string formatCrashReport(const CrashReport& report);

// The newest Crash-*.log in `folder` (default: Documents\My Games\Fallout4\F4SE).
std::filesystem::path newestCrashLog(const std::filesystem::path& folder = {});

} // namespace fo4mx::diagnostics
