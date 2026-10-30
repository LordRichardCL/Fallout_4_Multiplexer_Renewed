#include "papyrus/Pex.hpp"

#include "util/Binary.hpp"
#include "util/Text.hpp"

#include <cstring>
#include <stdexcept>

namespace fo4mx::pex {

const char* stringUseName(StringUse u)
{
    switch (u) {
    case StringUse::GetFormFromFile: return "Game.GetFormFromFile";
    case StringUse::IsPluginInstalled: return "Game.IsPluginInstalled";
    case StringUse::CallArgument: return "function argument";
    case StringUse::Assignment: return "assignment";
    case StringUse::Comparison: return "comparison";
    case StringUse::Concatenation: return "text concatenation";
    case StringUse::Default: return "default value";
    case StringUse::Other: return "other instruction";
    }
    return "";
}

namespace {

constexpr uint32_t kMagic = 0xFA57C0DE; // little endian: Fallout 4 (Skyrim scripts are big endian)
constexpr uint16_t kFallout4 = 2;

// Fixed arguments per opcode, Fallout 4 (nop .. array_clear). callmethod, callparent and callstatic
// are followed by a variable argument count and that many values.
constexpr uint8_t kArgs[] = {0, 3, 3, 3, 3, 3, 3, 3, 3, 3, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 1, 2, 2, 3, 2, 3,
                             1, 3, 3, 3, 2, 2, 3, 3, 4, 4, 3, 1, 3, 3, 5, 5, 3, 3, 1, 3, 1};
static_assert(sizeof(kArgs) == 0x2F, "opcodes 0x00 .. 0x2E");
constexpr uint8_t kCallMethod = 0x17, kCallParent = 0x18, kCallStatic = 0x19;

struct Value {
    uint8_t type = 0;
    size_t offset = 0; // payload offset
    uint16_t string = 0;
    int32_t integer = 0;
};

class Walker {
public:
    Walker(const std::vector<uint8_t>& bytes, Scan& out) : r_(bytes), out_(out) {}

    void run()
    {
        if (r_.read<uint32_t>() != kMagic)
            throw FormatError("not a Fallout 4 script (byte order or magic number)");
        r_.read<uint8_t>(); // major version
        r_.read<uint8_t>(); // minor version
        if (r_.read<uint16_t>() != kFallout4)
            throw FormatError("not a Fallout 4 script (game ID)");
        r_.read<uint64_t>(); // compilation time
        r_.readWString();    // source file
        r_.readWString();    // user
        r_.readWString();    // machine

        out_.stringTableBegin = r_.position();
        const uint16_t count = r_.read<uint16_t>();
        out_.strings.reserve(count);
        for (uint16_t i = 0; i < count; ++i)
            out_.strings.push_back(r_.readWString());
        out_.stringTableEnd = r_.position();
        out_.usedElsewhere.assign(count, false);

        debugInfo();
        const uint16_t flags = r_.read<uint16_t>(); // user flag definitions
        for (uint16_t i = 0; i < flags; ++i) {
            str();
            r_.read<uint8_t>();
        }
        const uint16_t objects = r_.read<uint16_t>();
        for (uint16_t i = 0; i < objects; ++i)
            object();
        if (r_.remaining() != 0)
            throw FormatError(std::to_string(r_.remaining()) + " bytes left after the last object");
    }

private:
    uint16_t str()
    {
        uint16_t i = r_.read<uint16_t>();
        if (i >= out_.strings.size())
            throw FormatError("string index out of range");
        return i;
    }
    void strs(uint16_t n)
    {
        for (uint16_t i = 0; i < n; ++i)
            str();
    }

    Value value()
    {
        Value v;
        v.type = r_.read<uint8_t>();
        v.offset = r_.position();
        switch (v.type) {
        case 0: break;
        case 1: v.string = str(); break; // identifier
        case 2:                          // string constant
            v.string = str();
            break;
        case 3: v.integer = r_.read<int32_t>(); break;
        case 4: r_.read<float>(); break;
        case 5: r_.read<uint8_t>(); break;
        default: throw FormatError("unknown value type " + std::to_string(v.type));
        }
        return v;
    }
    void markString(const Value& v, StringUse use = StringUse::Other, const std::string& function = {})
    {
        if (v.type != 2)
            return;
        out_.usedElsewhere[v.string] = true;
        StringUsage u;
        u.string = v.string;
        u.use = use;
        u.offset = v.offset;
        u.function = function;
        u.where = where_;
        out_.uses.push_back(std::move(u));
    }

