#pragma once
#include <QObject>
#include <QCanBusFrame>
#include <QList>
#include <QString>
#include <atomic>
#include <thread>

// Raw-CAN source for the DSD SH-C31G (candleLight / gs_usb over Microsoft's built-in WinUSB).
// Proven path: see scratchpad/c31probe. Listen-only by default (silent, no ACK) so it can sniff a
// live vehicle bus without disturbing it — the thing J2534 cannot do. A worker thread reads frames
// and emits them batched to the UI thread (queued). Error frames come through as ErrorFrame.
class GsUsbDevice : public QObject {
    Q_OBJECT
public:
    explicit GsUsbDevice(QObject *parent = nullptr);
    ~GsUsbDevice() override;

    // nominalBitrate = arbitration speed (500k HS, 125k/95.2k MS, …). dataBitrate > 0 ⇒ CAN FD
    // (that's the faster data-phase rate, e.g. 2M); 0 ⇒ classic CAN. listenOnly = silent (vehicle);
    // false = Active: the adapter joins the bus and can transmit / ACK.
    bool open(quint32 nominalBitrate, quint32 dataBitrate, bool listenOnly, QString *errorMsg = nullptr);
    void close();
    bool isOpen() const { return m_open; }
    bool isFd() const { return m_fd; }
    bool canSend() const { return m_open && !m_listenOnly; }   // Active mode only

    bool writeFrame(const QCanBusFrame &frame);   // no-op in Listen mode
    static bool present();                         // is an SH-C31G plugged in (on WinUSB)?

    QString firmware() const { return m_firmware; }

signals:
    void framesReceived(const QList<QCanBusFrame> &frames);
    void errorOccurred(const QString &msg);
    void closed();

private:
    void readLoop();     // worker thread
    void stopThread();

    void *m_winusb = nullptr;   // WINUSB_INTERFACE_HANDLE
    void *m_handle = nullptr;   // HANDLE (CreateFile)
    unsigned char m_epIn = 0, m_epOut = 0;
    bool m_open = false;
    bool m_listenOnly = true;
    bool m_fd = false;
    int m_frameLen = 20;   // bytes per gs_host_frame on the wire (classic 20, FD 76)
    std::atomic<bool> m_run{false};
    std::thread m_thread;
    QString m_firmware;
};
