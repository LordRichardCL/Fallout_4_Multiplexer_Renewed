#pragma once

#include "diagnostics/PapyrusLog.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace fo4mx::diagnostics {

// Startup popups and MCM menus: did the quests a build compiled start, and did the menus it relinked come up?
//
// A Papyrus log only records a quest when something goes wrong, or when the game writes out the scripts it is
// running (on a save, or a stack dump). So a quest that started cleanly leaves no trace; the check reports what
// the log does prove: quests seen running, quests whose script could not attach (they never run, so any
// message they would show never appears), and message calls that found no message. MCM keeps no per-menu log;
// for the menus whose required plugin a build relinked it lists what to look for in game, with the scripts
// that registered for their settings.

struct StartupMessage {
    uint32_t formId = 0; // runtime FormID
    std::string editorId;
    std::string text;    // title, or the start of the body
};

struct StartupQuest {
    enum class State : uint8_t {
        NoTrace,          // nothing in the log: normal for a quest that started without problems
        SeenRunning,      // its scripts appear in a call stack: it started
        ScriptMissing,    // "Cannot open store": the mod does not ship the script, so it never runs
        ScriptWrongType,  // the script exists but extends another type
    };
    uint32_t formId = 0;  // runtime FormID
    std::string outputPlugin;
    std::string editorId;
    std::string source;   // the plugin it was compiled from; "" for an override of a plugin that stays
    bool startGameEnabled = false;
    std::vector<std::string> scripts;
    std::vector<StartupMessage> messageBoxes; // messages its scripts hold that show as a box
    State state = State::NoTrace;
    std::string failedScript;
};

// A Show() (or ShowAsHelpMessage / ShowOnPipboy) call on a message property that is None.
struct FailedPopup {
    std::string call;     // "Show"
    std::string caller;   // the object running the script, as the log names it
    uint32_t formId = 0;
    std::string plugin;   // from the load order
    bool inOutput = false;
    std::string source;   // the original plugin when an output defines it
    size_t count = 0;
};

// An MCM menu whose config requires a plugin a build compiled; MCM hides a menu whose required plugin is not
// active, so the build pointed the requirement at the output.
struct McmMenu {
    std::string config;   // its folder under MCM\Config
    std::string required; // the plugins the config named (", " between several)
    std::string now;      // the outputs it names now
    bool scriptRegistered = false; // a script registered for "OnMCMSettingChange|<config>" in the log
};

struct McmLogSummary {
    bool found = false;
    size_t settingFiles = 0;
    size_t settings = 0;
    std::vector<std::string> warnings;
};

struct StartupCheckInputs {
    std::filesystem::path exportRoot; // the build to check (its Data folder holds the outputs and their records)
    std::string papyrusLog;           // Papyrus.0.log text; empty when there is none
    std::string mcmLog;               // MCM.log text; empty when there is none
    RuntimeLoadOrder loadOrder;       // the game's load order: numbers the outputs and their masters
};

struct StartupCheck {
    bool buildFound = false;
    bool outputsActive = false;  // the outputs are in the load order (else the log cannot name their records)
    bool logRead = false;
    size_t startupQuests = 0;    // start-game-enabled quests in the outputs
    size_t seenRunning = 0;
    size_t notStarted = 0;       // scripts that could not attach
    size_t withMessageBoxes = 0;
    std::vector<StartupQuest> quests; // the notable ones: could not start, seen running, or holding message boxes
    std::vector<FailedPopup> failedPopups;
    std::vector<McmMenu> mcmMenus;
    size_t mcmReferences = 0;    // Plugin|FormID pairs relinked in MCM configs
    McmLogSummary mcm;
};

StartupCheck checkStartup(const StartupCheckInputs& in);
std::string formatStartupCheck(const StartupCheck& check);

// The load order the game had, from f4se.log: the plugin list F4SE writes with each save ("[0A] Mod.esp",
// "[FE:012] Light.esl"). Empty lists when the log holds none.
RuntimeLoadOrder loadOrderFromF4seLog(std::string_view text);

// Documents\My Games\Fallout4\F4SE\MCM.log
std::filesystem::path defaultMcmLog();

} // namespace fo4mx::diagnostics
