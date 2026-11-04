#include "MainWindow.h"
#include "ConnectDialog.h"
#include "obd/VehicleId.h"
#include "obd/IdentifyDialog.h"
#include "obd/OverlayWindow.h"
#include "obd/Units.h"
#include "can/GsUsbDevice.h"
#include "dbc/DbcFile.h"
#include <QActionGroup>
#include <QComboBox>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QStandardPaths>
#include <QDir>
#include <QRegularExpression>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QCursor>
#include <QApplication>
#include <QMenuBar>
#include <QToolBar>
#include <QToolButton>
#include <QDockWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QPushButton>
#include <QHeaderView>
#include <QFileDialog>
#include <QInputDialog>
#include <QMessageBox>
#include <QSettings>
#include <QCloseEvent>
#include <QTextStream>
#include <QStatusBar>
#include <QMenu>
#include <QClipboard>
#include <QGuiApplication>

static QString formatPayload(const QByteArray &data) {
    return QString::fromLatin1(data.toHex(' ').toUpper());
}

// Column layouts for the two frame views.
// "Source" (J = Scan/J2534, G = Snort/C31G) is the first data column so both tools share one table.
namespace LogCol { enum { Time, Source, Message, Id, Len, Data, Decoded, Count }; }
namespace MsgCol { enum { Source, Message, Id, Len, Data, Decoded, Seen, Every, Count }; }

// Diagnostic (scan tool <-> module) traffic on 11-bit OBD/UDS IDs.
static constexpr quint32 kDiagFrom = 0x7DF;
static constexpr quint32 kDiagTo = 0x7EF;

// ── UDS / ISO-TP plain-language decode for the bus view ──────────────────────────
// Turns a diagnostic frame like "03 22 03 07" into "Read DID 0307", so you can read what a
// scan tool sends (session, read, IO control, routine, tester present) without decoding hex.
namespace {
bool isDiagId(quint32 id, bool ext) {
    if (!ext) return id == 0x7DF || (id >= 0x7E0 && id <= 0x7EF);
    return ((id >> 16) & 0xFF) == 0xDA;   // 29-bit 18DAxxxx physical addressing
}
QString nrcName(quint8 n) {
    switch (n) {
    case 0x11: return "service not supported";
    case 0x12: return "sub-function not supported";
    case 0x13: return "wrong length";
    case 0x22: return "conditions not correct";
    case 0x31: return "request out of range";
    case 0x33: return "security denied";
    case 0x35: return "invalid key";
    case 0x78: return "pending";
    case 0x7E: return "not supported in this session";
    case 0x7F: return "not supported in this session";
    default:   return "NRC " + QString::number(n, 16).toUpper();
    }
}
QString hexRest(const QByteArray &d, int from) {
    QString h;
    for (int i = from; i < d.size(); ++i) h += QString(" %1").arg(quint8(d[i]), 2, 16, QChar('0')).toUpper();
    return h.trimmed();
}
QString didOf(const QByteArray &d) {
    return d.size() >= 3 ? QString("%1%2").arg(quint8(d[1]), 2, 16, QChar('0'))
                               .arg(quint8(d[2]), 2, 16, QChar('0')).toUpper() : QString();
}
// d starts at the UDS service byte.
QString udsService(const QByteArray &d) {
    if (d.isEmpty()) return {};
    const quint8 s = quint8(d[0]);
    switch (s) {
    case 0x10: return "Session control " + hexRest(d, 1);
    case 0x50: return "Session started";
    case 0x11: return "ECU reset " + hexRest(d, 1);
    case 0x14: return "Clear DTCs";
    case 0x19: return "Read DTCs";
    case 0x22: return "Read DID " + didOf(d);
    case 0x62: return "DID " + didOf(d) + " = " + hexRest(d, 3);
    case 0x2E: return "Write DID " + didOf(d) + " = " + hexRest(d, 3);
    case 0x6E: return "Wrote DID " + didOf(d);
    case 0x27: return "Security access " + hexRest(d, 1);
    case 0x67: return "Security access reply";
    case 0x2F: return "IO control DID " + didOf(d) + "  " + hexRest(d, 3);   // actuator take-over
    case 0x6F: return "IO control OK DID " + didOf(d);
    case 0x31: return QString("Routine %1 %2")
                   .arg(d.size() > 1 && quint8(d[1]) == 1 ? "START" : d.size() > 1 && quint8(d[1]) == 2 ? "STOP" : "result",
                        d.size() >= 4 ? QString("%1%2").arg(quint8(d[2]), 2, 16, QChar('0'))
                                            .arg(quint8(d[3]), 2, 16, QChar('0')).toUpper() : QString());
    case 0x71: return "Routine done " + hexRest(d, 2);
    case 0x3E: return "Tester present";
    case 0x7E: return "Tester present OK";
    case 0x7F: return d.size() >= 3 ? "REFUSED service " + QString::number(quint8(d[1]), 16).toUpper()
                                          + " — " + nrcName(quint8(d[2]))
                                    : "REFUSED";
    default:   return "service " + QString::number(s, 16).rightJustified(2, '0').toUpper() + " " + hexRest(d, 1);
    }
}
QString udsDescribe(quint32 id, bool ext, const QByteArray &p) {
    if (!isDiagId(id, ext) || p.isEmpty()) return {};
    const quint8 pci = quint8(p[0]);
    switch (pci >> 4) {
    case 0x0: return udsService(p.mid(1, pci & 0x0F));                       // single frame
    case 0x1: return "multi-frame, " + QString::number(((pci & 0x0F) << 8) | (p.size() > 1 ? quint8(p[1]) : 0))
                         + " bytes: " + udsService(p.mid(2));                 // first frame
    case 0x2: return "…continued";                                           // consecutive frame
    case 0x3: return "flow control";
    default:  return {};
    }
}
}  // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_replay(new ReplayEngine(&m_bridge, this))
{
    setWindowTitle("XnetikCan");
    setMinimumSize(1000, 650);

    m_dbc.loadDefaults();
    m_enhanced.load();
    m_oe.scan();

    setupMenus();
    setupToolbar();
    setupCentralView();
    setupFilterDock();
    setupDiagnoseDock();
    setupLiveDataDock();
    setupStatusBar();
    setupVehicleBar();
    setupActuatorDock();

    // Every panel can be closed, so every panel needs a way back.
    m_viewMenu->addAction(m_filterDock->toggleViewAction());
    m_viewMenu->addAction(m_diagnosePanel->toggleViewAction());
    m_viewMenu->addAction(m_liveDataDock->toggleViewAction());
    m_viewMenu->addAction("Codes && Readiness…", this, [this] { m_codesBtn->click(); });
    m_viewMenu->addSeparator();
    m_viewMenu->addAction(m_diagViewAct);
    auto *fullScreen = m_viewMenu->addAction("Full screen");
    fullScreen->setCheckable(true);
    fullScreen->setShortcut(QKeySequence(Qt::Key_F11));
    connect(fullScreen, &QAction::toggled, this, [this](bool on) {
        if (on) showFullScreen();
        else showMaximized();
    });
    m_viewMenu->addAction(m_overlayAct);
    // Units: everything is read metric; this only changes what is shown (hover shows the reading as read).
    auto *unitsMenu = m_viewMenu->addMenu("Units");
    auto *unitGroup = new QActionGroup(this);
    auto *imperial = unitsMenu->addAction("Imperial (°F, psi, inHg, mph)");
    auto *metric = unitsMenu->addAction("Metric (°C, kPa, km/h)");
    for (QAction *a : {imperial, metric}) {
        a->setCheckable(true);
        unitGroup->addAction(a);
    }
    (Units::imperial() ? imperial : metric)->setChecked(true);
    connect(imperial, &QAction::toggled, this, [](bool on) { Units::setImperial(on); });
    m_defaultState = saveState();  // captured before loadSettings() applies the user's layout
    m_diagnosePanel->setContextProvider([this] { return aiContext(); });

    connect(&m_bridge, &CanBridge::frameReceived, this, &MainWindow::onFrameReceived);
    connect(&m_bridge, &CanBridge::stateChanged, this, &MainWindow::onBusStateChanged);
    connect(&m_bridge, &CanBridge::busError, this, &MainWindow::onBusError);
    connect(m_replay, &ReplayEngine::finished, this, &MainWindow::onReplayFinished);
    connect(&m_dbc, &DbcLibrary::changed, this, &MainWindow::refilterTable);

    // Flush queued frames to UI every 50 ms to prevent lag at high frame rates.
    connect(&m_flushTimer, &QTimer::timeout, this, &MainWindow::flushPendingFrames);
    m_flushTimer.start(50);
    // Repeating "send this frame" out Snort (Active mode).
    connect(&m_repeatSend, &QTimer::timeout, this, [this] {
        if (m_snort && m_snort->canSend()) m_snort->writeFrame(m_repeatFrame);
    });
    // Tester-present (3E 80, functional 7DF) keeps the diagnostic session alive while a test runs.
    connect(&m_testerTimer, &QTimer::timeout, this, [this] {
        if (m_snort && m_snort->canSend()) m_snort->writeFrame(QCanBusFrame(0x7DF, isoTpSingle(QByteArray::fromHex("3E80"))));
    });

    loadSettings();
    filterChanged();
    updateStatusBar();
}

MainWindow::~MainWindow() {
    delete m_overlay;   // a separate top-level window
    if (m_snort) m_snort->close();
}

void MainWindow::changeEvent(QEvent *e) {
    if (e->type() == QEvent::WindowStateChange && m_liveDataDock)
        m_liveDataDock->setMainMinimized(isMinimized());
    QMainWindow::changeEvent(e);
}

