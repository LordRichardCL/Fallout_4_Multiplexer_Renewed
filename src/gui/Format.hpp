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
QString formatBytes(qint64 v);

} // namespace gui
