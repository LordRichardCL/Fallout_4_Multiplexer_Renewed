#include "widgets/PluginSelection.hpp"

#include "AppController.hpp"
#include "Format.hpp"
#include "Theme.hpp"
#include "widgets/Widgets.hpp"

#include "esp/RecordDefinitions.hpp"

#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QSqlQuery>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace gui {

namespace {
constexpr const char* kNotChosen = "Not chosen for compilation";
constexpr const char* kExcluded = "Excluded by user";

// The record types in a summary such as "WEAP 12, KYWD 3".
QStringList typesOf(const QString& summary)
{
    QStringList out;
    static const QRegularExpression re("(?:^|, )([A-Z0-9_]{4}) \\d+");
    auto it = re.globalMatch(summary);
    while (it.hasNext())
        out << it.next().captured(1);
    return out;
}
} // namespace

PluginSelection::PluginSelection(AppController* app, QWidget* parent) : QFrame(parent), app_(app)
{
    setObjectName("Card");
    auto* l = new QVBoxLayout(this);
    l->setContentsMargins(16, 14, 16, 14);
    l->setSpacing(10);

    auto* head = new QHBoxLayout;
    auto* icon = new QLabel;
    icon->setPixmap(linePixmap("plugins", theme::kAccent, 18));
    head->addWidget(icon);
    head->addWidget(makeSectionTitle(
        "Plugins to compile",
        "Which plugins go into the outputs. \"All eligible\": every plugin that can be compiled is, unless you switch "
        "it off (the same as excluding it on the Plugins page). \"Only the ones I choose\": nothing is compiled "
        "until you switch it on, so new mods are never compiled by surprise; use it to compile only some mods, "
        "such as weapons or armour.\n\nA plugin that stays in the load order keeps its masters there too, so "
        "switching one off can stop others from compiling; the list shows that at once. The list fills after Scan "
        "and Analyse. Changes are saved at once and used by the next Compile."),
        1);
    l->addLayout(head);
    mode_ = new SegmentedControl({kAll, kChosen});
    mode_->button(kAll)->setToolTip("Every plugin that can be compiled is, unless you switch it off");
    mode_->button(kChosen)->setToolTip("Nothing is compiled until you switch it on");
    auto* modeRow = new QHBoxLayout;
    modeRow->addWidget(mode_);
    modeRow->addStretch(1);
    l->addLayout(modeRow);

    summary_ = new QLabel;
    summary_->setObjectName("OptionDescription");
    summary_->setWordWrap(true);
    l->addWidget(summary_);

    auto* filters = new QHBoxLayout;
    auto* actions = new QHBoxLayout;
    search_ = new QLineEdit;
    search_->setMinimumWidth(140);
    search_->setPlaceholderText("Search plugins…");
    search_->setClearButtonEnabled(true);
    show_ = new QComboBox;
    show_->addItems({"Every plugin", "Will compile", "Can compile, switched off", "Cannot compile"});
    type_ = new QComboBox;
    type_->setToolTip("Only plugins that contain this record type (for example WEAP for weapons)");
    type_->setMinimumWidth(180);
    on_ = makeButton("Switch on shown", nullptr, "check");
    on_->setToolTip("Switch on every plugin the filters show");
    off_ = makeButton("Switch off shown", nullptr, "block");
    off_->setToolTip("Switch off every plugin the filters show");
    filters->addWidget(search_, 1);
    filters->addWidget(show_);
    l->addLayout(filters);
    actions->addWidget(type_, 1);
    actions->addWidget(on_);
    actions->addWidget(off_);
    l->addLayout(actions);

    table_ = new QTableWidget;
    table_->setColumnCount(5);
    table_->setHorizontalHeaderLabels({"Compile", "Plugin", "Status", "Records", "Record types"});
    table_->verticalHeader()->hide();
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setAlternatingRowColors(true);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Interactive);
    table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Interactive);
    table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->setColumnWidth(1, 170);
    table_->setColumnWidth(2, 170);
    l->addWidget(table_);
    // The selected plugin's whole status: the table cuts long reasons short.
    detail_ = new QLabel;
    detail_->setObjectName("OptionDescription");
    detail_->setWordWrap(true);
    detail_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    detail_->hide();
    l->addWidget(detail_);
    connect(table_, &QTableWidget::currentCellChanged, this, [this](int row) {
        auto* name = row >= 0 ? table_->item(row, 1) : nullptr;
        auto* status = row >= 0 ? table_->item(row, 2) : nullptr;
        detail_->setVisible(name && status);
        if (name && status)
            detail_->setText("<b>" + name->text().toHtmlEscaped() + ":</b> " + status->toolTip().toHtmlEscaped());
    });
    empty_ = new QLabel("Run Scan and Analyse to list the plugins.");
    empty_->setObjectName("OptionDescription");
    l->addWidget(empty_);

    connect(mode_, &SegmentedControl::picked, this, [this](const QString& m) {
        const bool only = m == kChosen;
        if (only && app_->chosenPlugins().isEmpty()) {
            // Start from what compiles today, so switching modes changes nothing until the user does.
            QStringList now;
            for (const auto& r : rows_)
                if (r.eligibility == "Eligible")
                    now << r.name;
            app_->setChosen(now, true);
        }
        app_->setOnlySelected(only);
    });
    connect(search_, &QLineEdit::textChanged, this, [this] { fillTable(); });
    connect(show_, &QComboBox::currentIndexChanged, this, [this] { fillTable(); });
    connect(type_, &QComboBox::currentIndexChanged, this, [this] { fillTable(); });
    connect(on_, &QPushButton::clicked, this, [this] { setShown(true); });
    connect(off_, &QPushButton::clicked, this, [this] { setShown(false); });
    connect(table_, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
        if (filling_ || item->column() != 0)
            return;
        const QString name = item->data(Qt::UserRole).toString();
        const bool on = item->checkState() == Qt::Checked;
        if (app_->onlySelected())
            app_->setChosen({name}, on);
        else
            app_->setExcludedMany({name}, !on);
    });
    connect(app_, &AppController::dataChanged, this, [this] { refresh(); });
    connect(app_, &AppController::settingsChanged, this, [this] { mode_->setCurrent(app_->onlySelected() ? kChosen : kAll); });
}

