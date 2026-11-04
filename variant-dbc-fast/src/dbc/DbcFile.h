#pragma once
#include <QString>
#include <QList>
#include <QHash>
#include <QMap>
#include <QCanBusFrame>

// Minimal DBC (Vector CANdb++) reader: messages, signals, comments, value tables.
// Multiplexed signals are skipped; everything else decodes per the DBC rules.
struct DbcSignal {
    QString name;
    int startBit = 0;
    int length = 0;
    bool littleEndian = true;   // @1 = Intel, @0 = Motorola
    bool isSigned = false;
    double factor = 1.0;
    double offset = 0.0;
    double min = 0.0;
    double max = 0.0;
    QString unit;
    QString comment;
    QMap<qint64, QString> values;  // VAL_ enumerations (raw -> text)

    // Decode from a payload; returns false if the frame is too short.
    bool decode(const QByteArray &payload, double *physical, qint64 *raw = nullptr) const;
    QString format(double physical, qint64 raw) const;  // value text incl. enum names
};

struct DbcMessage {
    quint32 id = 0;          // without the extended flag bit
    bool extended = false;
    QString name;
    int dlc = 8;
    QString sender;
    QString comment;
    QList<DbcSignal> signalList;
};

class DbcFile {
public:
    bool load(const QString &path, QString *error = nullptr);
    bool isEmpty() const { return m_messages.isEmpty(); }
    QString path() const { return m_path; }

    const QList<DbcMessage> &messages() const { return m_messages; }
    const DbcMessage *message(quint32 id) const;

    // Write a DBC the loader above can read back (used by the in-app signal builder).
    static bool write(const QString &path, const QList<DbcMessage> &messages, QString *error = nullptr);

private:
    QString m_path;
    QList<DbcMessage> m_messages;
    QHash<quint32, int> m_index;
};
