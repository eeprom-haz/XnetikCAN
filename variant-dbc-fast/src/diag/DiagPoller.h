#pragma once
#include <QObject>
#include <QTimer>
#include <QElapsedTimer>
#include <QList>
#include <QSet>
#include <QHash>
#include "EnhancedLibrary.h"
#include "OeLibrary.h"
#include "IsoTp.h"
#include <QPair>
#include <QVector>

class CanBridge;
struct ObdPid;

// Round-robin poller for standard OBD-II PIDs (Mode 01, functional 0x7DF) and
// manufacturer DIDs (UDS 0x22, physical e.g. 0x7E0). One request in flight.
class DiagPoller : public QObject {
    Q_OBJECT
public:
    struct Item {
        int key = 0;            // caller's signal key, echoed in sample()
        bool isDid = false;
        quint8 pid = 0;
        QVector<quint8> pids;                          // Mode 01, filled by the poller: PIDs asked in one request
        QList<QPair<int, const ObdPid *>> obdValues;  // Mode 01: every ticked value carried by these PIDs
        EnhancedDid did;
        // Manufacturer (OBDb) command: one request, several decoded values.
        bool isOe = false;
        quint32 tx = 0, rx = 0;
        bool ext29 = false;          // 29-bit IDs
        int txEa = -1, rxEa = -1;    // extended addressing bytes (-1 = none)
        int timeoutMs = 0;           // 0 = default
        quint8 session = 0;          // 10 <session> needed before this request (library "din")
        quint8 fallbackSession = 0;  // session this module's other values need (tried on "wrong session")
        quint8 sessionOut = 0;       // 10 <sessionOut> leaves it (library "dout"; 0 = 81 / 01)
        QString module;              // module address text, e.g. "7E0" or "6F1.07" (sessions, log)
        QByteArray request;
        QList<QPair<int, OeSignal>> oeSignals;
        bool priority = false;       // asked several times a round (values on the overlay)
    };

    explicit DiagPoller(CanBridge *bridge, QObject *parent = nullptr);

    void setItems(const QList<Item> &items);
    void start();
    void stop();
    bool isRunning() const { return m_mode == Mode::Polling; }

    void discoverPids();     // Mode 01 support bitmaps 0x00, 0x20, 0x40, ...
    // Send each item once and report whether the vehicle answered it (manufacturer data).
    void probe(const QList<Item> &items);
    bool isProbing() const { return m_mode == Mode::Probing; }
    double elapsed() const { return m_clock.elapsed() / 1000.0; }
    int replies() const { return m_replies; }    // values updated so far
    bool grouping() const { return !m_groupingOff; }
    // Hold requests while another client talks to the ECU (e.g. setting up a stream); polling
    // carries on where it left off afterwards.
    void setPaused(bool paused);
    int timeouts() const { return m_timeouts; }
    // While the engine computer streams (extended session), standard values are asked directly
    // as 22 F4xx on 7E0: a broadcast OBD request (01 on 7DF) makes the Transit Connect PCM drop
    // the session and stop streaming (stream log 2026-10-26).
    void setStandardAsUds(bool on);

signals:
    void sample(int key, double t, double value, const QString &text);  // text: enum/on-off, else empty
    void itemFailed(int key, const QString &why);  // e.g. DID not supported
    void pidsDiscovered(const QSet<quint8> &supported);
    void discoveryFailed(const QString &msg);
    void probeResult(int index, bool answered, const QString &why);
    void probeFinished();

private slots:
    void onResponse(quint32 rxId, const QByteArray &payload);
    void onTimeout();
    void sendNext();

private:
    enum class Mode { Idle, Polling, Discovering, Probing };
    void scheduleNext();
    void handleDiscovery(const QByteArray &payload);
    bool sendItem(const Item &item);
    void probeNext();
    void probeDone(bool answered, const QString &why);
    void regroup();   // m_raw -> m_items: standard values up to 6 per request
    // Diagnostic sessions (generic, from the library): open the module's session before asking,
    // and when a module answers "wrong session" open the one its other values need and retry once.
    bool sendOe(const Item &item);
    bool sendActual(const Item &item);                     // the request itself, no session step
    QString heldObdSession() const;                        // an engine-bay module (7E0–7E7) held in a session
    bool handleSessionStep(const QByteArray *reply);     // true: this reply belonged to "10 xx"
    bool retryInSession(const QByteArray &reply);          // true: re-sent in a session
    void afterNoSend();                                    // a request couldn't go out
    static IsoTp::Address addressOf(const Item &item);
    int timeoutOf(const Item &item) const { return item.timeoutMs > 250 ? item.timeoutMs : 250; }
    QString itemText(const Item &item) const;
    void logPollSummary();

