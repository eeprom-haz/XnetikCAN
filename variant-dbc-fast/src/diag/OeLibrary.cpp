#include "OeLibrary.h"
#include "../obd/ObdPids.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <cfloat>
#include <cmath>

// ── Signal decoding (mirrors OBDb .schemas python/can/signals.py) ────────────

bool OeSignal::decode(const QByteArray &input, double *value, QString *text) const {
    const int endBit = bix + len;
    if (len <= 0 || len > 64 || endBit > input.size() * 8) return false;
    QByteArray data = input;
    if (blsb && len > 8) {  // reverse only the bytes holding the value
        const int startByte = bix / 8;
        const int count = (len + 7) / 8;
        const int endByte = qMin(startByte + count, int(data.size()));
        std::reverse(data.begin() + startByte, data.begin() + endByte);
    }
    quint64 raw = 0;
    for (int i = bix; i < endBit; ++i) {
        const quint8 byte = quint8(data[i / 8]);
        if (byte & (1u << (7 - (i % 8)))) raw |= quint64(1) << (endBit - i - 1);
    }
    if (!enumMap.isEmpty()) {
        auto it = enumMap.constFind(qint64(raw));
        if (it == enumMap.constEnd()) return false;
        *value = double(raw);
        if (text) *text = it.value();
        return true;
    }
    qint64 v = qint64(raw);
    if (isSigned && len < 64 && (raw >> (len - 1)) & 1u) v -= qint64(1) << len;
    double x = double(v) * mul / div + add;
    if (max > min) x = qBound(min, x, max);
    if (hasNullMin && x <= nullMin + DBL_EPSILON) return false;
    if (hasNullMax && x >= nullMax - DBL_EPSILON) return false;
    *value = x;
    if (text) {
        if (unit == "offon") *text = x != 0 ? "On" : "Off";
        else if (unit == "noyes") *text = x != 0 ? "Yes" : "No";
        else if (unit == "hex") *text = QString("%1").arg(quint64(raw), (len + 3) / 4, 16, QChar('0')).toUpper();
        else text->clear();
    }
    return true;
}

QString OeSignal::displayUnit() const {
    static const QHash<QString, QString> units = {
        {"celsius", "°C"}, {"fahrenheit", "°F"}, {"percent", "%"}, {"volts", "V"}, {"millivolts", "mV"},
        {"amps", "A"}, {"milliamps", "mA"}, {"rpm", "rpm"}, {"kilometersPerHour", "km/h"},
        {"milesPerHour", "mph"}, {"kilometers", "km"}, {"miles", "mi"}, {"meters", "m"},
        {"millimeters", "mm"}, {"newtonMeters", "Nm"}, {"milliseconds", "ms"}, {"seconds", "s"},
        {"minutes", "min"}, {"hours", "h"}, {"bars", "bar"}, {"kilopascal", "kPa"}, {"pascal", "Pa"},
        {"psi", "psi"}, {"degrees", "°"}, {"liters", "L"}, {"milliliters", "mL"},
        {"ampereHours", "Ah"}, {"kilowattHours", "kWh"}, {"wattHours", "Wh"}, {"kilowatts", "kW"},
        {"watts", "W"}, {"gramsPerSecond", "g/s"}, {"litersPerHour", "L/h"}, {"ohms", "Ω"},
        {"hertz", "Hz"}, {"metersPerSecondSquared", "m/s²"}, {"gForce", "g"}, {"degreesPerSecond", "°/s"},
    };
    if (!enumMap.isEmpty() || unit == "offon" || unit == "noyes" ||
        unit == "scalar" || unit == "unknown" || unit == "hex" || unit == "normal")
        return {};
    return units.value(unit, unit);
}

// ── Commands ─────────────────────────────────────────────────────────────────

bool OeCommand::matchesYear(int year) const {
    if (year <= 0 || (yearFrom == 0 && yearTo == 0 && years.isEmpty())) return true;
    if (years.contains(year)) return true;
    if (yearFrom && yearTo) return year >= yearFrom && year <= yearTo;
    if (yearTo) return year <= yearTo;
    if (yearFrom) return year >= yearFrom;
    return false;
}

QByteArray OeCommand::request() const {
    QByteArray r;
    r.append(char(service));
    if (service == 0x22) r.append(char(pid >> 8));
    r.append(char(pid & 0xFF));
    return r;
}

// ── Library index ────────────────────────────────────────────────────────────

static const QStringList kMultiWordMakes = {"Alfa-Romeo", "Aston-Martin", "Land-Rover",
                                            "Mercedes-Benz", "Rolls-Royce"};

