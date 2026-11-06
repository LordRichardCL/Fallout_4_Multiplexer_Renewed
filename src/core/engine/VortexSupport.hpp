#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace fo4mx::vortex {

// Vortex keeps downloaded archives in "<downloads>\<game id>"; for Fallout 4 the game id is
// "fallout4". Archives placed there are listed on Vortex's Downloads page (after a restart of
// Vortex at the latest), ready to install.
inline constexpr const char* kGameId = "fallout4";
inline constexpr const char* kProcessName = "Vortex.exe";

// Vortex's default: %APPDATA%\Vortex\downloads\fallout4. Empty when that folder does not exist.
std::filesystem::path suggestedDownloadsFolder();

struct FolderCheck {
    std::vector<std::string> problems; // the folder cannot be used
    std::vector<std::string> notes;    // it can, but look again
    bool ok() const { return problems.empty(); }
};
// Exists, is a folder, can be written to; notes when it is not Vortex's per-game folder.
FolderCheck checkDownloadsFolder(const std::filesystem::path& folder);

// What to do when the archive is already in the downloads folder.
enum class Collision { Replace, KeepBoth, Cancel };
// "Multiplexer Output (2).zip", "(3)", ...: the first name not taken in `folder`.
std::filesystem::path uniqueName(const std::filesystem::path& folder, const std::string& fileName);

} // namespace fo4mx::vortex
