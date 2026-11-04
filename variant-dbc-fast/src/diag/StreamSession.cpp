#include "StreamSession.h"
#include "IsoTp.h"
#include "../can/CanBridge.h"
#include <QCanBusFrame>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <algorithm>

namespace {
constexpr quint32 kEcuRequest = 0x7E0;
constexpr quint32 kEcuReply = 0x7E8;
constexpr int kMaxLists = 16;        // F200..F20F
constexpr int kListBytes = 7;        // one periodic frame: list number + 7 data bytes
constexpr int kTries = 3;            // a setup step that gets no answer is asked this often

QString hex(const QByteArray &b) { return QString::fromLatin1(b.toHex(' ').toUpper()); }
QString hex16(quint16 v) { return QString::number(v, 16).rightJustified(4, '0').toUpper(); }
QString didList(const QList<quint16> &dids) {
    QStringList s;
    for (quint16 d : dids) s << hex16(d);
    return s.join(' ');
}
}

StreamSession::StreamSession(CanBridge *bridge, QObject *parent)
    : QObject(parent), m_bridge(bridge), m_tp(new IsoTp(bridge, this)) {
    m_clock.start();
    connect(m_tp, &IsoTp::response, this, &StreamSession::onResponse);
    connect(m_tp, &IsoTp::timedOut, this, &StreamSession::onTimeout);
    connect(m_bridge, &CanBridge::liveFrame, this, &StreamSession::onFrame);
    connect(m_bridge, &CanBridge::stateChanged, this, [this](bool connected) {
        if (connected) return;
        // The ECU drops the session by itself; the next connection may be another vehicle.
        reset();
        m_batch.stop();
        m_pending.clear();
        m_wanted.clear();
        m_length.clear();
        m_rejected.clear();
        m_losses.clear();
        m_unavailable = false;
        m_why.clear();
        emit talking(false);
        emit changed();
    });
    m_batch.setSingleShot(true);
    m_batch.setInterval(kBatchMs);
    connect(&m_batch, &QTimer::timeout, this, [this] { applyWanted(m_pending); });
    m_health.setInterval(250);
    connect(&m_health, &QTimer::timeout, this, &StreamSession::checkHealth);
    m_keepAlive.setInterval(1000);
    connect(&m_keepAlive, &QTimer::timeout, this, [this] {
        QByteArray d(8, '\0');
        d[0] = 0x02; d[1] = 0x3E; d[2] = char(0x80);   // tester present, no reply wanted
        m_bridge->writeFrame(QCanBusFrame(kEcuRequest, d));
    });
    m_watchdog.setSingleShot(true);
    m_watchdog.setInterval(1500);
    connect(&m_watchdog, &QTimer::timeout, this, [this] {
        // Lost (e.g. something else made the PCM leave the extended session): set it up again,
        // but not forever — three losses within a minute and this connection polls instead.
        const qint64 now = m_clock.elapsed();
        m_losses.removeIf([now](qint64 t) { return now - t > 60000; });
        m_losses << now;
        if (m_losses.size() >= 3) {
            giveUp("the stream keeps stopping (3 times in a minute)");
            return;
        }
        log(QString("stream lost (%1 of 3 in a minute): setting it up again").arg(m_losses.size()));
        m_restartAfterStop = true;
        m_tp->cancel();
        m_steps.clear();
        m_keepAlive.stop();
        m_health.stop();
        m_streamingSet.clear();
        emit changed();
        queueCleanup();
    });
}

QString StreamSession::logPath() {
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/logs/stream-" +
           QDate::currentDate().toString("yyyy-MM-dd") + ".log";
}

void StreamSession::log(const QString &line) {
    if (!m_log) {
        const QString path = logPath();
        QDir().mkpath(QFileInfo(path).path());
        m_log = new QFile(path, this);
        if (!m_log->open(QIODevice::Append | QIODevice::Text)) return;
    }
    if (!m_log->isOpen()) return;
    m_log->write((QTime::currentTime().toString("HH:mm:ss.zzz") + "  " + line + "\n").toUtf8());
    m_log->flush();
}

