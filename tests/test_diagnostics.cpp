// Diagnostic Recorder logs (MultiplexerDiagnosticRecorder.dll): parsing and the recognised patterns.
// The log lines follow the exact format the DLL writes (checked by running it under a simulated F4SE).

#include "Fixtures.hpp"
#include "Test.hpp"

#include "diagnostics/RecorderLog.hpp"
#include "diagnostics/StartupCheck.hpp"
#include "util/Binary.hpp"

#include <cstdio>

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

// Listed archives that are not in Data cannot load whatever the list does: they are reported apart, and
// where loading stops is worked out from the archives that are there (also when the INI read was hidden).
TEST(diagnostics_archives_not_in_data_and_where_loading_stops)
{
    auto line = [](const std::string& json) { return json + "\n"; };
    const std::string value = "Fallout4 - Animations.ba2, Gone - Main.ba2, ModA - Main.ba2, ModB - Main.ba2";
    std::string log = kStart +
        line(R"({"schema":1,"seq":1,"event":"archive_ini","file":"Fallout4Custom.ini","key":"SResourceArchiveList2","length":1,"value":")" + value + R"("})") +
        line(R"({"schema":1,"seq":2,"event":"archive_presence","checked":4,"absent":1,"missing":["Gone - Main.ba2"]})") +
        line(R"({"schema":1,"seq":3,"event":"archive_observers","ini_reads":true,"open_a":true,"open_w":true})") +
        line(R"({"schema":1,"seq":4,"event":"archive_open","path":"D:\\Data\\Fallout4 - Animations.ba2","ok":true,"write":false})") +
        line(R"({"schema":1,"seq":5,"event":"archive_open","path":"D:\\Data\\ModA - Main.ba2","ok":true,"write":false})") + kEnd;
    const auto s = parseRecorderLog(log);
    const auto c = checkArchives(s);
    CHECK(!c.absentInferred);
    CHECK_EQ(c.absent.size(), size_t(1));
    CHECK_EQ(c.missing.size(), size_t(1));
    CHECK_EQ(c.firstMissing, std::string("ModB - Main.ba2"));
    CHECK(c.cutInferred);
    CHECK_EQ(c.cutAfter, value.find("ModA") + std::string("ModA - Main.ba2").size());
    CHECK_EQ(c.cutBefore, value.find("ModB"));
    const auto findings = analyse(s);
    CHECK(find(findings, "Listed archives are not installed") != nullptr);
    CHECK(find(findings, "The game stopped loading the archive list part way") != nullptr);
    // Not in order (a loaded archive after one in Data that did not load): no cut is claimed.
    std::string mixed = kStart +
        line(R"({"schema":1,"seq":1,"event":"archive_ini","file":"Fallout4Custom.ini","key":"SResourceArchiveList2","length":1,"value":")" + value + R"("})") +
        line(R"({"schema":1,"seq":2,"event":"archive_presence","checked":4,"absent":0,"missing":[]})") +
        line(R"({"schema":1,"seq":4,"event":"archive_open","path":"D:\\Data\\Fallout4 - Animations.ba2","ok":true,"write":false})") +
        line(R"({"schema":1,"seq":5,"event":"archive_open","path":"D:\\Data\\ModB - Main.ba2","ok":true,"write":false})") + kEnd;
    CHECK(!checkArchives(parseRecorderLog(mixed)).cutInferred);
}

// The archive loader (proof of concept): the game's read is still cut, but the archives past the cut load.
// That is reported as supplied in full, not as truncation.
TEST(diagnostics_cut_list_supplied_by_the_loader)
{
    auto line = [](const std::string& json) { return json + "\n"; };
    const std::string value = "Fallout4 - Animations.ba2, ModA - Main.ba2, ModB - Main.ba2";
    std::string log = kStart +
        line(R"({"schema":1,"seq":1,"event":"archive_ini","file":"Fallout4Custom.ini","key":"SResourceArchiveList2","length":1,"value":")" + value + R"("})") +
        line(R"({"schema":1,"seq":2,"event":"archive_presence","checked":3,"absent":0,"missing":[]})") +
        line(R"({"schema":1,"seq":3,"event":"archive_ini_read","section":"Archive","key":"sResourceArchiveList2","file":"Fallout4Custom.ini","buffer":31,"returned":30,"truncated":true,"value":")" + value.substr(0, 30) + R"("})") +
        line(R"({"schema":1,"seq":4,"event":"f4se_plugins","dlls":[{"name":"MultiplexerArchiveLoader.dll","loaded":true}],"address_library_file":"x","address_library_present":true})") +
        line(R"({"schema":1,"seq":5,"event":"archive_open","path":"D:\\Data\\Fallout4 - Animations.ba2","ok":true,"write":false})") +
        line(R"({"schema":1,"seq":6,"event":"archive_open","path":"D:\\Data\\ModA - Main.ba2","ok":true,"write":false})") +
        line(R"({"schema":1,"seq":7,"event":"archive_open","path":"D:\\Data\\ModB - Main.ba2","ok":true,"write":false})") + kEnd;
    const auto s = parseRecorderLog(log);
    const auto c = checkArchives(s);
    CHECK(c.truncated);
    CHECK(c.cutCompensated);
    CHECK(c.loader);
    CHECK(c.verdict.rfind("PASS", 0) == 0);
    const auto f = analyse(s);
    CHECK(find(f, "The archive list was supplied in full") != nullptr);
    CHECK(find(f, "The game read only part of the archive list") == nullptr);
}