    void debugInfo()
    {
        if (r_.read<uint8_t>() == 0)
            return;
        r_.read<uint64_t>(); // modification time
        const uint16_t functions = r_.read<uint16_t>();
        for (uint16_t i = 0; i < functions; ++i) {
            strs(3); // object, state, function
            r_.read<uint8_t>();
            r_.skip(size_t(r_.read<uint16_t>()) * 2); // line numbers
        }
        const uint16_t groups = r_.read<uint16_t>(); // property groups
        for (uint16_t i = 0; i < groups; ++i) {
            strs(3); // object, group, doc
            r_.read<uint32_t>();
            strs(r_.read<uint16_t>());
        }
        const uint16_t orders = r_.read<uint16_t>(); // struct orders
        for (uint16_t i = 0; i < orders; ++i) {
            strs(2);
            strs(r_.read<uint16_t>());
        }
    }

    void object()
    {
        object_ = out_.strings[str()]; // name
        const uint32_t size = r_.read<uint32_t>();
        const size_t start = r_.position();
        strs(2); // parent class, doc
        r_.read<uint8_t>();  // const
        r_.read<uint32_t>(); // user flags
        str();               // auto state
        const uint16_t structs = r_.read<uint16_t>();
        for (uint16_t i = 0; i < structs; ++i) {
            str();
            const uint16_t members = r_.read<uint16_t>();
            for (uint16_t m = 0; m < members; ++m) {
                strs(2);
                r_.read<uint32_t>();
                markString(value(), StringUse::Default);
                r_.read<uint8_t>();
                str();
            }
        }
        const uint16_t variables = r_.read<uint16_t>();
        for (uint16_t i = 0; i < variables; ++i) {
            strs(2);
            r_.read<uint32_t>();
            markString(value(), StringUse::Default);
            r_.read<uint8_t>();
        }
        const uint16_t properties = r_.read<uint16_t>();
        for (uint16_t i = 0; i < properties; ++i) {
            const std::string property = out_.strings[str()];
            strs(2); // type, doc
            r_.read<uint32_t>();
            const uint8_t flags = r_.read<uint8_t>();
            where_ = object_ + "." + property;
            if (flags & 4) {
                str(); // auto variable
            } else {
                if (flags & 1)
                    function();
                if (flags & 2)
                    function();
            }
        }
        const uint16_t states = r_.read<uint16_t>();
        for (uint16_t i = 0; i < states; ++i) {
            const std::string state = out_.strings[str()];
            const uint16_t functions = r_.read<uint16_t>();
            for (uint16_t f = 0; f < functions; ++f) {
                const std::string fn = out_.strings[str()];
                where_ = object_ + (state.empty() ? "" : "." + state) + "." + fn;
                function();
            }
        }
        // Compilers record the object's size with or without the size field itself.
        const size_t read = r_.position() - start;
        if (read != size && read + 4 != size)
            throw FormatError("object size does not match its contents");
    }

    void function()
    {
        strs(2); // return type, doc
        r_.read<uint32_t>();
        r_.read<uint8_t>();
        const uint16_t params = r_.read<uint16_t>();
        strs(params * 2);
        const uint16_t locals = r_.read<uint16_t>();
        strs(locals * 2);
        const uint16_t instructions = r_.read<uint16_t>();
        for (uint16_t i = 0; i < instructions; ++i)
            instruction();
    }

