#include "diagnostics/StartupCheck.hpp"

#include "engine/AutoMerger.hpp"
#include "engine/CompilationEngine.hpp"
#include "esp/PluginFile.hpp"
#include "util/Platform.hpp"
#include "util/Text.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace fo4mx::diagnostics {

namespace {

std::string lower(std::string s)
{
    for (auto& ch : s)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

std::string hex8(uint32_t v)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%08X", v);
    return buf;
}

bool isHex(char c)
{
    return std::isdigit(static_cast<unsigned char>(c)) || (c >= 'A' && c <= 'F');
}

// The FormID in "(0A001234)" ending at `close` (the index of ')').
bool idBefore(const std::string& s, size_t close, uint32_t& id)
{
    if (close < 9 || s[close - 9] != '(')
        return false;
    for (size_t i = close - 8; i < close; ++i)
        if (!isHex(s[i]))
            return false;
    id = static_cast<uint32_t>(std::stoul(s.substr(close - 8, 8), nullptr, 16));
    return true;
}

// A call-stack frame "\t[Name (0A001234)].Script.Function() - ...": the object running the script.
bool frameOwner(const std::string& line, uint32_t& id, std::string& name)
{
    const size_t open = line.find_first_not_of(" \t");
    if (open == std::string::npos || line[open] != '[')
        return false;
    const size_t end = line.find(")].", open);
    if (end == std::string::npos || !idBefore(line, end, id))
        return false;
    name = line.substr(open + 1, end - 9 - open - 1);
    while (!name.empty() && name.back() == ' ')
        name.pop_back();
    return true;
}

std::vector<std::string> splitLines(const std::string& text)
{
    std::vector<std::string> lines;
    std::string cur;
    std::istringstream in(text);
    while (std::getline(in, cur)) {
        if (!cur.empty() && cur.back() == '\r')
            cur.pop_back();
        lines.push_back(std::move(cur));
    }
    return lines;
}

std::string zstring(const Subrecord* s)
{
    return s ? s->asZString() : std::string();
}

// The scripts a VMAD attaches and the records its object properties name (as stored: file FormIDs).
void readVmad(const std::vector<uint8_t>& b, std::vector<std::string>& scripts, std::vector<uint32_t>& objects)
{
    size_t i = 0;
    auto need = [&](size_t n) {
        if (i + n > b.size())
            throw std::out_of_range("vmad");
    };
    auto u8 = [&] {
        need(1);
        return b[i++];
    };
    auto u16 = [&] {
        need(2);
        const uint16_t v = uint16_t(b[i] | (b[i + 1] << 8));
        i += 2;
        return v;
    };
    auto u32 = [&] {
        need(4);
        const uint32_t v = uint32_t(b[i]) | (uint32_t(b[i + 1]) << 8) | (uint32_t(b[i + 2]) << 16) |
                           (uint32_t(b[i + 3]) << 24);
        i += 4;
        return v;
    };
    auto wstring = [&] {
        const uint16_t n = u16();
        need(n);
        std::string s(reinterpret_cast<const char*>(b.data() + i), n);
        i += n;
        return s;
    };
    std::function<void(uint8_t)> value = [&](uint8_t type) {
        switch (type) {
        case 1: // object: unused, alias, FormID
            need(8);
            i += 4;
            objects.push_back(u32());
            return;
        case 2: wstring(); return;
        case 3:
        case 4: need(4); i += 4; return;
        case 5: need(1); i += 1; return;
        case 7: { // struct: members with name, type, status, value
            const uint32_t n = u32();
            for (uint32_t m = 0; m < n; ++m) {
                wstring();
                const uint8_t mt = u8();
                u8();
                value(mt);
            }
            return;
        }
        case 11: case 12: case 13: case 14: case 15: case 17: {
            const uint32_t n = u32();
            for (uint32_t k = 0; k < n; ++k)
                value(static_cast<uint8_t>(type - 10));
            return;
        }
        default: throw std::out_of_range("vmad type");
        }
    };
    try {
        u16(); // version
        u16(); // object format
        const uint16_t count = u16();
        for (uint16_t s = 0; s < count; ++s) {
            scripts.push_back(wstring());
            u8(); // status
            const uint16_t props = u16();
            for (uint16_t p = 0; p < props; ++p) {
                wstring();
                const uint8_t type = u8();
                u8(); // status
                value(type);
            }
        }
    } catch (const std::out_of_range&) {
        // what was read before the data ended stays
    }
}

struct RuntimeNumbers {
    std::unordered_map<std::string, uint32_t> full;  // lower-case name -> index
    std::unordered_map<std::string, uint32_t> light; // lower-case name -> slot
    explicit RuntimeNumbers(const RuntimeLoadOrder& lo)
    {
        for (uint32_t i = 0; i < lo.full.size(); ++i)
            full.emplace(lower(lo.full[i]), i);
        for (uint32_t i = 0; i < lo.light.size(); ++i)
            light.emplace(lower(lo.light[i]), i);
    }
    bool has(const std::string& plugin) const { return full.count(lower(plugin)) || light.count(lower(plugin)); }
    uint32_t id(const std::string& plugin, uint32_t local) const
    {
        const auto key = lower(plugin);
        if (auto f = full.find(key); f != full.end())
            return (f->second << 24) | (local & 0xFFFFFF);
        if (auto l = light.find(key); l != light.end())
            return 0xFE000000u | (l->second << 12) | (local & 0xFFF);
        return 0;
    }
};

} // namespace