// A build that relies on the archive loader, and a session where the list arrived cut anyway.
TEST(diagnostics_archive_loader_did_not_supply)
{
    auto line = [](const std::string& json) { return json + "\n"; };
    const std::string value = "Fallout4 - Animations.ba2, ModA - Main.ba2, ModB - Main.ba2";
    std::string log = kStart +
        line(R"({"schema":1,"seq":1,"event":"archive_ini","file":"Fallout4Custom.ini","key":"SResourceArchiveList2","length":1,"value":")" + value + R"("})") +
        line(R"({"schema":1,"seq":2,"event":"archive_presence","checked":3,"absent":0,"missing":[]})") +
        line(R"({"schema":1,"seq":3,"event":"archive_ini_read","section":"Archive","key":"sResourceArchiveList2","file":"Fallout4Custom.ini","buffer":31,"returned":30,"truncated":true,"value":")" + value.substr(0, 30) + R"("})") +
        line(R"({"schema":1,"seq":5,"event":"archive_open","path":"D:\\Data\\Fallout4 - Animations.ba2","ok":true,"write":false})") + kEnd;
    PackageContext pkg;
    pkg.usesArchiveLoader = true;
    const auto f = analyse(parseRecorderLog(log), &pkg);
    const auto* x = find(f, "The archive loader did not supply the archive list");
    CHECK(x != nullptr);
    CHECK(x && x->cause.find("was not loaded") != std::string::npos);
    PackageContext plain;
    CHECK(find(analyse(parseRecorderLog(log), &plain), "The archive loader did not supply") == nullptr);
}

// F4SE writes lines like "plugin 2 allocated 32 bytes from branch pool": not a plugin that failed to load.
TEST(diagnostics_f4se_allocation_lines_are_not_load_failures)
{
    const std::string log = kStart + kPostLoad +
        R"j({"schema":1,"seq":6,"event":"f4se_log","plugin_lines":["plugin 2 allocated 32 bytes from branch pool","plugin E:\\Data\\F4SE\\Plugins\\\\A.dll (00000001 A 00000001) loaded correctly"]})j" "\n" + kEnd;
    for (const auto& f : analyse(parseRecorderLog(log)))
        CHECK(f.title.find("F4SE did not load") == std::string::npos);
}

// Quitting to the desktop: an exit save, the previous exit save deleted, then the game ends without the
// shutdown the recorder sees. Not reported as a possible crash; a session that just stops still is.
TEST(diagnostics_quit_after_exit_save_is_not_a_crash)
{
    auto msg = [](int seq, const std::string& name, const std::string& extra = {}) {
        return R"({"schema":1,"seq":)" + std::to_string(seq) + R"(,"event":"f4se_message","message":")" + name + R"(","type":0)" +
               extra + "}\n";
    };
    const std::string base = kStart + kPostLoad + msg(2, "game_loaded") + msg(3, "pre_load_game", R"(,"save":"Quickstart")") +
                             msg(4, "post_load_game", R"(,"success":true)");
    const std::string quit = base + msg(5, "pre_save_game") + msg(6, "post_save_game") +
                             msg(7, "delete_game", R"(,"save":"Exitsave0_00000000_416C6578_Vault111Cryo_000053_1_2")");
    auto s = parseRecorderLog(quit);
    CHECK(quitAfterExitSave(s));
    const auto f = analyse(s);
    CHECK(find(f, "The game was quit after its exit save") != nullptr);
    CHECK(find(f, "The session ended without a normal exit") == nullptr);
    CHECK(formatReport(s, f).find("quit after the exit save") != std::string::npos);
    // An ordinary save (no exit save deleted) followed by nothing: still a possible crash.
    auto stopped = parseRecorderLog(base + msg(5, "pre_save_game") + msg(6, "post_save_game"));
    CHECK(!quitAfterExitSave(stopped));
    CHECK(find(analyse(stopped), "The session ended without a normal exit") != nullptr);
}