static QString makeKey(const QString &make) { return make.toLower().remove('-').remove(' '); }

static QString displayMake(const QString &key, const QString &fallback) {
    static const QHash<QString, QString> names = {
        {"landrover", "Land Rover"}, {"fiat", "Fiat"}, {"alfaromeo", "Alfa Romeo"},
        {"astonmartin", "Aston Martin"}, {"mercedesbenz", "Mercedes-Benz"}, {"rollsroyce", "Rolls-Royce"},
        {"vauxhallopel", "Vauxhall/Opel"}, {"nissaninfiniti", "Nissan/INFINITI"}, {"seat", "SEAT"},
    };
    return names.value(key, QString(fallback).replace('-', ' '));
}

void OeLibrary::scan() {
    m_root = QCoreApplication::applicationDirPath() + "/library/oe";
    m_byMake.clear();
    QHash<QString, QString> display;
    const QDir dir(m_root);
    for (const QString &folder : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        if (!QFile::exists(dir.filePath(folder + "/default.json"))) continue;
        QString make = folder, model;
        bool multi = false;
        for (const QString &mw : kMultiWordMakes) {
            if (folder.compare(mw, Qt::CaseInsensitive) == 0 || folder.startsWith(mw + "-", Qt::CaseInsensitive)) {
                make = folder.left(mw.size());
                model = folder.mid(mw.size() + 1);
                multi = true;
                break;
            }
        }
        if (!multi && folder.contains('-')) {
            make = folder.section('-', 0, 0);
            model = folder.section('-', 1);
        }
        const QString key = makeKey(make);
        if (!display.contains(key)) display.insert(key, displayMake(key, make));
        m_byMake[key].append({folder, QString(model).replace('-', ' ')});
    }
    m_makes.clear();
    m_keyOf.clear();
    for (auto it = display.cbegin(); it != display.cend(); ++it) {
        m_makes << it.value();
        m_keyOf.insert(it.value(), it.key());
    }
    m_makes.sort(Qt::CaseInsensitive);
    // Re-key by display name for lookups from the UI.
    QHash<QString, QList<Entry>> byDisplay;
    for (const QString &name : m_makes) byDisplay.insert(name, m_byMake.value(m_keyOf.value(name)));
    m_byMake = byDisplay;
}

// Brands that share engineering (same modules and diagnostic data). Only these are mixed.
QStringList OeLibrary::family(const QString &make) const {
    static const QList<QStringList> families = {
        {"nissan", "infiniti", "nissaninfiniti", "datsun"},
        {"ford", "lincoln", "mercury"},
        {"chevrolet", "gmc", "buick", "cadillac", "holden"},
        {"chrysler", "dodge", "jeep", "ram"},
        {"toyota", "lexus", "scion"},
        {"honda", "acura"},
        {"hyundai", "kia", "genesis"},
        {"volkswagen", "audi", "skoda", "seat", "cupra"},
        {"bmw", "mini"},
        {"peugeot", "citroen", "ds", "vauxhallopel"},
        {"renault", "dacia"},
    };
    const QString key = m_keyOf.value(make, makeKey(make));
    QStringList out{make};
    for (const QStringList &f : families) {
        if (!f.contains(key)) continue;
        for (const QString &k : f)
            for (auto it = m_keyOf.cbegin(); it != m_keyOf.cend(); ++it)
                if (it.value() == k && it.key() != make) out << it.key();
    }
    return out;
}

QStringList OeLibrary::models(const QString &make) const {
    QStringList out;
    for (const Entry &e : m_byMake.value(make))
        if (!e.model.isEmpty() && !out.contains(e.model)) out << e.model;
    out.sort(Qt::CaseInsensitive);
    return out;
}

QString OeLibrary::typeName(const QString &type) {
    static const QHash<QString, QString> names = {
        {"engine", "Engine"}, {"transmission", "Transmission"}, {"bodyControl", "Body control"},
        {"abs", "ABS / brakes"}, {"cluster", "Instrument cluster"}, {"hybridControl", "Hybrid control"},
        {"tpms", "Tire pressure"}, {"battery", "Battery"}, {"charger", "Charger"}, {"steering", "Steering"},
        {"hvac", "Climate (HVAC)"}, {"gateway", "Gateway"}, {"airbag", "Airbag"}, {"motor", "Motor"},
        {"telematics", "Telematics"}, {"vehicleControl", "Vehicle control"},
        {"driverAssistance", "Driver assistance"},
    };
    return names.value(type);
}

QString OeLibrary::moduleLabel(const QString &type, quint32 requestId) {
    const QString id = QString::number(requestId, 16).toUpper();
    const QString name = typeName(type);
    return name.isEmpty() ? QString("Module %1").arg(id) : QString("%1 (%2)").arg(name, id);
}