    CanBridge *m_bridge;
    IsoTp *m_tp;
    QTimer m_gap;
    QElapsedTimer m_clock;
    Mode m_mode = Mode::Idle;

    QList<Item> m_raw;                    // as given by setItems()
    QList<Item> m_items;                  // what is actually sent, in turn
    int m_index = 0;
    // Measured on a 2016 Transit Connect (V200): 6 PIDs per request = 67 values/s vs 33 one at a
    // time, no misses. ECUs that refuse or ignore grouped requests get one PID at a time.
    bool m_paused = false;
    bool m_udsStandard = false;   // standard values as 22 F4xx on 7E0 (no broadcast requests)
    bool m_groupingOff = false;
    bool m_groupingWorked = false;
    int m_groupMisses = 0;
    Item m_current;
    bool m_inFlight = false;
    quint8 m_discoverPid = 0;
    QSet<quint8> m_supported;
    int m_replies = 0;
    int m_timeouts = 0;
    QList<Item> m_probeItems;
    int m_probeIndex = 0;
    QHash<QString, int> m_silentStreak;   // module -> timeouts in a row with no answer yet
    QSet<QString> m_answeredIds;          // modules that answered at least once

    struct SessionState { quint8 session = 0; qint64 lastMs = -100000; qint64 failedUntil = 0; };
    QHash<QString, SessionState> m_sessions;   // module -> session we put it in
    QHash<QString, quint8> m_needSession;      // module -> session learned from a "wrong session" answer
    bool m_sessionStep = false;                // the request in flight is "10 xx" for m_current
    quint8 m_sessionWant = 0;
    bool m_retriedSession = false;             // m_current was already re-sent once in a session
    // Some engine computers (INFINITI Q50 in Nissan session C0) ignore standard OBD requests while
    // in a session: learned from misses, then each standard round first takes it back to normal
    // (10 81 / 10 01) and the manufacturer round re-opens the session.
    bool m_leaveForObd = false;
    int m_obdMissInSession = 0;
    bool m_leaveStep = false;                  // the request in flight is "leave session" before m_current
    QString m_leaveModule;
    QHash<QString, IsoTp::Address> m_sessionAddr;   // module -> its address (to leave its session)
    QHash<QString, quint8> m_sessionOut;            // module -> library "dout"
    static constexpr int kSessionHoldMs = 2000;   // re-open a session after this long without traffic
    // Request log: per-module counts, each refusal once, a summary every 10 s.
    struct Counts { int asked = 0, answered = 0, refused = 0, missed = 0; };
    QHash<QString, Counts> m_counts;
    QSet<QString> m_loggedOnce;
    qint64 m_lastSummary = 0;

    static constexpr int kGapMs = 10;  // ~100 requests/s max across all items
    static constexpr int kMaxPidsPerRequest = 6;   // ISO 15765-4 limit for Mode 01 on CAN
    static constexpr int kMaxGroupedPidBytes = 4;  // longer replies are asked one PID at a time
    static constexpr int kGroupGiveUp = 4;         // grouped requests never answered this often: stop grouping
    static constexpr int kPriorityEvery = 3;       // priority items asked again after this many others (at least)
};
