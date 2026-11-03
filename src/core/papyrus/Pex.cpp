#include "papyrus/Pex.hpp"

#include "util/Binary.hpp"
#include "util/Text.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_map>

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
    case StringUse::PropertyValue: return "constant property";
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
// Fallout 4 Papyrus opcode names (Champollion / the CK's assembler), for the reports.
constexpr const char* kOpNames[] = {
    "nop", "iadd", "fadd", "isub", "fsub", "imul", "fmul", "idiv", "fdiv", "imod", "not", "ineg", "fneg", "assign",
    "cast", "cmp_eq", "cmp_lt", "cmp_le", "cmp_gt", "cmp_ge", "jmp", "jmpt", "jmpf", "callmethod", "callparent",
    "callstatic", "return", "strcat", "propget", "propset", "array_create", "array_length", "array_getelement",
    "array_setelement", "array_findelement", "array_rfindelement", "is", "struct_create", "struct_get", "struct_set",
    "array_findstruct", "array_rfindstruct", "array_add", "array_insert", "array_removelast", "array_remove",
    "array_clear"};
static_assert(sizeof(kOpNames) / sizeof(kOpNames[0]) == 0x2F, "opcodes 0x00 .. 0x2E");

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
        u.opcode = opcode_;
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
            const std::string name = out_.strings[str()];
            str(); // type
            r_.read<uint32_t>();
            const Value v = value();
            markString(v, StringUse::Default);
            objectVars_[name] = v;
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
                autoVars_.insert(out_.strings[str()]); // the Auto property's variable: script data may set it
            } else {
                if (flags & 1) {
                    getter_ = true; // the read handler: a string it returns is the property's value
                    function();
                    getter_ = false;
                }
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
                functionName_ = state.empty() ? fn : std::string();
                function();
                functionName_.clear();
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
        Function f;
        f.name = functionName_;
        f.where = where_;
        for (uint16_t i = 0; i < params; ++i) {
            f.params.push_back(out_.strings[str()]);
            str(); // type
        }
        const uint16_t locals = r_.read<uint16_t>();
        for (uint16_t i = 0; i < locals; ++i) {
            f.locals.insert(out_.strings[str()]);
            str();
        }
        functions_.push_back(std::move(f));
        const uint16_t instructions = r_.read<uint16_t>();
        propertyVars_.clear();
        for (uint16_t i = 0; i < instructions; ++i)
            instruction();
    }

    void instruction()
    {
        const uint8_t op = r_.read<uint8_t>();
        if (op >= sizeof(kArgs))
            throw FormatError("unknown opcode " + std::to_string(op));
        opcode_ = kOpNames[op];
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
        if (!functions_.empty())
            functions_.back().code.push_back({op, args, varargs});

        const bool getFormFromFile = op == kCallStatic && args.size() == 3 && varargs.size() == 2 &&
                                     (args[0].type == 1 || args[0].type == 2) && args[1].type == 1 &&
                                     iequals(out_.strings[args[0].string], "Game") &&
                                     iequals(out_.strings[args[1].string], "GetFormFromFile");
        // A property's value followed through this function: read into a variable (propget), copied
        // (assign), then used.
        auto propertyOf = [&](const Value& v) -> const std::string* {
            if (v.type != 1)
                return nullptr;
            auto it = propertyVars_.find(out_.strings[v.string]);
            return it == propertyVars_.end() ? nullptr : &it->second;
        };
        auto readOf = [&](const std::string& property, PropertyRead::Use use) {
            PropertyRead pr;
            pr.property = property;
            pr.use = use;
            pr.opcode = kOpNames[op];
            pr.where = where_;
            return pr;
        };
        // A call's result is the called function's return value: a variable holding it is followed like a
        // property read (matched by the function's name).
        auto callResult = [&]() {
            std::string fn;
            if (op == kCallMethod && args.size() == 3 && args[0].type == 1 && args[2].type == 1)
                fn = out_.strings[args[0].string];
            else if (op == kCallStatic && args.size() == 3 && args[1].type == 1 && args[2].type == 1)
                fn = out_.strings[args[1].string];
            if (fn.empty())
                return;
            for (auto& ch : fn)
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            propertyVars_[out_.strings[args[2].string]] = fn;
        };
        if (op == 0x1C && args.size() == 3 && args[0].type == 1 && args[2].type == 1) { // propget name, object, dest
            std::string p = out_.strings[args[0].string];
            for (auto& ch : p)
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            propertyVars_[out_.strings[args[2].string]] = p;
        } else if (op == 0x0D && args.size() == 2 && args[0].type == 1) { // assign dest, source
            if (const auto* p = propertyOf(args[1]))
                propertyVars_[out_.strings[args[0].string]] = *p;
            else
                propertyVars_.erase(out_.strings[args[0].string]);
        } else if (getFormFromFile) {
            if (const auto* p = propertyOf(varargs[1])) {
                auto pr = readOf(*p, PropertyRead::Use::GetFormFromFile);
                pr.formIdLiteral = varargs[0].type == 3;
                pr.formId = varargs[0].integer;
                out_.propertyReads.push_back(std::move(pr));
            }
        } else {
            // A property read by name at run time (ScriptObject.GetPropertyValue("sName")): the name is
            // a string constant argument.
            if ((op == kCallMethod || op == kCallParent) && !args.empty() && args[0].type == 1 &&
                (iequals(out_.strings[args[0].string], "GetPropertyValue") ||
                 iequals(out_.strings[args[0].string], "SetPropertyValue")))
                for (const auto& v : varargs)
                    if (v.type == 2) {
                        std::string p = out_.strings[v.string];
                        for (auto& ch : p)
                            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                        auto pr = readOf(p, PropertyRead::Use::Other);
                        pr.opcode = out_.strings[args[0].string];
                        out_.propertyReads.push_back(std::move(pr));
                    }
            const bool presence = op == kCallStatic && args.size() == 3 && args[0].type == 1 && args[1].type == 1 &&
                                  iequals(out_.strings[args[0].string], "Game") &&
                                  iequals(out_.strings[args[1].string], "IsPluginInstalled") && varargs.size() == 1;
            for (size_t k = 0; k < args.size() + varargs.size(); ++k) {
                const Value& v = k < args.size() ? args[k] : varargs[k - args.size()];
                if (const auto* p = propertyOf(v))
                    out_.propertyReads.push_back(
                        readOf(*p, presence && k >= args.size() ? PropertyRead::Use::IsPluginInstalled : PropertyRead::Use::Other));
            }
        }

        if (!getFormFromFile)
            callResult();

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
        // A plugin name returned as a constant, by a property's read handler or by a function (such as a
        // mod's GetPluginName()): a named constant, classified by the scripts that read or call it.
        if (op == 0x1A && args.size() == 1 && args[0].type == 2 &&
            (getter_ || iendsWith(out_.strings[args[0].string], ".esp") || iendsWith(out_.strings[args[0].string], ".esm") ||
             iendsWith(out_.strings[args[0].string], ".esl")))
            use = StringUse::PropertyValue;
        for (const auto& a : args)
            markString(a, use == StringUse::IsPluginInstalled ? StringUse::Other : use, function);
        for (const auto& a : varargs)
            markString(a, use, function);
    }