QString OeLibrary::knownModuleName(const QString &make, quint32 id) {
    if (id == 0x7E0) return "Engine";
    if (id == 0x7E1) return "Transmission";
    if (id == 0x7DF) return "Asked to all modules";
    const QString m = makeKey(make);
    if (m == "ford" || m == "lincoln" || m == "mercury") {
        switch (id) {
        case 0x726: return "Body (BCM)";
        case 0x730: return "Power steering (PSCM)";
        case 0x760: return "ABS";
        case 0x764: return "Cruise control (CCM)";
        case 0x7D0: return "SYNC (APIM)";
        default: break;
        }
    }
    return {};
}

QString OeModule::title() const {
    QString id = OeLibrary::idText(requestId, ext29);
    if (txEa >= 0) id += "." + QString::number(txEa, 16).rightJustified(2, '0').toUpper();
    if (!name.isEmpty()) return QString("%1 (%2)").arg(name, id);
    if (topics.isEmpty()) return QString("Module %1").arg(id);
    QStringList t;
    for (const QString &s : topics.mid(0, 3)) t << s.toLower();
    return QString("Module %1 · %2").arg(id, t.join(", "));
}

QHash<quint32, QString> OeLibrary::moduleTypes(const QString &make) const {
    QHash<quint32, QString> out;
    for (const QString &m : family(make))
        for (const Entry &e : m_byMake.value(m)) {
            QFile f(m_root + "/" + e.folder + "/default.json");
            if (!f.open(QIODevice::ReadOnly)) continue;
            const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
            for (const QJsonValue &v : root.value("ecu").toArray()) {
                const QString hdr = v.toObject().value("hdr").toString();
                const QString type = v.toObject().value("type").toString();
                if ((hdr.size() != 3 && hdr.size() != 4) || type.isEmpty()) continue;
                // 29-bit "DA18" is listed with the default priority and tester (18 DA 18 F1).
                const quint32 id = hdr.size() == 3 ? hdr.toUInt(nullptr, 16)
                                                   : (0x18u << 24) | (hdr.toUInt(nullptr, 16) << 8) | 0xF1u;
                if (!out.contains(id)) out.insert(id, type);   // own make's files come first
            }
        }
    return out;
}

QList<OeModule> OeLibrary::modules(const QList<OeCommand> &commands, const QHash<quint32, QString> &types,
                                   const QString &make) {
    QMap<QString, OeModule> byId;   // module address (ID + extended-address byte)
    QHash<QString, QHash<QString, int>> topicCount;
    for (const OeCommand &c : commands) {
        if (!c.requestId) continue;
        const QString key = moduleId(c);
        OeModule &m = byId[key];
        m.requestId = c.requestId;
        m.responseId = c.responseId;
        m.ext29 = c.ext29;
        m.txEa = c.txEa;
        for (const OeSignal &s : c.signalList) {
            const QString head = s.path.section('.', 0, 0);
            if (!head.isEmpty()) ++topicCount[key][head];
        }
    }
    QList<OeModule> out;
    for (auto it = byId.begin(); it != byId.end(); ++it) {
        OeModule m = it.value();
        if (m.txEa < 0) m.type = types.value(m.requestId);
        m.name = m.requestId == 0x7DF ? knownModuleName(make, m.requestId) : typeName(m.type);
        if (m.name.isEmpty() && !m.ext29 && m.txEa < 0) m.name = knownModuleName(make, m.requestId);
        const QHash<QString, int> &tc = topicCount[it.key()];
        QList<QPair<int, QString>> sorted;
        for (auto it = tc.cbegin(); it != tc.cend(); ++it) sorted << qMakePair(-it.value(), it.key());
        std::sort(sorted.begin(), sorted.end());
        for (const auto &p : sorted) m.topics << p.second;
        out << m;
    }
    return out;
}

QString OeLibrary::valueKey(const OeCommand &c, const OeSignal &s) {
    return QString("%1|%2|%3|%4").arg(moduleId(c)).arg(c.responseId, 0, 16)
        .arg(QString(c.request().toHex()), s.name.trimmed().toLower());
}

QString OeLibrary::idText(quint32 id, bool ext29) {
    return QString::number(id, 16).rightJustified(ext29 ? 8 : 3, '0').toUpper();
}

QString OeLibrary::moduleId(const OeCommand &c) {
    QString s = idText(c.requestId, c.ext29);
    if (c.txEa >= 0) s += "." + QString::number(c.txEa, 16).rightJustified(2, '0').toUpper();
    return s;
}

