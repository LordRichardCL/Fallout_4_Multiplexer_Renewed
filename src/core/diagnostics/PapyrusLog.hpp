#pragma once

#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace fo4mx {
struct FormMapping;
class Session;
}

namespace fo4mx::diagnostics {

// A load order as the game numbers it: full plugins 00..FD by position, light plugins FE xxx.
struct RuntimeLoadOrder {
    std::vector<std::string> full;
    std::vector<std::string> light;
    // The plugin and object ID of a runtime FormID; plugin "" when its index is not in the lists,
    // "(created in game)" for FF.
    std::pair<std::string, uint32_t> resolve(uint32_t formId) const;
};

// Papyrus logs (Documents\My Games\Fallout4\Logs\Script\Papyrus.0.log) name objects by runtime FormID,
// "[Quest <MyQuest (0A001234)>]". Unlike a crash log they carry no plugin list, so they are read with the
// load order the game had, which is the current one when nothing changed since. Each FormID is translated
// through the build's FormID map when an output defines it, and the errors and warnings are counted.
struct PapyrusFormRef {
    std::string formId;   // 8 hex digits, as written
    std::string name;     // the object's name or type in the log ("MyQuest", "ObjectReference")
    std::string plugin;   // from the load order
    uint32_t objectId = 0;
    size_t mentions = 0;
    size_t inErrors = 0;  // mentions on error lines
    std::string firstLine;
    bool inOutput = false;
    std::string source;   // the original plugin, when translated
    uint32_t sourceObjectId = 0;
    std::string editorId;
    std::string signature;
    bool redirected = false;
};

struct PapyrusMessage {
    std::string text; // the message with numbers and FormIDs as written the first time
    size_t count = 0;
};

struct PapyrusReport {
    bool recognised = false; // has Papyrus log lines
    size_t lines = 0, errors = 0, warnings = 0;
    std::vector<PapyrusMessage> topErrors;   // most frequent first
    std::vector<PapyrusFormRef> refs;        // translated first, then most mentioned
    size_t translated = 0, untranslated = 0; // in an output: found in the map or not
};

PapyrusReport translatePapyrusLog(std::string_view text, const RuntimeLoadOrder& loadOrder,
                                  const std::vector<FormMapping>& formMap, const std::set<std::string>& redirected);
std::string formatPapyrusReport(const PapyrusReport& report, size_t limit = 100);

// The session's active load order, numbered as the game numbers it.
RuntimeLoadOrder runtimeLoadOrder(const Session& session);

// A build's FormID map and redirected copies from its export (its Data folder or root). False when the
// export holds no Multiplexer_FormMap.tsv.
bool readBuildMap(const std::filesystem::path& exportRoot, std::vector<FormMapping>& map,
                  std::set<std::string>& redirected);

// Documents\My Games\Fallout4\Logs\Script\Papyrus.0.log
std::filesystem::path defaultPapyrusLog();

} // namespace fo4mx::diagnostics
