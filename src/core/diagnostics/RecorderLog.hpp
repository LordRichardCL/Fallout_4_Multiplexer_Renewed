#pragma once

#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fo4mx::diagnostics {

// One game session recorded by MultiplexerDiagnosticRecorder.dll (docs/DIAGNOSTIC_RECORDER.md):
// a JSON Lines file, one event per line. Unknown events and fields are ignored, so newer recorders
// stay readable.
struct RecorderSession {
    std::string file;
    bool started = false;  // session_start was recorded
    bool ended = false;    // session_end: the process exited normally
    std::string recorderVersion, runtimeVersion, f4seVersion, gameExeVersion, gameFolder, startTime;

    struct Message {
        std::string name; // post_load, game_data_ready, pre_load_game, ...
        std::optional<bool> flag; // success (post_load_game) or ready (game_data_ready)
        std::string save;         // pre_load_game, delete_game
    };
    std::vector<Message> messages;

    bool hasLoadOrder = false;
    std::vector<std::pair<std::string, bool>> plugins; // plugins.txt: name, active
    std::vector<std::pair<std::string, bool>> creationClub; // Fallout4.ccc: name, present
    bool hasF4sePlugins = false;
    std::vector<std::pair<std::string, bool>> pluginDlls; // Data\F4SE\Plugins\*.dll: name, loaded
    std::string addressLibraryFile;
    std::optional<bool> addressLibraryPresent;
    bool hasF4seLog = false;
    std::vector<std::string> f4seLines; // "plugin <dll> (...) <result>"
    std::vector<std::pair<std::string, std::string>> errors, warnings; // recorder's own: where, message
    std::vector<std::string> unreadable; // lines that are not JSON events
    std::string lastEvent;
    size_t events = 0;
};

RecorderSession parseRecorderLog(std::string_view text, std::string file = {});
RecorderSession loadRecorderLog(const std::filesystem::path& file);

// What Multiplexer built: read from the package's notes ("Multiplexer Output" folder).
struct PackageContext {
    std::filesystem::path root;
    std::vector<std::pair<std::string, bool>> expected; // Multiplexer_plugins.txt: name, active
    std::set<std::string> compiled;                     // Multiplexer_DisablePlugins.txt (lower case)
    std::vector<std::string> outputs;                   // plugins in the package's Data folder
    std::string expectedProfile; // "oldgen" (1.10.163) or "nextgen" (1.10.980 and later); empty: not checked
};
std::optional<PackageContext> loadPackageContext(const std::filesystem::path& packageRoot);

enum class FindingSeverity { Error, Warning, Info };
const char* findingSeverityName(FindingSeverity s);

// A recognised pattern: what it likely means, the evidence in the log, and what to check.
struct Finding {
    FindingSeverity severity = FindingSeverity::Info;
    std::string title;
    std::string cause;    // the likely cause, worded as likely, never as certain
    std::string confidence; // High, Medium, Low
    std::vector<std::string> evidence;
    std::vector<std::string> checks;
};

std::vector<Finding> analyse(const RecorderSession& session, const PackageContext* package = nullptr);

// The newest session file in the recorder's folder (empty when there is none).
std::filesystem::path newestSession(const std::filesystem::path& folder);
// Where the recorder writes: Documents\My Games\Fallout4\F4SE\MultiplexerDiagnosticRecorder.
std::filesystem::path defaultRecorderFolder();

// A plain-text report of a session and its findings (CLI, saved reports).
std::string formatReport(const RecorderSession& session, const std::vector<Finding>& findings);

} // namespace fo4mx::diagnostics
