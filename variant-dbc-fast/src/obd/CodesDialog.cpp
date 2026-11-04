#include "CodesDialog.h"
#include "../can/CanBridge.h"
#include "Units.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QSettings>
#include <QTabWidget>
#include <QListWidget>
#include <QTimer>
#include <cmath>

static const QColor kText("#1a1a1a");
static const QColor kMuted("#8a8a8a");
static const QColor kRed("#c42b1c");
static const QColor kGreen("#107c10");
static const QColor kAmber("#b35900");

static QTableWidgetItem *cell(const QString &text, const QColor &fg = kText) {
    auto *it = new QTableWidgetItem(text);
    it->setForeground(fg);
    return it;
}

static QTableWidget *plainTable(const QStringList &headers) {
    auto *t = new QTableWidget(0, headers.size());
    t->setHorizontalHeaderLabels(headers);
    t->verticalHeader()->hide();
    t->verticalHeader()->setDefaultSectionSize(22);
    t->setEditTriggers(QAbstractItemView::NoEditTriggers);
    t->setSelectionBehavior(QAbstractItemView::SelectRows);
    t->setAlternatingRowColors(true);
    t->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    t->horizontalHeader()->setStretchLastSection(true);
    return t;
}

static QLabel *sectionLabel(const QString &text) {
    auto *l = new QLabel(text);
    l->setStyleSheet("font-weight:600; font-size:13px; padding-top:6px;");
    return l;
}

static QLabel *infoLabel(const QString &text = {}) {
    auto *l = new QLabel(text);
    l->setWordWrap(true);
    l->setStyleSheet("color:#555;");
    return l;
}

static QString number(double v) {
    const double a = std::fabs(v);
    const int decimals = a >= 100 ? 0 : a >= 10 ? 1 : a >= 1 ? 2 : 3;
    return QString::number(v, 'f', decimals);
}

static void addRow(QTableWidget *t, const QList<QTableWidgetItem *> &items) {
    const int row = t->rowCount();
    t->insertRow(row);
    for (int c = 0; c < items.size(); ++c) t->setItem(row, c, items[c]);
}

