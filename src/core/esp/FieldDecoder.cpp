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
    void probable(uint32_t sub, size_t offset) { out_.probableSlots.push_back({sub, static_cast<uint32_t>(offset)}); }
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
            if (def.partial) {
                // Scan everything except the ranges known to hold numbers and flags.
                uint32_t from = 0;
                for (auto [b, e] : def.plain) {
                    if (b > from)
                        heuristic(i, from, std::min<uint32_t>(b, static_cast<uint32_t>(size)));
                    from = std::max<uint32_t>(from, e);
                }
                if (from < size)
                    heuristic(i, from, static_cast<uint32_t>(size));
            }
            break;
        case FieldKind::StructArray: {
            size_t start = def.arrayStart;
            if (def.skipCountAt != 0xFFFF && def.skipCountAt + 4u <= size)
                start += static_cast<size_t>(loadLE<uint32_t>(sr.data.data() + def.skipCountAt)) * def.skipStride;
            if (def.stride == 0 || size < start || (size - start) % def.stride != 0) {
                issue(Severity::Warning, sr.signature, "array size is not a multiple of the element size");
                heuristic(i, 0, static_cast<uint32_t>(size));
                break;
            }
            for (size_t e = start; e < size; e += def.stride)
                for (uint16_t o : def.offsets)
                    slot(i, e + o);
            break;
        }
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
        case FieldKind::GameSetting: {
            // The setting's type is the first letter of its Editor ID: s = string (localised in a
            // localised plugin), f, i, u, b = numbers. No FormIDs either way.
            for (const auto& other : subs_)
                if (other.signature == Signature("EDID")) {
                    if (!other.data.empty() && (other.data[0] == 's' || other.data[0] == 'S'))
                        out_.localized.push_back({i, StringFileKind::Strings});
                    break;
                }
            break;
        }
        case FieldKind::Navmesh:
        case FieldKind::PrecutMap:
        case FieldKind::NavmeshInfo:
        case FieldKind::PrecomputedPathing:
            decodeNavmesh(i, sr, def.kind);
            break;
        case FieldKind::PackageValue: {
            // Before the first data input (ANAM) CNAM is the combat style; after it, a value.
            bool input = false;
            for (uint32_t k = 0; k < i; ++k)
                input |= subs_[k].signature == Signature("ANAM");
            if (!input) {
                if (size == 4)
                    slot(i, 0);
                else
                    heuristic(i, 0, static_cast<uint32_t>(size));
            }
            break;
        }
        case FieldKind::PackageLocation:
        case FieldKind::PackageTarget: {
            // xEdit wbPLDT / wbTargetData: type, value, then radius or count (and a collection index).
            if (size < 8) {
                heuristic(i, 0, static_cast<uint32_t>(size));
                break;
            }
            const int32_t type = loadLE<int32_t>(sr.data.data());
            const bool location = def.kind == FieldKind::PackageLocation;
            const bool form = location ? (type == 0 || type == 1 || type == 4 || type == 6)
                                       : (type == 0 || type == 1 || type == 3 || type == 7);
            const bool known = location ? (type >= 0 && type <= 14 && type != 12 && type != 13) : (type >= 0 && type <= 8);
            if (form)
                slot(i, 4);
            else if (!known)
                heuristic(i, 4, 8);
            break;
        }
        case FieldKind::ResponseData:
            // xEdit: emotion keyword or FFFFFFFF (none), response number, sound, unknown, interrupt
            // percentage, camera target and location aliases.
            if (size != 20) {
                issue(Severity::Warning, sr.signature, "unexpected size " + std::to_string(size) + " (expected 20)");
                heuristic(i, 0, static_cast<uint32_t>(size));
                break;
            }
            if (loadLE<uint32_t>(sr.data.data()) != 0xFFFFFFFFu)
                slot(i, 0);
            slot(i, 5);
            break;
        case FieldKind::TopicData:
            if (size == 8 && loadLE<uint32_t>(sr.data.data()) == 0)
                slot(i, 4); // a topic
            else if (size != 8 || loadLE<uint32_t>(sr.data.data()) != 1)
                heuristic(i, 0, static_cast<uint32_t>(size)); // not a subtype either: scanned
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
        // Object format 1: FormID, alias i16, unused u16.
        // Object format 2 (what Fallout 4 writes): unused u16, alias i16, FormID.
        size_t base = r.position();
        if (objFormat == 1) {
            slot(i, base);
            r.skip(8);
        } else {
            r.skip(4);
            slot(i, r.position());
            r.skip(4);
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

    // Script fragments (PERK, TERM, QUST, INFO, SCEN, PACK): an extra-bind-data version byte and the
    // fragment script with its own properties, followed by the fragment list (indices and names).
    // Anything that does not parse that way is scanned instead.
    void vmadFragments(BinaryReader& r, uint32_t i, const Subrecord& sr, int16_t version, int16_t objFormat)
    {
        const size_t start = r.position();
        const size_t slotsBefore = out_.formIdSlots.size();
        try {
            r.skip(1); // extra bind data version
            vmadScript(r, i, version, objFormat);
        } catch (const FormatError&) {
            out_.formIdSlots.resize(slotsBefore);
            heuristic(i, static_cast<uint32_t>(start), static_cast<uint32_t>(sr.data.size()));
            return;
        }
        // The fragment list (and aliases, for quests) is scanned; the definition check shows
        // whether anything in it still looks like a FormID.
        if (!r.atEnd())
            heuristic(i, static_cast<uint32_t>(r.position()), static_cast<uint32_t>(sr.data.size()));
    }

    // Fallout 4 QUST (xEdit wbVMADFragmentedQUST): extra bind data version, fragment count, the
    // fragment script (flags and properties only when it has a name), the fragments (stage, stage
    // index, a byte, script and function names), then the aliases: each an object (the quest and
    // the alias ID), its own version and object format, and its scripts. It must end exactly at the
    // end of the subrecord.
    void questFragments(BinaryReader& r, uint32_t i, int16_t version, int16_t objFormat)
    {
        r.skip(1); // extra bind data version
        const uint16_t fragments = r.read<uint16_t>();
        if (!r.readWString().empty()) {
            if (version >= 4)
                r.skip(1); // status
            const uint16_t props = r.read<uint16_t>();
            for (uint16_t p = 0; p < props; ++p)
                vmadProperty(r, i, version, objFormat, 0);
        }
        for (uint16_t f = 0; f < fragments; ++f) {
            r.skip(4 + 4 + 1); // stage, stage index, unknown
            r.readWString();   // script name
            r.readWString();   // fragment (function) name
        }
        const uint16_t aliases = r.read<uint16_t>();
        for (uint16_t a = 0; a < aliases; ++a) {
            vmadObject(r, i, objFormat);
            const int16_t aliasVersion = r.read<int16_t>();
            const int16_t aliasFormat = r.read<int16_t>();
            const uint16_t scripts = r.read<uint16_t>();
            for (uint16_t s = 0; s < scripts; ++s)
                vmadScript(r, i, aliasVersion, aliasFormat);
        }
        if (!r.atEnd())
            throw FormatError("data after the quest's aliases");
    }

    // Fallout 4 PACK (xEdit wbVMADFragmentedPACK): extra bind data version, flags (OnBegin, OnEnd,
    // OnChange), the fragment script, then one fragment per flag set (a byte, script and function
    // names). It must end exactly at the end of the subrecord.
    void packageFragments(BinaryReader& r, uint32_t i, int16_t version, int16_t objFormat)
    {
        r.skip(1); // extra bind data version
        const uint8_t flags = r.read<uint8_t>();
        vmadScript(r, i, version, objFormat);
        for (int f = 0; f < 3; ++f) {
            if (!(flags & (1 << f)))
                continue;
            r.skip(1);
            r.readWString(); // script name
            r.readWString(); // fragment (function) name
        }
        if (!r.atEnd())
            throw FormatError("data after the package's fragments");
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
                if (record_ == Signature("QUST"))
                    questFragments(r, i, version, objFormat);
                else if (record_ == Signature("PACK"))
                    packageFragments(r, i, version, objFormat);
                else
                    vmadFragments(r, i, sr, version, objFormat);
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
            // Parameters of other functions are FormIDs or numbers. A value that names an
            // existing record is treated as a FormID (numbers rarely do, and values under
            // master index 1 are written unchanged either way).
            probable(i, 12);
            probable(i, 16);
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

    // --- Navmeshes (layouts from xEdit) -----------------------------------------
    // Every one must end exactly at the end of its subrecord; anything else is scanned and reported.

    // A uint32 count of `size`-byte elements that must fit in what is left.
    uint32_t counted(BinaryReader& r, size_t size, const char* what)
    {
        const uint32_t n = r.read<uint32_t>();
        if (static_cast<uint64_t>(n) * size > r.remaining())
            throw FormatError(std::string("implausible ") + what + " count " + std::to_string(n));
        return n;
    }
    void formIds(BinaryReader& r, uint32_t i, uint32_t n)
    {
        for (uint32_t k = 0; k < n; ++k) {
            slot(i, r.position());
            r.skip(4);
        }
    }
    // Pathing cell: CRC, parent world, then the grid (exterior) or the parent cell (world 0).
    void pathingCell(BinaryReader& r, uint32_t i)
    {
        r.skip(4);
        const auto world = r.position();
        const bool interior = r.read<uint32_t>() == 0;
        if (interior)
            slot(i, r.position());
        else
            slot(i, world);
        r.skip(4);
    }

    // Older navmeshes (as xEdit notes): cover data from version 13, waypoints from version 12.
    void navmeshGeometry(BinaryReader& r, uint32_t i)
    {
        const uint32_t version = r.read<uint32_t>();
        try {
            navmeshBody(r, i, version);
        } catch (const FormatError& e) {
            throw FormatError("version " + std::to_string(version) + ": " + e.what());
        }
        // Some navmeshes end with 4 more bytes than xEdit describes (10 of 28,517 in a real load
        // order). They are kept as they are; if they name an existing record they are treated as a
        // FormID, so the check counts them.
        if (r.remaining() == 4) {
            probable(i, r.position());
            issue(Severity::Info, Signature("NVNM"), "4 bytes after the navmesh grid, beyond the xEdit layout");
            r.skip(4);
        }
        if (!r.atEnd())
            throw FormatError("version " + std::to_string(version) + ": " + std::to_string(r.remaining()) +
                              " trailing bytes");
    }
    void navmeshBody(BinaryReader& r, uint32_t i, uint32_t version)
    {
        pathingCell(r, i);
        r.skip(static_cast<size_t>(counted(r, 12, "vertex")) * 12);
        r.skip(static_cast<size_t>(counted(r, 21, "triangle")) * 21);
        for (uint32_t n = counted(r, 11, "edge link"), k = 0; k < n; ++k) { // type, navmesh, triangle, edge
            r.skip(4);
            slot(i, r.position());
            r.skip(7);
        }
        for (uint32_t n = counted(r, 10, "door link"), k = 0; k < n; ++k) { // triangle, CRC, door reference
            r.skip(6);
            slot(i, r.position());
            r.skip(4);
        }
        if (version > 12) {
            r.skip(static_cast<size_t>(counted(r, 8, "cover")) * 8);
            r.skip(static_cast<size_t>(counted(r, 4, "cover triangle")) * 4);
        }
        if (version > 11)
            r.skip(static_cast<size_t>(counted(r, 18, "waypoint")) * 18);
        const uint32_t divisor = r.read<uint32_t>();
        r.skip(8 + 24); // grid size, bounds
        const uint32_t cells = divisor <= 12 ? divisor * divisor : 0;
        for (uint32_t k = 0; k < cells; ++k)
            r.skip(static_cast<size_t>(counted(r, 2, "grid cell triangle")) * 2);
    }

    void navmeshInfo(BinaryReader& r, uint32_t i)
    {
        slot(i, r.position()); // the navmesh
        r.skip(4 + 4 + 12 + 4); // navmesh, flags, approximate location, preferred %
        formIds(r, i, counted(r, 4, "edge link"));
        formIds(r, i, counted(r, 4, "preferred edge link"));
        for (uint32_t n = counted(r, 8, "door link"), k = 0; k < n; ++k) { // CRC, door reference
            r.skip(4);
            slot(i, r.position());
            r.skip(4);
        }
        if (r.read<uint8_t>() != 0) { // island data: bounds, triangles, vertices
            r.skip(24);
            r.skip(static_cast<size_t>(counted(r, 6, "island triangle")) * 6);
            r.skip(static_cast<size_t>(counted(r, 12, "island vertex")) * 12);
        }
        pathingCell(r, i);
    }

    void precomputedPathing(BinaryReader& r, uint32_t i)
    {
        for (uint32_t n = counted(r, 4, "path"), k = 0; k < n; ++k)
            formIds(r, i, counted(r, 4, "path navmesh"));
        for (uint32_t n = counted(r, 8, "road marker"), k = 0; k < n; ++k) { // navmesh, index
            slot(i, r.position());
            r.skip(8);
        }
    }

    void precutMap(BinaryReader& r, uint32_t i)
    {
        while (!r.atEnd()) { // reference, uint16 count of uint16 triangles
            slot(i, r.position());
            r.skip(4);
            const uint16_t n = r.read<uint16_t>();
            if (static_cast<size_t>(n) * 2 > r.remaining())
                throw FormatError("implausible triangle count " + std::to_string(n));
            r.skip(static_cast<size_t>(n) * 2);
        }
    }

    void decodeNavmesh(uint32_t i, const Subrecord& sr, FieldKind kind)
    {
        const size_t slotsBefore = out_.formIdSlots.size();
        try {
            BinaryReader r(sr.data);
            switch (kind) {
            case FieldKind::Navmesh: navmeshGeometry(r, i); break;
            case FieldKind::NavmeshInfo: navmeshInfo(r, i); break;
            case FieldKind::PrecomputedPathing: precomputedPathing(r, i); break;
            default: precutMap(r, i); break;
            }
            if (!r.atEnd())
                throw FormatError(std::to_string(r.remaining()) + " trailing bytes");
        } catch (const FormatError& e) {
            out_.formIdSlots.resize(slotsBefore);
            issue(Severity::Warning, sr.signature, std::string("navmesh data not decoded: ") + e.what());
            heuristic(i, 0, static_cast<uint32_t>(sr.data.size()));
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
