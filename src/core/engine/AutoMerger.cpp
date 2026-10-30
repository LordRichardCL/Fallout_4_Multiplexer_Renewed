#include "engine/AutoMerger.hpp"

#include "engine/ReferenceScanner.hpp"
#include "papyrus/Pex.hpp"
#include "util/Binary.hpp"
#include "util/Platform.hpp"
#include "util/Text.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <tuple>
#include <unordered_map>

#include <spdlog/spdlog.h>

namespace fo4mx {

const char* relinkKindName(RelinkKind k)
{
    switch (k) {
    case RelinkKind::Script: return "Script";
    case RelinkKind::McmConfig: return "MCM Config";
    case RelinkKind::PairConfig: return "Config";
    case RelinkKind::Unsupported: return "Not Relinkable";
    }
    return "";
}

namespace {

constexpr const char* kLegacyList = "Multiplexer_Relinked.txt"; // written by earlier versions
constexpr PluginIndex kOutputBase = 0xFF000000u;              // output plugins in a rescan's name table

bool isScript(const std::string& path)
{
    return iendsWith(path, ".pex");
}
bool isMcmConfig(const std::string& path)
{
    return istartsWith(path, "mcm\\config\\") && iendsWith(path, ".json");
}
bool isText(const std::string& path)
{
    for (const char* ext : {".ini", ".json", ".txt", ".toml", ".xml"})
        if (iendsWith(path, ext))
            return true;
    return false;
}

uint64_t hashOf(const std::vector<uint8_t>& b)
{
    return fnv1a64(b.data(), b.size());
}

std::string hex64(uint64_t v)
{
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

std::string hexLike(uint32_t value, size_t width)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%X", value);
    std::string s = buf;
    if (s.size() < width)
        s.insert(0, width - s.size(), '0');
    return s;
}

// Data-relative asset path ("scripts\\x.pex") as a filesystem path below `dir`.
std::filesystem::path below(const std::filesystem::path& dir, std::string relative)
{
    std::replace(relative.begin(), relative.end(), '\\', '/');
    return dir / utf8ToPath(relative);
}

std::vector<uint8_t> readFile(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Where the records of a compiled plugin went: (plugin, object ID) -> (output, new object ID).
struct Relocation {
    std::unordered_map<uint64_t, std::pair<std::string, uint32_t>> byRecord;
    std::map<PluginIndex, std::string> primaryOutput; // output holding most of a plugin's records

    explicit Relocation(const CompiledOutput& compiled)
    {
        std::map<PluginIndex, std::map<std::string, size_t>> perOutput;
        for (const auto& m : compiled.mappings) {
            if (!m.source.valid())
                continue;
            byRecord[(uint64_t(m.source.origin) << 32) | (m.sourceObjectId & 0xFFFFFF)] = {m.outputPlugin, m.objectId};
            perOutput[m.source.origin][m.outputPlugin]++;
        }
        for (const auto& [plugin, outputs] : perOutput) {
            auto best = std::max_element(outputs.begin(), outputs.end(),
                                         [](const auto& a, const auto& b) { return a.second < b.second; });
            primaryOutput[plugin] = best->first;
        }
    }
    const std::pair<std::string, uint32_t>* find(const Session& session, PluginIndex plugin, uint32_t formId) const
    {
        const uint32_t mask = session.plugins[plugin].isLight() ? 0xFFF : 0xFFFFFF;
        auto it = byRecord.find((uint64_t(plugin) << 32) | (formId & mask));
        return it == byRecord.end() ? nullptr : &it->second;
    }
};

// A span of the original file and what replaces it.
struct Edit {
    size_t begin = 0, end = 0;
    std::string text;
};

// What a reference looks like for the "unchanged" comparison.
using RefKey = std::tuple<PluginIndex, int, std::string, std::string, std::string, bool, uint32_t>;
RefKey keyOf(const PluginReference& r)
{
    return {r.plugin, static_cast<int>(r.cls), r.context, toLower(r.text), r.location, r.hasFormId, r.formId};
}

using Expected = std::multiset<std::tuple<std::string, bool, uint32_t>>; // (output, has FormID, FormID)

// The checks every planned copy must pass. Returns the first failure (empty: passed).
std::string check(const ReferenceScanner::PluginNames& names, const std::string& path,
                  const std::vector<uint8_t>& original, const std::vector<uint8_t>& out,
                  const std::vector<PluginReference>& before, const std::set<PluginIndex>& compiledHere,
                  const std::vector<std::string>& outputNames, const Expected& expected, size_t expectedLeft,
                  const std::vector<Edit>& edits, const std::vector<pex::Patch>& patches)
{
    // 1. Scan the copy again, with the output plugins known by name.
    auto rescanNames = names;
    for (size_t i = 0; i < outputNames.size(); ++i)
        rescanNames[toLower(outputNames[i])] = kOutputBase + static_cast<PluginIndex>(i);
    const auto after = ReferenceScanner::inspect(rescanNames, path, out);

    // 2. Every expected change is there, in a rewritable use, and nothing else names an output.
    Expected found;
    for (const auto& r : after)
        if (r.plugin >= kOutputBase) {
            if (r.cls != ReferenceClass::Rewritable)
                return "the copy names " + r.text + " in a use that is not rewritable (" + r.context + ")";
            found.insert({toLower(r.text.substr(0, r.text.find('|'))), r.hasFormId, r.hasFormId ? r.formId : 0});
        }
    if (found != expected)
        return "the copy does not contain exactly the expected changes (" + std::to_string(found.size()) + " found, " +
               std::to_string(expected.size()) + " expected)";

    // 3. A compiled plugin is named only where it was left on purpose (a FormID the plugin does not
    //    define: the game found nothing before either), or where its name is safe.
    size_t left = 0;
    for (const auto& r : after)
        if (r.plugin < kOutputBase && compiledHere.count(r.plugin) && r.cls != ReferenceClass::Safe)
            ++left;
    if (left != expectedLeft)
        return "the copy still depends on a compiled plugin's file name (" + std::to_string(left) + " references)";

    // 4. Every reference to another plugin is exactly as it was; every safe reference to a compiled
    //    plugin is still there. The only new safe one allowed is the original name left unused in a
    //    script's string table once its uses point at the output.
    std::multiset<RefKey> otherBefore, otherAfter, safeBefore, safeAfter;
    for (const auto& r : before) {
        if (!compiledHere.count(r.plugin))
            otherBefore.insert(keyOf(r));
        else if (r.cls == ReferenceClass::Safe)
            safeBefore.insert(keyOf(r));
    }
    for (const auto& r : after) {
        if (r.plugin >= kOutputBase)
            continue;
        if (!compiledHere.count(r.plugin))
            otherAfter.insert(keyOf(r));
        else if (r.cls == ReferenceClass::Safe)
            safeAfter.insert(keyOf(r));
    }
    if (otherBefore != otherAfter)
        return "a reference to another plugin was changed";
    for (const auto& k : safeBefore) {
        auto it = safeAfter.find(k);
        if (it == safeAfter.end())
            return "a safe reference was changed";
        safeAfter.erase(it);
    }
    for (const auto& k : safeAfter)
        if (std::get<2>(k) != "string table")
            return "the copy has a new reference to a compiled plugin (" + std::get<2>(k) + ")";

    // 5. Every other byte is identical.
    if (iendsWith(path, ".pex")) {
        const auto a = pex::scan(original), b = pex::scan(out);
        if (!b.ok)
            return "the copy does not read back as a script (" + b.error + ")";
        if (b.uses.size() != a.uses.size() || b.strings.size() < a.strings.size())
            return "the copy's code differs from the original";
        for (size_t i = 0; i < a.strings.size(); ++i)
            if (a.strings[i] != b.strings[i])
                return "a string of the original was changed";
        const size_t shift = b.stringTableEnd;
        if (out.size() - shift != original.size() - a.stringTableEnd ||
            !std::equal(original.begin(), original.begin() + static_cast<std::ptrdiff_t>(a.stringTableBegin), out.begin()))
            return "the copy's layout differs from the original";
        std::vector<bool> patched(original.size() - a.stringTableEnd, false);
        for (const auto& p : patches)
            for (size_t k = 0; k < p.bytes.size(); ++k)
                patched[p.offset - a.stringTableEnd + k] = true;
        for (size_t k = 0; k < patched.size(); ++k)
            if (!patched[k] && original[a.stringTableEnd + k] != out[shift + k])
                return "a byte outside the changed references differs";
    } else {
        size_t from = 0, at = 0;
        for (const auto& e : edits) {
            const size_t n = e.begin - from;
            if (at + n > out.size() || !std::equal(original.begin() + static_cast<std::ptrdiff_t>(from),
                                                   original.begin() + static_cast<std::ptrdiff_t>(e.begin),
                                                   out.begin() + static_cast<std::ptrdiff_t>(at)))
                return "text outside the changed references differs";
            at += n + e.text.size();
            from = e.end;
        }
        if (at > out.size() || out.size() - at != original.size() - from ||
            !std::equal(original.begin() + static_cast<std::ptrdiff_t>(from), original.end(),
                        out.begin() + static_cast<std::ptrdiff_t>(at)))
            return "text outside the changed references differs";
    }
    return {};
}

} // namespace

std::unordered_map<std::string, uint64_t> AutoMerger::readManifest(const std::filesystem::path& dir)
{
    std::unordered_map<std::string, uint64_t> files;
    if (std::ifstream in{dir / kManifest}) {
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.empty() || line[0] == '#')
                continue;
            auto cols = split(line, '\t', false);
            if (cols.size() >= 2)
                files[cols[0]] = std::strtoull(cols[1].c_str(), nullptr, 16);
        }
    }
    if (std::ifstream in{dir / kLegacyList}) {
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (!line.empty() && line[0] != '#')
                files.emplace(line, 0); // no hash recorded
        }
    }
    return files;
}