CodesDialog::CodesDialog(CanBridge *bridge, QWidget *parent)
    : QDialog(parent)
    , m_bridge(bridge)
    , m_service(new DtcService(bridge, this))
{
    setWindowTitle("Codes & Readiness");
    resize(900, 640);
    m_db.load();

    auto *v = new QVBoxLayout(this);
    v->setSpacing(6);

    auto *bar = new QHBoxLayout;
    m_readBtn = new QPushButton("Read codes");
    m_readyBtn = new QPushButton("Check readiness");
    m_clearBtn = new QPushButton("Clear codes…");
    m_clearBtn->setStyleSheet("QPushButton { background-color: #c42b1c; } "
                              "QPushButton:hover { background-color: #a52317; } "
                              "QPushButton:disabled { background-color: #c8c8c8; color: #888888; }");
    m_autoRead = new QCheckBox("Read automatically when connecting");
    m_autoRead->setChecked(QSettings().value("codes/autoRead", true).toBool());
    bar->addWidget(m_readBtn);
    bar->addWidget(m_readyBtn);
    bar->addStretch();
    bar->addWidget(m_clearBtn);
    v->addLayout(bar);

    m_status = new QLabel;
    m_status->setStyleSheet("color:#666;");
    v->addWidget(m_status);

    m_tabs = new QTabWidget;
    v->addWidget(m_tabs, 1);

    // --- Engine codes & readiness
    auto *codesPage = new QWidget;
    auto *cv = new QVBoxLayout(codesPage);
    m_codesHeader = sectionLabel("Trouble codes");
    cv->addWidget(m_codesHeader);
    m_codes = plainTable({"Code", "Status", "Module", "Description"});
    m_codes->setColumnWidth(0, 70);
    m_codes->setColumnWidth(1, 80);
    m_codes->setColumnWidth(2, 140);
    cv->addWidget(m_codes, 3);
    cv->addWidget(sectionLabel("Emissions readiness"));
    m_mil = new QLabel("Check engine light: not read");
    cv->addWidget(m_mil);
    m_monitors = plainTable({"Monitor", "Status"});
    m_monitors->setSelectionMode(QAbstractItemView::NoSelection);
    m_monitors->setColumnWidth(0, 260);
    cv->addWidget(m_monitors, 2);
    m_readySummary = infoLabel();
    cv->addWidget(m_readySummary);
    m_tabs->addTab(codesPage, "Engine codes && readiness");

    // --- Freeze frame
    auto *freezePage = new QWidget;
    auto *fv = new QVBoxLayout(freezePage);
    auto *fbar = new QHBoxLayout;
    m_freezeBtn = new QPushButton("Read freeze frame");
    fbar->addWidget(m_freezeBtn);
    fbar->addStretch();
    fv->addLayout(fbar);
    m_freezeInfo = infoLabel("What the engine computer recorded at the moment it set a code. "
                             "Read automatically when there are codes.");
    fv->addWidget(m_freezeInfo);
    m_freeze = plainTable({"Value", "Reading", "Unit"});
    m_freeze->setColumnWidth(0, 340);
    m_freeze->setColumnWidth(1, 110);
    fv->addWidget(m_freeze, 1);
    m_tabs->addTab(freezePage, "Freeze frame");

    // --- Monitor tests (Mode 06)
    auto *testsPage = new QWidget;
    auto *tv = new QVBoxLayout(testsPage);
    auto *tbar = new QHBoxLayout;
    m_testsBtn = new QPushButton("Read monitor tests");
    m_problemsOnly = new QCheckBox("Only failed and close to the limit");
    tbar->addWidget(m_testsBtn);
    tbar->addWidget(m_problemsOnly);
    tbar->addStretch();
    tv->addLayout(tbar);
    m_testsInfo = infoLabel("The engine computer's own test results (Mode 06): each test's value against its "
                            "limits. Catches a part that still passes but is close to failing.");
    tv->addWidget(m_testsInfo);
    m_testsTable = plainTable({"Monitor", "Test", "Result", "Value", "Min", "Max"});
    m_testsTable->setColumnWidth(0, 210);
    m_testsTable->setColumnWidth(1, 250);
    m_testsTable->setColumnWidth(2, 110);
    m_testsTable->setColumnWidth(3, 95);
    m_testsTable->setColumnWidth(4, 95);
    tv->addWidget(m_testsTable, 1);
    m_tabs->addTab(testsPage, "Monitor tests (Mode 06)");

    // --- All modules
    auto *modPage = new QWidget;
    auto *mv = new QVBoxLayout(modPage);
    auto *mbar = new QHBoxLayout;
    m_scanBtn = new QPushButton("Scan all modules");
    mbar->addWidget(m_scanBtn);
    mbar->addStretch();
    mv->addLayout(mbar);
    m_scanInfo = infoLabel("Asks every module on this bus (OBD pins 6/14) for its codes. Read only, about "
                           "10 seconds. Modules on medium-speed CAN (pins 3/11) aren't reached yet.");
    mv->addWidget(m_scanInfo);
    m_moduleTable = plainTable({"Module", "Address", "Live data", "Code", "Status", "Description"});
    m_moduleTable->setColumnWidth(0, 230);
    m_moduleTable->setColumnWidth(1, 75);
    m_moduleTable->setColumnWidth(2, 110);
    m_moduleTable->setColumnWidth(3, 85);
    m_moduleTable->setColumnWidth(4, 80);
    mv->addWidget(m_moduleTable, 3);
    m_valueTitle = sectionLabel("Click a module to see the live data it has");
    mv->addWidget(m_valueTitle);
    m_valueList = new QListWidget;
    m_valueList->setAlternatingRowColors(true);
    m_valueList->setSelectionMode(QAbstractItemView::NoSelection);
    mv->addWidget(m_valueList, 2);
    m_tabs->addTab(modPage, "All modules");
    connect(m_moduleTable, &QTableWidget::currentCellChanged, this, [this](int row) {
        if (row < 0 || !m_moduleTable->item(row, 0)) return;
        showModuleValues(m_moduleTable->item(row, 0)->data(Qt::UserRole).toUInt());
    });

    auto *bottom = new QHBoxLayout;
    bottom->addWidget(m_autoRead);
    bottom->addStretch();
    auto *close = new QPushButton("Close");
    bottom->addWidget(close);
    v->addLayout(bottom);

    connect(close, &QPushButton::clicked, this, &QDialog::hide);
    connect(m_readBtn, &QPushButton::clicked, this, &CodesDialog::onRead);
    connect(m_readyBtn, &QPushButton::clicked, m_service, &DtcService::readReadiness);
    connect(m_clearBtn, &QPushButton::clicked, this, &CodesDialog::onClear);
    connect(m_freezeBtn, &QPushButton::clicked, this, [this] { m_service->readFreezeFrames(obdModules()); });
    connect(m_testsBtn, &QPushButton::clicked, this, [this] { m_service->readMonitorTests(obdModules()); });
    connect(m_scanBtn, &QPushButton::clicked, this, [this] {
        m_moduleTable->setRowCount(0);
        m_valueList->clear();
        QList<QPair<quint32, quint32>> known;   // documented addresses for this make family
        if (m_libModules)   // the scan asks 11-bit addresses (29-bit / address-byte modules: Live Data)
            for (const OeModule &m : m_libModules())
                if (!m.ext29 && m.txEa < 0) known.append({m.requestId, m.responseId});
        m_service->scanModules(m_make, known);
    });
    connect(m_problemsOnly, &QCheckBox::toggled, this, &CodesDialog::showMonitorTests);
    connect(m_autoRead, &QCheckBox::toggled, this, [](bool on) { QSettings().setValue("codes/autoRead", on); });
    connect(m_service, &DtcService::codesRead, this, &CodesDialog::onCodes);
    connect(m_service, &DtcService::readinessRead, this, &CodesDialog::onReadiness);
    connect(m_service, &DtcService::freezeFramesRead, this, &CodesDialog::onFreezeFrames);
    connect(Units::notifier(), &Units::Notifier::changed, this, [this] {
        if (!m_frames.isEmpty()) onFreezeFrames(QList<FreezeFrame>(m_frames));
        if (!m_tests.isEmpty()) showMonitorTests();
    });
    connect(m_service, &DtcService::monitorTestsRead, this, &CodesDialog::onMonitorTests);
    connect(m_service, &DtcService::modulesScanned, this, &CodesDialog::onModules);
    connect(m_service, &DtcService::progress, m_status, &QLabel::setText);
    connect(m_service, &DtcService::talking, this, [this](bool busy) {
        for (QPushButton *b : {m_readBtn, m_clearBtn, m_readyBtn, m_freezeBtn, m_testsBtn, m_scanBtn})
            b->setEnabled(!busy && m_bridge->isConnected());
        emit talking(busy);
    });
    connect(m_service, &DtcService::cleared, this, [this](bool ok, const QString &msg) {
        m_status->setText(msg);
        if (ok) {
            m_autoPending = true;  // re-read codes, then readiness (now reset)
            QTimer::singleShot(0, this, &CodesDialog::onRead);
        } else {
            QMessageBox::warning(this, "Clear codes", msg);
        }
    });
    connect(m_bridge, &CanBridge::stateChanged, this, &CodesDialog::onBusStateChanged);
    onBusStateChanged(m_bridge->isConnected());
}