QString OeLibrary::commandId(const OeCommand &c) {
    return moduleId(c) + ":" + QString(c.request().toHex()).toUpper();
}

bool OeLibrary::isStandardValue(const OeCommand &c) {
    const QByteArray r = c.request();
    return (c.requestId == 0x7E0 || c.requestId == 0x7DF) && r.size() == 3 && quint8(r[0]) == 0x22 &&
           quint8(r[1]) == 0xF4 && !findObdPid(quint8(r[2])).isEmpty();
}

static QString requestKey(const QString &hdr, const QString &svcPid, quint32 rax) {
    return hdr + "|" + svcPid + "|" + QString::number(rax, 16);
}

// requests: request keys already defined by a more specific file. Own files (borrowedFrom "")
// add to it; borrowed files skip any command whose request is already defined.
static void parseFile(const QString &path, QList<OeCommand> *out, QSet<QString> *seen,
                      QSet<QString> *requests, const QString &borrowedFrom) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return;
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();

    QHash<QString, QString> ecuType;  // hdr -> type
    for (const QJsonValue &e : root.value("ecu").toArray())
        ecuType.insert(e.toObject().value("hdr").toString().toUpper(), e.toObject().value("type").toString());

    for (const QJsonValue &cv : root.value("commands").toArray()) {
        const QJsonObject c = cv.toObject();
        const QString hdr = c.value("hdr").toString().toUpper();
        const QJsonObject cmd = c.value("cmd").toObject();
        if (cmd.isEmpty()) continue;
        OeCommand oc;
        const QString svc = cmd.keys().first();
        oc.service = quint8(svc.toUInt(nullptr, 16));
        oc.pid = quint16(cmd.value(svc).toString().toUInt(nullptr, 16));
        oc.freq = c.value("freq").toDouble(1);
        oc.moduleType = ecuType.value(hdr);
        // Addressing, as OBDb (and an ELM327 script) describes it:
        //  hdr "7E0"  11-bit request ID; rax = reply ID (default +8).
        //  hdr "DA15" 29-bit: request pri DA 15 tst (18 DA 15 F1); rax "15" = reply pri DA tst 15,
        //             rax of 6 digits = the reply ID below the priority byte.
        //  eax "07"   extended addressing: that byte in front of every request frame; replies carry
        //             tst (default F1) in front. BMW-style 6F1 replies come from 600 + target.
        const QString rax = c.value("rax").toString().toUpper();
        const quint32 tst = c.contains("tst") ? c.value("tst").toString().toUInt(nullptr, 16) : 0xF1;
        if (hdr.size() == 3) {
            oc.requestId = hdr.toUInt(nullptr, 16);
            if (c.contains("eax")) {
                oc.txEa = int(c.value("eax").toString().toUInt(nullptr, 16));
                oc.rxEa = int(tst);
            }
            if (!rax.isEmpty()) oc.responseId = rax.toUInt(nullptr, 16);
            else if (oc.txEa >= 0 && oc.requestId == 0x6F1) oc.responseId = 0x600 | quint32(oc.txEa);
            else oc.responseId = oc.requestId + 8;
        } else if (hdr.size() == 4) {
            const quint32 pri = c.contains("pri") ? c.value("pri").toString().toUInt(nullptr, 16) : 0x18;
            const quint32 h = hdr.toUInt(nullptr, 16);   // format byte (DA) + target
            oc.ext29 = true;
            oc.requestId = (pri << 24) | (h << 8) | tst;
            if (rax.size() == 6) oc.responseId = (pri << 24) | rax.toUInt(nullptr, 16);
            else oc.responseId = (pri << 24) | ((h >> 8) << 16) | (tst << 8) |
                                 (rax.isEmpty() ? (h & 0xFF) : rax.toUInt(nullptr, 16));
        } else {
            oc.supported = false;
            oc.unsupportedWhy = "Address format not understood (" + hdr + ")";
        }
        if (c.contains("tmo")) oc.timeoutMs = int(c.value("tmo").toString().toUInt(nullptr, 16)) * 4;   // ELM ATST units
        if (c.contains("din")) oc.sessionIn = quint8(c.value("din").toString().toUInt(nullptr, 16));
        if (c.contains("dout")) oc.sessionOut = quint8(c.value("dout").toString().toUInt(nullptr, 16));
        if (c.contains("proto") || c.contains("init") || c.contains("baud")) {
            oc.supported = false;
            oc.unsupportedWhy = "K-line protocol (not supported)";
        }
        if (oc.service != 0x22 && oc.service != 0x21 && oc.service != 0x01) {
            oc.supported = false;
            oc.unsupportedWhy = "Unsupported service";
        }
        const QJsonObject flt = c.value("filter").toObject();
        oc.yearFrom = flt.value("from").toInt(0);
        oc.yearTo = flt.value("to").toInt(0);
        for (const QJsonValue &y : flt.value("years").toArray()) oc.years.insert(y.toInt());
        // The same signal id can appear in several commands (e.g. per model-year layouts);
        // only an identical command (same request and year filter) counts as a repeat.
        const QString address = OeLibrary::moduleId(oc);   // ID + extended-address byte
        const QString cmdKey = address + "|" + svc + cmd.value(svc).toString().toUpper() + "|" +
                               QJsonDocument(flt).toJson(QJsonDocument::Compact);
        // A borrowed definition never replaces or doubles one from a more specific file.
        const QString rk = requestKey(address, svc + cmd.value(svc).toString().toUpper(), oc.responseId);
        if (!borrowedFrom.isEmpty() && requests->contains(rk)) continue;
        oc.borrowedFrom = borrowedFrom;

        for (const QJsonValue &sv : c.value("signals").toArray()) {
            const QJsonObject s = sv.toObject();
            const QJsonObject fmt = s.value("fmt").toObject();
            OeSignal sig;
            sig.id = s.value("id").toString();
            if (seen->contains(cmdKey + "|" + sig.id)) continue;  // model file may repeat make-level items
            sig.name = s.value("name").toString(sig.id);
            sig.path = s.value("path").toString();
            sig.description = s.value("description").toString();
            sig.bix = fmt.value("bix").toInt(0);
            sig.len = fmt.value("len").toInt(8);
            sig.blsb = fmt.value("blsb").toBool(false);
            sig.isSigned = fmt.value("sign").toBool(false);
            sig.min = fmt.value("min").toDouble(0);
            sig.max = fmt.value("max").toDouble(0);
            sig.mul = fmt.value("mul").toDouble(1);
            sig.div = fmt.value("div").toDouble(1);
            if (sig.div == 0) sig.div = 1;
            sig.add = fmt.value("add").toDouble(0);
            sig.hasNullMin = fmt.contains("nullmin");
            sig.nullMin = fmt.value("nullmin").toDouble();
            sig.hasNullMax = fmt.contains("nullmax");
            sig.nullMax = fmt.value("nullmax").toDouble();
            sig.unit = fmt.value("unit").toString();
            const QJsonObject map = fmt.value("map").toObject();
            for (auto it = map.begin(); it != map.end(); ++it) {
                const QString t = it.value().isObject() ? it.value().toObject().value("value").toString()
                                                        : it.value().toVariant().toString();
                sig.enumMap.insert(it.key().toLongLong(), t);
            }
            seen->insert(cmdKey + "|" + sig.id);
            oc.signalList.append(sig);
        }
        if (oc.signalList.isEmpty()) continue;
        requests->insert(rk);
        out->append(oc);
    }
}