AutoMergeReport AutoMerger::analyse(const Session& session)
{
    AutoMergeReport report;
    report.enabled = true;
    std::map<std::string, size_t> byPath;
    for (size_t i = 0; i < session.nameReferences.size(); ++i) {
        const auto& r = session.nameReferences[i];
        auto [it, added] = byPath.emplace(r.assetPath, report.files.size());
        if (added) {
            RelinkFile f;
            f.path = r.assetPath;
            f.mod = r.source;
            report.files.push_back(std::move(f));
        }
        auto& f = report.files[it->second];
        f.references.push_back(i);
        if (std::find(f.plugins.begin(), f.plugins.end(), r.plugin) == f.plugins.end())
            f.plugins.push_back(r.plugin);
        if (static_cast<int>(r.cls) > static_cast<int>(f.cls))
            f.cls = r.cls;
        if (r.cls == ReferenceClass::Rewritable)
            ++(!r.hasFormId ? f.mentions : r.format == "PEX" ? f.formCalls : f.pairs);
    }
    for (size_t i = 0; i < report.files.size(); ++i) {
        auto& f = report.files[i];
        std::sort(f.plugins.begin(), f.plugins.end());
        std::vector<std::string> problems;
        for (size_t ri : f.references) {
            const auto& r = session.nameReferences[ri];
            if (r.cls == ReferenceClass::Safe || r.cls == ReferenceClass::Rewritable)
                continue;
            std::string p = std::string(referenceClassName(r.cls)) + ": " + r.context + " (" + r.reason + ")";
            if (std::find(problems.begin(), problems.end(), p) == problems.end())
                problems.push_back(p);
        }
        f.problem = join(problems, "; ");
        f.kind = !f.problem.empty()  ? RelinkKind::Unsupported
                 : isScript(f.path)    ? RelinkKind::Script
                 : isMcmConfig(f.path) ? RelinkKind::McmConfig
                 : isText(f.path)      ? RelinkKind::PairConfig
                                       : RelinkKind::Unsupported;
        for (PluginIndex p : f.plugins)
            report.filesOf[p].push_back(i);
    }
    for (const auto& [plugin, d] : session.referenceDecisions)
        if (d.cls == ReferenceClass::Safe || d.cls == ReferenceClass::Rewritable)
            report.relinkable.insert(plugin);
    spdlog::info("Auto-Merger: {} files name {} plugins; {} plugins have only safe or rewritable references",
                 report.files.size(), report.filesOf.size(), report.relinkable.size());
    return report;
}

