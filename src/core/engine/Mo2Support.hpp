#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace fo4mx {

class Session;
class FileTransaction;

namespace mo2 {

inline constexpr const char* kProcessName = "ModOrganizer.exe";

// The mod this build's output folder is, when it is a folder directly inside MO2's mods folder
// (e.g. "<mods>\Multiplexer Output"); empty otherwise.
std::string outputModName(const Session& session, const std::filesystem::path& outputDirectory);

// One line of modlist.txt (highest priority first): '+' enabled, '-' disabled, '*' unmanaged.
struct ModListLine {
    char flag = '+';
    std::string name;
};
std::vector<ModListLine> readModList(const std::filesystem::path& profilePath);

// Whether the profile's modlist.txt (as on disk now) enables the mod.
bool modEnabled(const std::filesystem::path& profilePath, const std::string& modName);

// The output plugins the game would not see through MO2 right now: in no enabled mod of the
// profile (read fresh from modlist.txt), not in overwrite and not in the game's Data folder, or
// different from this build. `assumeEnabled` is a mod treated as enabled (it is about to be).
// With `base`, each output is looked up at its path relative to it (scripts, MCM configs) and
// reported by that path; without, by its file name in the root of Data.
std::vector<std::string> outputsNotVisible(const Session& session, const std::vector<std::filesystem::path>& outputs,
                                           const std::string& assumeEnabled = {},
                                           const std::filesystem::path& base = {});

// Enables `modName` in the profile's modlist.txt: a disabled entry is switched on where it is, a
// missing one is added at the top (highest priority, so it wins every file conflict). Every other
// line, its order and its state stay as they are.
// `alsoDisable`: other mods to switch off in the same edit (earlier Multiplexer exports: only one
// export's files should be in the game). Their folders are never touched.
void planEnableMod(FileTransaction& tx, const std::filesystem::path& profilePath, const std::string& modName,
                   const std::function<bool(const std::string&)>& alsoDisable = {});

// "N compiled plugins come from M mods": which mod provides each compiled plugin (MO2's winner)
// and how many plugins are provided by more than one mod.
struct Ownership {
    size_t plugins = 0;
    size_t mods = 0;
    size_t providedTwice = 0; // also present in a lower-priority mod (MO2 shows only the winner)
    std::vector<std::string> lines; // "Mod name: a.esp, b.esp" (first 30 mods)
};
Ownership ownership(const Session& session, const std::vector<std::string>& compiledPlugins);

} // namespace mo2
} // namespace fo4mx
