#include "esp/FieldDecoder.hpp"

#include "util/Binary.hpp"

namespace fo4mx {

const char* severityName(Severity s)
{
    switch (s) {
    case Severity::Info: return "Info";
    case Severity::Warning: return "Warning";
    case Severity::Error: return "Error";
    }
    return "Info";
}

bool DecodedLayout::slotCovers(uint32_t sub, uint32_t offset) const
{
    for (const auto& s : formIdSlots)
        if (s.sub == sub && offset < s.offset + 4 && s.offset < offset + 4)
            return true;
    return false;
}

namespace {

constexpr size_t kObjectModPropertySize = 24;
constexpr size_t kIncludeSize = 7;

bool looksLikeText(const std::vector<uint8_t>& d)
{
    if (d.empty() || d.back() != 0)
        return false;
    for (size_t i = 0; i + 1 < d.size(); ++i)
        if (d[i] < 0x20 && d[i] != '\t')
            return false;
    return true;
}

class Decoder {
public:
    Decoder(const RecordDefinitions& defs, Signature record, const std::vector<Subrecord>& subs, DecodedLayout& out)
        : defs_(defs), record_(record), subs_(subs), out_(out)
    {
    }

    void run()
    {
        for (uint32_t i = 0; i < subs_.size(); ++i) {
            const auto& sr = subs_[i];
            const FieldDef& def = defs_.field(record_, sr.signature);
            try {
                decode(i, sr, def);
            } catch (const FormatError& e) {
                issue(Severity::Warning, sr.signature, std::string("could not decode: ") + e.what());
                heuristic(i, 0, static_cast<uint32_t>(sr.data.size()));
            }
        }
    }

private:
    void slot(uint32_t sub, size_t offset) { out_.formIdSlots.push_back({sub, static_cast<uint32_t>(offset)}); }
    void heuristic(uint32_t sub, uint32_t begin, uint32_t end)
    {
        if (end >= begin + 4)
            out_.heuristic.push_back({sub, begin, end});
    }
    void issue(Severity s, Signature sub, std::string msg) { out_.issues.push_back({s, sub, std::move(msg)}); }

    void decode(uint32_t i, const Subrecord& sr, const FieldDef& def)
    {
        const size_t size = sr.data.size();
        switch (def.kind) {
        case FieldKind::Unknown:
            heuristic(i, 0, static_cast<uint32_t>(size));
            break;
        case FieldKind::NoRefs:
        case FieldKind::ZString:
            break;
        case FieldKind::LString:
        case FieldKind::DLString:
        case FieldKind::ILString:
            out_.localized.push_back({i, def.stringFile()});
            break;
        case FieldKind::FormId:
            if (size == 4)
                slot(i, 0);
            else if (size != 0 && !looksLikeText(sr.data)) {
                issue(Severity::Info, sr.signature, "expected a 4-byte FormID, found " + std::to_string(size) + " bytes");
                heuristic(i, 0, static_cast<uint32_t>(size));
            }
            break;
        case FieldKind::FormIdArray:
            if (size % 4 != 0) {
                issue(Severity::Warning, sr.signature, "FormID array size is not a multiple of 4");
                heuristic(i, 0, static_cast<uint32_t>(size));
                break;
            }
            for (size_t o = 0; o < size; o += 4)
                slot(i, o);
            break;
        case FieldKind::Struct:
            if (def.requiredSize && size != def.requiredSize) {
                issue(Severity::Info, sr.signature,
                      "unexpected size " + std::to_string(size) + " (expected " + std::to_string(def.requiredSize) + ")");
                heuristic(i, 0, static_cast<uint32_t>(size));
                break;
            }
            for (uint16_t o : def.offsets)
                if (o + 4u <= size)
                    slot(i, o);
            if (def.partial)
                heuristic(i, 0, static_cast<uint32_t>(size));
            break;
        case FieldKind::StructArray:
            if (def.stride == 0 || size % def.stride != 0) {
                issue(Severity::Warning, sr.signature, "array size is not a multiple of the element size");
                heuristic(i, 0, static_cast<uint32_t>(size));
                break;
            }
            for (size_t e = 0; e < size; e += def.stride)
                for (uint16_t o : def.offsets)
                    slot(i, e + o);
            break;
        case FieldKind::Vmad:
            decodeVmad(i, sr);
            break;
        case FieldKind::Condition:
            decodeCondition(i, sr);
            break;
        case FieldKind::ObjectTemplate:
            decodeObjectTemplate(i, sr);
            break;
        case FieldKind::OmodData:
            decodeOmodData(i, sr);
            break;
        case FieldKind::PerkData:
            decodePerkData(i, sr);
            break;
        case FieldKind::PerkEntryData:
            decodePerkEntryData(i, sr);
            break;
        case FieldKind::VendorLocation:
            decodeVendorLocation(i, sr);
            break;
        }

        // Context tracking for PERK entry points.
        if (sr.signature == Signature("PRKE") && size >= 1) {
            inPerkEffect_ = true;
            perkEffectType_ = sr.data[0];
        } else if (sr.signature == Signature("EPFT") && size >= 1) {
            epft_ = sr.data[0];
        } else if (sr.signature == Signature("PRKF")) {
            inPerkEffect_ = false;
        }
    }

