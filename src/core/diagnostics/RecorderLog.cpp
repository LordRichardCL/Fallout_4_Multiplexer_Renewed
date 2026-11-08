#include "diagnostics/RecorderLog.hpp"
#include "diagnostics/RecorderPackage.hpp"

#include "engine/OutputRuns.hpp"

#include "util/Json.hpp"
#include "util/Platform.hpp"
#include "util/Text.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
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

std::string fileNameOf(const std::string& path)
{
    const size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// "a.ba2, b.ba2" -> entries and the character position where each starts.
void splitArchiveList(const std::string& value, std::vector<std::string>& names, std::vector<size_t>& offsets)
{
    size_t start = 0;
    while (start <= value.size()) {
        size_t comma = value.find(',', start);
        if (comma == std::string::npos)
            comma = value.size();
        size_t b = start, e = comma;
        while (b < e && (value[b] == ' ' || value[b] == '\t'))
            ++b;
        while (e > b && (value[e - 1] == ' ' || value[e - 1] == '\t'))
            --e;
        if (e > b) {
            names.push_back(value.substr(b, e - b));
            offsets.push_back(b);
        }
        start = comma + 1;
    }
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
        } else if (event == "archive_ini") {
            s.archiveIni.push_back({e["file"].asString(), e["key"].asString(), e["value"].asString()});
        } else if (event == "archive_ini_read") {
            RecorderSession::ArchiveIniRead r;
            r.section = e["section"].asString();
            r.key = e["key"].asString();
            r.file = e["file"].asString();
            r.value = e["value"].asString();
            r.buffer = static_cast<uint64_t>(e["buffer"].asNumber());
            r.returned = static_cast<uint64_t>(e["returned"].asNumber());
            r.truncated = e["truncated"].asBool();
            s.archiveIniReads.push_back(std::move(r));
        } else if (event == "archive_observers") {
            s.observesIniReads = e["ini_reads"].asBool();
            s.observesOpens = e["open_a"].asBool() || e["open_w"].asBool();
        } else if (event == "archive_open") {
            const std::string path = e["path"].asString();
            if (e["write"].asBool())
                ; // written, not loaded (an installer or tool): not evidence of the game loading it
            else if (e["ok"].asBool())
                s.archivesOpened.insert(toLower(fileNameOf(path)));
            else
                s.archivesFailed.push_back(path);
        } else if (event == "archive_presence") {
            std::set<std::string> absent;
            if (e["missing"].isArray())
                for (const auto& a : e["missing"].asArray())
                    absent.insert(toLower(a.asString()));
            s.archivesAbsent = std::move(absent);
        } else if (event == "archive_handles") {
            s.hasArchiveHandles = true;
            if (e["archives"].isArray())
                for (const auto& a : e["archives"].asArray())
                    s.archivesHeld.insert(toLower(fileNameOf(a.asString())));
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

std::optional<PackageContext> loadPackageContext(const fs::path& given)
{
    std::error_code ec;
    // The folder holding the exports: the latest finished export in it.
    fs::path packageRoot = given;
    if (!fs::exists(packageRoot / "Multiplexer_plugins.txt", ec))
        if (auto latest = output::latestRun(output::Location{given, output::Layout::Package}))
            packageRoot = *latest;
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
    // Plugins are in its Data folder, or at the top for an export that is a Mod Organizer 2 mod.
    const fs::path data = fs::is_directory(packageRoot / "Data", ec) ? packageRoot / "Data" : packageRoot;
    for (const auto& entry : fs::directory_iterator(data, ec)) {
        const std::string name = pathToUtf8(entry.path().filename());
        const std::string ext = toLower(entry.path().extension().string());
        if (entry.is_regular_file(ec) && (ext == ".esp" || ext == ".esm" || ext == ".esl"))
            c.outputs.push_back(name);
    }
    std::sort(c.outputs.begin(), c.outputs.end());
    // The archive list the build asked for, and the archives it added (kept beside the exports).
    for (const auto& l : textLines(readAll(packageRoot / "Multiplexer_Archives.ini"))) {
        const auto eq = l.find('=');
        if (eq != std::string::npos && iequals(trim(l.substr(0, eq)), "SResourceArchiveList2")) {
            std::vector<size_t> offsets;
            splitArchiveList(l.substr(eq + 1), c.archiveList, offsets);
        }
    }
    for (const auto& dir : {packageRoot, packageRoot.parent_path()})
        for (const auto& l : textLines(readAll(dir / "Multiplexer_RegisteredArchives.txt"))) {
            const std::string name = trim(l);
            if (!name.empty() && name[0] != '#')
                c.registeredArchives.insert(toLower(name));
        }
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

    if (s.hasArchiveDiagnostics()) {
        const auto c = checkArchives(s, package);
        if (c.truncated) {
            std::vector<std::string> ev = {
                "the game read " + c.key + " (" + c.iniFile + ") with a " + std::to_string(c.buffer) +
                "-byte buffer and received " + std::to_string(c.received) + " of " + std::to_string(c.iniValue.size()) +
                " characters"};
            for (size_t i = 0; i < c.iniEntries.size(); ++i)
                if (c.iniOffsets[i] + c.iniEntries[i].size() > c.received) {
                    ev.push_back("first archive past the cut: " + c.iniEntries[i] + " (entry " + std::to_string(i + 1) +
                                 " of " + std::to_string(c.iniEntries.size()) + ", starting at character " +
                                 std::to_string(c.iniOffsets[i]) + ")");
                    break;
                }
            add(out, FindingSeverity::Error, "The game read only part of the archive list",
                "Fallout 4 reads " + c.key + " into a fixed buffer and every archive after the cut is never loaded: "
                "their scripts, meshes and textures are missing in game",
                "High", ev,
                {"Build again with the current version of Multiplexer: it keeps the list within what the game reads.",
                 "Plugins whose archives do not fit stay in the load order until a longer list can be loaded."});
        }
        if (c.observable && c.cutInferred && !c.truncated)
            add(out, FindingSeverity::Error, "The game stopped loading the archive list part way",
                "every archive listed in " + c.key + " before character " + std::to_string(c.cutAfter) +
                    " loaded and none after character " + std::to_string(c.cutBefore) +
                    ": the game read only the start of the list (it reads INI values into a fixed buffer)",
                "High",
                {std::to_string(c.iniValue.size()) + " characters in the INI (" + c.iniFile + ")",
                 "first archive not loaded: " + c.firstMissing + " (entry " + std::to_string(c.firstMissingIndex + 1) +
                     ", character " + std::to_string(c.firstMissingOffset) + ")",
                 c.readSeen ? "the game's read was observed" : "the game's read itself was not observed (another plugin, "
                                                              "such as X-Cell's Profile patch, reads the INI for it)"},
                {"Build again with the current version of Multiplexer: it keeps the list within what the game reads."});
        if (c.observable && !c.absent.empty()) {
            std::vector<std::string> ev = {std::to_string(c.absent.size()) + " of " + std::to_string(c.requested.size()) +
                                           " listed archives are not in Data"};
            for (size_t i = 0; i < c.absent.size() && i < 10; ++i)
                ev.push_back("not in Data: " + c.absent[i]);
            if (c.absentInferred)
                ev.push_back("inferred: their plugins are not in plugins.txt (recorder 1.2.0 builds before the presence "
                             "check); a newer recorder checks Data directly");
            add(out, FindingSeverity::Error, "Listed archives are not installed",
                "the mods providing them are not installed or not deployed. Compiled plugins must be disabled as "
                "plugins only; their mods must stay installed and deployed, because the archives (textures, meshes, "
                "scripts) still come from them",
                c.absentInferred ? "Medium" : "High", ev,
                {"Vortex: enable and deploy those mods again, and disable only the plugins (Plugins page, or Apply to Game).",
                 "Mod Organizer 2: tick the mods again in the left pane and untick only their plugins."});
        }
        if (c.observable && !c.missing.empty()) {
            std::vector<std::string> ev = {std::to_string(c.missing.size()) + " of " + std::to_string(c.requested.size()) +
                                           " requested archives in Data were not opened by the game"};
            ev.push_back("first missing: " + c.firstMissing +
                         (c.firstMissingOffset != std::string::npos
                              ? " (entry " + std::to_string(c.firstMissingIndex + 1) + ", character " +
                                    std::to_string(c.firstMissingOffset) + " of the INI value)"
                              : " (not in the INI archive list)"));
            for (size_t i = 0; i < c.missing.size() && i < 10; ++i)
                ev.push_back("missing: " + c.missing[i]);
            add(out, FindingSeverity::Error, "Requested archives did not load",
                c.truncated || c.cutInferred ? "the archive list was cut when the game read it"
                                             : "the archives were not opened: a name that does not match, or an archive "
                                               "limit",
                c.truncated || c.cutInferred ? "High" : "Medium", ev,
                {"Check that the archives exist in Data under these names.",
                 "Open the Diagnostics page for the full archive comparison."});
        }
        if (!c.observable)
            add(out, FindingSeverity::Info, "Archive loading could not be verified in this session",
                "the game's own archives were not seen opening (another plugin may observe the same files first)",
                "Medium", {c.verdict}, {});
        if (c.archivesOpen > 230 && !c.addictol && !c.xcell)
            add(out, FindingSeverity::Warning, "Close to the archive limit",
                "Fallout 4 1.10.163 and 1.10.984 load at most 256 general and 256 texture archives; past that the game "
                "stops at start-up. No archive-limit plugin (Addictol) was loaded",
                "Medium", {std::to_string(c.archivesOpen) + " BA2 archives opened"},
                {"Install Addictol (its Archive Limits module is on by default)."});
    }

    if (!s.unreadable.empty())
        add(out, FindingSeverity::Info, "Some lines could not be read",
            s.ended ? "the file was edited or damaged" : "the last line was cut off when the game stopped", "High",
            {std::to_string(s.unreadable.size()) + " line(s), first: " + s.unreadable.front()}, {});

    std::stable_sort(out.begin(), out.end(), [](const Finding& a, const Finding& b) { return a.severity < b.severity; });
    return out;
}

ArchiveCheck checkArchives(const RecorderSession& s, const PackageContext* package)
{
    ArchiveCheck c;
    c.available = s.hasArchiveDiagnostics();
    for (const auto& [name, loaded] : s.pluginDlls) {
        const std::string n = toLower(name);
        c.addictol |= loaded && n.find("addictol") != std::string::npos;
        c.xcell |= loaded && n.rfind("x-cell", 0) == 0;
    }
    if (!c.available) {
        c.verdict = "this session has no archive diagnostics (recorder 1.2.0 or later)";
        return c;
    }
    // The value the game uses: Fallout4Custom.ini over Fallout4.ini.
    const RecorderSession::ArchiveIniValue* chosen = nullptr;
    for (const auto& v : s.archiveIni)
        if (iequals(v.key, c.key) && (!chosen || iequals(v.file, "Fallout4Custom.ini")))
            chosen = &v;
    if (chosen) {
        c.iniFile = chosen->file;
        c.iniValue = chosen->value;
        splitArchiveList(c.iniValue, c.iniEntries, c.iniOffsets);
    }
    // The game's read of it: the one from the same file, else the last.
    const RecorderSession::ArchiveIniRead* read = nullptr;
    for (const auto& r : s.archiveIniReads)
        if (iequals(r.key, c.key) && (!read || iequals(r.file, c.iniFile)))
            read = &r;
    if (read) {
        c.readSeen = true;
        c.buffer = read->buffer;
        c.received = read->value.size();
        c.truncated = read->truncated && c.received < c.iniValue.size();
    }
    std::set<std::string> seen = s.archivesOpened;
    seen.insert(s.archivesHeld.begin(), s.archivesHeld.end());
    c.archivesOpen = seen.size();
    for (const auto& a : seen)
        c.observable |= a.rfind("fallout4 - ", 0) == 0;
    c.requested = package && !package->archiveList.empty() ? package->archiveList : c.iniEntries;
    // Not in Data: from the recorder's own check; for logs without it, an archive named after a plugin that
    // is not in plugins.txt at all (Vortex and MO2 list every installed plugin, enabled or not).
    std::set<std::string> absentSet;
    if (s.archivesAbsent) {
        absentSet = *s.archivesAbsent;
    } else if (s.hasLoadOrder) {
        c.absentInferred = true;
        std::set<std::string> bases;
        for (const auto& [name, active] : s.plugins) {
            const auto dot = name.rfind('.');
            bases.insert(toLower(dot == std::string::npos ? name : name.substr(0, dot)));
        }
        for (const auto& a : c.requested) {
            const std::string n = toLower(a);
            const auto dash = n.rfind(" - ");
            if (n.rfind("fallout4 - ", 0) == 0 || n.rfind("dlc", 0) == 0 || dash == std::string::npos)
                continue;
            if (!bases.count(n.substr(0, dash)))
                absentSet.insert(n);
        }
    }
    for (const auto& a : c.requested) {
        if (seen.count(toLower(a)))
            c.loaded.push_back(a);
        else if (absentSet.count(toLower(a)))
            c.absent.push_back(a);
        else
            c.missing.push_back(a);
    }
    // Where loading stops, from the archives themselves (also when another plugin hid the INI read): every
    // listed archive in Data before a point loaded and none after it.
    if (c.observable && !c.missing.empty()) {
        bool anyLoaded = false, ordered = true, pastLoaded = false;
        size_t lastLoadedEnd = 0, firstNotLoaded = std::string::npos;
        for (size_t i = 0; i < c.iniEntries.size(); ++i) {
            const std::string n = toLower(c.iniEntries[i]);
            if (absentSet.count(n))
                continue;
            if (seen.count(n)) {
                if (pastLoaded)
                    ordered = false;
                anyLoaded = true;
                lastLoadedEnd = c.iniOffsets[i] + c.iniEntries[i].size();
            } else {
                pastLoaded = true;
                if (firstNotLoaded == std::string::npos)
                    firstNotLoaded = c.iniOffsets[i];
            }
        }
        if (anyLoaded && ordered && firstNotLoaded != std::string::npos) {
            c.cutInferred = true;
            c.cutAfter = lastLoadedEnd;
            c.cutBefore = firstNotLoaded;
        }
    }
    c.firstMissingOffset = std::string::npos;
    if (c.observable && !c.missing.empty()) {
        std::set<std::string> missing;
        for (const auto& m : c.missing)
            missing.insert(toLower(m));
        for (size_t i = 0; i < c.iniEntries.size(); ++i)
            if (missing.count(toLower(c.iniEntries[i]))) {
                c.firstMissing = c.iniEntries[i];
                c.firstMissingIndex = i;
                c.firstMissingOffset = c.iniOffsets[i];
                break;
            }
        if (c.firstMissing.empty())
            c.firstMissing = c.missing.front();
    }
    const size_t notLoaded = c.missing.size() + c.absent.size();
    if (!c.observable)
        c.verdict = "cannot verify: the game's own archives were not seen opening in this session";
    else if (notLoaded == 0)
        c.verdict = "PASS: all " + std::to_string(c.requested.size()) + " requested archives loaded";
    else {
        c.verdict = "FAIL: " + std::to_string(notLoaded) + " of " + std::to_string(c.requested.size()) +
                    " requested archives did not load";
        if (!c.absent.empty())
            c.verdict += "; " + std::to_string(c.absent.size()) + " are not in Data";
        if (c.truncated)
            c.verdict += "; archive-list truncation: the game received " + std::to_string(c.received) + " of " +
                         std::to_string(c.iniValue.size()) + " characters";
        else if (c.cutInferred)
            c.verdict += "; possible archive-list truncation: loading stops between characters " +
                         std::to_string(c.cutAfter) + " and " + std::to_string(c.cutBefore);
    }
    return c;
}

std::string formatArchiveCheck(const ArchiveCheck& c)
{
    std::ostringstream o;
    o << "Archive compatibility\n---------------------\n";
    o << "Addictol loaded:      " << (c.addictol ? "Yes" : "No") << (c.xcell ? " (X-Cell loaded)" : "") << "\n";
    if (!c.available) {
        o << c.verdict << "\n";
        return o.str();
    }
    o << "INI " << c.key << ": " << c.iniEntries.size() << " entries, " << c.iniValue.size() << " characters"
      << (c.iniFile.empty() ? std::string() : " (" + c.iniFile + ")") << "\n";
    if (c.readSeen)
        o << "Received by the game: " << c.received << " characters (buffer " << c.buffer << ")"
          << (c.truncated ? " - CUT" : "") << "\n";
    else
        o << "Received by the game: not observed\n";
    o << "Requested archives:   " << c.requested.size() << "\n";
    o << "Runtime verified:     " << c.loaded.size() << "\n";
    o << "Missing:              " << (c.missing.size() + c.absent.size()) << "\n";
    if (!c.absent.empty())
        o << "  Not in Data:        " << c.absent.size()
          << (c.absentInferred ? " (probably: their plugins are not in plugins.txt, so their mods are not installed)"
                               : " (their mods are not installed or deployed)")
          << "\n";
    if (!c.missing.empty())
        o << "  In Data, not loaded: " << c.missing.size() << "\n";
    if (c.cutInferred)
        o << "Loading stops:        between character " << c.cutAfter << " and " << c.cutBefore
          << " (every listed archive in Data before it loaded, none after)\n";
    o << "BA2 files opened:     " << c.archivesOpen << "\n";
    o << "Archive loading:      " << c.verdict << "\n";
    if (!c.firstMissing.empty()) {
        o << "First missing:        " << c.firstMissing << "\n";
        if (c.firstMissingOffset != std::string::npos)
            o << "  Archive index:      " << (c.firstMissingIndex + 1) << "\n  String position:    "
              << c.firstMissingOffset << "\n";
        for (const auto& m : c.missing)
            o << "  not loaded: " << m << "\n";
    }
    for (const auto& a : c.absent)
        o << "  not in Data: " << a << "\n";
    return o.str();
}

ArchiveLimitFix detectArchiveLimitFix(const fs::path& gamePath)
{
    ArchiveLimitFix f;
    if (gamePath.empty())
        return f;
    const auto plugins = findPathCaseInsensitive(gamePath, "Data\\F4SE\\Plugins");
    std::error_code ec;
    if (!plugins.empty())
        for (const auto& e : fs::directory_iterator(plugins, ec)) {
            const std::string n = toLower(pathToUtf8(e.path().filename()));
            if (n == "addictol.dll")
                f.addictol = true;
            else if (n.rfind("x-cell", 0) == 0 && iendsWith(n, ".dll"))
                f.xcell = true;
        }
    if (f.addictol) {
        // Addictol.toml: [Patches] bArchiveLimits, on unless set to false.
        f.addictolArchiveLimits = true;
        for (const auto& l : textLines(readAll(findPathCaseInsensitive(plugins, "Addictol.toml")))) {
            std::string t = toLower(trim(l));
            t.erase(std::remove(t.begin(), t.end(), ' '), t.end());
            if (t.rfind("barchivelimits=", 0) == 0)
                f.addictolArchiveLimits = t.find("false") == std::string::npos && t.find("=0") == std::string::npos;
        }
    }
    f.summary = f.addictol ? (f.addictolArchiveLimits ? "Addictol installed, Archive Limits on: no 256-archive limit"
                                                      : "Addictol installed, but its Archive Limits module is off")
                : f.xcell  ? "X-Cell installed (its archive-limit patch applies only to Fallout 4 1.11.191)"
                           : "no archive-limit plugin: Fallout 4 1.10.163 and 1.10.984 load at most 256 general and "
                             "256 texture archives";
    return f;
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

RecorderInstallStatus checkRecorderInstall(const fs::path& gamePath, const fs::path& f4seFolderIn)
{
    RecorderInstallStatus st;
    const std::string dll = kRecorderDllName;
    const fs::path f4seFolder = f4seFolderIn.empty() ? defaultRecorderFolder().parent_path() : f4seFolderIn;
    std::error_code ec;
    if (!gamePath.empty()) {
        st.expected = gamePath / "Data" / "F4SE" / "Plugins" / dll;
        st.installed = !findPathCaseInsensitive(gamePath, "Data\\F4SE\\Plugins\\" + dll).empty();
        for (const char* wrong : {"", "Data\\", "Data\\F4SE\\", "F4SE\\Plugins\\", "Data\\Plugins\\"})
            if (auto p = findPathCaseInsensitive(gamePath, std::string(wrong) + dll); !p.empty())
                st.misplaced.push_back(p);
    }
    st.f4seLog = f4seFolder / "f4se.log";
    std::string text;
    if (std::ifstream in{st.f4seLog, std::ios::binary}) {
        st.f4seLogFound = true;
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    // The last line naming the DLL holds F4SE's result ("loaded correctly", "disabled, ...", "couldn't load").
    {
        std::istringstream in(text);
        for (std::string line; std::getline(in, line);) {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (toLower(line).find("multiplexerdiagnosticrecorder") != std::string::npos)
                st.f4seLine = line;
        }
    }
    st.loadedByF4se = toLower(st.f4seLine).find("loaded correctly") != std::string::npos;
    st.statusLog = f4seFolder / "MultiplexerDiagnosticRecorder.log";
    if (std::ifstream in{st.statusLog, std::ios::binary})
        st.statusText.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    st.newestSession = newestSession(f4seFolder / "MultiplexerDiagnosticRecorder");
    st.archiveLimits = detectArchiveLimitFix(gamePath).summary;

    if (st.loadedByF4se) {
        st.ok = true;
        st.verdict = "F4SE loaded the recorder in the last game session";
        if (st.newestSession.empty())
            st.verdict += ", but no session log was found in " +
                          pathToUtf8(f4seFolder / "MultiplexerDiagnosticRecorder");
    } else if (!st.f4seLine.empty()) {
        st.verdict = "F4SE found the recorder but did not load it: \"" + trim(st.f4seLine) + "\"";
    } else if (!st.installed && !st.misplaced.empty()) {
        st.verdict = "the recorder is in a folder F4SE never reads (" + pathToUtf8(st.misplaced.front()) +
                     "): move it to " + pathToUtf8(st.expected);
    } else if (!st.f4seLogFound) {
        st.verdict = "no f4se.log in " + pathToUtf8(f4seFolder) +
                     ": the game has not been started through F4SE (f4se_loader.exe or the mod manager's F4SE entry)";
    } else if (st.installed) {
        st.verdict = "the recorder is installed, but the last F4SE session did not mention it: start the game "
                     "through F4SE again (f4se.log is rewritten on every start)";
    } else {
        st.verdict = "F4SE did not see the recorder: it is not in " + pathToUtf8(st.expected) +
                     " (under Mod Organizer 2: enable the mod holding Data\\F4SE\\Plugins\\" + dll +
                     "; under Vortex: deploy it)";
    }
    return st;
}

std::string formatInstallStatus(const RecorderInstallStatus& s)
{
    std::ostringstream o;
    o << "Diagnostic Recorder installation\n================================\n";
    o << "Verdict:   " << (s.ok ? "OK: " : "PROBLEM: ") << s.verdict << "\n";
    o << "Expected:  " << pathToUtf8(s.expected) << (s.installed ? " (present)" : " (not there)") << "\n";
    for (const auto& m : s.misplaced)
        o << "Misplaced: " << pathToUtf8(m) << " (F4SE does not read this folder)\n";
    o << "f4se.log:  " << pathToUtf8(s.f4seLog) << (s.f4seLogFound ? "" : " (not found)") << "\n";
    o << "F4SE says: " << (s.f4seLine.empty() ? std::string("(nothing about the recorder)") : trim(s.f4seLine)) << "\n";
    if (!s.archiveLimits.empty())
        o << "Archive limit: " << s.archiveLimits << "\n";
    o << "Newest session log: " << (s.newestSession.empty() ? std::string("(none)") : pathToUtf8(s.newestSession))
      << "\n";
    if (!s.statusText.empty())
        o << "\nThe recorder's own log (" << pathToUtf8(s.statusLog) << "):\n" << s.statusText;
    return o.str();
}

std::string formatReport(const RecorderSession& s, const std::vector<Finding>& findings, const PackageContext* package)
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
    if (s.hasArchiveDiagnostics())
        o << formatArchiveCheck(checkArchives(s, package)) << "\n";
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
