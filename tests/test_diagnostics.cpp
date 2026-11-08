// Diagnostic Recorder logs (MultiplexerDiagnosticRecorder.dll): parsing and the recognised patterns.
// The log lines follow the exact format the DLL writes (checked by running it under a simulated F4SE).

#include "Fixtures.hpp"
#include "Test.hpp"

#include "diagnostics/RecorderLog.hpp"

#include <fstream>

using namespace fo4mx::diagnostics;
using namespace fo4mx::fixtures;

namespace {

const std::string kStart =
    R"({"schema":1,"seq":0,"time":"2026-11-04T21:13:09.013Z","ms":4,"event":"session_start","recorder_version":"1.0.0","runtime_version":"1.10.163.0","f4se_version":"0.6.23.0","f4se_release_index":42,"game_folder":"E:\\Fallout 4\\","game_exe_version":"1.10.163.0","process_id":332})"
    "\n";
const std::string kPostLoad = R"({"schema":1,"seq":1,"event":"f4se_message","message":"post_load","type":0})" "\n";
const std::string kLoadOrder =
    R"({"schema":1,"seq":2,"event":"load_order","source":"plugins.txt","entries":4,"active":3,"plugins":[{"name":"Fallout4.esm","active":true},{"name":"ModA.esp","active":true},{"name":"Dummy_Plugin_01.esp","active":false},{"name":"ModB.esp","active":true}]})"
    "\n";
const std::string kF4sePlugins =
    R"({"schema":1,"seq":5,"event":"f4se_plugins","dlls":[{"name":"BrokenPlugin.dll","loaded":false},{"name":"Lonely.dll","loaded":false},{"name":"MultiplexerDiagnosticRecorder.dll","loaded":true}],"address_library_file":"version-1-10-163-0.bin","address_library_present":true})"
    "\n";
const std::string kF4seLog =
    R"j({"schema":1,"seq":6,"event":"f4se_log","plugin_lines":["plugin BrokenPlugin.dll (00000001 Broken 00000001) disabled, incompatible with current version of the game 0 (handle 0)","plugin MultiplexerDiagnosticRecorder.dll (00000001 MultiplexerDiagnosticRecorder 01000000) loaded correctly (handle 1)"]})j"
    "\n";
const std::string kDataReady =
    R"({"schema":1,"seq":7,"event":"f4se_message","message":"game_data_ready","type":10,"ready":false})" "\n"
    R"({"schema":1,"seq":8,"event":"f4se_message","message":"game_data_ready","type":10,"ready":true})" "\n";
const std::string kEnd = R"({"schema":1,"seq":20,"ms":27,"event":"session_end","reason":"process_exit"})" "\n";

const Finding* find(const std::vector<Finding>& f, const std::string& title)
{
    for (const auto& x : f)
        if (x.title.find(title) != std::string::npos)
            return &x;
    return nullptr;
}

} // namespace

TEST(diagnostics_session_parsed_and_plugin_load_failures)
{
    const auto s = parseRecorderLog(kStart + kPostLoad + kLoadOrder + kF4sePlugins + kF4seLog + kDataReady + kEnd, "a.jsonl");
    CHECK(s.started && s.ended);
    CHECK_EQ(s.runtimeVersion, std::string("1.10.163.0"));
    CHECK_EQ(s.plugins.size(), size_t(4));
    CHECK_EQ(s.f4seLines.size(), size_t(2));
    const auto f = analyse(s);
    // A plugin F4SE refused, with F4SE's own reason; a DLL present but not loaded without one.
    const auto* broken = find(f, "F4SE did not load brokenplugin.dll");
    CHECK(broken != nullptr);
    if (broken) {
        CHECK(broken->cause.find("another version of Fallout 4") != std::string::npos);
        CHECK(broken->evidence.front().find("incompatible with current version") != std::string::npos);
    }
    CHECK(find(f, "Lonely.dll is in Data") != nullptr);
    CHECK(find(f, "MultiplexerDiagnosticRecorder") == nullptr); // loaded correctly
    CHECK(find(f, "ended without") == nullptr);                  // a normal exit
    CHECK(!formatReport(s, f).empty());
}