void PluginSelection::refresh()
{
    mode_->setCurrent(app_->onlySelected() ? kChosen : kAll);
    reload();
    fillTable();
}

void PluginSelection::reload()
{
    rows_.clear();
    QSqlDatabase db = app_->readDb();
    if (!db.isOpen())
        return;
    QSqlQuery q(db);
    // Plugins that could be compiled: active, not the base game, not an output.
    q.exec("SELECT name, eligibility, eligibility_reason, record_count, record_types FROM Plugins WHERE active = 1 "
           "AND base_game = 0 AND eligibility <> 'Inactive' ORDER BY CASE WHEN load_order < 0 THEN 99999 ELSE "
           "load_order END, name");
    while (q.next())
        rows_.push_back({q.value(0).toString(), q.value(1).toString(), q.value(2).toString(), q.value(4).toString(),
                         q.value(3).toLongLong()});
    // The record types found, most plugins first, for the type filter.
    QMap<QString, int> count;
    for (const auto& r : rows_)
        for (const auto& t : typesOf(r.types))
            ++count[t];
    std::vector<std::pair<int, QString>> types;
    for (auto it = count.begin(); it != count.end(); ++it)
        types.push_back({it.value(), it.key()});
    std::sort(types.begin(), types.end(), [](const auto& a, const auto& b) { return a.first != b.first ? a.first > b.first : a.second < b.second; });
    const QString current = type_->currentData().toString();
    filling_ = true;
    type_->clear();
    type_->addItem("Any record type", QString());
    const auto& defs = fo4mx::RecordDefinitions::get();
    for (const auto& [n, t] : types) {
        const std::string name = defs.recordName(fo4mx::Signature::fromString(t.toStdString()));
        type_->addItem(QString("%1 %2 (%3)").arg(t, name.empty() ? QString() : "– " + QString::fromStdString(name)).arg(n), t);
    }
    if (const int i = type_->findData(current); i >= 0)
        type_->setCurrentIndex(i);
    filling_ = false;
}