void MainWindow::setupMenus() {
    auto *fileMenu = menuBar()->addMenu("&File");

    fileMenu->addAction("Save Log…", this, [this]() {
        QString path = QFileDialog::getSaveFileName(
            this, "Save Log", {}, "XnetikCan Log (*.xncap);;Text (*.txt)");
        if (path.isEmpty()) return;
        QFile f(path);
        if (!f.open(QFile::WriteOnly | QFile::Text)) {
            QMessageBox::critical(this, "Error", f.errorString());
            return;
        }
        QTextStream out(&f);
        for (const QCanBusFrame &frame : m_bridge.buffer()->all()) {
            double ts = frame.timeStamp().seconds() + frame.timeStamp().microSeconds() / 1e6;
            out << QString("%1 0x%2 [%3] %4\n")
                .arg(ts, 10, 'f', 6)
                .arg(frame.frameId(), 4, 16, QChar('0'))
                .arg(frame.payload().size())
                .arg(formatPayload(frame.payload()));
        }
    });

    fileMenu->addAction("Save capture as CSV…", this, &MainWindow::saveCaptureCsv);
    fileMenu->addAction("Open capture (CSV)…", this, &MainWindow::openCaptureCsv);

    fileMenu->addAction("Load Vehicle Definition (DBC)…", this, [this]() {
        const QString path = QFileDialog::getOpenFileName(this, "Load vehicle definition", {},
                                                          "DBC files (*.dbc)");
        if (path.isEmpty()) return;
        QString err;
        if (!m_dbc.addFile(path, &err)) {
            QMessageBox::critical(this, "Load DBC", err);
            return;
        }
        QSettings s;
        QStringList files = s.value("dbc/userFiles").toStringList();
        if (!files.contains(path)) files << path;
        s.setValue("dbc/userFiles", files);
        statusBar()->showMessage("Loaded " + QFileInfo(path).fileName(), 5000);
    });

    fileMenu->addSeparator();
    fileMenu->addAction("E&xit", qApp, &QApplication::quit);

    m_viewMenu = menuBar()->addMenu("&View");
    m_viewMenu->addAction("Reset Layout", this, [this]() {
        m_diagViewAct->setChecked(false);
        for (QDockWidget *d : {static_cast<QDockWidget *>(m_filterDock), static_cast<QDockWidget *>(m_diagnosePanel),
                               static_cast<QDockWidget *>(m_liveDataDock)}) {
            d->setFloating(false);
            d->show();
        }
        restoreState(m_defaultState);
    });
    m_viewMenu->addSeparator();
}

void MainWindow::setupToolbar() {
    auto *tb = addToolBar("Main");
    tb->setObjectName("MainToolbar");
    tb->setMovable(false);

    // Connect gets a styled QPushButton widget so it stays visible and prominent.
    auto *connectBtn = new QPushButton("⚡  Connect");
    connectBtn->setObjectName("ConnectBtn");
    connectBtn->setFixedHeight(28);
    connectBtn->setCursor(Qt::PointingHandCursor);
    tb->addWidget(connectBtn);

    m_connectAct = new QAction("Connect", this);
    connect(connectBtn, &QPushButton::clicked, this, &MainWindow::onConnect);

    m_disconnectAct = tb->addAction("Disconnect");
    tb->addSeparator();
    // Snort: the SH-C31G raw-CAN sniffer. Independent of the J2534 "Connect" path — it never
    // runs Auto-ID; its frames stream straight into the hex table tagged "G".
    m_snortAct = tb->addAction("Snort");
    m_snortAct->setCheckable(true);
    m_snortAct->setToolTip("Sniff raw CAN with the SH-C31G (silent listen by default). "
                           "Frames appear in the table tagged G. Runs alongside J2534.");
    connect(m_snortAct, &QAction::triggered, this, &MainWindow::onSnort);
    tb->addSeparator();
    m_startCaptureAct = tb->addAction("▶  Capture");
    m_stopCaptureAct = tb->addAction("■  Stop");
    tb->addAction("Clear", this, &MainWindow::onClearCapture);
    tb->addSeparator();
    m_replayAct = tb->addAction("↺  Replay");
    tb->addSeparator();
    m_diagViewAct = tb->addAction("📈  Diagnostics view");
    m_diagViewAct->setCheckable(true);
    m_diagViewAct->setShortcut(QKeySequence("Ctrl+D"));
    m_diagViewAct->setToolTip("Live Data and graphs fill the window (hides the message table, filter and "
                              "Claude panel). Click again to go back.  Ctrl+D");
    connect(m_diagViewAct, &QAction::toggled, this, &MainWindow::setDiagnosticsView);
    m_overlayAct = tb->addAction("Overlay");
    m_overlayAct->setCheckable(true);
    m_overlayAct->setToolTip("Small always-on-top window with up to 10 values you pick — put it over PicoScope.\n"
                             "F8 freezes it (works while PicoScope is the active window).");
    connect(m_overlayAct, &QAction::toggled, this, [this](bool on) {
        if (!m_overlay) {
            if (!on) return;
            m_overlay = new OverlayWindow(m_liveDataDock, &m_bridge);
            connect(m_overlay, &OverlayWindow::visibilityChanged, m_overlayAct, &QAction::setChecked);
        }
        m_overlay->setVisible(on);
        if (on) m_overlay->raise();
    });

    auto *spacer = new QWidget;
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    tb->addWidget(spacer);

    // Battery voltage at the OBD connector (pin 16), read by the adapter about once a second.
    m_batteryLabel = new QLabel;
    m_batteryLabel->setToolTip("Battery voltage at the OBD connector (pin 16), measured by the adapter.\n"
                               "Red below 12.2 V · green 12.2–15.5 V · amber above 15.5 V");
    tb->addWidget(m_batteryLabel);
    auto showBattery = [this](double volts) {
        // Just the number, coloured by level; grey when there is no fresh reading.
        const char *color = volts < 0 ? "#8a8a8a" : volts < 12.2 ? "#c42b1c" : volts > 15.5 ? "#c77800" : "#1e8e3e";
        m_batteryLabel->setText(volts < 0 ? QStringLiteral("— V") : QString("%1 V").arg(volts, 0, 'f', 1));
        m_batteryLabel->setStyleSheet(QString("QLabel { color:%1; font-weight:600; margin-right:14px; }").arg(color));
    };
    showBattery(-1);
    m_batteryStale.setSingleShot(true);
    m_batteryStale.setInterval(3500);
    connect(&m_batteryStale, &QTimer::timeout, this, [showBattery] { showBattery(-1); });
    connect(&m_bridge, &CanBridge::batteryVoltage, this, [this, showBattery](double v) {
        showBattery(v);
        m_batteryStale.start();
    });

    // Codes indicator: updated by the automatic read on connect; click for details.
    m_codesDialog = new CodesDialog(&m_bridge, this);
    m_codesBtn = new QToolButton;
    m_codesBtn->setText("●  Codes");
    m_codesBtn->setToolTip("Trouble codes and emissions readiness — click to open");
    m_codesBtn->setCursor(Qt::PointingHandCursor);
    m_codesBtn->setStyleSheet("QToolButton { color: #8a8a8a; font-weight: 600; }");
    tb->addWidget(m_codesBtn);
    connect(m_codesBtn, &QToolButton::clicked, this, [this] {
        m_codesDialog->show();
        m_codesDialog->raise();
        m_codesDialog->activateWindow();
    });
    connect(m_codesDialog, &CodesDialog::summaryChanged, this, [this](const QString &text, const QColor &c) {
        m_codesBtn->setText("●  " + text);
        m_codesBtn->setStyleSheet(QString("QToolButton { color: %1; font-weight: 600; }").arg(c.name()));
    });
    tb->addSeparator();

    // eeSolutions mark, right-aligned.
    auto *logo = new QLabel;
    logo->setPixmap(QIcon(":/icons/logo.png").pixmap(26, 26));
    logo->setToolTip("eeSolutions LLC");
    logo->setContentsMargins(0, 0, 6, 0);
    tb->addWidget(logo);

    // Store button ref so we can toggle it
    m_connectBtn = connectBtn;

    m_disconnectAct->setEnabled(false);
    m_startCaptureAct->setEnabled(false);
    m_stopCaptureAct->setEnabled(false);
    m_replayAct->setEnabled(false);

    connect(m_connectAct, &QAction::triggered, this, &MainWindow::onConnect);
    connect(m_disconnectAct, &QAction::triggered, this, &MainWindow::onDisconnect);
    connect(m_startCaptureAct, &QAction::triggered, this, &MainWindow::onStartCapture);
    connect(m_stopCaptureAct, &QAction::triggered, this, &MainWindow::onStopCapture);
    connect(m_replayAct, &QAction::triggered, this, &MainWindow::onReplay);
}

void MainWindow::setupCentralView() {
    auto *central = new QWidget;
    auto *v = new QVBoxLayout(central);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);

    auto *bar = new QHBoxLayout;
    bar->setContentsMargins(8, 4, 8, 4);
    m_perMessageCheck = new QCheckBox("One row per message");
    m_perMessageCheck->setToolTip("On: each message keeps one row that updates in place, with how often it is sent.\n"
                                  "Off: every frame is added as a new row (scrolling log).");
    m_perMessageCheck->setChecked(QSettings().value("view/perMessage", true).toBool());
    // Notch (SavvyCAN-style suppress): hide every ID on the bus right now, so only NEW IDs appear
    // — the way you find which message a button/door/switch sends. "Clear" shows everything again.
    m_notchBtn = new QPushButton("Notch — hide all shown");
    m_notchBtn->setToolTip("Hide every message currently in the list. New messages that appear after "
                           "(e.g. when you press a button or open a door) show highlighted. Click again to re-notch.");
    auto *clearNotch = new QPushButton("Clear");
    clearNotch->setToolTip("Show all messages again (clears notch and any hidden/only filters).");
    auto *hint = new QLabel("Right-click a row to show only that message or hide it.");
    hint->setStyleSheet("color:#777;");
    bar->addWidget(m_perMessageCheck);
    bar->addSpacing(16);
    bar->addWidget(m_notchBtn);
    bar->addWidget(clearNotch);
    bar->addSpacing(12);
    bar->addWidget(hint);
    bar->addStretch();
    v->addLayout(bar);
    connect(m_notchBtn, &QPushButton::clicked, this, &MainWindow::onNotch);
    connect(clearNotch, &QPushButton::clicked, this, &MainWindow::onClearNotch);

    m_frameModel = new QStandardItemModel(this);
    m_frameView = new QTableView;
    m_frameView->setModel(m_frameModel);
    m_frameView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_frameView->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_frameView->setAlternatingRowColors(true);
    m_frameView->verticalHeader()->hide();
    m_frameView->verticalHeader()->setDefaultSectionSize(20);
    m_frameView->horizontalHeader()->setStretchLastSection(true);
    m_frameView->setFont(QFont("Consolas", 9));
    m_frameView->setContextMenuPolicy(Qt::CustomContextMenu);
    v->addWidget(m_frameView, 1);

    setCentralWidget(central);
    connect(m_frameView, &QTableView::customContextMenuRequested, this, &MainWindow::onFrameContextMenu);
    connect(m_perMessageCheck, &QCheckBox::toggled, this, &MainWindow::setPerMessageView);
    setPerMessageView(m_perMessageCheck->isChecked());
}