QList<quint32> CodesDialog::obdModules() const {
    return m_answered.isEmpty() ? QList<quint32>{0x7E0} : m_answered;
}

void CodesDialog::publishSummary() {
    if (!m_bridge->isConnected()) {
        emit summaryChanged("Codes", kMuted);
        return;
    }
    if (m_codeCount < 0) {
        emit summaryChanged("Codes: not read", kMuted);
        return;
    }
    QString text = m_codeCount == 0 ? QStringLiteral("No codes")
                                    : QString("%1 code%2").arg(m_codeCount).arg(m_codeCount == 1 ? "" : "s");
    if (m_milKnown && m_milOn) text += "  ·  Check engine ON";
    emit summaryChanged(text, (m_codeCount > 0 || m_milOn) ? kRed : kGreen);
}

QString CodesDialog::snapshotText() const {
    QString out;
    if (m_codeCount < 0) {
        out += "Trouble codes: not read yet\n";
    } else {
        out += QString("Trouble codes (modules checked: %1): %2\n")
                   .arg(m_modulesChecked.join(", "), m_codeCount == 0 ? QStringLiteral("none") : QString());
        for (const QString &l : m_codeLines) out += "- " + l + "\n";
    }
    if (m_readyLines.isEmpty()) {
        out += "Emissions readiness: not read yet\n";
    } else {
        out += "Emissions readiness:\n";
        for (const QString &l : m_readyLines) out += "- " + l + "\n";
    }
    if (!m_freezeLines.isEmpty()) {
        out += "Freeze frame:\n";
        for (const QString &l : m_freezeLines) out += "- " + l + "\n";
    }
    if (!m_tests.isEmpty()) {
        int bad = 0;
        out += "Monitor tests (Mode 06), failed or close to the limit:\n";
        for (const MonitorTest &t : m_tests) {
            if (t.status != MonitorTest::Fail && t.status != MonitorTest::Close) continue;
            ++bad;
            out += QString("- %1, %2: %3 %4 (min %5, max %6) %7\n")
                       .arg(t.monitor, t.test, number(t.value), t.unit, number(t.min), number(t.max),
                            t.status == MonitorTest::Fail ? "FAILED" : "close to limit");
        }
        if (bad == 0) out += QString("- none (%1 results)\n").arg(m_tests.size());
    }
    if (!m_moduleLines.isEmpty()) {
        out += "All-module scan:\n";
        for (const QString &l : m_moduleLines) out += "- " + l + "\n";
    }
    return out;
}