void StreamSession::setSources(const QList<quint16> &dids) {
    QList<quint16> want;
    for (quint16 d : dids)
        if (!m_rejected.contains(d) && !want.contains(d)) want << d;
    std::sort(want.begin(), want.end());
    if (want.isEmpty()) { stop(); return; }
    // Same values, or fewer than are streaming: keep the stream as it is (the extra data is
    // simply not shown). Only new values need the lists rebuilt — every rebuild is a short gap.
    const bool active = m_state == State::Streaming || m_state == State::Setup;
    if ((want == m_wanted || (active && std::all_of(want.cbegin(), want.cend(),
                                                    [this](quint16 d) { return m_wanted.contains(d); }))) &&
        !m_restartAfterStop) {
        m_batch.stop();
        m_pending.clear();
        return;
    }
    // New values: wait until the ticking stops, then rebuild once.
    m_pending = want;
    m_batch.start();
}

void StreamSession::applyWanted(const QList<quint16> &wantIn) {
    QList<quint16> want;
    for (quint16 d : wantIn)
        if (!m_rejected.contains(d)) want << d;
    m_pending.clear();
    if (want.isEmpty()) { stop(); return; }
    if (want == m_wanted && !m_restartAfterStop && m_state != State::Idle) return;
    log(QString("values to stream (%1): %2").arg(want.size()).arg(didList(want)));
    m_wanted = want;
    if (m_unavailable) return;
    switch (m_state) {
    case State::Idle:
        startSetup();
        break;
    case State::Setup:
    case State::Streaming:
        // Different values: take the old lists down first, then set up the new ones.
        log("rebuilding the stream for new values");
        m_restartAfterStop = true;
        m_tp->cancel();
        m_steps.clear();
        m_keepAlive.stop();
        m_watchdog.stop();
        m_health.stop();
        m_streamingSet.clear();
        emit changed();
        queueCleanup();
        break;
    case State::Stopping:
        m_restartAfterStop = true;
        break;
    }
}

void StreamSession::stop() {
    m_batch.stop();
    m_pending.clear();
    m_wanted.clear();
    m_restartAfterStop = false;
    if (m_state == State::Idle || m_state == State::Stopping) return;
    log("stop");
    m_tp->cancel();
    m_steps.clear();
    m_keepAlive.stop();
    m_watchdog.stop();
    m_health.stop();
    m_streamingSet.clear();
    emit changed();
    queueCleanup();
}

void StreamSession::reset() {
    m_tp->cancel();
    m_steps.clear();
    m_keepAlive.stop();
    m_watchdog.stop();
    m_health.stop();
    m_lists.clear();
    m_streamingSet.clear();
    m_listCanId.clear();
    m_listLast.clear();
    m_frameCount.clear();
    m_retries = 0;
    m_restartAfterStop = false;
    m_state = State::Idle;
}

