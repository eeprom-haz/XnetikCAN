#include "BridgeDevice.h"
#include "BridgeProtocol.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QDir>
#include <cstring>

using namespace xnbridge;

BridgeDevice::BridgeDevice(QObject *parent) : QObject(parent) {}

BridgeDevice::~BridgeDevice() { close(); }

QString BridgeDevice::bridgeExePath() {
    return QDir(QCoreApplication::applicationDirPath()).filePath("J2534Bridge.exe");
}

int BridgeDevice::dllBitness(const QString &dllPath) {
    QString path = dllPath;
    // A 64-bit process sees the real System32; 32-bit drivers registered there
    // actually live in SysWOW64 (the bridge itself gets redirected automatically).
    if (!QFile::exists(path) && path.contains("\\System32\\", Qt::CaseInsensitive))
        path.replace("\\System32\\", "\\SysWOW64\\", Qt::CaseInsensitive);

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return 0;
    QByteArray dos = f.read(64);
    if (dos.size() < 64 || !dos.startsWith("MZ")) return 0;
    quint32 peOffset;
    std::memcpy(&peOffset, dos.constData() + 0x3C, 4);
    if (!f.seek(peOffset)) return 0;
    QByteArray pe = f.read(6);
    if (pe.size() < 6 || !pe.startsWith(QByteArray("PE\0\0", 4))) return 0;
    quint16 machine;
    std::memcpy(&machine, pe.constData() + 4, 2);
    if (machine == 0x014c) return 32;                      // IMAGE_FILE_MACHINE_I386
    if (machine == 0x8664 || machine == 0xAA64) return 64; // AMD64 / ARM64
    return 0;
}

bool BridgeDevice::open(const QString &dllPath, quint32 bitrate, QString *errorMsg) {
    close();

    const QString exe = bridgeExePath();
    if (!QFile::exists(exe)) {
        if (errorMsg) *errorMsg = "J2534Bridge.exe not found next to XnetikCan.exe:\n" + exe;
        return false;
    }

    m_proc = new QProcess(this);
    m_proc->setProcessChannelMode(QProcess::SeparateChannels);
    // The bridge's stderr is a debug trace nobody reads here; unread, it would pile up in memory.
    m_proc->setStandardErrorFile(QProcess::nullDevice());
    connect(m_proc, &QProcess::readyReadStandardOutput, this, &BridgeDevice::onReadyRead);
    connect(m_proc, &QProcess::finished, this, &BridgeDevice::onFinished);
    m_proc->start(exe, {});
    if (!m_proc->waitForStarted(5000)) {
        if (errorMsg) *errorMsg = "Could not start J2534Bridge.exe: " + m_proc->errorString();
        delete m_proc;
        m_proc = nullptr;
        return false;
    }

    QByteArray payload(4, '\0');
    std::memcpy(payload.data(), &bitrate, 4);
    payload += dllPath.toUtf8();
    m_haveResult = false;
    send(CmdOpen, payload);

    // Vendor DLLs can take a few seconds to find the adapter (Bluetooth especially).
    QElapsedTimer t;
    t.start();
    while (!m_haveResult && m_proc && m_proc->state() == QProcess::Running && t.elapsed() < 15000)
        m_proc->waitForReadyRead(100);

    if (!m_haveResult || m_resultCode != 0) {
        if (errorMsg)
            *errorMsg = m_haveResult ? m_resultText
                                     : QStringLiteral("J2534Bridge did not answer (timed out or exited)");
        close();
        return false;
    }
    m_open = true;
    return true;
}

void BridgeDevice::close() {
    if (!m_proc) return;
    const bool wasOpen = m_open;
    m_open = false;
    disconnect(m_proc, nullptr, this, nullptr);
    if (m_proc->state() == QProcess::Running) {
        send(CmdClose);
        if (!m_proc->waitForFinished(3000)) {
            m_proc->kill();
            m_proc->waitForFinished(1000);
        }
    }
    delete m_proc;
    m_proc = nullptr;
    m_inbox.clear();
    if (wasOpen) emit closed();
}

bool BridgeDevice::writeFrame(const QCanBusFrame &frame) {
    if (!m_open) return false;
    WireFrame w{};
    w.id = frame.frameId();
    w.flags = frame.hasExtendedFrameFormat() ? FlagExtended : 0;
    const QByteArray data = frame.payload().left(8);
    w.len = quint8(data.size());
    std::memcpy(w.data, data.constData(), w.len);
    send(CmdWrite, QByteArray(reinterpret_cast<const char *>(&w), sizeof(w)));
    return true;
}

void BridgeDevice::send(quint8 type, const QByteArray &payload) {
    if (!m_proc) return;
    QByteArray msg(kHeaderSize, '\0');
    msg[0] = char(type);
    const quint32 len = quint32(payload.size());
    std::memcpy(msg.data() + 1, &len, 4);
    msg += payload;
    m_proc->write(msg);
}

void BridgeDevice::onReadyRead() {
    if (!m_proc) return;
    m_inbox += m_proc->readAllStandardOutput();

    QList<QCanBusFrame> frames;
    while (m_inbox.size() >= int(kHeaderSize)) {
        quint32 len;
        std::memcpy(&len, m_inbox.constData() + 1, 4);
        if (m_inbox.size() < int(kHeaderSize + len)) break;
        const quint8 type = quint8(m_inbox[0]);
        const char *p = m_inbox.constData() + kHeaderSize;

        if (type == RspFrames) {
            for (quint32 off = 0; off + sizeof(WireFrame) <= len; off += sizeof(WireFrame)) {
                WireFrame w;
                std::memcpy(&w, p + off, sizeof(w));
                QCanBusFrame f(w.id, QByteArray(reinterpret_cast<const char *>(w.data), w.len));
                f.setExtendedFrameFormat(w.flags & FlagExtended);
                f.setTimeStamp(QCanBusFrame::TimeStamp::fromMicroSeconds(w.timestampUs));
                frames.append(f);
            }
        } else if (type == RspResult && len >= 4) {
            std::memcpy(&m_resultCode, p, 4);
            m_resultText = QString::fromUtf8(p + 4, int(len - 4));
            m_haveResult = true;
        } else if (type == RspError) {
            emit errorOccurred(QString::fromUtf8(p, int(len)));
        } else if (type == RspVbatt && len == 4) {
            quint32 mv;
            std::memcpy(&mv, p, 4);
            emit batteryVoltage(mv / 1000.0);
        }
        m_inbox.remove(0, int(kHeaderSize + len));
    }
    if (!frames.isEmpty()) emit framesReceived(frames);
}

void BridgeDevice::onFinished() {
    if (!m_open) return;
    m_open = false;
    emit errorOccurred("J2534Bridge exited unexpectedly");
    emit closed();
}
