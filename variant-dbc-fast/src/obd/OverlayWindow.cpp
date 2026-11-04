#include "OverlayWindow.h"
#include "LiveDataDock.h"
#include "../can/CanBridge.h"
#include <QApplication>
#include <QContextMenuEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSlider>
#include <QVBoxLayout>
#include <QWindow>
#include <cmath>
#include <functional>
#include <windows.h>

namespace {
constexpr int kHotkeyId = 0x5A11;
constexpr int kEdge = 6;   // px from the border where dragging resizes
QString fmt(double v) {
    return std::fabs(v) >= 100 ? QString::number(v, 'f', 0) : QString::number(v, 'f', std::fabs(v) >= 10 ? 1 : 2);
}
}

// Neon on dark: the overlay sits on PicoScope's black screen, so nothing dark is ever drawn.
namespace Neon {
const QColor cyan("#00F0FF"), green("#39FF14"), magenta("#FF2BD6"), yellow("#FFE600"), orange("#FF8C00"),
    blue("#4D9CFF"), red("#FF3B3B"), white("#FFFFFF"), soft("#D8F8FF");
const QColor trend[] = {green, magenta, yellow, orange, cyan, blue};
}

// All rows painted in one widget: name, big value, unit, freshness, and a trend line in its own
// neon colour. Only the background takes the see-through setting.
class OverlayView : public QWidget {
public:
    OverlayView(QList<OverlayWindow::Item> *items, std::function<double()> clock, std::function<int()> alpha)
        : m_items(items), m_clock(std::move(clock)), m_alpha(std::move(alpha)) {
        setAttribute(Qt::WA_TranslucentBackground);
        setMouseTracking(true);
    }
    void setCompact(bool c) { m_compact = c; updateGeometry(); update(); }
    void setFrozen(bool frozen) {
        m_frozenAt = frozen ? m_clock() : -1;
        if (frozen) m_frozenItems = *m_items;
        update();
    }
    int rowAt(int y) const { return y / rowHeight(); }
    int rowHeight() const { return m_compact ? 30 : 64; }
    QSize sizeHint() const override { return QSize(300, qMax(1, int(m_items->size())) * rowHeight()); }
    // Never squeezed: a smaller box scrolls instead of cutting rows off.
    QSize minimumSizeHint() const override { return QSize(180, qMax(1, int(m_items->size())) * rowHeight()); }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        m_halo = false;   // solid text only (the white edge looked blurry)
        const QList<OverlayWindow::Item> &items = m_frozenAt >= 0 ? m_frozenItems : *m_items;
        const double now = m_frozenAt >= 0 ? m_frozenAt : m_clock();
        if (items.isEmpty()) {
            p.setFont(QFont("Segoe UI", 9));
            text(p, rect().adjusted(10, 0, -10, 0), Qt::AlignCenter, "No values yet. Press “+ Add”,\nor right-click "
                 "a value in Live Data → Add to overlay.", Neon::soft);
            return;
        }
        const int rh = rowHeight();
        for (int i = 0; i < items.size(); ++i) {
            const OverlayWindow::Item &it = items[i];
            const QRect row(0, i * rh, width(), rh);
            const QColor lineColor = Neon::trend[i % 6];
            if (i + 1 < items.size()) {
                p.setPen(QColor(0, 240, 255, 70));
                p.drawLine(row.bottomLeft() + QPoint(6, 0), row.bottomRight() - QPoint(6, 0));
            }

            // Fresh = updated within ~3 typical intervals (at least 1.5 s). Older: orange "x s old";
            // nothing for over 2 s: red "no data".
            const double age = it.lastT < 0 ? 1e9 : now - it.lastT;
            const bool stale = age > qMax(1.5, it.interval * 3);

            // Name + number + unit on the top line, the trend across the whole width below.
            QFont nameFont("Segoe UI", m_compact ? 8 : 9, QFont::Bold);
            p.setFont(nameFont);
            const QRect nameRect = m_compact ? QRect(8, row.top(), width() / 2 - 8, rh)
                                             : QRect(8, row.top() + 2, width() / 2 - 8, 26);
            text(p, nameRect, Qt::AlignLeft | Qt::AlignVCenter,
                 p.fontMetrics().elidedText(it.name, Qt::ElideRight, nameRect.width()), Neon::cyan);

            QString value, note;
            QColor noteColor = Neon::orange;
            // No number yet: leave the space empty.
            if (!it.available) note = "not on this vehicle", noteColor = Neon::red;
            else if (it.lastT < 0) note = "waiting";
            else {
                value = it.text.isEmpty() ? fmt(it.history.isEmpty() ? 0 : it.history.last().y()) : it.text;
                if (age > 2.0 && m_frozenAt < 0) note = QString("no data %1 s").arg(age < 60 ? age : 60, 0, 'f', 0), noteColor = Neon::red;
                else if (stale) note = QString("%1 s old").arg(age, 0, 'f', 1);
            }
            QFont big("Segoe UI", m_compact ? 12 : 17, QFont::Bold);
            p.setFont(big);
            const QRect valueRect = m_compact ? QRect(width() / 2, row.top(), width() / 2 - 60, rh)
                                              : QRect(width() / 2, row.top() + 2, width() / 2 - 58, 26);
            text(p, valueRect, Qt::AlignRight | Qt::AlignVCenter, value, Neon::white);
            p.setFont(QFont("Segoe UI", 8, QFont::DemiBold));
            const QRect unitR = m_compact ? QRect(width() - 56, row.top(), 50, rh / 2 + 4)
                                          : QRect(width() - 54, row.top() + 2, 48, 26);
            if (it.text.isEmpty() && !value.isEmpty()) text(p, unitR, Qt::AlignLeft | Qt::AlignVCenter, it.unit, Neon::soft);
            // Alive: a dot that flashes on every new reading.
            if (!m_compact && it.available && it.lastT >= 0) {
                if (age < 0.15 && m_frozenAt < 0) {
                    p.setPen(Qt::NoPen);
                    p.setBrush(Neon::green);
                    p.drawEllipse(QPointF(width() - 8, row.top() + 9), 3.5, 3.5);
                }
            }
            if (!note.isEmpty()) {
                p.setFont(QFont("Segoe UI", 8, QFont::DemiBold));
                const QRect noteR = m_compact ? QRect(width() - 56, row.top() + rh / 2 - 2, 52, rh / 2)
                                              : QRect(width() - 160, row.bottom() - 15, 152, 14);
                text(p, noteR, Qt::AlignRight | Qt::AlignVCenter, note, noteColor);
            }
            // Trend over the whole row width (labels to its right), 15 s wide, its own neon colour.
            if (!m_compact) drawTrend(p, QRect(8, row.top() + 32, width() - 16 - 40, rh - 40), it, now, lineColor);
        }
    }

