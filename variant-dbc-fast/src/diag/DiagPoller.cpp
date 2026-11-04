#include "DiagPoller.h"
#include "IsoTp.h"
#include "../can/CanBridge.h"
#include "../obd/ObdPids.h"
#include "DiagLog.h"
#include <cmath>

static constexpr quint32 kFunctional = 0x7DF;

DiagPoller::DiagPoller(CanBridge *bridge, QObject *parent)
    : QObject(parent), m_bridge(bridge), m_tp(new IsoTp(bridge, this)) {
    m_gap.setSingleShot(true);
    connect(&m_gap, &QTimer::timeout, this, &DiagPoller::sendNext);
    connect(m_tp, &IsoTp::response, this, &DiagPoller::onResponse);
    connect(m_tp, &IsoTp::timedOut, this, &DiagPoller::onTimeout);
    connect(m_bridge, &CanBridge::stateChanged, this, [this](bool connected) {
        if (!connected) {
            stop();
            m_groupingOff = m_groupingWorked = false;   // the next vehicle gets a fresh try
            m_groupMisses = 0;
            m_sessions.clear();                          // sessions and what was learned belong
            m_needSession.clear();                       // to this connection's vehicle
            m_sessionAddr.clear();
            m_sessionOut.clear();
            m_leaveForObd = false;
            m_obdMissInSession = 0;
        }
    });
    m_clock.start();
}

void DiagPoller::setItems(const QList<Item> &items) {
    m_raw = items;
    regroup();
    if (m_mode == Mode::Polling && !m_inFlight && !m_gap.isActive()) scheduleNext();
}

void DiagPoller::regroup() {
    m_items.clear();
    QVector<bool> isGroup;   // per entry of m_items: a standard group that may take more PIDs
    // Standard values first, then manufacturer / own values: each kind as one block per round,
    // so a module only switches session twice a round (not between every request).
    // Priority items (the overlay) lead their block and come again after every few others.
    QList<Item> ordered;
    for (bool manufacturer : {false, true}) {
        QList<Item> prio, norm;
        for (const Item &raw : std::as_const(m_raw))
            if ((raw.isOe || raw.isDid) == manufacturer) (raw.priority ? prio : norm) << raw;
        // Again after as many others as there are priority items (at least kPriorityEvery), so
        // 10 overlay values don't starve the ticked ones.
        const int every = qMax(kPriorityEvery, int(prio.size()));
        ordered << prio;
        for (int i = 0; i < norm.size(); ++i) {
            ordered << norm[i];
            if (!prio.isEmpty() && (i + 1) % every == 0 && i + 1 < norm.size()) ordered << prio;
        }
    }
    for (const Item &raw : std::as_const(ordered)) {
        Item it = raw;
        it.pids = {it.pid};
        const bool obd = !it.isOe && !it.isDid;
        const QList<const ObdPid *> defs = obd ? findObdPid(it.pid) : QList<const ObdPid *>();
        const bool groupable = obd && !m_groupingOff && !m_udsStandard && !defs.isEmpty() &&
                               defs.first()->bytes <= kMaxGroupedPidBytes;
        // Join the previous entry when it is a standard group with room, else start a new one.
        if (groupable && !isGroup.isEmpty() && isGroup.last() && m_items.last().pids.size() < kMaxPidsPerRequest &&
            !m_items.last().pids.contains(it.pid)) {
            m_items.last().pids << it.pid;
            m_items.last().obdValues << it.obdValues;
        } else {
            m_items << it;
            isGroup << groupable;
        }
    }
    if (m_index >= m_items.size()) m_index = 0;
}

void DiagPoller::start() {
    if (!m_bridge->isConnected()) return;
    m_mode = Mode::Polling;
    m_replies = m_timeouts = 0;
    m_index = 0;
    scheduleNext();
}

void DiagPoller::stop() {
    m_mode = Mode::Idle;
    m_gap.stop();
    m_tp->cancel();
    m_inFlight = false;
}

