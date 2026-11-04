#include "DiagLog.h"
#include <QDate>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QStandardPaths>
#include <QTime>

QString DiagLog::path(const QString &name) {
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/logs/" + name + "-" +
           QDate::currentDate().toString("yyyy-MM-dd") + ".log";
}

void DiagLog::write(const QString &name, const QString &line) {
    static QHash<QString, QFile *> files;
    const QString p = path(name);
    QFile *&f = files[p];
    if (!f) {
        QDir().mkpath(QFileInfo(p).path());
        f = new QFile(p);
        if (!f->open(QIODevice::Append | QIODevice::Text)) return;
    }
    if (!f->isOpen()) return;
    f->write((QTime::currentTime().toString("HH:mm:ss.zzz") + "  " + line + "\n").toUtf8());
    f->flush();
}