    // --- VMAD -------------------------------------------------------------

    void vmadObject(BinaryReader& r, uint32_t i, int16_t objFormat)
    {
        size_t base = r.position();
        if (objFormat == 1) {
            r.skip(4); // unused u16 + alias i16
            slot(i, r.position());
            r.skip(4);
        } else {
            slot(i, base);
            r.skip(8); // formid, alias i16, unused u16
        }
    }

    void vmadValue(BinaryReader& r, uint32_t i, uint8_t type, int16_t version, int16_t objFormat, int depth)
    {
        if (depth > 8)
            throw FormatError("VMAD nesting too deep");
        switch (type) {
        case 0: break;
        case 1: vmadObject(r, i, objFormat); break;
        case 2: r.readWString(); break;
        case 3: r.skip(4); break;
        case 4: r.skip(4); break;
        case 5: r.skip(1); break;
        case 7: { // struct
            uint32_t members = r.read<uint32_t>();
            for (uint32_t m = 0; m < members; ++m)
                vmadProperty(r, i, version, objFormat, depth + 1);
            break;
        }
        case 11: {
            uint32_t n = r.read<uint32_t>();
            for (uint32_t k = 0; k < n; ++k)
                vmadObject(r, i, objFormat);
            break;
        }
        case 12: {
            uint32_t n = r.read<uint32_t>();
            for (uint32_t k = 0; k < n; ++k)
                r.readWString();
            break;
        }
        case 13:
        case 14: r.skip(static_cast<size_t>(r.read<uint32_t>()) * 4); break;
        case 15: r.skip(r.read<uint32_t>()); break;
        case 17: {
            uint32_t n = r.read<uint32_t>();
            for (uint32_t k = 0; k < n; ++k) {
                uint32_t members = r.read<uint32_t>();
                for (uint32_t m = 0; m < members; ++m)
                    vmadProperty(r, i, version, objFormat, depth + 1);
            }
            break;
        }
        default:
            throw FormatError("unsupported script property type " + std::to_string(type));
        }
    }

    void vmadProperty(BinaryReader& r, uint32_t i, int16_t version, int16_t objFormat, int depth)
    {
        r.readWString(); // name
        uint8_t type = r.read<uint8_t>();
        if (version >= 4)
            r.skip(1); // status
        vmadValue(r, i, type, version, objFormat, depth);
    }

    void vmadScript(BinaryReader& r, uint32_t i, int16_t version, int16_t objFormat)
    {
        r.readWString(); // script name
        if (version >= 4)
            r.skip(1); // status
        uint16_t props = r.read<uint16_t>();
        for (uint16_t p = 0; p < props; ++p)
            vmadProperty(r, i, version, objFormat, 0);
    }

