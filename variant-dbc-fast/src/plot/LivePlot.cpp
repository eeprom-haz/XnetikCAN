#include "LivePlot.h"
#include <QPainter>
#include <QPainterPath>
#include <cmath>
#include <algorithm>

// Bright neon colours (user: "bright", "neon" — never dark ones), easy to tell apart at a glance.
static const QColor kPalette[] = {
    QColor("#00C8FF"), QColor("#FF2BD6"), QColor("#2EE60F"), QColor("#FF8C00"),
    QColor("#B026FF"), QColor("#FF1F4B"), QColor("#00E5C0"), QColor("#4D9CFF"),
};

static constexpr double kKeepSeconds = 300.0;  // history kept per series (in memory only), for "Save last …"

LivePlot::LivePlot(QWidget *parent) : QWidget(parent) {
    setMinimumSize(320, 160);
    setAutoFillBackground(false);
    connect(&m_repaint, &QTimer::timeout, this, [this] {
        if (m_dirty) { m_dirty = false; update(); }
    });
    m_repaint.start(33);
}

void LivePlot::addSeries(int key, const QString &name, const QString &unit,
                         double rangeMin, double rangeMax) {
    if (m_series.contains(key)) return;
    Series s;
    s.name = name;
    s.unit = unit;
    s.rangeMin = rangeMin;
    s.rangeMax = rangeMax;
    s.color = kPalette[m_colorIndex++ % (sizeof(kPalette) / sizeof(kPalette[0]))];
    m_series.insert(key, s);
    updateMinimumHeight();
    m_dirty = true;
}

void LivePlot::removeSeries(int key) {
    m_series.remove(key);
    updateMinimumHeight();
    m_dirty = true;
}

void LivePlot::setDisplay(int key, const QString &unit, double scale, double offset) {
    auto it = m_series.find(key);
    if (it == m_series.end()) return;
    it->unit = unit;
    it->scale = scale;
    it->offset = offset;
    m_dirty = true;
}

void LivePlot::updateMinimumHeight() {
    // Inside a QScrollArea this makes many lanes scroll instead of squashing.
    setMinimumHeight(qMax(160, int(m_series.size()) * kMinLaneHeight + 18));
}

void LivePlot::addSample(int key, double t, double value) {
    auto it = m_series.find(key);
    if (it == m_series.end()) return;
    QVector<QPointF> &pts = it->pts;
    pts.append({t, value});
    // Trim in chunks so we don't shift the vector on every sample.
    if (pts.size() > 256 && pts.first().x() < t - kKeepSeconds - 30) {
        auto cut = std::lower_bound(pts.begin(), pts.end(), t - kKeepSeconds,
                                    [](const QPointF &a, double x) { return a.x() < x; });
        pts.erase(pts.begin(), cut);
    }
    m_latest = std::max(m_latest, t);
    if (!m_paused) m_dirty = true;
}

void LivePlot::clearData() {
    for (Series &s : m_series) s.pts.clear();
    m_latest = 0.0;
    m_dirty = true;
}

void LivePlot::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), QColor("#ffffff"));

    if (m_series.isEmpty()) {
        p.setPen(QColor("#8a8a8a"));
        p.drawText(rect(), Qt::AlignCenter, "Tick PIDs in the list to graph them");
        return;
    }

    if (!m_paused) m_frozenEnd = m_latest;
    const double tEnd = std::max(m_frozenEnd, m_window);

    const int axisH = 18;
    const double laneH = double(height() - axisH) / m_series.size();
    int i = 0;
    for (const Series &s : m_series) {
        paintLane(p, QRectF(0, i * laneH, width(), laneH), s, tEnd);
        ++i;
    }

    // Shared time axis (seconds relative to newest sample).
    const QRectF plotX(56, 0, width() - 56 - 12, height());
    p.setPen(QColor("#8a8a8a"));
    QFont f = font();
    f.setPointSizeF(8);
    p.setFont(f);
    const double step = m_window <= 10 ? 2 : m_window <= 30 ? 5 : m_window <= 60 ? 10 : 20;
    for (double rel = 0; rel <= m_window + 1e-9; rel += step) {
        const double x = plotX.right() - rel / m_window * plotX.width();
        p.drawText(QRectF(x - 30, height() - axisH, 60, axisH), Qt::AlignCenter,
                   rel == 0 ? QStringLiteral("now") : QString("-%1s").arg(rel));
    }
    if (m_paused) {
        p.setPen(QColor("#ca5010"));
        p.drawText(rect().adjusted(0, 4, -14, 0), Qt::AlignTop | Qt::AlignRight, "PAUSED");
    }
}