void DiagPoller::discoverPids() {
    if (!m_bridge->isConnected()) return;
    stop();
    m_mode = Mode::Discovering;
    m_supported.clear();
    m_discoverPid = 0x00;
    m_inFlight = m_tp->request(kFunctional, 0x7E8, 0x7EF, QByteArray("\x01\x00", 2));
}

void DiagPoller::scheduleNext() {
    if (m_mode == Mode::Polling && !m_items.isEmpty()) m_gap.start(kGapMs);
}

IsoTp::Address DiagPoller::addressOf(const Item &item) {
    IsoTp::Address a;
    a.tx = item.tx;
    a.rxMin = a.rxMax = item.rx;
    a.ext29 = item.ext29;
    a.txEa = item.txEa;
    a.rxEa = item.rxEa;
    return a;
}

QString DiagPoller::itemText(const Item &item) const {
    if (item.isOe) return item.module + " " + QString::fromLatin1(item.request.toHex(' ').toUpper());
    if (item.isDid) return QString::number(item.did.requestId, 16).toUpper() + " 22 " +
                           QString::number(item.did.did, 16).rightJustified(4, '0').toUpper();
    QStringList pids;
    for (quint8 p : item.pids.isEmpty() ? QVector<quint8>{item.pid} : item.pids)
        pids << QString::number(p, 16).rightJustified(2, '0').toUpper();
    return m_udsStandard ? "7E0 22 F4" + pids.join("") : "7DF 01 " + pids.join(' ');
}

bool DiagPoller::sendOe(const Item &item) {
    const quint8 want = item.session ? item.session : m_needSession.value(item.module, 0);
    if (want) {
        SessionState &st = m_sessions[item.module];
        const qint64 now = m_clock.elapsed();
        if ((st.session != want || now - st.lastMs > kSessionHoldMs) && now >= st.failedUntil) {
            m_sessionStep = true;
            m_sessionWant = want;
            m_sessionAddr.insert(item.module, addressOf(item));
            if (item.sessionOut) m_sessionOut.insert(item.module, item.sessionOut);
            QByteArray req(2, '\0');
            req[0] = 0x10;
            req[1] = char(want);
            return m_tp->request(addressOf(item), req, timeoutOf(item));
        }
    }
    m_sessionStep = false;
    return m_tp->request(addressOf(item), item.request, timeoutOf(item));
}

QString DiagPoller::heldObdSession() const {
    for (auto it = m_sessions.cbegin(); it != m_sessions.cend(); ++it) {
        if (!it->session) continue;
        const IsoTp::Address a = m_sessionAddr.value(it.key());
        if (!a.ext29 && a.txEa < 0 && a.tx >= 0x7E0 && a.tx <= 0x7E7) return it.key();
    }
    return {};
}

bool DiagPoller::handleSessionStep(const QByteArray *reply) {
    if (m_leaveStep) {
        // Back to normal mode before a standard request; the next manufacturer request re-opens it.
        m_leaveStep = false;
        m_sessions[m_leaveModule].session = 0;
        const QString what = m_leaveModule + " back to normal mode for standard values";
        if (!m_loggedOnce.contains(what)) {
            m_loggedOnce.insert(what);
            DiagLog::write("requests", what + " -> " + (reply ? QString::fromLatin1(reply->toHex(' ').toUpper())
                                                              : QStringLiteral("no answer")));
        }
        m_inFlight = sendActual(m_current);
        if (!m_inFlight) afterNoSend();
        return true;
    }
    if (!m_sessionStep) return false;
    m_sessionStep = false;
    SessionState &st = m_sessions[m_current.module];
    const qint64 now = m_clock.elapsed();
    const QString what = QString("%1 session 10 %2").arg(m_current.module)
                             .arg(QString::number(m_sessionWant, 16).rightJustified(2, '0').toUpper());
    if (reply && !reply->isEmpty() && quint8((*reply)[0]) == 0x50) {
        st.session = m_sessionWant;
        st.lastMs = now;
        if (!m_loggedOnce.contains(what)) {
            m_loggedOnce.insert(what);
            DiagLog::write("requests", what + " -> opened");
        }
    } else {
        // Refused or silent: don't hammer it; ask the value anyway and try again in 10 s.
        st.session = 0;
        st.failedUntil = now + 10000;
        DiagLog::write("requests", what + " -> " + (reply ? QString::fromLatin1(reply->toHex(' ').toUpper())
                                                          : QStringLiteral("no answer")));
    }
    m_inFlight = sendActual(m_current);
    if (!m_inFlight) afterNoSend();
    return true;
}

