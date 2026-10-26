#include "pages/DependenciesPage.hpp"

#include "Theme.hpp"
#include "models/SqlModel.hpp"
#include "widgets/GraphView.hpp"
#include "widgets/Widgets.hpp"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QSet>
#include <QSpinBox>
#include <QSqlQuery>
#include <QTabWidget>
#include <QTableView>
#include <QVBoxLayout>

#include <deque>
#include <tuple>

namespace gui {

namespace {

QColor colourForEligibility(const QString& e)
{
    QColor c = theme::statusText(e);
    return c.isValid() ? c : theme::kLine;
}

QColor colourForSignature(const QString& s)
{
    static const QHash<QString, QColor> map = {
        {"WEAP", QColor(0xff, 0x9f, 0x43)}, {"ARMO", QColor(0x5c, 0xb8, 0xff)}, {"OMOD", QColor(0xb3, 0x8c, 0xff)},
        {"KYWD", QColor(0x3d, 0xdc, 0x84)}, {"FLST", QColor(0xf5, 0xc5, 0x42)}, {"COBJ", QColor(0xff, 0x6b, 0x9d)},
        {"LVLI", QColor(0x4e, 0xd8, 0xd8)}, {"AMMO", QColor(0xe0, 0x7a, 0x5f)}};
    return map.value(s, theme::kMuted);
}

} // namespace

DependenciesPage::DependenciesPage(AppController* app, QWidget* parent) : Page(app, parent)
{
    auto* l = new QVBoxLayout(this);
    l->setContentsMargins(28, 24, 28, 18);
    l->setSpacing(10);
    l->addWidget(makePageHeader("Dependency Graph",
                                "Trace master relationships and record references. Scroll to zoom, drag to pan, click a "
                                "node to highlight what it uses (green) and what uses it (yellow)."));
    tabs_ = new QTabWidget;
    l->addWidget(tabs_, 1);

    // Plugin graph
    auto* pluginTab = new QWidget;
    auto* pl = new QVBoxLayout(pluginTab);
    auto* pbar = new QHBoxLayout;
    pluginSearch_ = new QLineEdit;
    pluginSearch_->setPlaceholderText("Find plugin…");
    neighbourhoodOnly_ = new QCheckBox("Only the selected plugin's masters and dependents");
    auto* fitP = makeButton("Fit");
    pbar->addWidget(pluginSearch_, 1);
    pbar->addWidget(neighbourhoodOnly_);
    pbar->addWidget(fitP);
    pl->addLayout(pbar);
    pluginGraph_ = new GraphView;
    pl->addWidget(pluginGraph_, 1);
    tabs_->addTab(pluginTab, "Plugins");

    // Record graph
    auto* recordTab = new QWidget;
    auto* rl = new QVBoxLayout(recordTab);
    auto* rbar = new QHBoxLayout;
    recordSearch_ = new QLineEdit;
    recordSearch_->setPlaceholderText("Editor ID or load-order FormID, e.g. 10mmPistol or 0004822B");
    depth_ = new QSpinBox;
    depth_->setRange(1, 4);
    depth_->setValue(2);
    depth_->setPrefix("Depth ");
    auto* show = makeButton("Show", "Primary");
    auto* fitR = makeButton("Fit");
    rbar->addWidget(recordSearch_, 1);
    rbar->addWidget(depth_);
    rbar->addWidget(show);
    rbar->addWidget(fitR);
    rl->addLayout(rbar);
    recordInfo_ = new QLabel("Search for a weapon, armour, keyword, form list… to see what it references and what references it.");
    recordInfo_->setObjectName("PageSubtitle");
    rl->addWidget(recordInfo_);
    recordGraph_ = new GraphView;
    rl->addWidget(recordGraph_, 1);
    tabs_->addTab(recordTab, "Records");

    // Issues
    issues_ = new QTableView;
    styleTable(issues_);
    issuesModel_ = new SqlModel(this);
    issuesModel_->setStatusColumns({0});
    issues_->setModel(issuesModel_);
    tabs_->addTab(issues_, "Missing Requirements && Breakages");

    connect(pluginSearch_, &QLineEdit::returnPressed, this, [this] {
        if (neighbourhoodOnly_->isChecked()) {
            focusPlugin_ = pluginSearch_->text().trimmed();
            buildPluginGraph();
        } else {
            pluginGraph_->findAndSelect(pluginSearch_->text().trimmed());
        }
    });
    connect(neighbourhoodOnly_, &QCheckBox::toggled, this, [this] {
        focusPlugin_ = pluginSearch_->text().trimmed();
        buildPluginGraph();
    });
    connect(pluginGraph_, &GraphView::nodeActivated, this, [this](const QString& id) {
        pluginSearch_->setText(id);
        focusPlugin_ = id;
        neighbourhoodOnly_->setChecked(true);
        buildPluginGraph();
    });
    connect(fitP, &QPushButton::clicked, pluginGraph_, &GraphView::fit);
    connect(fitR, &QPushButton::clicked, recordGraph_, &GraphView::fit);
    connect(show, &QPushButton::clicked, this, [this] { buildRecordGraph(recordSearch_->text().trimmed()); });
    connect(recordSearch_, &QLineEdit::returnPressed, this, [this] { buildRecordGraph(recordSearch_->text().trimmed()); });
    connect(recordGraph_, &GraphView::nodeActivated, this, [this](const QString& id) {
        recordSearch_->clear();
        buildRecordGraph(id);
    });
}

void DependenciesPage::refresh()
{
    buildPluginGraph();
    issuesModel_->load(app_->readDb(),
                       "SELECT severity AS Severity, category AS Category, p.name AS Plugin, form_id AS FormID, "
                       "message AS Details FROM DependencyIssues d LEFT JOIN Plugins p ON p.id = d.plugin_id "
                       "ORDER BY CASE severity WHEN 'Error' THEN 0 WHEN 'Warning' THEN 1 ELSE 2 END LIMIT 100000");
    fitColumns(issues_, 640);
    tabs_->setTabText(2, QString("Missing Requirements && Breakages (%1)").arg(issuesModel_->rowCount()));
}

void DependenciesPage::buildPluginGraph()
{
    QSqlDatabase db = app_->readDb();
    if (!db.isOpen())
        return;
    std::vector<GraphView::Node> nodes;
    std::vector<GraphView::Edge> edges;
    QHash<qint64, QString> names;
    QHash<QString, QString> elig;
    QSqlQuery q(db);
    q.exec("SELECT id, name, eligibility, type, record_count FROM Plugins WHERE active = 1");
    QHash<QString, QString> detail;
    while (q.next()) {
        names[q.value(0).toLongLong()] = q.value(1).toString();
        elig[q.value(1).toString()] = q.value(2).toString();
        detail[q.value(1).toString()] = QString("%1 · %2 records · %3")
                                            .arg(q.value(3).toString())
                                            .arg(q.value(4).toLongLong())
                                            .arg(q.value(2).toString());
    }
    q.exec("SELECT m.plugin_id, m.master_plugin_id FROM Masters m JOIN Plugins p ON p.id = m.plugin_id "
           "WHERE p.active = 1 AND m.master_plugin_id IS NOT NULL");
    while (q.next()) {
        QString from = names.value(q.value(0).toLongLong()), to = names.value(q.value(1).toLongLong());
        if (!from.isEmpty() && !to.isEmpty())
            edges.push_back({from, to, from + " requires " + to});
    }

    QSet<QString> include;
    bool neighbourhood = neighbourhoodOnly_->isChecked() && !focusPlugin_.isEmpty();
    QString focus;
    if (neighbourhood) {
        for (auto it = names.begin(); it != names.end(); ++it)
            if (it.value().compare(focusPlugin_, Qt::CaseInsensitive) == 0 || (focus.isEmpty() && it.value().contains(focusPlugin_, Qt::CaseInsensitive)))
                focus = it.value();
        if (!focus.isEmpty()) {
            include.insert(focus);
            for (const auto& e : edges)
                if (e.from == focus || e.to == focus)
                    include.insert(e.from == focus ? e.to : e.from);
        }
    }
    for (auto it = elig.begin(); it != elig.end(); ++it) {
        if (neighbourhood && !include.contains(it.key()))
            continue;
        nodes.push_back({it.key(), it.key(), detail.value(it.key()), colourForEligibility(it.value())});
    }
    if (neighbourhood) {
        std::vector<GraphView::Edge> kept;
        for (const auto& e : edges)
            if (include.contains(e.from) && include.contains(e.to))
                kept.push_back(e);
        edges.swap(kept);
    }
    pluginGraph_->setGraph(std::move(nodes), std::move(edges), focus);
}

void DependenciesPage::buildRecordGraph(const QString& query)
{
    QSqlDatabase db = app_->readDb();
    if (!db.isOpen() || query.isEmpty())
        return;
    // Node ids: "origin|object|label"
    qint64 origin = -1, object = -1;
    QSqlQuery q(db);
    if (query.contains('|')) {
        origin = query.section('|', 0, 0).toLongLong();
        object = query.section('|', 1, 1).toLongLong();
    } else {
        q.prepare("SELECT origin_plugin_id, object_id FROM Forms WHERE editor_id = ? OR form_id = ? "
                  "UNION ALL SELECT origin_plugin_id, object_id FROM Forms WHERE editor_id LIKE ? LIMIT 1");
        q.addBindValue(query);
        q.addBindValue(query.toUpper());
        q.addBindValue("%" + query + "%");
        if (q.exec() && q.next()) {
            origin = q.value(0).toLongLong();
            object = q.value(1).toLongLong();
        }
    }
    if (origin < 0) {
        recordInfo_->setText("No record matches \"" + query + "\". Run Analyse to build the reference index.");
        return;
    }

    auto key = [](qint64 o, qint64 id) { return QString::number(o) + "|" + QString::number(id); };
    QHash<QString, std::pair<QString, QString>> info; // key -> label, signature
    auto describe = [&](qint64 o, qint64 id) {
        QString k = key(o, id);
        if (info.contains(k))
            return;
        QSqlQuery f(db);
        f.prepare("SELECT f.form_id, f.signature, f.editor_id, p.name FROM Forms f LEFT JOIN Plugins p "
                  "ON p.id = f.winner_plugin_id WHERE f.origin_plugin_id = ? AND f.object_id = ?");
        f.addBindValue(o);
        f.addBindValue(id);
        if (f.exec() && f.next()) {
            QString edid = f.value(2).toString();
            info[k] = {QString("%1 [%2]").arg(edid.isEmpty() ? f.value(0).toString() : edid, f.value(1).toString()),
                       f.value(1).toString() + " · " + f.value(0).toString() + " · " + f.value(3).toString()};
        } else {
            QSqlQuery p(db);
            p.prepare("SELECT name FROM Plugins WHERE id = ?");
            p.addBindValue(o);
            QString plugin = p.exec() && p.next() ? p.value(0).toString() : QString("?");
            info[k] = {QString("%1:%2").arg(plugin).arg(id, 6, 16, QChar('0')).toUpper(), "not indexed / missing"};
        }
    };

    std::vector<GraphView::Edge> edges;
    QSet<QString> visited;
    std::deque<std::tuple<qint64, qint64, int>> queue{{origin, object, 0}};
    visited.insert(key(origin, object));
    describe(origin, object);
    while (!queue.empty() && info.size() < 400) {
        auto [o, id, d] = queue.front();
        queue.pop_front();
        if (d >= depth_->value())
            continue;
        QSqlQuery out(db);
        out.prepare("SELECT to_origin, to_object, via FROM Dependencies WHERE from_origin = ? AND from_object = ?");
        out.addBindValue(o);
        out.addBindValue(id);
        out.exec();
        while (out.next()) {
            qint64 to = out.value(0).toLongLong(), toId = out.value(1).toLongLong();
            describe(to, toId);
            edges.push_back({key(o, id), key(to, toId), out.value(2).toString()});
            if (!visited.contains(key(to, toId))) {
                visited.insert(key(to, toId));
                queue.emplace_back(to, toId, d + 1);
            }
        }
        QSqlQuery in(db);
        in.prepare("SELECT from_origin, from_object, via FROM Dependencies WHERE to_origin = ? AND to_object = ? LIMIT 200");
        in.addBindValue(o);
        in.addBindValue(id);
        in.exec();
        while (in.next()) {
            qint64 from = in.value(0).toLongLong(), fromId = in.value(1).toLongLong();
            describe(from, fromId);
            edges.push_back({key(from, fromId), key(o, id), in.value(2).toString()});
            if (!visited.contains(key(from, fromId))) {
                visited.insert(key(from, fromId));
                queue.emplace_back(from, fromId, d + 1);
            }
        }
    }
    std::vector<GraphView::Node> nodes;
    for (auto it = info.begin(); it != info.end(); ++it) {
        QString sig = it.value().second.section(" · ", 0, 0);
        nodes.push_back({it.key(), it.value().first, it.value().second, colourForSignature(sig)});
    }
    QString focus = key(origin, object);
    recordInfo_->setText(QString("%1 — %2 related records, %3 references. Double-click a node to re-centre.")
                             .arg(info.value(focus).first)
                             .arg(nodes.size() - 1)
                             .arg(edges.size()));
    recordGraph_->setGraph(std::move(nodes), std::move(edges), focus);
}

} // namespace gui
