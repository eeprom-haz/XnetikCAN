#include "DbcLibrary.h"
#include <QCoreApplication>
#include <QDir>
#include <QSettings>
#include <QSet>
#include <algorithm>

DbcLibrary::DbcLibrary(QObject *parent) : QObject(parent) {}

// Library file name prefix -> vehicle makes (as the OBDb make picker names them).
QStringList DbcLibrary::makesForFile(const QString &fileName) {
    static const QStringList gm = {"Chevrolet", "GMC", "Buick", "Cadillac", "Pontiac", "Saturn", "Hummer",
                                   "Oldsmobile", "Holden"};
    static const QStringList nissan = {"Nissan", "INFINITI", "Nissan/INFINITI", "Datsun"};
    static const QStringList toyota = {"Toyota", "Lexus", "Scion"};
    static const QStringList honda = {"Honda", "Acura"};
    static const QStringList vw = {"Volkswagen", "Audi", "Skoda", "SEAT", "Cupra"};
    static const QList<QPair<QString, QStringList>> prefixes = {
        {"gm_", gm},
        {"cadillac_", gm},
        {"ford_", {"Ford", "Lincoln", "Mercury"}},
        {"chrysler_", {"Chrysler", "Dodge", "Jeep", "Ram", "Fiat"}},
        {"tesla_", {"Tesla"}},
        {"nissan_", nissan},
        {"toyota_", toyota},
        {"honda_", honda},
        {"acura_", honda},
        {"hyundai_", {"Hyundai", "Kia", "Genesis"}},
        {"subaru_", {"Subaru"}},
        {"mazda_", {"Mazda"}},
        {"vw_", vw},
        {"bmw_", {"BMW", "MINI"}},
        {"mercedes_benz_", {"Mercedes-Benz"}},
        {"volvo_", {"Volvo"}},
        {"psa_", {"Peugeot", "Citroen", "DS"}},
        {"opel_", {"Vauxhall/Opel"}},
        {"fca_giorgio", {"Alfa Romeo"}},
        {"byd_", {"BYD"}},
        {"gwm_", {"Haval"}},
        {"mg.", {"MG"}},
        {"luxgen_", {"Luxgen"}},
        {"hongqi_", {"Hongqi"}},
        {"rivian_", {"Rivian"}},
    };
    const QString lower = fileName.toLower();
    for (const auto &p : prefixes)
        if (lower.startsWith(p.first)) return p.second;
    return {"?"};   // library file of unknown make: never applied automatically
}

void DbcLibrary::loadDefaults() {
    const QDir dir(QCoreApplication::applicationDirPath() + "/library/dbc");
    for (const QString &name : dir.entryList({"*.dbc"}, QDir::Files, QDir::Name)) {
        DbcFile f;
        if (f.load(dir.filePath(name))) m_files.append({f, makesForFile(name)});
    }
    for (const QString &path : QSettings().value("dbc/userFiles").toStringList())
        addFile(path);
}

bool DbcLibrary::addFile(const QString &path, QString *error) {
    for (const Entry &e : m_files)
        if (QFileInfo(e.file.path()) == QFileInfo(path)) return true;
    DbcFile f;
    if (!f.load(path, error)) return false;
    m_files.append({f, {}});
    emit changed();
    return true;
}

bool DbcLibrary::reloadFile(const QString &path, QString *error) {
    for (Entry &e : m_files) {
        if (QFileInfo(e.file.path()) == QFileInfo(path)) {
            DbcFile f;
            if (!f.load(path, error)) return false;
            e.file = f; e.makes = {}; e.trafficOk = false; e.trafficScore = 0; e.rejected = false;
            emit changed();
            return true;
        }
    }
    return addFile(path, error);
}

void DbcLibrary::setVehicleMake(const QString &make) {
    if (make == m_make) return;
    m_make = make;
    emit changed();
}

bool DbcLibrary::isActive(const Entry &e) const {
    if (e.makes.isEmpty()) return true;
    return !m_make.isEmpty() && e.makes.contains(m_make, Qt::CaseInsensitive) && e.trafficOk && !e.rejected;
}

QList<const DbcLibrary::Entry *> DbcLibrary::activeInOrder() const {
    QList<const Entry *> out;
    for (const Entry &e : m_files)
        if (isActive(e)) out << &e;
    // User files first, then the library file that best matches the bus.
    std::stable_sort(out.begin(), out.end(), [](const Entry *a, const Entry *b) {
        if (a->makes.isEmpty() != b->makes.isEmpty()) return a->makes.isEmpty();
        return a->trafficScore > b->trafficScore;
    });
    return out;
}

