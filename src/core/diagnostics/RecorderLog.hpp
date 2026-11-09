#pragma once

#include <cstdint>
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
    // Archive diagnostics (recorder 1.2.0).
    struct ArchiveIniValue {
        std::string file, key, value; // the [Archive] list as written in Fallout4.ini / Fallout4Custom.ini
    };
    std::vector<ArchiveIniValue> archiveIni;
    struct ArchiveIniRead {
        std::string section, key, file, value; // what the game received
        uint64_t buffer = 0, returned = 0;
        bool truncated = false;
    };
    std::vector<ArchiveIniRead> archiveIniReads;
    std::optional<bool> observesIniReads, observesOpens; // the import entries could be observed
    std::set<std::string> archivesOpened;               // BA2 file names (lower case) opened successfully
    std::vector<std::string> archivesFailed;            // BA2 paths that failed to open
    std::set<std::string> archivesHeld;                 // BA2 file names (lower case) held open
    bool hasArchiveHandles = false;
    std::optional<std::set<std::string>> archivesAbsent; // listed archives not in Data (lower case), when checked
    bool hasArchiveDiagnostics() const { return !archiveIni.empty() || observesIniReads.has_value(); }
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
    // Multiplexer_Archives.ini: the SResourceArchiveList2 value the build asked for, in order.
    std::vector<std::string> archiveList;
    std::set<std::string> registeredArchives; // the archives the build added to it (lower case)
    bool usesArchiveLoader = false;           // the build ships the F4SE archive loader and its list
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

// No session_end, but the game was quit to the desktop: a completed exit save, then only the deletion of
// the previous "Exitsave" (the game ends without the shutdown the recorder listens for).
bool quitAfterExitSave(const RecorderSession& session);

// Which requested archives loaded, compared layer by layer: requested by the build (or written in
// the INI), what the game received when it read the list, and what it opened.
struct ArchiveCheck {
    bool available = false;  // the session has archive diagnostics (recorder 1.2.0 or later)
    bool observable = false; // the game's own archives were seen loading: the method works here
    std::string key = "SResourceArchiveList2";
    std::string iniFile;     // the file whose value the game uses (Fallout4Custom.ini over Fallout4.ini)
    std::string iniValue;    // as written
    std::vector<std::string> iniEntries;
    std::vector<size_t> iniOffsets; // character position of each entry in iniValue
    bool readSeen = false;          // the game's read of the key was observed
    uint64_t buffer = 0, received = 0;
    bool truncated = false;
    std::vector<std::string> requested; // what Multiplexer asked for (else the INI entries)
    std::vector<std::string> loaded;
    std::vector<std::string> absent;  // not in Data at all: their mods are not installed or deployed
    bool absentInferred = false;      // no presence check in the log: absent = its plugin is not in plugins.txt
    std::vector<std::string> missing; // in Data (or not checked) but not loaded
    // Where loading stops: every listed archive in Data before this point loaded, none after it.
    bool cutInferred = false;
    size_t cutAfter = 0, cutBefore = 0; // characters: end of the last loaded entry, start of the first not loaded
    std::string firstMissing;
    size_t firstMissingIndex = 0, firstMissingOffset = 0; // in the INI value; offset npos: not in the INI
    bool addictol = false, xcell = false; // archive-limit plugins loaded in the session
    bool loader = false;                  // MultiplexerArchiveLoader loaded in the session
    bool cutCompensated = false;          // the read was cut, yet archives past the cut of inactive plugins loaded
    size_t pastCutWithPlugin = 0;         // archives past the cut that loaded with their own active plugin
    size_t archivesOpen = 0;              // BA2 files seen opened or held
    std::string verdict;
};
ArchiveCheck checkArchives(const RecorderSession& session, const PackageContext* package = nullptr);
std::string formatArchiveCheck(const ArchiveCheck& c);

// Archive-limit plugins installed in the game folder (the BA2 count limit: 256 per type on Old and Next
// Gen without one).
struct ArchiveLimitFix {
    bool addictol = false;
    bool addictolArchiveLimits = false; // [Patches] bArchiveLimits (on unless the TOML turns it off)
    bool xcell = false;
    std::string summary;
};
ArchiveLimitFix detectArchiveLimitFix(const std::filesystem::path& gamePath);

// The newest session file in the recorder's folder (empty when there is none).
std::filesystem::path newestSession(const std::filesystem::path& folder);
// Where the recorder writes: Documents\My Games\Fallout4\F4SE\MultiplexerDiagnosticRecorder.
std::filesystem::path defaultRecorderFolder();

// Whether the recorder is installed where F4SE looks, and whether F4SE loaded it. Answers "it
// captured nothing": the DLL in the wrong folder, F4SE refusing it, or the game not started through F4SE.
struct RecorderInstallStatus {
    std::filesystem::path expected;                // <game>\Data\F4SE\Plugins\MultiplexerDiagnosticRecorder.dll
    bool installed = false;                        // found there
    std::vector<std::filesystem::path> misplaced;  // found in a folder F4SE never reads
    std::filesystem::path f4seLog;                 // Documents\My Games\Fallout4\F4SE\f4se.log
    bool f4seLogFound = false;
    std::string f4seLine;                          // F4SE's line about the recorder (empty: not mentioned)
    bool loadedByF4se = false;                     // "... loaded correctly"
    std::filesystem::path statusLog;               // the recorder's own text log (1.1.0 and later)
    std::string statusText;                        // its contents
    std::filesystem::path newestSession;
    std::string archiveLimits;                     // archive-limit plugin found (Addictol), or none
    std::string verdict;                           // one sentence: what is wrong, or that it works
    bool ok = false;
};
// `f4seFolder`: Documents\My Games\Fallout4\F4SE (empty: the default). Under Mod Organizer 2 the DLL
// is inside a mod folder, so `installed` may be false while F4SE still loads it: its log decides.
RecorderInstallStatus checkRecorderInstall(const std::filesystem::path& gamePath,
                                           const std::filesystem::path& f4seFolder = {});
std::string formatInstallStatus(const RecorderInstallStatus& s);

// A plain-text report of a session and its findings (CLI, saved reports).
std::string formatReport(const RecorderSession& session, const std::vector<Finding>& findings,
                         const PackageContext* package = nullptr);

} // namespace fo4mx::diagnostics