void StreamSession::startSetup() {
    m_state = State::Setup;   // first, so listeners of talking() already see the stream as active
    emit talking(true);   // polling holds off; it resumes once the stream runs (or we give up)
    m_steps.clear();
    m_lists.clear();
    m_listCanId.clear();
    m_listLast.clear();
    m_frameCount.clear();
    log(QString("setup for %1 values").arg(m_wanted.size()));
    // Lists are always F200..: the Transit Connect PCM refuses F210 and up (NRC 31). Each list is
    // cleared (2C 03) right before it is defined, so an old definition can't get values appended.
    // 1. Learn each value's length with a plain read (and whether it can be read as a DID at all).
    for (quint16 did : std::as_const(m_wanted)) {
        if (m_length.contains(did) || m_rejected.contains(did)) continue;
        m_steps << Step{QByteArray("\x22", 1) + char(did >> 8) + char(did & 0xFF), [this, did](const QByteArray *r) {
                            if (r && r->size() > 3 && quint8((*r)[0]) == 0x62 && r->size() - 3 <= kListBytes) {
                                m_length.insert(did, int(r->size()) - 3);
                            } else {
                                m_rejected.insert(did);   // polled instead
                                log(QString("%1 can't be read as a data ID: polled").arg(hex16(did)));
                            }
                            return true;
                        }};
    }
    // 2. Pack into lists, then queue the session, the definitions and the start.
    m_steps << Step{QByteArray(), [this](const QByteArray *) {
        // Values that can't be streamed stay polled; leaving them out here also keeps a later
        // setSources() with the same ticked values from looking like a change (and restarting).
        m_wanted.removeIf([this](quint16 did) { return m_rejected.contains(did); });
        int used = kListBytes;
        for (quint16 did : std::as_const(m_wanted)) {
            if (!m_length.contains(did)) continue;
            if (used + m_length[did] > kListBytes) {
                if (m_lists.size() >= kMaxLists) break;
                m_lists.append(QList<quint16>());
                used = 0;
            }
            m_lists.last() << did;
            used += m_length[did];
        }
        if (m_lists.isEmpty()) {   // nothing on this ECU can be streamed: all of it stays polled
            log("nothing streamable: all polled the normal way");
            m_state = State::Idle;
            emit talking(false);
            emit changed();
            return true;
        }
        for (int l = 0; l < m_lists.size(); ++l)
            log(QString("list F2%1: %2").arg(QString::number(m_listBase + l, 16).rightJustified(2, '0').toUpper(),
                                            didList(m_lists[l])));
        m_lastRefusal.clear();
        m_steps << Step{QByteArray::fromHex("1003"), [this](const QByteArray *r) {
            if (r && quint8((*r)[0]) == 0x50) return true;
            giveUp(r ? QString("the engine computer refused the extended diagnostic session (NRC %1)")
                           .arg(QString::number(r->size() > 2 ? quint8((*r)[2]) : 0, 16).rightJustified(2, '0').toUpper())
                     : QStringLiteral("no answer to the extended diagnostic session request"));
            return false;
        }};
        for (int l = 0; l < m_lists.size(); ++l) {
            // Clear first; "nothing to clear" refusals don't matter.
            m_steps << Step{QByteArray::fromHex("2C03F2") + char(m_listBase + l), [](const QByteArray *) { return true; }};
            for (quint16 did : std::as_const(m_lists[l])) {
                // Ford takes one source per definition request (four at once was refused).
                QByteArray req = QByteArray::fromHex("2C01F2");
                req += char(m_listBase + l);
                req += char(did >> 8); req += char(did & 0xFF); req += char(1); req += char(m_length[did]);
                m_steps << Step{req, [this, l, did](const QByteArray *r) {
                    if (r && quint8((*r)[0]) == 0x6C) return true;
                    // Readable but not allowed in a stream list (or no answer): that value stays
                    // polled, the others still stream. The frame then simply doesn't carry it.
                    m_lists[l].removeAll(did);
                    m_rejected.insert(did);
                    m_wanted.removeAll(did);
                    if (r && r->size() >= 3 && quint8((*r)[0]) == 0x7F)
                        m_lastRefusal = "NRC " + QString::number(quint8((*r)[2]), 16).rightJustified(2, '0').toUpper();
                    log(QString("%1 refused for a list (%2): polled").arg(hex16(did), r ? m_lastRefusal : "no answer"));
                    return true;
                }};
            }
        }
        // Start every list that ended up with something in it.
        m_steps << Step{QByteArray(), [this](const QByteArray *) {
            QByteArray start = QByteArray::fromHex("2A03");   // 03 = fast
            for (int l = 0; l < m_lists.size(); ++l)
                if (!m_lists[l].isEmpty()) start += char(m_listBase + l);
            if (start.size() == 2) {
                giveUp("the engine computer wouldn't put any of these values in a stream list" +
                       (m_lastRefusal.isEmpty() ? QString() : " (" + m_lastRefusal + ")"));
                return false;
            }
            m_steps.prepend(Step{start, [this](const QByteArray *r) {
                if (r && quint8((*r)[0]) == 0x7F) {
                    giveUp(QString("the engine computer refused to start streaming (NRC %1)")
                               .arg(QString::number(r->size() > 2 ? quint8((*r)[2]) : 0, 16).rightJustified(2, '0').toUpper()));
                    return false;
                }
                // Some ECUs just start sending without a reply: the watchdog decides.
                m_state = State::Streaming;
                m_streamStart = m_lastSummary = m_clock.elapsed();
                m_keepAlive.start();
                m_watchdog.start();
                m_health.start();
                log("streaming started");
                emit talking(false);
                return true;
            }});
            return true;
        }};
        return true;
    }};
    // Let a poll request that is already on its way get its answer first.
    QTimer::singleShot(250, this, &StreamSession::runNext);
}

