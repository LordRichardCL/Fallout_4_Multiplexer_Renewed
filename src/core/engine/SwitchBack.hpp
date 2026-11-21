#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace fo4mx {

class FileTransaction;

// "Switch back": undoes the last Apply to Game. Apply records what it changed (the files it wrote, the
// backups it made, what each file held afterwards, the plugins it ghosted) in Multiplexer_Applied.json in the
// folder holding the exports; switching back restores those files from their backups, removes the ones that
// did not exist before (renamed aside, never deleted) and renames ghosted plugins back. Copies of the
// package in a downloads folder are left (they change nothing in the game). Planned into a FileTransaction:
// shown before it happens, applied all or nothing, and itself backed up, so a switch back can be undone too.
namespace switch_back {

inline constexpr const char* kRecordFile = "Multiplexer_Applied.json";

struct AppliedFile {
    enum class Kind { Written, Ghosted } kind = Kind::Written;
    std::filesystem::path target; // the file Apply wrote, or the ".ghost" name it renamed a plugin to
    std::filesystem::path backup; // Written: the copy of the file before Apply (empty: there was none)
    std::filesystem::path original; // Ghosted: the plugin's name before
    std::string hashAfter;          // Written: the content Apply left (hex FNV-1a), to tell later changes
};

struct AppliedRecord {
    std::string when;  // backup stamp of the Apply
    std::string build; // the export it switched to
    bool switchedBack = false;
    std::vector<AppliedFile> files;
};

// Records a successful Apply to Game (only operations that changed something). Appends to the record file.
void record(const FileTransaction& applied, const std::filesystem::path& recordDirectory, const std::string& build);

// The latest Apply recorded in `recordDirectory`, if any.
std::optional<AppliedRecord> latest(const std::filesystem::path& recordDirectory);

struct Plan {
    bool possible = false;
    std::string nothing;               // why there is nothing to switch back (possible == false)
    std::vector<std::string> problems; // stop it (a backup is missing)
    std::vector<std::string> changedSince; // files changed after Apply: switching back replaces those changes
    AppliedRecord record;
};

// Plans switching back the latest Apply into `tx`.
Plan plan(FileTransaction& tx, const std::filesystem::path& recordDirectory);

// Marks the latest Apply as switched back (after `tx` was applied), so it is not offered again.
void markSwitchedBack(const std::filesystem::path& recordDirectory);

} // namespace switch_back

} // namespace fo4mx
