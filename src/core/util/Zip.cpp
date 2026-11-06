#include "util/Zip.hpp"

#include "util/Binary.hpp"
#include "util/Text.hpp"

#include <algorithm>
#include <ctime>
#include <fstream>
#include <stdexcept>
#include <vector>

#include <zlib.h>

namespace fo4mx {

namespace {

constexpr uint32_t k32 = 0xFFFFFFFFu;

bool storedFormat(const std::string& lowerPath)
{
    for (const char* ext : {".fuz", ".xwm", ".wav", ".dds", ".ba2", ".zip", ".7z", ".png", ".jpg", ".bk2", ".swf"})
        if (iendsWith(lowerPath, ext))
            return true;
    return false;
}

std::vector<uint8_t> rawDeflate(const std::vector<uint8_t>& in)
{
    z_stream zs{};
    if (deflateInit2(&zs, 6, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        throw std::runtime_error("zip: cannot initialise deflate");
    std::vector<uint8_t> out(deflateBound(&zs, static_cast<uLong>(in.size())));
    zs.next_in = const_cast<Bytef*>(in.data());
    zs.avail_in = static_cast<uInt>(in.size());
    zs.next_out = out.data();
    zs.avail_out = static_cast<uInt>(out.size());
    const int rc = deflate(&zs, Z_FINISH);
    out.resize(zs.total_out);
    deflateEnd(&zs);
    if (rc != Z_STREAM_END)
        throw std::runtime_error("zip: deflate failed");
    return out;
}

struct Entry {
    std::string name;
    uint32_t crc = 0;
    uint64_t packed = 0, size = 0, offset = 0;
    uint16_t method = 0;
};

void put16(std::vector<uint8_t>& b, uint16_t v) { b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8)); }
void put32(std::vector<uint8_t>& b, uint32_t v) { put16(b, uint16_t(v)); put16(b, uint16_t(v >> 16)); }
void put64(std::vector<uint8_t>& b, uint64_t v) { put32(b, uint32_t(v)); put32(b, uint32_t(v >> 32)); }

} // namespace

ZipStats writeZip(const std::filesystem::path& zip, const std::filesystem::path& folder, const std::string& prefix)
{
    return writeZip(zip, folder, prefix, {});
}

ZipStats writeZip(const std::filesystem::path& zip, const std::filesystem::path& folder, const std::string& prefix,
                  const std::function<bool(const std::filesystem::path&)>& skip)
{
    namespace fs = std::filesystem;
    std::vector<fs::path> files;
    for (auto it = fs::recursive_directory_iterator(folder); it != fs::recursive_directory_iterator(); ++it)
        if (it->is_regular_file() && it->path().extension() != ".tmp" &&
            !(skip && skip(it->path().lexically_relative(folder))))
            files.push_back(it->path());
    std::sort(files.begin(), files.end());

    // DOS date and time of now.
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    const uint16_t dosTime = uint16_t((tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2));
    const uint16_t dosDate = uint16_t(((tm.tm_year - 80) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday);

    auto tmp = zip;
    tmp += ".tmp";
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out)
        throw std::runtime_error("cannot write " + pathToUtf8(zip));
    uint64_t pos = 0;
    auto write = [&](const std::vector<uint8_t>& b) {
        out.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
        pos += b.size();
    };
    std::vector<Entry> entries;
    ZipStats stats;
    for (const auto& f : files) {
        std::ifstream in(f, std::ios::binary);
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        Entry e;
        e.name = prefix + pathToUtf8(f.lexically_relative(folder).generic_u8string());
        e.size = data.size();
        e.crc = static_cast<uint32_t>(crc32(0L, data.data(), static_cast<uInt>(data.size())));
        std::vector<uint8_t> payload;
        if (!storedFormat(toLower(e.name)) && !data.empty()) {
            payload = rawDeflate(data);
            e.method = 8;
            if (payload.size() >= data.size()) { // incompressible: store
                payload = std::move(data);
                e.method = 0;
            }
        } else {
            payload = std::move(data);
        }
        e.packed = payload.size();
        e.offset = pos;
        const bool big = e.size >= k32 || e.packed >= k32;
        std::vector<uint8_t> h;
        put32(h, 0x04034b50);
        put16(h, big ? 45 : 20);
        put16(h, 0x0800); // UTF-8 names
        put16(h, e.method);
        put16(h, dosTime);
        put16(h, dosDate);
        put32(h, e.crc);
        put32(h, big ? k32 : uint32_t(e.packed));
        put32(h, big ? k32 : uint32_t(e.size));
        put16(h, uint16_t(e.name.size()));
        put16(h, big ? 20 : 0);
        h.insert(h.end(), e.name.begin(), e.name.end());
        if (big) {
            put16(h, 0x0001);
            put16(h, 16);
            put64(h, e.size);
            put64(h, e.packed);
        }
        write(h);
        write(payload);
        stats.files++;
        stats.bytes += e.size;
        entries.push_back(std::move(e));
    }

    const uint64_t cdStart = pos;
    for (const auto& e : entries) {
        const bool bigSize = e.size >= k32 || e.packed >= k32;
        const bool bigOffset = e.offset >= k32;
        std::vector<uint8_t> extra;
        if (bigSize || bigOffset) {
            std::vector<uint8_t> body;
            if (bigSize) {
                put64(body, e.size);
                put64(body, e.packed);
            }
            if (bigOffset)
                put64(body, e.offset);
            put16(extra, 0x0001);
            put16(extra, uint16_t(body.size()));
            extra.insert(extra.end(), body.begin(), body.end());
        }
        std::vector<uint8_t> c;
        put32(c, 0x02014b50);
        put16(c, 45);
        put16(c, (bigSize || bigOffset) ? 45 : 20);
        put16(c, 0x0800);
        put16(c, e.method);
        put16(c, dosTime);
        put16(c, dosDate);
        put32(c, e.crc);
        put32(c, bigSize ? k32 : uint32_t(e.packed));
        put32(c, bigSize ? k32 : uint32_t(e.size));
        put16(c, uint16_t(e.name.size()));
        put16(c, uint16_t(extra.size()));
        put16(c, 0); // comment
        put16(c, 0); // disk
        put16(c, 0); // internal attributes
        put32(c, 0); // external attributes
        put32(c, bigOffset ? k32 : uint32_t(e.offset));
        c.insert(c.end(), e.name.begin(), e.name.end());
        c.insert(c.end(), extra.begin(), extra.end());
        write(c);
    }
    const uint64_t cdSize = pos - cdStart;
    const bool zip64 = entries.size() >= 0xFFFF || cdStart >= k32 || cdSize >= k32;
    if (zip64) {
        const uint64_t eocd64 = pos;
        std::vector<uint8_t> z;
        put32(z, 0x06064b50);
        put64(z, 44);
        put16(z, 45);
        put16(z, 45);
        put32(z, 0);
        put32(z, 0);
        put64(z, entries.size());
        put64(z, entries.size());
        put64(z, cdSize);
        put64(z, cdStart);
        put32(z, 0x07064b50); // locator
        put32(z, 0);
        put64(z, eocd64);
        put32(z, 1);
        write(z);
    }
    std::vector<uint8_t> end;
    put32(end, 0x06054b50);
    put16(end, 0);
    put16(end, 0);
    put16(end, zip64 ? 0xFFFF : uint16_t(entries.size()));
    put16(end, zip64 ? 0xFFFF : uint16_t(entries.size()));
    put32(end, zip64 ? k32 : uint32_t(cdSize));
    put32(end, zip64 ? k32 : uint32_t(cdStart));
    put16(end, 0);
    write(end);
    out.close();
    if (!out)
        throw std::runtime_error("cannot write " + pathToUtf8(zip));
    std::error_code ec;
    fs::remove(zip, ec);
    fs::rename(tmp, zip);
    return stats;
}

} // namespace fo4mx