bool DiagPoller::retryInSession(const QByteArray &p) {
    if (!m_current.isOe || m_retriedSession || p.size() < 3 || quint8(p[0]) != 0x7F) return false;
    const quint8 nrc = quint8(p[2]);
    // 7F serviceNotSupportedInActiveSession, 7E subFunction…InActiveSession (UDS),
    // 80 serviceNotSupportedInActiveDiagnosticMode (KWP2000, e.g. Nissan).
    if (nrc != 0x7F && nrc != 0x7E && nrc != 0x80) return false;
    m_retriedSession = true;
    if (m_current.session) {
        m_sessions[m_current.module].session = 0;   // it dropped out: open it again
    } else if (!m_needSession.contains(m_current.module)) {
        const quint8 s = m_current.fallbackSession ? m_current.fallbackSession : quint8(0x03);
        m_needSession.insert(m_current.module, s);
        DiagLog::write("requests", QString("%1 answered \"wrong session\" (NRC %2): asking in session 10 %3 from now on")
                                       .arg(m_current.module)
                                       .arg(QString::number(nrc, 16).rightJustified(2, '0').toUpper())
                                       .arg(QString::number(s, 16).rightJustified(2, '0').toUpper()));
    } else {
        m_sessions[m_current.module].session = 0;
    }
    m_sessions[m_current.module].failedUntil = 0;
    m_inFlight = sendOe(m_current);
    if (!m_inFlight) afterNoSend();
    return true;
}

void DiagPoller::afterNoSend() {
    if (m_mode == Mode::Probing) probeDone(false, "Could not send the request");
    else scheduleNext();
}

void DiagPoller::logPollSummary() {
    const qint64 now = m_clock.elapsed();
    if (now - m_lastSummary < 10000) return;
    m_lastSummary = now;
    QStringList parts;
    for (auto it = m_counts.cbegin(); it != m_counts.cend(); ++it)
        parts << QString("%1 %2 asked/%3 answered/%4 refused/%5 missed").arg(it.key()).arg(it->asked)
                     .arg(it->answered).arg(it->refused).arg(it->missed);
    if (!parts.isEmpty()) DiagLog::write("requests", "polling (last 10 s): " + parts.join(" | "));
    m_counts.clear();
}

bool DiagPoller::sendItem(const Item &item) {
    if (item.isOe)
        return sendOe(item);
    // Standard value while an engine-bay module is held in a session that blocks standard requests
    // (learned): take it back to normal mode first.
    if (!item.isDid && !m_udsStandard && m_leaveForObd && m_mode == Mode::Polling) {
        const QString mod = heldObdSession();
        if (!mod.isEmpty()) {
            const quint8 held = m_sessions.value(mod).session;
            const quint8 out = m_sessionOut.value(mod, held >= 0x80 ? quint8(0x81) : quint8(0x01));
            QByteArray req(2, '\0');
            req[0] = 0x10;
            req[1] = char(out);
            m_leaveStep = true;
            m_leaveModule = mod;
            return m_tp->request(m_sessionAddr.value(mod), req, 250);
        }
    }
    return sendActual(item);
}

