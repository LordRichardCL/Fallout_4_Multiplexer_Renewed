#include "engine/BuildCache.hpp"

#include "util/Binary.hpp"
#include "util/Compression.hpp"
#include "util/Text.hpp"

#include <cstdio>
#include <fstream>
#include <unordered_map>

#include <spdlog/spdlog.h>

namespace fo4mx {

namespace {

constexpr uint32_t kFormatVersion = 1;

const char* kCacheSchema = R"SQL(
CREATE TABLE IF NOT EXISTS Info (key TEXT PRIMARY KEY, value TEXT);
CREATE TABLE IF NOT EXISTS PluginIndex (path TEXT PRIMARY KEY, size INTEGER, modified INTEGER, sample INTEGER, data BLOB);
CREATE TABLE IF NOT EXISTS conflicts (key INTEGER PRIMARY KEY, data BLOB);
CREATE TABLE IF NOT EXISTS merges (key INTEGER PRIMARY KEY, data BLOB);
CREATE TABLE IF NOT EXISTS edges (key INTEGER PRIMARY KEY, data BLOB);
)SQL";

std::vector<uint8_t> pack(const std::vector<uint8_t>& raw)
{
    BinaryWriter w;
    w.write<uint32_t>(static_cast<uint32_t>(raw.size()));
    w.writeBytes(zlibDeflate(raw, 1));
    return w.take();
}

std::optional<std::vector<uint8_t>> unpack(std::span<const uint8_t> blob)
{
    if (blob.size() < 4)
        return std::nullopt;
    try {
        return zlibInflate(blob.subspan(4), loadLE<uint32_t>(blob.data()));
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

void writeString(BinaryWriter& w, std::string_view s)
{
    w.write<uint32_t>(static_cast<uint32_t>(s.size()));
    w.writeBytes(s.data(), s.size());
}

std::string readString(BinaryReader& r)
{
    uint32_t n = r.read<uint32_t>();
    auto b = r.readBytes(n);
    return std::string(reinterpret_cast<const char*>(b.data()), b.size());
}

// Collects the plugin names a payload refers to.
class NameTable {
public:
    explicit NameTable(const Session& s) : session_(s) {}
    uint32_t id(PluginIndex p)
    {
        if (p == kInvalidPlugin || p >= session_.plugins.size())
            return 0xFFFFFFFFu;
        auto [it, inserted] = ids_.try_emplace(p, static_cast<uint32_t>(names_.size()));
        if (inserted)
            names_.push_back(session_.plugins[p].name);
        return it->second;
    }
    void key(BinaryWriter& w, FormKey k)
    {
        w.write<uint32_t>(id(k.origin));
        w.write<uint32_t>(k.objectId);
    }
    std::vector<uint8_t> finish(const BinaryWriter& payload) const
    {
        BinaryWriter out;
        out.write<uint32_t>(kFormatVersion);
        out.write<uint32_t>(static_cast<uint32_t>(names_.size()));
        for (const auto& n : names_)
            writeString(out, n);
        out.writeBytes(payload.buffer());
        return pack(out.buffer());
    }

private:
    const Session& session_;
    std::unordered_map<PluginIndex, uint32_t> ids_;
    std::vector<std::string> names_;
};

class NameResolver {
public:
    // Reads the name table; ok() is false if any plugin is no longer known.
    NameResolver(const Session& s, BinaryReader& r)
    {
        if (r.read<uint32_t>() != kFormatVersion) {
            ok_ = false;
            return;
        }
        uint32_t n = r.read<uint32_t>();
        map_.reserve(n);
        for (uint32_t i = 0; i < n; ++i) {
            auto name = readString(r);
            const PluginInfo* p = s.findPlugin(name);
            if (!p)
                ok_ = false;
            map_.push_back(p ? p->index : kInvalidPlugin);
        }
    }
    bool ok() const { return ok_; }
    PluginIndex plugin(uint32_t id) const { return id == 0xFFFFFFFFu ? kInvalidPlugin : map_.at(id); }
    FormKey key(BinaryReader& r) const
    {
        uint32_t id = r.read<uint32_t>();
        uint32_t obj = r.read<uint32_t>();
        return FormKey{plugin(id), obj};
    }

private:
    std::vector<PluginIndex> map_;
    bool ok_ = true;
};

void writeSub(BinaryWriter& w, NameTable& names, const NormSub& s)
{
    w.write(s.sig.value);
    w.write<uint32_t>(static_cast<uint32_t>(s.bytes.size()));
    w.writeBytes(s.bytes);
    w.write<uint32_t>(static_cast<uint32_t>(s.refs.size()));
    for (const auto& [off, k] : s.refs) {
        w.write(off);
        names.key(w, k);
    }
    w.write<uint32_t>(static_cast<uint32_t>(s.softRefs.size()));
    for (const auto& [off, k] : s.softRefs) {
        w.write(off);
        names.key(w, k);
    }
    w.write<uint8_t>(s.heuristic ? 1 : 0);
}

NormSub readSub(BinaryReader& r, const NameResolver& names)
{
    NormSub s;
    s.sig = Signature(r.read<uint32_t>());
    auto b = r.readBytes(r.read<uint32_t>());
    s.bytes.assign(b.begin(), b.end());
    uint32_t n = r.read<uint32_t>();
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t off = r.read<uint32_t>();
        s.refs.emplace_back(off, names.key(r));
    }
    n = r.read<uint32_t>();
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t off = r.read<uint32_t>();
        s.softRefs.emplace_back(off, names.key(r));
    }
    s.heuristic = r.read<uint8_t>() != 0;
    return s;
}

void writeSubs(BinaryWriter& w, NameTable& names, const std::vector<NormSub>& subs)
{
    w.write<uint32_t>(static_cast<uint32_t>(subs.size()));
    for (const auto& s : subs)
        writeSub(w, names, s);
}

std::vector<NormSub> readSubs(BinaryReader& r, const NameResolver& names)
{
    std::vector<NormSub> out(r.read<uint32_t>());
    for (auto& s : out)
        s = readSub(r, names);
    return out;
}

void writeIssues(BinaryWriter& w, const std::vector<DecodeIssue>& issues)
{
    w.write<uint32_t>(static_cast<uint32_t>(issues.size()));
    for (const auto& is : issues) {
        w.write<uint8_t>(static_cast<uint8_t>(is.severity));
        w.write(is.sub.value);
        writeString(w, is.message);
    }
}

std::vector<DecodeIssue> readIssues(BinaryReader& r)
{
    std::vector<DecodeIssue> out(r.read<uint32_t>());
    for (auto& is : out) {
        is.severity = static_cast<Severity>(r.read<uint8_t>());
        is.sub = Signature(r.read<uint32_t>());
        is.message = readString(r);
    }
    return out;
}

// Rewrites "\x1F origin:object \x1F" tokens between in-memory plugin indices and name ids.
std::string mapTokens(const std::string& text, const std::function<std::string(uint32_t, uint32_t)>& map)
{
    if (text.find('\x1F') == std::string::npos)
        return text;
    std::string out;
    size_t pos = 0;
    while (true) {
        size_t a = text.find('\x1F', pos);
        if (a == std::string::npos) {
            out.append(text, pos, std::string::npos);
            break;
        }
        size_t b = text.find('\x1F', a + 1);
        if (b == std::string::npos) {
            out.append(text, pos, std::string::npos);
            break;
        }
        out.append(text, pos, a - pos);
        unsigned origin = 0, object = 0;
        std::sscanf(text.c_str() + a + 1, "%x:%x", &origin, &object);
        out += map(origin, object);
        pos = b + 1;
    }
    return out;
}

std::string tokenFor(uint32_t origin, uint32_t object)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "\x1F%x:%x\x1F", origin, object);
    return buf;
}

} // namespace

