#include "engine/Session.hpp"

#include "util/Text.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>

#include <spdlog/spdlog.h>

namespace fo4mx {

const char* pluginKindName(PluginKind k)
{
    switch (k) {
    case PluginKind::ESM: return "ESM";
    case PluginKind::ESP: return "ESP";
    case PluginKind::ESL: return "ESL";
    case PluginKind::LightESP: return "ESL Flagged ESP";
    case PluginKind::LightESM: return "ESL Flagged ESM";
    }
    return "ESP";
}

const char* eligibilityName(Eligibility e)
{
    switch (e) {
    case Eligibility::Eligible: return "Eligible";
    case Eligibility::Retained: return "Retained";
    case Eligibility::Ineligible: return "Ineligible";
    case Eligibility::Inactive: return "Inactive";
    case Eligibility::Error: return "Error";
    }
    return "Unknown";
}

bool PluginInfo::isMaster() const
{
    return header.isMasterFlagged() || iendsWith(name, ".esm") || iendsWith(name, ".esl");
}

bool PluginInfo::isLight() const
{
    return header.isLightFlagged() || iendsWith(name, ".esl");
}

std::string PluginInfo::baseName() const
{
    auto dot = name.find_last_of('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

Session::Session(const RecordDefinitions& d) : defs(d) {}

PluginInfo* Session::findPlugin(std::string_view name)
{
    auto it = byKey_.find(toLower(name));
    return it == byKey_.end() ? nullptr : &plugins[it->second];
}

const PluginInfo* Session::findPlugin(std::string_view name) const
{
    auto it = byKey_.find(toLower(name));
    return it == byKey_.end() ? nullptr : &plugins[it->second];
}

PluginIndex Session::addPlugin(PluginInfo info)
{
    info.index = static_cast<PluginIndex>(plugins.size());
    info.key = toLower(info.name);
    byKey_[info.key] = info.index;
    plugins.push_back(std::move(info));
    return plugins.back().index;
}

void Session::rebuildPluginIndex()
{
    byKey_.clear();
    for (auto& p : plugins)
        byKey_[p.key] = p.index;
}

const FormGroup* Session::findGroup(FormKey key) const
{
    auto it = std::lower_bound(groups.begin(), groups.end(), key, [this](const FormGroup& g, FormKey k) {
        return records[g.begin].key < k;
    });
    if (it == groups.end() || records[it->begin].key != key)
        return nullptr;
    return &*it;
}

bool Session::exists(FormKey key) const
{
    if (!key.valid())
        return false;
    if (key.objectId < 0x800 && key.origin < plugins.size() && plugins[key.origin].key == "fallout4.esm")
        return true;
    return findGroup(key) != nullptr;
}

bool Session::dependsOn(PluginIndex plugin, PluginIndex master) const
{
    if (plugin >= plugins.size() || master >= plugins.size() || plugin == master)
        return false;
    if (masterClosure.size() == plugins.size())
        return masterClosure[plugin][master];
    std::vector<PluginIndex> stack{plugin};
    std::vector<bool> seen(plugins.size(), false);
    while (!stack.empty()) {
        PluginIndex p = stack.back();
        stack.pop_back();
        for (PluginIndex m : plugins[p].masters) {
            if (m == master)
                return true;
            if (m < plugins.size() && !seen[m]) {
                seen[m] = true;
                stack.push_back(m);
            }
        }
    }
    return false;
}

void Session::computeMasterClosure()
{
    masterClosure.clear();
    std::vector<std::vector<bool>> closure(plugins.size());
    for (PluginIndex p = 0; p < plugins.size(); ++p) {
        auto& reach = closure[p];
        reach.assign(plugins.size(), false);
        std::vector<PluginIndex> stack{p};
        while (!stack.empty()) {
            PluginIndex q = stack.back();
            stack.pop_back();
            for (PluginIndex m : plugins[q].masters)
                if (m < plugins.size() && m != p && !reach[m]) {
                    reach[m] = true;
                    stack.push_back(m);
                }
        }
    }
    masterClosure = std::move(closure);
}

std::vector<size_t> Session::overrideParents(std::span<const RecordInstance> chain) const
{
    std::vector<size_t> parents(chain.size(), 0);
    for (size_t c = 2; c < chain.size(); ++c)
        for (size_t j = c - 1; j > 0; --j)
            if (dependsOn(chain[c].plugin, chain[j].plugin)) {
                parents[c] = j;
                break;
            }
    return parents;
}

const RecordInstance* Session::winner(FormKey key) const
{
    auto g = findGroup(key);
    return g ? &records[g->end - 1] : nullptr;
}

FormKey Session::resolve(const PluginInfo& p, uint32_t raw) const
{
    uint32_t mi = masterIndexOf(raw);
    FormKey k;
    k.objectId = objectIdOf(raw);
    if (mi < p.masters.size())
        k.origin = p.masters[mi];
    else
        k.origin = p.index;
    return k;
}

Record Session::loadRecord(const RecordInstance& inst)
{
    return reader_.read(plugins[inst.plugin].path, inst.offset);
}

const StringTables* Session::stringTables(PluginIndex plugin)
{
    std::lock_guard lock(stringsMutex_);
    auto it = stringTables_.find(plugin);
    if (it != stringTables_.end())
        return it->second.get();

    auto tables = std::make_unique<StringTables>();
    const auto& p = plugins[plugin];
    std::string lang = install.config.language.empty() ? "en" : install.config.language;
    for (auto kind : {StringFileKind::Strings, StringFileKind::DLStrings, StringFileKind::ILStrings}) {
        std::string path = normalizeAssetPath("strings\\" + p.baseName() + "_" + lang + "." + StringTables::extension(kind));
        try {
            auto bytes = readAsset(path);
            if (!bytes.empty())
                tables->load(kind, bytes);
        } catch (const std::exception& e) {
            spdlog::debug("No string table {} ({})", path, e.what());
        }
    }
    if (tables->empty() && p.header.isLocalized())
        spdlog::warn("{} is localised but no '{}' string tables were found", p.name, lang);
    auto* raw = tables.get();
    stringTables_[plugin] = std::move(tables);
    return raw;
}

std::vector<uint8_t> Session::readAsset(std::string_view normalizedPath, size_t maxBytes)
{
    if (auto e = assets.find(normalizedPath))
        return assets.read(*e, e->sources.size() - 1, vfs, maxBytes);
    if (auto f = vfs.find(normalizedPath)) {
        std::ifstream in(vfs.winnerPath(*f), std::ios::binary);
        std::vector<uint8_t> out((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (maxBytes && out.size() > maxBytes)
            out.resize(maxBytes);
        return out;
    }
    return {};
}

std::string Session::pluginName(PluginIndex p) const
{
    return p < plugins.size() ? plugins[p].name : std::string("<missing>");
}

std::string Session::editorIdOf(FormKey key) const
{
    auto g = findGroup(key);
    if (!g)
        return {};
    for (uint32_t i = g->end; i-- > g->begin;)
        if (records[i].editorId)
            return strings.get(records[i].editorId);
    return {};
}

std::string Session::loadOrderFormId(FormKey key) const
{
    if (!key.valid() || key.origin >= plugins.size())
        return "????????";
    const auto& p = plugins[key.origin];
    if (p.loadOrderHex.empty())
        return "??" + formatFormId(key.objectId).substr(2);
    char buf[16];
    if (p.isLight()) {
        unsigned light = 0;
        std::sscanf(p.loadOrderHex.c_str() + 3, "%x", &light);
        std::snprintf(buf, sizeof(buf), "FE%03X%03X", light & 0xFFF, key.objectId & 0xFFF);
    } else {
        unsigned idx = 0;
        std::sscanf(p.loadOrderHex.c_str(), "%x", &idx);
        std::snprintf(buf, sizeof(buf), "%02X%06X", idx & 0xFF, key.objectId & 0xFFFFFF);
    }
    return buf;
}

std::string Session::formLabel(FormKey key) const
{
    if (!key.valid())
        return "[unresolved " + formatFormId(key.objectId) + "]";
    std::string edid = editorIdOf(key);
    auto g = findGroup(key);
    std::string sig = g ? records[g->begin].signature.str() : std::string("????");
    std::string label = edid.empty() ? std::string() : edid + " ";
    label += "[" + sig + ":" + loadOrderFormId(key) + "]";
    if (!g)
        label += " (missing in " + pluginName(key.origin) + ")";
    return label;
}

} // namespace fo4mx