bool PluginSelection::switchedOn(const Row& r) const
{
    if (app_->onlySelected())
        return app_->chosenPlugins().contains(r.name, Qt::CaseInsensitive);
    return !app_->excludedPlugins().contains(r.name, Qt::CaseInsensitive);
}

void PluginSelection::fillTable()
{
    filling_ = true;
    table_->setRowCount(0);
    if (detail_)
        detail_->hide();
    const QString text = search_->text().trimmed();
    const QString type = type_->currentData().toString();
    const int show = show_->currentIndex();
    size_t compiling = 0, switchedOff = 0, cannot = 0;
    for (const auto& r : rows_) {
        const bool on = switchedOn(r);
        const bool will = r.eligibility == "Eligible";
        // "Can compile" = only the user's choice keeps it out.
        const bool choiceOnly = !will && (r.reason.startsWith(kNotChosen) || r.reason.startsWith(kExcluded));
        compiling += will;
        switchedOff += choiceOnly;
        cannot += !will && !choiceOnly;
        if (!text.isEmpty() && !r.name.contains(text, Qt::CaseInsensitive))
            continue;
        if (!type.isEmpty() && !typesOf(r.types).contains(type))
            continue;
        if ((show == 1 && !will) || (show == 2 && !choiceOnly) || (show == 3 && (will || choiceOnly)))
            continue;
        const int row = table_->rowCount();
        table_->insertRow(row);
        auto* box = new QTableWidgetItem;
        box->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
        box->setCheckState(on ? Qt::Checked : Qt::Unchecked);
        box->setData(Qt::UserRole, r.name);
        box->setToolTip(on ? "Switched on" : "Switched off");
        table_->setItem(row, 0, box);
        table_->setItem(row, 1, new QTableWidgetItem(r.name));
        QString status = will ? QString("Will compile") : choiceOnly ? QString("Switched off") : r.reason.section("; ", 0, 0);
        auto* st = new QTableWidgetItem(status);
        st->setToolTip(r.reason.isEmpty() ? status : r.reason);
        st->setForeground(will ? theme::kGreen : choiceOnly ? theme::kMuted : theme::kYellow);
        table_->setItem(row, 2, st);
        auto* n = new QTableWidgetItem(formatCount(r.records));
        n->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        table_->setItem(row, 3, n);
        auto* t = new QTableWidgetItem(r.types);
        t->setToolTip(r.types);
        table_->setItem(row, 4, t);
    }
    filling_ = false;
    // As tall as its rows (3 to 12; it scrolls beyond), so the options below stay in reach.
    {
        const int shown = std::clamp(table_->rowCount(), 3, 12);
        const int rowHeight = table_->verticalHeader()->defaultSectionSize();
        table_->setFixedHeight(table_->horizontalHeader()->sizeHint().height() + shown * rowHeight +
                               2 * table_->frameWidth() + 2);
    }
    const bool have = !rows_.empty();
    table_->setVisible(have);
    empty_->setVisible(!have);
    on_->setEnabled(have);
    off_->setEnabled(have);
    summary_->setText(!have ? QString()
                            : QString("%1 will compile · %2 switched off · %3 cannot compile (select a plugin for why)%4")
                                  .arg(formatCount(qint64(compiling)), formatCount(qint64(switchedOff)),
                                       formatCount(qint64(cannot)),
                                       app_->onlySelected() ? QString(" · only the plugins you switch on are compiled")
                                                            : QString()));
}

QStringList PluginSelection::shownNames() const
{
    QStringList out;
    for (int r = 0; r < table_->rowCount(); ++r)
        out << table_->item(r, 0)->data(Qt::UserRole).toString();
    return out;
}

void PluginSelection::setShown(bool on)
{
    const QStringList names = shownNames();
    if (names.isEmpty())
        return;
    if (app_->onlySelected())
        app_->setChosen(names, on);
    else
        app_->setExcludedMany(names, !on);
}

} // namespace gui
