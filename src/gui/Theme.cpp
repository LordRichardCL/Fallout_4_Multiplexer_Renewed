#include "Theme.hpp"

#include <QApplication>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPixmap>
#include <QStyleFactory>

namespace gui::theme {

namespace {

const char* kStyleSheet = R"QSS(
QWidget { color: #d7e4dc; font-size: 10pt; }
QMainWindow, QDialog { background: #0f1311; }
QToolTip { background: #1c2420; color: #d7e4dc; border: 1px solid #2a3530; padding: 4px; }

#NavPanel { background: #121815; border-right: 1px solid #2a3530; }
#NavTitle { color: #3ddc84; font-size: 13pt; font-weight: 600; padding: 18px 16px 2px 16px; }
#NavSubtitle { color: #8aa196; font-size: 8pt; padding: 0 16px 14px 16px; }
#NavList { background: transparent; border: none; outline: none; }
#NavList::item { padding: 10px 16px; border-left: 3px solid transparent; color: #a9bcb2; }
#NavList::item:hover { background: #18201c; color: #d7e4dc; }
#NavList::item:selected { background: #1a2a21; color: #3ddc84; border-left: 3px solid #3ddc84; }

#PageTitle { font-size: 16pt; font-weight: 600; color: #e4efe8; }
#PageSubtitle { color: #8aa196; }
#SectionTitle { font-size: 11pt; font-weight: 600; color: #cfe0d6; padding-top: 6px; }

#Card { background: #161c19; border: 1px solid #26302b; border-radius: 10px; }
#CardValue { font-size: 20pt; font-weight: 600; color: #3ddc84; }
#CardLabel { color: #8aa196; font-size: 8pt; letter-spacing: 1px; }
#CardHint { color: #6f857a; font-size: 8pt; }

QPushButton { background: #1c2420; border: 1px solid #2f3b35; border-radius: 6px; padding: 7px 16px; }
QPushButton:hover { background: #22302a; border-color: #3ddc84; }
QPushButton:pressed { background: #1a2a21; }
QPushButton:disabled { color: #5a6a62; border-color: #222b27; background: #151b18; }
QPushButton#Primary { background: #1f6e45; border-color: #3ddc84; color: #eafff2; font-weight: 600; }
QPushButton#Primary:hover { background: #26844f; }
QPushButton#Danger { border-color: #ff5c5c; color: #ffb3b3; }
QPushButton#Chip { border-radius: 12px; padding: 4px 12px; background: #161c19; }
QPushButton#Chip:checked { background: #1a2a21; border-color: #3ddc84; color: #3ddc84; }

QLineEdit, QSpinBox, QComboBox, QPlainTextEdit, QTextEdit, QTextBrowser {
  background: #121815; border: 1px solid #2a3530; border-radius: 6px; padding: 5px 8px;
  selection-background-color: #1f6e45; }
QLineEdit:focus, QSpinBox:focus, QComboBox:focus { border-color: #3ddc84; }
QComboBox QAbstractItemView { background: #161c19; border: 1px solid #2a3530; selection-background-color: #1f6e45; }
QCheckBox, QRadioButton { spacing: 8px; }
QCheckBox::indicator, QRadioButton::indicator { width: 14px; height: 14px; border: 1px solid #4f6359; background: #121815; }
QCheckBox::indicator { border-radius: 3px; }
QRadioButton::indicator { border-radius: 8px; }
QCheckBox::indicator:hover, QRadioButton::indicator:hover { border-color: #3ddc84; }
QCheckBox::indicator:checked, QRadioButton::indicator:checked { background: #3ddc84; border-color: #3ddc84; }
QCheckBox::indicator:disabled, QRadioButton::indicator:disabled { border-color: #2a3530; }

QTableView, QTreeView, QListView {
  background: #131916; alternate-background-color: #161d19; border: 1px solid #26302b; border-radius: 8px;
  gridline-color: #1f2824; selection-background-color: #1f4a33; selection-color: #ffffff; }
QHeaderView::section { background: #1a221e; color: #8aa196; border: none; border-right: 1px solid #26302b;
  border-bottom: 1px solid #26302b; padding: 6px 8px; font-weight: 600; }
QTableCornerButton::section { background: #1a221e; border: none; }

QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; }
QScrollBar::handle:vertical { background: #2a3530; border-radius: 4px; min-height: 30px; }
QScrollBar::handle:vertical:hover { background: #3ddc84; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 2px; }
QScrollBar::handle:horizontal { background: #2a3530; border-radius: 4px; min-width: 30px; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }

QTabWidget::pane { border: 1px solid #26302b; border-radius: 8px; top: -1px; }
QTabBar::tab { background: transparent; padding: 8px 16px; color: #8aa196; border-bottom: 2px solid transparent; }
QTabBar::tab:selected { color: #3ddc84; border-bottom: 2px solid #3ddc84; }
QTabBar::tab:hover { color: #d7e4dc; }

QGroupBox { border: 1px solid #26302b; border-radius: 8px; margin-top: 14px; padding: 12px; }
QGroupBox::title { subcontrol-origin: margin; left: 12px; padding: 0 4px; color: #8aa196; }

QProgressBar { background: #121815; border: 1px solid #2a3530; border-radius: 5px; height: 12px; text-align: center; }
QProgressBar::chunk { background: #3ddc84; border-radius: 4px; }
QSplitter::handle { background: #1a221e; }
QStatusBar { background: #121815; border-top: 1px solid #26302b; color: #8aa196; }
QDockWidget::title { background: #161c19; padding: 6px; }
QMenu { background: #161c19; border: 1px solid #2a3530; }
QMenu::item:selected { background: #1f4a33; }
)QSS";

} // namespace

void apply(QApplication& app)
{
    app.setStyle(QStyleFactory::create("Fusion"));
    QPalette p;
    p.setColor(QPalette::Window, kBackground);
    p.setColor(QPalette::WindowText, kText);
    p.setColor(QPalette::Base, QColor(0x13, 0x19, 0x16));
    p.setColor(QPalette::AlternateBase, QColor(0x16, 0x1d, 0x19));
    p.setColor(QPalette::Text, kText);
    p.setColor(QPalette::Button, kPanelRaised);
    p.setColor(QPalette::ButtonText, kText);
    p.setColor(QPalette::Highlight, QColor(0x1f, 0x4a, 0x33));
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::ToolTipBase, kPanelRaised);
    p.setColor(QPalette::ToolTipText, kText);
    p.setColor(QPalette::PlaceholderText, kMuted);
    p.setColor(QPalette::Link, kAccent);
    p.setColor(QPalette::Disabled, QPalette::Text, QColor(0x5a, 0x6a, 0x62));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(0x5a, 0x6a, 0x62));
    app.setPalette(p);
    app.setStyleSheet(kStyleSheet);
    app.setWindowIcon(appIcon());
}

QIcon appIcon()
{
    QIcon icon;
    for (int size : {16, 24, 32, 48, 64, 128, 256}) {
        QPixmap pm(size, size);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        const double s = size;
        // Rounded square with a stylised "multiplexer" mark: three inputs merging into one.
        p.setBrush(QColor(0x12, 0x1a, 0x16));
        p.setPen(QPen(kAccent, s / 24.0));
        p.drawRoundedRect(QRectF(s * 0.06, s * 0.06, s * 0.88, s * 0.88), s * 0.18, s * 0.18);
        QPen line(kAccent, s / 14.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(line);
        QPointF out(s * 0.74, s * 0.5);
        for (double y : {0.3, 0.5, 0.7}) {
            QPainterPath path(QPointF(s * 0.24, s * y));
            path.cubicTo(QPointF(s * 0.46, s * y), QPointF(s * 0.48, s * 0.5), out);
            p.setBrush(Qt::NoBrush);
            p.drawPath(path);
        }
        p.setBrush(kAccent);
        p.setPen(Qt::NoPen);
        p.drawEllipse(out, s * 0.07, s * 0.07);
        icon.addPixmap(pm);
    }
    return icon;
}

QColor statusTint(const QString& status)
{
    if (status == "Winning" || status == "Eligible" || status == "OK" || status == "Passed")
        return QColor(0x1d, 0x4a, 0x31);
    if (status == "Replaced" || status == "Error" || status == "Critical" || status == "High" ||
        status == "Hard Conflicts" || status == "Missing Masters")
        return QColor(0x52, 0x1f, 0x22);
    if (status == "Merged" || status == "Warning" || status == "Medium" || status == "Conflicting")
        return QColor(0x4d, 0x40, 0x14);
    if (status == "Result")
        return QColor(0x17, 0x33, 0x4a);
    return {};
}

QColor statusText(const QString& status)
{
    if (status == "Winning" || status == "Eligible" || status == "OK")
        return kGreen;
    if (status == "Replaced" || status == "Error" || status == "Critical" || status == "High" ||
        status == "Hard Conflicts" || status == "Missing Masters" || status == "Ineligible")
        return kRed;
    if (status == "Merged" || status == "Warning" || status == "Medium" || status == "Conflicting")
        return kYellow;
    if (status == "Info" || status == "Low" || status == "Retained" || status == "Result")
        return kBlue;
    if (status == "Inactive" || status == "Disabled")
        return kMuted;
    return {};
}

} // namespace gui::theme
