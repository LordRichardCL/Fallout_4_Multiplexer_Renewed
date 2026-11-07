#include "esp/FieldValues.hpp"

#include "util/Binary.hpp"

#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace fo4mx {

namespace {

std::string hexBytes(const uint8_t* p, size_t n)
{
    std::string out;
    char b[4];
    const size_t shown = n > 64 ? 64 : n;
    for (size_t i = 0; i < shown; ++i) {
        std::snprintf(b, sizeof(b), i ? " %02X" : "%02X", p[i]);
        out += b;
    }
    if (n > shown)
        out += " ... (" + std::to_string(n) + " bytes)";
    return out;
}

std::string formIdText(uint32_t v)
{
    char b[16];
    std::snprintf(b, sizeof(b), "%08X", v);
    return b;
}

std::string numberText(double v)
{
    char b[32];
    std::snprintf(b, sizeof(b), "%.6g", v);
    return b;
}

size_t typeSize(const ValueMember& m)
{
    switch (m.type) {
    case ValueType::U8:
    case ValueType::I8:
    case ValueType::Flags8:
    case ValueType::Enum8: return 1;
    case ValueType::U16:
    case ValueType::I16:
    case ValueType::Flags16: return 2;
    case ValueType::Bytes: return m.size;
    default: return 4;
    }
}

FieldValue member(const std::string& base, const FieldDef& def, const ValueMember& m, const std::vector<uint8_t>& d)
{
    FieldValue v;
    v.path = base + "\\" + m.name;
    v.field = def.label;
    v.type = m.type;
    const size_t size = typeSize(m);
    if (size_t(m.offset) + size > d.size()) {
        v.text = "(not present: the field has " + std::to_string(d.size()) + " bytes)";
        v.described = false;
        return v;
    }
    const uint8_t* p = d.data() + m.offset;
    uint64_t u = 0;
    switch (m.type) {
    case ValueType::U8:
    case ValueType::Flags8:
    case ValueType::Enum8: u = p[0]; break;
    case ValueType::U16:
    case ValueType::Flags16: u = loadLE<uint16_t>(p); break;
    case ValueType::U32:
    case ValueType::Flags32:
    case ValueType::Enum32:
    case ValueType::FormId: u = loadLE<uint32_t>(p); break;
    default: break;
    }
    switch (m.type) {
    case ValueType::I8: v.number = static_cast<int8_t>(p[0]); v.text = numberText(v.number); break;
    case ValueType::I16: v.number = loadLE<int16_t>(p); v.text = numberText(v.number); break;
    case ValueType::I32: v.number = loadLE<int32_t>(p); v.text = numberText(v.number); break;
    case ValueType::Float: v.number = loadLE<float>(p); v.text = numberText(v.number); break;
    case ValueType::FormId:
        v.formId = static_cast<uint32_t>(u);
        v.number = static_cast<double>(u);
        v.text = formIdText(v.formId);
        break;
    case ValueType::Flags8:
    case ValueType::Flags16:
    case ValueType::Flags32: {
        v.number = static_cast<double>(u);
        char b[16];
        std::snprintf(b, sizeof(b), "0x%llX", static_cast<unsigned long long>(u));
        v.text = b;
        std::string names;
        for (size_t bit = 0; bit < 32; ++bit)
            if (u & (uint64_t(1) << bit))
                names += (names.empty() ? "" : ", ") +
                         (bit < m.labels.size() ? m.labels[bit] : "bit " + std::to_string(bit));
        if (!names.empty())
            v.text += " (" + names + ")";
        break;
    }
    case ValueType::Enum8:
    case ValueType::Enum32:
        v.number = static_cast<double>(u);
        v.text = u < m.labels.size() ? m.labels[u] : numberText(v.number);
        break;
    case ValueType::Bytes: v.text = hexBytes(p, size); break;
    default: v.number = static_cast<double>(u); v.text = numberText(v.number); break;
    }
    return v;
}

} // namespace

std::vector<FieldValue> decodeValues(const RecordDefinitions& defs, Signature record, const std::vector<Subrecord>& subs,
                                     bool localized)
{
    std::vector<FieldValue> out;
    std::unordered_map<uint32_t, int> seen;
    std::unordered_map<uint32_t, int> total;
    for (const auto& sr : subs)
        ++total[sr.signature.value];
    for (const auto& sr : subs) {
        const int n = seen[sr.signature.value]++;
        std::string base = sr.signature.str();
        if (total[sr.signature.value] > 1)
            base += "#" + std::to_string(n);
        const auto& def = defs.field(record, sr.signature);
        const auto& d = sr.data;
        if (!def.values.empty()) {
            for (const auto& m : def.values)
                out.push_back(member(base, def, m, d));
            continue;
        }
        FieldValue v;
        v.path = base;
        v.field = def.label;
        switch (def.kind) {
        case FieldKind::ZString:
            v.type = ValueType::Bytes;
            v.text = sr.asZString();
            out.push_back(std::move(v));
            break;
        case FieldKind::LString:
        case FieldKind::DLString:
        case FieldKind::ILString:
            // A localised plugin stores a string table id, others the text itself.
            if (localized && d.size() == 4) {
                v.type = ValueType::U32;
                v.number = loadLE<uint32_t>(d.data());
                v.text = "string id " + formIdText(static_cast<uint32_t>(v.number));
            } else {
                v.text = sr.asZString();
            }
            out.push_back(std::move(v));
            break;
        case FieldKind::FormId:
            if (d.size() == 4) {
                v.type = ValueType::FormId;
                v.formId = loadLE<uint32_t>(d.data());
                v.number = v.formId;
                v.text = formIdText(v.formId);
            } else {
                v.described = false;
                v.text = hexBytes(d.data(), d.size());
            }
            out.push_back(std::move(v));
            break;
        case FieldKind::FormIdArray:
            for (size_t o = 0; o + 4 <= d.size(); o += 4) {
                FieldValue e;
                e.path = base + "[" + std::to_string(o / 4) + "]";
                e.field = def.label;
                e.type = ValueType::FormId;
                e.formId = loadLE<uint32_t>(d.data() + o);
                e.number = e.formId;
                e.text = formIdText(e.formId);
                out.push_back(std::move(e));
            }
            break;
        default:
            v.described = false;
            v.text = hexBytes(d.data(), d.size());
            out.push_back(std::move(v));
            break;
        }
    }
    return out;
}

} // namespace fo4mx
