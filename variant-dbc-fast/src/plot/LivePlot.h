#pragma once
#include <QWidget>
#include <QTimer>
#include <QColor>
#include <QMap>
#include <QVector>
#include <QPointF>

// Stacked strip charts: one lane per series, shared scrolling time axis,
// per-lane autoscale. Repaints at most ~30x/s, only when new data arrived.
class LivePlot : public QWidget {
    Q_OBJECT
public:
    explicit LivePlot(QWidget *parent = nullptr);

    // rangeMin == rangeMax means "no typical range known": auto-scale that lane.
    void addSeries(int key, const QString &name, const QString &unit,
                   double rangeMin = 0, double rangeMax = 0);
    void removeSeries(int key);
    // Samples stay as read; the lane shows value * scale + offset in `unit` (e.g. °C -> °F).
    void setDisplay(int key, const QString &unit, double scale, double offset);
    void addSample(int key, double t, double value);
    void clearData();

    void setWindowSeconds(double s) { m_window = s; m_dirty = true; }
    void setPaused(bool p) { m_paused = p; m_dirty = true; }
    bool isPaused() const { return m_paused; }
    // Fixed ranges (default) read like a scan tool; auto-fit zooms in on the data.
    void setAutoFit(bool on) { m_autoFit = on; m_dirty = true; }

    static constexpr int kMinLaneHeight = 120;

protected:
    void paintEvent(QPaintEvent *) override;

private:
    struct Series {
        QString name;
        QString unit;
        double rangeMin = 0;
        double rangeMax = 0;
        double scale = 1, offset = 0;   // display conversion
        QColor color;
        QVector<QPointF> pts;  // x = seconds, y = value
    };

    void paintLane(QPainter &p, const QRectF &r, const Series &s, double tEnd) const;
    void updateMinimumHeight();

    QMap<int, Series> m_series;  // ordered by key = stable lane order
    QTimer m_repaint;
    double m_window = 10.0;
    double m_latest = 0.0;
    double m_frozenEnd = 0.0;
    bool m_paused = false;
    bool m_autoFit = false;
    bool m_dirty = false;
    int m_colorIndex = 0;
};