bool DiagPoller::sendActual(const Item &item) {
    if (item.isOe)
        return m_tp->request(addressOf(item), item.request, timeoutOf(item));
    if (item.isDid) {
        QByteArray req(3, '\0');
        req[0] = 0x22;
        req[1] = char(item.did.did >> 8);
        req[2] = char(item.did.did & 0xFF);
        return m_tp->request(item.did.requestId, item.did.responseId, item.did.responseId, req, 250);
    }
    if (m_udsStandard) {   // 22 F4 <pid> straight to the engine computer, one value per request
        QByteArray req(3, '\0');
        req[0] = 0x22;
        req[1] = char(0xF4);
        req[2] = char(item.pid);
        return m_tp->request(0x7E0, 0x7E8, 0x7E8, req, 250);
    }
    QByteArray req(1, char(0x01));
    for (quint8 p : item.pids.isEmpty() ? QVector<quint8>{item.pid} : item.pids) req += char(p);
    return m_tp->request(kFunctional, 0x7E8, 0x7EF, req);
}

void DiagPoller::setStandardAsUds(bool on) {
    if (on == m_udsStandard) return;
    m_udsStandard = on;
    regroup();
}

void DiagPoller::setPaused(bool paused) {
    m_paused = paused;
    if (paused) m_gap.stop();
    else if (m_mode == Mode::Polling && !m_inFlight && !m_gap.isActive()) scheduleNext();
}

void DiagPoller::sendNext() {
    if (m_mode != Mode::Polling || m_paused || m_items.isEmpty() || m_inFlight) return;
    if (m_index >= m_items.size()) m_index = 0;
    m_current = m_items[m_index++];
    m_retriedSession = false;
    logPollSummary();
    ++m_counts[m_current.isOe ? m_current.module : QStringLiteral("OBD")].asked;
    m_inFlight = sendItem(m_current);
    if (!m_inFlight) scheduleNext();
}

void DiagPoller::probe(const QList<Item> &items) {
    if (!m_bridge->isConnected()) return;
    stop();
    m_mode = Mode::Probing;
    m_probeItems = items;
    m_probeIndex = 0;
    m_silentStreak.clear();
    m_answeredIds.clear();
    probeNext();
}

void DiagPoller::probeNext() {
    if (m_inFlight) return;
    while (m_mode == Mode::Probing && m_probeIndex < m_probeItems.size()) {
        const Item &item = m_probeItems[m_probeIndex];
        const QString mod = item.isOe ? item.module : QString::number(item.tx, 16);
        // A module that never answered 3 requests in a row isn't there: skip the rest of it.
        if (!m_answeredIds.contains(mod) && m_silentStreak.value(mod) >= 3) {
            emit probeResult(m_probeIndex++, false, "This module did not answer");
            continue;
        }
        m_current = item;
        m_retriedSession = false;
        m_inFlight = sendItem(item);
        if (m_inFlight) return;
        emit probeResult(m_probeIndex++, false, "Could not send the request");
    }
    if (m_mode == Mode::Probing) {
        m_mode = Mode::Idle;
        emit probeFinished();
    }
}

void DiagPoller::probeDone(bool answered, const QString &why) {
    if (m_probeIndex >= m_probeItems.size()) return;
    const Item &item = m_probeItems[m_probeIndex];
    const QString mod = item.isOe ? item.module : QString::number(item.tx, 16);
    if (answered || !why.isEmpty()) {   // a refusal still proves the module is there
        m_answeredIds.insert(mod);
        m_silentStreak.remove(mod);
    } else {                            // silence
        ++m_silentStreak[mod];
    }
    emit probeResult(m_probeIndex++, answered, why.isEmpty() ? QStringLiteral("No answer") : why);
    // Small gap so the bus isn't flooded during the sweep.
    QTimer::singleShot(kGapMs, this, &DiagPoller::probeNext);
}

