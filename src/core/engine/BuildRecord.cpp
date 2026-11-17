#include "engine/BuildRecord.hpp"

#include "util/FileTransaction.hpp"
#include "util/Text.hpp"

#include <cstdio>
#include <fstream>
#include <stdexcept>

namespace fo4mx::buildrecord {

namespace fs = std::filesystem;

namespace {

std::string hex(uint64_t v)
{
    char b[24];
    std::snprintf(b, sizeof(b), "%016llx", static_cast<unsigned long long>(v));
    return b;
}

uint64_t unhex(const std::string& s)
{
    return s.empty() ? 0 : std::strtoull(s.c_str(), nullptr, 16);
}

std::string relativeTo(const fs::path& file, const fs::path& base)
{
    if (file.empty())
        return {};
    const auto rel = file.lexically_relative(base);
    if (rel.empty() || rel.generic_string().rfind("..", 0) == 0)
        return {};
    return rel.generic_string();
}

json::Value strings(const std::vector<std::string>& v)
{
    json::Array a;
    for (const auto& s : v)
        a.emplace_back(s);
    return json::Value(std::move(a));
}

std::vector<std::string> strings(const json::Value& v)
{
    std::vector<std::string> out;
    if (v.isArray())
        for (const auto& e : v.asArray())
            out.push_back(e.asString());
    return out;
}

} // namespace

std::optional<uint64_t> hashFile(const fs::path& file)
{
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return std::nullopt;
    uint64_t h = 0xcbf29ce484222325ULL;
    std::vector<char> buf(1 << 20);
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const auto n = static_cast<size_t>(in.gcount());
        if (n)
            h = fnv1a64(buf.data(), n, h);
    }
    if (in.bad())
        return std::nullopt;
    return h;
}

Record make(const Session& session, const CompiledOutput& c, const ExportResult& e, const InstallSnapshot& snapshot,
            uint64_t settingsHash)
{
    Record r;
    const fs::path base = e.packageRoot.empty() ? e.directory : e.packageRoot;
    r.run = e.runName;
    r.settingsHash = settingsHash;
    r.snapshot = snapshot;
    r.f4seArchiveLoader = c.f4seArchiveLoader;
    for (auto p : c.compiledPlugins)
        r.compiled.push_back(session.plugins[p].name);
    for (auto p : c.archiveLoaders)
        r.archiveLoaders.push_back(session.plugins[p].name);
    r.archivesToRegister = c.archivesToRegister;
    for (const auto& op : c.outputs) {
        if (!op.hasRecords())
            continue;
        Output o;
        o.name = op.name;
        o.light = op.light;
        o.patch = op.patch;
        o.newRecords = op.newRecords;
        o.overrides = op.overrides;
        for (const auto& f : e.pluginFiles)
            if (iequals(pathToUtf8(f.filename()), op.name)) {
                o.file = relativeTo(f, base);
                std::error_code ec;
                o.size = fs::file_size(f, ec);
                o.hash = hashFile(f).value_or(0);
            }
        r.outputs.push_back(std::move(o));
    }
    r.directory = relativeTo(e.directory, base);
    r.zip = relativeTo(e.zipPath, base);
    r.disableList = relativeTo(e.disableList, base);
    r.recorderFolder = relativeTo(e.recorderFolder, base);
    for (const auto& f : e.archiveLoaderFiles)
        r.archiveLoaderFiles.push_back(relativeTo(f, base));
    for (const auto& f : e.archiveLoaders)
        r.loaderPlugins.push_back(relativeTo(f, base));
    return r;
}

void write(const fs::path& exportFolder, const Record& r)
{
    json::Value v(json::Object{});
    v.set("format", r.format);
    v.set("run", r.run);
    v.set("settingsHash", hex(r.settingsHash));
    v.set("f4seArchiveLoader", r.f4seArchiveLoader);
    v.set("compiled", strings(r.compiled));
    v.set("archiveLoaders", strings(r.archiveLoaders));
    v.set("archivesToRegister", strings(r.archivesToRegister));
    json::Array outs;
    for (const auto& o : r.outputs) {
        json::Value ov(json::Object{});
        ov.set("name", o.name);
        ov.set("light", o.light);
        ov.set("patch", o.patch);
        ov.set("newRecords", o.newRecords);
        ov.set("overrides", o.overrides);
        ov.set("file", o.file);
        ov.set("size", hex(o.size));
        ov.set("hash", hex(o.hash));
        outs.push_back(std::move(ov));
    }
    v.set("outputs", json::Value(std::move(outs)));
    json::Value files(json::Object{});
    files.set("directory", r.directory);
    files.set("zip", r.zip);
    files.set("disableList", r.disableList);
    files.set("recorderFolder", r.recorderFolder);
    files.set("archiveLoaderFiles", strings(r.archiveLoaderFiles));
    files.set("loaderPlugins", strings(r.loaderPlugins));
    v.set("files", std::move(files));
    v.set("snapshot", r.snapshot.toJson());
    atomicWriteText(exportFolder / kFile, v.dump());
}

Record read(const fs::path& exportFolder)
{
    std::ifstream in(exportFolder / kFile, std::ios::binary);
    if (!in)
        throw std::runtime_error(std::string(kFile) + " is not in the export");
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto v = json::Value::parse(text);
    Record r;
    r.format = static_cast<int>(v["format"].asNumber());
    if (r.format != kFormat)
        throw std::runtime_error(std::string(kFile) + " has format " + std::to_string(r.format) + ", this app reads " +
                                 std::to_string(kFormat));
    r.run = v["run"].asString();
    r.settingsHash = unhex(v["settingsHash"].asString());
    r.f4seArchiveLoader = v["f4seArchiveLoader"].asBool();
    r.compiled = strings(v["compiled"]);
    r.archiveLoaders = strings(v["archiveLoaders"]);
    r.archivesToRegister = strings(v["archivesToRegister"]);
    if (v["outputs"].isArray())
        for (const auto& ov : v["outputs"].asArray()) {
            Output o;
            o.name = ov["name"].asString();
            o.light = ov["light"].asBool();
            o.patch = ov["patch"].asBool();
            o.newRecords = static_cast<uint32_t>(ov["newRecords"].asNumber());
            o.overrides = static_cast<uint32_t>(ov["overrides"].asNumber());
            o.file = ov["file"].asString();
            o.size = unhex(ov["size"].asString());
            o.hash = unhex(ov["hash"].asString());
            r.outputs.push_back(std::move(o));
        }
    const auto& files = v["files"];
    r.directory = files["directory"].asString();
    r.zip = files["zip"].asString();
    r.disableList = files["disableList"].asString();
    r.recorderFolder = files["recorderFolder"].asString();
    r.archiveLoaderFiles = strings(files["archiveLoaderFiles"]);
    r.loaderPlugins = strings(files["loaderPlugins"]);
    r.snapshot = InstallSnapshot::fromJson(v["snapshot"]);
    if (r.compiled.empty() || r.outputs.empty())
        throw std::runtime_error(std::string(kFile) + " lists no compiled plugins or no outputs");
    return r;
}

} // namespace fo4mx::buildrecord
