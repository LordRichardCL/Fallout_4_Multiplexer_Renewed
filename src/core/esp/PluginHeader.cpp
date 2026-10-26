#include "esp/PluginHeader.hpp"

namespace fo4mx {

PluginHeader PluginHeader::fromRecord(const Record& rec)
{
    if (rec.signature() != sig::TES4)
        throw FormatError("plugin does not start with a TES4 record");

    PluginHeader h;
    h.record = rec.header;
    for (size_t i = 0; i < rec.subrecords.size(); ++i) {
        const auto& sr = rec.subrecords[i];
        if (sr.signature == sig::HEDR) {
            if (sr.data.size() < 12)
                throw FormatError("HEDR subrecord too small");
            h.version = loadLE<float>(sr.data.data());
            h.numRecords = loadLE<int32_t>(sr.data.data() + 4);
            h.nextObjectId = loadLE<uint32_t>(sr.data.data() + 8);
        } else if (sr.signature == sig::CNAM) {
            h.author = sr.asZString();
        } else if (sr.signature == sig::SNAM) {
            h.description = sr.asZString();
        } else if (sr.signature == sig::MAST) {
            MasterEntry m;
            m.name = sr.asZString();
            if (i + 1 < rec.subrecords.size() && rec.subrecords[i + 1].signature == sig::DATA) {
                if (auto v = rec.subrecords[i + 1].as<uint64_t>())
                    m.size = *v;
                ++i;
            }
            h.masters.push_back(std::move(m));
        } else {
            h.extra.push_back(sr);
        }
    }
    return h;
}

Record PluginHeader::toRecord() const
{
    Record rec;
    rec.header = record;
    rec.header.signature = sig::TES4;
    rec.header.formId = 0;

    BinaryWriter hedr;
    hedr.write(version);
    hedr.write(numRecords);
    hedr.write(nextObjectId);
    rec.subrecords.emplace_back(sig::HEDR, hedr.take());

    if (!author.empty())
        rec.subrecords.push_back(Subrecord::fromZString(sig::CNAM, author));
    if (!description.empty())
        rec.subrecords.push_back(Subrecord::fromZString(sig::SNAM, description));
    for (const auto& m : masters) {
        rec.subrecords.push_back(Subrecord::fromZString(sig::MAST, m.name));
        rec.subrecords.push_back(Subrecord::fromValue<uint64_t>(sig::DATA, m.size));
    }
    for (const auto& sr : extra)
        rec.subrecords.push_back(sr);
    return rec;
}

} // namespace fo4mx
