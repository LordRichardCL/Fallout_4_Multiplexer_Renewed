#pragma once

#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace fo4mx {
struct BuildComparison;
}

namespace fo4mx::diagnostics {

// A Fallout 4 save (.fos): its header and the plugins it was made with. Saves hold records by FormID, and a
// FormID names its plugin by that plugin's place in this list, so the list says which plugins a save needs.
struct SaveGameInfo {
    uint32_t version = 0;
    uint32_t saveNumber = 0;
    std::string player;
    uint32_t level = 0;
    std::string location;
    std::string playTime;
    uint8_t formVersion = 0;
    std::string gameVersion;
    std::vector<std::string> plugins;      // full plugins, in load order
    std::vector<std::string> lightPlugins; // light (ESL) plugins, in load order
};

// Throws std::runtime_error when the file is not a Fallout 4 save or is damaged.
SaveGameInfo readSaveGame(const std::filesystem::path& file);
SaveGameInfo parseSaveGame(const std::vector<uint8_t>& bytes);

// The save against the load order it would be loaded with: now, or after switching to the next build.
struct SaveCheckInput {
    std::vector<std::string> loadOrder;    // plugins that will load with their records
    std::vector<std::string> compiledAway; // plugins the next build compiles (their records get new FormIDs)
    std::set<std::string> outputs;         // this app's output plugins (earlier builds' and the next one's)
    const BuildComparison* comparison = nullptr; // the next build against the previous one, when compiled
    bool nextBuild = false;                // compared with the next build (else with the load order now)
};

struct SaveCheck {
    std::vector<std::string> missing;      // in the save, not loading: what the save holds of them is lost
    std::vector<std::string> compiledAway; // in the save, compiled by the next build: lost under their FormIDs
    std::vector<std::string> added;        // loading, not in the save (new to it: usually fine)
    std::vector<std::string> outputsInSave;
    size_t recordsChanged = 0; // records of the previous build that the next one moves, renumbers or drops
    bool safe = true;
};

SaveCheck checkSave(const SaveGameInfo& save, const SaveCheckInput& input);
std::string formatSaveCheck(const SaveGameInfo& save, const SaveCheck& check, bool nextBuild);

// The newest .fos in `folder` (default: Documents\My Games\Fallout4\Saves).
std::filesystem::path newestSave(const std::filesystem::path& folder = {});

} // namespace fo4mx::diagnostics
