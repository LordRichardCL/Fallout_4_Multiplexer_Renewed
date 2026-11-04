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
