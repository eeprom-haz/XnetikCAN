#pragma once
#include <QObject>
#include <QString>
#include <QList>

// One manufacturer-specific data item read with UDS service 0x22 (ReadDataByIdentifier).
struct EnhancedDid {
    QString module = "ECM";
    quint32 requestId = 0x7E0;
    quint32 responseId = 0x7E8;
    quint16 did = 0;
    QString name;
    QString unit;
    int startByte = 0;       // offset into the data that follows "62 <did hi> <did lo>"
    int bytes = 1;           // 1..4, big-endian
    bool isSigned = false;
    double factor = 1.0;
    double offset = 0.0;
    double rangeMin = 0.0;   // display range; equal = auto-scale
    double rangeMax = 0.0;
    QString source;          // file it came from

    bool decode(const QByteArray &data, double *value) const;
};

// Loads <exe>/library/enhanced/*.json (shipped/shared) and the user's own file
// in %APPDATA%, which is where "Add to library" saves.
class EnhancedLibrary : public QObject {
    Q_OBJECT
public:
    explicit EnhancedLibrary(QObject *parent = nullptr);

    void load();
    const QList<EnhancedDid> &dids() const { return m_dids; }
    bool addUserDid(const EnhancedDid &d, QString *error = nullptr);
    static QString userFilePath();

signals:
    void changed();

private:
    void loadFile(const QString &path);
    QList<EnhancedDid> m_dids;
};
