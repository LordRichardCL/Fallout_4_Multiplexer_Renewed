#pragma once

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace fo4mx {

// A support bundle: one zip with what it takes to look into a problem with a build, ready to send. It holds
// the latest export's notes, reports and build records (FormID map, redirected copies, previs and relinked
// copies), the app's logs, the load order files, the newest crash log with its reading against the build,
// the newest Diagnostic Recorder session, and the settings. Never plugins, archives or other mod files.
// Personal details are taken out of every text file: the home folder and the user name become "<user>".
struct SupportBundleInputs {
    std::filesystem::path packageRoot;   // the latest export ("Multiplexer Output DDMMYY HHMMSS"), if any
    std::filesystem::path reportDirectory;
    std::vector<std::filesystem::path> logs;        // app logs
    std::vector<std::filesystem::path> loadOrder;   // plugins.txt, loadorder.txt, modlist.txt
    std::filesystem::path crashLog;                 // newest Buffout 4 log, if any
    std::string crashReading;                       // its reading against the build
    std::filesystem::path recorderSession;          // newest Diagnostic Recorder log, if any
    std::vector<std::pair<std::string, std::string>> settings; // shown as "key = value"
    std::vector<std::string> redact;                // more text to take out (besides home folder and user)
};

struct SupportBundleResult {
    std::filesystem::path zip;
    std::vector<std::string> included; // paths inside the zip
    std::vector<std::string> missing;  // what was asked for but not found (for the summary)
};

SupportBundleResult writeSupportBundle(const SupportBundleInputs& inputs, const std::filesystem::path& zip);

// Takes personal details out of text: each of `secrets` (and its forward-slash spelling) becomes "<user>".
std::string redactText(std::string text, const std::vector<std::string>& secrets);
// The home folder and user name of this account (for redactText).
std::vector<std::string> personalDetails();

} // namespace fo4mx
