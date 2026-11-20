#include "widgets/QuickSearch.hpp"

#include "AppController.hpp"
#include "Theme.hpp"
#include "widgets/Widgets.hpp"

#include <QDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QSet>
#include <QSqlQuery>
#include <QTimer>
#include <QVBoxLayout>

namespace gui {

namespace {
constexpr int kKind = Qt::UserRole;
constexpr int kValue = Qt::UserRole + 1;
} // namespace

void showQuickSearch(QWidget* parent, AppController* app, const std::vector<std::pair<QString, QString>>& pages,
                     const std::function<void(const QuickSearchTarget&)>& open)
{
    QDialog dlg(parent);
    dlg.setWindowTitle("Go to");
    dlg.resize(620, 460);
    auto* l = new QVBoxLayout(&dlg);
    auto* field = new QLineEdit;
    field->setPlaceholderText("Type a page, plugin or record name (Editor ID)…");
    field->setClearButtonEnabled(true);
    auto* list = new QListWidget;
    auto* hint = new QLabel("Enter opens the first result; arrows choose another. Esc closes.");
    hint->setObjectName("OptionDescription");
    l->addWidget(field);
    l->addWidget(list, 1);
    l->addWidget(hint);

    auto add = [&](const QString& icon, const QString& text, const QString& sub, const QString& kind, const QString& value) {
        auto* it = new QListWidgetItem(lineIcon(icon, theme::kText, 16), sub.isEmpty() ? text : text + "    " + sub);
        it->setData(kKind, kind);
        it->setData(kValue, value);
        list->addItem(it);
    };
    auto fill = [&] {
        list->clear();
        const QString text = field->text().trimmed();
        for (const auto& [label, page] : pages)
            if (text.isEmpty() || label.contains(text, Qt::CaseInsensitive))
                add("dashboard", label, "page", "page", page);
        if (text.size() >= 2) {
            QSqlDatabase db = app->readDb();
            if (db.isOpen()) {
                const QString like = "%" + text + "%";
                QSqlQuery q(db);
                q.prepare("SELECT name, type FROM Plugins WHERE name LIKE ? ORDER BY CASE WHEN load_order < 0 THEN "
                          "99999 ELSE load_order END LIMIT 20");
                q.addBindValue(like);
                if (q.exec())
                    while (q.next())
                        add("plugins", q.value(0).toString(), q.value(1).toString() + " plugin", "plugin", q.value(0).toString());
                QSet<QString> seen;
                q.prepare("SELECT DISTINCT editor_id, signature, form_id FROM Conflicts WHERE editor_id LIKE ? LIMIT 15");
                q.addBindValue(like);
                if (q.exec())
                    while (q.next()) {
                        seen.insert(q.value(0).toString());
                        add("conflicts", q.value(0).toString(), q.value(1).toString() + " " + q.value(2).toString() +
                                                                    " · conflict", "conflict", q.value(0).toString());
                    }
                q.prepare("SELECT DISTINCT editor_id, signature, form_id FROM Records WHERE editor_id LIKE ? LIMIT 25");
                q.addBindValue(like);
                if (q.exec())
                    while (q.next())
                        if (!seen.contains(q.value(0).toString())) {
                            seen.insert(q.value(0).toString());
                            add("records", q.value(0).toString(), q.value(1).toString() + " " + q.value(2).toString() +
                                                                      " · record", "record", q.value(0).toString());
                        }
            }
        }
        if (list->count())
            list->setCurrentRow(0);
    };
    QTimer debounce;
    debounce.setSingleShot(true);
    debounce.setInterval(200);
    QObject::connect(&debounce, &QTimer::timeout, &dlg, fill);
    QObject::connect(field, &QLineEdit::textChanged, &dlg, [&debounce] { debounce.start(); });
    QuickSearchTarget picked;
    auto choose = [&](QListWidgetItem* it) {
        if (!it)
            return;
        picked = {it->data(kKind).toString(), it->data(kValue).toString()};
        dlg.accept();
    };
    QObject::connect(list, &QListWidget::itemActivated, &dlg, choose);
    QObject::connect(field, &QLineEdit::returnPressed, &dlg, [&] {
        // Typed faster than the debounce: the list still shows the previous text's results.
        if (debounce.isActive() || list->count() == 0) {
            debounce.stop();
            fill();
        }
        choose(list->currentItem());
    });
    // Up and down in the field move through the results.
    struct Keys : QObject {
        QListWidget* list;
        bool eventFilter(QObject* o, QEvent* e) override
        {
            if (e->type() == QEvent::KeyPress) {
                const int k = static_cast<QKeyEvent*>(e)->key();
                if (k == Qt::Key_Down || k == Qt::Key_Up) {
                    const int row = list->currentRow() + (k == Qt::Key_Down ? 1 : -1);
                    if (row >= 0 && row < list->count())
                        list->setCurrentRow(row);
                    return true;
                }
            }
            return QObject::eventFilter(o, e);
        }
    } keys;
    keys.list = list;
    field->installEventFilter(&keys);
    fill();
    field->setFocus();
    if (dlg.exec() == QDialog::Accepted && !picked.kind.isEmpty())
        open(picked);
}

} // namespace gui