std::string formToken(FormKey key)
{
    return tokenFor(key.origin, key.objectId);
}

std::string renderFormTokens(const Session& session, const std::string& text)
{
    return mapTokens(text, [&](uint32_t origin, uint32_t object) { return session.formLabel(FormKey{origin, object}); });
}

uint64_t sampleFileHash(const std::filesystem::path& path, uint64_t size)
{
    constexpr size_t kChunk = 64 * 1024;
    std::ifstream in(path, std::ios::binary);
    uint64_t h = fnv1a64(&size, sizeof(size));
    if (!in)
        return h;
    std::vector<char> buf(kChunk);
    in.read(buf.data(), static_cast<std::streamsize>(std::min<uint64_t>(kChunk, size)));
    h = fnv1a64(buf.data(), static_cast<size_t>(in.gcount()), h);
    if (size > 2 * kChunk) {
        in.clear();
        in.seekg(static_cast<std::streamoff>(size - kChunk));
        in.read(buf.data(), kChunk);
        h = fnv1a64(buf.data(), static_cast<size_t>(in.gcount()), h);
    }
    return h;
}

uint64_t fullFileHash(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    uint64_t h = 0xcbf29ce484222325ULL;
    std::vector<char> buf(1 << 20);
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        h = fnv1a64(buf.data(), static_cast<size_t>(in.gcount()), h);
    }
    return h;
}