void MainWindow::setPerMessageView(bool on) {
    m_perMessage = on;
    QSettings().setValue("view/perMessage", on);
    m_frameModel->clear();
    m_rowOfId.clear();
    m_lastSeen.clear();
    if (on) {
        m_frameModel->setHorizontalHeaderLabels({"Src", "Message", "ID", "Len", "Data (hex)", "Decoded",
                                                 "Seen", "Every"});
        m_frameView->setColumnWidth(MsgCol::Source, 34);
        m_frameView->setColumnWidth(MsgCol::Message, 200);
        m_frameView->setColumnWidth(MsgCol::Id, 60);
        m_frameView->setColumnWidth(MsgCol::Len, 40);
        m_frameView->setColumnWidth(MsgCol::Data, 190);
        m_frameView->setColumnWidth(MsgCol::Decoded, 380);
        m_frameView->setColumnWidth(MsgCol::Seen, 60);
    } else {
        m_frameModel->setHorizontalHeaderLabels({"Time (s)", "Src", "Message", "ID", "Len", "Data (hex)", "Decoded"});
        m_frameView->setColumnWidth(LogCol::Time, 90);
        m_frameView->setColumnWidth(LogCol::Source, 34);
        m_frameView->setColumnWidth(LogCol::Message, 200);
        m_frameView->setColumnWidth(LogCol::Id, 60);
        m_frameView->setColumnWidth(LogCol::Len, 40);
        m_frameView->setColumnWidth(LogCol::Data, 190);
    }
    refilterTable();
}

QString MainWindow::messageName(quint32 id, int dlc) const {
    if (const DbcMessage *m = m_dbc.message(id, dlc)) return DbcLibrary::humanize(m->name);
    if (id >= kDiagFrom && id <= kDiagTo)
        return id == 0x7DF ? "Scan tool request (all)" :
               id < 0x7E8 ? QString("Scan tool → module %1").arg(id - 0x7E0 + 1)
                          : QString("Module %1 reply").arg(id - 0x7E8 + 1);
    return {};
}

QString MainWindow::describeFrame(const QCanBusFrame &f) const {
    const QString uds = udsDescribe(f.frameId(), f.hasExtendedFrameFormat(), f.payload());
    return uds.isEmpty() ? m_dbc.summary(f) : uds;
}

QString MainWindow::aiContext() {
    QString out;
    out += QString("Connection: %1%2\n")
               .arg(m_bridge.isConnected() ? "connected" : "not connected")
               .arg(m_bridge.isConnected() ? QString(" via %1, high-speed CAN 500 kbit/s (OBD pins 6/14)")
                                                 .arg(QFileInfo(m_bridge.deviceName()).fileName())
                                           : QString());
    const QString vehicle = m_liveDataDock->vehicleText();
    out += "Vehicle selected in the tool: " + (vehicle.isEmpty() ? QStringLiteral("not set") : vehicle) + "\n\n";
    out += m_codesDialog->snapshotText() + "\n";
    const QString live = m_liveDataDock->snapshotText();
    out += live.isEmpty() ? QStringLiteral("Live values: none read yet\n") : live;

    // Bus summary from the capture buffer: one line per message ID.
    const QVector<QCanBusFrame> frames = m_bridge.buffer()->snapshot(5000);
    if (frames.isEmpty()) {
        out += "\nBus capture: not running (no frames captured)\n";
        return out;
    }
    struct Stat { int count = 0; double first = 0, last = 0; QCanBusFrame latest; };
    QMap<quint32, Stat> stats;
    for (const QCanBusFrame &f : frames) {
        const double t = f.timeStamp().seconds() + f.timeStamp().microSeconds() / 1e6;
        Stat &s = stats[f.frameId()];
        if (s.count == 0 || t < s.first) s.first = t;
        if (s.count == 0 || t >= s.last) { s.last = t; s.latest = f; }
        ++s.count;
    }
    out += QString("\nBus capture summary (%1 recent frames, %2 message IDs):\n").arg(frames.size()).arg(stats.size());
    int shown = 0;
    for (auto it = stats.cbegin(); it != stats.cend() && shown < 80; ++it, ++shown) {
        const Stat &s = it.value();
        const double periodMs = s.count > 1 ? (s.last - s.first) * 1000.0 / (s.count - 1) : 0;
        QString line = QString("- %1").arg(it.key(), 3, 16, QChar('0')).toUpper();
        const QString name = messageName(it.key(), int(s.latest.payload().size()));
        if (!name.isEmpty()) line += " " + name;
        line += QString(": %1 frames").arg(s.count);
        if (periodMs > 0) line += QString(", every ~%1 ms").arg(qRound(periodMs));
        line += ", last data " + formatPayload(s.latest.payload());
        const QString decoded = m_dbc.summary(s.latest);
        if (!decoded.isEmpty()) line += " (" + decoded + ")";
        out += line + "\n";
    }
    if (stats.size() > shown) out += QString("- …and %1 more IDs\n").arg(stats.size() - shown);
    return out;
}

double MainWindow::relTime(const QCanBusFrame &f) {
    const double t = f.timeStamp().seconds() + f.timeStamp().microSeconds() / 1e6;
    if (m_t0 < 0) m_t0 = t;
    return t - m_t0;
}

void MainWindow::appendLogRow(const QCanBusFrame &f, char src) {
    QList<QStandardItem *> row;
    row << new QStandardItem(QString::number(relTime(f), 'f', 4));
    row << new QStandardItem(QString(QChar(src)));
    auto *name = new QStandardItem(messageName(f.frameId(), int(f.payload().size())));
    row << name;
    auto *id = new QStandardItem(QString("%1").arg(f.frameId(), 3, 16, QChar('0')).toUpper());
    id->setData(f.frameId(), Qt::UserRole);
    row << id;
    row << new QStandardItem(QString::number(f.payload().size()));
    row << new QStandardItem(formatPayload(f.payload()));
    row << new QStandardItem(describeFrame(f));
    m_frameModel->appendRow(row);
}

void MainWindow::updateMessageRow(const QCanBusFrame &f, char src) {
    const quint32 id = f.frameId();
    const double t = relTime(f);
    const quint64 key = srcId(src, id);
    auto it = m_rowOfId.find(key);
    if (it == m_rowOfId.end()) {
        QList<QStandardItem *> row;
        for (int c = 0; c < MsgCol::Count; ++c) row << new QStandardItem;
        row[MsgCol::Source]->setText(QString(QChar(src)));
        row[MsgCol::Message]->setText(messageName(id, int(f.payload().size())));
        row[MsgCol::Id]->setText(QString("%1").arg(id, 3, 16, QChar('0')).toUpper());
        row[MsgCol::Id]->setData(id, Qt::UserRole);
        row[MsgCol::Seen]->setData(0, Qt::UserRole);
        row[MsgCol::Every]->setData(0.0, Qt::UserRole);
        // New message that appeared after a Notch: highlight it so it pops.
        if (m_notched)
            for (QStandardItem *cell : row) cell->setBackground(QColor("#d6f5d6"));
        // Keep rows sorted by ID (then source) so the list doesn't jump around.
        int insertAt = 0;
        while (insertAt < m_frameModel->rowCount() &&
               m_frameModel->item(insertAt, MsgCol::Id)->data(Qt::UserRole).toUInt() < id)
            ++insertAt;
        m_frameModel->insertRow(insertAt, row);
        for (auto r = m_rowOfId.begin(); r != m_rowOfId.end(); ++r)
            if (*r >= insertAt) ++*r;
        it = m_rowOfId.insert(key, insertAt);
    }
    const int r = *it;
    const QString data = formatPayload(f.payload());
    QStandardItem *dataItem = m_frameModel->item(r, MsgCol::Data);
    // Blue = data changed since the last frame of this message.
    dataItem->setForeground(dataItem->text() != data && !dataItem->text().isEmpty()
                                ? QColor("#0b63c4") : QColor("#1a1a1a"));
    dataItem->setText(data);
    m_frameModel->item(r, MsgCol::Len)->setText(QString::number(f.payload().size()));
    m_frameModel->item(r, MsgCol::Decoded)->setText(describeFrame(f));
    QStandardItem *seen = m_frameModel->item(r, MsgCol::Seen);
    const int count = seen->data(Qt::UserRole).toInt() + 1;
    seen->setData(count, Qt::UserRole);
    seen->setText(QString::number(count));
    auto last = m_lastSeen.find(key);
    if (last != m_lastSeen.end()) {
        QStandardItem *every = m_frameModel->item(r, MsgCol::Every);
        const double dtMs = (t - *last) * 1000.0;
        double avg = every->data(Qt::UserRole).toDouble();
        avg = avg <= 0 ? dtMs : avg * 0.9 + dtMs * 0.1;  // smoothed period
        every->setData(avg, Qt::UserRole);
        every->setText(avg >= 1000 ? QString("%1 s").arg(avg / 1000.0, 0, 'f', 1)
                                   : QString("%1 ms").arg(qRound(avg)));
    }
    m_lastSeen[key] = t;
}

void MainWindow::refilterTable() {
    m_pendingFrames.clear();
    m_frameModel->removeRows(0, m_frameModel->rowCount());
    m_rowOfId.clear();
    m_lastSeen.clear();
    const QVector<QCanBusFrame> all = m_bridge.buffer()->all();
    QVector<QCanBusFrame> shown;
    for (const QCanBusFrame &f : all)
        if (m_filter.accepts(f)) shown.append(f);
    const int startAt = m_perMessage ? 0 : qMax(0, int(shown.size()) - kMaxTableRows);
    for (int i = startAt; i < shown.size(); ++i) m_pendingFrames.enqueue({shown[i], 'J'});
    flushPendingFrames();
}

// ── Filter dock ───────────────────────────────────────────────────────────────

