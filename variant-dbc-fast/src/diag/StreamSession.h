#pragma once
#include <QObject>
#include <QTimer>
#include <QElapsedTimer>
#include <QHash>
#include <QSet>
#include <QList>
#include <QByteArray>
#include <functional>

class CanBridge;
class IsoTp;
class QCanBusFrame;
class QFile;

// ECU streaming ("fast mode"): asks the engine computer (7E0) to send values by itself instead
// of being polled for each one. Recipe proven on a 2016 Ford Transit Connect PCM:
//   10 03 extended session → 2C 01 F2nn <DID> 01 <len> (one source per request) → 2A 03 <lists>
//   → periodic frames (list number + up to 7 data bytes) on 6A0/6A1, ~100 frames/s shared
//   → keep alive with 3E 80 → stop 2A 04, clear 2C 03, back to 10 01.
// Standard OBD values are streamed as DID F4xx (Mode 01 PID xx read the UDS way).
// A value counts as streamed only while its own list keeps arriving; anything refused, silent or
// stopped goes back to polling straight away, so a ticked value never freezes.
// Every step and what actually arrives is written to <AppData>/logs/stream-<date>.log.
class StreamSession : public QObject {
    Q_OBJECT
public:
    explicit StreamSession(CanBridge *bridge, QObject *parent = nullptr);

    // The DIDs that should be streamed; an empty list stops streaming. Changes are collected for
    // a moment so several ticks in a row cause one rebuild, not one each.
    void setSources(const QList<quint16> &dids);
    void stop();                                  // stop and clean up in the ECU
    void allowRetry() { m_unavailable = false; m_why.clear(); }   // e.g. Fast mode ticked again

    bool unavailable() const { return m_unavailable; }   // the ECU won't stream (this connection)
    // The PCM is (being put) in the extended session for streaming: setup, streaming or cleanup.
    bool active() const { return m_state != State::Idle; }
    QString unavailableWhy() const { return m_why; }
    bool rejected(quint16 did) const { return m_rejected.contains(did); }   // can't be a stream source
    QSet<quint16> streaming() const { return m_streamingSet; }               // arriving right now
    int framesReceived() const { return m_frames; }
    static QString logPath();

    static constexpr int kBatchMs = 1200;         // wait this long after the last change before rebuilding
    static constexpr int kAliveMs = 700;          // a list silent longer than this is "not arriving"
    static constexpr int kFirstFrameMs = 2500;    // a list that never arrives within this is dropped

signals:
    void data(quint16 did, const QByteArray &bytes);   // one value's bytes, as read by 22 <DID>
    void changed();                                     // what is streamed (or not) changed
    // True while setting up or taking down a stream: other requests to the ECU should wait,
    // the engine computer handles one at a time.
    void talking(bool busy);

private:
    struct Step {
        QByteArray request;
        std::function<bool(const QByteArray *reply)> handle;   // nullptr reply = no answer; false = abort
    };
    enum class State { Idle, Setup, Streaming, Stopping };

    void applyWanted(const QList<quint16> &want);
    void startSetup();
    void runNext();
    void onResponse(quint32 rxId, const QByteArray &payload);
    void onTimeout();
    void onFrame(const QCanBusFrame &f);
    void checkHealth();                // which lists are arriving; drop lists that never come
    void giveUp(const QString &why);   // ECU won't stream: clean up, everything back to polling
    void queueCleanup();
    void reset();
    void log(const QString &line);

    CanBridge *m_bridge;
    IsoTp *m_tp;
    State m_state = State::Idle;
    QList<Step> m_steps;
    int m_retries = 0;

    QList<quint16> m_wanted;                // asked for by the caller (being streamed or set up)
    QList<quint16> m_pending;               // latest request, applied when the batch timer fires
    QHash<quint16, int> m_length;           // DID -> data bytes (learned once per connection)
    QSet<quint16> m_rejected;               // DIDs that can't be streamed: polled
    QList<QList<quint16>> m_lists;          // F200, F201, ... : DIDs in each list, in order
    quint8 m_listBase = 0x00;               // lists are F200.. (this PCM refuses F210 and up)
    QSet<quint16> m_streamingSet;           // DIDs whose list is arriving right now
    QHash<int, quint32> m_listCanId;        // list -> CAN ID it is taken from (first one seen)
    QHash<int, qint64> m_listLast;          // list -> time of its last frame (m_clock ms)
    QHash<quint64, int> m_frameCount;       // (CAN ID << 8 | list) -> frames, for the log
    QElapsedTimer m_clock;
    qint64 m_streamStart = 0;
    qint64 m_lastSummary = 0;
    bool m_unavailable = false;
    QString m_why;
    QString m_lastRefusal;                  // "NRC 31" of the last value refused for a list
    bool m_restartAfterStop = false;
    int m_frames = 0;
    QList<qint64> m_losses;                 // times the stream was lost (m_clock ms)

    QTimer m_batch;                         // collects tick changes
    QTimer m_health;                        // per-list arrival check
    QTimer m_keepAlive;                     // 3E 80 every second keeps the extended session open
    QTimer m_watchdog;                      // no frames at all for this long: back to polling
    QFile *m_log = nullptr;
};
