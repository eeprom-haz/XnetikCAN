#include "ReplayEngine.h"
#include "CanBridge.h"
#include <QDateTime>

ReplayEngine::ReplayEngine(CanBridge *bridge, QObject *parent)
    : QObject(parent), m_bridge(bridge)
{
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, &ReplayEngine::sendNext);
}

void ReplayEngine::setFrames(const QVector<QCanBusFrame> &frames) {
    m_frames = frames;
}

void ReplayEngine::start() {
    if (m_frames.isEmpty() || m_running) return;
    m_index = 0;
    m_running = true;
    m_startFrameMs = tsToMs(m_frames[0].timeStamp());
    m_wallStartMs = QDateTime::currentMSecsSinceEpoch();
    scheduleNext();
}

void ReplayEngine::stop() {
    m_timer.stop();
    m_running = false;
}

qint64 ReplayEngine::tsToMs(const QCanBusFrame::TimeStamp &ts) {
    return ts.seconds() * 1000 + ts.microSeconds() / 1000;
}

void ReplayEngine::scheduleNext() {
    if (m_index >= m_frames.size()) {
        m_running = false;
        emit finished();
        return;
    }
    qint64 frameOffset = tsToMs(m_frames[m_index].timeStamp()) - m_startFrameMs;
    qint64 scaledOffset = static_cast<qint64>(frameOffset / m_scale);
    qint64 elapsed = QDateTime::currentMSecsSinceEpoch() - m_wallStartMs;
    int delay = static_cast<int>(qMax(0LL, scaledOffset - elapsed));
    m_timer.start(delay);
}

void ReplayEngine::sendNext() {
    if (m_index >= m_frames.size()) {
        m_running = false;
        emit finished();
        return;
    }
    if (m_send) m_send(m_frames[m_index]);
    else m_bridge->writeFrame(m_frames[m_index]);
    emit progress(++m_index, m_frames.size());
    scheduleNext();
}