void CodesDialog::onBusStateChanged(bool connected) {
    for (QPushButton *b : {m_readBtn, m_clearBtn, m_readyBtn, m_freezeBtn, m_testsBtn, m_scanBtn})
        b->setEnabled(connected);
    m_codeCount = -1;
    m_milKnown = false;
    m_answered.clear();
    m_codeLines.clear();
    m_modulesChecked.clear();
    m_readyLines.clear();
    m_freezeLines.clear();
    m_tests.clear();
    m_moduleLines.clear();
    if (!connected) {
        m_status->setText("Not connected");
    } else if (m_autoRead->isChecked()) {
        // Give the adapter a moment after connecting, then read codes and readiness.
        m_autoPending = true;
        QTimer::singleShot(400, this, &CodesDialog::onRead);
    }
    publishSummary();
}

void CodesDialog::onRead() {
    m_service->readCodes();
}

void CodesDialog::onCodes(const QList<Dtc> &codes, const QList<quint32> &modules) {
    m_codes->setRowCount(0);
    if (modules.isEmpty()) {
        m_codesHeader->setText("Trouble codes — no module answered (is the key on?)");
        m_status->clear();
        m_autoPending = false;
        m_codeCount = -1;
        publishSummary();
        return;
    }
    m_answered = modules;
    QStringList names;
    for (quint32 m : modules) names << DtcService::moduleName(m, m_make);
    m_codesHeader->setText(codes.isEmpty() ? QString("Trouble codes — none found")
                                           : QString("Trouble codes — %1 found").arg(codes.size()));
    m_status->setText("Checked: " + names.join(", "));

    m_modulesChecked = names;
    m_codeLines.clear();
    for (const Dtc &d : codes) {
        QString text = m_db.describe(d.code);
        if (text.isEmpty())
            text = DtcDatabase::system(d.code) +
                   (DtcDatabase::manufacturerSpecific(d.code) ? " (manufacturer-specific code)" : "");
        m_codeLines << QString("%1 %2, %3: %4").arg(d.code, d.kind, DtcService::moduleName(d.module, m_make), text);
        addRow(m_codes, {cell(d.code),
                         cell(d.kind, d.kind == "Stored" ? kRed : d.kind == "Pending" ? kAmber : kText),
                         cell(DtcService::moduleName(d.module, m_make)), cell(text)});
    }
    m_codeCount = codes.size();
    publishSummary();

    if (m_autoPending) {
        m_autoPending = false;
        m_autoFreeze = !codes.isEmpty();
        QTimer::singleShot(0, m_service, &DtcService::readReadiness);
    }
}