void StreamSession::runNext() {
    if (m_tp->busy()) return;   // a request is on its way; its answer calls runNext again
    while (!m_steps.isEmpty() && m_steps.first().request.isEmpty()) {
        const Step s = m_steps.takeFirst();
        if (!s.handle(nullptr)) return;   // gave up: cleanup has been queued and runs on its own
    }
    if (!m_steps.isEmpty()) {
        if (m_tp->request(kEcuRequest, kEcuReply, kEcuReply, m_steps.first().request, 300))
            log("-> " + hex(m_steps.first().request));
        else
            QTimer::singleShot(20, this, &StreamSession::runNext);   // not connected / busy: try again shortly
        return;
    }
    if (m_state == State::Stopping) {
        const bool restart = m_restartAfterStop && !m_unavailable && !m_wanted.isEmpty();
        reset();
        log(restart ? "old lists cleared, setting up again" : "stopped, everything polled");
        if (!restart) emit talking(false);
        emit changed();
        if (restart) startSetup();
    }
}

void StreamSession::onResponse(quint32, const QByteArray &payload) {
    if (m_steps.isEmpty()) return;
    log("<- " + hex(payload));
    // 7F xx 21 = "busy, ask again": not a refusal.
    if (payload.size() >= 3 && quint8(payload[0]) == 0x7F && quint8(payload[2]) == 0x21 && ++m_retries < kTries) {
        QTimer::singleShot(50, this, &StreamSession::runNext);
        return;
    }
    m_retries = 0;
    const Step s = m_steps.takeFirst();
    if (s.handle(&payload)) runNext();
}

void StreamSession::onTimeout() {
    if (m_steps.isEmpty()) return;
    log(QString("<- no answer (try %1)").arg(m_retries + 1));
    // A reply can get lost on a busy bus: ask again before calling it a refusal.
    if (++m_retries < kTries) { runNext(); return; }
    m_retries = 0;
    const Step s = m_steps.takeFirst();
    if (s.handle(nullptr)) runNext();
}

void StreamSession::onFrame(const QCanBusFrame &f) {
    if (m_state != State::Streaming || f.hasExtendedFrameFormat()) return;
    const quint32 id = f.frameId();
    if (id != 0x6A0 && id != 0x6A1) return;
    const QByteArray p = f.payload();
    const int list = p.isEmpty() ? -1 : int(quint8(p[0])) - m_listBase;
    if (list < 0 || list >= m_lists.size()) return;
    ++m_frameCount[(quint64(id) << 8) | quint64(list)];
    // Each list is taken from the first ID it shows up on: the PCM may repeat a list on the other
    // ID, or send different lists on each — both work this way.
    auto lock = m_listCanId.find(list);
    if (lock == m_listCanId.end()) {
        lock = m_listCanId.insert(list, id);
        log(QString("list F2%1 arriving on %2").arg(QString::number(m_listBase + list, 16).rightJustified(2, '0').toUpper(),
                                                    QString::number(id, 16).toUpper()));
    }
    if (*lock != id) return;
    ++m_frames;
    m_watchdog.start();
    m_listLast[list] = m_clock.elapsed();
    // A list that (re)starts arriving: its values count as streamed from now on.
    bool added = false;
    for (quint16 did : std::as_const(m_lists[list]))
        if (!m_streamingSet.contains(did)) { m_streamingSet.insert(did); added = true; }
    if (added) emit changed();
    int pos = 1;
    for (quint16 did : std::as_const(m_lists[list])) {
        const int len = m_length.value(did);
        if (pos + len > p.size()) break;
        emit data(did, p.mid(pos, len));
        pos += len;
    }
}