void MainWindow::setupFilterDock() {
    auto *dock = new QDockWidget("Filter", this);
    dock->setObjectName("FilterDock");
    m_filterDock = dock;
    dock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);

    auto *w = new QWidget;
    auto *vlay = new QVBoxLayout(w);
    vlay->setContentsMargins(8, 8, 8, 8);
    vlay->setSpacing(6);

    vlay->addWidget(new QLabel("Show:"));
    m_presetCombo = new QComboBox;
    m_presetCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_presetCombo->setMinimumContentsLength(12);
    vlay->addWidget(m_presetCombo);

    m_filterSummary = new QLabel;
    m_filterSummary->setWordWrap(true);
    m_filterSummary->setStyleSheet("color:#555;");
    vlay->addWidget(m_filterSummary);

    auto *showAll = new QPushButton("Show all messages");
    vlay->addWidget(showAll);

    auto *savePresetBtn = new QPushButton("Save as preset…");
    savePresetBtn->setStyleSheet("QPushButton { background: transparent; color: #0078d4; border: none; "
                                 "text-decoration: underline; padding: 2px; text-align: left; }");
    vlay->addWidget(savePresetBtn, 0, Qt::AlignLeft);

    // Advanced: raw ID / mask / data criteria, folded away by default.
    auto *advToggle = new QToolButton;
    advToggle->setText("Advanced (IDs, mask, data)");
    advToggle->setCheckable(true);
    advToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    advToggle->setArrowType(Qt::RightArrow);
    advToggle->setStyleSheet("QToolButton { border: none; padding: 4px 0; font-weight: 600; }");
    vlay->addWidget(advToggle);

    m_advancedBox = new QWidget;
    auto *adv = new QFormLayout(m_advancedBox);
    adv->setContentsMargins(0, 0, 0, 0);
    adv->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    adv->setRowWrapPolicy(QFormLayout::WrapLongRows);
    m_idFromEdit = new QLineEdit("0");
    m_idToEdit = new QLineEdit("1FFFFFFF");
    m_excludeRangeCheck = new QCheckBox("Hide this range instead");
    m_maskCheck = new QCheckBox("Use mask / match");
    m_maskCheck->setToolTip("The mask picks which bits of the ID must match; Match says what they must be.\n"
                            "Example: mask 7F0, match 7E0 keeps IDs 7E0–7EF.");
    m_maskEdit = new QLineEdit("7FF");
    m_matchEdit = new QLineEdit("000");
    m_maskEdit->setEnabled(false);
    m_matchEdit->setEnabled(false);
    m_payloadEdit = new QLineEdit;
    m_payloadEdit->setPlaceholderText("e.g. 41 0C");
    adv->addRow("ID from:", m_idFromEdit);
    adv->addRow("ID to:", m_idToEdit);
    adv->addRow(m_excludeRangeCheck);
    adv->addRow(m_maskCheck);
    adv->addRow("Mask:", m_maskEdit);
    adv->addRow("Match:", m_matchEdit);
    adv->addRow("Data contains:", m_payloadEdit);
    auto *applyBtn = new QPushButton("Apply");
    adv->addRow(applyBtn);
    m_advancedBox->setVisible(false);
    vlay->addWidget(m_advancedBox);
    vlay->addStretch();

    w->setMinimumWidth(180);
    dock->setWidget(w);
    addDockWidget(Qt::LeftDockWidgetArea, dock);
    resizeDocks({dock}, {230}, Qt::Horizontal);

    rebuildPresetCombo();

    connect(advToggle, &QToolButton::toggled, this, [this, advToggle](bool on) {
        m_advancedBox->setVisible(on);
        advToggle->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
    });
    connect(m_maskCheck, &QCheckBox::toggled, m_maskEdit, &QLineEdit::setEnabled);
    connect(m_maskCheck, &QCheckBox::toggled, m_matchEdit, &QLineEdit::setEnabled);
    connect(applyBtn, &QPushButton::clicked, this, &MainWindow::onApplyAdvanced);
    connect(m_presetCombo, &QComboBox::activated, this, &MainWindow::onPresetChosen);
    connect(showAll, &QPushButton::clicked, this, [this] {
        m_presetCombo->setCurrentIndex(0);
        onPresetChosen(0);
    });
    connect(savePresetBtn, &QPushButton::clicked, this, &MainWindow::savePreset);
}

void MainWindow::rebuildPresetCombo() {
    m_presetCombo->clear();
    m_presetCombo->addItem("All messages", "all");
    m_presetCombo->addItem("Hide scan-tool traffic", "nodiag");
    m_presetCombo->addItem("Scan-tool traffic only", "diag");
    m_presetCombo->addItem("Decoded messages only", "known");
    const QVariantList saved = QSettings().value("filter/presets").toList();
    if (!saved.isEmpty()) m_presetCombo->insertSeparator(m_presetCombo->count());
    for (const QVariant &v : saved) {
        const QVariantMap m = v.toMap();
        m_presetCombo->addItem(m.value("name").toString(), m);
    }
}

void MainWindow::onPresetChosen(int index) {
    const QVariant data = m_presetCombo->itemData(index);
    CanFilter f;
    f.hiddenIds = m_filter.hiddenIds;  // right-click "hide" survives preset changes
    if (data.typeId() == QMetaType::QVariantMap) {
        const QVariantMap m = data.toMap();
        f.presetName = m.value("name").toString();
        f.idFrom = m.value("idFrom").toUInt();
        f.idTo = m.value("idTo").toUInt();
        f.excludeRange = m.value("exclude").toBool();
        f.useMask = m.value("useMask").toBool();
        f.mask = m.value("mask").toUInt();
        f.match = m.value("match").toUInt();
        f.payloadContains = QByteArray::fromHex(m.value("payload").toByteArray());
        for (const QVariant &id : m.value("only").toList()) f.onlyIds.insert(id.toUInt());
        f.hiddenIds.clear();
        for (const QVariant &id : m.value("hidden").toList()) f.hiddenIds.insert(id.toUInt());
    } else {
        const QString kind = data.toString();
        if (kind == "all") {
            f.hiddenIds.clear();
        } else if (kind == "nodiag") {
            f.idFrom = kDiagFrom; f.idTo = kDiagTo; f.excludeRange = true;
            f.presetName = "Hiding scan-tool traffic";
        } else if (kind == "diag") {
            f.idFrom = kDiagFrom; f.idTo = kDiagTo;
            f.presetName = "Scan-tool traffic only";
        } else if (kind == "known") {
            f.onlyIds = m_dbc.knownIds();
            f.presetName = "Decoded messages only";
        }
    }
    m_filter = f;
    m_idFromEdit->setText(QString::number(f.idFrom, 16).toUpper());
    m_idToEdit->setText(QString::number(f.idTo, 16).toUpper());
    m_excludeRangeCheck->setChecked(f.excludeRange);
    m_maskCheck->setChecked(f.useMask);
    m_maskEdit->setText(QString::number(f.useMask ? f.mask : 0x7FF, 16).toUpper());
    m_matchEdit->setText(QString::number(f.match, 16).toUpper());
    m_payloadEdit->setText(QString::fromLatin1(f.payloadContains.toHex(' ').toUpper()));
    filterChanged();
}

void MainWindow::onApplyAdvanced() {
    bool ok;
    m_filter.idFrom = m_idFromEdit->text().toUInt(&ok, 16);
    if (!ok) m_filter.idFrom = 0;
    m_filter.idTo = m_idToEdit->text().toUInt(&ok, 16);
    if (!ok) m_filter.idTo = 0x1FFFFFFF;
    m_filter.excludeRange = m_excludeRangeCheck->isChecked();
    m_filter.useMask = m_maskCheck->isChecked();
    m_filter.mask = m_maskEdit->text().toUInt(&ok, 16);
    if (!ok) m_filter.mask = 0;
    m_filter.match = m_matchEdit->text().toUInt(&ok, 16);
    if (!ok) m_filter.match = 0;
    const QString pay = m_payloadEdit->text().simplified().remove(' ');
    m_filter.payloadContains = pay.isEmpty() ? QByteArray() : QByteArray::fromHex(pay.toLatin1());
    m_filter.presetName.clear();
    filterChanged();
}

void MainWindow::filterChanged() {
    m_filterSummary->setText(m_filter.describe());
    refilterTable();
}

void MainWindow::savePreset() {
    bool ok;
    const QString name = QInputDialog::getText(this, "Save filter preset", "Name for this filter:",
                                               QLineEdit::Normal, m_filter.presetName, &ok).trimmed();
    if (!ok || name.isEmpty()) return;
    QVariantMap m;
    m["name"] = name;
    m["idFrom"] = m_filter.idFrom;
    m["idTo"] = m_filter.idTo;
    m["exclude"] = m_filter.excludeRange;
    m["useMask"] = m_filter.useMask;
    m["mask"] = m_filter.mask;
    m["match"] = m_filter.match;
    m["payload"] = m_filter.payloadContains.toHex();
    QVariantList only, hidden;
    for (quint32 id : m_filter.onlyIds) only << id;
    for (quint32 id : m_filter.hiddenIds) hidden << id;
    m["only"] = only;
    m["hidden"] = hidden;
    QSettings s;
    QVariantList presets = s.value("filter/presets").toList();
    for (int i = presets.size() - 1; i >= 0; --i)
        if (presets[i].toMap().value("name").toString() == name) presets.removeAt(i);
    presets << m;
    s.setValue("filter/presets", presets);
    rebuildPresetCombo();
    m_presetCombo->setCurrentIndex(m_presetCombo->findText(name));
    m_filter.presetName = name;
    filterChanged();
}

void MainWindow::onFrameContextMenu(const QPoint &pos) {
    const QModelIndex idx = m_frameView->indexAt(pos);
    QMenu menu(this);
    if (idx.isValid()) {
        const int idCol = m_perMessage ? int(MsgCol::Id) : int(LogCol::Id);
        const quint32 id = m_frameModel->item(idx.row(), idCol)->data(Qt::UserRole).toUInt();
        QString label = messageName(id);
        const QString hexId = QString::number(id, 16).rightJustified(3, '0').toUpper();
        label = label.isEmpty() ? QString("ID %1").arg(hexId) : QString("%1 (%2)").arg(label, hexId);
        menu.addAction("Show only " + label, this, [this, id] {
            m_filter.onlyIds = {id};
            m_filter.presetName.clear();
            m_presetCombo->setCurrentIndex(-1);
            filterChanged();
        });
        menu.addAction("Also show " + label, this, [this, id] {
            if (!m_filter.onlyIds.isEmpty()) m_filter.onlyIds.insert(id);
            m_filter.hiddenIds.remove(id);
            filterChanged();
        })->setEnabled(!m_filter.onlyIds.isEmpty() || m_filter.hiddenIds.contains(id));
        menu.addAction("Hide " + label, this, [this, id] {
            m_filter.hiddenIds.insert(id);
            m_filter.onlyIds.remove(id);
            filterChanged();
        });
        menu.addSeparator();
        const int srcCol = m_perMessage ? int(MsgCol::Source) : int(LogCol::Source);
        const QString srcText = m_frameModel->item(idx.row(), srcCol)->text();
        const char src = srcText.isEmpty() ? 'G' : srcText.at(0).toLatin1();
        menu.addAction("Define signal from " + label + "…", this, [this, id, src] {
            defineSignalDialog(id, src);
        });
        menu.addSeparator();
        // Send/replay this message out the C31G (needs Snort in Active mode): edit the bytes first.
        menu.addAction("Send / edit this frame…", this, [this, id, src] { sendComposer(id, src); });
        menu.addAction("Send once (unchanged)", this, [this, id, src] { sendFrameOnce(id, src); });
        if (m_repeatSend.isActive())
            menu.addAction("Stop repeating send", this, [this] {
                m_repeatSend.stop();
                statusBar()->showMessage("Stopped repeating send", 3000);
            });
        if (isDiagId(id, id > 0x7FF))
            menu.addAction("Add to module test…", this, [this, id, src] { addActuatorFromRow(id, src); });
        menu.addSeparator();
        menu.addAction("Copy row", this, [this, idx] {
            QStringList cells;
            for (int c = 0; c < m_frameModel->columnCount(); ++c)
                cells << m_frameModel->item(idx.row(), c)->text();
            QGuiApplication::clipboard()->setText(cells.join('\t'));
        });
        menu.addSeparator();
    }
    menu.addAction("Show all messages", this, [this] {
        m_presetCombo->setCurrentIndex(0);
        onPresetChosen(0);
    });
    menu.exec(m_frameView->viewport()->mapToGlobal(pos));
}

// ── Other docks / status ─────────────────────────────────────────────────────

