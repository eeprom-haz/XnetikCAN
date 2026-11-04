#pragma once
#include <QObject>
#include <QProcess>
#include <QCanBusFrame>
#include <QByteArray>

// Hosts a 32-bit J2534 DLL out-of-process via J2534Bridge.exe (see BridgeProtocol.h).
class BridgeDevice : public QObject {
    Q_OBJECT
public:
    explicit BridgeDevice(QObject *parent = nullptr);
    ~BridgeDevice() override;

    bool open(const QString &dllPath, quint32 bitrate, QString *errorMsg = nullptr);
    void close();
    bool isOpen() const { return m_open; }
    bool writeFrame(const QCanBusFrame &frame);

    // 32 or 64 from the DLL's PE header, 0 if unreadable.
    static int dllBitness(const QString &dllPath);
    static QString bridgeExePath();

signals:
    void framesReceived(const QList<QCanBusFrame> &frames);
    void errorOccurred(const QString &msg);
    void closed();
    void batteryVoltage(double volts);   // OBD pin 16, from the adapter (J2534 READ_VBATT)

private slots:
    void onReadyRead();
    void onFinished();

private:
    void send(quint8 type, const QByteArray &payload = {});

    QProcess *m_proc = nullptr;
    QByteArray m_inbox;
    bool m_open = false;
    bool m_haveResult = false;
    quint32 m_resultCode = 0;
    QString m_resultText;
};
