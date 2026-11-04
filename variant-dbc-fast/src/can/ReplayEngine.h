#pragma once
#include <QObject>
#include <QCanBusFrame>
#include <QVector>
#include <QTimer>
#include <functional>

class CanBridge;

class ReplayEngine : public QObject {
    Q_OBJECT
public:
    explicit ReplayEngine(CanBridge *bridge, QObject *parent = nullptr);

    void setFrames(const QVector<QCanBusFrame> &frames);
    void setSpeedScale(double scale) { m_scale = scale; }
    // By default frames go out the J2534 bridge. Set a sender to replay out another device (Snort/C31G).
    void setSender(std::function<bool(const QCanBusFrame &)> fn) { m_send = std::move(fn); }

    void start();
    void stop();
    bool isRunning() const { return m_running; }

signals:
    void progress(int sent, int total);
    void finished();

private slots:
    void sendNext();

private:
    void scheduleNext();
    static qint64 tsToMs(const QCanBusFrame::TimeStamp &ts);

    CanBridge *m_bridge;
    std::function<bool(const QCanBusFrame &)> m_send;   // if set, used instead of the bridge
    QVector<QCanBusFrame> m_frames;
    QTimer m_timer;
    int m_index = 0;
    double m_scale = 1.0;
    bool m_running = false;
    qint64 m_startFrameMs = 0;
    qint64 m_wallStartMs = 0;
};
