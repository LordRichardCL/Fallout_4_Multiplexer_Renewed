#pragma once

#include "AppController.hpp"
#include "widgets/Widgets.hpp"

#include <QPushButton>
#include <QSqlQuery>
#include <QVariant>
#include <QWidget>

namespace gui {

class Page : public QWidget {
    Q_OBJECT
public:
    explicit Page(AppController* app, QWidget* parent = nullptr) : QWidget(parent), app_(app) {}
    // Reload everything shown from the database / session.
    virtual void refresh() = 0;

signals:
    void navigate(const QString& page);

protected:
    AppController* app_;

    // A "Save Settings" button for a page with options: saves the whole settings state (the same as
    // the Settings page's button) and shows when there are unsaved changes.
    QPushButton* makeSaveSettingsButton()
    {
        auto* b = makeButton("Save Settings", "Primary");
        auto show = [b](bool unsaved) {
            b->setText(unsaved ? "Save Settings \u2022" : "Save Settings");
            b->setToolTip(unsaved ? "Some settings have changed and are not saved yet. Changes are kept while you "
                                    "move between pages; saving keeps them after a restart."
                                  : "All settings are saved.");
        };
        show(app_->hasUnsavedSettings());
        connect(app_, &AppController::unsavedSettingsChanged, b, show);
        connect(b, &QPushButton::clicked, app_, &AppController::saveSettings);
        return b;
    }

    QVariant scalar(const QString& sql, const QVariantList& binds = {})
    {
        QSqlDatabase db = app_->readDb();
        if (!db.isOpen())
            return {};
        QSqlQuery q(db);
        q.prepare(sql);
        for (int i = 0; i < binds.size(); ++i)
            q.bindValue(i, binds[i]);
        if (q.exec() && q.next())
            return q.value(0);
        return {};
    }

    QString meta(const QString& key) { return scalar("SELECT value FROM Meta WHERE key = ?", {key}).toString(); }
};

} // namespace gui