// Startup popups and MCM menus: start-game quests of a build against a Papyrus log, MCM.log and the relink list.
TEST(startup_check_reads_quests_popups_and_mcm_menus)
{
    using fo4mx::BinaryWriter;
    auto root = tempDir("startup_check");
    auto data = root / "Data";
    auto wstr = [](BinaryWriter& w, const std::string& s) {
        w.write(static_cast<uint16_t>(s.size()));
        for (char ch : s)
            w.write(static_cast<uint8_t>(ch));
    };
    // VMAD: one script, optionally one object property naming `object` (file FormID).
    auto vmad = [&](const std::string& script, uint32_t object) {
        BinaryWriter w;
        w.write(static_cast<int16_t>(6));
        w.write(static_cast<int16_t>(2));
        w.write(static_cast<uint16_t>(1));
        wstr(w, script);
        w.write(static_cast<uint8_t>(0));
        w.write(static_cast<uint16_t>(object ? 1 : 0));
        if (object) {
            wstr(w, "Popup");
            w.write(static_cast<uint8_t>(1)); // object
            w.write(static_cast<uint8_t>(1)); // status
            w.write(static_cast<uint16_t>(0));
            w.write(static_cast<int16_t>(-1));
            w.write(object);
        }
        return raw("VMAD", w.take());
    };
    auto qdnam = [](uint16_t flags) {
        return raw("DNAM", {static_cast<uint8_t>(flags & 0xFF), static_cast<uint8_t>(flags >> 8), 0, 0});
    };
    writePlugin(data / "Dummy_Plugin_01.esp", {"Fallout4.esm"},
                {record("QUST", 0x01000800, {zs("EDID", "ModA_Start"), qdnam(1), vmad("ModA:StartScript", 0x01000801)}),
                 record("QUST", 0x01000802, {zs("EDID", "ModA_Broken"), qdnam(1), vmad("ModA:MissingScript", 0)}),
                 record("QUST", 0x01000803, {zs("EDID", "ModA_Running"), qdnam(0x101), vmad("ModA:RunScript", 0)}),
                 record("QUST", 0x01000804, {zs("EDID", "ModA_Later"), qdnam(0), vmad("ModA:LaterScript", 0)}),
                 record("MESG", 0x01000801, {zs("EDID", "ModA_Welcome"), zs("FULL", "Welcome"), u32("DNAM", 1)})});
    std::string map = "# Fallout 4 Multiplexer Renewed FormID map v1\n";
    for (uint32_t id = 0x800; id <= 0x804; ++id) {
        char row[96];
        std::snprintf(row, sizeof(row), "ModA.esp\t%06X\tDummy_Plugin_01.esp\t%06X\tQUST\t\n", id, id);
        map += row;
    }
    writeFile(data / "Multiplexer_FormMap.tsv", map);
    writeFile(data / "Multiplexer_Relinked.tsv",
              "# header\n# header\n# file\twritten\toriginal\tplugin\tlocation\tuse\tbefore\tafter\n"
              "MCM\\Config\\ModA\\config.json\ta\tb\tModA.esp\tpluginRequirements[0]\tMCM pluginRequirements\tModA.esp\t"
              "Dummy_Plugin_01.esp\n"
              "MCM\\Config\\ModA\\config.json\ta\tb\tModA.esp\tline 5\tPlugin|FormID pair\tModA.esp|800\t"
              "Dummy_Plugin_01.esp|800\n");

    StartupCheckInputs in;
    in.exportRoot = root;
    in.loadOrder = loadOrderFromF4seLog("save name is x\nSaving plugin list:\n\t[0]\tFallout4.esm\n\t[1]\tModB.esp\n"
                                        "\t[FE:0]\tLight.esl\n\t[2]\tDummy_Plugin_01.esp\nSaving key input event\n");
    CHECK_EQ(in.loadOrder.full.size(), size_t(3));
    CHECK_EQ(in.loadOrder.light.size(), size_t(1));
    in.papyrusLog =
        "[11/23/2026 - 10:00:00PM] Cannot open store for class \"ModA:MissingScript\", missing file?\n"
        "[11/23/2026 - 10:00:00PM] error: Unable to bind script ModA:MissingScript to ModA_Broken (02000802) because "
        "their base types do not match\n"
        "[11/23/2026 - 10:00:01PM] error: Cannot call Show() on a None object, aborting function call\n"
        "stack:\n"
        "\t[ModB_Quest (01000900)].ModB:Script.OnQuestInit() - \"x.psc\" Line 3\n"
        "[11/23/2026 - 10:00:02PM] Dumping stack 1:\n"
        "\t\t[ModA_Running (02000803)].ModA:RunScript.OnQuestInit() - \"y.psc\" Line 9\n"
        "\t\t\t[::temp1]: \"OnMCMSettingChange|ModA\"\n";
    in.mcmLog = "MCM v1.38\nNumber of mod setting files: 2\nRegistered 30 mod settings in 5 ms.\n"
                "WARNING: ModSetting x has an unknown type and cannot be registered.\n";

    const auto c = checkStartup(in);
    CHECK(c.buildFound);
    CHECK(c.outputsActive);
    CHECK_EQ(c.startupQuests, size_t(3)); // not ModA_Later, which does not start with the game
    CHECK_EQ(c.notStarted, size_t(1));
    CHECK_EQ(c.seenRunning, size_t(1));
    CHECK_EQ(c.withMessageBoxes, size_t(1));
    const StartupQuest* start = nullptr;
    const StartupQuest* broken = nullptr;
    for (const auto& q : c.quests) {
        if (q.editorId == "ModA_Start")
            start = &q;
        if (q.editorId == "ModA_Broken")
            broken = &q;
    }
    CHECK(start && broken);
    if (start && broken) {
        CHECK_EQ(start->formId, uint32_t(0x02000800));
        CHECK_EQ(start->source, std::string("ModA.esp"));
        CHECK_EQ(start->messageBoxes.size(), size_t(1));
        CHECK_EQ(start->messageBoxes.front().formId, uint32_t(0x02000801));
        CHECK_EQ(start->messageBoxes.front().text, std::string("Welcome"));
        CHECK(broken->state == StartupQuest::State::ScriptMissing);
        CHECK_EQ(broken->failedScript, std::string("ModA:MissingScript"));
    }
    CHECK_EQ(c.failedPopups.size(), size_t(1));
    if (!c.failedPopups.empty()) {
        CHECK_EQ(c.failedPopups.front().plugin, std::string("ModB.esp"));
        CHECK(!c.failedPopups.front().inOutput);
    }
    CHECK_EQ(c.mcmMenus.size(), size_t(1));
    if (!c.mcmMenus.empty()) {
        CHECK_EQ(c.mcmMenus.front().config, std::string("ModA"));
        CHECK_EQ(c.mcmMenus.front().now, std::string("Dummy_Plugin_01.esp"));
        CHECK(c.mcmMenus.front().scriptRegistered);
    }
    CHECK_EQ(c.mcmReferences, size_t(1));
    CHECK_EQ(c.mcm.settingFiles, size_t(2));
    CHECK_EQ(c.mcm.settings, size_t(30));
    CHECK_EQ(c.mcm.warnings.size(), size_t(1));

    const auto text = formatStartupCheck(c);
    CHECK(text.find("NOT STARTED: script file missing in the mod: ModA:MissingScript") != std::string::npos);
    CHECK(text.find("ModA_Welcome: \"Welcome\"") != std::string::npos);
    CHECK(text.find("ModB.esp, not compiled") != std::string::npos);
    CHECK(text.find("its script registered for setting changes") != std::string::npos);

    // Without the outputs in the load order the log cannot name their records, and the check says so.
    in.loadOrder = {};
    const auto noOrder = checkStartup(in);
    CHECK(!noOrder.outputsActive);
    CHECK(formatStartupCheck(noOrder).find("not in the load order") != std::string::npos);
}

