#pragma once
#include <QMainWindow>
#include <QTableView>
#include <QStandardItemModel>
#include <QLabel>
#include <QLineEdit>
#include <QCheckBox>
#include <QComboBox>
#include <QPushButton>
#include <QToolButton>
#include <QTimer>
#include <QQueue>
#include <QHash>
#include "can/CanBridge.h"
#include "can/ReplayEngine.h"
#include "filter/FilterEngine.h"
#include "dbc/DbcLibrary.h"
#include "diag/EnhancedLibrary.h"
#include "diag/OeLibrary.h"
#include "diagnose/DiagnosePanel.h"
#include "obd/LiveDataDock.h"
#include "obd/CodesDialog.h"

class VehicleId;
class IdentifyDialog;
class OverlayWindow;
class GsUsbDevice;

// A saved bidirectional actuator test ("Fuel pump" etc.): the few UDS lines needed to run it,
// captured from a scan tool. The app adds ISO-TP framing and tester-present; it never bypasses
// security access.
struct ActuatorTest {
    QString name;
    quint32 reqId = 0x7E0;        // module request ID (e.g. 7E0)
    QByteArray session;          // enter-session UDS bytes, e.g. 10 03 (empty = none)
    QByteArray on;               // ON command UDS bytes, e.g. 2F F0 10 03
    QByteArray off;              // OFF/stop UDS bytes (empty = just exit session)
    bool tester = true;          // send tester-present while active
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *e) override;
    void changeEvent(QEvent *e) override;   // minimized + overlay open = scope mode

private slots:
    void onConnect();
    void onDisconnect();
    void onStartCapture();
    void onStopCapture();
    void onClearCapture();
    void onReplay();
    void onFrameReceived(const QCanBusFrame &frame);   // Scan (J2534) frames, while capturing
    void onSnort();                                    // toggle the SH-C31G sniffer
    void onSnortFrames(const QList<QCanBusFrame> &frames);  // Snort (C31G) frames, always-on
    void onNotch();                                    // hide every ID shown now, so only new ones appear
    void onClearNotch();                               // show everything again
    void onBusError(const QString &msg);
    void onBusStateChanged(bool connected);
    void onApplyAdvanced();
    void onPresetChosen(int index);
    void onReplayFinished();
    void flushPendingFrames();
    void onFrameContextMenu(const QPoint &pos);