void StreamSession::checkHealth() {
    if (m_state != State::Streaming) return;
    const qint64 now = m_clock.elapsed();
    QSet<quint16> alive;
    QList<int> neverCame;
    for (int l = 0; l < m_lists.size(); ++l) {
        if (m_lists[l].isEmpty()) continue;
        auto last = m_listLast.constFind(l);
        if (last != m_listLast.constEnd()) {
            if (now - *last <= kAliveMs)
                for (quint16 did : std::as_const(m_lists[l])) alive.insert(did);
        } else if (now - m_streamStart > kFirstFrameMs) {
            neverCame << l;
        }
    }
    // A list the PCM never sends (e.g. more lists than it will run): its values are polled for
    // the rest of this connection, so they can't freeze and don't cause another rebuild.
    for (int l : neverCame) {
        log(QString("list F2%1 never arrived: %2 polled from now on")
                .arg(QString::number(m_listBase + l, 16).rightJustified(2, '0').toUpper(), didList(m_lists[l])));
        for (quint16 did : std::as_const(m_lists[l])) {
            m_rejected.insert(did);
            m_wanted.removeAll(did);
        }
        m_lists[l].clear();
    }
    if (alive != m_streamingSet) {
        const QSet<quint16> stopped = m_streamingSet - alive;
        if (!stopped.isEmpty()) log("stopped arriving, polled until they come back: " + didList(stopped.values()));
        m_streamingSet = alive;
        emit changed();
    } else if (!neverCame.isEmpty()) {
        emit changed();
    }
    if (now - m_lastSummary >= 5000) {   // what actually arrives, per list and ID
        m_lastSummary = now;
        QStringList parts;
        for (int l = 0; l < m_lists.size(); ++l) {
            const int a = m_frameCount.value((quint64(0x6A0) << 8) | quint64(l));
            const int b = m_frameCount.value((quint64(0x6A1) << 8) | quint64(l));
            parts << QString("F2%1 6A0:%2 6A1:%3").arg(QString::number(m_listBase + l, 16).rightJustified(2, '0').toUpper())
                         .arg(a).arg(b);
        }
        log("frames so far: " + parts.join(" | "));
    }
}

void StreamSession::giveUp(const QString &why) {
    log("giving up: " + why);
    m_unavailable = true;
    m_why = why;
    m_restartAfterStop = false;
    m_tp->cancel();
    m_steps.clear();
    m_keepAlive.stop();
    m_watchdog.stop();
    m_health.stop();
    m_streamingSet.clear();
    emit changed();
    queueCleanup();
}

void StreamSession::queueCleanup() {
    m_state = State::Stopping;
    emit talking(true);
    m_steps.clear();
    auto ignore = [](const QByteArray *) { return true; };
    if (!m_lists.isEmpty()) {
        QByteArray stop = QByteArray::fromHex("2A04");
        for (int l = 0; l < m_lists.size(); ++l) stop += char(m_listBase + l);
        m_steps << Step{stop, ignore};
        for (int l = 0; l < m_lists.size(); ++l)
            m_steps << Step{QByteArray::fromHex("2C03F2") + char(m_listBase + l), ignore};
    }
    m_steps << Step{QByteArray::fromHex("1001"), ignore};
    QTimer::singleShot(250, this, &StreamSession::runNext);   // let a poll already on its way finish
}
