#pragma once
#include <QString>
#include <QStringList>
#include <QList>
#include <QMap>
#include <QHash>
#include <QSet>
#include <QByteArray>

// Manufacturer (OE) data definitions from OBDb signalsets (<exe>/library/oe/<Repo>/*.json).
// Decoding follows OBDb's reference implementation (.schemas python/can/signals.py):
// bits are counted MSB-first from the first data byte after the service/PID echo.
struct OeSignal {
    QString id;
    QString name;
    QString path;            // "Engine", "Transmission", "Battery.Cells", ...
    QString description;
    int bix = 0;             // bit offset, MSB-first
    int len = 8;
    bool blsb = false;       // bytes little-endian
    bool isSigned = false;
    double min = 0, max = 0;
    double mul = 1, div = 1, add = 0;
    bool hasNullMin = false, hasNullMax = false;
    double nullMin = 0, nullMax = 0;
    QString unit;            // OBDb unit name, e.g. "celsius", "offon"
    QMap<qint64, QString> enumMap;  // fmt.map (value -> text)

    // false = not enough data or a "no data" sentinel
    bool decode(const QByteArray &data, double *value, QString *text) const;
    QString displayUnit() const;
};

struct OeCommand {
    quint32 requestId = 0;   // hdr: 11-bit ID, or the full 29-bit ID (pri DA target tester)
    quint32 responseId = 0;  // rax (11-bit default hdr + 8; 29-bit default pri DA tester target)
    bool ext29 = false;      // 29-bit addressing
    int txEa = -1;           // eax: address byte in front of every request frame (-1 = none)
    int rxEa = -1;           // tst: address byte in front of every reply frame (with eax)
    int timeoutMs = 0;       // tmo (0 = the poller's default)
    quint8 sessionIn = 0;    // din: 10 <din> before asking (0 = none), e.g. 03, C0 (Nissan)
    quint8 sessionOut = 0;   // dout: 10 <dout> to leave it (0 = default session)
    quint8 service = 0x22;   // 0x22, 0x21 or 0x01
    quint16 pid = 0;
    double freq = 1;         // OBDb suggested update period (s)
    int yearFrom = 0, yearTo = 0;   // filter (0 = open)
    QSet<int> years;
    bool supported = true;   // false: this adapter can't ask it (K-line, unknown service)
    QString unsupportedWhy;
    QString moduleType;      // from the file's "ecu" list: engine, transmission, bodyControl, ...
    QString borrowedFrom;    // "" = this vehicle's own model/make file, else e.g. "Nissan Maxima"
    QList<OeSignal> signalList;

    bool matchesYear(int year) const;
    QByteArray request() const;   // e.g. 22 40 05
};

// A module the library has data for, for the module scan and the per-module value lists.
struct OeModule {
    quint32 requestId = 0, responseId = 0;
    bool ext29 = false;
    int txEa = -1;           // extended-addressing target byte (-1 = none)
    QString type;            // library "ecu" type ("transmission"), "" if the files don't say
    QString name;            // plain name: "Transmission", "Body (BCM)" or "" (unknown)
    QStringList topics;      // what its values are about, most first: "Tires", "Battery", ...
    QString title() const;   // "Transmission (7E1)" or "Module 726 · tires, battery, doors"
};

class OeLibrary {
public:
    void scan();                                  // index makes/models (no parsing yet)
    QStringList makes() const { return m_makes; }
    QStringList models(const QString &make) const;  // display names, "" = all-models (make file)
    // Everything documented for this make family, most specific first: the model's own file, the
    // make file, then the make's other models and sister brands (Nissan/INFINITI, Ford/Lincoln, …).
    // A request already defined by a more specific file is never taken from a less specific one.
    // Never another make's data.
    QList<OeCommand> load(const QString &make, const QString &model, QString *error = nullptr) const;
    // Module types declared by the family's files ("ecu" lists), request ID -> type.
    QHash<quint32, QString> moduleTypes(const QString &make) const;
    static QString moduleLabel(const QString &type, quint32 requestId);
    static QString typeName(const QString &type);   // "transmission" -> "Transmission", "" if unknown
    // Names confirmed for a make's module addresses (not from the library files).
    static QString knownModuleName(const QString &make, quint32 requestId);
    // One row per value: the same module, request and name from several files is one value.
    static QString valueKey(const OeCommand &c, const OeSignal &s);
    // A standard OBD-II value read the UDS way (22 F4xx on the engine computer): it is already
    // in the Standard list, so it isn't listed again under manufacturer data.
    static bool isStandardValue(const OeCommand &c);
    // One string per module address (ID + extended-address byte), and per request on it.
    static QString moduleId(const OeCommand &c);
    static QString commandId(const OeCommand &c);
    static QString idText(quint32 id, bool ext29);   // "7E0" or "18DA15F1"
    // Modules in a command list, with names and topics.
    static QList<OeModule> modules(const QList<OeCommand> &commands, const QHash<quint32, QString> &types,
                                   const QString &make);

private:
    struct Entry { QString folder; QString model; };  // model "" = make-level folder
    QStringList family(const QString &make) const;     // display makes: this one first, then sisters
    QStringList m_makes;
    QHash<QString, QList<Entry>> m_byMake;
    QHash<QString, QString> m_keyOf;                    // display make -> key ("nissan")
    QString m_root;
};