void MainWindow::setupDiagnoseDock() {
    m_diagnosePanel = new DiagnosePanel(&m_bridge, this);
    m_diagnosePanel->setFilter(&m_filter);
    addDockWidget(Qt::RightDockWidgetArea, m_diagnosePanel);
}

void MainWindow::setupLiveDataDock() {
    m_liveDataDock = new LiveDataDock(&m_bridge, &m_dbc, &m_enhanced, &m_oe, this);
    addDockWidget(Qt::BottomDockWidgetArea, m_liveDataDock);
}

void MainWindow::setupStatusBar() {
    m_statusLabel = new QLabel("Not connected");
    m_frameCountLabel = new QLabel("Frames: 0");
    m_errorCountLabel = new QLabel("Errors: 0");
    statusBar()->addWidget(m_statusLabel, 1);
    statusBar()->addPermanentWidget(m_errorCountLabel);
    statusBar()->addPermanentWidget(m_frameCountLabel);
}

void MainWindow::updateStatusBar() {
    m_frameCountLabel->setText(QString("Frames: %1").arg(m_bridge.totalFrames()));
    m_errorCountLabel->setText(QString("Errors: %1").arg(m_bridge.errorFrames()));
}

void MainWindow::onConnect() {
    ConnectDialog dlg(this);
    if (dlg.exec() != QDialog::Accepted) return;

    QString err;
    QString dll = dlg.selectedDllPath();
    if (dll.isEmpty()) return;
    QSettings().setValue("can/dll_path", dll);

    if (!m_bridge.connectDevice(dll, &err))
        QMessageBox::critical(this, "Connection failed", err);
}

void MainWindow::onDisconnect() {
    m_bridge.disconnectDevice();
}

void MainWindow::onStartCapture() {
    m_bridge.startCapture();
    m_t0 = -1;
    m_frameModel->removeRows(0, m_frameModel->rowCount());
    m_rowOfId.clear();
    m_lastSeen.clear();
    updateCaptureActions();
    m_statusLabel->setText("Capturing…");
}

void MainWindow::onStopCapture() {
    m_bridge.stopCapture();
    updateCaptureActions();
    m_statusLabel->setText("Stopped");
}

void MainWindow::saveCaptureCsv() {
    const QVector<QCanBusFrame> all = m_bridge.buffer()->all();
    if (all.isEmpty()) { QMessageBox::information(this, "Save CSV", "Nothing captured yet. Hit Capture first."); return; }
    QString path = QFileDialog::getSaveFileName(this, "Save capture as CSV", "capture.csv", "CSV (*.csv)");
    if (path.isEmpty()) return;
    QFile f(path);
    if (!f.open(QFile::WriteOnly | QFile::Text)) { QMessageBox::critical(this, "Save CSV", f.errorString()); return; }
    QTextStream out(&f);
    // SavvyCAN-compatible GVRET CSV; also opens in Excel and the cansub Python tools.
    out << "Time Stamp,ID,Extended,Dir,Bus,LEN,D1,D2,D3,D4,D5,D6,D7,D8\n";
    for (const QCanBusFrame &fr : all) {
        const qint64 us = fr.timeStamp().seconds() * 1000000 + fr.timeStamp().microSeconds();
        out << us << ',' << QString::number(fr.frameId(), 16).toUpper() << ','
            << (fr.hasExtendedFrameFormat() ? "true" : "false") << ",Rx,0,"
            << fr.payload().size();
        const QByteArray p = fr.payload();
        for (int i = 0; i < 8; ++i) out << ',' << (i < p.size() ? QString::number(quint8(p[i]), 16).rightJustified(2, '0').toUpper() : QString());
        out << '\n';
    }
    statusBar()->showMessage(QString("Saved %1 frames to %2").arg(all.size()).arg(QFileInfo(path).fileName()), 6000);
}

void MainWindow::openCaptureCsv() {
    QString path = QFileDialog::getOpenFileName(this, "Open capture CSV", {}, "CSV (*.csv)");
    if (path.isEmpty()) return;
    QFile f(path);
    if (!f.open(QFile::ReadOnly | QFile::Text)) { QMessageBox::critical(this, "Open CSV", f.errorString()); return; }
    QTextStream in(&f);
    QVector<QCanBusFrame> frames;
    bool first = true;
    while (!in.atEnd()) {
        QString line = in.readLine().trimmed();
        if (line.isEmpty()) continue;
        if (first) { first = false; if (line.startsWith("Time", Qt::CaseInsensitive)) continue; }  // skip header
        const QStringList c = line.split(',');
        if (c.size() < 6) continue;
        bool ok = false;
        const qint64 us = c[0].toLongLong();
        const quint32 id = c[1].trimmed().toUInt(&ok, 16);
        if (!ok) continue;
        const bool ext = c[2].trimmed().compare("true", Qt::CaseInsensitive) == 0 || c[2].trimmed() == "1";
        const int len = c[5].toInt();
        QByteArray data;
        for (int i = 0; i < len && 6 + i < c.size(); ++i) data.append(char(c[6 + i].trimmed().toUInt(nullptr, 16)));
        QCanBusFrame fr(id, data);
        fr.setExtendedFrameFormat(ext);
        fr.setTimeStamp(QCanBusFrame::TimeStamp::fromMicroSeconds(us));
        frames.append(fr);
    }
    if (frames.isEmpty()) { QMessageBox::warning(this, "Open CSV", "No frames read from that file."); return; }
    // Load into the table + buffer (tagged F = from file) so you can analyse it or Replay it.
    m_frameModel->removeRows(0, m_frameModel->rowCount());
    m_rowOfId.clear(); m_lastSeen.clear(); m_t0 = -1;
    m_bridge.buffer()->clear();
    for (const QCanBusFrame &fr : frames) { m_bridge.buffer()->push(fr); m_pendingFrames.enqueue({fr, 'F'}); }
    flushPendingFrames();
    m_replayAct->setEnabled(true);
    statusBar()->showMessage(QString("Loaded %1 frames from %2 — right-click to send, or press Replay")
                                 .arg(frames.size()).arg(QFileInfo(path).fileName()), 8000);
}

void MainWindow::updateCaptureActions() {
    const bool live = m_bridge.isConnected() || (m_snort && m_snort->isOpen());
    const bool capturing = m_bridge.isCapturing();
    m_startCaptureAct->setEnabled(live && !capturing);
    m_stopCaptureAct->setEnabled(capturing);
    if (!m_replay->isRunning())
        m_replayAct->setEnabled(live && m_bridge.buffer()->size() > 0);
}

void MainWindow::onClearCapture() {
    m_pendingFrames.clear();
    m_bridge.buffer()->clear();
    m_frameModel->removeRows(0, m_frameModel->rowCount());
    m_rowOfId.clear();
    m_lastSeen.clear();
    m_t0 = -1;
    m_replayAct->setEnabled(false);
    updateStatusBar();
}

void MainWindow::onReplay() {
    if (m_replay->isRunning()) {
        m_replay->stop();
        m_replayAct->setText("↺  Replay");
        return;
    }

    QVector<QCanBusFrame> frames;
    for (const QCanBusFrame &f : m_bridge.buffer()->all())
        if (m_filter.accepts(f)) frames.append(f);

    if (frames.isEmpty()) {
        QMessageBox::information(this, "Replay", "No frames match the current filter.");
        return;
    }

    // If Snort is the only live device, replay out the C31G (needs Active mode); else the J2534 bridge.
    if (!m_bridge.isConnected() && m_snort) {
        if (!ensureSnortActive()) return;
        m_replay->setSender([this](const QCanBusFrame &f) { return m_snort->writeFrame(f); });
    } else {
        m_replay->setSender(nullptr);
    }
    m_replay->setFrames(frames);
    m_replay->start();
    m_replayAct->setText("■  Stop Replay");
}

void MainWindow::onReplayFinished() {
    m_replayAct->setText("↺  Replay");
}

void MainWindow::onFrameReceived(const QCanBusFrame &frame) {
    handleIncoming(frame, 'J');   // Scan (J2534), while capturing
}

void MainWindow::onSnortFrames(const QList<QCanBusFrame> &frames) {
    for (const QCanBusFrame &f : frames) handleIncoming(f, 'G');   // Snort (C31G), always-on
}

void MainWindow::handleIncoming(const QCanBusFrame &f, char src) {
    const quint64 key = srcId(src, f.frameId());
    m_latestPayload[key] = f.payload();   // newest data, for the DBC builder preview
    // Record Snort frames into the capture buffer while capturing (J2534 frames are pushed by
    // CanBridge itself), so Save and Replay work for the sniffer too.
    if (src == 'G' && m_bridge.isCapturing()) m_bridge.buffer()->push(f);
    // Notch: a message stays hidden only while its bytes match the snapshot taken at notch time.
    // The moment its data changes (you pressed the button), it reappears — and stays visible.
    if (m_notched) {
        auto snap = m_notchSnapshot.constFind(key);
        if (snap != m_notchSnapshot.constEnd()) {
            if (*snap == f.payload()) return;       // unchanged since notch → keep hidden
            m_notchSnapshot.remove(key);            // changed → reveal from now on
        }
        // not in the snapshot (a genuinely new ID) also falls through and shows, highlighted
    }
    if (m_filter.accepts(f)) m_pendingFrames.enqueue({f, src});
}

void MainWindow::flushPendingFrames() {
    if (m_pendingFrames.isEmpty()) return;

    if (m_perMessage) {
        // Only the newest frame per (source,ID) matters for display, but every frame
        // counts towards "Seen" and the period average.
        while (!m_pendingFrames.isEmpty()) { Incoming in = m_pendingFrames.dequeue(); updateMessageRow(in.frame, in.src); }
    } else {
        m_frameModel->blockSignals(true);
        int over = (m_frameModel->rowCount() + m_pendingFrames.size()) - kMaxTableRows;
        if (over > 0) m_frameModel->removeRows(0, over);
        while (!m_pendingFrames.isEmpty()) { Incoming in = m_pendingFrames.dequeue(); appendLogRow(in.frame, in.src); }
        m_frameModel->blockSignals(false);
        // Notify the view of all changes at once.
        emit m_frameModel->layoutChanged();
        m_frameView->scrollToBottom();
    }
    updateStatusBar();
}