public:
    // Follows plugin-name constants that enter variables (an assignment, a script variable's default,
    // an argument to one of the script's own functions) through copies, text joins and the script's
    // own function parameters, to where they end up. Flow-insensitive and repeated until nothing
    // changes, so it over-approximates: a value that may reach a use counts as reaching it.
    void analyse()
    {
        auto pluginLike = [&](const Value& v) {
            if (v.type != 2)
                return false;
            const auto& t = out_.strings[v.string];
            return iendsWith(t, ".esp") || iendsWith(t, ".esm") || iendsWith(t, ".esl");
        };
        struct Origin {
            uint16_t string;
            size_t offset;
            bool overridable;
            std::set<ValueSink> sinks;
        };
        std::vector<Origin> origins;
        std::map<size_t, size_t> originAt;
        auto originOf = [&](const Value& v, bool overridable) {
            auto it = originAt.find(v.offset);
            if (it != originAt.end())
                return it->second;
            originAt[v.offset] = origins.size();
            origins.push_back({v.string, v.offset, overridable, {}});
            return origins.size() - 1;
        };
        std::map<std::string, std::set<size_t>> plain, text; // variable -> origins (text: built from them)
        for (const auto& [name, v] : objectVars_)
            if (pluginLike(v))
                plain[":" + name].insert(originOf(v, autoVars_.count(name) > 0));
        std::map<std::string, size_t> byName;
        for (size_t f = 0; f < functions_.size(); ++f)
            if (!functions_[f].name.empty())
                byName.emplace(toLower(functions_[f].name), f);
        auto nodeOf = [&](size_t f, const std::string& n) {
            const auto& F = functions_[f];
            if (F.locals.count(n) || std::find(F.params.begin(), F.params.end(), n) != F.params.end())
                return std::to_string(f) + ":" + n;
            return ":" + n;
        };
        bool changed = true;
        for (int round = 0; changed && round < 32; ++round) {
            changed = false;
            for (size_t f = 0; f < functions_.size(); ++f) {
                const auto& F = functions_[f];
                auto of = [&](const Value& v, std::set<size_t>& p, std::set<size_t>& t) {
                    p.clear();
                    t.clear();
                    if (v.type == 1) {
                        const auto n = nodeOf(f, out_.strings[v.string]);
                        if (auto it = plain.find(n); it != plain.end())
                            p = it->second;
                        if (auto it = text.find(n); it != text.end())
                            t = it->second;
                    }
                };
                auto add = [&](std::map<std::string, std::set<size_t>>& m, const std::string& n, const std::set<size_t>& o) {
                    auto& dst = m[n];
                    for (size_t x : o)
                        changed |= dst.insert(x).second;
                };
                auto sink = [&](const std::set<size_t>& o, ValueSink s) {
                    s.where = F.where;
                    for (size_t x : o)
                        origins[x].sinks.insert(s);
                };
                auto other = [&](const std::set<size_t>& o, const std::string& what) {
                    ValueSink s;
                    s.kind = ValueSink::Kind::Other;
                    s.what = what;
                    sink(o, s);
                };
                std::set<size_t> p, t, p2, t2;
                for (const auto& in : F.code) {
                    const auto& a = in.args;
                    const auto& va = in.varargs;
                    const char* name = kOpNames[in.op];
                    if ((in.op == 0x0D || in.op == 0x0E) && a.size() == 2 && a[0].type == 1) { // assign, cast
                        const auto dest = nodeOf(f, out_.strings[a[0].string]);
                        if (pluginLike(a[1]))
                            add(plain, dest, {originOf(a[1], false)});
                        of(a[1], p, t);
                        add(plain, dest, p);
                        add(text, dest, t);
                    } else if (in.op == 0x1B && a.size() == 3 && a[0].type == 1) { // strcat: text built from it
                        const auto dest = nodeOf(f, out_.strings[a[0].string]);
                        for (size_t k = 1; k < 3; ++k) {
                            of(a[k], p, t);
                            add(text, dest, p);
                            add(text, dest, t);
                        }
                    } else if (in.op == kCallStatic && a.size() == 3 && a[0].type == 1 && a[1].type == 1) {
                        const std::string type = out_.strings[a[0].string], fn = out_.strings[a[1].string];
                        const bool gfff = iequals(type, "Game") && iequals(fn, "GetFormFromFile") && va.size() == 2;
                        const bool presence = iequals(type, "Game") && iequals(fn, "IsPluginInstalled") && va.size() == 1;
                        // Debug.* and a mod's own Trace function (AR:AR_CommonFunctions.Trace) only write logs.
                        const bool log = iequals(type, "Debug") || iequals(fn, "Trace");
                        for (size_t k = 0; k < va.size(); ++k) {
                            of(va[k], p, t);
                            if (p.empty() && t.empty())
                                continue;
                            if (log) {
                                ValueSink s;
                                s.kind = ValueSink::Kind::Log;
                                sink(p, s);
                                sink(t, s);
                            } else if (gfff && k == 1) {
                                ValueSink s;
                                s.kind = ValueSink::Kind::GetFormFromFile;
                                s.formIdLiteral = va[0].type == 3;
                                s.formId = va[0].integer;
                                s.formIdOffset = va[0].offset;
                                sink(p, s);
                                other(t, "GetFormFromFile with text built from the name");
                            } else if (presence) {
                                ValueSink s;
                                s.kind = ValueSink::Kind::IsPluginInstalled;
                                sink(p, s);
                                other(t, "IsPluginInstalled with text built from the name");
                            } else {
                                other(p, type + "." + fn);
                                other(t, type + "." + fn);
                            }
                        }
                    } else if (in.op == kCallMethod && a.size() == 3 && a[0].type == 1) {
                        const std::string fn = out_.strings[a[0].string];
                        const bool own = a[1].type == 1 && iequals(out_.strings[a[1].string], "self");
                        auto callee = own ? byName.find(toLower(fn)) : byName.end();
                        for (size_t k = 0; k < va.size(); ++k) {
                            if (callee != byName.end() && k < functions_[callee->second].params.size()) {
                                const auto param = std::to_string(callee->second) + ":" + functions_[callee->second].params[k];
                                if (pluginLike(va[k]))
                                    add(plain, param, {originOf(va[k], false)});
                                of(va[k], p, t);
                                add(plain, param, p);
                                add(text, param, t);
                                continue;
                            }
                            of(va[k], p, t);
                            other(p, fn);
                            other(t, fn);
                        }
                    } else {
                        // Any other instruction reading a variable that holds the name: its destination
                        // operand (written, not read) is skipped.
                        int dest = -1;
                        if ((in.op >= 0x01 && in.op <= 0x13 && in.op != 0x0A) || in.op == 0x0A || in.op == 0x1B ||
                            in.op == 0x1E || in.op == 0x1F || in.op == 0x20 || in.op == 0x24 || in.op == 0x25 ||
                            in.op == 0x26)
                            dest = 0;
                        else if (in.op == 0x1C || in.op == kCallStatic || in.op == kCallMethod)
                            dest = 2;
                        else if (in.op == kCallParent || in.op == 0x22 || in.op == 0x23 || in.op == 0x28 || in.op == 0x29)
                            dest = 1;
                        if (in.op == 0x14 || in.op == 0x15 || in.op == 0x16)
                            continue; // jumps
                        for (size_t k = 0; k < a.size() + va.size(); ++k) {
                            if (static_cast<int>(k) == dest)
                                continue;
                            of(k < a.size() ? a[k] : va[k - a.size()], p, t);
                            other(p, in.op == 0x1A ? std::string("returned") : std::string(name));
                            other(t, in.op == 0x1A ? std::string("returned") : std::string(name));
                        }
                    }
                }
            }
        }
        for (auto& o : origins) {
            ValueFlow vf;
            vf.string = o.string;
            vf.offset = o.offset;
            vf.overridable = o.overridable;
            vf.sinks.assign(o.sinks.begin(), o.sinks.end());
            out_.flows.push_back(std::move(vf));
        }
    }

private:
    BinaryReader r_;
    Scan& out_;
    std::string object_, where_;
    const char* opcode_ = "";
    // For the value-flow analysis (flows()): every function's parameters, locals and code, the
    // script's variables with their defaults, and the variables behind Auto properties.
    struct Instr {
        uint8_t op = 0;
        std::vector<Value> args, varargs;
    };
    struct Function {
        std::string name; // callable name (default state), empty otherwise
        std::string where;
        std::vector<std::string> params;
        std::set<std::string> locals;
        std::vector<Instr> code;
    };
    std::vector<Function> functions_;
    std::map<std::string, Value> objectVars_;
    std::set<std::string> autoVars_;
    std::string functionName_;
    bool getter_ = false;
    std::unordered_map<std::string, std::string> propertyVars_; // variable -> property it holds
};

} // namespace

Scan scan(const std::vector<uint8_t>& bytes)
{
    Scan out;
    try {
        Walker w(bytes, out);
        w.run();
        w.analyse();
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