QList<OeCommand> OeLibrary::load(const QString &make, const QString &model, QString *error) const {
    QList<OeCommand> out;
    QSet<QString> seen, requests;
    auto file = [this](const Entry &e) { return m_root + "/" + e.folder + "/default.json"; };
    auto label = [](const QString &mk, const Entry &e) { return e.model.isEmpty() ? mk : mk + " " + e.model; };
    const QList<Entry> entries = m_byMake.value(make);
    // 1. This vehicle's own files: the model file first (more specific), then the make file.
    for (const Entry &e : entries)
        if (!model.isEmpty() && e.model == model) parseFile(file(e), &out, &seen, &requests, {});
    for (const Entry &e : entries)
        if (e.model.isEmpty()) parseFile(file(e), &out, &seen, &requests, {});
    // 2. The make's other models, then sister brands (make file first, then their models).
    //    Check supported keeps only what this vehicle answers.
    for (const Entry &e : entries)
        if (!e.model.isEmpty() && e.model != model) parseFile(file(e), &out, &seen, &requests, label(make, e));
    const QStringList fam = family(make);
    for (int i = 1; i < fam.size(); ++i) {
        const QList<Entry> sister = m_byMake.value(fam[i]);
        for (const Entry &e : sister)
            if (e.model.isEmpty()) parseFile(file(e), &out, &seen, &requests, label(fam[i], e));
        for (const Entry &e : sister)
            if (!e.model.isEmpty()) parseFile(file(e), &out, &seen, &requests, label(fam[i], e));
    }
    if (out.isEmpty() && error) *error = "No manufacturer data found for " + make + " " + model;
    return out;
}
