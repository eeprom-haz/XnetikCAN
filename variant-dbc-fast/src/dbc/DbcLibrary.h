#pragma once
#include "DbcFile.h"
#include <QObject>
#include <QStringList>

// All DBC files: <exe dir>/library/dbc/*.dbc plus any the user loads. CAN IDs mean
// different things on different makes, so a library file only applies when the chosen
// vehicle's make matches its file name prefix (gm_, ford_, ...). User-loaded files always apply.
class DbcLibrary : public QObject {
    Q_OBJECT
public:
    explicit DbcLibrary(QObject *parent = nullptr);

    void loadDefaults();                               // library folder + remembered files
    bool addFile(const QString &path, QString *error = nullptr);
    bool reloadFile(const QString &path, QString *error = nullptr);   // re-read a user file after editing
    void setVehicleMake(const QString &make);          // "" = no vehicle chosen: only user files apply
    QString activeFileNames() const;                   // for the status line, e.g. "ford_cgea1_2_ptcan_2011.dbc"

    // First active file that defines it wins. With dlc >= 0 the definition's length must match,
    // so a same-numbered message from another model generation isn't mis-decoded.
    const DbcMessage *message(quint32 id, int dlc = -1) const;
    QString fileOf(quint32 id, int dlc = -1) const;    // file name the message above comes from
    // A library file whose values contradict the vehicle's own (standard OBD) values: off until
    // the next vehicle / connection.
    void rejectFile(const QString &fileName, const QString &why);
    QSet<quint32> knownIds() const;
    // Bus traffic seen so far (ID -> data length). Files whose messages don't match it are
    // switched off, and the best-matching file is tried first.
    void matchTraffic(const QHash<quint32, int> &idLengths);
    void clearTrafficMatch();

    // "EngineCoolantTemp" -> "Engine Coolant Temp", "ECMEngineStatus" -> "ECM Engine Status"
    static QString humanize(const QString &identifier);
    // One-line decoded summary for the frame view, e.g. "Engine RPM 812 · Engine TPS 0 %"
    QString summary(const QCanBusFrame &f) const;
    // Library file name -> the makes it may be used for ({"?"} = never applied automatically).
    static QStringList makesForFile(const QString &fileName);

signals:
    void changed();

private:
    struct Entry {
        DbcFile file;
        QStringList makes;      // empty = applies to every vehicle (user-loaded)
        bool trafficOk = false; // true once this vehicle's bus traffic clearly matches the file
        int trafficScore = 0;   // messages seen on the bus with the right length
        bool rejected = false;  // decoded values contradicted standard OBD values
    };
    bool isActive(const Entry &e) const;
    QList<const Entry *> activeInOrder() const;

    QList<Entry> m_files;
    QString m_make;
};