// ---- Overwritten changes: a plugin's changes the winning versions do not hold ------------------------------

#include "engine/OverwrittenChanges.hpp"

TEST(overwritten_changes_names_the_later_plugins)
{
    using namespace fo4mx;
    Session s;
    auto add = [&](const std::string& name, std::vector<PluginIndex> masters) {
        PluginInfo p;
        p.index = static_cast<PluginIndex>(s.plugins.size());
        p.name = name;
        p.key = name;
        for (auto& c : p.key)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        p.active = true;
        p.masters = std::move(masters);
        char hex[8];
        std::snprintf(hex, sizeof(hex), "%02X", unsigned(p.index));
        p.loadOrderHex = hex;
        s.loadOrder.push_back(p.index);
        s.plugins.push_back(std::move(p));
    };
    add("Fallout4.esm", {});          // 0
    add("Skills.esp", {0});           // 1: the mod that "doesn't work"
    add("Skills Patch.esp", {0, 1});  // 2: made for it
    add("Perk Tweaks.esp", {0});      // 3: loads later, made without it
    add("Skills - Fix.esp", {0, 1});  // 4: made with it, changes it on purpose

    ConflictReport report;
    auto change = [](PluginIndex source, ChangeStatus status, PluginIndex lostAt, std::vector<PluginIndex> keptBy = {}) {
        TracedChange t;
        t.field = "Record Flags";
        t.before = "0x00000000";
        t.after = "0x00000004";
        t.source = source;
        t.status = status;
        t.lostAt = lostAt;
        t.keptBy = std::move(keptBy);
        return t;
    };
    ConflictEntry perk;
    perk.key = FormKey{0, 0x4A09F};
    perk.sig = Signature("PERK");
    perk.chain = {0, 1, 2, 3};
    perk.winner = 3;
    perk.changes = {change(1, ChangeStatus::OmittedLater, 3, {2}), change(2, ChangeStatus::OmittedLater, 3),
                    change(1, ChangeStatus::Survives, kInvalidPlugin)};
    ConflictEntry perk2 = perk;
    perk2.key = FormKey{0, 0x4A0A9};
    perk2.changes = {change(1, ChangeStatus::OmittedLater, kInvalidPlugin)}; // no lostAt: the winner
    ConflictEntry list = perk; // a list a build merges: the change is in the merged version
    list.key = FormKey{0, 0x9000};
    list.sig = Signature("FLST");
    list.changes = {change(2, ChangeStatus::OmittedLater, 3)};
    list.changes[0].inMerged = true;
    ConflictEntry book;
    book.key = FormKey{0, 0x1234};
    book.sig = Signature("BOOK");
    book.chain = {0, 1, 4};
    book.winner = 4;
    book.changes = {change(1, ChangeStatus::Replaced, 4)};
    ConflictEntry other; // changes by plugins unrelated to it are not counted
    other.chain = {0, 3};
    other.winner = 3;
    other.changes = {change(3, ChangeStatus::Survives, kInvalidPlugin)};
    report.entries = {perk, perk2, list, book, other};

    auto r = findOverwrittenChanges(s, &report, "skills.ESP");
    CHECK(r.found && r.analysed);
    CHECK_EQ(r.madeFor.size(), size_t(2));
    CHECK_EQ(r.records, size_t(4));
    CHECK_EQ(r.changes, size_t(6));
    CHECK_EQ(r.merged, size_t(1));
    CHECK_EQ(r.mergedRecords, size_t(1));
    CHECK_EQ(r.lost, size_t(4));
    CHECK_EQ(r.lostRecords, size_t(3));
    CHECK_EQ(r.overwriters.size(), size_t(2));
    const auto& tweaks = r.overwriters[0];
    CHECK_EQ(tweaks.plugin, PluginIndex(3));
    CHECK_EQ(tweaks.records, size_t(2));
    CHECK_EQ(tweaks.changes, size_t(2));
    CHECK_EQ(tweaks.patchChanges, size_t(1));
    CHECK_EQ(tweaks.deliberate, size_t(0));
    CHECK_EQ(tweaks.keptBefore.at(2), size_t(1));
    CHECK_EQ(tweaks.types.at("PERK"), size_t(2));
    CHECK_EQ(r.overwriters[1].plugin, PluginIndex(4));
    CHECK_EQ(r.overwriters[1].deliberate, size_t(1));

    const auto text = formatOverwrittenChanges(s, r, "Skills.esp");
    CHECK(text.find("Overwritten by plugins made without it") != std::string::npos);
    CHECK(text.find("Perk Tweaks.esp [03]") != std::string::npos);
    CHECK(text.find("Kept until it loads by: Skills Patch.esp") != std::string::npos);
    CHECK(text.find("Changed by plugins made with it as a master") != std::string::npos);
    CHECK(text.find("Load the plugins in the first list before Skills.esp") != std::string::npos);
    CHECK(text.find("1 more change, in 1 record (lists") != std::string::npos);

    // Without the patches' changes; a plugin that is not active; no analysis yet.
    CHECK_EQ(findOverwrittenChanges(s, &report, "Skills.esp", false).lost, size_t(3));
    CHECK(!findOverwrittenChanges(s, &report, "Missing.esp").found);
    const auto none = findOverwrittenChanges(s, nullptr, "Skills.esp");
    CHECK(none.found && !none.analysed);
    CHECK(formatOverwrittenChanges(s, none, "Skills.esp").find("has not run yet") != std::string::npos);
}
