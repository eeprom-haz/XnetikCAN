#pragma once
#include <QDialog>
#include <QTableWidget>
#include <QPushButton>
#include <QCheckBox>
#include <QLabel>
#include <QColor>
#include "../diag/DtcService.h"
#include "../diag/DtcDatabase.h"
#include "../diag/OeLibrary.h"
#include <functional>

class CanBridge;
class QTabWidget;
class QListWidget;

// Trouble codes (read / clear), emissions readiness, freeze frame, monitor tests (Mode 06) and a
// code scan of every module, in a separate window opened from the toolbar indicator.
// Reads codes and readiness automatically after connecting.
class CodesDialog : public QDialog {
    Q_OBJECT
public:
    explicit CodesDialog(CanBridge *bridge, QWidget *parent = nullptr);

    QString snapshotText() const;  // last results, for the AI context
    void setMake(const QString &make) { m_make = make; }   // module names + which addresses to scan
    // Where module names and value lists come from (the vehicle library via Live Data).
    void setModuleSource(std::function<QList<OeModule>()> modules,
                         std::function<QStringList(quint32, bool *)> values);
    void refreshModuleValues();    // Check supported finished / vehicle changed

signals:
    // Short status for the toolbar indicator, e.g. "2 codes · Check engine ON".
    void summaryChanged(const QString &text, const QColor &color);
    void talking(bool busy);       // reading from the modules: live data should wait

private slots:
    void onRead();
    void onClear();
    void onCodes(const QList<Dtc> &codes, const QList<quint32> &modules);
    void onReadiness(const Readiness &r);
    void onFreezeFrames(const QList<FreezeFrame> &frames);
    void onMonitorTests(const QList<MonitorTest> &tests, bool anyAnswer);
    void onModules(const QList<ModuleCodes> &modules, int tried, int skipped);
    void onBusStateChanged(bool connected);

private:
    void publishSummary();
    void showMonitorTests();
    QList<quint32> obdModules() const;
    QString moduleTitle(quint32 requestId) const;   // plain name, never just hex if anything is known
    void showModuleValues(quint32 requestId);

    std::function<QList<OeModule>()> m_libModules;
    std::function<QStringList(quint32, bool *)> m_libValues;
    QList<ModuleCodes> m_lastScan;
    int m_lastTried = 0, m_lastSkipped = 0;
    QListWidget *m_valueList;
    QLabel *m_valueTitle;

    CanBridge *m_bridge;
    DtcService *m_service;
    DtcDatabase m_db;
    QString m_make;
    bool m_autoPending = false;   // read readiness after the automatic code read
    bool m_autoFreeze = false;    // ...then the freeze frame, if there are codes

    // Latest results, for the indicator and the AI context
    int m_codeCount = -1;         // -1 = not read
    bool m_milKnown = false;
    bool m_milOn = false;
    QList<quint32> m_answered;    // OBD modules that answered the last code read
    QStringList m_codeLines;      // "P0301 Stored, Engine (ECM): Cylinder 1 Misfire Detected"
    QStringList m_modulesChecked;
    QStringList m_readyLines;
    QStringList m_freezeLines;
    QList<FreezeFrame> m_frames;   // last read (shown again when the units change)
    QList<MonitorTest> m_tests;
    QStringList m_moduleLines;

    QPushButton *m_readBtn, *m_clearBtn, *m_readyBtn, *m_freezeBtn, *m_testsBtn, *m_scanBtn;
    QCheckBox *m_autoRead;
    QCheckBox *m_problemsOnly;
    QLabel *m_status;
    QTabWidget *m_tabs;
    QLabel *m_codesHeader;
    QTableWidget *m_codes;
    QLabel *m_mil;
    QTableWidget *m_monitors;
    QLabel *m_readySummary;
    QLabel *m_freezeInfo;
    QTableWidget *m_freeze;
    QLabel *m_testsInfo;
    QTableWidget *m_testsTable;
    QLabel *m_scanInfo;
    QTableWidget *m_moduleTable;
};