StartupCheck checkStartup(const StartupCheckInputs& in)
{
    StartupCheck out;
    std::error_code ec;
    std::filesystem::path dir;
    for (const auto& d : {in.exportRoot / "Data", in.exportRoot})
        if (!in.exportRoot.empty() && std::filesystem::exists(d / "Multiplexer_FormMap.tsv", ec)) {
            dir = d;
            break;
        }
    const RuntimeNumbers numbers(in.loadOrder);

    // The build: its FormID map, then the quests and messages of its outputs.
    std::map<std::pair<std::string, uint32_t>, std::string> sourceOf; // (output lower, object) -> source plugin
    std::vector<std::string> outputs;
    if (!dir.empty()) {
        out.buildFound = true;
        std::set<std::string> seen;
        for (const auto& m : CompilationEngine::readFormMap(dir / "Multiplexer_FormMap.tsv")) {
            sourceOf[{lower(m.outputPlugin), m.objectId}] = m.sourcePlugin;
            if (seen.insert(lower(m.outputPlugin)).second)
                outputs.push_back(m.outputPlugin);
        }
    }
    std::unordered_map<uint32_t, StartupMessage> boxes; // message-box MESG records by runtime FormID
    std::vector<std::pair<StartupQuest, std::vector<uint32_t>>> quests;
    for (const auto& name : outputs) {
        const auto file = dir / name;
        if (!std::filesystem::exists(file, ec))
            continue;
        if (numbers.has(name))
            out.outputsActive = true;
        PluginFile plugin;
        try {
            plugin = PluginFile::load(file);
        } catch (const std::exception&) {
            continue;
        }
        const auto& masters = plugin.header().masters;
        const bool localized = plugin.header().isLocalized();
        auto runtime = [&](uint32_t fileId) {
            const uint32_t top = fileId >> 24;
            return top < masters.size() ? numbers.id(masters[top].name, fileId) : numbers.id(name, fileId);
        };
        plugin.forEachRecord([&](const RecordLocation& loc) {
            const Signature sig = loc.header.signature;
            if (sig != Signature{"QUST"} && sig != sig::MESG)
                return true;
            const Record rec = Record::parse(loc.header, loc.rawData);
            if (sig == sig::MESG) {
                const auto* dnam = rec.find(Signature{"DNAM"});
                const uint32_t flags = dnam ? dnam->as<uint32_t>().value_or(0) : 0;
                if ((flags & 1) == 0) // not shown as a message box
                    return true;
                StartupMessage m;
                m.formId = runtime(loc.header.formId);
                m.editorId = rec.editorId();
                if (!localized) {
                    m.text = zstring(rec.find(Signature{"FULL"}));
                    if (m.text.empty())
                        m.text = zstring(rec.find(Signature{"DESC"}));
                }
                if (m.text.size() > 90)
                    m.text = m.text.substr(0, 87) + "...";
                boxes.emplace(m.formId, std::move(m));
                return true;
            }
            const auto* dnam = rec.find(Signature{"DNAM"});
            StartupQuest q;
            q.startGameEnabled = dnam && (dnam->as<uint16_t>().value_or(0) & 1);
            if (!q.startGameEnabled)
                return true;
            q.formId = runtime(loc.header.formId);
            q.outputPlugin = name;
            q.editorId = rec.editorId();
            if ((loc.header.formId >> 24) >= masters.size())
                if (auto s = sourceOf.find({lower(name), loc.header.formId & 0xFFFFFF}); s != sourceOf.end())
                    q.source = s->second;
            std::vector<uint32_t> objects;
            if (const auto* vmad = rec.find(sig::VMAD))
                readVmad(vmad->data, q.scripts, objects);
            for (auto& o : objects)
                o = runtime(o);
            quests.emplace_back(std::move(q), std::move(objects));
            return true;
        });
    }

    // The log: scripts the game did not find, scripts that did not attach, objects seen running, failed popups,
    // scripts that registered for MCM settings.
    const auto lines = splitLines(in.papyrusLog);
    out.logRead = !lines.empty();
    std::unordered_set<std::string> missing;
    std::unordered_map<uint32_t, std::string> bindFailed;
    std::unordered_set<uint32_t> running;
    std::set<std::string> mcmRegistered;
    std::map<std::pair<std::string, uint32_t>, FailedPopup> popups;
    for (size_t li = 0; li < lines.size(); ++li) {
        const std::string& line = lines[li];
        if (const size_t at = line.find("Cannot open store for class \""); at != std::string::npos) {
            const size_t from = at + 29, to = line.find('"', from);
            if (to != std::string::npos)
                missing.insert(lower(line.substr(from, to - from)));
            continue;
        }
        if (const size_t at = line.find("Unable to bind script "); at != std::string::npos) {
            const size_t from = at + 22, to = line.find(' ', from);
            const size_t because = line.find(") because");
            uint32_t id = 0;
            if (to != std::string::npos && because != std::string::npos && idBefore(line, because, id))
                bindFailed.emplace(id, line.substr(from, to - from));
            continue;
        }
        if (const size_t at = line.find("OnMCMSettingChange|"); at != std::string::npos) {
            size_t to = at + 19;
            while (to < line.size() && line[to] != '"' && line[to] != '\'' && line[to] != ' ')
                ++to;
            mcmRegistered.insert(lower(line.substr(at + 19, to - at - 19)));
        }
        uint32_t id = 0;
        std::string owner;
        if (frameOwner(line, id, owner)) {
            running.insert(id);
            continue;
        }
        for (const char* call : {"Show", "ShowAsHelpMessage", "ShowOnPipboy"}) {
            const std::string what = std::string("Cannot call ") + call + "() on a None object";
            if (line.find(what) == std::string::npos)
                continue;
            for (size_t k = li + 1; k < std::min(lines.size(), li + 4); ++k)
                if (frameOwner(lines[k], id, owner)) {
                    auto& p = popups[{call, id}];
                    p.call = call;
                    p.caller = owner;
                    p.formId = id;
                    ++p.count;
                    break;
                }
            break;
        }
    }

    for (auto& [q, objects] : quests) {
        ++out.startupQuests;
        for (uint32_t o : objects)
            if (auto b = boxes.find(o); b != boxes.end() &&
                std::none_of(q.messageBoxes.begin(), q.messageBoxes.end(),
                             [&](const StartupMessage& m) { return m.formId == o; }))
                q.messageBoxes.push_back(b->second);
        if (auto f = bindFailed.find(q.formId); q.formId && f != bindFailed.end()) {
            q.failedScript = f->second;
            q.state = missing.count(lower(f->second)) ? StartupQuest::State::ScriptMissing
                                                      : StartupQuest::State::ScriptWrongType;
            ++out.notStarted;
        } else if (q.formId && running.count(q.formId)) {
            q.state = StartupQuest::State::SeenRunning;
            ++out.seenRunning;
        }
        if (!q.messageBoxes.empty())
            ++out.withMessageBoxes;
        if (q.state != StartupQuest::State::NoTrace || !q.messageBoxes.empty())
            out.quests.push_back(std::move(q));
    }
    std::sort(out.quests.begin(), out.quests.end(), [](const StartupQuest& a, const StartupQuest& b) {
        auto rank = [](const StartupQuest& q) {
            switch (q.state) {
            case StartupQuest::State::ScriptMissing:
            case StartupQuest::State::ScriptWrongType: return 0;
            case StartupQuest::State::NoTrace: return q.messageBoxes.empty() ? 3 : 1;
            case StartupQuest::State::SeenRunning: return 2;
            }
            return 3;
        };
        return std::make_tuple(rank(a), a.editorId) < std::make_tuple(rank(b), b.editorId);
    });

    for (auto& [key, p] : popups) {
        const auto [plugin, local] = in.loadOrder.resolve(p.formId);
        p.plugin = plugin;
        if (auto s = sourceOf.find({lower(plugin), local}); s != sourceOf.end()) {
            p.inOutput = true;
            p.source = s->second;
        }
        out.failedPopups.push_back(p);
    }
    std::sort(out.failedPopups.begin(), out.failedPopups.end(),
              [](const FailedPopup& a, const FailedPopup& b) { return a.inOutput > b.inOutput; });

    // MCM: the menus whose requirement the build relinked (from its relink list), and MCM's own log.
    if (!dir.empty())
        if (std::ifstream rel{dir / AutoMerger::kManifest}) {
            std::set<std::pair<std::string, std::string>> seenMenus;
            for (std::string row; std::getline(rel, row);) {
                if (!row.empty() && row.back() == '\r')
                    row.pop_back();
                if (row.empty() || row[0] == '#')
                    continue;
                std::vector<std::string> f;
                std::istringstream cells(row);
                for (std::string c; std::getline(cells, c, '\t');)
                    f.push_back(c);
                if (f.size() < 8)
                    continue;
                std::string path = f[0];
                std::replace(path.begin(), path.end(), '/', '\\');
                if (lower(path).rfind("mcm\\", 0) != 0)
                    continue;
                if (f[5] == "MCM pluginRequirements") {
                    const auto parts = split(path, '\\');
                    const std::string config = parts.size() > 2 ? parts[2] : path;
                    auto it = std::find_if(out.mcmMenus.begin(), out.mcmMenus.end(),
                                           [&](const McmMenu& m) { return lower(m.config) == lower(config); });
                    if (it == out.mcmMenus.end()) {
                        McmMenu m;
                        m.config = config;
                        m.scriptRegistered = mcmRegistered.count(lower(config)) > 0;
                        out.mcmMenus.push_back(std::move(m));
                        it = std::prev(out.mcmMenus.end());
                    }
                    const std::string pair = f[3] + " -> " + f[7];
                    if (seenMenus.insert({lower(config), lower(pair)}).second) {
                        it->required += (it->required.empty() ? "" : ", ") + f[3];
                        if (it->now.find(f[7]) == std::string::npos)
                            it->now += (it->now.empty() ? "" : ", ") + f[7];
                    }
                } else {
                    ++out.mcmReferences;
                }
            }
            std::sort(out.mcmMenus.begin(), out.mcmMenus.end(),
                      [](const McmMenu& a, const McmMenu& b) { return lower(a.config) < lower(b.config); });
        }
    for (const auto& line : splitLines(in.mcmLog)) {
        out.mcm.found = true;
        if (const size_t at = line.find("Number of mod setting files: "); at != std::string::npos)
            out.mcm.settingFiles = std::strtoul(line.c_str() + at + 29, nullptr, 10);
        else if (const size_t at2 = line.find("Registered "); at2 != std::string::npos &&
                                                               line.find(" mod settings") != std::string::npos)
            out.mcm.settings = std::strtoul(line.c_str() + at2 + 11, nullptr, 10);
        else if (line.rfind("WARNING", 0) == 0 || line.rfind("ERROR", 0) == 0 ||
                 line.find("error") != std::string::npos)
            out.mcm.warnings.push_back(line);
    }
    return out;
}