private:
    // Text in a solid colour; over a faint background with a thin white edge.
    void text(QPainter &p, const QRect &r, int align, const QString &s, const QColor &c) {
        if (!m_halo || s.contains('\n')) {
            p.setPen(c);
            p.drawText(r, align, s);
            return;
        }
        const QFontMetrics fm(p.font());
        const int w = fm.horizontalAdvance(s);
        const int x = (align & Qt::AlignRight) ? r.right() - w : (align & Qt::AlignHCenter) ? r.center().x() - w / 2 : r.left();
        const int y = r.top() + (r.height() + fm.ascent() - fm.descent()) / 2;
        QPainterPath path;
        path.addText(x, y, p.font(), s);
        p.strokePath(path, QPen(QColor(255, 255, 255, 230), 3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.fillPath(path, c);
    }

    void drawTrend(QPainter &p, const QRect &r, const OverlayWindow::Item &it, double now, const QColor &color) {
        if (r.width() < 30 || it.history.size() < 2 || !it.text.isEmpty()) return;
        // Time marks every 5 s slide left with the line, so the overlay visibly moves even when
        // the value is steady (engine off). They stop when frozen.
        p.setPen(QPen(QColor(0, 240, 255, 55), 1));
        const double t0m = now - OverlayWindow::kHistoryS;
        for (double t = std::ceil(t0m / 5.0) * 5.0; t <= now; t += 5.0) {
            const double x = r.left() + (t - t0m) / OverlayWindow::kHistoryS * r.width();
            p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
        }
        double lo = 1e300, hi = -1e300;
        for (const QPointF &pt : it.history) { lo = qMin(lo, pt.y()); hi = qMax(hi, pt.y()); }
        const bool flat = hi - lo < 1e-9;
        if (flat) { hi += 0.5; lo -= 0.5; }
        const double t0 = now - OverlayWindow::kHistoryS;
        QPainterPath path;
        bool first = true;
        for (const QPointF &pt : it.history) {
            const QPointF xy(r.left() + (pt.x() - t0) / OverlayWindow::kHistoryS * r.width(),
                             r.bottom() - (pt.y() - lo) / (hi - lo) * r.height());
            if (first) path.moveTo(xy); else path.lineTo(xy);
            first = false;
        }
        p.strokePath(path, QPen(color, 2.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        if (flat) return;   // a steady value has no scale worth showing
        p.setFont(QFont("Segoe UI", 7));
        text(p, QRect(r.right() + 4, r.top() - 3, 38, 10), Qt::AlignLeft, fmt(hi), Neon::soft);
        text(p, QRect(r.right() + 4, r.bottom() - 8, 38, 10), Qt::AlignLeft, fmt(lo), Neon::soft);
    }

    QList<OverlayWindow::Item> *m_items;
    QList<OverlayWindow::Item> m_frozenItems;
    std::function<double()> m_clock;
    std::function<int()> m_alpha;
    bool m_compact = false;
    bool m_halo = false;
    double m_frozenAt = -1;
};

OverlayWindow::OverlayWindow(LiveDataDock *live, CanBridge *bridge)
    : QWidget(nullptr, Qt::Window | Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint)
    , m_live(live), m_bridge(bridge) {
    setWindowTitle("XnetikCan overlay");
    setAttribute(Qt::WA_QuitOnClose, false);
    // A see-through background with solid content: the window paints its own background.
    setAttribute(Qt::WA_TranslucentBackground);
    setMouseTracking(true);
    setMinimumSize(230, 110);
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(kEdge, kEdge, kEdge, kEdge);
    lay->setSpacing(4);

    auto *bar = new QHBoxLayout;
    // Small symbol buttons, no words (the tooltip says what each does).
    auto *add = new QPushButton("+");
    add->setToolTip("Add / remove values (up to 10)");
    m_freezeBtn = new QPushButton("‖");
    m_freezeBtn->setCheckable(true);
    m_freezeBtn->setToolTip("Freeze — F8 works even while PicoScope is the active window");
    m_compactBtn = new QPushButton("–");
    m_compactBtn->setCheckable(true);
    m_compactBtn->setToolTip("Compact: numbers only, no trend lines");
    auto *close = new QPushButton("×");
    close->setToolTip("Close the overlay");
    for (QPushButton *b : {add, m_freezeBtn, m_compactBtn, close}) {
        b->setFocusPolicy(Qt::NoFocus);
        b->setFixedSize(22, 20);
    }
    // Dark buttons with neon symbols: the overlay sits on PicoScope's black screen.
    // Bare symbols only: no box, no border, no background. Hover = brighter; on (frozen /
    // compact) = neon green.
    setStyleSheet("QPushButton { background:transparent; color:#00F0FF; border:none; padding:0px;"
                  " font-size:15px; font-weight:700; }"
                  "QPushButton:hover { color:#FFFFFF; }"
                  "QPushButton:checked { color:#39FF14; }"
                  "QScrollArea, QScrollArea > QWidget > QWidget { background:transparent; border:none; }"
                  "QScrollBar:vertical { background:transparent; width:6px; margin:0; }"
                  "QScrollBar::handle:vertical { background:rgba(0,240,255,140); border-radius:3px; min-height:20px; }"
                  "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height:0; }"
                  "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background:transparent; }");
    m_opacity = new QSlider(Qt::Horizontal);
    m_opacity->setRange(15, 100);
    m_opacity->setFixedWidth(64);
    m_opacity->setFocusPolicy(Qt::NoFocus);
    m_opacity->setToolTip("Background darkness: left = see-through (the scope shows through), right = solid black.\n"
                          "The border fades with it. Numbers and trend lines always stay bright.");
    bar->addWidget(add);
    bar->addWidget(m_freezeBtn);
    bar->addWidget(m_compactBtn);
    bar->addStretch();
    bar->addWidget(m_opacity);
    bar->addWidget(close);
    lay->addLayout(bar);

    m_view = new OverlayView(&m_items, [this] { return m_live->now(); },
                             [this] { return m_opacity->value() * 255 / 100; });
    m_view->installEventFilter(this);
    // Rows scroll (wheel or the thin bar) when the box is made smaller than the list.
    m_scroll = new QScrollArea;
    m_scroll->setWidget(m_view);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->viewport()->setAutoFillBackground(false);
    m_scroll->setAttribute(Qt::WA_TranslucentBackground);
    m_scroll->viewport()->setAttribute(Qt::WA_TranslucentBackground);
    lay->addWidget(m_scroll, 1);
    m_hint = new QLabel;   // kept for the code below; no text line at the bottom any more
    m_hint->hide();

    QSettings s;
    m_opacity->setValue(s.value("overlay/darkBackground", 70).toInt());
    m_compactBtn->setChecked(s.value("overlay/compact", false).toBool());
    m_view->setCompact(m_compactBtn->isChecked());
    if (restoreGeometry(s.value("overlay/geometry").toByteArray()))
        m_fittedRows = int(m_live->overlayIds().size());   // keep the size you left it at
    else
        resize(330, 420);

    connect(add, &QPushButton::clicked, this, &OverlayWindow::pickValues);
    connect(close, &QPushButton::clicked, this, &QWidget::hide);
    connect(m_freezeBtn, &QPushButton::toggled, this, &OverlayWindow::setFrozen);
    connect(m_compactBtn, &QPushButton::toggled, this, [this](bool on) {
        m_view->setCompact(on);
        fitHeight();
        saveSettings();
    });
    connect(m_opacity, &QSlider::valueChanged, this, [this] { update(); saveSettings(); });
    connect(m_live, &LiveDataDock::overlayChanged, this, &OverlayWindow::reload);
    connect(m_live, &LiveDataDock::overlaySample, this, &OverlayWindow::onSample);
    connect(m_bridge, &CanBridge::stateChanged, this, &OverlayWindow::reload);
    // Ages and trends move with time even when nothing new arrives.
    connect(&m_repaint, &QTimer::timeout, m_view, QOverload<>::of(&QWidget::update));
    m_repaint.start(100);
    connect(m_live, &LiveDataDock::scopeModeChanged, this, &OverlayWindow::reload);
    // Nothing new for over 2 s (and not frozen): make sure polling runs.
    connect(&m_watchdog, &QTimer::timeout, this, [this] {
        if (!isVisible() || m_freezeBtn->isChecked() || !m_bridge->isConnected()) return;
        const double now = m_live->now();
        for (const Item &it : std::as_const(m_items))
            if (it.available && (it.lastT < 0 || now - it.lastT > 2.0)) {
                m_live->ensurePolling();
                break;
            }
    });
    m_watchdog.start(2000);
    reload();
}

OverlayWindow::~OverlayWindow() {
    if (isVisible()) saveSettings();
    if (m_hotkey) UnregisterHotKey(reinterpret_cast<HWND>(winId()), kHotkeyId);
}

void OverlayWindow::saveSettings() {
    QSettings s;
    s.setValue("overlay/geometry", saveGeometry());
    s.setValue("overlay/darkBackground", m_opacity->value());
    s.setValue("overlay/compact", m_compactBtn->isChecked());
}

void OverlayWindow::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    const int alpha = m_opacity->value() * 255 / 100;
    p.setBrush(QColor(8, 10, 14, alpha));
    // The border fades with the background (never quite gone, so the window can be grabbed);
    // frozen = bright, thick cyan.
    const bool frozen = m_freezeBtn->isChecked();
    QColor edge = !m_bridge->isConnected() ? QColor("#FF8C00") : m_live->scopeMode() ? QColor("#39FF14")
                                                                                       : QColor("#00F0FF");
    if (!frozen) edge.setAlpha(qMax(40, alpha));
    p.setPen(QPen(edge, frozen ? 3 : 1));
    p.drawRoundedRect(frozen ? r.adjusted(1, 1, -1, -1) : r, 5, 5);
}

Qt::Edges OverlayWindow::edgesAt(const QPoint &pos) const {
    Qt::Edges e;
    if (pos.x() < kEdge) e |= Qt::LeftEdge;
    if (pos.x() >= width() - kEdge) e |= Qt::RightEdge;
    if (pos.y() < kEdge) e |= Qt::TopEdge;
    if (pos.y() >= height() - kEdge) e |= Qt::BottomEdge;
    return e;
}

bool OverlayWindow::startDragOrResize(const QPoint &pos) {
    QWindow *w = windowHandle();
    if (!w) return false;
    const Qt::Edges e = edgesAt(pos);
    return e ? w->startSystemResize(e) : w->startSystemMove();
}

void OverlayWindow::mousePressEvent(QMouseEvent *e) {
    if (e->button() == Qt::LeftButton && startDragOrResize(e->position().toPoint())) return;
    QWidget::mousePressEvent(e);
}

void OverlayWindow::mouseMoveEvent(QMouseEvent *e) {
    const Qt::Edges edges = edgesAt(e->position().toPoint());
    if ((edges & Qt::LeftEdge && edges & Qt::TopEdge) || (edges & Qt::RightEdge && edges & Qt::BottomEdge))
        setCursor(Qt::SizeFDiagCursor);
    else if ((edges & Qt::RightEdge && edges & Qt::TopEdge) || (edges & Qt::LeftEdge && edges & Qt::BottomEdge))
        setCursor(Qt::SizeBDiagCursor);
    else if (edges & (Qt::LeftEdge | Qt::RightEdge)) setCursor(Qt::SizeHorCursor);
    else if (edges & (Qt::TopEdge | Qt::BottomEdge)) setCursor(Qt::SizeVerCursor);
    else unsetCursor();
    QWidget::mouseMoveEvent(e);
}

bool OverlayWindow::eventFilter(QObject *watched, QEvent *e) {
    // The value rows are a handle too: drag them to move the window.
    if (watched == m_view && e->type() == QEvent::MouseButtonPress) {
        auto *me = static_cast<QMouseEvent *>(e);
        if (me->button() == Qt::LeftButton && startDragOrResize(m_view->mapTo(this, me->position().toPoint())))
            return true;
    }
    return QWidget::eventFilter(watched, e);
}

void OverlayWindow::showEvent(QShowEvent *e) {
    QWidget::showEvent(e);
    // F8 freezes the overlay from anywhere (e.g. while PicoScope has the keyboard).
    if (!m_hotkey) m_hotkey = RegisterHotKey(reinterpret_cast<HWND>(winId()), kHotkeyId, MOD_NOREPEAT, VK_F8);
    setFrozen(m_freezeBtn->isChecked());
    if (!m_hotkey) m_freezeBtn->setToolTip("F8 is taken by another program; use this button instead");
    m_live->setOverlayShown(true);
    m_live->ensurePolling();
    reload();
    emit visibilityChanged(true);
}

void OverlayWindow::hideEvent(QHideEvent *e) {
    saveSettings();
    m_live->setOverlayShown(false);
    if (m_hotkey) {
        UnregisterHotKey(reinterpret_cast<HWND>(winId()), kHotkeyId);
        m_hotkey = false;
    }
    QWidget::hideEvent(e);
    emit visibilityChanged(false);
}

bool OverlayWindow::nativeEvent(const QByteArray &type, void *message, qintptr *result) {
    auto *msg = static_cast<MSG *>(message);
    if (type == "windows_generic_MSG" && msg->message == WM_HOTKEY && msg->wParam == kHotkeyId) {
        m_freezeBtn->toggle();
        *result = 0;
        return true;
    }
    return QWidget::nativeEvent(type, message, result);
}

void OverlayWindow::setFrozen(bool frozen) {
    m_view->setFrozen(frozen);
    m_freezeBtn->setToolTip(frozen ? "Frozen — press again or F8 to go on" : "Freeze — F8 works even while PicoScope is active");
    update();   // blue edge while frozen
}

void OverlayWindow::reload() {
    QHash<QString, Item> old;
    for (const Item &it : std::as_const(m_items)) old.insert(it.id, it);
    m_items.clear();
    for (const QString &id : m_live->overlayIds()) {
        Item it = old.value(id);
        const LiveDataDock::ValueInfo info = m_live->valueInfo(id);
        it.id = id;
        if (!info.name.isEmpty()) {
            if (it.unit != info.unit) it.history.clear();   // units switched: the old trend is in the old unit
            it.name = info.name;
            it.unit = info.unit;
        }
        it.available = info.available;
        m_items << it;
    }
    // No text line at the bottom: the window edge says it — orange = not connected,
    // green = scope mode (only these values are read), cyan = normal.
    update();
    m_view->updateGeometry();
    m_view->update();
    // Fit the height only when values are added or removed, so a box you made smaller stays small.
    if (m_items.size() != m_fittedRows) {
        m_fittedRows = int(m_items.size());
        fitHeight();
    }
    if (isVisible()) m_live->ensurePolling();
}

void OverlayWindow::fitHeight() {
    // Exactly as tall as its rows: no empty block under the values.
    const QMargins m = layout()->contentsMargins();
    int h = m.top() + m.bottom() + m_view->sizeHint().height() + layout()->spacing();
    h += layout()->itemAt(0)->sizeHint().height();                       // the button row
    if (!m_hint->isHidden()) h += layout()->spacing() + m_hint->sizeHint().height();
    resize(width(), qMax(minimumHeight(), h));
}

void OverlayWindow::onSample(const QString &id, double t, double value, const QString &text) {
    for (Item &it : m_items) {
        if (it.id != id) continue;
        if (it.lastT >= 0) {
            const double dt = t - it.lastT;
            it.interval = it.interval <= 0 ? dt : it.interval * 0.8 + dt * 0.2;
        }
        it.lastT = t;
        it.text = text;
        it.history << QPointF(t, value);
        while (!it.history.isEmpty() && it.history.first().x() < t - kHistoryS) it.history.removeFirst();
    }
}

void OverlayWindow::pickValues() {
    QDialog dlg(this);
    dlg.setWindowTitle("Values on the overlay");
    dlg.resize(460, 560);
    auto *lay = new QVBoxLayout(&dlg);
    auto *search = new QLineEdit;
    search->setPlaceholderText("Search (e.g. rpm, cam, trim, oil)…");
    search->setClearButtonEnabled(true);
    auto *count = new QLabel;
    auto *list = new QListWidget;
    lay->addWidget(search);
    lay->addWidget(list, 1);
    lay->addWidget(count);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    lay->addWidget(buttons);

    QStringList chosen = m_live->overlayIds();
    const QList<LiveDataDock::ValueInfo> values = m_live->availableValues();
    for (const auto &v : values) {
        auto *item = new QListWidgetItem(QString("%1%2   —   %3").arg(v.name, v.unit.isEmpty() ? QString() : " (" + v.unit + ")", v.where));
        item->setData(Qt::UserRole, v.id);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(chosen.contains(v.id) ? Qt::Checked : Qt::Unchecked);
        list->addItem(item);
    }
    auto updateCount = [&] {
        count->setText(QString("%1 of %2 picked").arg(chosen.size()).arg(LiveDataDock::kMaxOverlay));
    };
    updateCount();
    QObject::connect(search, &QLineEdit::textChanged, &dlg, [&](const QString &t) {
        for (int i = 0; i < list->count(); ++i)
            list->item(i)->setHidden(!t.isEmpty() && !list->item(i)->text().contains(t, Qt::CaseInsensitive));
    });
    QObject::connect(list, &QListWidget::itemChanged, &dlg, [&](QListWidgetItem *item) {
        const QString id = item->data(Qt::UserRole).toString();
        if (item->checkState() == Qt::Checked) {
            if (chosen.contains(id)) return;
            if (chosen.size() >= LiveDataDock::kMaxOverlay) {
                const QSignalBlocker block(list);
                item->setCheckState(Qt::Unchecked);
                count->setText(QString("Already %1 picked — remove one first").arg(LiveDataDock::kMaxOverlay));
                return;
            }
            chosen << id;
        } else {
            chosen.removeAll(id);
        }
        updateCount();
    });
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (values.isEmpty())
        count->setText("Nothing to pick yet — connect and let Auto ID check the vehicle, or choose the vehicle.");
    if (dlg.exec() == QDialog::Accepted) m_live->setOverlayIds(chosen);
}

void OverlayWindow::contextMenuEvent(QContextMenuEvent *e) {
    const QPoint inView = m_view->mapFrom(this, e->pos());
    const int row = m_view->rect().contains(inView) ? m_view->rowAt(inView.y()) : -1;
    QMenu menu(this);
    QStringList ids = m_live->overlayIds();
    if (row >= 0 && row < ids.size()) {
        const QString id = ids[row];
        menu.addAction("Remove", this, [this, ids, id]() mutable {
            ids.removeAll(id);
            m_live->setOverlayIds(ids);
        });
        menu.addAction("Move up", this, [this, ids, row]() mutable {
            ids.swapItemsAt(row, row - 1);
            m_live->setOverlayIds(ids);
        })->setEnabled(row > 0);
        menu.addAction("Move down", this, [this, ids, row]() mutable {
            ids.swapItemsAt(row, row + 1);
            m_live->setOverlayIds(ids);
        })->setEnabled(row + 1 < ids.size());
        menu.addSeparator();
    }
    menu.addAction("Add / remove values…", this, &OverlayWindow::pickValues);
    menu.addAction("Close overlay", this, &QWidget::hide);
    menu.exec(e->globalPos());
}
