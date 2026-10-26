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

QString formatCount(qint64 v)
{
    return qs(fo4mx::formatCount(static_cast<uint64_t>(std::max<qint64>(0, v))));
}

QString formatBytes(qint64 v)
{
    return qs(fo4mx::formatBytes(static_cast<uint64_t>(std::max<qint64>(0, v))));
}

} // namespace gui
