// The Load Monitor's platform-independent core: file kinds, the observed-state record, read counters and
// the text it logs and shows.

#include "Test.hpp"

#include "../src/loadmonitor/MonitorCore.hpp"

using namespace fo4mx::loadmon;

TEST(load_monitor_classifies_the_files_the_game_opens)
{
    const std::string data = "c:\\games\\fallout 4\\data\\";
    CHECK(classify("C:\\Games\\Fallout 4\\Data\\Fallout4.esm", data) == FileKind::Plugin);
    CHECK(classify("Data\\PennsWoods.esp", data) == FileKind::Plugin);
    CHECK(classify("Data\\ccBGSFO4001-PipBoy(Black).esl", data) == FileKind::Plugin);
    CHECK(classify("Data\\Fallout4 - Textures1.BA2", data) == FileKind::Archive);
    CHECK(classify("C:\\Users\\x\\Documents\\My Games\\Fallout4\\Saves\\Save1.ess", data) == FileKind::Save);
    CHECK(classify("Data\\Scripts\\TS_Init.pex", data) == FileKind::Loose);
    CHECK(classify("C:/Games/Fallout 4/Data/Meshes/a.nif", data) == FileKind::Loose);
    CHECK(classify(".\\Data\\Interface\\HUDMenu.swf", data) == FileKind::Loose);
    CHECK(classify("C:\\Users\\x\\Documents\\My Games\\Fallout4\\Fallout4Prefs.ini", data) == FileKind::Other);
    CHECK_EQ(fileName("C:\\Games\\Fallout 4\\Data\\Dummy_Plugin_05.esp"), std::string("Dummy_Plugin_05.esp"));
}

TEST(load_monitor_keeps_the_previous_item_and_the_time_on_the_current_one)
{
    State st;
    st.stage("Opening plugin files", 1000);
    st.current("Opened plugin A.esp", 1000);
    st.current("Reading A.esp (1.0 MB)", 1500);
    st.refresh("Reading A.esp (2.0 MB)", 3000); // same item, newer counters: its time does not restart
    st.current("Reading A.esp (2.0 MB)", 3200); // same text: no new item
    const Snapshot s = st.snapshot();
    CHECK_EQ(s.stage, std::string("Opening plugin files"));
    CHECK_EQ(s.current, std::string("Reading A.esp (2.0 MB)"));
    CHECK_EQ(s.previous, std::string("Opened plugin A.esp"));
    CHECK_EQ(s.currentSince, 1500LL);
    CHECK_EQ(s.lastActivity, 3200LL);

    st.stage("Opening plugin files", 4000); // the same stage again keeps its start time
    CHECK_EQ(st.snapshot().stageSince, 1000LL);
}

TEST(load_monitor_counts_reads_per_file_and_by_handle)
{
    FileTable t;
    TrackedFile* a = t.opened(10, "Data\\A.esp", FileKind::Plugin, 4 * 1024 * 1024);
    TrackedFile* b = t.opened(11, "Data\\Main.ba2", FileKind::Archive, 0);
    CHECK(t.find(10) == a);
    CHECK(t.find(12) == nullptr);
    a->bytes += 3 * 1024 * 1024;
    b->bytes += 1024 * 1024;
    auto first = t.takeInterval();
    CHECK_EQ(first.size(), size_t(2));
    CHECK(first[0].first == a); // most bytes first
    CHECK_EQ(intervalLine(first, 5), std::string("Reads in the last 5 s: A.esp 3.0 MB, Main.ba2 1.0 MB"));
    CHECK(t.takeInterval().empty()); // nothing new since
    CHECK(intervalLine({}, 5).empty());
    CHECK_EQ(readingText(*a), std::string("Reading A.esp (3.0 MB of 4.0 MB read so far)"));

    // A closed handle no longer counts; opening the same path again shares the file's counters.
    t.closed(10);
    CHECK(t.find(10) == nullptr);
    CHECK(t.opened(20, "data\\a.ESP", FileKind::Plugin, 0) == a);
    CHECK_EQ(a->opens.load(), 2u);
    CHECK_EQ(t.totals().files[static_cast<size_t>(FileKind::Plugin)], size_t(1));
}

TEST(load_monitor_text_never_presents_the_last_item_as_the_cause)
{
    Snapshot s;
    s.stage = "New game: the engine is loading the world";
    s.stageSince = 0;
    s.current = "F4SE message NewGame";
    s.currentSince = 1000;
    s.lastActivity = 1000;
    s.lastPlugin = "Z.esp";
    const std::string q = quietLine(s, 91000);
    CHECK(q.find("No observed activity for 1 min 30 s") == 0);
    CHECK(q.find("Last plugin: Z.esp") != std::string::npos);
    CHECK(q.find(kNotACause) != std::string::npos);

    const auto lines = overlayLines(s, 7000);
    CHECK_EQ(lines.size(), size_t(6));
    CHECK_EQ(lines[1], std::string("Stage: New game: the engine is loading the world"));
    CHECK_EQ(lines[4], std::string("On current: 6.0 s   No new activity: 6.0 s"));
    CHECK_EQ(lines[5], std::string("Last observed is not a confirmed cause"));
    CHECK_EQ(duration(3723000), std::string("1 h 02 min"));
}