void CodesDialog::onClear() {
    const auto answer = QMessageBox::warning(
        this, "Clear trouble codes",
        "Clear trouble codes on all modules?\n\n"
        "This also erases freeze-frame data and resets every readiness monitor to "
        "\"not complete\". The vehicle will need a full drive cycle before it can pass "
        "an emissions test.\n\n"
        "Key ON, engine OFF.",
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer == QMessageBox::Yes) m_service->clearCodes();
}

void CodesDialog::onReadiness(const Readiness &r) {
    if (m_autoFreeze) {
        m_autoFreeze = false;
        QTimer::singleShot(0, this, [this] { m_service->readFreezeFrames(obdModules()); });
    }
    m_monitors->setRowCount(0);
    if (!r.valid) {
        m_mil->setText("Check engine light: no answer from the engine computer");
        m_readySummary->clear();
        return;
    }
    m_milKnown = true;
    m_milOn = r.milOn;
    m_readyLines.clear();
    m_readyLines << QString("Check engine light: %1").arg(r.milOn ? "ON" : "OFF");
    for (const ReadinessMonitor &m : r.monitors)
        if (m.supported) m_readyLines << QString("%1: %2").arg(m.name, m.complete ? "complete" : "NOT complete");
    if (r.distanceSinceClearKm >= 0)
        m_readyLines << QString("Distance since codes cleared: %1 km").arg(r.distanceSinceClearKm);
    if (r.warmUpsSinceClear >= 0) m_readyLines << QString("Warm-ups since cleared: %1").arg(r.warmUpsSinceClear);
    if (r.distanceMilOnKm >= 0) m_readyLines << QString("Distance with light on: %1 km").arg(r.distanceMilOnKm);
    m_mil->setText(QString("Check engine light: <b style='color:%1'>%2</b>")
                       .arg(r.milOn ? kRed.name() : kGreen.name(), r.milOn ? "ON" : "OFF"));

    int incomplete = 0, used = 0;
    for (const ReadinessMonitor &m : r.monitors) {
        if (!m.supported) {
            addRow(m_monitors, {cell(m.name, kMuted), cell("Not used on this vehicle", kMuted)});
            continue;
        }
        ++used;
        if (!m.complete) ++incomplete;
        addRow(m_monitors, {cell(m.name), m.complete ? cell("Complete", kGreen) : cell("Not complete", kAmber)});
    }

    QStringList lines;
    lines << (incomplete == 0
                  ? QString("All %1 monitors used on this vehicle are complete.").arg(used)
                  : QString("%1 of %2 monitors not complete. Many emissions programs allow one incomplete "
                            "monitor (1996–2000 vehicles: two); check local rules.").arg(incomplete).arg(used));
    QStringList since;
    if (r.distanceSinceClearKm >= 0)
        since << QString("%1 mi").arg(qRound(r.distanceSinceClearKm * 0.621371));
    if (r.warmUpsSinceClear >= 0) since << QString("%1 warm-ups").arg(r.warmUpsSinceClear);
    if (r.minutesSinceClear >= 0)
        since << QString("%1 h %2 min run time").arg(r.minutesSinceClear / 60).arg(r.minutesSinceClear % 60);
    if (!since.isEmpty()) lines << "Since codes were last cleared: " + since.join(", ") + ".";
    if (r.distanceMilOnKm > 0)
        lines << QString("Driven %1 mi with the check engine light on.").arg(qRound(r.distanceMilOnKm * 0.621371));
    m_readySummary->setText(lines.join("\n"));
    m_status->setText("Readiness read");
    publishSummary();
}