uint64_t chainSignature(const Session& session, const FormGroup& group)
{
    // The definitions decide how a chain decodes, so a layout change invalidates its cached results.
    uint64_t h = session.defs.fingerprint();
    for (const auto& r : session.instances(group)) {
        const auto& p = session.plugins[r.plugin];
        h = fnv1a64(p.key.data(), p.key.size(), h);
        for (const auto& m : p.header.masters) {
            auto lower = toLower(m.name);
            h = fnv1a64(lower.data(), lower.size(), h);
        }
        uint64_t fields[4] = {r.hash, r.dataSize, r.flags & ~RecordFlags::Compressed, r.rawFormId};
        h = fnv1a64(fields, sizeof(fields), h);
        h = fnv1a64("|", 1, h);
    }
    return h;
}

BuildCache::BuildCache(const std::filesystem::path& file)
{
    db_.open(file);
    db_.exec(kCacheSchema);
    auto version = db_.scalar("SELECT CAST(value AS INTEGER) FROM Info WHERE key = 'version'");
    if (!version || *version != kFormatVersion) {
        db_.exec("DELETE FROM PluginIndex; DELETE FROM conflicts; DELETE FROM merges; DELETE FROM edges;");
        db_.prepare("INSERT OR REPLACE INTO Info(key, value) VALUES('version', ?)").bind(1, kFormatVersion).run();
    }
}

std::filesystem::path BuildCache::defaultPathFor(const std::filesystem::path& databasePath)
{
    auto dir = databasePath.has_parent_path() ? databasePath.parent_path() : std::filesystem::current_path();
    return dir / (pathToUtf8(databasePath.stem()) + ".cache.sqlite");
}

void BuildCache::resetCounters()
{
    for (auto* c : {&plugins, &conflicts, &merges, &edges}) {
        c->hits = 0;
        c->misses = 0;
    }
}

std::optional<CachedPluginIndex> BuildCache::loadPluginIndex(const std::string& path, uint64_t size, int64_t modified,
                                                             uint64_t sample)
{
    std::vector<uint8_t> blob;
    {
        std::lock_guard lock(mutex_);
        auto st = db_.prepare("SELECT data FROM PluginIndex WHERE path = ? AND size = ? AND modified = ? AND sample = ?");
        st.bind(1, path).bind(2, size).bind(3, static_cast<int64_t>(modified)).bind(4, sample);
        if (!st.step()) {
            plugins.misses++;
            return std::nullopt;
        }
        blob = st.columnBlob(0);
    }
    auto raw = unpack(blob);
    if (!raw) {
        plugins.misses++;
        return std::nullopt;
    }
    try {
        BinaryReader r(*raw);
        CachedPluginIndex out;
        out.fileHash = r.read<uint64_t>();
        out.records.resize(r.read<uint32_t>());
        for (auto& c : out.records) {
            c.sig = Signature(r.read<uint32_t>());
            c.flags = r.read<uint32_t>();
            c.dataSize = r.read<uint32_t>();
            c.rawFormId = r.read<uint32_t>();
            c.offset = r.read<uint64_t>();
            c.hash = r.read<uint64_t>();
            c.editorId = r.readWString();
        }
        plugins.hits++;
        return out;
    } catch (const std::exception&) {
        plugins.misses++;
        return std::nullopt;
    }
}

void BuildCache::storePluginIndex(const std::string& path, uint64_t size, int64_t modified, uint64_t sample,
                                  const CachedPluginIndex& index)
{
    BinaryWriter w;
    w.write(index.fileHash);
    w.write<uint32_t>(static_cast<uint32_t>(index.records.size()));
    for (const auto& c : index.records) {
        w.write(c.sig.value);
        w.write(c.flags);
        w.write(c.dataSize);
        w.write(c.rawFormId);
        w.write(c.offset);
        w.write(c.hash);
        w.writeWString(c.editorId.size() > 0xFFFF ? c.editorId.substr(0, 0xFFFF) : c.editorId);
    }
    auto blob = pack(w.buffer());
    std::lock_guard lock(mutex_);
    db_.prepare("INSERT OR REPLACE INTO PluginIndex(path, size, modified, sample, data) VALUES(?,?,?,?,?)")
        .bind(1, path)
        .bind(2, size)
        .bind(3, static_cast<int64_t>(modified))
        .bind(4, sample)
        .bindBlob(5, blob.data(), blob.size())
        .run();
}