std::string formatStartupCheck(const StartupCheck& c)
{
    std::ostringstream o;
    o << "Startup popups and MCM menus\n============================\n";
    if (!c.buildFound) {
        o << "No build found (no Multiplexer_FormMap.tsv in the latest export): nothing to check.\n";
        return o.str();
    }
    if (!c.logRead)
        o << "No Papyrus log read: play a session first (Papyrus logging on in Fallout4Custom.ini: "
             "[Papyrus] bEnableLogging=1).\n";
    if (!c.outputsActive)
        o << "The outputs are not in the load order, so the log cannot name their records. Apply the build first.\n";
    o << "\nStartup quests in the outputs: " << c.startupQuests << "\n"
      << "  seen running in the log:      " << c.seenRunning << "\n"
      << "  could not start:              " << c.notStarted << "\n"
      << "  holding message boxes:        " << c.withMessageBoxes << "\n"
      << "A quest that starts without problems leaves no trace in a Papyrus log, so \"no trace\" is normal.\n";

    auto label = [](StartupQuest::State s) {
        switch (s) {
        case StartupQuest::State::SeenRunning: return "started";
        case StartupQuest::State::ScriptMissing: return "NOT STARTED: script file missing in the mod";
        case StartupQuest::State::ScriptWrongType: return "NOT STARTED: script extends another type";
        case StartupQuest::State::NoTrace: return "no trace (normal)";
        }
        return "";
    };
    auto from = [](const StartupQuest& q) {
        return q.source.empty() ? "override in " + q.outputPlugin : q.source;
    };

    o << "\nQuests that could not start\n---------------------------\n";
    size_t n = 0;
    for (const auto& q : c.quests)
        if (q.state == StartupQuest::State::ScriptMissing || q.state == StartupQuest::State::ScriptWrongType) {
            o << "  " << hex8(q.formId) << "  " << q.editorId << "  (" << from(q) << ")\n      " << label(q.state)
              << ": " << q.failedScript << "\n";
            ++n;
        }
    if (!n)
        o << "  none\n";
    o << "A missing script is the mod's own: its plugin names a script it does not ship. Compiling does not "
         "remove scripts; the same error appears without the build.\n";

    o << "\nStartup quests with message boxes\n---------------------------------\n";
    n = 0;
    for (const auto& q : c.quests) {
        if (q.messageBoxes.empty())
            continue;
        o << "  " << hex8(q.formId) << "  " << q.editorId << "  (" << from(q) << ")  " << label(q.state) << "\n";
        size_t k = 0;
        for (const auto& m : q.messageBoxes) {
            if (++k > 3) {
                o << "      ... " << (q.messageBoxes.size() - 3) << " more\n";
                break;
            }
            o << "      " << hex8(m.formId) << "  " << m.editorId << (m.text.empty() ? "" : ": \"" + m.text + "\"")
              << "\n";
        }
        ++n;
    }
    if (!n)
        o << "  none\n";
    o << "Most of these are menus or prompts shown when you use something; the ones a mod shows at start appear "
         "in the first minutes of a new game.\n";

    o << "\nPopups that found no message\n----------------------------\n";
    if (c.failedPopups.empty())
        o << "  none\n";
    for (const auto& p : c.failedPopups)
        o << "  " << p.call << "() x" << p.count << "  " << p.caller << " (" << hex8(p.formId) << ")  "
          << (p.inOutput ? "in " + p.plugin + ", from " + p.source + ": CHECK THIS ONE"
                         : (p.plugin.empty() ? std::string("not in the load order") : p.plugin + ", not compiled"))
          << "\n";
    if (!c.failedPopups.empty())
        o << "A message property that is None in a plugin that was not compiled is the mod's own setting.\n";

    o << "\nMCM menus\n---------\n";
    if (c.mcm.found)
        o << "MCM.log: " << c.mcm.settingFiles << " settings files, " << c.mcm.settings << " settings registered"
          << (c.mcm.warnings.empty() ? ", no warnings" : ", " + std::to_string(c.mcm.warnings.size()) + " warning(s)")
          << "\n";
    else
        o << "MCM.log not read.\n";
    for (const auto& w : c.mcm.warnings)
        o << "  " << w << "\n";
    o << c.mcmMenus.size() << " menu(s) require a plugin this build compiled; their requirement now names the "
      << "output, and " << c.mcmReferences << " Plugin|FormID reference(s) in MCM configs were relinked.\n"
      << "MCM hides a menu whose required plugin is not active, so each of these should appear in game. A missing "
         "one means the original config won the file conflict over the relinked copy.\n";
    for (const auto& m : c.mcmMenus)
        o << "  " << m.config << "  (" << m.required << " -> " << m.now << ")"
          << (m.scriptRegistered ? "  its script registered for setting changes" : "") << "\n";
    return o.str();
}