QString DbcLibrary::activeFileNames() const {
    QStringList names;
    for (const Entry *e : activeInOrder()) names << QFileInfo(e->file.path()).fileName();
    return names.join(", ");
}

const DbcMessage *DbcLibrary::message(quint32 id, int dlc) const {
    for (const Entry *e : activeInOrder())
        if (const DbcMessage *m = e->file.message(id))
            if (dlc < 0 || m->dlc == dlc) return m;
    return nullptr;
}

QString DbcLibrary::fileOf(quint32 id, int dlc) const {
    for (const Entry *e : activeInOrder())
        if (const DbcMessage *m = e->file.message(id))
            if (dlc < 0 || m->dlc == dlc) return QFileInfo(e->file.path()).fileName();
    return {};
}

void DbcLibrary::rejectFile(const QString &fileName, const QString &why) {
    Q_UNUSED(why)
    for (Entry &e : m_files)
        if (!e.makes.isEmpty() && QFileInfo(e.file.path()).fileName() == fileName && !e.rejected) {
            e.rejected = true;
            emit changed();
        }
}

QSet<quint32> DbcLibrary::knownIds() const {
    QSet<quint32> ids;
    for (const Entry *e : activeInOrder())
        for (const DbcMessage &m : e->file.messages()) ids.insert(m.id);
    return ids;
}

void DbcLibrary::matchTraffic(const QHash<quint32, int> &idLengths) {
    bool changedAny = false;
    for (Entry &e : m_files) {
        if (e.makes.isEmpty()) continue;   // the user chose this file: always use it
        int seen = 0, sameLength = 0;
        for (const DbcMessage &m : e.file.messages()) {
            auto it = idLengths.constFind(m.id);
            if (it == idLengths.constEnd()) continue;
            ++seen;
            sameLength += *it == m.dlc;
        }
        // Only trust a file the bus clearly matches: a fair share of its messages present, almost
        // all with the right length. A few IDs that happen to exist on another make prove nothing.
        const int total = int(e.file.messages().size());
        const bool ok = sameLength >= 4 && seen * 100 >= total * 15 && sameLength * 10 >= seen * 9;
        if (ok != e.trafficOk || sameLength != e.trafficScore) changedAny = true;
        e.trafficOk = ok;
        e.trafficScore = sameLength;
    }
    if (changedAny) emit changed();
}

void DbcLibrary::clearTrafficMatch() {
    bool changedAny = false;
    for (Entry &e : m_files) {
        changedAny |= e.trafficOk || e.trafficScore != 0 || e.rejected;
        e.trafficOk = false;   // no verdict until traffic is seen
        e.trafficScore = 0;
        e.rejected = false;
    }
    if (changedAny) emit changed();
}

QString DbcLibrary::humanize(const QString &s) {
    QString out;
    for (int i = 0; i < s.size(); ++i) {
        const QChar c = s[i];
        if (c == '_') { if (!out.endsWith(' ')) out += ' '; continue; }
        if (i > 0 && !out.endsWith(' ')) {
            const QChar p = s[i - 1];
            const bool nextLower = i + 1 < s.size() && s[i + 1].isLower();
            if ((c.isUpper() && (p.isLower() || p.isDigit() || (p.isUpper() && nextLower))) ||
                (c.isDigit() && p.isLetter()))
                out += ' ';
        }
        out += c;
    }
    return out.trimmed();
}

QString DbcLibrary::summary(const QCanBusFrame &f) const {
    const DbcMessage *m = message(f.frameId(), int(f.payload().size()));
    if (!m) return {};
    QStringList parts;
    for (const DbcSignal &s : m->signalList) {
        if (s.name.contains("Checksum", Qt::CaseInsensitive) ||
            s.name.contains("Counter", Qt::CaseInsensitive))
            continue;
        double v;
        qint64 raw;
        if (!s.decode(f.payload(), &v, &raw)) continue;
        QString text = humanize(s.name) + ' ' + s.format(v, raw);
        if (!s.unit.isEmpty() && !s.values.contains(raw)) text += ' ' + s.unit;
        parts << text;
    }
    return parts.join("  ·  ");
}