void MainWindow::onSnort() {
    if (m_snort) {   // running -> stop
        m_repeatSend.stop();
        m_snort->close();
        m_snort->deleteLater();
        m_snort = nullptr;
        m_snortAct->setChecked(false);
        m_snortAct->setText("Snort");
        statusBar()->showMessage("Snort stopped", 4000);
        updateCaptureActions();
        return;
    }
    if (!GsUsbDevice::present()) {
        QMessageBox::warning(this, "Snort", "No SH-C31G found. Plug it in (it appears under "
                             "Universal Serial Bus devices) and try again.");
        m_snortAct->setChecked(false);
        return;
    }
    // Small picker: speed (incl MS-CAN), optional CAN FD + data rate, Listen vs Active.
    QDialog dlg(this);
    dlg.setWindowTitle("Snort — SH-C31G sniffer");
    auto *form = new QFormLayout(&dlg);
    auto *speed = new QComboBox;
    for (auto p : {qMakePair(QString("500k (HS-CAN)"), 500000), qMakePair(QString("250k"), 250000),
                   qMakePair(QString("125k (Ford/Mazda MS-CAN)"), 125000), qMakePair(QString("95.2k (GM MS-CAN)"), 95238),
                   qMakePair(QString("1M"), 1000000)})
        speed->addItem(p.first, p.second);
    auto *fd = new QCheckBox("CAN FD");
    auto *dataSpeed = new QComboBox;
    for (auto p : {qMakePair(QString("2M"), 2000000), qMakePair(QString("5M"), 5000000),
                   qMakePair(QString("1M"), 1000000), qMakePair(QString("4M"), 4000000)})
        dataSpeed->addItem(p.first, p.second);
    dataSpeed->setEnabled(false);
    connect(fd, &QCheckBox::toggled, dataSpeed, &QWidget::setEnabled);
    auto *listen = new QCheckBox("Listen-only (silent — never touches the bus)");
    listen->setChecked(true);
    form->addRow("Bitrate:", speed);
    form->addRow(fd, dataSpeed);
    form->addRow("", listen);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    bb->button(QDialogButtonBox::Ok)->setText("Start Snort");
    form->addRow(bb);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted) { m_snortAct->setChecked(false); return; }

    m_snort = new GsUsbDevice(this);
    connect(m_snort, &GsUsbDevice::framesReceived, this, &MainWindow::onSnortFrames);
    connect(m_snort, &GsUsbDevice::errorOccurred, this, &MainWindow::onBusError);
    const quint32 nominal = speed->currentData().toUInt();
    const quint32 data = fd->isChecked() ? dataSpeed->currentData().toUInt() : 0u;
    QString err;
    if (!m_snort->open(nominal, data, listen->isChecked(), &err)) {
        QMessageBox::critical(this, "Snort", err);
        m_snort->deleteLater(); m_snort = nullptr;
        m_snortAct->setChecked(false);
        return;
    }
    m_snortNominal = nominal; m_snortData = data; m_snortListen = listen->isChecked();
    m_snortAct->setChecked(true);
    m_snortAct->setText(m_snortListen ? "Snort ●" : "Snort ● (active)");
    if (m_t0 < 0) m_t0 = 0;   // so Snort frames timestamp from now even without a Scan capture
    statusBar()->showMessage(QString("Snort %1 — %2%3 · %4  (frames tagged G)")
                                 .arg(m_snortListen ? "listening" : "ACTIVE", data ? "CAN FD " : "",
                                      QString::number(nominal / 1000.0) + "k", m_snort->firmware()), 6000);
    updateCaptureActions();
}

// Make sure Snort can transmit (Active mode). Listen-only is silent, so ask before joining the bus.
bool MainWindow::ensureSnortActive() {
    if (!m_snort || !m_snort->isOpen()) {
        QMessageBox::warning(this, "Send", "Start Snort first — it's the device that sends.");
        return false;
    }
    if (m_snort->canSend()) return true;   // already Active
    if (QMessageBox::question(this, "Send on the bus",
            "Snort is in Listen-only (silent). Switch it to Active so it can transmit on the bus?")
        != QMessageBox::Yes)
        return false;
    QString err;
    m_snort->close();
    auto *ns = new GsUsbDevice(this);
    if (!ns->open(m_snortNominal, m_snortData, false, &err)) {
        QMessageBox::critical(this, "Send", "Could not switch to Active: " + err);
        ns->deleteLater();
        if (m_snort) { m_snort->deleteLater(); m_snort = nullptr; }
        m_snortAct->setChecked(false); m_snortAct->setText("Snort");
        updateCaptureActions();
        return false;
    }
    connect(ns, &GsUsbDevice::framesReceived, this, &MainWindow::onSnortFrames);
    connect(ns, &GsUsbDevice::errorOccurred, this, &MainWindow::onBusError);
    m_snort->deleteLater();
    m_snort = ns;
    m_snortListen = false;
    m_snortAct->setText("Snort ● (active)");
    statusBar()->showMessage("Snort switched to Active — it can now transmit", 5000);
    return true;
}

QCanBusFrame MainWindow::frameFor(quint32 id, char src) const {
    QCanBusFrame f(id, m_latestPayload.value(srcId(src, id)));
    f.setExtendedFrameFormat(id > 0x7FF);
    return f;
}

void MainWindow::sendFrameOnce(quint32 id, char src) {
    if (!ensureSnortActive()) return;
    const QCanBusFrame f = frameFor(id, src);
    if (f.payload().isEmpty()) { statusBar()->showMessage("No data seen for this ID yet", 4000); return; }
    const bool ok = m_snort->writeFrame(f);
    statusBar()->showMessage(ok ? QString("Sent %1  %2").arg(QString::number(id, 16).toUpper(),
                                                             QString::fromLatin1(f.payload().toHex(' ').toUpper()))
                                : "Send failed", 5000);
}

void MainWindow::sendFrameRepeat(quint32 id, char src) {
    if (!ensureSnortActive()) return;
    const QCanBusFrame f = frameFor(id, src);
    if (f.payload().isEmpty()) { statusBar()->showMessage("No data seen for this ID yet", 4000); return; }
    bool ok = false;
    const int ms = QInputDialog::getInt(this, "Send repeatedly",
                                        "Send this frame every … ms:", 100, 5, 10000, 5, &ok);
    if (!ok) return;
    m_repeatFrame = f;
    m_repeatSend.start(ms);
    statusBar()->showMessage(QString("Repeating %1 every %2 ms — right-click → Stop repeating send")
                                 .arg(QString::number(id, 16).toUpper()).arg(ms), 0);
}

// Edit a frame's bytes, then send it (once or repeating). This is the "change one byte and send"
// tool — e.g. take the turn-signal message, flip its bit, and watch the lamp.
void MainWindow::sendComposer(quint32 id, char src) {
    auto *dlg = new QDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle(QString("Send frame — ID %1").arg(QString::number(id, 16).toUpper()));
    auto *form = new QFormLayout(dlg);
    auto *idEdit = new QLineEdit(QString::number(id, 16).toUpper());
    auto *dataEdit = new QLineEdit(QString::fromLatin1(m_latestPayload.value(srcId(src, id)).toHex(' ').toUpper()));
    dataEdit->setToolTip("Data bytes in hex — each pair is one byte, e.g. 82 87 00 06 80 00 00 00.");
    auto *live = new QLabel("—"); live->setStyleSheet("color:#777;");
    auto *status = new QLabel;
    form->addRow("ID (hex):", idEdit);
    form->addRow("Data (hex):", dataEdit);
    form->addRow("Live now:", live);
    auto *roww = new QWidget; auto *row = new QHBoxLayout(roww); row->setContentsMargins(0, 0, 0, 0);
    auto *once = new QPushButton("Send once");
    auto *rep = new QPushButton("Send repeatedly…");
    auto *stop = new QPushButton("Stop");
    auto *close = new QPushButton("Close");
    row->addWidget(once); row->addWidget(rep); row->addWidget(stop); row->addStretch(); row->addWidget(close);
    form->addRow(roww);
    form->addRow(status);

    auto build = [=](QCanBusFrame *out) -> bool {
        bool ok = false;
        quint32 fid = idEdit->text().trimmed().toUInt(&ok, 16);
        if (!ok) { status->setText("Bad ID (hex)"); return false; }
        QByteArray data = QByteArray::fromHex(QString(dataEdit->text()).remove(' ').toLatin1());
        QCanBusFrame f(fid, data);
        f.setExtendedFrameFormat(fid > 0x7FF);
        *out = f;
        return true;
    };
    connect(once, &QPushButton::clicked, dlg, [=] {
        QCanBusFrame f; if (!build(&f) || !ensureSnortActive()) return;
        status->setText(m_snort->writeFrame(f) ? "Sent" : "Send failed");
    });
    connect(rep, &QPushButton::clicked, dlg, [=] {
        QCanBusFrame f; if (!build(&f) || !ensureSnortActive()) return;
        bool ok = false;
        int ms = QInputDialog::getInt(dlg, "Send repeatedly", "Every … ms:", 100, 5, 10000, 5, &ok);
        if (!ok) return;
        m_repeatFrame = f; m_repeatSend.start(ms);
        status->setText(QString("Repeating every %1 ms").arg(ms));
    });
    connect(stop, &QPushButton::clicked, dlg, [this, status] { m_repeatSend.stop(); status->setText("Stopped repeating"); });
    connect(close, &QPushButton::clicked, dlg, &QDialog::accept);
    auto *t = new QTimer(dlg);
    connect(t, &QTimer::timeout, dlg, [=] {
        bool ok = false; quint32 fid = idEdit->text().trimmed().toUInt(&ok, 16);
        if (ok) live->setText(QString::fromLatin1(m_latestPayload.value(srcId(src, fid)).toHex(' ').toUpper()));
    });
    t->start(200);
    dlg->show();   // modeless, so the bus keeps streaming behind it
}

// ── Actuator tests (bidirectional toggle buttons) ───────────────────────────────
QString MainWindow::actuatorKey() const {
    QString v = m_liveDataDock ? (m_liveDataDock->make() + "_" + m_liveDataDock->model()) : QString();
    v.replace(QRegularExpression("[^A-Za-z0-9]+"), "_");
    while (v.startsWith('_')) v.remove(0, 1);
    while (v.endsWith('_')) v.chop(1);
    return "actuators/" + (v.isEmpty() ? QStringLiteral("default") : v);
}

QByteArray MainWindow::isoTpSingle(const QByteArray &uds) {
    QByteArray f;
    const int n = qMin(uds.size(), 7);
    f.append(char(n));
    f.append(uds.left(n));
    while (f.size() < 8) f.append('\0');   // pad to 8
    return f;
}

void MainWindow::loadActuators() {
    m_actuators.clear();
    const QJsonArray arr = QJsonDocument::fromJson(QSettings().value(actuatorKey()).toByteArray()).array();
    for (const QJsonValue &v : arr) {
        const QJsonObject o = v.toObject();
        ActuatorTest t;
        t.name = o["name"].toString();
        t.reqId = o["req"].toString().toUInt(nullptr, 16);
        t.session = QByteArray::fromHex(o["session"].toString().toLatin1());
        t.on = QByteArray::fromHex(o["on"].toString().toLatin1());
        t.off = QByteArray::fromHex(o["off"].toString().toLatin1());
        t.tester = o["tester"].toBool(true);
        if (!t.name.isEmpty()) m_actuators.append(t);
    }
}