void CodesDialog::onFreezeFrames(const QList<FreezeFrame> &frames) {
    m_frames = frames;
    m_freeze->setRowCount(0);
    m_freezeLines.clear();
    if (frames.isEmpty()) {
        m_freezeInfo->setText("No freeze frame stored. A module keeps one when it sets a code that turns on "
                              "the check engine light; clearing codes erases it.");
        m_tabs->setTabText(1, "Freeze frame");
        return;
    }
    m_freezeInfo->setText("What the module recorded at the moment it set the code.");
    for (const FreezeFrame &f : frames) {
        QString text = m_db.describe(f.dtc);
        const QString title = QString("%1 — %2%3").arg(f.dtc, DtcService::moduleName(f.module, m_make),
                                                       text.isEmpty() ? QString() : "  ·  " + text);
        auto *head = cell(title, kRed);
        QFont bold = head->font();
        bold.setBold(true);
        head->setFont(bold);
        const int row = m_freeze->rowCount();
        m_freeze->insertRow(row);
        m_freeze->setItem(row, 0, head);
        m_freeze->setSpan(row, 0, 1, 3);
        m_freezeLines << QString("Set %1 (%2)").arg(f.dtc, DtcService::moduleName(f.module, m_make));
        for (const FreezeValue &fv : f.values) {
            const Units::Conv conv = fv.numeric ? Units::forUnit(fv.unit, fv.name) : Units::Conv{fv.unit};
            const QString value = conv.changed ? number(conv.apply(fv.number)) : fv.value;
            auto *valueCell = cell(value);
            if (conv.changed) valueCell->setToolTip(QString("As read: %1 %2").arg(fv.value, fv.unit));
            addRow(m_freeze, {cell(fv.name), valueCell, cell(conv.unit, kMuted)});
            m_freezeLines << QString("%1: %2 %3").arg(fv.name, value, conv.unit).trimmed();
        }
        if (f.values.isEmpty()) addRow(m_freeze, {cell("The module didn't give any values for this frame.", kMuted)});
    }
    m_tabs->setTabText(1, QString("Freeze frame (%1)").arg(frames.first().dtc));
}

void CodesDialog::onMonitorTests(const QList<MonitorTest> &tests, bool anyAnswer) {
    m_tests = tests;
    if (!anyAnswer) {
        m_testsTable->setRowCount(0);
        m_testsInfo->setText("The engine computer didn't answer Mode 06. Older vehicles and some diesels "
                             "don't support it over CAN.");
        m_tabs->setTabText(2, "Monitor tests (Mode 06)");
        return;
    }
    showMonitorTests();
}