    void decodeVmad(uint32_t i, const Subrecord& sr)
    {
        size_t slotsBefore = out_.formIdSlots.size();
        try {
            BinaryReader r(sr.data);
            int16_t version = r.read<int16_t>();
            int16_t objFormat = r.read<int16_t>();
            uint16_t scripts = r.read<uint16_t>();
            for (uint16_t s = 0; s < scripts; ++s)
                vmadScript(r, i, version, objFormat);
            if (!r.atEnd()) {
                // Script fragments (PERK and other fragment-bearing records).
                // Their layout is not decoded, so scan it heuristically.
                if (record_ != Signature("PERK"))
                    issue(Severity::Info, sr.signature, "unexpected data after script list");
                heuristic(i, static_cast<uint32_t>(r.position()), static_cast<uint32_t>(sr.data.size()));
            }
        } catch (const FormatError& e) {
            out_.formIdSlots.resize(slotsBefore);
            issue(Severity::Warning, sr.signature, std::string("script data not fully decoded: ") + e.what());
            heuristic(i, 0, static_cast<uint32_t>(sr.data.size()));
        }
    }

    // --- CTDA -------------------------------------------------------------

    void decodeCondition(uint32_t i, const Subrecord& sr)
    {
        if (sr.data.size() != 32) {
            issue(Severity::Info, sr.signature, "condition has unexpected size " + std::to_string(sr.data.size()));
            heuristic(i, 0, static_cast<uint32_t>(sr.data.size()));
            return;
        }
        const uint8_t* d = sr.data.data();
        uint8_t flags = d[0];
        if (flags & 0x04)
            slot(i, 4); // comparison value is a global
        uint16_t fn = loadLE<uint16_t>(d + 8);
        if (defs_.conditionFunctionKnown(fn)) {
            if (defs_.conditionParamIsForm(fn, 1))
                slot(i, 12);
            if (defs_.conditionParamIsForm(fn, 2))
                slot(i, 16);
        } else {
            heuristic(i, 12, 20);
        }
        uint32_t runOn = loadLE<uint32_t>(d + 20);
        if (runOn == 2)
            slot(i, 24);
    }

    // --- Object modification properties shared by OBTS and OMOD DATA ------

    void objectModProperties(BinaryReader& r, uint32_t i, uint32_t count)
    {
        for (uint32_t p = 0; p < count; ++p) {
            size_t base = r.position();
            uint8_t valueType = r.read<uint8_t>();
            r.skip(kObjectModPropertySize - 1);
            if (valueType == 4 || valueType == 6)
                slot(i, base + 12);
        }
    }

    void includes(BinaryReader& r, uint32_t i, uint32_t count)
    {
        for (uint32_t k = 0; k < count; ++k) {
            slot(i, r.position());
            r.skip(kIncludeSize);
        }
    }

    void decodeObjectTemplate(uint32_t i, const Subrecord& sr)
    {
        size_t slotsBefore = out_.formIdSlots.size();
        try {
            BinaryReader r(sr.data);
            uint32_t includeCount = r.read<uint32_t>();
            uint32_t propertyCount = r.read<uint32_t>();
            r.skip(7); // level min, unused, level max, unused, addon index (i16), default
            uint8_t keywordCount = r.read<uint8_t>();
            size_t expected = 16 + keywordCount * 4ull + 2 + includeCount * kIncludeSize +
                              propertyCount * kObjectModPropertySize;
            if (expected != sr.data.size())
                throw FormatError("size " + std::to_string(sr.data.size()) + " does not match decoded layout (" +
                                  std::to_string(expected) + ")");
            for (uint8_t k = 0; k < keywordCount; ++k) {
                slot(i, r.position());
                r.skip(4);
            }
            r.skip(2); // min level for ranks, alt levels per tier
            includes(r, i, includeCount);
            objectModProperties(r, i, propertyCount);
        } catch (const FormatError& e) {
            out_.formIdSlots.resize(slotsBefore);
            issue(Severity::Warning, sr.signature, std::string("object template not decoded: ") + e.what());
            heuristic(i, 0, static_cast<uint32_t>(sr.data.size()));
        }
    }

