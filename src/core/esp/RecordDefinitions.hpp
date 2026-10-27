#pragma once

#include "esp/Signature.hpp"
#include "esp/StringTables.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace fo4mx {

// How the bytes of a subrecord are laid out, as far as FormIDs and strings
// are concerned. The compiler only needs to know where FormIDs live (so they
// can be rewritten) and which fields are localised strings (so they can be
// materialised), so every other value is treated as opaque bytes.
enum class FieldKind : uint8_t {
    Unknown,        // not described: may contain FormIDs (heuristically scanned)
    NoRefs,         // described: numbers / flags / markers, no FormIDs
    ZString,        // inline text
    LString,        // localised string (STRINGS)
    DLString,       // localised string (DLSTRINGS)
    ILString,       // localised string (ILSTRINGS)
    FormId,         // one FormID at offset 0 (size must be 4)
    FormIdArray,    // packed array of FormIDs
    Struct,         // fixed layout, FormIDs at `offsets`
    StructArray,    // packed array of `stride`-byte elements, FormIDs at `offsets` in each
    Vmad,           // Papyrus script attachments
    Condition,      // CTDA
    ObjectTemplate, // OBTS
    OmodData,       // OMOD DATA
    PerkData,       // PERK DATA (meaning depends on the preceding PRKE)
    PerkEntryData,  // PERK EPFD (meaning depends on the preceding EPFT)
    VendorLocation, // FACT PLVD
};

// How two plugins' edits of a field combine during merging.
enum class MergeMode : uint8_t {
    Scalar,   // whole value; last changer wins
    Set,      // list of unique items; additions and removals merge
    Multiset, // list allowing duplicates (leveled list entries)
    Keyed,    // list keyed by the first FormID; values per key merge
};

struct FieldDef {
    FieldKind kind = FieldKind::Unknown;
    std::vector<uint16_t> offsets; // Struct / StructArray FormID offsets
    uint16_t stride = 0;           // StructArray element size
    uint16_t requiredSize = 0;     // FormId / Struct: expected size (0 = any)
    bool partial = false;          // bytes outside `offsets` may hold undescribed FormIDs
    std::vector<std::pair<uint16_t, uint16_t>> plain; // partial structs: [begin, end) known to hold no FormIDs
    MergeMode merge = MergeMode::Scalar;
    Signature countSig;            // count subrecord written before this list (KSIZ, LLCT)
    uint8_t countWidth = 0;        // byte width of the count subrecord
    Signature companion;           // subrecord that belongs to each list item (COED after LVLO)

    bool isLocalized() const
    {
        return kind == FieldKind::LString || kind == FieldKind::DLString || kind == FieldKind::ILString;
    }
    StringFileKind stringFile() const
    {
        return kind == FieldKind::DLString   ? StringFileKind::DLStrings
               : kind == FieldKind::ILString ? StringFileKind::ILStrings
                                             : StringFileKind::Strings;
    }
    // Lists spread over repeated subrecords (LNAM, LVLO) as opposed to arrays packed in one.
    bool isRepeatedList() const
    {
        return merge != MergeMode::Scalar && kind != FieldKind::FormIdArray && kind != FieldKind::StructArray;
    }
    bool isPackedArray() const { return kind == FieldKind::FormIdArray || kind == FieldKind::StructArray; }
};

// A run of subrecords that only make sense together (a model and its
// texture hashes, a condition list, an effect list). Regions are compared and
// merged atomically.
struct RegionDef {
    std::string name;
    std::vector<Signature> start;
    std::vector<Signature> members;
    Signature end; // optional terminator consumed as the last member

    bool startsWith(Signature s) const;
    bool contains(Signature s) const;
};

struct RecordDef {
    Signature signature;
    std::string name;
    bool compilable = false;
    std::unordered_map<Signature, FieldDef> fields;
    std::vector<RegionDef> regions;
};

// Runtime profile: which game build the definitions describe.
enum class RuntimeProfile : uint8_t { OldGen_1_10_163, NextGen };

class RecordDefinitions {
public:
    static const RecordDefinitions& get(RuntimeProfile profile = RuntimeProfile::OldGen_1_10_163);

    const std::string& profileName() const { return profileName_; }
    // Changes whenever any layout changes, so cached decoding results are not reused across versions.
    uint64_t fingerprint() const { return fingerprint_; }
    const RecordDef* record(Signature s) const;
    // Record-specific definition first, then the common one. Never null.
    const FieldDef& field(Signature record, Signature sub) const;
    bool isCompilable(Signature s) const;
    const std::vector<Signature>& compilableTypes() const { return compilable_; }
    std::string recordName(Signature s) const;

    // Condition functions whose parameters are FormIDs.
    bool conditionParamIsForm(uint16_t function, int param) const;
    bool conditionFunctionKnown(uint16_t function) const;

    // Constructed by the profile builders.
    RecordDefinitions() = default;
    std::string profileName_;
    std::unordered_map<Signature, FieldDef> common_;
    std::unordered_map<Signature, RecordDef> records_;
    std::vector<Signature> compilable_;
    std::unordered_map<uint16_t, uint8_t> conditionFormParams_; // bit0 = param1, bit1 = param2
    std::unordered_map<Signature, std::string> names_;
    uint64_t fingerprint_ = 0;

    void computeFingerprint();
};

} // namespace fo4mx
