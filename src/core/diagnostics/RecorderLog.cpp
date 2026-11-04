#include "diagnostics/RecorderLog.hpp"

#include "util/Json.hpp"
#include "util/Platform.hpp"
#include "util/Text.hpp"

#include <algorithm>
#include <fstream>
#include <map>
#include <sstream>

namespace fo4mx::diagnostics {

namespace fs = std::filesystem;

const char* findingSeverityName(FindingSeverity s)
{
    switch (s) {
    case FindingSeverity::Error: return "Error";
    case FindingSeverity::Warning: return "Warning";
    case FindingSeverity::Info: return "Info";
    }
    return "";
}

namespace {

std::string readAll(const fs::path& file)
{
    std::ifstream in(file, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

std::vector<std::string> textLines(std::string_view text)
{
    std::vector<std::string> out;
    for (auto& l : split(text, '\n', false)) {
        while (!l.empty() && (l.back() == '\r' || l.back() == ' '))
            l.pop_back();
        out.push_back(std::move(l));
    }
    return out;
}

// "*Name.esp" / "Name.esp" lines (plugins.txt format), comments skipped.
std::vector<std::pair<std::string, bool>> pluginsTxt(const std::string& text)
{
    std::vector<std::pair<std::string, bool>> out;
    for (const auto& raw : textLines(text)) {
        const std::string l = trim(raw);
        if (l.empty() || l[0] == '#')
            continue;
        if (l[0] == '*')
            out.emplace_back(trim(l.substr(1)), true);
        else
            out.emplace_back(l, false);
    }
    return out;
}

std::vector<std::pair<std::string, bool>> namedFlags(const json::Value& array, const char* flag)
{
    std::vector<std::pair<std::string, bool>> out;
    if (!array.isArray())
        return out;
    for (const auto& v : array.asArray())
        out.emplace_back(v["name"].asString(), v[flag].asBool());
    return out;
}

} // namespace

RecorderSession parseRecorderLog(std::string_view text, std::string file)
{
    RecorderSession s;
    s.file = std::move(file);
    for (const auto& line : textLines(text)) {
        if (line.empty())
            continue;
        json::Value e;
        try {
            e = json::Value::parse(line);
        } catch (const std::exception&) {
            // A line cut off when the game stopped mid-write is expected at the very end only.
            s.unreadable.push_back(line.substr(0, 120));
            continue;
        }
        const std::string event = e["event"].asString();
        if (event.empty()) {
            s.unreadable.push_back(line.substr(0, 120));
            continue;
        }
        ++s.events;
        s.lastEvent = event;
        if (event == "session_start") {
            s.started = true;
            s.recorderVersion = e["recorder_version"].asString();
            s.runtimeVersion = e["runtime_version"].asString();
            s.f4seVersion = e["f4se_version"].asString();
            s.gameExeVersion = e["game_exe_version"].asString();
            s.gameFolder = e["game_folder"].asString();
            s.startTime = e["time"].asString();
        } else if (event == "session_end") {
            s.ended = true;
        } else if (event == "f4se_message") {
            RecorderSession::Message m;
            m.name = e["message"].asString();
            if (e["success"].isBool())
                m.flag = e["success"].asBool();
            if (e["ready"].isBool())
                m.flag = e["ready"].asBool();
            m.save = e["save"].asString();
            s.messages.push_back(std::move(m));
        } else if (event == "load_order") {
            s.hasLoadOrder = true;
            s.plugins = namedFlags(e["plugins"], "active");
        } else if (event == "creation_club") {
            s.creationClub = namedFlags(e["plugins"], "present");
        } else if (event == "f4se_plugins") {
            s.hasF4sePlugins = true;
            s.pluginDlls = namedFlags(e["dlls"], "loaded");
            s.addressLibraryFile = e["address_library_file"].asString();
            if (e["address_library_present"].isBool())
                s.addressLibraryPresent = e["address_library_present"].asBool();
        } else if (event == "f4se_log") {
            s.hasF4seLog = true;
            if (e["plugin_lines"].isArray())
                for (const auto& l : e["plugin_lines"].asArray())
                    s.f4seLines.push_back(l.asString());
        } else if (event == "recorder_error") {
            s.errors.emplace_back(e["where"].asString(), e["message"].asString());
        } else if (event == "recorder_warning") {
            s.warnings.emplace_back(e["where"].asString(), e["message"].asString());
        }
    }
    return s;
}

RecorderSession loadRecorderLog(const fs::path& file)
{
    return parseRecorderLog(readAll(file), pathToUtf8(file));
}

std::optional<PackageContext> loadPackageContext(const fs::path& packageRoot)
{
    std::error_code ec;
    const auto plugins = packageRoot / "Multiplexer_plugins.txt";
    if (!fs::exists(plugins, ec))
        return std::nullopt;
    PackageContext c;
    c.root = packageRoot;
    c.expected = pluginsTxt(readAll(plugins));
    for (const auto& l : textLines(readAll(packageRoot / "Multiplexer_DisablePlugins.txt"))) {
        const std::string name = trim(l);
        if (!name.empty() && name[0] != '#')
            c.compiled.insert(toLower(name));
    }
    for (const auto& entry : fs::directory_iterator(packageRoot / "Data", ec)) {
        const std::string name = pathToUtf8(entry.path().filename());
        const std::string ext = toLower(entry.path().extension().string());
        if (entry.is_regular_file(ec) && (ext == ".esp" || ext == ".esm" || ext == ".esl"))
            c.outputs.push_back(name);
    }
    std::sort(c.outputs.begin(), c.outputs.end());
    return c;
}

namespace {

void add(std::vector<Finding>& out, FindingSeverity sev, std::string title, std::string cause, std::string confidence,
         std::vector<std::string> evidence, std::vector<std::string> checks)
{
    out.push_back({sev, std::move(title), std::move(cause), std::move(confidence), std::move(evidence), std::move(checks)});
}

const char* kCrashLogCheck =
    "Look for a crash log written at the same time (Buffout 4 writes Documents\\My Games\\Fallout4\\F4SE\\Crash-*.log): "
    "its call stack names the module that crashed.";

// The last F4SE messages, for evidence.
std::string recentMessages(const RecorderSession& s, size_t n = 4)
{
    std::vector<std::string> names;
    for (size_t i = s.messages.size() > n ? s.messages.size() - n : 0; i < s.messages.size(); ++i) {
        std::string m = s.messages[i].name;
        if (s.messages[i].flag)
            m += *s.messages[i].flag ? " (true)" : " (false)";
        names.push_back(m);
    }
    return names.empty() ? std::string("none") : join(names, ", ");
}

// What an F4SE plugin load line says went wrong.
std::pair<std::string, std::vector<std::string>> pluginLoadCause(const std::string& line, const RecorderSession& s)
{
    const std::string l = toLower(line);
    if (l.find("incompatible with current version of the game") != std::string::npos)
        return {"the plugin was built for another version of Fallout 4 (this session ran " + s.runtimeVersion + ")",
                {"Install the plugin's version for Fallout 4 " + s.runtimeVersion + ", or remove it."}};
    if (l.find("address library") != std::string::npos)
        return {"the Address Library data for this game version is missing",
                {"Install \"Address Library for F4SE Plugins\" for Fallout 4 " + s.runtimeVersion + "."}};
    if (l.find("no version data") != std::string::npos)
        return {"the plugin is for an older F4SE (Old Gen) and this F4SE (Next Gen) cannot load it",
                {"Install the plugin's Next Gen version, or remove it."}};
    if (l.find("couldn't load plugin") != std::string::npos && l.find(" 126 ") != std::string::npos)
        return {"Windows could not load a DLL the plugin needs (error 126: module not found)",
                {"Install what the plugin's page lists as requirements (often another F4SE plugin or the Microsoft "
                 "Visual C++ runtime)."}};
    if (l.find("couldn't load plugin") != std::string::npos && l.find(" 193 ") != std::string::npos)
        return {"the file is not a valid 64-bit DLL (error 193)", {"Reinstall the plugin; the file may be damaged."}};
    if (l.find("couldn't load plugin") != std::string::npos)
        return {"Windows could not load the DLL", {"Reinstall the plugin and install its requirements."}};
    if (l.find("reported as incompatible") != std::string::npos)
        return {"the plugin checked this game or F4SE version and declined to load",
                {"Check the plugin's page for the versions it supports."}};
    if (l.find("fatal error") != std::string::npos)
        return {"the plugin failed while F4SE was loading it", {"Update or remove the plugin."}};
    if (l.find("does not appear to be an f4se plugin") != std::string::npos)
        return {"a DLL in Data\\F4SE\\Plugins that is not an F4SE plugin (often a library another plugin uses)", {}};
    return {"F4SE did not load the plugin", {"Check the plugin's requirements and supported versions."}};
}

// "plugin Name.dll (...) result": the DLL's name.
std::string dllOfLine(const std::string& line)
{
    if (line.rfind("plugin ", 0) != 0)
        return {};
    const size_t end = line.find(' ', 7);
    return toLower(line.substr(7, end == std::string::npos ? std::string::npos : end - 7));
}

} // namespace

std::vector<Finding> analyse(const RecorderSession& s, const PackageContext* package)
{
    std::vector<Finding> out;

    if (!s.started) {
        add(out, FindingSeverity::Warning, "The log has no session start",
            "the file is not a Diagnostic Recorder session, or the recorder failed before it could write", "High",
            {s.events ? "first event: " + s.lastEvent : std::string("no events")}, {"Open a session-*.jsonl file."});
        return out;
    }
    for (const auto& [where, message] : s.errors)
        add(out, FindingSeverity::Warning, "The recorder could not record everything", message, "High",
            {"recorder_error in " + where}, {"The rest of the log is still valid; findings that need this part are skipped."});
    for (const auto& [where, message] : s.warnings)
        add(out, FindingSeverity::Info, "Recorder note", message, "High", {"recorder_warning in " + where}, {});

    // ---- How the session ended -----------------------------------------------------------
    auto last = [&](const std::string& name) -> long {
        for (size_t i = s.messages.size(); i-- > 0;)
            if (s.messages[i].name == name)
                return static_cast<long>(i);
        return -1;
    };
    const long preLoad = last("pre_load_game"), postLoad = last("post_load_game");
    const long preSave = last("pre_save_game"), postSave = last("post_save_game");
    long dataStart = -1, dataDone = -1;
    for (size_t i = 0; i < s.messages.size(); ++i)
        if (s.messages[i].name == "game_data_ready")
            (s.messages[i].flag.value_or(false) ? dataDone : dataStart) = static_cast<long>(i);
    const bool postLoaded = last("post_load") >= 0;
    if (!s.ended) {
        const std::vector<std::string> evidence = {"no session_end event: the process did not exit normally",
                                                   "last F4SE messages: " + recentMessages(s),
                                                   "last event recorded: " + s.lastEvent};
        if (preLoad > postLoad) {
            const std::string save = s.messages[static_cast<size_t>(preLoad)].save;
            auto ev = evidence;
            ev.push_back("save being loaded: " + (save.empty() ? std::string("(not recorded)") : save));
            add(out, FindingSeverity::Error, "The game stopped while loading a save",
                "a crash while the save was read: often the save refers to plugins, records or scripts that are no "
                "longer in the load order (a save made before the load order changed, for example before switching "
                "to the Multiplexer output)",
                "High", ev,
                {kCrashLogCheck,
                 "If the save was made before the compiled plugins were replaced by the output, start a new game: "
                 "compiled new records have new FormIDs.",
                 "Try loading an older save, or a new game, to see whether the load order itself starts."});
        } else if (preSave > postSave) {
            add(out, FindingSeverity::Error, "The game stopped while saving", "a crash during the save", "High", evidence,
                {kCrashLogCheck, "Check free disk space and that the Saves folder can be written."});
        } else if (dataStart >= 0 && dataDone < dataStart) {
            add(out, FindingSeverity::Error, "The game stopped while loading the plugins",
                "a crash while the game read its plugins (game data): commonly a plugin whose masters are missing or "
                "load after it, too many plugins, or a damaged plugin",
                "Medium", evidence,
                {kCrashLogCheck, "Check the load order below for missing masters and the plugin limits (254 full, "
                                 "4096 light plugins)."});
        } else if (!postLoaded) {
            add(out, FindingSeverity::Error, "The game stopped before F4SE finished loading its plugins",
                "an F4SE plugin crashed while loading, or the game was closed during start-up", "Medium", evidence,
                {kCrashLogCheck, "Check the F4SE plugin results below and f4se.log."});
        } else {
            add(out, FindingSeverity::Warning, "The session ended without a normal exit",
                "a crash, or the game was closed in a way that skips the normal shutdown (Task Manager, some quit "
                "paths): the log alone cannot tell which",
                "Low", evidence, {kCrashLogCheck});
        }
    }
    for (size_t i = 0; i < s.messages.size(); ++i)
        if (s.messages[i].name == "post_load_game" && s.messages[i].flag && !*s.messages[i].flag) {
            std::string save;
            for (size_t j = i; j-- > 0;)
                if (s.messages[j].name == "pre_load_game") {
                    save = s.messages[j].save;
                    break;
                }
            add(out, FindingSeverity::Error, "A save failed to load",
                "the game could not read the save (damaged, or made with plugins that are not in the load order)",
                "High", {"post_load_game reported failure" + (save.empty() ? std::string() : " for " + save)},
                {"Load another save or start a new game.", "If the save predates switching to the Multiplexer output, "
                                                           "it cannot be expected to load: start a new game."});
        }

    // ---- F4SE plugins ---------------------------------------------------------------------
    std::set<std::string> explained;
    for (const auto& line : s.f4seLines) {
        const std::string l = toLower(line);
        if (l.find("loaded correctly") != std::string::npos)
            continue;
        const auto [cause, checks] = pluginLoadCause(line, s);
        const std::string dll = dllOfLine(line);
        explained.insert(dll);
        const bool notPlugin = l.find("does not appear to be an f4se plugin") != std::string::npos;
        std::vector<std::string> evidence = {"f4se.log: " + line};
        if (cause.find("Address Library") != std::string::npos && s.addressLibraryPresent && !*s.addressLibraryPresent)
            evidence.push_back("the Address Library file " + s.addressLibraryFile + " is not in Data\\F4SE\\Plugins");
        add(out, notPlugin ? FindingSeverity::Info : FindingSeverity::Warning,
            "F4SE did not load " + (dll.empty() ? std::string("a plugin") : dll), cause, "High", evidence, checks);
    }
    for (const auto& [dll, loaded] : s.pluginDlls) {
        if (loaded || explained.count(toLower(dll)))
            continue;
        add(out, FindingSeverity::Warning, dll + " is in Data\\F4SE\\Plugins but was not loaded",
            "F4SE did not load it and f4se.log gave no reason the recorder could read", "Medium",
            {"present in the F4SE plugin folder, not among the loaded modules"},
            {"Check f4se.log for this DLL.", "Remove the file if it is left over from an uninstalled mod."});
    }
    if (s.addressLibraryPresent && !*s.addressLibraryPresent)
        add(out, FindingSeverity::Info, "No Address Library data for this game version",
            "plugins that need \"Address Library for F4SE Plugins\" will not load", "High",
            {s.addressLibraryFile + " is not in Data\\F4SE\\Plugins"}, {"Install it if an F4SE plugin asks for it."});

    // ---- The Multiplexer build ------------------------------------------------------------
    if (package && s.hasLoadOrder) {
        std::map<std::string, bool> active;
        for (const auto& [name, on] : s.plugins)
            active[toLower(name)] = on;
        std::vector<std::string> stillOn, outputOff, missing;
        for (const auto& name : package->compiled)
            if (auto it = active.find(name); it != active.end() && it->second)
                stillOn.push_back(name);
        for (const auto& name : package->outputs)
            if (auto it = active.find(toLower(name)); it == active.end() || !it->second)
                outputOff.push_back(name);
        std::set<std::string> outputs;
        for (const auto& o : package->outputs)
            outputs.insert(toLower(o));
        for (const auto& [name, on] : package->expected)
            if (on && !outputs.count(toLower(name)))
                if (auto it = active.find(toLower(name)); it == active.end() || !it->second)
                    missing.push_back(name);
        if (!stillOn.empty())
            add(out, FindingSeverity::Error, "Plugins compiled into the output are still enabled",
                "their records load twice: once from the plugin and once from the output, with different FormIDs for "
                "new records",
                "High", {std::to_string(stillOn.size()) + " active: " + join(std::vector<std::string>(stillOn.begin(), stillOn.begin() + std::min<size_t>(stillOn.size(), 12)), ", ")},
                {"Disable the plugins listed in Multiplexer_DisablePlugins.txt (Apply to Game does this)."});
        if (!outputOff.empty())
            add(out, FindingSeverity::Error, "An output plugin is not enabled",
                "the records of the compiled plugins are not loaded at all", "High",
                {"not active: " + join(outputOff, ", ")},
                {"Install the Multiplexer Output package and enable its plugins (Multiplexer_plugins.txt)."});
        if (!missing.empty())
            add(out, FindingSeverity::Warning, "The game's load order differs from the one Multiplexer built for",
                "plugins Multiplexer expected to be active are not", "High",
                {std::to_string(missing.size()) + " missing: " +
                 join(std::vector<std::string>(missing.begin(), missing.begin() + std::min<size_t>(missing.size(), 12)), ", ")},
                {"Build again with the current load order, or enable these plugins."});
        // Order of the plugins both lists have active.
        std::vector<std::string> expectedOrder, gameOrder;
        std::set<std::string> both;
        for (const auto& [name, on] : package->expected)
            if (on && active.count(toLower(name)) && active[toLower(name)])
                both.insert(toLower(name));
        for (const auto& [name, on] : package->expected)
            if (on && both.count(toLower(name)))
                expectedOrder.push_back(toLower(name));
        for (const auto& [name, on] : s.plugins)
            if (on && both.count(toLower(name)))
                gameOrder.push_back(toLower(name));
        for (size_t i = 0; i < expectedOrder.size() && i < gameOrder.size(); ++i)
            if (expectedOrder[i] != gameOrder[i]) {
                add(out, FindingSeverity::Warning, "The plugins load in a different order than Multiplexer built for",
                    "the output was built for one order; another plugin may now override it, or load before its "
                    "masters",
                    "High", {"first difference at position " + std::to_string(i + 1) + ": expected " + expectedOrder[i] +
                             ", the game loads " + gameOrder[i]},
                    {"Sort the load order as in Multiplexer_plugins.txt, or build again with the current order."});
                break;
            }
    }
    if (package && !package->expectedProfile.empty() && !s.runtimeVersion.empty()) {
        const bool oldGen = s.runtimeVersion.rfind("1.10.163", 0) == 0;
        const bool wantOld = package->expectedProfile == "oldgen";
        if (oldGen != wantOld)
            add(out, FindingSeverity::Warning, "The game version differs from the one Multiplexer is set up for",
                std::string("Multiplexer is set up for ") + (wantOld ? "Old Gen (1.10.163)" : "Next Gen") +
                    "; this session ran Fallout 4 " + s.runtimeVersion,
                "High", {"runtime_version " + s.runtimeVersion},
                {"Choose the matching game version in Settings and build again."});
    }

    if (!s.unreadable.empty())
        add(out, FindingSeverity::Info, "Some lines could not be read",
            s.ended ? "the file was edited or damaged" : "the last line was cut off when the game stopped", "High",
            {std::to_string(s.unreadable.size()) + " line(s), first: " + s.unreadable.front()}, {});

    std::stable_sort(out.begin(), out.end(), [](const Finding& a, const Finding& b) { return a.severity < b.severity; });
    return out;
}

fs::path newestSession(const fs::path& folder)
{
    fs::path best;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(folder, ec)) {
        const std::string name = pathToUtf8(e.path().filename());
        if (name.rfind("session-", 0) == 0 && iendsWith(name, ".jsonl") && (best.empty() || e.path().filename() > best.filename()))
            best = e.path();
    }
    return best;
}

fs::path defaultRecorderFolder()
{
    return platform::fallout4DocumentsDir() / "F4SE" / "MultiplexerDiagnosticRecorder";
}

std::string formatReport(const RecorderSession& s, const std::vector<Finding>& findings)
{
    std::ostringstream o;
    o << "Diagnostic Recorder session\n===========================\n";
    o << "File:      " << s.file << "\n";
    o << "Started:   " << s.startTime << "\n";
    o << "Game:      " << s.runtimeVersion << "    F4SE: " << s.f4seVersion << "    Recorder: " << s.recorderVersion << "\n";
    size_t active = 0;
    for (const auto& p : s.plugins)
        active += p.second;
    o << "Plugins:   " << active << " active of " << s.plugins.size() << " in plugins.txt\n";
    o << "Ended:     " << (s.ended ? "normally" : "without a normal exit") << " (" << s.messages.size()
      << " F4SE messages)\n\n";
    o << "Findings (" << findings.size() << ")\n-----------\n";
    for (const auto& f : findings) {
        o << "[" << findingSeverityName(f.severity) << "] " << f.title << "\n";
        o << "  Likely cause: " << f.cause << " (confidence: " << f.confidence << ")\n";
        for (const auto& e : f.evidence)
            o << "  Evidence: " << e << "\n";
        for (const auto& c : f.checks)
            o << "  Check: " << c << "\n";
        o << "\n";
    }
    if (findings.empty())
        o << "No recognised problems.\n";
    return o.str();
}

} // namespace fo4mx::diagnostics