void DiagPoller::onResponse(quint32, const QByteArray &p) {
    m_inFlight = false;
    if (m_mode == Mode::Discovering) { handleDiscovery(p); return; }
    if (p.isEmpty() || handleSessionStep(&p)) return;
    if (m_mode == Mode::Probing) {
        if (retryInSession(p)) return;
        const QString line = "check " + itemText(m_current) + " -> " + QString::fromLatin1(p.left(12).toHex(' ').toUpper());
        DiagLog::write("requests", line);
        if (quint8(p[0]) == 0x7F)
            probeDone(false, "ECU refused (NRC " +
                                 QString::number(p.size() > 2 ? quint8(p[2]) : 0, 16).rightJustified(2, '0').toUpper() + ")");
        else
            probeDone(true, {});
        return;
    }
    if (m_mode != Mode::Polling) return;

    const quint8 sid = quint8(p[0]);
    Counts &counts = m_counts[m_current.isOe ? m_current.module : QStringLiteral("OBD")];
    if (sid == 0x7F && retryInSession(p)) return;
    if (m_current.isOe) m_sessions[m_current.module].lastMs = m_clock.elapsed();   // session still in use
    if (sid == 0x7F) {  // negative response, e.g. 7F 22 31 = request out of range
        ++m_timeouts;
        ++counts.refused;
        const quint8 nrc = p.size() > 2 ? quint8(p[2]) : 0;
        const QString why = "ECU refused (NRC " + QString::number(nrc, 16).rightJustified(2, '0').toUpper() + ")";
        const QString once = itemText(m_current) + " " + why;
        if (!m_loggedOnce.contains(once)) {
            m_loggedOnce.insert(once);
            DiagLog::write("requests", "poll " + once);
        }
        if (m_current.isOe) {
            for (const auto &ks : m_current.oeSignals) emit itemFailed(ks.first, why);
        } else if (!m_current.obdValues.isEmpty() && m_current.request.isEmpty() &&
                   m_udsStandard) {
            // Refused as 22 F4xx says nothing about the standard value itself (it may only answer
            // as Mode 01): keep it ticked, it is asked the normal way once streaming stops.
        } else if (!m_current.obdValues.isEmpty()) {
            if (m_current.pids.size() > 1) {
                // Refusing a grouped request says nothing about each value: ask them one at a time.
                m_groupingOff = true;
                regroup();
            } else {
                for (const auto &kv : m_current.obdValues) emit itemFailed(kv.first, why);
            }
        } else {
            emit itemFailed(m_current.key, why);
        }
    } else if (m_current.isOe && !m_current.request.isEmpty() &&
               sid == quint8(m_current.request[0]) + 0x40) {
        ++counts.answered;
        // IsoTp already matched the PID/DID echo; values start right after it.
        const QByteArray data = p.mid(m_current.request.size());
        const double t = elapsed();
        for (const auto &ks : m_current.oeSignals) {
            double v;
            QString text;
            if (ks.second.decode(data, &v, &text)) {
                emit sample(ks.first, t, v, text);
                ++m_replies;
            }
        }
    } else if (!m_current.isOe && !m_current.isDid && sid == 0x41) {
        ++counts.answered;
        m_obdMissInSession = 0;
        // One reply may carry several PIDs (41 PID data PID data ...).
        const double t = elapsed();
        const int found = parseMode01Reply(p, [&](quint8 pid, const quint8 *d, int len) {
            for (const auto &kv : m_current.obdValues) {
                const ObdPid *def = kv.second;
                if (def->pid != pid) continue;
                if (len < def->bytes) {
                    emit itemFailed(kv.first, "Reply shorter than the standard expects");
                    continue;
                }
                const double v = def->decode(d);
                if (std::isnan(v)) continue;   // e.g. O2 sensor not used for trim
                emit sample(kv.first, t, v, def->text ? def->text(d) : QString());
                ++m_replies;
            }
        });
        if (found > 0 && m_current.pids.size() > 1) {
            m_groupingWorked = true;
            m_groupMisses = 0;
        }
    } else if (!m_current.isOe && !m_current.isDid && sid == 0x62 && p.size() >= 3 &&
               quint8(p[1]) == 0xF4 && quint8(p[2]) == m_current.pid) {
        ++counts.answered;
        // Standard value read as 22 F4xx: same data bytes as its Mode 01 reply.
        const QByteArray data = p.mid(3);
        const quint8 *d = reinterpret_cast<const quint8 *>(data.constData());
        const double t = elapsed();
        for (const auto &kv : m_current.obdValues) {
            const ObdPid *def = kv.second;
            if (def->pid != m_current.pid || data.size() < def->bytes) continue;
            const double v = def->decode(d);
            if (std::isnan(v)) continue;
            emit sample(kv.first, t, v, def->text ? def->text(d) : QString());
            ++m_replies;
        }
    } else if (m_current.isDid && sid == 0x62 && p.size() >= 3 &&
               ((quint8(p[1]) << 8) | quint8(p[2])) == m_current.did.did) {
        double v;
        if (m_current.did.decode(p.mid(3), &v)) {
            ++m_replies;
            emit sample(m_current.key, elapsed(), v, QString());
        } else {
            emit itemFailed(m_current.key, "Reply shorter than the definition expects");
        }
    }
    scheduleNext();
}