private:
    void setupMenus();
    void setupToolbar();
    void setupCentralView();
    void setupFilterDock();
    void setupDiagnoseDock();
    void setupLiveDataDock();
    void setupStatusBar();
    void setupVehicleBar();   // menu bar: configured vehicle, Auto ID, Change…
    void setupActuatorDock(); // "Actuator tests": saved bidirectional toggle buttons per vehicle
    void loadActuators();
    void saveActuators();
    void rebuildActuatorButtons();
    void addActuatorFromRow(quint32 id, char src);   // right-click a captured command → save as a test
    void runActuator(int index, bool on);            // toggle: send the sequence, hold tester-present
    QString actuatorKey() const;                     // per-vehicle settings key
    static QByteArray isoTpSingle(const QByteArray &uds);   // wrap UDS bytes as one ISO-TP frame
    void updateStatusBar();
    void saveSettings();
    void loadSettings();
    void setDiagnosticsView(bool on);  // Live Data fills the window; message table, filter, Claude hidden

    void setPerMessageView(bool on);
    void refilterTable();             // rebuild the table from the capture buffer
    void filterChanged();             // summary text + refilter
    void rebuildPresetCombo();
    void savePreset();
    void appendLogRow(const QCanBusFrame &f, char src);
    void updateMessageRow(const QCanBusFrame &f, char src);
    void handleIncoming(const QCanBusFrame &f, char src);   // J (Scan) or G (Snort) into the hex table
    void defineSignalDialog(quint32 id, char src);          // build a DBC signal from a live ID
    void saveUserSignal(quint32 id, bool extended, int dlc, const DbcSignal &sig);  // append to the vehicle's .dbc
    QString userDbcPath() const;
    void saveCaptureCsv();                                  // write the capture as SavvyCAN-style CSV
    void openCaptureCsv();                                  // load a CSV to view / replay
    void updateCaptureActions();                            // enable Capture/Replay for Scan or Snort
    bool ensureSnortActive();                               // switch Snort to Active (asks) so it can transmit
    void sendFrameOnce(quint32 id, char src);
    void sendFrameRepeat(quint32 id, char src);
    void sendComposer(quint32 id, char src);                // edit bytes, then send once / repeatedly
    QCanBusFrame frameFor(quint32 id, char src) const;      // rebuild a frame from the latest data seen
    QString messageName(quint32 id, int dlc = -1) const;   // dlc: frame length, so mismatched definitions are skipped
    QString describeFrame(const QCanBusFrame &f) const;     // UDS plain-language for diag IDs, else DBC decode
    QString aiContext();              // snapshot sent with each Claude question
    double relTime(const QCanBusFrame &f);

    CanBridge m_bridge;
    ReplayEngine *m_replay;
    CanFilter m_filter;
    DbcLibrary m_dbc;
    EnhancedLibrary m_enhanced;
    OeLibrary m_oe;

    QTableView *m_frameView;
    QStandardItemModel *m_frameModel;
    QCheckBox *m_perMessageCheck;
    QPushButton *m_notchBtn = nullptr;
    bool m_perMessage = true;
    bool m_notched = false;        // notch active: rows that arrive new are highlighted
    // per-message view keyed by (source,id) so Scan and Snort never merge into one row
    static quint64 srcId(char src, quint32 id) { return (quint64(src == 'G' ? 1 : 0) << 32) | id; }
    QHash<quint64, int> m_rowOfId;            // (source,id) -> row
    QHash<quint64, double> m_lastSeen;        // (source,id) -> last timestamp (s)
    QHash<quint64, QByteArray> m_latestPayload;   // (source,id) -> newest data (for the DBC builder preview)
    QHash<quint64, QByteArray> m_notchSnapshot;   // notch: data per (source,id) when notched; hidden while unchanged
    double m_t0 = -1;                         // first frame time of this capture

    // Filter dock
    QComboBox *m_presetCombo;
    QLabel *m_filterSummary;
    QWidget *m_advancedBox;
    QLineEdit *m_idFromEdit;
    QLineEdit *m_idToEdit;
    QCheckBox *m_excludeRangeCheck;
    QCheckBox *m_maskCheck;
    QLineEdit *m_maskEdit;
    QLineEdit *m_matchEdit;
    QLineEdit *m_payloadEdit;

    // Toolbar actions
    QPushButton *m_connectBtn = nullptr;
    QAction *m_connectAct;
    QAction *m_disconnectAct;
    QAction *m_startCaptureAct;
    QAction *m_stopCaptureAct;
    QAction *m_replayAct;
    QAction *m_diagViewAct;
    QAction *m_overlayAct = nullptr;
    QAction *m_snortAct = nullptr;         // toolbar toggle for the SH-C31G sniffer
    OverlayWindow *m_overlay = nullptr;   // made the first time it is shown
    GsUsbDevice *m_snort = nullptr;       // the raw-CAN sniffer, independent of the Scan bridge
    quint32 m_snortNominal = 0, m_snortData = 0;   // speeds Snort opened with (to reopen in Active)
    bool m_snortListen = true;
    QTimer m_repeatSend;                  // repeating "send this frame" out Snort
    QCanBusFrame m_repeatFrame;

    // Actuator tests (bidirectional toggle buttons)
    QList<ActuatorTest> m_actuators;
    QDockWidget *m_actuatorDock = nullptr;
    QWidget *m_actuatorButtons = nullptr;   // holds one toggle button per test
    QTimer m_testerTimer;                   // sends tester-present while any test is active
    int m_actuatorsOn = 0;
    QByteArray m_preDiagState;        // layout to go back to when leaving Diagnostics view

    // Status bar
    QLabel *m_statusLabel;
    QLabel *m_frameCountLabel;
    QLabel *m_errorCountLabel;

    DiagnosePanel *m_diagnosePanel;
    LiveDataDock *m_liveDataDock = nullptr;
    CodesDialog *m_codesDialog;
    QToolButton *m_codesBtn;      // toolbar indicator: "No codes" / "2 codes · Check engine ON"
    QLabel *m_batteryLabel;       // "12.6 V", coloured by level
    QLabel *m_vehicleLabel;       // menu bar: "2016 Ford Transit Connect · VIN …"
    VehicleId *m_vehicleId;
    IdentifyDialog *m_identify;
    QTimer m_batteryStale;        // greys the battery out when readings stop
    QDockWidget *m_filterDock;
    QMenu *m_viewMenu;
    QByteArray m_defaultState;

    // Batch UI updates to avoid lag at high frame rates. Each frame carries its source (J/G).
    struct Incoming { QCanBusFrame frame; char src; };
    QQueue<Incoming> m_pendingFrames;
    QTimer m_flushTimer;

    static constexpr int kMaxTableRows = 10000;
};
