#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fo4mx {

class Session;

// Every export writes into a folder of its own, "Multiplexer Output DDMMYY HHMMSS", inside the
// output root (the Output folder setting; default "<game>\Multiplexer Output"). It is written as
// "<name>.tmp" and renamed to "<name>" only when complete, so an export never needs to move, rename,
// delete or change an earlier one, and an unfinished one can never pass for a finished one.
// "Multiplexer Latest.txt" in the root names the latest finished export.
namespace output {

inline constexpr const char* kRunPrefix = "Multiplexer Output ";
inline constexpr const char* kTmpSuffix = ".tmp";
inline constexpr const char* kLatestFile = "Multiplexer Latest.txt";
inline constexpr const char* kRunMarker = "Multiplexer_Run.txt"; // written into every export folder

enum class Layout {
    Package,   // <root>\<export>\Data (+ zip, notes, reports beside Data)
    ModFolder, // the root is Mod Organizer 2's mods folder: each export is a mod, its files at the top
    GameData,  // the Output folder is the game's own Data folder: written there directly (no exports)
};

struct Location {
    std::filesystem::path root;
    Layout layout = Layout::Package;
};

// Where exports go for the Output folder setting `configured` (empty: the default).
// - the game's Data folder: GameData;
// - a folder in Mod Organizer 2's mods folder: ModFolder, the root being the mods folder;
// - a folder named "Data" (the earlier package setting "...\Multiplexer Output\Data"): its parent;
// - anything else: that folder.
Location locate(const std::filesystem::path& configured, const std::filesystem::path& gamePath,
                const std::filesystem::path& gameData, const std::filesystem::path& modsPath);
Location locate(const Session& session, const std::filesystem::path& configured);

// "Multiplexer Output DDMMYY HHMMSS" or "... (n)": an export folder's name (without .tmp).
bool isRunName(const std::string& name);
// "DDMMYY HHMMSS", local time.
std::string runStamp();

struct Run {
    std::string name;                 // "Multiplexer Output 081126 163020"
    std::filesystem::path folder;     // while running: "<root>\<name>.tmp"; after finishRun: "<root>\<name>"
    Layout layout = Layout::Package;
    std::filesystem::path root;
    bool finished = false;
    std::filesystem::path dataDir() const; // where the plugins go
};

// Creates "<root>\<name>.tmp" for a new export: never an existing folder (a name already used,
// finished or not, gets " (2)", " (3)", ...). Throws when the folder cannot be created.
Run beginRun(const Location& location, const std::string& stamp = {});

// Renames "<name>.tmp" to "<name>" (retrying briefly when a scanner holds a new file). The finished
// name is never one that exists. Throws when it cannot be renamed: the export then has not finished.
void finishRun(Run& run);

// Records `run` as the latest finished export ("Multiplexer Latest.txt", written whole or not at all).
// Returns false when it could not be written; latestRun() then still finds the run by its name.
bool setLatest(const Run& run);

// The latest finished export folder: the one named in "Multiplexer Latest.txt" when it is a finished
// export, otherwise the newest finished export by the date and time in its name.
std::optional<std::filesystem::path> latestRun(const Location& location);

// The folder of an export where its plugins are.
std::filesystem::path dataDir(const std::filesystem::path& runFolder, Layout layout);

// Retention, separate from exporting: deletes finished exports beyond the newest `keep` (never the
// latest) and, when asked, unfinished ones (".tmp"). Only folders whose name is an export's and that
// hold this app's export marker are touched. Each is first renamed (a folder in use cannot be, and is
// skipped untouched), then deleted.
struct CleanupResult {
    std::vector<std::filesystem::path> removed;
    std::vector<std::pair<std::filesystem::path, std::string>> skipped; // folder, why
};
CleanupResult cleanupRuns(const Location& location, size_t keep, bool removeUnfinished);

} // namespace output
} // namespace fo4mx