    void instruction()
    {
        const uint8_t op = r_.read<uint8_t>();
        if (op >= sizeof(kArgs))
            throw FormatError("unknown opcode " + std::to_string(op));
        std::vector<Value> args(kArgs[op]);
        for (auto& a : args)
            a = value();
        std::vector<Value> varargs;
        if (op == kCallMethod || op == kCallParent || op == kCallStatic) {
            Value n = value();
            if (n.type != 3 || n.integer < 0)
                throw FormatError("invalid variable argument count");
            varargs.resize(static_cast<size_t>(n.integer));
            for (auto& a : varargs)
                a = value();
        }

        const bool getFormFromFile = op == kCallStatic && args.size() == 3 && varargs.size() == 2 &&
                                     (args[0].type == 1 || args[0].type == 2) && args[1].type == 1 &&
                                     iequals(out_.strings[args[0].string], "Game") &&
                                     iequals(out_.strings[args[1].string], "GetFormFromFile");
        if (getFormFromFile) {
            FormFromFileCall c;
            c.formIdLiteral = varargs[0].type == 3;
            c.formId = varargs[0].integer;
            c.formIdOffset = varargs[0].offset;
            c.fileLiteral = varargs[1].type == 2;
            c.fileString = varargs[1].string;
            c.fileStringOffset = varargs[1].offset;
            out_.getFormFromFile.push_back(c);
            markString(varargs[0], StringUse::CallArgument, "Game.GetFormFromFile");
            for (const auto& a : args)
                markString(a);
            if (varargs[1].type == 2) {
                StringUsage u;
                u.string = varargs[1].string;
                u.use = StringUse::GetFormFromFile;
                u.offset = varargs[1].offset;
                u.function = "Game.GetFormFromFile";
                u.where = where_;
                u.formIdLiteral = c.formIdLiteral;
                u.formId = c.formId;
                u.formIdOffset = c.formIdOffset;
                out_.uses.push_back(std::move(u));
            }
            return;
        }

        // The called function, for call arguments.
        std::string function;
        if (op == kCallStatic && args.size() == 3 && args[1].type == 1)
            function = (args[0].type == 1 || args[0].type == 2 ? out_.strings[args[0].string] + "." : std::string()) +
                       out_.strings[args[1].string];
        else if ((op == kCallMethod || op == kCallParent) && !args.empty() && args[0].type == 1)
            function = out_.strings[args[0].string];
        const bool isPluginInstalled = op == kCallStatic && iequals(function, "Game.IsPluginInstalled") &&
                                       varargs.size() == 1;

        StringUse use = StringUse::Other;
        if (op == kCallMethod || op == kCallParent || op == kCallStatic)
            use = isPluginInstalled ? StringUse::IsPluginInstalled : StringUse::CallArgument;
        else if (op == 0x0D || op == 0x1D) // assign, propset
            use = StringUse::Assignment;
        else if (op >= 0x0F && op <= 0x13) // cmp_eq .. cmp_ge
            use = StringUse::Comparison;
        else if (op == 0x1B) // strcat
            use = StringUse::Concatenation;
        for (const auto& a : args)
            markString(a, use == StringUse::IsPluginInstalled ? StringUse::Other : use, function);
        for (const auto& a : varargs)
            markString(a, use, function);
    }

    BinaryReader r_;
    Scan& out_;
    std::string object_, where_;
};

} // namespace

Scan scan(const std::vector<uint8_t>& bytes)
{
    Scan out;
    try {
        Walker(bytes, out).run();
        out.ok = true;
    } catch (const std::exception& e) {
        out = Scan{};
        out.error = e.what();
    }
    return out;
}

std::vector<uint8_t> rewrite(const std::vector<uint8_t>& bytes, const Scan& scan, const std::vector<std::string>& strings,
                             const std::vector<Patch>& patches)
{
    if (!scan.ok)
        throw std::logic_error("rewrite of a script that did not scan");
    if (strings.size() < scan.strings.size() || strings.size() > 0xFFFF)
        throw std::logic_error("string table cannot shrink or exceed 65535 entries");
    BinaryWriter w;
    w.writeBytes(std::span<const uint8_t>(bytes.data(), scan.stringTableBegin));
    w.write<uint16_t>(static_cast<uint16_t>(strings.size()));
    for (const auto& s : strings) {
        if (s.size() > 0xFFFF)
            throw std::logic_error("string too long");
        w.writeWString(s);
    }
    const size_t tail = w.size();
    w.writeBytes(std::span<const uint8_t>(bytes.data() + scan.stringTableEnd, bytes.size() - scan.stringTableEnd));
    auto out = w.take();
    for (const auto& p : patches) {
        if (p.offset < scan.stringTableEnd || p.offset + p.bytes.size() > bytes.size())
            throw std::logic_error("patch outside the code section");
        std::memcpy(out.data() + tail + (p.offset - scan.stringTableEnd), p.bytes.data(), p.bytes.size());
    }
    return out;
}

} // namespace fo4mx::pex