TEST(diagnostics_how_a_session_ended)
{
    // Stopped while loading a save: the save's name is the evidence.
    const std::string preLoad =
        R"({"schema":1,"seq":9,"event":"f4se_message","message":"pre_load_game","type":2,"save":"Save1_Nora.fos"})" "\n";
    auto f = analyse(parseRecorderLog(kStart + kPostLoad + kDataReady + preLoad));
    const auto* loading = find(f, "stopped while loading a save");
    CHECK(loading != nullptr && loading->severity == FindingSeverity::Error);
    if (loading) {
        bool named = false;
        for (const auto& e : loading->evidence)
            named |= e.find("Save1_Nora.fos") != std::string::npos;
        CHECK(named);
    }
    // Stopped while the game read its plugins.
    const std::string dataStart =
        R"({"schema":1,"seq":7,"event":"f4se_message","message":"game_data_ready","type":10,"ready":false})" "\n";
    f = analyse(parseRecorderLog(kStart + kPostLoad + dataStart));
    CHECK(find(f, "stopped while loading the plugins") != nullptr);
    // Stopped before F4SE finished loading plugins.
    f = analyse(parseRecorderLog(kStart));
    CHECK(find(f, "before F4SE finished") != nullptr);
    // In game, no exit recorded: low confidence, a warning.
    f = analyse(parseRecorderLog(kStart + kPostLoad + kDataReady));
    const auto* noExit = find(f, "without a normal exit");
    CHECK(noExit != nullptr && noExit->severity == FindingSeverity::Warning && noExit->confidence == "Low");
    // A save that failed to load, and a last line cut off mid-write.
    const std::string failed =
        preLoad + R"({"schema":1,"seq":10,"event":"f4se_message","message":"post_load_game","type":3,"success":false})" "\n";
    f = analyse(parseRecorderLog(kStart + kPostLoad + kDataReady + failed + R"({"schema":1,"seq":11,"eve)"));
    CHECK(find(f, "A save failed to load") != nullptr);
    CHECK(find(f, "could not be read") != nullptr);
    // Not a recorder log.
    f = analyse(parseRecorderLog("hello\n"));
    CHECK(find(f, "no session start") != nullptr);
}

TEST(diagnostics_compared_with_the_multiplexer_build)
{
    auto root = tempDir("diagpackage");
    std::filesystem::create_directories(root / "Data");
    writeFile(root / "Multiplexer_plugins.txt", "*Fallout4.esm\n*ModB.esp\n*Missing.esp\n*Dummy_Plugin_01.esp\n");
    writeFile(root / "Multiplexer_DisablePlugins.txt", "# Plugins compiled into the output; disable these\nModA.esp\n");
    writeFile(root / "Data" / "Dummy_Plugin_01.esp", "x");
    const auto package = loadPackageContext(root);
    CHECK(package.has_value());
    if (!package)
        return;
    CHECK_EQ(package->outputs.size(), size_t(1));
    const auto s = parseRecorderLog(kStart + kPostLoad + kLoadOrder + kEnd);
    const auto f = analyse(s, &*package);
    const auto* twice = find(f, "compiled into the output are still enabled");
    CHECK(twice != nullptr && twice->severity == FindingSeverity::Error);
    if (twice)
        CHECK(twice->evidence.front().find("moda.esp") != std::string::npos);
    const auto* off = find(f, "An output plugin is not enabled");
    CHECK(off != nullptr && off->evidence.front().find("Dummy_Plugin_01.esp") != std::string::npos);
    const auto* missing = find(f, "differs from the one Multiplexer built for");
    CHECK(missing != nullptr && missing->evidence.front().find("Missing.esp") != std::string::npos);

    // The game version Multiplexer is set up for.
    auto ng = *package;
    ng.expectedProfile = "nextgen";
    CHECK(find(analyse(s, &ng), "game version differs") != nullptr);
    ng.expectedProfile = "oldgen";
    CHECK(find(analyse(s, &ng), "game version differs") == nullptr);

    // The newest session file in a folder.
    auto logs = tempDir("diaglogs");
    writeFile(logs / "session-20261101-100000-1.jsonl", kStart);
    writeFile(logs / "session-20261104-090000-2.jsonl", kStart + kEnd);
    writeFile(logs / "notes.txt", "x");
    CHECK_EQ(newestSession(logs).filename().string(), std::string("session-20261104-090000-2.jsonl"));
    CHECK(loadRecorderLog(newestSession(logs)).ended);
}

// "It captured nothing": the recorder's installation is checked from where it is and what F4SE said.
TEST(diagnostics_recorder_installation)
{
    namespace fs = std::filesystem;
    const auto root = tempDir("recorder_install");
    const auto game = root / "Fallout 4";
    const auto f4se = root / "My Games" / "Fallout4" / "F4SE";
    fs::create_directories(game / "Data");
    fs::create_directories(f4se);
    auto write = [](const fs::path& p, const std::string& text) {
        fs::create_directories(p.parent_path());
        std::ofstream(p, std::ios::binary) << text;
    };

    // Never started through F4SE.
    auto st = checkRecorderInstall(game, f4se);
    CHECK(!st.ok);
    CHECK(st.verdict.find("not been started through F4SE") != std::string::npos);

    // Copied into the game folder: F4SE never reads it there.
    write(game / "MultiplexerDiagnosticRecorder.dll", "MZ");
    write(f4se / "f4se.log", "plugin directory = X\\Data\\F4SE\\Plugins\\\nplugin X\\Data\\F4SE\\Plugins\\\\achievements.dll "
                             "(00000001 ACNT 00000001) loaded correctly\n");
    st = checkRecorderInstall(game, f4se);
    CHECK(!st.ok);
    CHECK(!st.installed);
    CHECK_EQ(st.misplaced.size(), size_t(1));
    CHECK(st.verdict.find("never reads") != std::string::npos);

    // In Data\F4SE\Plugins but F4SE refused it: its own words are the verdict.
    write(game / "Data" / "F4SE" / "Plugins" / "MultiplexerDiagnosticRecorder.dll", "MZ");
    write(f4se / "f4se.log", "plugin X\\MultiplexerDiagnosticRecorder.dll (00000001 MultiplexerDiagnosticRecorder "
                             "01010000) disabled, couldn't load plugin\r\n");
    st = checkRecorderInstall(game, f4se);
    CHECK(st.installed);
    CHECK(!st.ok);
    CHECK(st.verdict.find("couldn't load plugin") != std::string::npos);

    // Loaded: OK, with the status log and the newest session.
    write(f4se / "f4se.log", "plugin X\\MultiplexerDiagnosticRecorder.dll (00000001 MultiplexerDiagnosticRecorder "
                             "01010000) loaded correctly (handle 7)\n");
    write(f4se / "MultiplexerDiagnosticRecorder.log", "[10:00:00] MultiplexerDiagnosticRecorder 1.1.0 loaded\r\n");
    write(f4se / "MultiplexerDiagnosticRecorder" / "session-20261108-100000-1.jsonl", kStart);
    st = checkRecorderInstall(game, f4se);
    CHECK(st.ok);
    CHECK(!st.newestSession.empty());
    const auto text = formatInstallStatus(st);
    CHECK(text.find("OK: F4SE loaded the recorder") != std::string::npos);
    CHECK(text.find("1.1.0 loaded") != std::string::npos);
}

// Archive diagnostics (recorder 1.2.0): the list as written, what the game received, what it opened.
// The event lines follow what the DLL writes (checked by running it under Wine with a host that reads
// the list with a 1,024-byte buffer, as Fallout4.exe does).
TEST(diagnostics_archive_list_cut_and_what_loaded)
{
    const std::string value = "Fallout4 - Animations.ba2, ModA - Main.ba2, ModB - Main.ba2, ModC - Textures.ba2";
    const std::string cut = value.substr(0, 50); // the game received only this much
    auto line = [](const std::string& json) { return json + "\n"; };
    std::string log = kStart + kPostLoad +
        line(R"({"schema":1,"seq":2,"event":"archive_observers","ini_reads":true,"open_a":true,"open_w":true})") +
        line(R"({"schema":1,"seq":3,"event":"archive_ini","file":"Fallout4.ini","key":"SResourceArchiveList2","length":25,"value":"Fallout4 - Animations.ba2"})") +
        line(R"({"schema":1,"seq":4,"event":"archive_ini","file":"Fallout4Custom.ini","key":"SResourceArchiveList2","length":)" +
             std::to_string(value.size()) + R"(,"value":")" + value + R"("})") +
        line(R"({"schema":1,"seq":5,"event":"archive_ini_read","section":"Archive","key":"SResourceArchiveList2","file":"Fallout4Custom.ini","buffer":51,"returned":50,"truncated":true,"value":")" + cut + R"("})") +
        line(R"({"schema":1,"seq":6,"event":"archive_open","path":"E:\\Fallout 4\\Data\\Fallout4 - Animations.ba2","ok":true,"write":false})") +
        line(R"({"schema":1,"seq":7,"event":"archive_open","path":"E:\\Fallout 4\\Data\\ModA - Main.ba2","ok":true,"write":false})") +
        // Written by a tool, not loaded by the game: no evidence.
        line(R"({"schema":1,"seq":8,"event":"archive_open","path":"E:\\Fallout 4\\Data\\ModC - Textures.ba2","ok":true,"write":true})") +
        line(R"({"schema":1,"seq":9,"event":"archive_handles","stage":"main_menu","method":"snapshot","handles":900,"count":2,"ini_reads_seen":2,"opens_seen":3,"archives":["E:\\Fallout 4\\Data\\Fallout4 - Animations.ba2","E:\\Fallout 4\\Data\\ModA - Main.ba2"]})") +
        kEnd;
    const auto s = parseRecorderLog(log, "archives.jsonl");
    CHECK(s.hasArchiveDiagnostics());
    const auto c = checkArchives(s);
    CHECK(c.observable);
    CHECK_EQ(c.iniFile, std::string("Fallout4Custom.ini"));
    CHECK_EQ(c.iniEntries.size(), size_t(4));
    CHECK(c.truncated);
    CHECK_EQ(c.received, uint64_t(50));
    CHECK_EQ(c.loaded.size(), size_t(2));
    CHECK_EQ(c.missing.size(), size_t(2));
    CHECK_EQ(c.firstMissing, std::string("ModB - Main.ba2"));
    CHECK_EQ(c.firstMissingIndex, size_t(2));
    CHECK_EQ(c.firstMissingOffset, value.find("ModB"));
    CHECK(c.verdict.rfind("FAIL: 2 of 4", 0) == 0);
    const auto findings = analyse(s);
    CHECK(find(findings, "The game read only part of the archive list") != nullptr);
    CHECK(find(findings, "Requested archives did not load") != nullptr);
    const auto report = formatReport(s, findings);
    CHECK(report.find("Runtime verified:     2") != std::string::npos);
    CHECK(report.find("String position:    " + std::to_string(value.find("ModB"))) != std::string::npos);

    // The build's own list is what was requested, when a build is given.
    PackageContext pkg;
    pkg.archiveList = {"Fallout4 - Animations.ba2", "ModA - Main.ba2"};
    const auto withBuild = checkArchives(s, &pkg);
    CHECK(withBuild.missing.empty());
    CHECK(withBuild.verdict.rfind("PASS", 0) == 0);

    // Nothing of the game's own seen opening: the method does not work there, so no verdict is given.
    std::string blind = kStart + line(R"({"schema":1,"seq":2,"event":"archive_observers","ini_reads":false,"open_a":false,"open_w":false})") +
                        line(R"({"schema":1,"seq":3,"event":"archive_ini","file":"Fallout4Custom.ini","key":"SResourceArchiveList2","length":15,"value":"ModA - Main.ba2"})");
    const auto b = checkArchives(parseRecorderLog(blind));
    CHECK(!b.observable);
    CHECK(b.missing.size() == 1 && b.firstMissing.empty());
    CHECK(b.verdict.rfind("cannot verify", 0) == 0);
    // A session from an older recorder has no archive section.
    CHECK(!checkArchives(parseRecorderLog(kStart + kEnd)).available);
}

TEST(diagnostics_archive_limit_plugins_detected)
{
    namespace fs = std::filesystem;
    const auto game = tempDir("archive_limit_fix") / "Fallout 4";
    fs::create_directories(game / "Data" / "F4SE" / "Plugins");
    CHECK(!detectArchiveLimitFix(game).addictol);
    CHECK(detectArchiveLimitFix(game).summary.find("256") != std::string::npos);
    std::ofstream(game / "Data" / "F4SE" / "Plugins" / "Addictol.dll") << "MZ";
    auto f = detectArchiveLimitFix(game);
    CHECK(f.addictol && f.addictolArchiveLimits); // on by default
    std::ofstream(game / "Data" / "F4SE" / "Plugins" / "Addictol.toml") << "[Patches]\nbArchiveLimits = false\n";
    f = detectArchiveLimitFix(game);
    CHECK(f.addictol && !f.addictolArchiveLimits);
    CHECK(f.summary.find("off") != std::string::npos);
}