void LivePlot::paintLane(QPainter &p, const QRectF &lane, const Series &s, double tEnd) const {
    const QRectF r = lane.adjusted(56, 22, -12, -6);
    const double tStart = tEnd - m_window;

    // Visible slice.
    auto first = std::lower_bound(s.pts.begin(), s.pts.end(), tStart,
                                  [](const QPointF &a, double x) { return a.x() < x; });
    auto last = std::upper_bound(first, s.pts.end(), tEnd,
                                 [](double x, const QPointF &a) { return x < a.x(); });
    if (first != s.pts.begin()) --first;  // carry the line in from the left edge

    // Everything below is in display units (scale > 0 keeps the order).
    auto conv = [&s](double v) { return v * s.scale + s.offset; };
    double dataLo = INFINITY, dataHi = -INFINITY;
    for (auto it = first; it != last; ++it) {
        dataLo = std::min(dataLo, conv(it->y()));
        dataHi = std::max(dataHi, conv(it->y()));
    }
    const bool haveData = std::isfinite(dataLo);
    const bool haveRange = s.rangeMax > s.rangeMin;
    const double span = haveRange ? (s.rangeMax - s.rangeMin) * s.scale : 0.0;

    double lo, hi;
    if (haveRange && !m_autoFit) {
        // Scan-tool style: fixed typical range, stretched only if data leaves it.
        lo = conv(s.rangeMin);
        hi = conv(s.rangeMax);
        if (haveData) { lo = std::min(lo, dataLo); hi = std::max(hi, dataHi); }
    } else {
        lo = haveData ? dataLo : 0.0;
        hi = haveData ? dataHi : 1.0;
        // Never zoom in so far that sensor noise fills the lane: at least 10 % of
        // the signal's typical range, or ±1 unit / 5 % of the value when unknown.
        const double minSpan = haveRange ? span * 0.10
                                         : std::max(1.0, std::fabs((lo + hi) / 2) * 0.05);
        if (hi - lo < minSpan) {
            const double mid = (lo + hi) / 2;
            lo = mid - minSpan / 2;
            hi = mid + minSpan / 2;
        }
        const double pad = (hi - lo) * 0.08;
        lo -= pad;
        hi += pad;
    }

    // Frame + grid.
    p.setPen(QColor("#e5e5e5"));
    p.drawLine(QPointF(lane.left(), lane.bottom()), QPointF(lane.right(), lane.bottom()));
    p.setPen(QPen(QColor("#f0f0f0"), 1));
    for (int g = 0; g <= 4; ++g) {
        const double y = r.top() + r.height() * g / 4.0;
        p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
    }

    QFont f = font();
    f.setPointSizeF(8);
    p.setFont(f);
    p.setPen(QColor("#8a8a8a"));
    auto fmt = [](double v) { return std::fabs(v) >= 100 ? QString::number(v, 'f', 0)
                                                         : QString::number(v, 'f', 1); };
    for (int g = 0; g <= 4; g += 2) {  // top, middle, bottom labels
        const double v = hi - (hi - lo) * g / 4.0;
        const double y = r.top() + r.height() * g / 4.0;
        p.drawText(QRectF(0, y - 7, 52, 14), Qt::AlignRight | Qt::AlignVCenter, fmt(v));
    }

    // Title with current value.
    QFont tf = font();
    tf.setPointSizeF(9);
    tf.setBold(true);
    p.setFont(tf);
    p.setPen(s.color);
    QString title = s.name;
    if (!s.pts.isEmpty())
        title += QString("   %1 %2").arg(fmt(conv(s.pts.last().y())), s.unit);
    p.drawText(QRectF(r.left(), lane.top() + 2, r.width(), 18), Qt::AlignLeft | Qt::AlignVCenter, title);

    if (first == last) return;
    QPainterPath path;
    bool started = false;
    for (auto it = first; it != last; ++it) {
        const double x = r.left() + (it->x() - tStart) / m_window * r.width();
        const double y = r.bottom() - (conv(it->y()) - lo) / (hi - lo) * r.height();
        const QPointF pt(std::max(x, r.left()), y);
        if (!started) { path.moveTo(pt); started = true; }
        else path.lineTo(pt);
    }
    p.save();
    p.setClipRect(r.adjusted(-1, -2, 1, 2));
    p.setPen(QPen(s.color, 2.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);
    p.restore();
}