void MainWindow::saveActuators() {
    QJsonArray arr;
    for (const ActuatorTest &t : m_actuators) {
        QJsonObject o;
        o["name"] = t.name;
        o["req"] = QString::number(t.reqId, 16).toUpper();
        o["session"] = QString::fromLatin1(t.session.toHex());
        o["on"] = QString::fromLatin1(t.on.toHex());
        o["off"] = QString::fromLatin1(t.off.toHex());
        o["tester"] = t.tester;
        arr.append(o);
    }
    QSettings().setValue(actuatorKey(), QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

void MainWindow::setupActuatorDock() {
    m_actuatorDock = new QDockWidget("Actuator tests", this);
    m_actuatorDock->setObjectName("ActuatorDock");
    auto *scroll = new QScrollArea;
    m_actuatorButtons = new QWidget;
    new QVBoxLayout(m_actuatorButtons);
    scroll->setWidget(m_actuatorButtons);
    scroll->setWidgetResizable(true);
    m_actuatorDock->setWidget(scroll);
    addDockWidget(Qt::RightDockWidgetArea, m_actuatorDock);
    if (m_viewMenu) m_viewMenu->addAction(m_actuatorDock->toggleViewAction());
    if (m_liveDataDock)
        connect(m_liveDataDock, &LiveDataDock::vehicleChanged, this, [this] { loadActuators(); rebuildActuatorButtons(); });
    loadActuators();
    rebuildActuatorButtons();
}

void MainWindow::rebuildActuatorButtons() {
    if (!m_actuatorButtons) return;
    auto *lay = qobject_cast<QVBoxLayout *>(m_actuatorButtons->layout());
    QLayoutItem *it;
    while ((it = lay->takeAt(0))) { if (it->widget()) delete it->widget(); delete it; }
    m_actuatorsOn = 0; m_testerTimer.stop();
    auto *hint = new QLabel(m_actuators.isEmpty()
        ? "No tests yet.\n\nCapture your scan tool, then right-click the command line\n"
          "(\"IO control …\" or \"Routine START …\") → Add to module test."
        : "Press a test to run it. It switches Snort to Active, opens the\nsession and holds tester-present while on. Right-click to delete.");
    hint->setStyleSheet("color:#777;");
    hint->setWordWrap(true);
    lay->addWidget(hint);
    for (int i = 0; i < m_actuators.size(); ++i) {
        const ActuatorTest &t = m_actuators[i];
        auto *b = new QPushButton(t.name);
        b->setCheckable(true);
        b->setMinimumHeight(34);
        b->setStyleSheet("QPushButton:checked { background:#2e7d32; color:white; font-weight:600; }");
        b->setToolTip(QString("%1 → %2   (session %3)")
                          .arg(QString::number(t.reqId, 16).toUpper(),
                               QString::fromLatin1(t.on.toHex(' ').toUpper()),
                               t.session.isEmpty() ? QStringLiteral("none") : QString::fromLatin1(t.session.toHex(' ').toUpper())));
        connect(b, &QPushButton::toggled, this, [this, i](bool on) { runActuator(i, on); });
        b->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(b, &QWidget::customContextMenuRequested, this, [this, i](const QPoint &) {
            QMenu m(this);
            m.addAction("Delete \"" + m_actuators[i].name + "\"", this, [this, i] {
                m_actuators.removeAt(i); saveActuators(); rebuildActuatorButtons();
            });
            m.exec(QCursor::pos());
        });
        lay->addWidget(b);
    }
    lay->addStretch();
}

void MainWindow::runActuator(int index, bool on) {
    if (index < 0 || index >= m_actuators.size()) return;
    const ActuatorTest t = m_actuators[index];
    if (on) {
        if (!ensureSnortActive()) {   // couldn't go Active: pop the button back out
            auto *lay = qobject_cast<QVBoxLayout *>(m_actuatorButtons->layout());
            if (lay && index + 1 < lay->count())
                if (auto *b = qobject_cast<QPushButton *>(lay->itemAt(index + 1)->widget())) {
                    QSignalBlocker s(b); b->setChecked(false);
                }
            return;
        }
        if (!t.session.isEmpty()) m_snort->writeFrame(QCanBusFrame(t.reqId, isoTpSingle(t.session)));
        m_snort->writeFrame(QCanBusFrame(t.reqId, isoTpSingle(t.on)));
        if (t.tester) { ++m_actuatorsOn; if (!m_testerTimer.isActive()) m_testerTimer.start(1000); }
        statusBar()->showMessage("Running: " + t.name + "  — press again to stop", 0);
    } else {
        if (m_snort && m_snort->canSend()) {
            if (!t.off.isEmpty()) m_snort->writeFrame(QCanBusFrame(t.reqId, isoTpSingle(t.off)));
            if (!t.session.isEmpty()) m_snort->writeFrame(QCanBusFrame(t.reqId, isoTpSingle(QByteArray::fromHex("1081"))));
        }
        if (t.tester && m_actuatorsOn > 0 && --m_actuatorsOn == 0) m_testerTimer.stop();
        statusBar()->showMessage("Stopped: " + t.name, 4000);
    }
}

void MainWindow::addActuatorFromRow(quint32 id, char src) {
    const quint32 reqId = (id >= 0x7E8 && id <= 0x7EF) ? id - 8 : id;   // if they clicked the reply, use the request
    const QByteArray full = m_latestPayload.value(srcId(src, id));
    QByteArray uds;
    if (!full.isEmpty()) uds = full.mid(1, quint8(full[0]) & 0x0F);      // strip the ISO-TP single-frame length byte
    // Auto-find the session: the most recent "10 xx" sent to this module in the capture.
    QByteArray session;
    const QVector<QCanBusFrame> all = m_bridge.buffer()->all();
    for (int i = all.size() - 1; i >= 0; --i) {
        if (all[i].frameId() != reqId) continue;
        const QByteArray p = all[i].payload();
        if (p.size() >= 2 && (quint8(p[0]) >> 4) == 0 && quint8(p[1]) == 0x10) { session = p.mid(1, quint8(p[0]) & 0x0F); break; }
    }
    // Guess the OFF command.
    QByteArray off;
    if (uds.size() >= 2 && quint8(uds[0]) == 0x31) { off = uds; off[1] = char(0x02); }          // routine: start→stop
    else if (uds.size() >= 1 && quint8(uds[0]) == 0x2F) { off = uds; off[off.size() - 1] = char(0x00); }  // io control: →00

    QDialog dlg(this);
    dlg.setWindowTitle("Add module test");
    auto *form = new QFormLayout(&dlg);
    auto *name = new QLineEdit; name->setPlaceholderText("e.g. Fuel pump");
    auto *reqE = new QLineEdit(QString::number(reqId, 16).toUpper());
    auto *sesE = new QLineEdit(QString::fromLatin1(session.toHex(' ').toUpper()));
    auto *onE = new QLineEdit(QString::fromLatin1(uds.toHex(' ').toUpper()));
    auto *offE = new QLineEdit(QString::fromLatin1(off.toHex(' ').toUpper()));
    auto *tp = new QCheckBox("Hold tester-present while on"); tp->setChecked(true);
    form->addRow("Name:", name);
    form->addRow("Request ID (hex):", reqE);
    form->addRow("Session (hex):", sesE);
    form->addRow("ON command (hex):", onE);
    form->addRow("OFF command (hex):", offE);
    form->addRow("", tp);
    auto *note = new QLabel("The app adds ISO-TP framing and tester-present. Security-access tests can't be saved.");
    note->setStyleSheet("color:#777;"); note->setWordWrap(true);
    form->addRow(note);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    form->addRow(bb);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted) return;

    ActuatorTest t;
    t.name = name->text().trimmed();
    if (t.name.isEmpty()) { QMessageBox::warning(this, "Add test", "Give the test a name."); return; }
    bool ok = false;
    t.reqId = reqE->text().trimmed().toUInt(&ok, 16); if (!ok) t.reqId = reqId;
    t.session = QByteArray::fromHex(QString(sesE->text()).remove(' ').toLatin1());
    t.on = QByteArray::fromHex(QString(onE->text()).remove(' ').toLatin1());
    t.off = QByteArray::fromHex(QString(offE->text()).remove(' ').toLatin1());
    t.tester = tp->isChecked();
    if ((!t.on.isEmpty() && quint8(t.on[0]) == 0x27) || (!t.session.isEmpty() && quint8(t.session[0]) == 0x27)) {
        QMessageBox::warning(this, "Add test", "That sequence uses Security Access (27) — it's gated and can't be saved as a plain test.");
        return;
    }
    m_actuators.append(t);
    saveActuators();
    rebuildActuatorButtons();
    statusBar()->showMessage("Saved test: " + t.name + " — it's now a button under Actuator tests", 6000);
}

void MainWindow::onNotch() {
    // Snapshot every message's current data, then hide them. A message reappears the instant its
    // bytes change (e.g. you turn on the lights) — that's how you find which message carries it.
    m_notchSnapshot.clear();
    for (auto it = m_latestPayload.cbegin(); it != m_latestPayload.cend(); ++it)
        m_notchSnapshot.insert(it.key(), it.value());
    m_notched = true;
    m_notchBtn->setText("Re-notch");
    // Empty the table; only changed or new messages repopulate it (highlighted).
    m_frameModel->removeRows(0, m_frameModel->rowCount());
    m_rowOfId.clear();
    m_lastSeen.clear();
    statusBar()->showMessage(QString("Notched %1 messages — only ones whose data changes (or new IDs) "
                                     "will appear now").arg(m_notchSnapshot.size()), 6000);
}

void MainWindow::onClearNotch() {
    m_notched = false;
    m_notchSnapshot.clear();
    m_filter.hiddenIds.clear();
    m_filter.onlyIds.clear();
    m_notchBtn->setText("Notch — hide all shown");
    m_frameModel->removeRows(0, m_frameModel->rowCount());
    m_rowOfId.clear();
    m_lastSeen.clear();
    filterChanged();
    statusBar()->showMessage("Showing all messages", 4000);
}

QString MainWindow::userDbcPath() const {
    QString veh = m_liveDataDock ? (m_liveDataDock->make() + "_" + m_liveDataDock->model()) : QString();
    veh.replace(QRegularExpression("[^A-Za-z0-9]+"), "_");
    veh = veh.trimmed();
    while (veh.endsWith('_')) veh.chop(1);
    while (veh.startsWith('_')) veh.remove(0, 1);
    if (veh.isEmpty()) veh = "user";
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
                        + "/XnetikCan/dbc";
    QDir().mkpath(dir);
    return dir + "/" + veh + ".dbc";
}

// Build a DBC signal from a live message: pick the bits, watch the value track, save it per-vehicle.
void MainWindow::defineSignalDialog(quint32 id, char src) {
    const quint64 key = srcId(src, id);
    const int dlc = qMax(1, m_latestPayload.value(key).size());
    const QString hexId = QString::number(id, 16).toUpper();

    QDialog dlg(this);
    dlg.setWindowTitle(QString("Define a signal on ID %1").arg(hexId));
    auto *form = new QFormLayout(&dlg);
    auto *name = new QLineEdit;
    name->setPlaceholderText("e.g. Steering angle");
    auto *startBit = new QSpinBox; startBit->setRange(0, 63);
    auto *length = new QSpinBox; length->setRange(1, 64); length->setValue(8);
    auto *order = new QComboBox; order->addItem("Intel (little-endian)", true); order->addItem("Motorola (big-endian)", false);
    auto *sign = new QCheckBox("Signed");
    auto *factor = new QDoubleSpinBox; factor->setDecimals(6); factor->setRange(-1e6, 1e6); factor->setValue(1.0);
    auto *offset = new QDoubleSpinBox; offset->setDecimals(6); offset->setRange(-1e9, 1e9);
    auto *unit = new QLineEdit;
    auto *preview = new QLabel("—");
    preview->setStyleSheet("font-weight:600; color:#0b63c4;");
    form->addRow("Name:", name);
    form->addRow("Start bit:", startBit);
    form->addRow("Length (bits):", length);
    form->addRow("Byte order:", order);
    form->addRow("", sign);
    form->addRow("Scale (×):", factor);
    form->addRow("Offset (+):", offset);
    form->addRow("Unit:", unit);
    form->addRow("Live value:", preview);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    form->addRow(bb);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    // Live preview: decode the newest frame of this ID with the current field settings, ~10 Hz.
    auto buildSig = [&] {
        DbcSignal s;
        s.name = name->text();
        s.startBit = startBit->value();
        s.length = length->value();
        s.littleEndian = order->currentData().toBool();
        s.isSigned = sign->isChecked();
        s.factor = factor->value();
        s.offset = offset->value();
        s.unit = unit->text();
        return s;
    };
    auto *tick = new QTimer(&dlg);
    connect(tick, &QTimer::timeout, &dlg, [&] {
        const QByteArray p = m_latestPayload.value(key);
        double v; qint64 raw;
        if (!p.isEmpty() && buildSig().decode(p, &v, &raw))
            preview->setText(QString("%1 %2    (raw %3, data %4)")
                                 .arg(QString::number(v, 'f', 2), unit->text(), QString::number(raw),
                                      QString::fromLatin1(p.toHex(' ').toUpper())));
        else
            preview->setText(p.isEmpty() ? "waiting for a frame…" : "bits don't fit this frame");
    });
    tick->start(100);

    if (dlg.exec() != QDialog::Accepted) return;
    DbcSignal s = buildSig();
    s.name = s.name.trimmed();
    if (s.name.isEmpty()) { QMessageBox::warning(this, "Define signal", "Give the signal a name."); return; }
    // DBC identifiers are word-characters only; keep the typed text as the comment for display.
    QString ident = s.name;
    ident.replace(QRegularExpression("[^A-Za-z0-9_]+"), "");
    if (ident.isEmpty() || ident.at(0).isDigit()) ident = "Sig" + ident;
    s.comment = s.name;
    s.name = ident;
    saveUserSignal(id, id > 0x7FF, dlc, s);
}

void MainWindow::saveUserSignal(quint32 id, bool extended, int dlc, const DbcSignal &sig) {
    const QString path = userDbcPath();
    // Start from what's already in the file so we append rather than overwrite.
    QList<DbcMessage> msgs;
    DbcFile existing;
    if (QFile::exists(path) && existing.load(path)) msgs = existing.messages();
    int mi = -1;
    for (int i = 0; i < msgs.size(); ++i) if (msgs[i].id == id) { mi = i; break; }
    if (mi < 0) {
        DbcMessage m;
        m.id = id; m.extended = extended; m.dlc = dlc;
        m.name = "ID_" + QString::number(id, 16).toUpper();
        msgs.append(m); mi = msgs.size() - 1;
    }
    if (msgs[mi].dlc < dlc) msgs[mi].dlc = dlc;
    // Replace a signal of the same name, else add.
    bool replaced = false;
    for (DbcSignal &s : msgs[mi].signalList) if (s.name == sig.name) { s = sig; replaced = true; break; }
    if (!replaced) msgs[mi].signalList.append(sig);

    QString err;
    if (!DbcFile::write(path, msgs, &err)) { QMessageBox::critical(this, "Save signal", err); return; }
    if (!m_dbc.reloadFile(path, &err)) { QMessageBox::warning(this, "Save signal", "Saved, but reload failed: " + err); return; }
    QStringList files = QSettings().value("dbc/userFiles").toStringList();
    if (!files.contains(path)) { files << path; QSettings().setValue("dbc/userFiles", files); }
    statusBar()->showMessage(QString("Saved \"%1\" to %2 — now decoding live").arg(sig.comment, QFileInfo(path).fileName()), 6000);
}

void MainWindow::onBusError(const QString &msg) {
    statusBar()->showMessage("Bus error: " + msg, 5000);
}

// The configured vehicle, always visible on the menu bar row, with Auto ID and Change….
void MainWindow::setupVehicleBar() {
    m_vehicleId = new VehicleId(&m_bridge, this);
    m_identify = new IdentifyDialog(m_vehicleId, m_liveDataDock, &m_oe, this);
    auto *bar = new QWidget;
    auto *lay = new QHBoxLayout(bar);
    lay->setContentsMargins(0, 0, 8, 0);
    lay->setSpacing(6);
    m_vehicleLabel = new QLabel;
    m_vehicleLabel->setStyleSheet("QLabel { font-weight:600; margin-right:6px; }");
    auto *autoId = new QToolButton;
    autoId->setText("Auto ID");
    autoId->setToolTip("Read the VIN and set up the vehicle (also runs Check supported)");
    auto *change = new QToolButton;
    change->setText("Change…");
    change->setToolTip("Choose make, model and year by hand");
    auto *recheck = new QToolButton;
    recheck->setText("Re-check");
    recheck->setToolTip("Ask the vehicle again which values it supports (it's remembered per vehicle).\n"
                        "Useful after starting the engine: some values only answer with it running.");
    lay->addWidget(m_vehicleLabel);
    lay->addWidget(autoId);
    lay->addWidget(recheck);
    lay->addWidget(change);
    menuBar()->setCornerWidget(bar, Qt::TopRightCorner);

    auto refresh = [this] {
        const QString v = m_liveDataDock->vehicleText();
        QString text = v.isEmpty() ? QStringLiteral("No vehicle set") : QString("%1 %2 %3").arg(
            m_liveDataDock->year() > 0 ? QString::number(m_liveDataDock->year()) : QString(),
            m_liveDataDock->make(), m_liveDataDock->model()).simplified();
        if (!m_identify->lastVin().isEmpty()) text += "  ·  VIN " + m_identify->lastVin();
        m_vehicleLabel->setText(text);
    };
    refresh();
    connect(m_liveDataDock, &LiveDataDock::vehicleChanged, this, refresh);
    // The Codes window needs the make (module names, safe scan addresses); live data waits while it reads.
    m_codesDialog->setMake(m_liveDataDock->make());
    connect(m_liveDataDock, &LiveDataDock::vehicleChanged, this, [this] { m_codesDialog->setMake(m_liveDataDock->make()); });
    connect(m_codesDialog, &CodesDialog::talking, m_liveDataDock, &LiveDataDock::setOthersTalking);
    // Module names and per-module value lists come from the vehicle library loaded in Live Data.
    m_codesDialog->setModuleSource([this] { return m_liveDataDock->modules(); },
                                   [this](quint32 id, bool *checked) { return m_liveDataDock->moduleValues(id, checked); });
    connect(m_liveDataDock, &LiveDataDock::modulesChanged, m_codesDialog, &CodesDialog::refreshModuleValues);
    connect(m_identify, &IdentifyDialog::vehicleSet, this, refresh);
    connect(autoId, &QToolButton::clicked, this, [this] {
        if (!m_bridge.isConnected()) {
            QMessageBox::information(this, "Auto ID", "Connect to the vehicle first.");
            return;
        }
        m_identify->show();
        m_identify->raise();
        m_identify->startAuto();
    });
    connect(change, &QToolButton::clicked, this, [this] {
        m_identify->show();
        m_identify->raise();
        m_identify->startManual();
    });
    connect(recheck, &QToolButton::clicked, this, [this] {
        if (!m_bridge.isConnected()) {
            QMessageBox::information(this, "Re-check", "Connect to the vehicle first.");
            return;
        }
        m_identify->show();
        m_identify->raise();
        m_identify->startRecheck();
    });
    // Identify automatically on every connect (after the code read has had a moment).
    connect(&m_bridge, &CanBridge::stateChanged, this, [this](bool connected) {
        if (!connected) return;
        QTimer::singleShot(1500, this, [this] {
            if (!m_bridge.isConnected()) return;
            m_identify->show();
            m_identify->raise();
            m_identify->startAuto();
        });
    });
}

void MainWindow::onBusStateChanged(bool connected) {
    m_connectBtn->setEnabled(!connected);
    m_connectBtn->setText(connected ? "⚡  Connected" : "⚡  Connect");
    m_disconnectAct->setEnabled(connected);
    m_statusLabel->setText(connected ? "Connected" : (m_snort && m_snort->isOpen() ? "Snort running" : "Not connected"));
    updateCaptureActions();
}

// Bump when the default dock arrangement changes, so an old saved layout
// doesn't override it (restoreState() rejects a different version).
static constexpr int kLayoutVersion = 3;

void MainWindow::saveSettings() {
    QSettings s;
    s.setValue("main/geometry", saveGeometry());
    // In Diagnostics view, keep the normal layout as the saved one; the view is reapplied on start.
    s.setValue("main/state", m_diagViewAct->isChecked() ? m_preDiagState : saveState(kLayoutVersion));
    s.setValue("view/diagnostics", m_diagViewAct->isChecked());
}

void MainWindow::loadSettings() {
    QSettings s;
    if (s.contains("main/geometry")) restoreGeometry(s.value("main/geometry").toByteArray());
    if (s.contains("main/state")) restoreState(s.value("main/state").toByteArray(), kLayoutVersion);
    if (s.value("view/diagnostics", false).toBool()) m_diagViewAct->setChecked(true);
}

void MainWindow::setDiagnosticsView(bool on) {
    if (on) {
        m_preDiagState = saveState(kLayoutVersion);
        centralWidget()->hide();
        m_filterDock->hide();
        m_diagnosePanel->hide();
        m_liveDataDock->setFloating(false);
        m_liveDataDock->show();
        m_liveDataDock->raise();
    } else {
        centralWidget()->show();
        if (!restoreState(m_preDiagState, kLayoutVersion)) restoreState(m_defaultState);
    }
}

void MainWindow::closeEvent(QCloseEvent *e) {
    saveSettings();
    e->accept();
}
