#include "EnhancedLibrary.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

// File format (one module per file):
// { "module": "ECM", "request": "7E0", "response": "7E8",
//   "dids": [ { "did": "1940", "name": "...", "unit": "°C", "startByte": 0, "bytes": 1,
//               "signed": false, "factor": 1, "offset": -40, "min": -20, "max": 120 } ] }

bool EnhancedDid::decode(const QByteArray &data, double *value) const {
    if (bytes < 1 || bytes > 4 || startByte < 0 || startByte + bytes > data.size()) return false;
    quint32 raw = 0;
    for (int i = 0; i < bytes; ++i) raw = (raw << 8) | quint8(data[startByte + i]);
    qint64 v = raw;
    if (isSigned && (raw >> (bytes * 8 - 1)) & 1u) v -= qint64(1) << (bytes * 8);
    *value = double(v) * factor + offset;
    return true;
}

EnhancedLibrary::EnhancedLibrary(QObject *parent) : QObject(parent) {}

QString EnhancedLibrary::userFilePath() {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/enhanced_user.json";
}

void EnhancedLibrary::load() {
    m_dids.clear();
    const QDir dir(QCoreApplication::applicationDirPath() + "/library/enhanced");
    for (const QString &name : dir.entryList({"*.json"}, QDir::Files, QDir::Name))
        loadFile(dir.filePath(name));
    loadFile(userFilePath());
    emit changed();
}

void EnhancedLibrary::loadFile(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return;
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    const QString module = root.value("module").toString("ECM");
    const quint32 req = root.value("request").toString("7E0").toUInt(nullptr, 16);
    const quint32 resp = root.value("response").toString("7E8").toUInt(nullptr, 16);
    for (const QJsonValue &v : root.value("dids").toArray()) {
        const QJsonObject o = v.toObject();
        EnhancedDid d;
        d.module = o.value("module").toString(module);
        d.requestId = o.contains("request") ? o.value("request").toString().toUInt(nullptr, 16) : req;
        d.responseId = o.contains("response") ? o.value("response").toString().toUInt(nullptr, 16) : resp;
        d.did = quint16(o.value("did").toString().toUInt(nullptr, 16));
        d.name = o.value("name").toString();
        d.unit = o.value("unit").toString();
        d.startByte = o.value("startByte").toInt(0);
        d.bytes = o.value("bytes").toInt(1);
        d.isSigned = o.value("signed").toBool(false);
        d.factor = o.value("factor").toDouble(1.0);
        d.offset = o.value("offset").toDouble(0.0);
        d.rangeMin = o.value("min").toDouble(0.0);
        d.rangeMax = o.value("max").toDouble(0.0);
        d.source = path;
        if (!d.name.isEmpty()) m_dids.append(d);
    }
}

bool EnhancedLibrary::addUserDid(const EnhancedDid &d, QString *error) {
    const QString path = userFilePath();
    QDir().mkpath(QFileInfo(path).absolutePath());

    QJsonObject root;
    QFile f(path);
    if (f.open(QIODevice::ReadOnly)) {
        root = QJsonDocument::fromJson(f.readAll()).object();
        f.close();
    }
    root["module"] = root.value("module").toString("ECM");
    QJsonArray arr = root.value("dids").toArray();
    QJsonObject o;
    o["module"] = d.module;
    o["request"] = QString::number(d.requestId, 16).toUpper();
    o["response"] = QString::number(d.responseId, 16).toUpper();
    o["did"] = QString("%1").arg(d.did, 4, 16, QChar('0')).toUpper();
    o["name"] = d.name;
    o["unit"] = d.unit;
    o["startByte"] = d.startByte;
    o["bytes"] = d.bytes;
    o["signed"] = d.isSigned;
    o["factor"] = d.factor;
    o["offset"] = d.offset;
    o["min"] = d.rangeMin;
    o["max"] = d.rangeMax;
    arr.append(o);
    root["dids"] = arr;

    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) *error = f.errorString();
        return false;
    }
    f.write(QJsonDocument(root).toJson());
    f.close();
    load();
    return true;
}
