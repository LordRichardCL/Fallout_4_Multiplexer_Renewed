#pragma once

#include "AppController.hpp"

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
