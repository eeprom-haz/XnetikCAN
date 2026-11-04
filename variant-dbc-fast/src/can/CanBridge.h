#pragma once
#include <QObject>
#include <QCanBusDevice>
#include <QCanBusFrame>
#include "RingBuffer.h"

class BridgeDevice;
class GsUsbDevice;

class CanBridge : public QObject {
    Q_OBJECT
public:
    explicit CanBridge(QObject *parent = nullptr);
    ~CanBridge() override;

    bool connectDevice(const QString &dllPath, QString *errorMsg = nullptr);
    // Raw CAN via the SH-C31G (gs_usb/WinUSB). dataBitrate > 0 ⇒ CAN FD; listenOnly = silent sniff.
    bool connectGsUsb(quint32 nominalBitrate, quint32 dataBitrate, bool listenOnly, QString *errorMsg = nullptr);
    void disconnectDevice();
    bool isConnected() const;

    void startCapture();
    void stopCapture();
    bool isCapturing() const { return m_capturing; }

    bool writeFrame(const QCanBusFrame &frame);

    RingBuffer *buffer() { return &m_buffer; }
    int totalFrames() const { return m_totalFrames; }
    int errorFrames() const { return m_errorFrames; }
    QString deviceName() const { return m_dllPath; }
    bool usingBridge() const { return m_proxy != nullptr; }
    bool usingGsUsb() const { return m_gsusb != nullptr; }

signals:
    void frameReceived(const QCanBusFrame &frame);   // only while capturing
    void liveFrame(const QCanBusFrame &frame);       // every frame, capture or not
    void stateChanged(bool connected);
    void busError(const QString &msg);
    void batteryVoltage(double volts);   // OBD pin 16 via the adapter, about once a second (bridge only)

private slots:
    void onFramesReceived();
    void onDeviceError(QCanBusDevice::CanBusError err);
    void onBridgeFrames(const QList<QCanBusFrame> &frames);

private:
    bool connectViaBridge(const QString &dllPath, QString *errorMsg);
    void handleFrame(const QCanBusFrame &f);

    QCanBusDevice *m_device = nullptr;
    BridgeDevice *m_proxy = nullptr;  // set when a 32-bit DLL is hosted out-of-process
    GsUsbDevice *m_gsusb = nullptr;   // set when sniffing raw CAN via the SH-C31G
    RingBuffer m_buffer;
    QString m_dllPath;
    bool m_capturing = false;
    int m_totalFrames = 0;
    int m_errorFrames = 0;
};