void CodesDialog::showMonitorTests() {
    m_testsTable->setRowCount(0);
    int fail = 0, close = 0, pass = 0, notRun = 0;
    for (const MonitorTest &t : m_tests) {
        switch (t.status) {
        case MonitorTest::Fail: ++fail; break;
        case MonitorTest::Close: ++close; break;
        case MonitorTest::Pass: ++pass; break;
        case MonitorTest::NotRun: ++notRun; break;
        }
    }
    // Problems first, then the rest in monitor order.
    QList<MonitorTest> order;
    for (int s : {MonitorTest::Fail, MonitorTest::Close, MonitorTest::Pass, MonitorTest::NotRun})
        for (const MonitorTest &t : m_tests)
            if (t.status == s) order << t;
    for (const MonitorTest &t : order) {
        if (m_problemsOnly->isChecked() && t.status != MonitorTest::Fail && t.status != MonitorTest::Close)
            continue;
        QString result;
        QColor color;
        switch (t.status) {
        case MonitorTest::Fail: result = "FAILED"; color = kRed; break;
        case MonitorTest::Close: result = "Close to limit"; color = kAmber; break;
        case MonitorTest::Pass: result = "Pass"; color = kGreen; break;
        case MonitorTest::NotRun: result = "Not run yet"; color = kMuted; break;
        }
        const QColor fg = t.status == MonitorTest::NotRun ? kMuted : kText;
        // Shown in the chosen units (EVAP tests in inHg); pass / fail was decided on the raw numbers.
        const Units::Conv conv = Units::forUnit(t.unit, t.monitor + " " + t.test);
        const QString unit = t.unitsKnown ? (conv.unit.isEmpty() ? QString() : " " + conv.unit)
                                          : QString(" (units unknown)");
        auto *name = cell(t.monitor, fg);
        auto hex = [](quint8 b) { return QString::number(b, 16).rightJustified(2, '0').toUpper(); };
        name->setToolTip(QString("Monitor ID %1, test ID %2, units ID %3 (Mode 06)")
                             .arg(hex(t.mid), hex(t.tid), hex(t.uas)));
        if (t.status == MonitorTest::NotRun) {
            addRow(m_testsTable, {name, cell(t.test, fg), cell(result, color), cell("—", kMuted),
                                  cell("—", kMuted), cell("—", kMuted)});
            continue;
        }
        auto *valueCell = cell(number(conv.apply(t.value)) + unit, fg);
        if (conv.changed)
            valueCell->setToolTip(QString("As read: %1 %4 (limits %2 – %3 %4)")
                                      .arg(number(t.value), number(t.min), number(t.max), t.unit));
        addRow(m_testsTable, {name, cell(t.test, fg), cell(result, color), valueCell,
                              cell(number(conv.apply(t.min)) + unit, kMuted),
                              cell(number(conv.apply(t.max)) + unit, kMuted)});
    }
    m_testsInfo->setText(QString("%1 failed, %2 close to the limit, %3 passed, %4 not run yet "
                                 "(not run = the test hasn't finished since codes were cleared or this drive). "
                                 "Hover a monitor for its Mode 06 IDs.")
                             .arg(fail).arg(close).arg(pass).arg(notRun));
    m_tabs->setTabText(2, fail + close > 0 ? QString("Monitor tests (%1 to check)").arg(fail + close)
                                           : QString("Monitor tests (Mode 06)"));
}

void CodesDialog::setModuleSource(std::function<QList<OeModule>()> modules,
                                  std::function<QStringList(quint32, bool *)> values) {
    m_libModules = std::move(modules);
    m_libValues = std::move(values);
}

void CodesDialog::refreshModuleValues() {
    if (!m_lastScan.isEmpty()) onModules(m_lastScan, m_lastTried, m_lastSkipped);
}

QString CodesDialog::moduleTitle(quint32 id) const {
    if (m_libModules)
        for (const OeModule &m : m_libModules())
            if (m.requestId == id) {
                if (!m.name.isEmpty()) return m.name;
                QStringList t;
                for (const QString &s : m.topics.mid(0, 3)) t << s.toLower();
                if (!t.isEmpty()) return "Unknown module · has " + t.join(", ") + " data";
            }
    const QString name = DtcService::moduleName(id, m_make);
    return name.startsWith("Module ") ? QStringLiteral("Unknown module") : name;
}

void CodesDialog::showModuleValues(quint32 id) {
    m_valueList->clear();
    bool checked = false;
    const QStringList values = m_libValues ? m_libValues(id, &checked) : QStringList();
    const QString title = moduleTitle(id) + " (" + QString::number(id, 16).toUpper() + ")";
    if (values.isEmpty()) {
        m_valueTitle->setText(title + " — no live data documented for it yet");
        return;
    }
    m_valueTitle->setText(checked ? QString("%1 — %2 live values it answered (they're in Live Data under this module)")
                                        .arg(title).arg(values.size())
                                  : QString("%1 — %2 documented live values (press Check supported in Live Data to "
                                            "see which this vehicle answers)").arg(title).arg(values.size()));
    m_valueList->addItems(values);
}

