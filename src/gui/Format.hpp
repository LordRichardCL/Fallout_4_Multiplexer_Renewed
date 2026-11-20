#pragma once

#include <QString>

#include <filesystem>
#include <string>

namespace gui {

inline QString qs(const std::string& s)
{
    return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size()));
}

inline std::string ss(const QString& s)
{
    return s.toStdString();
}

QString toQString(const std::filesystem::path& p);
std::filesystem::path toPath(const QString& s);
QString formatCount(qint64 v);
// A plugin's compile status in plain words: Will compile, Stays in load order, Kept by you, Base game,
// Not enabled, Error (from Plugins.eligibility, eligibility_reason and base_game).
QString plainStatus(const QString& eligibility, const QString& reason, bool baseGame);
// The same as an SQL expression over the Plugins table's columns.
QString plainStatusSql();
QString formatBytes(qint64 v);

} // namespace gui
