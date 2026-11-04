#include "CanBridge.h"
#include "BridgeDevice.h"
#include "GsUsbDevice.h"
#include <QCanBus>
#include <QSysInfo>

// Qt's passthrucan plugin defaults to 500 kbit/s too.
static constexpr quint32 kDefaultBitrate = 500000;

CanBridge::CanBridge(QObject *parent) : QObject(parent) {}

CanBridge::~CanBridge() { disconnectDevice(); }

bool CanBridge::connectDevice(const QString &dllPath, QString *errorMsg) {
    if (m_device || m_proxy) disconnectDevice();

    // A 64-bit process cannot load a 32-bit DLL; host it in J2534Bridge.exe instead.
    if (QSysInfo::WordSize == 64 && BridgeDevice::dllBitness(dllPath) == 32)
        return connectViaBridge(dllPath, errorMsg);

    QString err;
    m_device = QCanBus::instance()->createDevice(
        QStringLiteral("passthrucan"), dllPath, &err);

    if (!m_device) {
        if (errorMsg) *errorMsg = err;
        return false;
    }

    connect(m_device, &QCanBusDevice::framesReceived,
            this, &CanBridge::onFramesReceived);
    connect(m_device, &QCanBusDevice::errorOccurred,
            this, &CanBridge::onDeviceError);

    if (!m_device->connectDevice()) {
        if (errorMsg) *errorMsg = m_device->errorString();
        delete m_device;
        m_device = nullptr;
        return false;
    }

    m_dllPath = dllPath;
    m_totalFrames = 0;
    m_errorFrames = 0;
    m_buffer.clear();
    emit stateChanged(true);
    return true;
}

bool CanBridge::connectViaBridge(const QString &dllPath, QString *errorMsg) {
    m_proxy = new BridgeDevice(this);
    connect(m_proxy, &BridgeDevice::framesReceived, this, &CanBridge::onBridgeFrames);
    connect(m_proxy, &BridgeDevice::errorOccurred, this, &CanBridge::busError);
    connect(m_proxy, &BridgeDevice::batteryVoltage, this, &CanBridge::batteryVoltage);
    connect(m_proxy, &BridgeDevice::closed, this, [this] { emit stateChanged(false); });

    if (!m_proxy->open(dllPath, kDefaultBitrate, errorMsg)) {
        delete m_proxy;
        m_proxy = nullptr;
        return false;
    }

    m_dllPath = dllPath;
    m_totalFrames = 0;
    m_errorFrames = 0;
    m_buffer.clear();
    emit stateChanged(true);
    return true;
}

bool CanBridge::connectGsUsb(quint32 nominalBitrate, quint32 dataBitrate, bool listenOnly, QString *errorMsg) {
    if (m_device || m_proxy || m_gsusb) disconnectDevice();
    m_gsusb = new GsUsbDevice(this);
    connect(m_gsusb, &GsUsbDevice::framesReceived, this, &CanBridge::onBridgeFrames);
    connect(m_gsusb, &GsUsbDevice::errorOccurred, this, &CanBridge::busError);
    connect(m_gsusb, &GsUsbDevice::closed, this, [this] { emit stateChanged(false); });
    if (!m_gsusb->open(nominalBitrate, dataBitrate, listenOnly, errorMsg)) {
        delete m_gsusb;
        m_gsusb = nullptr;
        return false;
    }
    m_dllPath = QStringLiteral("SH-C31G (%1%2)")
                    .arg(dataBitrate ? "CAN FD" : "classic", listenOnly ? ", listen-only" : ", active");
    m_totalFrames = 0;
    m_errorFrames = 0;
    m_buffer.clear();
    emit stateChanged(true);
    return true;
}

void CanBridge::disconnectDevice() {
    m_capturing = false;
    if (m_gsusb) {
        m_gsusb->close();   // emits closed() -> stateChanged(false) if it was open
        delete m_gsusb;
        m_gsusb = nullptr;
        return;
    }
    if (m_proxy) {
        m_proxy->close();  // emits closed() -> stateChanged(false) if it was open
        delete m_proxy;
        m_proxy = nullptr;
        return;
    }
    if (!m_device) return;
    m_device->disconnectDevice();
    delete m_device;
    m_device = nullptr;
    emit stateChanged(false);
}

bool CanBridge::isConnected() const {
    if (m_gsusb) return m_gsusb->isOpen();
    if (m_proxy) return m_proxy->isOpen();
    return m_device && m_device->state() == QCanBusDevice::ConnectedState;
}

void CanBridge::startCapture() {
    m_capturing = true;
    m_totalFrames = 0;
    m_errorFrames = 0;
    m_buffer.clear();
}

void CanBridge::stopCapture() {
    m_capturing = false;
}

bool CanBridge::writeFrame(const QCanBusFrame &frame) {
    if (!isConnected()) return false;
    if (m_gsusb) return m_gsusb->writeFrame(frame);
    if (m_proxy) return m_proxy->writeFrame(frame);
    return m_device->writeFrame(frame);
}

void CanBridge::handleFrame(const QCanBusFrame &f) {
    if (f.frameType() == QCanBusFrame::ErrorFrame) {
        ++m_errorFrames;
    } else {
        ++m_totalFrames;
        m_buffer.push(f);
        emit frameReceived(f);
    }
}

void CanBridge::onFramesReceived() {
    while (m_device->framesAvailable()) {
        const QCanBusFrame f = m_device->readFrame();
        emit liveFrame(f);
        if (m_capturing) handleFrame(f);
    }
}

void CanBridge::onBridgeFrames(const QList<QCanBusFrame> &frames) {
    for (const QCanBusFrame &f : frames) {
        emit liveFrame(f);
        if (m_capturing) handleFrame(f);
    }
}

void CanBridge::onDeviceError(QCanBusDevice::CanBusError err) {
    if (err != QCanBusDevice::NoError)
        emit busError(m_device->errorString());
}