    void decodeOmodData(uint32_t i, const Subrecord& sr)
    {
        size_t slotsBefore = out_.formIdSlots.size();
        try {
            BinaryReader r(sr.data);
            uint32_t includeCount = r.read<uint32_t>();
            uint32_t propertyCount = r.read<uint32_t>();
            r.skip(2); // two unknown bools
            r.skip(4); // form type
            r.skip(2); // max rank, level tier scaled offset
            slot(i, r.position()); // attach point keyword
            r.skip(4);
            uint32_t slotsCount = r.read<uint32_t>();
            if (slotsCount > sr.data.size() / 4)
                throw FormatError("implausible attach parent slot count");
            for (uint32_t k = 0; k < slotsCount; ++k) {
                slot(i, r.position());
                r.skip(4);
            }
            uint32_t itemCount = r.read<uint32_t>();
            if (itemCount > sr.data.size() / 8)
                throw FormatError("implausible item count");
            r.skip(static_cast<size_t>(itemCount) * 8);
            includes(r, i, includeCount);
            objectModProperties(r, i, propertyCount);
            if (!r.atEnd())
                throw FormatError(std::to_string(r.remaining()) + " trailing bytes");
        } catch (const FormatError& e) {
            out_.formIdSlots.resize(slotsBefore);
            issue(Severity::Warning, sr.signature, std::string("OMOD data not decoded: ") + e.what());
            heuristic(i, 0, static_cast<uint32_t>(sr.data.size()));
        }
    }

    // --- PERK ---------------------------------------------------------------

    void decodePerkData(uint32_t i, const Subrecord& sr)
    {
        if (!inPerkEffect_)
            return; // perk header: trait, level, ranks, playable, hidden
        switch (perkEffectType_) {
        case 0: // quest + stage
        case 1: // ability spell
            if (sr.data.size() >= 4)
                slot(i, 0);
            break;
        case 2: // entry point: entry point, function, condition tab count
            break;
        default:
            heuristic(i, 0, static_cast<uint32_t>(sr.data.size()));
        }
    }

    void decodePerkEntryData(uint32_t i, const Subrecord& sr)
    {
        switch (epft_) {
        case 1: // float
            break;
        case 3: // leveled item
        case 4: // spell
            if (sr.data.size() == 4)
                slot(i, 0);
            else
                heuristic(i, 0, static_cast<uint32_t>(sr.data.size()));
            break;
        default:
            heuristic(i, 0, static_cast<uint32_t>(sr.data.size()));
        }
    }

    // --- FACT PLVD -----------------------------------------------------------

    void decodeVendorLocation(uint32_t i, const Subrecord& sr)
    {
        if (sr.data.size() < 8) {
            heuristic(i, 0, static_cast<uint32_t>(sr.data.size()));
            return;
        }
        int32_t type = loadLE<int32_t>(sr.data.data());
        switch (type) {
        case 0: // near reference
        case 1: // in cell
        case 4: // object id
            slot(i, 4);
            break;
        case 2:
        case 3:
        case 5:
        case 7:
        case 12:
            break;
        default:
            heuristic(i, 4, static_cast<uint32_t>(sr.data.size()));
        }
    }

    const RecordDefinitions& defs_;
    Signature record_;
    const std::vector<Subrecord>& subs_;
    DecodedLayout& out_;
    bool inPerkEffect_ = false;
    uint8_t perkEffectType_ = 0;
    uint8_t epft_ = 0;
};

} // namespace

DecodedLayout decodeLayout(const RecordDefinitions& defs, Signature record, const std::vector<Subrecord>& subs)
{
    DecodedLayout out;
    Decoder(defs, record, subs, out).run();
    return out;
}

} // namespace fo4mx
