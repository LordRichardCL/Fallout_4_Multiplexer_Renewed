#include "diagnostics/SaveGame.hpp"

#include "engine/BuildComparison.hpp"
#include "util/Platform.hpp"
#include "util/Text.hpp"

#include <cstring>
#include <fstream>
#include <stdexcept>

namespace fo4mx::diagnostics {

namespace fs = std::filesystem;

namespace {

class Reader {
public:
    explicit Reader(const std::vector<uint8_t>& b) : b_(b) {}
    size_t pos() const { return pos_; }
    void seek(size_t p)
    {
        if (p > b_.size())
            throw std::runtime_error("the save is damaged (an offset points past its end)");
        pos_ = p;
    }
    void need(size_t n) const
    {
        if (n > b_.size() - pos_)
            throw std::runtime_error("the save is damaged (it ends early)");
    }
    template <class T> T get()
    {
        need(sizeof(T));
        T v;
        std::memcpy(&v, b_.data() + pos_, sizeof(T));
        pos_ += sizeof(T);
        return v;
    }
    std::string str() // uint16 length, then that many bytes
    {
        const auto n = get<uint16_t>();
        need(n);
        std::string s(reinterpret_cast<const char*>(b_.data() + pos_), n);
        pos_ += n;
        return s;
    }
    void skip(uint64_t n)
    {
        if (n > b_.size() - pos_)
            throw std::runtime_error("the save is damaged (it ends early)");
        pos_ += static_cast<size_t>(n);
    }

private:
    const std::vector<uint8_t>& b_;
    size_t pos_ = 0;
};

} // namespace

SaveGameInfo parseSaveGame(const std::vector<uint8_t>& bytes)
{
    Reader r(bytes);
    r.need(12);
    if (std::memcmp(bytes.data(), "FO4_SAVEGAME", 12) != 0)
        throw std::runtime_error("not a Fallout 4 save (no FO4_SAVEGAME mark)");
    r.skip(12);
    SaveGameInfo s;
    const uint32_t headerSize = r.get<uint32_t>();
    const size_t headerStart = r.pos();
    s.version = r.get<uint32_t>();
    s.saveNumber = r.get<uint32_t>();
    s.player = r.str();
    s.level = r.get<uint32_t>();
    s.location = r.str();
    s.playTime = r.str();
    (void)r.str();           // race
    (void)r.get<uint16_t>(); // sex
    (void)r.get<float>();    // experience
    (void)r.get<float>();    // experience for the next level
    (void)r.get<uint64_t>(); // file time
    const uint32_t width = r.get<uint32_t>();
    const uint32_t height = r.get<uint32_t>();
    if (headerSize < r.pos() - headerStart)
        throw std::runtime_error("the save is damaged (its header is shorter than its fields)");
    r.seek(headerStart + headerSize);
    r.skip(uint64_t(width) * height * 4); // the screenshot
    s.formVersion = r.get<uint8_t>();
    s.gameVersion = r.str();
    const uint32_t pluginInfoSize = r.get<uint32_t>();
    const size_t pluginStart = r.pos();
    r.need(pluginInfoSize);
    const uint8_t count = r.get<uint8_t>();
    for (unsigned i = 0; i < count; ++i)
        s.plugins.push_back(r.str());
    // Light plugins follow, in saves of game versions that load them, within the same section.
    if (r.pos() - pluginStart < pluginInfoSize) {
        const uint16_t light = r.get<uint16_t>();
        for (unsigned i = 0; i < light; ++i)
            s.lightPlugins.push_back(r.str());
    }
    if (r.pos() - pluginStart > pluginInfoSize)
        throw std::runtime_error("the save is damaged (its plugin list is longer than its section)");
    return s;
}

SaveGameInfo readSaveGame(const fs::path& file)
{
    std::ifstream in(file, std::ios::binary);
    if (!in)
        throw std::runtime_error("cannot read " + pathToUtf8(file));
    // The plugin list is near the start: the header, a screenshot and the list (a few MB at most).
    std::vector<uint8_t> bytes(16u << 20);
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    bytes.resize(static_cast<size_t>(in.gcount()));
    try {
        return parseSaveGame(bytes);
    } catch (const std::runtime_error&) {
        if (in.eof())
            throw; // the whole file was read: it is damaged
    }
    // A larger screenshot: read all of it.
    std::ifstream all(file, std::ios::binary);
    std::vector<uint8_t> whole((std::istreambuf_iterator<char>(all)), std::istreambuf_iterator<char>());
    return parseSaveGame(whole);
}

SaveCheck checkSave(const SaveGameInfo& save, const SaveCheckInput& in)
{
    SaveCheck c;
    std::set<std::string> loading, away, inSave;
    for (const auto& p : in.loadOrder)
        loading.insert(toLower(p));
    for (const auto& p : in.compiledAway)
        away.insert(toLower(p));
    std::set<std::string> outputs;
    for (const auto& o : in.outputs)
        outputs.insert(toLower(o));
    std::set<std::string> before;
    for (const auto& p : in.compiledBefore)
        before.insert(toLower(p));
    bool madeWithBuild = false;
    for (const auto* list : {&save.plugins, &save.lightPlugins})
        for (const auto& p : *list)
            madeWithBuild |= outputs.count(toLower(p)) != 0;
    for (const auto* list : {&save.plugins, &save.lightPlugins})
        for (const auto& p : *list) {
            const std::string k = toLower(p);
            inSave.insert(k);
            if (outputs.count(k))
                c.outputsInSave.push_back(p);
            if (madeWithBuild && before.count(k))
                continue; // an archive loader of the build the save was made with: its records are in the outputs
            if (away.count(k))
                c.compiledAway.push_back(p);
            else if (!loading.count(k))
                c.missing.push_back(p);
        }
    for (const auto& p : in.loadOrder)
        if (!inSave.count(toLower(p)))
            c.added.push_back(p);
    if (in.comparison && !c.outputsInSave.empty())
        c.recordsChanged = in.comparison->formIdChanges();
    c.safe = c.missing.empty() && c.compiledAway.empty() && c.recordsChanged == 0;
    return c;
}

std::string formatSaveCheck(const SaveGameInfo& s, const SaveCheck& c, bool nextBuild)
{
    std::string o = s.player + ", level " + std::to_string(s.level) + ", " + s.location + " (save " +
                    std::to_string(s.saveNumber) + ", played " + s.playTime + ", game " + s.gameVersion + ")\n" +
                    std::to_string(s.plugins.size()) + " full and " + std::to_string(s.lightPlugins.size()) +
                    " light plugins in the save" +
                    (c.outputsInSave.empty() ? std::string(", none of them a Multiplexer output")
                                             : ", " + std::to_string(c.outputsInSave.size()) + " of them outputs") +
                    ".\nCompared with " + (nextBuild ? "the load order after the next build" : "the load order now") +
                    ":\n\n";
    if (c.safe)
        o += "Nothing the save needs goes away: every plugin it was made with still loads" +
             std::string(c.outputsInSave.empty() ? "" : ", and the outputs keep every FormID it can hold") + ".\n";
    else
        o += "This save would lose content:\n";
    auto list = [&](const std::string& title, const std::vector<std::string>& v) {
        if (v.empty())
            return;
        o += "\n" + title + " (" + std::to_string(v.size()) + "):\n";
        for (const auto& p : v)
            o += "  " + p + "\n";
    };
    list("Not in the load order any more: what the save holds of them is lost", c.missing);
    list("Compiled by the build: their records get new FormIDs in the outputs, so the save loses what it holds of "
         "them (start a new game, or leave them out of compiling: Plugins page)",
         c.compiledAway);
    if (c.recordsChanged)
        o += "\n" + std::to_string(c.recordsChanged) +
             " record(s) of the build the save was made with get other FormIDs or are no longer in the build "
             "(Compilation Report → Changes since the previous build): the save loses them.\n";
    list("New to the save (usually fine: they start fresh)", c.added);
    o += "\nThis compares the plugins the save was made with; what the save holds of each one is not read.\n";
    return o;
}

fs::path newestSave(const fs::path& folderIn)
{
    const fs::path folder = folderIn.empty() ? platform::fallout4DocumentsDir() / "Saves" : folderIn;
    fs::path best;
    fs::file_time_type bestTime{};
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(folder, ec)) {
        if (!iendsWith(pathToUtf8(e.path().filename()), ".fos"))
            continue;
        const auto t = e.last_write_time(ec);
        if (best.empty() || t > bestTime) {
            best = e.path();
            bestTime = t;
        }
    }
    return best;
}

} // namespace fo4mx::diagnostics
