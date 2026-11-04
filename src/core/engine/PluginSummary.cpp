#include "engine/PluginSummary.hpp"

#include "engine/Session.hpp"

namespace fo4mx {

void PluginTypeCounts::add(std::string_view type)
{
    ++total;
    if (type == pluginKindName(PluginKind::ESL))
        ++esl;
    else if (type == pluginKindName(PluginKind::LightESP))
        ++eslFlaggedEsp;
    else if (type == pluginKindName(PluginKind::ESP))
        ++esp;
    else if (type == pluginKindName(PluginKind::ESM))
        ++esm;
    else if (type == pluginKindName(PluginKind::LightESM)) {
        ++esm;
        ++eslFlaggedEsm;
    } else
        ++other;
}

void PluginSummary::add(std::string_view type, std::string_view eligibility, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        ++total;
        if (eligibility == eligibilityName(Eligibility::Eligible)) {
            eligible.add(type);
            continue;
        }
        ineligible.add(type);
        if (eligibility == eligibilityName(Eligibility::Ineligible))
            ++notEligibleIneligible;
        else if (eligibility == eligibilityName(Eligibility::Retained))
            ++retained;
        else if (eligibility == eligibilityName(Eligibility::Inactive))
            ++inactive;
        else if (eligibility == eligibilityName(Eligibility::Error))
            ++error;
        else
            ++otherStatus;
    }
}

PluginSummary summarisePlugins(const Session& session)
{
    PluginSummary s;
    for (const auto& p : session.plugins)
        s.add(pluginKindName(p.kind), eligibilityName(p.eligibility));
    return s;
}

} // namespace fo4mx