std::optional<std::vector<uint8_t>> BuildCache::get(const char* table, uint64_t key)
{
    std::lock_guard lock(mutex_);
    auto st = db_.prepare(std::string("SELECT data FROM ") + table + " WHERE key = ?");
    st.bind(1, static_cast<int64_t>(key));
    if (!st.step())
        return std::nullopt;
    return st.columnBlob(0);
}

void BuildCache::put(const char* table, uint64_t key, std::span<const uint8_t> blob)
{
    std::lock_guard lock(mutex_);
    db_.prepare(std::string("INSERT OR REPLACE INTO ") + table + "(key, data) VALUES(?, ?)")
        .bind(1, static_cast<int64_t>(key))
        .bindBlob(2, blob.data(), blob.size())
        .run();
}

void BuildCache::begin()
{
    std::lock_guard lock(mutex_);
    db_.begin();
}

void BuildCache::commit()
{
    std::lock_guard lock(mutex_);
    db_.commit();
}

void BuildCache::clear()
{
    std::lock_guard lock(mutex_);
    db_.exec("DELETE FROM PluginIndex; DELETE FROM conflicts; DELETE FROM merges; DELETE FROM edges;");
}

// ---------------------------------------------------------------------------

std::vector<uint8_t> serializeConflict(const Session& session, const ConflictEntry& e)
{
    NameTable names(session);
    BinaryWriter w;
    auto tokens = [&](const std::string& text) {
        return mapTokens(text, [&](uint32_t origin, uint32_t object) { return tokenFor(names.id(origin), object); });
    };
    w.write<uint8_t>(static_cast<uint8_t>(e.type));
    w.write<uint8_t>(static_cast<uint8_t>(e.severity));
    w.write<uint8_t>(e.fieldsAnalysed ? 1 : 0);
    w.write<uint8_t>(e.winnerLosesChanges ? 1 : 0);
    writeString(w, e.summary);
    w.write<uint32_t>(static_cast<uint32_t>(e.itmPlugins.size()));
    for (auto p : e.itmPlugins)
        w.write(names.id(p));
    w.write<uint32_t>(static_cast<uint32_t>(e.fields.size()));
    for (const auto& f : e.fields) {
        writeString(w, f.key);
        writeString(w, f.label);
        w.write<uint8_t>(f.hard ? 1 : 0);
        w.write<uint8_t>(f.list ? 1 : 0);
        writeString(w, tokens(f.result));
        w.write<uint32_t>(static_cast<uint32_t>(f.cells.size()));
        for (const auto& c : f.cells) {
            w.write(names.id(c.plugin));
            writeString(w, tokens(c.value));
            w.write<uint8_t>(static_cast<uint8_t>(c.status));
        }
    }
    return names.finish(w);
}