RuntimeLoadOrder loadOrderFromF4seLog(std::string_view text)
{
    RuntimeLoadOrder lo;
    bool inList = false;
    for (const auto& line : splitLines(std::string(text))) {
        if (line.find("Saving plugin list:") != std::string::npos) {
            if (!lo.full.empty() || !lo.light.empty())
                break; // the first list is enough: every save in a session writes the same one
            inList = true;
            continue;
        }
        if (!inList)
            continue;
        const size_t open = line.find('[');
        const size_t close = line.find(']', open == std::string::npos ? 0 : open);
        if (open == std::string::npos || close == std::string::npos || line.find_first_not_of(" \t") != open) {
            if (!lo.full.empty() || !lo.light.empty())
                break;
            continue;
        }
        std::string name = line.substr(close + 1);
        name.erase(0, name.find_first_not_of(" \t"));
        while (!name.empty() && (name.back() == ' ' || name.back() == '\t'))
            name.pop_back();
        const std::string slot = line.substr(open + 1, close - open - 1);
        if (slot.rfind("FE:", 0) == 0) {
            const size_t i = std::strtoul(slot.c_str() + 3, nullptr, 10);
            if (lo.light.size() <= i)
                lo.light.resize(i + 1);
            lo.light[i] = name;
        } else {
            const size_t i = std::strtoul(slot.c_str(), nullptr, 10);
            if (lo.full.size() <= i)
                lo.full.resize(i + 1);
            lo.full[i] = name;
        }
    }
    return lo;
}

std::filesystem::path defaultMcmLog()
{
    return platform::fallout4DocumentsDir() / "F4SE" / "MCM.log";
}

} // namespace fo4mx::diagnostics
