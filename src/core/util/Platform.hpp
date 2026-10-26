#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace fo4mx::platform {

// "Documents/My Games/Fallout4" for the current user.
std::filesystem::path fallout4DocumentsDir();
// "%LOCALAPPDATA%/Fallout4" (where the game keeps plugins.txt).
std::filesystem::path fallout4LocalAppDataDir();
// "%LOCALAPPDATA%" or empty.
std::filesystem::path localAppDataDir();
// Install path from the registry (Steam / GOG), if any.
std::optional<std::filesystem::path> detectGamePath();
// File version of Fallout4.exe ("1.10.163.0"), or a best guess.
std::string detectGameVersion(const std::filesystem::path& gamePath);
// Per-user application data directory for our own settings and logs.
std::filesystem::path applicationDataDir();

} // namespace fo4mx::platform
