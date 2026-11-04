#include "IsoTp.h"
#include "../can/CanBridge.h"

static constexpr int kConsecutiveTimeoutMs = 1000;  // N_Cr
static constexpr quint8 kPad = 0x00;

IsoTp::IsoTp(CanBridge *bridge, QObject *parent) : QObject(parent), m_bridge(bridge) {
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, &IsoTp::onTimeout);
    connect(m_bridge, &CanBridge::liveFrame, this, &IsoTp::onFrame);
}

bool IsoTp::request(quint32 txId, quint32 rxMin, quint32 rxMax, const QByteArray &payload,
                    int timeoutMs) {
    Address a;
    a.tx = txId;
    a.rxMin = rxMin;
    a.rxMax = rxMax;
    return request(a, payload, timeoutMs);
}

bool IsoTp::request(const Address &a, const QByteArray &payload, int timeoutMs) {
    if (busy() || payload.isEmpty() || payload.size() > 4095 || !m_bridge->isConnected())
        return false;
    m_txId = a.tx;
    m_rxMin = a.rxMin;
    m_rxMax = a.rxMax;
    m_ext29 = a.ext29;
    m_txEa = a.txEa;
    m_rxEa = a.rxEa;
    m_request = payload;
    m_timeoutMs = timeoutMs;
    m_buffer.clear();
    const int o = off();
    QByteArray data(8, char(kPad));
    if (o) data[0] = char(m_txEa);
    if (payload.size() <= 7 - o) {
        data[o] = char(payload.size());  // single frame PCI
        data.replace(o + 1, payload.size(), payload);
        m_state = State::WaitFirst;
        m_timer.start(timeoutMs);
    } else {
        // First frame; the rest goes out once the ECU's flow control says so.
        data[o] = char(0x10 | (payload.size() >> 8));
        data[o + 1] = char(payload.size() & 0xFF);
        data.replace(o + 2, 6 - o, payload.left(6 - o));
        m_sendPos = 6 - o;
        m_sendSeq = 1;
        m_state = State::WaitFlowControl;
        m_timer.start(kConsecutiveTimeoutMs);
    }
    send(m_txId, data);
    return true;
}

QSet<quint32> &IsoTp::ourIds() {
    static QSet<quint32> ids;
    return ids;
}

void IsoTp::send(quint32 id, const QByteArray &data) {
    ourIds().insert(id);
    QCanBusFrame f(id, data);
    f.setExtendedFrameFormat(m_ext29);
    m_bridge->writeFrame(f);
}

void IsoTp::sendConsecutive() {
    if (m_state != State::SendingConsecutive) return;
    const int o = off();
    QByteArray cf(8, char(kPad));
    if (o) cf[0] = char(m_txEa);
    cf[o] = char(0x20 | (m_sendSeq++ & 0x0F));
    const QByteArray chunk = m_request.mid(m_sendPos, 7 - o);
    cf.replace(o + 1, chunk.size(), chunk);
    m_sendPos += 7 - o;
    send(m_txId, cf);
    if (m_sendPos < m_request.size()) {
        if (m_stMinMs > 0) QTimer::singleShot(m_stMinMs, this, &IsoTp::sendConsecutive);
        else sendConsecutive();
        return;
    }
    m_state = State::WaitFirst;   // request complete: now wait for the answer
    m_timer.start(m_timeoutMs);
}

bool IsoTp::echoMatches(const QByteArray &afterSid) const {
    // Mode 01 with several PIDs: the reply lists each PID before its data, so only the
    // first one sits right after the service id — accept any PID we asked for.
    const quint8 sid = quint8(m_request[0]);
    if (sid == 0x01 && m_request.size() > 2)
        return !afterSid.isEmpty() && m_request.mid(1).contains(afterSid[0]);
    // Compare only what this kind of request echoes in its positive reply:
    // 22 → the DID, 2C → sub-function + list id, 2A → nothing, others (01, 09, 10, 21, 3E…) → 1 byte.
    int echo = sid == 0x22 ? 2 : sid == 0x2C ? 3 : sid == 0x2A ? 0 : 1;
    echo = qMin(echo, int(m_request.size()) - 1);
    return afterSid.size() >= echo && afterSid.left(echo) == m_request.mid(1, echo);
}

void IsoTp::cancel() {
    m_timer.stop();
    m_state = State::Idle;
}

