#pragma once
#include <QObject>
#include <QTimer>
#include <QByteArray>
#include <QSet>
#include <QCanBusFrame>

class CanBridge;

// ISO 15765-2 (ISO-TP) request/response client. One transaction at a time. Requests and
// responses may be multi-frame (we wait for / send the flow-control frames).
// Addressing: 11-bit or 29-bit IDs, and "extended addressing" where the first data byte of
// every frame is an address (BMW / Toyota gateways: target byte out, tester byte back).
class IsoTp : public QObject {
    Q_OBJECT
public:
    struct Address {
        quint32 tx = 0, rxMin = 0, rxMax = 0;
        bool ext29 = false;   // 29-bit IDs
        int txEa = -1;        // address byte in front of every frame we send (-1 = none)
        int rxEa = -1;        // address byte expected in front of every reply frame (-1 = none)
    };

    explicit IsoTp(CanBridge *bridge, QObject *parent = nullptr);

    // txId: 0x7DF (functional) or a physical request ID such as 0x7E0.
    // Responses are accepted from rxMin..rxMax (e.g. 0x7E8..0x7EF).
    bool request(quint32 txId, quint32 rxMin, quint32 rxMax, const QByteArray &payload,
                 int timeoutMs = 150);
    bool request(const Address &a, const QByteArray &payload, int timeoutMs = 150);
    bool busy() const { return m_state != State::Idle; }
    void cancel();
    // IDs any IsoTp in this app has sent diagnostic frames on (requests, flow control).
    static bool ourId(quint32 id) { return ourIds().contains(id); }

signals:
    void response(quint32 rxId, const QByteArray &payload);  // complete UDS/OBD payload
    void timedOut();

private slots:
    void onFrame(const QCanBusFrame &f);
    void onTimeout();

private:
    enum class State { Idle, WaitFlowControl, SendingConsecutive, WaitFirst, WaitConsecutive };
    void finish();
    void sendConsecutive();
    bool echoMatches(const QByteArray &afterSid) const;   // reply belongs to our request?
    void send(quint32 id, const QByteArray &data);
    static QSet<quint32> &ourIds();

    CanBridge *m_bridge;
    QTimer m_timer;
    State m_state = State::Idle;
    quint32 m_txId = 0, m_rxMin = 0, m_rxMax = 0, m_rxId = 0;
    bool m_ext29 = false;
    int m_txEa = -1, m_rxEa = -1;
    int off() const { return m_txEa >= 0 ? 1 : 0; }   // data bytes taken by the address byte
    QByteArray m_request;
    QByteArray m_buffer;
    int m_expected = 0;
    quint8 m_nextSeq = 1;
    int m_sendPos = 0;        // multi-frame request: next byte to send
    quint8 m_sendSeq = 1;
    int m_stMinMs = 0;        // ECU's minimum gap between our consecutive frames
    int m_timeoutMs = 150;
};
