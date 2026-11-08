#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace fo4mx {

// Before an export: the previous export's output is moved, unchanged, into a new folder
// "Multiplexer Outputs Backup DDMMYY HHMMSS" beside the output, so every export starts from an empty
// output folder and the package zip can only hold what the current export writes.
//
// What counts as the previous output:
// - package layout (the output folder is "...\Data", inside a package folder such as
//   "<game>\Multiplexer Output"): the Data folder, and in the package folder the zip, the notes
//   (Multiplexer_*.txt / .ini / .json, the build-failed note), "Multiplexer Reports" and "Diagnostic
//   Recorder". Anything else the user put in the package folder stays where it is.
// - an output folder picked by hand (not named Data, e.g. a mod folder in Mod Organizer 2's mods
//   folder): everything in it except Mod Organizer 2's meta.ini, provided Multiplexer made it (its
//   build records, zip, notes or plugins are there). A folder holding only files Multiplexer did not
//   make is refused instead of moved.
// The game's own Data folder (an export written straight into it) is never moved.
//
// All or nothing: a move that fails puts back what was already moved and removes the empty backup
// folder, and the export stops; nothing is deleted. An existing backup folder is never reused.
namespace output {

inline constexpr const char* kBackupPrefix = "Multiplexer Outputs Backup ";

struct BackupResult {
    bool moved = false;                   // a previous output was found and moved
    std::filesystem::path backupFolder;   // where it went
    std::vector<std::filesystem::path> entries; // what was moved (paths in the backup)
    bool gameDataOutput = false;          // the output folder is the game's Data folder: left as it is
};

// `stamp`: "DDMMYY HHMMSS" (empty: now, local time). `gameData`: the game's Data folder.
// Throws std::runtime_error, with nothing moved, when the previous output cannot be backed up.
BackupResult backupPreviousOutput(const std::filesystem::path& outputDirectory, const std::filesystem::path& gameData,
                                  const std::string& stamp = {});

// "DDMMYY HHMMSS", local time.
std::string backupStamp();

// True for a folder name this app gives backups ("Multiplexer Outputs Backup ...").
bool isBackupFolderName(const std::string& name);

// True when the output folder is the game's Data folder (or holds the base game's master).
bool isGameDataFolder(const std::filesystem::path& outputDirectory, const std::filesystem::path& gameData);

} // namespace output
} // namespace fo4mx
