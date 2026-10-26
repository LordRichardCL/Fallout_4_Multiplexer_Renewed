#pragma once

#include <QColor>
#include <QIcon>
#include <QString>

class QApplication;

namespace gui::theme {

// Palette
inline const QColor kBackground{0x0f, 0x13, 0x11};
inline const QColor kPanel{0x16, 0x1c, 0x19};
inline const QColor kPanelRaised{0x1c, 0x24, 0x20};
inline const QColor kLine{0x2a, 0x35, 0x30};
inline const QColor kText{0xd7, 0xe4, 0xdc};
inline const QColor kMuted{0x8a, 0xa1, 0x96};
inline const QColor kAccent{0x3d, 0xdc, 0x84};  // Fallout green
inline const QColor kAccentDim{0x1f, 0x6e, 0x45};
inline const QColor kGreen{0x3d, 0xdc, 0x84};
inline const QColor kRed{0xff, 0x5c, 0x5c};
inline const QColor kYellow{0xf5, 0xc5, 0x42};
inline const QColor kBlue{0x5c, 0xb8, 0xff};

void apply(QApplication& app);
QIcon appIcon();
// Background tint for a status keyword (Winning, Replaced, Error, ...); invalid colour when none.
QColor statusTint(const QString& status);
QColor statusText(const QString& status);

} // namespace gui::theme