void IsoTp::finish() {
    m_timer.stop();
    m_state = State::Idle;
    emit response(m_rxId, m_buffer);
}

void IsoTp::onFrame(const QCanBusFrame &f) {
    if (m_state == State::Idle || f.hasExtendedFrameFormat() != m_ext29) return;
    const quint32 id = f.frameId();
    if (m_state == State::WaitConsecutive ? id != m_rxId : (id < m_rxMin || id > m_rxMax))
        return;
    QByteArray p = f.payload();
    // Extended addressing: the reply starts with its address byte, then the normal frame.
    if (m_rxEa >= 0) {
        if (p.isEmpty() || quint8(p[0]) != quint8(m_rxEa)) return;
        p.remove(0, 1);
    }
    if (p.isEmpty()) return;
    const int room = 7 - (m_rxEa >= 0 ? 1 : 0);   // data bytes per frame after the PCI byte
    const quint8 pci = quint8(p[0]);

    switch (pci >> 4) {
    case 0x0: {  // single frame
        if (m_state != State::WaitFirst) return;
        const int len = pci & 0x0F;
        if (len == 0 || len > p.size() - 1) return;
        const QByteArray body = p.mid(1, len);
        // Ignore replies that answer someone else's request (service id + 0x40, or 7F <sid>).
        const quint8 sid = quint8(m_request[0]);
        // Positive replies to 0x01/0x22 echo the PID/DID; matching it keeps a late
        // answer from a second ECU being taken as the reply to our next request.
        const bool positive = quint8(body[0]) == sid + 0x40 && echoMatches(body.mid(1));
        const bool negative = body.size() >= 3 && quint8(body[0]) == 0x7F && quint8(body[1]) == sid;
        if (!positive && !negative) return;
        if (negative && quint8(body[2]) == 0x78) {  // response pending: keep waiting
            m_timer.start(5000);
            return;
        }
        m_rxId = id;
        m_buffer = body;
        finish();
        break;
    }
    case 0x1: {  // first frame
        if (m_state != State::WaitFirst || p.size() < room + 1) return;
        m_expected = ((pci & 0x0F) << 8) | quint8(p[1]);
        if (m_expected < room) return;
        if (quint8(p[2]) != quint8(m_request[0]) + 0x40) return;
        if (!echoMatches(p.mid(3))) return;
        m_rxId = id;
        m_buffer = p.mid(2, room - 1);
        m_nextSeq = 1;
        m_state = State::WaitConsecutive;
        // Flow control to the responder's physical request ID (0x7E8 -> 0x7E0), with our
        // address byte in front when extended addressing is used:
        // block size 0, separation time 0 = send everything.
        QByteArray fc(8, char(kPad));
        const int o = off();
        if (o) fc[0] = char(m_txEa);
        fc[o] = 0x30;
        const quint32 fcId = (m_txId == 0x7DF) ? id - 8 : m_txId;
        send(fcId, fc);
        m_timer.start(kConsecutiveTimeoutMs);
        break;
    }
    case 0x2: {  // consecutive frame
        if (m_state != State::WaitConsecutive) return;
        if ((pci & 0x0F) != m_nextSeq) {  // lost a frame: give up on this transaction
            m_timer.stop();
            m_state = State::Idle;
            emit timedOut();
            return;
        }
        m_nextSeq = (m_nextSeq + 1) & 0x0F;
        m_buffer += p.mid(1, qMin(room, m_expected - int(m_buffer.size())));
        if (m_buffer.size() >= m_expected) finish();
        else m_timer.start(kConsecutiveTimeoutMs);
        break;
    }
    case 0x3: {  // flow control for our multi-frame request
        if (m_state != State::WaitFlowControl) return;
        const int status = pci & 0x0F;   // 0 continue, 1 wait, 2 overflow
        if (status == 1) { m_timer.start(kConsecutiveTimeoutMs); return; }
        if (status != 0) { m_timer.stop(); m_state = State::Idle; emit timedOut(); return; }
        const int stMin = p.size() > 2 ? quint8(p[2]) : 0;
        m_stMinMs = stMin <= 0x7F ? stMin : 1;   // 0xF1..0xF9 = 100–900 µs: 1 ms is close enough
        m_timer.stop();
        m_state = State::SendingConsecutive;
        sendConsecutive();
        break;
    }
    default:
        break;
    }
}

void IsoTp::onTimeout() {
    m_state = State::Idle;
    emit timedOut();
}
