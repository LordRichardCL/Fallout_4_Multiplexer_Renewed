#pragma once

#include "esp/Record.hpp"
#include "esp/RecordDefinitions.hpp"

#include <string>
#include <vector>

namespace fo4mx {

// One named value of a record, as xEdit shows it: "DNAM \ Speed = 1.25".
struct FieldValue {
    std::string path;  // subrecord (with #n for the n-th repeat), then the member: "LVLO#2\Reference"
    std::string field; // the field's display name, when known ("Data")
    ValueType type = ValueType::Bytes;
    bool described = true; // false: no member layout, shown as bytes
    double number = 0;     // integer, float, flag and enum values
    uint32_t formId = 0;   // FormId: as stored in the plugin (master index in the top byte)
    std::string text;      // display text
};

// The record's values, member by member where the definitions name them, otherwise by the field's
// kind (text, FormIDs, localised string ids) or as bytes. Every byte of every subrecord is covered.
// `localized`: the plugin's Localized flag (string fields then hold string table ids).
std::vector<FieldValue> decodeValues(const RecordDefinitions& defs, Signature record, const std::vector<Subrecord>& subs,
                                     bool localized = false);

} // namespace fo4mx