std::map<PluginIndex, std::string> AutoMerger::plan(const Session& session, const CompiledOutput& compiled,
                                                    const std::filesystem::path& outputDir, AutoMergeReport& report)
{
    std::map<PluginIndex, std::string> failed;
    const Relocation where(compiled);
    const auto names = ReferenceScanner::pluginNames(session);
    const auto previous = readManifest(outputDir);
    std::vector<std::string> outputNames;
    std::set<std::string> outputSet;
    for (const auto& o : compiled.outputs) {
        outputNames.push_back(o.name);
        outputSet.insert(toLower(o.name));
    }

    for (auto& f : report.files) {
        f.planned = false;
        f.changes.clear();
        f.output.clear();
        // A failed check of an earlier round of this compilation stays on record (its plugins are kept out).
        if (std::none_of(f.plugins.begin(), f.plugins.end(), [&](PluginIndex p) { return report.keptOut.count(p); }))
            f.validation.clear();
        f.notes.clear();
        f.originalHash = f.outputHash = 0;
        f.changedCalls = f.changedPairs = f.renamed = 0;

        std::set<PluginIndex> compiledHere;
        std::vector<const PluginReference*> todo;
        std::string blocking;
        for (size_t ri : f.references) {
            const auto& r = session.nameReferences[ri];
            if (!compiled.isCompiled(r.plugin) || r.cls == ReferenceClass::Safe)
                continue;
            compiledHere.insert(r.plugin);
            if (r.cls == ReferenceClass::Rewritable)
                todo.push_back(&r);
            else
                blocking = std::string("compiled although it has a ") + referenceClassName(r.cls) + " reference (" +
                           r.context + ")";
        }
        if (compiledHere.empty())
            continue;

        try {
            if (!blocking.empty())
                throw std::runtime_error(blocking);
            const auto* entry = session.assets.find(f.path);
            if (!entry)
                throw std::runtime_error("not found in the load order's files");
            // A file in the output folder that this app did not write would be replaced: never do that.
            const auto target = below(outputDir, f.path);
            std::error_code ec;
            if (std::filesystem::exists(target, ec)) {
                auto it = previous.find(f.path);
                if (it == previous.end() || (it->second != 0 && it->second != hashOf(readFile(target))))
                    throw std::runtime_error("the output folder already has this file and it was not written by this "
                                             "app; it is never replaced (use a separate output folder, such as a Mod "
                                             "Organizer 2 mod)");
            }
            const auto original = ReferenceScanner::readOriginal(session, *entry);
            f.originalHash = hashOf(original);
            const auto before = ReferenceScanner::inspect(names, f.path, original);

            Expected expected;
            size_t expectedLeft = 0;
            std::vector<Edit> edits;
            std::vector<pex::Patch> patches;
            auto change = [&](const PluginReference& r, const std::string& after) {
                f.changes.push_back({r.plugin, r.location, r.context, r.text, after});
            };
            auto leave = [&](const PluginReference& r) {
                ++expectedLeft;
                f.notes.push_back(r.text + " (" + r.location + "): " + formatFormId(r.formId) + " is not a record of " +
                                  session.plugins[r.plugin].name +
                                  ", so the game found nothing before either: left as it was");
            };
            auto primaryOf = [&](PluginIndex p) {
                auto it = where.primaryOutput.find(p);
                if (it == where.primaryOutput.end())
                    throw std::runtime_error(session.plugins[p].name + " has no records in the outputs");
                return it->second;
            };

            if (isScript(f.path)) {
                const auto scan = pex::scan(original);
                if (!scan.ok)
                    throw std::runtime_error("the script does not read back (" + scan.error + ")");
                auto strings = scan.strings;
                auto indexOf = [&](const std::string& s) {
                    for (size_t i = 0; i < strings.size(); ++i)
                        if (strings[i] == s)
                            return i;
                    strings.push_back(s); // a new string: other uses of the plugin name keep theirs
                    return strings.size() - 1;
                };
                for (const auto* r : todo) {
                    if (r->pexStringOffset < scan.stringTableEnd || r->pexStringOffset + 2 > original.size() ||
                        loadLE<uint16_t>(original.data() + r->pexStringOffset) >= scan.strings.size() ||
                        toLower(scan.strings[loadLE<uint16_t>(original.data() + r->pexStringOffset)]) !=
                            session.plugins[r->plugin].key)
                        throw std::runtime_error("the script changed since it was scanned");
                    std::string to;
                    if (r->hasFormId) {
                        auto moved = where.find(session, r->plugin, r->formId);
                        if (!moved) {
                            leave(*r);
                            continue;
                        }
                        to = moved->first;
                        pex::Patch id{r->pexFormIdOffset, std::vector<uint8_t>(4)};
                        storeLE<uint32_t>(id.bytes.data(), moved->second);
                        patches.push_back(std::move(id));
                        expected.insert({toLower(to), true, moved->second});
                        change(*r, to + " " + formatFormId(moved->second));
                        ++f.changedCalls;
                    } else {
                        to = primaryOf(r->plugin);
                        expected.insert({toLower(to), false, 0});
                        change(*r, to);
                        ++f.renamed;
                    }
                    const size_t index = indexOf(to);
                    if (strings.size() > 0xFFFF)
                        throw std::runtime_error("the script's string table is full (65535 strings)");
                    pex::Patch use{r->pexStringOffset, std::vector<uint8_t>(2)};
                    storeLE<uint16_t>(use.bytes.data(), static_cast<uint16_t>(index));
                    patches.push_back(std::move(use));
                }
                if (!f.changes.empty())
                    f.output = pex::rewrite(original, scan, strings, patches);
            } else {
                const std::string_view text(reinterpret_cast<const char*>(original.data()), original.size());
                for (const auto* r : todo) {
                    if (r->nameEnd > text.size() || r->nameEnd <= r->nameBegin ||
                        toLower(std::string(text.substr(r->nameBegin, r->nameEnd - r->nameBegin))) !=
                            session.plugins[r->plugin].key)
                        throw std::runtime_error("the file changed since it was scanned");
                    if (r->hasFormId) {
                        auto moved = where.find(session, r->plugin, r->formId);
                        if (!moved) {
                            leave(*r);
                            continue;
                        }
                        const std::string id = hexLike(moved->second, r->idEnd - r->idBegin);
                        edits.push_back({r->nameBegin, r->nameEnd, moved->first});
                        edits.push_back({r->idBegin, r->idEnd, id});
                        expected.insert({toLower(moved->first), true, moved->second});
                        change(*r, moved->first + "|" + id);
                        ++f.changedPairs;
                    } else {
                        const auto to = primaryOf(r->plugin);
                        edits.push_back({r->nameBegin, r->nameEnd, to});
                        expected.insert({toLower(to), false, 0});
                        change(*r, to);
                        ++f.renamed;
                    }
                }
                std::sort(edits.begin(), edits.end(), [](const Edit& a, const Edit& b) { return a.begin < b.begin; });
                for (size_t i = 1; i < edits.size(); ++i)
                    if (edits[i].begin < edits[i - 1].end)
                        throw std::runtime_error("overlapping changes");
                std::string out;
                size_t from = 0;
                for (const auto& e : edits) {
                    out.append(text.substr(from, e.begin - from));
                    out += e.text;
                    from = e.end;
                }
                out.append(text.substr(from));
                if (!f.changes.empty())
                    f.output.assign(out.begin(), out.end());
            }
            if (f.changes.empty())
                continue; // every reference left as it was: no copy needed
            for (const auto& c : f.changes)
                if (!outputSet.count(toLower(c.after.substr(0, c.after.find_first_of(" |")))))
                    throw std::runtime_error(c.after + " is not an output of this build");
            auto problem = check(names, f.path, original, f.output, before, compiledHere, outputNames, expected,
                                 expectedLeft, edits, patches);
            if (!problem.empty())
                throw std::runtime_error(problem);
            f.outputHash = hashOf(f.output);
            f.planned = true;
        } catch (const std::exception& e) {
            f.validation = e.what();
            f.output.clear();
            f.changes.clear();
            for (PluginIndex p : compiledHere)
                failed.emplace(p, "Advanced Users Auto-Merger: " + f.path + " could not be rewritten safely (" +
                                      f.validation + ")");
        }
    }
    report.keptOut.insert(failed.begin(), failed.end());
    size_t planned = 0;
    for (const auto& f : report.files)
        planned += f.planned;
    spdlog::info("Auto-Merger: {} copies planned and checked; {} plugins kept out by a failed check", planned,
                 failed.size());
    return failed;
}