void CodesDialog::onModules(const QList<ModuleCodes> &modules, int tried, int skipped) {
    m_lastScan = modules;
    m_lastTried = tried;
    m_lastSkipped = skipped;
    m_moduleTable->setRowCount(0);
    m_moduleLines.clear();
    int withCodes = 0, codes = 0;
    for (const ModuleCodes &m : modules) {
        const QString name = moduleTitle(m.requestId);
        const QString addr = QString::number(m.requestId, 16).toUpper() + "/" + QString::number(m.responseId, 16).toUpper();
        bool checked = false;
        const int nValues = m_libValues ? int(m_libValues(m.requestId, &checked).size()) : 0;
        const QString valuesText = nValues == 0 ? QStringLiteral("none documented")
                                 : checked ? QString("%1 values").arg(nValues)
                                           : QString("%1 documented").arg(nValues);
        auto nameCell = [&] {
            auto *c = cell(name);
            c->setData(Qt::UserRole, m.requestId);
            return c;
        };
        auto valuesCell = [&] { return cell(valuesText, nValues ? kText : kMuted); };
        if (!m.reportsCodes) {
            addRow(m_moduleTable, {nameCell(), cell(addr, kMuted), valuesCell(), cell("—", kMuted), cell("", kMuted),
                                   cell(QString("Answered, but doesn't report codes this way (%1)").arg(m.refusal),
                                        kMuted)});
            m_moduleLines << QString("%1 (%2): answered, codes not available (%3)").arg(name, addr, m.refusal);
            continue;
        }
        if (m.codes.isEmpty()) {
            addRow(m_moduleTable, {nameCell(), cell(addr, kMuted), valuesCell(), cell("—", kMuted),
                                   cell("No codes", kGreen), cell("")});
            m_moduleLines << QString("%1 (%2): no codes").arg(name, addr);
            continue;
        }
        ++withCodes;
        for (const ModuleDtc &d : m.codes) {
            ++codes;
            QString text = m_db.describe(d.code);
            if (text.isEmpty()) text = DtcDatabase::system(d.code);
            text += " — " + DtcService::failureTypeText(d.failureType);
            const QString status = DtcService::statusText(d.status);
            const QString code = d.code + "-" + QString::number(d.failureType, 16).rightJustified(2, '0').toUpper();
            auto *codeCell = cell(code);
            codeCell->setToolTip("Status byte " + QString::number(d.status, 16).rightJustified(2, '0').toUpper());
            addRow(m_moduleTable, {nameCell(), cell(addr, kMuted), valuesCell(), codeCell,
                                   cell(status, status == "Pending" ? kAmber : kRed), cell(text)});
            m_moduleLines << QString("%1 (%2): %3 %4, %5").arg(name, addr, code, status, text);
        }
    }
    QString info = modules.isEmpty()
        ? QString("No module answered on this bus. Is the key on?")
        : QString("%1 module%2 answered, %3 with codes (%4 code%5). %6 addresses asked.")
              .arg(modules.size()).arg(modules.size() == 1 ? "" : "s").arg(withCodes)
              .arg(codes).arg(codes == 1 ? "" : "s").arg(tried);
    if (skipped > 0)
        info += QString(" %1 address%2 left out because a module sends its normal messages there.")
                    .arg(skipped).arg(skipped == 1 ? "" : "es");
    info += " Modules on medium-speed CAN (pins 3/11) aren't reached yet.";
    m_scanInfo->setText(info);
    m_status->setText("Module scan done");
    m_tabs->setTabText(3, withCodes > 0 ? QString("All modules (%1 with codes)").arg(withCodes)
                                        : QString("All modules (%1)").arg(modules.size()));
}
