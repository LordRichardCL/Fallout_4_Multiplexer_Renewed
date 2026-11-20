#include "Format.hpp"

#include "util/Text.hpp"

namespace gui {

QString toQString(const std::filesystem::path& p)
{
    return qs(fo4mx::pathToUtf8(p));
}

std::filesystem::path toPath(const QString& s)
{
    return fo4mx::utf8ToPath(s.toStdString());
}

QString plainStatus(const QString& eligibility, const QString& reason, bool baseGame)
{
    if (eligibility == "Eligible")
        return "Will compile";
    if (eligibility == "Inactive")
        return "Not enabled";
    if (eligibility == "Error")
        return "Error";
    if (reason.startsWith("Excluded by user") || reason.contains("Not chosen for compilation"))
        return "Kept by you";
    if (baseGame)
        return "Base game";
    if (reason.contains("Redirected (Master Redirection)"))
        return "Stays, redirected";
    return "Stays in load order";
}

QString plainStatusSql()
{
    return "CASE WHEN eligibility = 'Eligible' THEN 'Will compile' WHEN eligibility = 'Inactive' THEN 'Not enabled' "
           "WHEN eligibility = 'Error' THEN 'Error' WHEN eligibility_reason LIKE 'Excluded by user%' OR "
           "eligibility_reason LIKE '%Not chosen for compilation%' THEN 'Kept by you' WHEN base_game = 1 THEN "
           "'Base game' WHEN eligibility_reason LIKE '%Redirected (Master Redirection)%' THEN 'Stays, redirected' "
           "ELSE 'Stays in load order' END";
}

QString formatCount(qint64 v)
{
    return qs(fo4mx::formatCount(static_cast<uint64_t>(std::max<qint64>(0, v))));
}

QString formatBytes(qint64 v)
{
    return qs(fo4mx::formatBytes(static_cast<uint64_t>(std::max<qint64>(0, v))));
}

} // namespace gui