void AutoMerger::relink(const Session& session, const std::filesystem::path& outputDir, AutoMergeReport& report)
{
    namespace fs = std::filesystem;
    report.written.clear();
    report.errors.clear();
    for (auto& f : report.files)
        f.written = false;
    const auto previous = readManifest(outputDir);

    std::vector<RelinkFile*> planned;
    for (auto& f : report.files)
        if (f.planned)
            planned.push_back(&f);

    // 1. Write every copy beside its target and read it back.
    std::vector<std::pair<fs::path, fs::path>> staged; // (temporary, target)
    auto discard = [&] {
        std::error_code ec;
        for (const auto& st : staged)
            fs::remove(st.first, ec);
    };
    for (auto* f : planned) {
        const auto target = below(outputDir, f->path);
        auto tmp = target;
        tmp += ".fo4mx-new";
        try {
            std::error_code ec;
            if (fs::exists(target, ec)) {
                auto it = previous.find(f->path);
                if (it == previous.end() || (it->second != 0 && it->second != hashOf(readFile(target))))
                    throw std::runtime_error("a file this app did not write is in the output folder; it is not replaced");
            }
            fs::create_directories(target.parent_path());
            {
                std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
                out.write(reinterpret_cast<const char*>(f->output.data()), static_cast<std::streamsize>(f->output.size()));
                if (!out)
                    throw std::runtime_error("could not be written");
            }
            staged.emplace_back(tmp, target);
            if (hashOf(readFile(tmp)) != f->outputHash)
                throw std::runtime_error("did not read back as written");
        } catch (const std::exception& e) {
            report.errors.push_back(f->path + ": " + e.what());
            break;
        }
    }
    if (!report.errors.empty()) {
        discard();
        spdlog::error("Auto-Merger: nothing was relinked: {}", report.errors.front());
        return;
    }

    // 2. Move them into place, keeping the earlier copies until every move succeeded.
    std::vector<std::pair<fs::path, fs::path>> moved; // (target, backup or empty)
    std::string failure;
    for (const auto& [tmp, target] : staged) {
        std::error_code ec;
        fs::path backup;
        if (fs::exists(target, ec)) {
            backup = target;
            backup += ".fo4mx-old";
            fs::rename(target, backup, ec);
            if (ec) {
                failure = pathToUtf8(target) + ": " + ec.message();
                break;
            }
        }
        fs::rename(tmp, target, ec);
        if (ec) {
            std::error_code ignored;
            if (!backup.empty())
                fs::rename(backup, target, ignored);
            failure = pathToUtf8(target) + ": " + ec.message();
            break;
        }
        moved.emplace_back(target, backup);
    }
    if (!failure.empty()) {
        // Roll back: remove every new copy, restore every earlier one.
        for (auto it = moved.rbegin(); it != moved.rend(); ++it) {
            std::error_code ec;
            fs::remove(it->first, ec);
            if (!it->second.empty())
                fs::rename(it->second, it->first, ec);
        }
        discard();
        report.errors.push_back("relinking was rolled back: " + failure);
        spdlog::error("Auto-Merger: {}", report.errors.back());
        return;
    }
    for (const auto& mv : moved) {
        std::error_code ec;
        if (!mv.second.empty())
            fs::remove(mv.second, ec);
    }

    // 3. Remove copies an earlier export wrote that this build no longer produces (unless changed since).
    for (const auto& [path, hash] : previous) {
        if (std::any_of(planned.begin(), planned.end(), [&](const RelinkFile* f) { return f->path == path; }))
            continue;
        const auto target = below(outputDir, path);
        std::error_code ec;
        if (fs::exists(target, ec) && (hash == 0 || hashOf(readFile(target)) == hash))
            fs::remove(target, ec);
    }

    // 4. The manifest: every change with its old and new value.
    std::error_code ec;
    fs::remove(outputDir / kLegacyList, ec);
    const auto manifest = outputDir / kManifest;
    if (planned.empty()) {
        fs::remove(manifest, ec);
    } else {
        auto tmp = manifest;
        tmp += ".fo4mx-new";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            out << "# Scripts and configs rewritten by the Advanced Users Auto-Merger. They override the originals,\n"
                   "# which are not changed. Delete the files listed here (and this file) to undo.\n"
                   "# file\twritten fnv1a64\toriginal fnv1a64\tplugin\tlocation\tuse\tbefore\tafter\n";
            for (const auto* f : planned)
                for (const auto& c : f->changes)
                    out << f->path << '\t' << hex64(f->outputHash) << '\t' << hex64(f->originalHash) << '\t'
                        << session.plugins[c.plugin].name << '\t' << c.location << '\t' << c.context << '\t'
                        << c.before << '\t' << c.after << '\n';
        }
        fs::rename(tmp, manifest, ec);
        if (ec)
            report.errors.push_back(pathToUtf8(manifest) + ": " + ec.message());
    }
    for (auto* f : planned) {
        f->written = true;
        report.written.push_back(below(outputDir, f->path));
    }
    spdlog::info("Auto-Merger: relinked {} files ({} errors)", planned.size(), report.errors.size());
}

size_t AutoMerger::rollback(const std::filesystem::path& outputDir)
{
    size_t removed = 0;
    for (const auto& [path, hash] : readManifest(outputDir)) {
        const auto target = below(outputDir, path);
        std::error_code ec;
        if (std::filesystem::exists(target, ec) && (hash == 0 || hashOf(readFile(target)) == hash) &&
            std::filesystem::remove(target, ec))
            ++removed;
    }
    std::error_code ec;
    std::filesystem::remove(outputDir / kManifest, ec);
    std::filesystem::remove(outputDir / kLegacyList, ec);
    return removed;
}

} // namespace fo4mx
