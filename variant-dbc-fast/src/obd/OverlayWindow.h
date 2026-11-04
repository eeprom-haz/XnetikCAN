#pragma once
#include <QWidget>
#include <QHash>
#include <QVector>
#include <QPointF>
#include <QTimer>

class LiveDataDock;
class CanBridge;
class QLabel;
class QPushButton;
class QSlider;
class QScrollArea;
class OverlayView;

// Small always-on-top window that floats over PicoScope (or anything else) and shows up to
// 10 Live Data values the tech picked: big number, unit, a short trend, and how fresh it is.
// See-through fades only the background; numbers and trend lines stay solid. F8 freezes it
// from anywhere, so it can be paused together with the scope.
class OverlayWindow : public QWidget {
    Q_OBJECT
public:
    OverlayWindow(LiveDataDock *live, CanBridge *bridge);
    ~OverlayWindow() override;

    struct Item {
        QString id, name, unit;
        bool available = false;
        QVector<QPointF> history;   // (t, value), last kHistoryS seconds, in the units shown
        QString text;               // on/off or status text, else empty
        double lastT = -1;          // when the newest value arrived (-1 = none yet)
        double interval = 0;        // typical time between updates
    };
    static constexpr double kHistoryS = 15;

signals:
    void visibilityChanged(bool shown);

protected:
    bool nativeEvent(const QByteArray &type, void *message, qintptr *result) override;
    void showEvent(QShowEvent *e) override;
    void hideEvent(QHideEvent *e) override;
    void contextMenuEvent(QContextMenuEvent *e) override;
    void paintEvent(QPaintEvent *e) override;
    // No title bar (a native frame can't be see-through): drag anywhere empty, resize at the edges.
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    bool eventFilter(QObject *watched, QEvent *e) override;

private slots:
    void reload();                                   // overlay list, vehicle or units changed
    void onSample(const QString &id, double t, double value, const QString &text);
    void pickValues();
    void setFrozen(bool frozen);

private:
    void saveSettings();
    void fitHeight();
    Qt::Edges edgesAt(const QPoint &pos) const;
    bool startDragOrResize(const QPoint &pos);       // true: the window manager took over

    LiveDataDock *m_live;
    CanBridge *m_bridge;
    QList<Item> m_items;
    OverlayView *m_view;
    QScrollArea *m_scroll = nullptr;
    int m_fittedRows = -1;   // rows the height was last fitted to (resize only when that changes)
    QPushButton *m_freezeBtn, *m_compactBtn;
    QSlider *m_opacity;
    QLabel *m_hint;
    QTimer m_repaint;
    QTimer m_watchdog;   // no values for 2 s: make sure polling runs
    bool m_hotkey = false;
};