std::optional<ConflictEntry> deserializeConflict(const Session& session, std::span<const uint8_t> blob)
{
    auto raw = unpack(blob);
    if (!raw)
        return std::nullopt;
    try {
        BinaryReader r(*raw);
        NameResolver names(session, r);
        if (!names.ok())
            return std::nullopt;
        auto tokens = [&](const std::string& text) {
            return mapTokens(text, [&](uint32_t id, uint32_t object) { return tokenFor(names.plugin(id), object); });
        };
        ConflictEntry e;
        e.type = static_cast<ConflictType>(r.read<uint8_t>());
        e.severity = static_cast<ConflictSeverity>(r.read<uint8_t>());
        e.fieldsAnalysed = r.read<uint8_t>() != 0;
        e.winnerLosesChanges = r.read<uint8_t>() != 0;
        e.summary = readString(r);
        uint32_t n = r.read<uint32_t>();
        for (uint32_t i = 0; i < n; ++i)
            e.itmPlugins.push_back(names.plugin(r.read<uint32_t>()));
        e.fields.resize(r.read<uint32_t>());
        for (auto& f : e.fields) {
            f.key = readString(r);
            f.label = readString(r);
            f.hard = r.read<uint8_t>() != 0;
            f.list = r.read<uint8_t>() != 0;
            f.result = tokens(readString(r));
            f.cells.resize(r.read<uint32_t>());
            for (auto& c : f.cells) {
                c.plugin = names.plugin(r.read<uint32_t>());
                c.value = tokens(readString(r));
                c.status = static_cast<CellStatus>(r.read<uint8_t>());
            }
        }
        return e;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::vector<uint8_t> serializeMerge(const Session& session, const CachedMerge& m)
{
    NameTable names(session);
    BinaryWriter w;
    const auto& fs = m.fields;
    names.key(w, fs.key);
    w.write(fs.sig.value);
    w.write(names.id(fs.plugin));
    w.write(fs.formVersion);
    w.write<uint32_t>(static_cast<uint32_t>(fs.fields.size()));
    for (const auto& f : fs.fields) {
        writeString(w, f.key);
        writeString(w, f.label);
        w.write<uint8_t>(static_cast<uint8_t>(f.shape));
        w.write(f.sig.value);
        w.write<uint8_t>(static_cast<uint8_t>(f.merge));
        w.write(f.countSig.value);
        w.write(f.countWidth);
        w.write(f.stride);
        w.write<uint8_t>(f.hadCount ? 1 : 0);
        writeSubs(w, names, f.subs);
        w.write<uint32_t>(static_cast<uint32_t>(f.items.size()));
        for (const auto& item : f.items)
            writeSubs(w, names, item.subs);
    }
    w.write<uint8_t>(m.merged ? 1 : 0);
    w.write<uint32_t>(static_cast<uint32_t>(m.contributors.size()));
    for (auto c : m.contributors)
        w.write(c);
    w.write<uint32_t>(static_cast<uint32_t>(m.equalsInstance.size()));
    for (auto e : m.equalsInstance)
        w.write(e);
    w.write<uint32_t>(static_cast<uint32_t>(m.issues.size()));
    for (const auto& is : m.issues)
        writeIssues(w, is);
    return names.finish(w);
}

std::optional<CachedMerge> deserializeMerge(const Session& session, std::span<const uint8_t> blob)
{
    auto raw = unpack(blob);
    if (!raw)
        return std::nullopt;
    try {
        BinaryReader r(*raw);
        NameResolver names(session, r);
        if (!names.ok())
            return std::nullopt;
        CachedMerge m;
        auto& fs = m.fields;
        fs.key = names.key(r);
        fs.sig = Signature(r.read<uint32_t>());
        fs.plugin = names.plugin(r.read<uint32_t>());
        fs.formVersion = r.read<uint16_t>();
        fs.fields.resize(r.read<uint32_t>());
        for (auto& f : fs.fields) {
            f.key = readString(r);
            f.label = readString(r);
            f.shape = static_cast<FieldShape>(r.read<uint8_t>());
            f.sig = Signature(r.read<uint32_t>());
            f.merge = static_cast<MergeMode>(r.read<uint8_t>());
            f.countSig = Signature(r.read<uint32_t>());
            f.countWidth = r.read<uint8_t>();
            f.stride = r.read<uint16_t>();
            f.hadCount = r.read<uint8_t>() != 0;
            f.subs = readSubs(r, names);
            f.items.resize(r.read<uint32_t>());
            for (auto& item : f.items)
                item.subs = readSubs(r, names);
        }
        m.merged = r.read<uint8_t>() != 0;
        m.contributors.resize(r.read<uint32_t>());
        for (auto& c : m.contributors)
            c = r.read<uint32_t>();
        m.equalsInstance.resize(r.read<uint32_t>());
        for (auto& e : m.equalsInstance)
            e = r.read<uint8_t>();
        m.issues.resize(r.read<uint32_t>());
        for (auto& is : m.issues)
            is = readIssues(r);
        return m;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::vector<uint8_t> serializeEdges(const Session& session, const std::vector<std::pair<FormKey, Signature>>& refs)
{
    NameTable names(session);
    BinaryWriter w;
    w.write<uint32_t>(static_cast<uint32_t>(refs.size()));
    for (const auto& [k, via] : refs) {
        names.key(w, k);
        w.write(via.value);
    }
    return names.finish(w);
}

std::optional<std::vector<std::pair<FormKey, Signature>>> deserializeEdges(const Session& session,
                                                                            std::span<const uint8_t> blob)
{
    auto raw = unpack(blob);
    if (!raw)
        return std::nullopt;
    try {
        BinaryReader r(*raw);
        NameResolver names(session, r);
        if (!names.ok())
            return std::nullopt;
        std::vector<std::pair<FormKey, Signature>> out(r.read<uint32_t>());
        for (auto& [k, via] : out) {
            k = names.key(r);
            via = Signature(r.read<uint32_t>());
        }
        return out;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

} // namespace fo4mx