void DiagPoller::onTimeout() {
    m_inFlight = false;
    if (m_mode == Mode::Discovering) {
        m_mode = Mode::Idle;
        if (m_supported.isEmpty()) emit discoveryFailed("No ECU answered (Mode 01 PID 00 on 0x7DF)");
        else emit pidsDiscovered(m_supported);
        return;
    }
    if (handleSessionStep(nullptr)) return;
    if (m_mode == Mode::Probing) {
        DiagLog::write("requests", "check " + itemText(m_current) + " -> no answer");
        probeDone(false, {});
        return;
    }
    // A missed reply is only counted; nothing is dropped. An ECU that never answers grouped
    // requests gets one PID at a time from then on.
    ++m_timeouts;
    ++m_counts[m_current.isOe ? m_current.module : QStringLiteral("OBD")].missed;
    // Standard requests unanswered while an engine-bay module sits in a session: that session
    // blocks them (seen on the INFINITI Q50). From now on standard rounds leave it first.
    if (!m_current.isOe && !m_current.isDid && !m_udsStandard && !m_leaveForObd &&
        !heldObdSession().isEmpty() && ++m_obdMissInSession >= 2) {
        m_leaveForObd = true;
        DiagLog::write("requests", "standard values go unanswered while " + heldObdSession() +
                                       " is in its session: standard and manufacturer values now take turns");
    }
    if (!m_current.isOe && !m_current.isDid && m_current.pids.size() > 1 && !m_groupingWorked &&
        ++m_groupMisses >= kGroupGiveUp) {
        m_groupingOff = true;
        regroup();
    }
    scheduleNext();
}

void DiagPoller::handleDiscovery(const QByteArray &p) {
    if (p.size() >= 6 && quint8(p[0]) == 0x41 && quint8(p[1]) == m_discoverPid) {
        const quint32 bits = (quint32(quint8(p[2])) << 24) | (quint32(quint8(p[3])) << 16) |
                             (quint32(quint8(p[4])) << 8) | quint8(p[5]);
        for (int i = 0; i < 32; ++i)
            if (bits & (0x80000000u >> i)) m_supported.insert(quint8(m_discoverPid + i + 1));
        if ((bits & 1u) && m_discoverPid < 0xE0) {
            m_discoverPid += 0x20;
            QByteArray req(2, '\0');
            req[0] = 0x01;
            req[1] = char(m_discoverPid);
            m_inFlight = m_tp->request(kFunctional, 0x7E8, 0x7EF, req);
            if (m_inFlight) return;
        }
    }
    m_mode = Mode::Idle;
    if (m_supported.isEmpty()) emit discoveryFailed("ECU did not report any supported PIDs");
    else emit pidsDiscovered(m_supported);
}
