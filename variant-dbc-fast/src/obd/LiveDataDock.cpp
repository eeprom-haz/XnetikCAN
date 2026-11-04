#include "LiveDataDock.h"
#include "ObdPids.h"
#include "DidScanDialog.h"
#include "VehicleId.h"
#include "../diag/DiagLog.h"
#include "../can/CanBridge.h"
#include "../dbc/DbcLibrary.h"
#include "../diag/DiagPoller.h"
#include "../diag/StreamSession.h"
#include <QCheckBox>
#include "../plot/LivePlot.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QScrollArea>
#include <QHeaderView>
#include <QMessageBox>
#include <QSettings>
#include <QDate>
#include <QMenu>
#include <cmath>

static QString fmtValue(double v) {
    const double a = std::fabs(v);
    return QString::number(v, 'f', a >= 100 ? 0 : (a < 1 && a > 0) ? 3 : 2);   // small: EVAP in inHg
}

static bool isNoise(const QString &signalName) {
    return signalName.contains("Checksum", Qt::CaseInsensitive) ||
           signalName.contains("Counter", Qt::CaseInsensitive);
}

LiveDataDock::LiveDataDock(CanBridge *bridge, DbcLibrary *dbc, EnhancedLibrary *enhanced, OeLibrary *oe,
                           QWidget *parent)
    : QDockWidget("Live Data", parent)
    , m_bridge(bridge)
    , m_dbc(dbc)
    , m_enhanced(enhanced)
    , m_oe(oe)
    , m_poller(new DiagPoller(bridge, this))
    , m_stream(new StreamSession(bridge, this))
{
    setObjectName("LiveDataDock");
    QSettings settings;
    // Ticked values are kept per vehicle (VIN); a new vehicle starts with RPM, coolant, voltage.
    m_vin = settings.value("oe/vin").toString();
    for (const QString &id : settings.value(selectionKey(), QStringList{"obd:0C", "obd:05", "obd:42"})
                                 .toStringList())
        m_checked.insert(id);
    m_overlayOrder = settings.value(overlayKey()).toStringList().mid(0, kMaxOverlay);

    auto *w = new QWidget;
    auto *vlay = new QVBoxLayout(w);
    vlay->setContentsMargins(6, 6, 6, 6);
    vlay->setSpacing(6);

    // The vehicle is set from the menu bar (Auto ID on connect, or Change…); remembered here.
    // The vehicle saved for the last VIN wins over the plain "last vehicle" setting, so the
    // two can never disagree (seen: Ford setting with the INFINITI's VIN).
    m_make = settings.value("oe/make").toString();
    m_model = settings.value("oe/model").toString();
    m_year = settings.value("oe/year", 0).toInt();
    if (!m_vin.isEmpty()) {
        const VehicleId::Saved saved = VehicleId::saved(m_vin);
        if (saved.found) {
            m_make = saved.make;
            m_model = saved.model;
            m_year = saved.year;
        }
    }

    // ── Controls ──
    auto *bar = new QHBoxLayout;
    // Supported values are checked by Auto ID on connect (Re-check is in the vehicle bar, top
    // right). The button stays as the internal state holder but isn't shown.
    m_discoverBtn = new QPushButton("Check supported");
    m_discoverBtn->setVisible(false);
    m_startBtn = new QPushButton("▶  Start polling");
    m_startBtn->setToolTip("Request the ticked OBD-II, manufacturer and enhanced values over and over.\n"
                           "Broadcast values update by themselves, no polling needed.");
    m_scanBtn = new QPushButton("Find enhanced data…");
    m_scanBtn->setToolTip("Scan a module for manufacturer data (Mode 22) and add items to your library");
    auto *windowCombo = new QComboBox;
    for (int s : {2, 10, 20, 30}) windowCombo->addItem(QString("%1 s").arg(s), s);
    windowCombo->setCurrentIndex(1);   // 10 s
    windowCombo->setToolTip("How much history the graph shows");
    auto *autoFit = new QPushButton("Auto-fit");
    autoFit->setCheckable(true);
    autoFit->setToolTip("Off: each graph uses its normal range (e.g. RPM 0–7000).\n"
                        "On: zoom each graph to the data it shows.");
    auto *graphOnly = new QPushButton("Graph only");
    graphOnly->setCheckable(true);
    graphOnly->setToolTip("Hide the parameter list so the graph gets the whole width");
    auto *pauseBtn = new QPushButton("Pause");
    pauseBtn->setCheckable(true);
    auto *clearBtn = new QPushButton("Clear");
    m_rateLabel = new QLabel;
    m_rateLabel->setStyleSheet("color: #666;");
    bar->addWidget(m_discoverBtn);
    bar->addWidget(m_startBtn);
    m_fastBox = new QCheckBox("Fast mode");
    m_fastBox->setChecked(QSettings().value("live/fastMode", true).toBool());
    m_fastBox->setToolTip("Ask the engine computer to stream the values it can (much faster updates).\n"
                          "Values or vehicles that don't support it are polled as usual.");
    bar->addWidget(m_fastBox);
    bar->addWidget(m_scanBtn);
    bar->addSpacing(12);
    bar->addWidget(new QLabel("Graph:"));
    bar->addWidget(windowCombo);
    bar->addWidget(autoFit);
    bar->addWidget(graphOnly);
    bar->addWidget(pauseBtn);
    bar->addWidget(clearBtn);
    bar->addStretch();
    bar->addWidget(m_rateLabel);
    vlay->addLayout(bar);

    // ── Parameter list ──
    auto *left = new QWidget;
    auto *leftLay = new QVBoxLayout(left);
    leftLay->setContentsMargins(0, 0, 0, 0);
    leftLay->setSpacing(4);
    m_search = new QLineEdit;
    m_search->setPlaceholderText("Search parameters (e.g. coolant, rpm, trans temp)…");
    m_search->setClearButtonEnabled(true);
    auto *untickAll = new QPushButton("Untick all");
    untickAll->setToolTip("Start fresh: untick every value (remembered for this vehicle)");
    auto *searchRow = new QHBoxLayout;
    searchRow->addWidget(m_search, 1);
    searchRow->addWidget(untickAll);
    leftLay->addLayout(searchRow);
    connect(untickAll, &QPushButton::clicked, this, [this] {
        m_building = true;
        for (Sig &s : m_sigs)
            if (s.item->flags() & Qt::ItemIsUserCheckable) s.item->setCheckState(ColName, Qt::Unchecked);
        m_building = false;
        m_checked.clear();   // also values not on screen right now (e.g. broadcast not seen yet)
        applySelection();
        m_relocate.start(0);
    });

    m_tree = new QTreeWidget;
    m_tree->setColumnCount(ColCount);
    m_tree->setHeaderLabels({"Parameter", "Value", "Unit", "Min", "Max"});
    m_tree->setRootIsDecorated(true);
    m_tree->setUniformRowHeights(true);
    m_tree->setAlternatingRowColors(true);
    m_tree->setSelectionMode(QAbstractItemView::NoSelection);
    // Deep folders (Manufacturer › Module › System) shouldn't push the tick boxes far right.
    m_tree->setIndentation(12);
    // Every column keeps the width you drag it to, both ways. No column grabs the leftover
    // space (a stretching last column made Max huge and impossible to shrink).
    m_tree->header()->setMinimumSectionSize(36);
    m_tree->header()->setStretchLastSection(false);
    const QList<QVariant> savedWidths = settings.value("live/columnWidths").toList();
    const int widths[ColCount] = {240, 80, 48, 64, 64};
    for (int c = ColName; c < ColCount; ++c) {
        m_tree->header()->setSectionResizeMode(c, QHeaderView::Interactive);
        int w = c < savedWidths.size() ? savedWidths[c].toInt() : 0;
        if (w < 36 || (c != ColName && w > 200)) w = widths[c];   // drop old stretched widths
        m_tree->setColumnWidth(c, w);
    }
    connect(m_tree->header(), &QHeaderView::sectionResized, this, [this] {
        QList<QVariant> w;
        for (int c = ColName; c < ColCount; ++c) w << m_tree->columnWidth(c);
        QSettings().setValue("live/columnWidths", w);
    });
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_tree, &QTreeWidget::customContextMenuRequested, this, &LiveDataDock::onTreeMenu);
    leftLay->addWidget(m_tree, 1);

    // Two parts: what this vehicle answers on top, everything else collapsed below.
    m_workTop = makeGroup(nullptr, "Working on this vehicle");
    m_restTop = makeGroup(nullptr, "Everything else");
    m_relocate.setSingleShot(true);
    m_relocate.setInterval(0);
    connect(&m_relocate, &QTimer::timeout, this, &LiveDataDock::relocateAll);

    m_building = true;
    for (const ObdPid &pid : obdPids()) {
        Sig s;
        s.section = {QStringLiteral("Standard OBD-II"), pid.group};
        s.source = Source::Obd;
        s.persistId = "obd:" + QString("%1").arg(pid.pid, 2, 16, QChar('0')).toUpper();
        if (pid.sub > 0) s.persistId += QString(":%1").arg(pid.sub);
        s.name = pid.name;
        s.unit = pid.unit;
        s.rangeMin = pid.rangeMin;
        s.rangeMax = pid.rangeMax;
        s.pid = pid.pid;
        s.obd = &pid;
        const int key = addSignal(s);
        m_sigs[key].item->setToolTip(ColName, QString("OBD-II Mode 01, PID 0x%1")
                                                  .arg(QString::number(pid.pid, 16).rightJustified(2, '0').toUpper()));
    }
    m_building = false;

    resetBroadcast();

    // ── Graph ──
    m_plot = new LivePlot;
    auto *scroll = new QScrollArea;
    scroll->setWidget(m_plot);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto *split = new QSplitter(Qt::Horizontal);
    split->addWidget(left);
    split->addWidget(scroll);
    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 1);
    split->setSizes({480, 900});
    left->setMinimumWidth(320);
    vlay->addWidget(split, 1);
    setWidget(w);

    rebuildEnhanced();

    connect(m_startBtn, &QPushButton::clicked, this, &LiveDataDock::onStartStop);
    connect(m_discoverBtn, &QPushButton::clicked, this, &LiveDataDock::onDiscover);
    connect(m_scanBtn, &QPushButton::clicked, this, &LiveDataDock::openScanner);
    connect(graphOnly, &QPushButton::toggled, left, [left](bool on) { left->setVisible(!on); });
    connect(pauseBtn, &QPushButton::toggled, m_plot, &LivePlot::setPaused);
    connect(autoFit, &QPushButton::toggled, m_plot, &LivePlot::setAutoFit);
    connect(windowCombo, &QComboBox::currentIndexChanged, this, [this, windowCombo] {
        m_plot->setWindowSeconds(windowCombo->currentData().toInt());
    });
    connect(clearBtn, &QPushButton::clicked, this, [this] {
        m_plot->clearData();
        for (Sig &s : m_sigs) {
            s.hasValue = false;
            s.dirty = true;
        }
        refreshValues();
    });
    connect(m_search, &QLineEdit::textChanged, this, &LiveDataDock::applySearch);
    connect(m_tree, &QTreeWidget::itemChanged, this, &LiveDataDock::onItemChanged);
    connect(m_poller, &DiagPoller::sample, this, &LiveDataDock::onSample);
    connect(m_stream, &StreamSession::data, this, &LiveDataDock::onStreamData);
    // The engine computer takes one request at a time: polling waits while a stream is set up.
    connect(m_stream, &StreamSession::talking, this, [this](bool busy) {
        m_streamTalking = busy;
        // While the PCM is in the streaming session, standard values go to it as 22 F4xx; when no
        // stream runs (or the ECU can't stream, e.g. the INFINITI refuses 22 F4xx) the normal way.
        m_poller->setStandardAsUds(m_stream->active());
        m_poller->setPaused(m_streamTalking || m_othersTalking);
    });
    // Queued: re-splitting streamed vs polled must not happen inside the session's own call.
    connect(m_stream, &StreamSession::changed, this, &LiveDataDock::applySelection, Qt::QueuedConnection);
    connect(m_fastBox, &QCheckBox::toggled, this, [this](bool on) {
        QSettings().setValue("live/fastMode", on);
        if (on) m_stream->allowRetry();
        applySelection();
    });
    connect(m_poller, &DiagPoller::itemFailed, this, &LiveDataDock::onItemFailed);
    connect(m_poller, &DiagPoller::pidsDiscovered, this, &LiveDataDock::onDiscovered);
    connect(m_poller, &DiagPoller::probeResult, this, &LiveDataDock::onProbeResult);
    connect(m_poller, &DiagPoller::probeFinished, this, &LiveDataDock::onProbeFinished);
    connect(m_poller, &DiagPoller::discoveryFailed, this, [this](const QString &msg) {
        m_discoverBtn->setEnabled(m_bridge->isConnected());
        m_discoverBtn->setText("Check supported");
        m_rateLabel->setText("Check supported: " + msg);
        emit supportDone(0, 0, msg);
    });
    connect(m_bridge, &CanBridge::stateChanged, this, &LiveDataDock::onBusStateChanged);
    connect(m_bridge, &CanBridge::liveFrame, this, &LiveDataDock::onLiveFrame);
    connect(m_enhanced, &EnhancedLibrary::changed, this, &LiveDataDock::rebuildEnhanced);
    connect(m_dbc, &DbcLibrary::changed, this, [this] {
        const bool wasBuilding = m_building;   // may arrive from inside loadVehicle()
        m_building = true;
        resetBroadcast();
        m_building = wasBuilding;
        if (!wasBuilding) {
            applySearch();
            applySelection();
        }
    });
    // Once new message IDs stop appearing for a moment, check which definition files fit this bus.
    m_trafficMatch.setSingleShot(true);
    m_trafficMatch.setInterval(2000);
    connect(&m_trafficMatch, &QTimer::timeout, this, [this] { m_dbc->matchTraffic(m_busLengths); });
    connect(&m_refresh, &QTimer::timeout, this, &LiveDataDock::refreshValues);
    m_refresh.start(100);
    connect(Units::notifier(), &Units::Notifier::changed, this, &LiveDataDock::applyUnits);
    // Freeze check (requests log): the app not responding, and how fast the overlay gets values.
    m_stallClock.start();
    connect(&m_stallTick, &QTimer::timeout, this, [this] {
        const qint64 now = m_stallClock.elapsed();
        const qint64 gap = now - m_lastTick;
        if (m_lastTick > 0 && gap > 500)
            DiagLog::write("requests", QString("app did not respond for %1 ms (polling %2%3%4)").arg(gap)
                                           .arg(m_poller->isRunning() ? "on" : "off")
                                           .arg(m_poller->isProbing() ? ", checking vehicle" : "")
                                           .arg(m_scope ? ", scope mode" : ""));
        m_lastTick = now;
        if (m_overlayShown && now - m_lastOverlayLog >= 10000) {
            DiagLog::write("requests", QString("overlay (last 10 s): %1 values for %2 shown%3")
                                           .arg(m_overlaySamples).arg(overlayIds().size())
                                           .arg(m_scope ? ", scope mode" : ""));
            m_overlaySamples = 0;
            m_lastOverlayLog = now;
        }
    });
    m_stallTick.start(100);

    loadVehicle();   // the last vehicle

    m_workTop->setExpanded(true);
    applySelection();
    onBusStateChanged(m_bridge->isConnected());
}

QTreeWidgetItem *LiveDataDock::makeGroup(QTreeWidgetItem *parent, const QString &title, const QString &hint) {
    auto *g = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(m_tree);
    g->setText(ColName, hint.isEmpty() ? title : QString("%1   ·   %2").arg(title, hint));
    g->setData(ColName, Qt::UserRole + 2, title);
    g->setFlags(Qt::ItemIsEnabled);
    QFont f = g->font(ColName);
    f.setBold(true);
    g->setFont(ColName, f);
    if (!hint.isEmpty()) g->setToolTip(ColName, hint);
    // Group title spans the whole row so it never widens the value columns.
    g->setFirstColumnSpanned(true);
    return g;
}

int LiveDataDock::addSignal(Sig s) {
    const int key = m_nextKey++;
    auto *item = new QTreeWidgetItem;
    item->setText(ColName, s.name);
    item->setText(ColValue, "—");
    s.conv = Units::forUnit(s.unit, s.name);
    item->setText(ColUnit, s.conv.unit);
    for (int c = ColValue; c < ColCount; ++c) item->setTextAlignment(c, Qt::AlignRight | Qt::AlignVCenter);
    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
    item->setCheckState(ColName, m_checked.contains(s.persistId) ? Qt::Checked : Qt::Unchecked);
    item->setData(ColName, Qt::UserRole, key);
    s.item = item;
    m_keyOf.insert(s.persistId, key);
    m_sigs.insert(key, s);
    place(m_sigs[key]);
    return key;
}

bool LiveDataDock::isWorking(const Sig &s) const {
    if (s.item->data(ColName, kNotReadableRole).toBool()) return false;
    if (s.item->checkState(ColName) == Qt::Checked) return true;   // what you graph stays on top
    if (s.item->data(ColName, kUnsupportedRole).toBool()) return false;
    switch (s.source) {
    case Source::Obd: return m_stdChecked;       // supported by this vehicle (checked)
    case Source::Oe: return m_probeChecked;      // answered when this vehicle was checked
    case Source::Broadcast:                      // only listed once seen on this bus
    case Source::Enhanced: return true;          // your own items
    }
    return false;
}

QTreeWidgetItem *LiveDataDock::groupFor(bool working, const QStringList &section) {
    QTreeWidgetItem *parent = working ? m_workTop : m_restTop;
    QString key = working ? QStringLiteral("W") : QStringLiteral("R");
    for (int i = 0; i < section.size(); ++i) {
        key += "|" + section[i];
        QTreeWidgetItem *&g = m_groups[key];
        if (!g) {
            g = makeGroup(parent, section[i]);
            if (i > 0) {
                QFont f = g->font(ColName);
                f.setBold(false);
                g->setFont(ColName, f);
            }
            g->setExpanded(working);
        }
        parent = g;
    }
    return parent;
}

void LiveDataDock::place(Sig &s) {
    QTreeWidgetItem *target = groupFor(isWorking(s), s.section);
    QTreeWidgetItem *current = s.item->parent();
    if (current == target) return;
    if (current) current->removeChild(s.item);
    target->addChild(s.item);
}

void LiveDataDock::pruneGroups() {
    bool removed = true;
    while (removed) {   // deepest first: a parent empties once its last sub-group goes
        removed = false;
        for (auto it = m_groups.begin(); it != m_groups.end();) {
            if (it.value()->childCount() == 0) {
                delete it.value();
                it = m_groups.erase(it);
                removed = true;
            } else {
                ++it;
            }
        }
    }
}

void LiveDataDock::relocateAll() {
    const bool wasBuilding = m_building;
    m_building = true;
    // Open / closed groups stay as you left them.
    QHash<QString, bool> wasOpen;
    for (auto it = m_groups.cbegin(); it != m_groups.cend(); ++it) wasOpen.insert(it.key(), it.value()->isExpanded());
    // Fixed order of the sections: standard values, the modules, broadcast, your own items.
    for (Source src : {Source::Obd, Source::Oe, Source::Broadcast, Source::Enhanced})
        for (Sig &s : m_sigs)
            if (s.source == src) place(s);
    pruneGroups();
    for (QTreeWidgetItem *top : {m_workTop, m_restTop}) {
        QList<QTreeWidgetItem *> sections = top->takeChildren();
        auto rank = [](QTreeWidgetItem *g) {
            const QString t = g->data(ColName, Qt::UserRole + 2).toString();
            if (t == "Standard OBD-II") return 0;
            if (t == "Broadcast data") return 2;
            if (t == "My enhanced items") return 3;
            return 1;
        };
        std::stable_sort(sections.begin(), sections.end(), [&](QTreeWidgetItem *a, QTreeWidgetItem *b) {
            return rank(a) < rank(b);
        });
        top->addChildren(sections);
    }
    for (auto it = m_groups.cbegin(); it != m_groups.cend(); ++it)
        it.value()->setExpanded(wasOpen.value(it.key(), it.key().startsWith('W')));
    m_building = wasBuilding;
    updateHeaders();
    applySearch();
}

void LiveDataDock::updateHeaders() {
    int working = 0, notChecked = 0, notReadable = 0, refused = 0;
    for (const Sig &s : std::as_const(m_sigs)) {
        if (s.item->data(ColName, kNotReadableRole).toBool()) ++notReadable;
        else if (s.item->data(ColName, kUnsupportedRole).toBool()) ++refused;
        else if (isWorking(s)) ++working;
        else ++notChecked;
    }
    const bool checked = m_stdChecked || m_probeChecked;
    m_workTop->setText(ColName, checked ? QString("Working on this vehicle   ·   %1 values").arg(working)
                                        : QString("Working on this vehicle   ·   not checked yet (Auto ID checks on connect)"));
    QString rest = m_make.isEmpty() ? QStringLiteral("Everything else   ·   no vehicle set (Auto ID or Change…, top right)")
                                    : QString("Everything else for %1").arg(m_vehicleLabel);
    QStringList bits;
    if (notChecked) bits << QString("%1 not checked").arg(notChecked);
    if (notReadable) bits << QString("%1 can't be read with this adapter").arg(notReadable);
    if (!bits.isEmpty()) rest += "   ·   " + bits.join(", ");
    m_restTop->setText(ColName, rest);
    m_restTop->setToolTip(ColName, QString("%1 values this vehicle didn't answer are hidden (Re-check, top right, "
                                           "checks again — e.g. with the engine running).").arg(refused));
}

void LiveDataDock::removeSignals(Source source) {
    for (auto it = m_sigs.begin(); it != m_sigs.end();) {
        if (it->source == source) {
            if (m_plot) m_plot->removeSeries(it.key());
            m_keyOf.remove(it->persistId);
            delete it->item;
            it = m_sigs.erase(it);
        } else {
            ++it;
        }
    }
}

void LiveDataDock::rebuildEnhanced() {
    m_building = true;
    removeSignals(Source::Enhanced);
    for (const EnhancedDid &d : m_enhanced->dids()) {
        Sig s;
        s.section = {QStringLiteral("My enhanced items")};
        s.source = Source::Enhanced;
        s.persistId = "did:" + QString("%1:%2").arg(d.requestId, 0, 16).arg(d.did, 4, 16, QChar('0')).toUpper();
        s.name = d.name;
        s.unit = d.unit;
        s.rangeMin = d.rangeMin;
        s.rangeMax = d.rangeMax;
        s.did = d;
        const int key = addSignal(s);
        m_sigs[key].item->setToolTip(ColName, QString("%1 · Mode 22 DID 0x%2 · request 0x%3")
                                                  .arg(d.module)
                                                  .arg(QString::number(d.did, 16).rightJustified(4, '0').toUpper())
                                                  .arg(QString::number(d.requestId, 16).toUpper()));
    }
    m_building = false;
    if (m_workTop) m_relocate.start(0);
    if (m_plot) applySelection();
}

void LiveDataDock::setOthersTalking(bool busy) {
    m_othersTalking = busy;
    m_poller->setPaused(m_streamTalking || m_othersTalking);
}

void LiveDataDock::setVehicle(const QString &make, const QString &model, int year, const QString &vin) {
    const bool newVin = !vin.isEmpty() && vin != m_vin;
    if (make == m_make && model == m_model && year == m_year && !newVin) return;
    m_make = make;
    m_model = model;
    m_year = year;
    if (newVin) {
        m_vin = vin;
        QSettings().setValue("oe/vin", vin);
        loadSelection();   // this vehicle's own ticks, not the last car's
    }
    loadVehicle();
}

QString LiveDataDock::vehicleKey() const {
    return m_vin.isEmpty() ? QString("%1|%2|%3").arg(m_make, m_model).arg(m_year) : m_vin;
}

QString LiveDataDock::selectionKey() const {
    return m_vin.isEmpty() ? QStringLiteral("live/checked") : "live/checkedByVin/" + m_vin;
}

QString LiveDataDock::overlayKey() const {
    return m_vin.isEmpty() ? QStringLiteral("overlay/items") : "overlay/byVin/" + m_vin;
}

void LiveDataDock::loadSelection() {
    QSettings s;
    m_overlayOrder = s.value(overlayKey()).toStringList().mid(0, kMaxOverlay);   // this vehicle's overlay
    const QStringList ids = s.contains(selectionKey()) ? s.value(selectionKey()).toStringList()
                                                       : QStringList{"obd:0C", "obd:05", "obd:42"};
    m_checked = QSet<QString>(ids.cbegin(), ids.cend());
    const bool wasBuilding = m_building;
    m_building = true;
    for (Sig &sig : m_sigs) {
        if (!(sig.item->flags() & Qt::ItemIsUserCheckable)) continue;
        sig.item->setCheckState(ColName, m_checked.contains(sig.persistId) ? Qt::Checked : Qt::Unchecked);
    }
    m_building = wasBuilding;
}

void LiveDataDock::loadVehicle() {
    const bool wasRunning = m_poller->isRunning();
    if (m_poller->isProbing()) {   // the sweep belongs to the previous vehicle's command list
        m_poller->stop();
        m_discoverBtn->setText("Check supported");
        m_discoverBtn->setEnabled(m_bridge->isConnected());
        m_startBtn->setEnabled(m_bridge->isConnected());
    }
    m_probeCommands.clear();
    m_building = true;
    removeSignals(Source::Oe);
    m_oeCommands.clear();
    m_modules.clear();
    m_moduleSessions.clear();
    m_probeChecked = false;
    // Standard support belongs to the vehicle too: start unknown, then apply what is remembered.
    m_stdChecked = false;
    for (Sig &sig : m_sigs)
        if (sig.source == Source::Obd) sig.item->setData(ColName, kUnsupportedRole, false);

    const QString make = m_make;
    const QString model = m_model;
    const int year = m_year;
    QSettings s;
    s.setValue("oe/make", make);
    s.setValue("oe/model", model);
    s.setValue("oe/year", year);
    if (make != m_dbcMake) {       // broadcast names only from this make's definition files
        m_dbcMake = make;
        m_busLengths.clear();
        m_dbc->clearTrafficMatch();
        m_dbc->setVehicleMake(make);
    }

    if (make.isEmpty()) {
        m_vehicleLabel.clear();
        m_building = false;
        relocateAll();
        applySelection();
        emit vehicleChanged();
        emit modulesChanged();
        return;
    }

    m_oeCommands = m_oe->load(make, model);
    m_modules = OeLibrary::modules(m_oeCommands, m_oe->moduleTypes(make), make);
    QHash<QString, QString> moduleTitle;   // module address -> heading
    for (const OeModule &m : std::as_const(m_modules)) {
        OeCommand probe;
        probe.requestId = m.requestId;
        probe.ext29 = m.ext29;
        probe.txEa = m.txEa;
        moduleTitle.insert(OeLibrary::moduleId(probe), m.title());
    }
    // The session each module's values need (the library's "din"), used when one of its other
    // values answers "wrong session": the most common one per module.
    QHash<QString, QHash<quint8, int>> sessionCount;
    for (const OeCommand &c : std::as_const(m_oeCommands))
        if (c.sessionIn) ++sessionCount[OeLibrary::moduleId(c)][c.sessionIn];
    for (auto it = sessionCount.cbegin(); it != sessionCount.cend(); ++it) {
        quint8 best = 0;
        int most = 0;
        for (auto s = it->cbegin(); s != it->cend(); ++s)
            if (s.value() > most) { most = s.value(); best = s.key(); }
        m_moduleSessions.insert(it.key(), best);
    }
    // Module -> system (path) -> signals. One row per value: the same module, request and name
    // from the model file and the make file is listed once (the model's own wins, it comes
    // first), and standard values read the UDS way stay in the Standard list only.
    QSet<QString> listed;
    for (int ci = 0; ci < m_oeCommands.size(); ++ci) {
        const OeCommand &c = m_oeCommands[ci];
        if (!c.matchesYear(year) || OeLibrary::isStandardValue(c)) continue;
        const QString module = moduleTitle.value(OeLibrary::moduleId(c), OeLibrary::moduleId(c));
        for (const OeSignal &sig : c.signalList) {
            const QString vk = OeLibrary::valueKey(c, sig);
            if (listed.contains(vk)) continue;
            listed.insert(vk);
            const QString path = sig.path.isEmpty() ? QStringLiteral("General") : QString(sig.path).replace('.', " › ");
            Sig s;
            s.section = {module, path};
            s.source = Source::Oe;
            s.persistId = QString("oe:%1:%2:%3").arg(OeLibrary::moduleId(c), QString(c.request().toHex()), sig.id).toUpper();
            s.name = sig.name;
            s.unit = sig.displayUnit();
            // Use the definition's range for the graph only when it is a sensible display range.
            if (sig.max > sig.min && sig.max - sig.min <= 100000 && sig.unit != "hex") {
                s.rangeMin = sig.min;
                s.rangeMax = sig.max;
            }
            s.oeCommand = ci;
            s.oe = sig;
            const bool readable = c.supported;
            const int key = addSignal(s);
            QTreeWidgetItem *item = m_sigs[key].item;
            QString tip = QString("%1 · request %2 → reply %3 · %4")
                              .arg(module, OeLibrary::idText(c.requestId, c.ext29),
                                   OeLibrary::idText(c.responseId, c.ext29),
                                   QString(c.request().toHex(' ').toUpper()));
            if (c.txEa >= 0)
                tip += QString(" · address byte %1").arg(QString::number(c.txEa, 16).rightJustified(2, '0').toUpper());
            if (c.sessionIn)
                tip += QString("\nAsked in diagnostic session 10 %1 (opened automatically)")
                           .arg(QString::number(c.sessionIn, 16).rightJustified(2, '0').toUpper());
            if (!sig.description.isEmpty()) tip += "\n" + sig.description;
            if (!c.borrowedFrom.isEmpty())
                tip += "\nDefinition from " + c.borrowedFrom + " data (same make family, not this model's "
                       "own file). Kept only if this vehicle answers it.";
            if (!readable) {   // e.g. K-line: this adapter can't ask it — shown greyed, never hidden
                item->setFlags(Qt::ItemIsEnabled);
                item->setData(ColName, Qt::CheckStateRole, QVariant());
                item->setData(ColName, kNotReadableRole, true);
                item->setForeground(ColName, QBrush(QColor("#9a9a9a")));
                item->setText(ColValue, QString());
                tip += "\nCan't be read with this adapter: " + c.unsupportedWhy;
            }
            item->setToolTip(ColName, tip);
        }
    }
    m_vehicleLabel = make + " " + (model.isEmpty() ? QStringLiteral("(all models)") : model);
    // What this vehicle answered last time is remembered: the list is right straight away.
    if (s.contains(standardCacheKey())) {
        QSet<quint8> std;
        for (const QString &p : s.value(standardCacheKey()).toStringList()) std.insert(quint8(p.toUInt(nullptr, 16)));
        applyStandardSupport(std);
    }
    m_probeSilent = s.value(probeCacheKey()).toStringList();
    m_probeChecked = s.contains(probeCacheKey());
    for (int ci = 0; ci < m_oeCommands.size(); ++ci)
        if (m_probeSilent.contains(commandId(m_oeCommands[ci])))
            markOeCommand(ci, false, "Did not answer when this vehicle was checked (Re-check, top right, to check again)");
    updateSourceLabels();
    m_building = false;
    relocateAll();
    applySelection();
    if (wasRunning && !m_poller->isRunning() && m_bridge->isConnected()) m_poller->start();
    emit vehicleChanged();
    emit modulesChanged();
    emit overlayChanged();   // overlay values of this vehicle: names, available or not
}

QStringList LiveDataDock::moduleValues(quint32 requestId, bool *checked) const {
    if (checked) *checked = m_probeChecked;
    QStringList out;
    for (const Sig &s : m_sigs) {
        if (s.source != Source::Oe || s.oeCommand < 0) continue;
        const OeCommand &c = m_oeCommands[s.oeCommand];
        if (c.requestId != requestId || c.ext29 || c.txEa >= 0 || !c.supported || !c.matchesYear(m_year)) continue;
        if (s.item->data(ColName, kUnsupportedRole).toBool()) continue;
        const QString line = s.conv.unit.isEmpty() ? s.name : QString("%1 (%2)").arg(s.name, s.conv.unit);
        if (!out.contains(line)) out << line;
    }
    out.sort(Qt::CaseInsensitive);
    return out;
}

void LiveDataDock::resetBroadcast() {
    removeSignals(Source::Broadcast);
    m_seenBroadcast.clear();
    m_bcKeys.clear();
    if (m_workTop) m_relocate.start(0);
}

void LiveDataDock::addBroadcastMessage(const DbcMessage &m) {
    const QString file = m_dbc->fileOf(m.id, m.dlc);
    m_building = true;
    for (const DbcSignal &ds : m.signalList) {
        if (isNoise(ds.name)) continue;
        Sig s;
        s.section = {QStringLiteral("Broadcast data")};
        s.dbcFile = file;
        s.source = Source::Broadcast;
        s.persistId = QString("bc:%1:%2").arg(m.id).arg(ds.name);
        s.name = DbcLibrary::humanize(ds.name);
        s.unit = ds.unit;
        // DBC [min|max] is often 0|0 or the full encoding range; only trust sane ones.
        if (ds.max > ds.min && ds.values.isEmpty()) {
            s.rangeMin = ds.min;
            s.rangeMax = ds.max;
        }
        s.dbc = ds;
        const int key = addSignal(s);
        QString tip = QString("Broadcast · %1 (ID 0x%2) · from %3").arg(DbcLibrary::humanize(m.name))
                          .arg(QString::number(m.id, 16).rightJustified(3, '0').toUpper(), file);
        if (!ds.comment.isEmpty()) tip += "\n" + ds.comment;
        m_sigs[key].item->setToolTip(ColName, tip);
        m_bcKeys[m.id].append(key);
    }
    m_building = false;
    for (bool w : {true, false})
        if (QTreeWidgetItem *g = m_groups.value(QString(w ? "W" : "R") + "|Broadcast data"))
            g->sortChildren(ColName, Qt::AscendingOrder);
    updateHeaders();
    applySearch();
    applySelection();
}

// A broadcast file that matched the bus by message IDs can still be the wrong car's file. When it
// decodes RPM, speed or coolant, compare with the vehicle's own standard values (when those are
// being read): if it keeps disagreeing, the file is switched off for this connection.
void LiveDataDock::checkBroadcast(const Sig &bc, double value) {
    if (bc.dbcFile.isEmpty()) return;
    const QString n = bc.name.toLower();
    const QString unit = bc.unit.toLower();
    // Only plain "the engine's RPM" names: a target / idle / limit value may differ legitimately.
    for (const char *other : {"target", "desired", "idle", "max", "min", "limit", "request", "set"})
        if (n.contains(QLatin1String(other))) return;
    quint8 pid = 0;
    if (n == "rpm" || n == "engine rpm" || n == "engine speed" || n == "eng rpm") pid = 0x0C;
    else if (n.contains("coolant") || n == "clt" || n == "engine temp") pid = 0x05;
    else if ((n == "speed" || n.contains("vehicle speed") || n.startsWith("speed")) &&
             !n.contains("fan") && !n.contains("wiper")) pid = 0x0D;
    else return;
    const double now = m_poller->elapsed();
    const Sig *ref = nullptr;
    for (const Sig &s : std::as_const(m_sigs))
        if (s.source == Source::Obd && s.pid == pid && s.obd && s.obd->sub == 0 && s.hasValue && now - s.lastT < 1.0) {
            ref = &s;
            break;
        }
    if (!ref) return;   // the standard value isn't being read right now: nothing to compare with
    double v = value;
    if (pid == 0x0D && (n.contains("mph") || unit == "mph")) v *= 1.609344;
    const double r = ref->value;
    const bool bad = pid == 0x0C ? std::fabs(v - r) > qMax(150.0, 0.15 * r)
                   : pid == 0x05 ? std::fabs(v - r) > 8.0
                                 : std::fabs(v - r) > qMax(5.0, 0.10 * r);
    int &badCount = m_bcBad[bc.dbcFile];
    int &goodCount = m_bcGood[bc.dbcFile];
    (bad ? badCount : goodCount)++;
    if (badCount >= 20 && badCount > 2 * goodCount) {
        const QString file = bc.dbcFile;
        const QString why = QString("%1 decoded %2 while the vehicle reports %3").arg(bc.name).arg(value).arg(r);
        badCount = goodCount = 0;
        // Not from inside the frame handler: switching files rebuilds the broadcast list.
        QTimer::singleShot(0, this, [this, file, why] {
            DiagLog::write("requests", "broadcast file " + file + " switched off: " + why);
            m_dbc->rejectFile(file, why);
        });
    }
}

void LiveDataDock::onLiveFrame(const QCanBusFrame &f) {
    if (f.hasExtendedFrameFormat()) return;
    const quint32 id = f.frameId();
    const int len = int(f.payload().size());
    if (!m_busLengths.contains(id) && (id < 0x7DF || id > 0x7EF)) {   // diagnostic replies aren't broadcast
        m_busLengths.insert(id, len);
        m_trafficMatch.start();
    }
    if (!m_seenBroadcast.contains(id)) {
        m_seenBroadcast.insert(id);
        if (const DbcMessage *m = m_dbc->message(id, len)) addBroadcastMessage(*m);
    }
    auto it = m_bcKeys.constFind(id);
    if (it == m_bcKeys.constEnd()) return;
    const double t = m_poller->elapsed();
    for (int key : *it) {
        Sig &s = m_sigs[key];
        double v;
        qint64 raw;
        if (s.dbc.decode(f.payload(), &v, &raw)) {
            record(s, t, v, s.dbc.values.isEmpty() ? QString() : s.dbc.format(v, raw));
            if (s.dbc.values.isEmpty()) checkBroadcast(s, v);
        }
    }
}

void LiveDataDock::record(Sig &s, double t, double value, const QString &text) {
    if (!s.hasValue) {
        s.min = s.max = value;
        s.hasValue = true;
    }
    s.value = value;
    s.lastT = t;
    s.min = std::min(s.min, value);
    s.max = std::max(s.max, value);
    s.valueText = text;
    s.dirty = true;
    if (s.item->checkState(ColName) == Qt::Checked)
        m_plot->addSample(s.item->data(ColName, Qt::UserRole).toInt(), t, value);
    if (!m_overlayOrder.isEmpty() && m_overlayOrder.contains(s.persistId)) {
        ++m_overlaySamples;
        emit overlaySample(s.persistId, t, s.conv.apply(value), text);
    }
}

void LiveDataDock::setOverlayShown(bool shown) {
    m_overlayShown = shown;
    m_lastOverlayLog = m_stallClock.elapsed();
    m_overlaySamples = 0;
    updateScope();
}

void LiveDataDock::setMainMinimized(bool minimized) {
    m_mainMinimized = minimized;
    updateScope();
}

void LiveDataDock::updateScope() {
    const bool on = m_overlayShown && m_mainMinimized && !overlayIds().isEmpty();
    if (on == m_scope) return;
    m_scope = on;
    DiagLog::write("requests", on ? "scope mode on: only the overlay values are read"
                                  : "scope mode off: all ticked values read again");
    applySelection();
    if (!on)
        for (Sig &s : m_sigs) s.dirty = true;   // the list catches up at once
    emit scopeModeChanged(on);
}

QString LiveDataDock::valueTip(const Sig &s) const {
    QStringList lines;
    if (!s.feedNote.isEmpty()) lines << s.feedNote;
    if (s.conv.changed && s.hasValue && s.valueText.isEmpty())
        lines << QString("As read: %1 %2   (min %3, max %4)")
                     .arg(fmtValue(s.value), s.unit, fmtValue(s.min), fmtValue(s.max));
    return lines.join('\n');
}

void LiveDataDock::applyUnits() {
    for (auto it = m_sigs.begin(); it != m_sigs.end(); ++it) {
        Sig &s = *it;
        s.conv = Units::forUnit(s.unit, s.name);
        s.item->setText(ColUnit, s.conv.unit);
        m_plot->setDisplay(it.key(), s.conv.unit, s.conv.scale, s.conv.offset);
        s.dirty = true;
    }
    refreshValues();
    emit overlayChanged();
}

void LiveDataDock::refreshValues() {
    if (m_scope) return;   // the window is minimized: nothing to show, values catch up afterwards
    for (Sig &s : m_sigs) {
        if (!s.dirty) continue;
        s.dirty = false;
        if (!s.hasValue) {
            s.item->setText(ColValue, "—");
            s.item->setText(ColMin, {});
            s.item->setText(ColMax, {});
            continue;
        }
        const bool number = s.valueText.isEmpty();
        s.item->setText(ColValue, number ? fmtValue(s.conv.apply(s.value)) : s.valueText);
        s.item->setText(ColMin, number ? fmtValue(s.conv.apply(s.min)) : QString());
        s.item->setText(ColMax, number ? fmtValue(s.conv.apply(s.max)) : QString());
        if (s.conv.changed) s.item->setToolTip(ColValue, valueTip(s));   // hover: the reading as read
    }
    if (++m_rateTick >= 10) {  // once a second
        m_rateTick = 0;
        // Which ticked values come by stream right now (blue) and which are polled.
        const QSet<quint16> streamedNow = m_stream->streaming();
        static const QBrush streamBrush(QColor("#1d4ed8"));
        for (Sig &s : m_sigs) {
            if (s.source == Source::Broadcast || s.item->checkState(ColName) != Qt::Checked) continue;
            const quint16 did = streamDid(s);
            const bool fast = did && streamedNow.contains(did);
            s.item->setForeground(ColValue, fast ? streamBrush : QBrush());
            s.feedNote = fast ? QStringLiteral("Streamed by the engine computer (fast mode)") : QStringLiteral("Polled");
            s.item->setToolTip(ColValue, valueTip(s));
        }
        if (m_poller->isRunning()) {
            const int replies = m_poller->replies() + m_streamValues;
            int streamedTicked = 0;
            const QSet<quint16> streamed = m_stream->streaming();
            for (auto k = m_streamKeys.cbegin(); k != m_streamKeys.cend(); ++k)
                if (streamed.contains(k.key())) streamedTicked += int(k.value().size());
            // Every ticked value refused as a stream source (e.g. the INFINITI refuses 22 F4xx):
            // this vehicle doesn't stream — say so instead of "setting up".
            bool noneStreamable = !m_streamKeys.isEmpty();
            for (auto k = m_streamKeys.cbegin(); k != m_streamKeys.cend() && noneStreamable; ++k)
                noneStreamable = m_stream->rejected(k.key());
            QString mode;
            if (!m_fastBox->isChecked()) mode = QStringLiteral(" · polling");
            else if (m_stream->unavailable()) mode = QStringLiteral(" · fast mode not available, polling");
            else if (streamedTicked > 0) mode = QString(" · %1 streamed, rest polled").arg(streamedTicked);
            else if (noneStreamable && !m_stream->active())
                mode = QStringLiteral(" · fast mode not supported on this vehicle, polling");
            else if (!m_streamKeys.isEmpty()) mode = QStringLiteral(" · setting up fast mode…");
            m_rateLabel->setText(QString("%1 values/s%2 · %3 missed").arg(replies - m_lastReplies).arg(mode)
                                     .arg(m_poller->timeouts()));
            m_rateLabel->setToolTip((m_stream->unavailable() ? "Fast mode not available: " + m_stream->unavailableWhy() + "\n"
                                                             : QString()) +
                                    "Blue values are streamed, black ones polled.\nStream log: " +
                                    StreamSession::logPath());
            m_lastReplies = replies;
        }
    }
}

void LiveDataDock::onSample(int key, double t, double value, const QString &text) {
    auto it = m_sigs.find(key);
    if (it != m_sigs.end()) record(*it, t, value, text);
}

void LiveDataDock::onItemFailed(int key, const QString &why) {
    auto it = m_sigs.find(key);
    if (it == m_sigs.end()) return;
    it->item->setText(ColValue, "not supported");
    it->item->setToolTip(ColValue, why);
    it->item->setCheckState(ColName, Qt::Unchecked);  // stops polling it
    it->item->setData(ColName, kUnsupportedRole, true);
    m_relocate.start(0);
}

QString LiveDataDock::commandId(const OeCommand &c) {
    return OeLibrary::commandId(c);
}

// Remembered per vehicle (VIN when read) and per vehicle setting: a different model choice for
// the same VIN is a different list of manufacturer requests. "support2": checks made before
// sessions / 29-bit / extended addressing were supported don't count (they missed values).
QString LiveDataDock::probeCacheKey() const {
    return QString("support2/%1/%2|%3/oeSilent").arg(vehicleKey(), m_make, m_model);
}

QString LiveDataDock::standardCacheKey() const {
    return QString("support2/%1/standard").arg(vehicleKey());
}

DiagPoller::Item LiveDataDock::oeItem(int command) const {
    const OeCommand &c = m_oeCommands[command];
    DiagPoller::Item pi;
    pi.isOe = true;
    pi.tx = c.requestId;
    pi.rx = c.responseId;
    pi.ext29 = c.ext29;
    pi.txEa = c.txEa;
    pi.rxEa = c.rxEa;
    pi.timeoutMs = c.timeoutMs;
    pi.session = c.sessionIn;
    pi.sessionOut = c.sessionOut;
    pi.module = OeLibrary::moduleId(c);
    pi.fallbackSession = m_moduleSessions.value(pi.module, 0);
    pi.request = c.request();
    return pi;
}

QString LiveDataDock::capabilitySummary() const {
    if (m_make.isEmpty()) return QStringLiteral("No vehicle set.");
    int standard = 0, notReadable = 0;
    QMap<QString, int> perModule;
    for (const Sig &s : m_sigs) {
        if (s.item->data(ColName, kNotReadableRole).toBool()) { ++notReadable; continue; }
        if (s.item->data(ColName, kUnsupportedRole).toBool()) continue;
        if (s.source == Source::Obd && m_stdChecked) ++standard;
        if (s.source == Source::Oe && m_probeChecked && !s.section.isEmpty()) ++perModule[s.section.first()];
    }
    QStringList lines;
    lines << QString("Standard OBD-II: %1 values").arg(standard);
    int manufacturer = 0;
    QStringList modules;
    for (auto it = perModule.cbegin(); it != perModule.cend(); ++it) {
        manufacturer += it.value();
        modules << QString("%1 %2").arg(it.key()).arg(it.value());
    }
    lines << (manufacturer ? QString("Manufacturer: %1 values — %2").arg(manufacturer).arg(modules.join(", "))
                           : QStringLiteral("Manufacturer: none answered"));
    QStringList sessions;
    for (auto it = m_moduleSessions.cbegin(); it != m_moduleSessions.cend(); ++it)
        sessions << QString("%1 → 10 %2").arg(it.key(), QString::number(it.value(), 16).rightJustified(2, '0').toUpper());
    if (!sessions.isEmpty()) lines << "Diagnostic sessions opened automatically: " + sessions.join(", ");
    if (notReadable) lines << QString("%1 values can't be read with this adapter (shown greyed)").arg(notReadable);
    return lines.join("\n");
}

void LiveDataDock::updateSourceLabels() {
    // Same name from two different requests, both answering (or not yet checked): number them so
    // the two can be compared instead of one silently showing a wrong number.
    QHash<QString, QList<int>> byName;
    for (auto it = m_sigs.cbegin(); it != m_sigs.cend(); ++it) {
        if (it->source != Source::Oe || it->item->data(ColName, kUnsupportedRole).toBool()) continue;
        byName[it->name.trimmed().toLower()] << it.key();
    }
    const bool wasBuilding = m_building;
    m_building = true;
    for (auto it = m_sigs.begin(); it != m_sigs.end(); ++it)
        if (it->source == Source::Oe) it->item->setText(ColName, it->name);
    for (auto g = byName.cbegin(); g != byName.cend(); ++g) {
        if (g->size() < 2) continue;
        QList<int> keys = *g;
        std::sort(keys.begin(), keys.end());   // listing order: the model's own file first
        for (int i = 0; i < keys.size(); ++i) {
            Sig &s = m_sigs[keys[i]];
            s.item->setText(ColName, QString("%1  (source %2 of %3)").arg(s.name).arg(i + 1).arg(keys.size()));
        }
    }
    m_building = wasBuilding;
}

void LiveDataDock::applyStandardSupport(const QSet<quint8> &supported) {
    const bool wasBuilding = m_building;
    m_building = true;
    // O2 sensor PIDs 14–1B / 24–2B / 34–3B are numbered by the ECU's sensor layout:
    // PID 1D supported (and 13 not) means 4 banks × 2 sensors instead of 2 × 4.
    const bool fourBank = supported.contains(0x1D) && !supported.contains(0x13);
    int count = 0;
    for (Sig &s : m_sigs) {
        if (s.source != Source::Obd) continue;
        const bool ok = supported.contains(s.pid);
        count += ok;
        if (!s.obd->name4Bank.isEmpty()) {
            s.name = fourBank ? s.obd->name4Bank : s.obd->name;
            s.item->setText(ColName, s.name);
        }
        if (!ok) s.item->setCheckState(ColName, Qt::Unchecked);
        s.item->setData(ColName, kUnsupportedRole, !ok);
        s.item->setText(ColValue, ok ? (s.hasValue ? s.item->text(ColValue) : QStringLiteral("—")) : QString());
    }
    m_building = wasBuilding;
    m_stdChecked = true;
    m_lastStandardCount = count;
}

void LiveDataDock::markOeCommand(int command, bool answered, const QString &why) {
    // Every command with the same request shares the answer (each request is asked once).
    if (m_cmdIds.size() != m_oeCommands.size()) {
        m_cmdIds.clear();
        for (const OeCommand &c : std::as_const(m_oeCommands)) m_cmdIds << commandId(c);
    }
    const QString &id = m_cmdIds[command];
    QSet<int> same;
    for (int ci = 0; ci < m_oeCommands.size(); ++ci)
        if (ci == command || m_cmdIds[ci] == id) same.insert(ci);
    for (Sig &s : m_sigs) {
        if (s.source != Source::Oe || !same.contains(s.oeCommand)) continue;
        s.item->setData(ColName, kUnsupportedRole, !answered);
        s.item->setToolTip(ColValue, answered ? QString() : why);
        if (!answered) s.item->setCheckState(ColName, Qt::Unchecked);
    }
}

void LiveDataDock::updateOeHeader() {
    updateHeaders();
}

void LiveDataDock::onProbeResult(int index, bool answered, const QString &why) {
    if (index < 0 || index >= m_probeCommands.size()) return;
    const int ci = m_probeCommands[index];
    m_building = true;
    markOeCommand(ci, answered, why);
    m_building = false;
    const QString id = commandId(m_oeCommands[ci]);
    m_probeSilent.removeAll(id);
    if (!answered) m_probeSilent << id;
    m_rateLabel->setText(QString("Checking manufacturer values… %1 of %2").arg(index + 1)
                             .arg(m_probeCommands.size()));
    emit supportProgress(QString("%1 standard values supported · checking manufacturer values… %2 of %3")
                             .arg(m_lastStandardCount).arg(index + 1).arg(m_probeCommands.size()));
}

void LiveDataDock::onProbeFinished() {
    QSettings().setValue(probeCacheKey(), m_probeSilent);
    m_probeChecked = true;
    m_discoverBtn->setText("Check supported");
    m_discoverBtn->setEnabled(m_bridge->isConnected());
    m_startBtn->setEnabled(m_bridge->isConnected());
    updateSourceLabels();
    relocateAll();
    applySelection();
    DiagLog::write("requests", "check finished for " + m_vehicleLabel + ":\n" + capabilitySummary());
    int answered = 0;
    for (int ci : std::as_const(m_probeCommands))
        answered += !m_probeSilent.contains(commandId(m_oeCommands[ci]));
    m_rateLabel->setText(QString("Done · vehicle answered %1 of %2 manufacturer requests")
                             .arg(answered).arg(m_probeCommands.size()));
    int values = 0;
    for (const Sig &s : std::as_const(m_sigs))
        values += s.source == Source::Oe && !s.item->data(ColName, kUnsupportedRole).toBool();
    emit supportDone(m_lastStandardCount, values, {});
    emit modulesChanged();
    emit overlayChanged();
}

void LiveDataDock::onItemChanged(QTreeWidgetItem *item, int column) {
    if (m_building || column != ColName || !item->data(ColName, Qt::UserRole).isValid()) return;
    applySelection();
    // A ticked value stays where you ticked it (nothing jumps or closes while you pick values);
    // it moves up to "Working on this vehicle" when polling starts or the vehicle loads.
}

// The DID a ticked value can be streamed as from the engine computer: standard values as F4xx
// (Mode 01 read the UDS way), PCM manufacturer values as their own 22 xxxx DID. 0 = can't stream.
quint16 LiveDataDock::streamDid(const Sig &s) const {
    if (s.source == Source::Obd) return quint16(0xF400 | s.pid);
    if (s.source == Source::Oe && s.oeCommand >= 0 && s.oeCommand < m_oeCommands.size()) {
        const OeCommand &c = m_oeCommands[s.oeCommand];
        const QByteArray r = c.request();
        if (c.requestId == 0x7E0 && r.size() == 3 && quint8(r[0]) == 0x22)
            return quint16(quint8(r[1]) << 8 | quint8(r[2]));
    }
    return 0;
}

void LiveDataDock::onStreamData(quint16 did, const QByteArray &bytes) {
    const double t = m_poller->elapsed();
    const quint8 *d = reinterpret_cast<const quint8 *>(bytes.constData());
    for (int key : m_streamKeys.value(did)) {
        auto it = m_sigs.find(key);
        if (it == m_sigs.end()) continue;
        Sig &s = *it;
        double v;
        QString text;
        if (s.source == Source::Obd) {
            if (!s.obd || bytes.size() < s.obd->bytes) continue;
            v = s.obd->decode(d);
            if (std::isnan(v)) continue;
            if (s.obd->text) text = s.obd->text(d);
        } else if (!s.oe.decode(bytes, &v, &text)) {
            continue;
        }
        record(s, t, v, text);
        ++m_streamValues;
    }
}

void LiveDataDock::applySelection() {
    QList<DiagPoller::Item> items;
    QHash<int, int> oeItemOfCommand;  // command index -> position in items
    QHash<quint8, int> obdItemOfPid;  // Mode 01 PID -> position in items
    // Values arriving by stream right now aren't polled; everything else (and anything the
    // engine computer won't stream) is.
    const QSet<quint16> streamed = m_stream->streaming();
    m_streamKeys.clear();
    // Overlay values are read (and asked first) even when not ticked for the graph.
    auto onOverlay = [this](const Sig &s) {
        return !m_overlayOrder.isEmpty() && m_overlayOrder.contains(s.persistId) &&
               !s.item->data(ColName, kUnsupportedRole).toBool() && !s.item->data(ColName, kNotReadableRole).toBool();
    };
    for (auto it = m_sigs.cbegin(); it != m_sigs.cend(); ++it) {
        const Sig &s = it.value();
        const bool wanted = m_scope ? onOverlay(s) : (s.item->checkState(ColName) == Qt::Checked || onOverlay(s));
        if (!wanted) continue;
        if (const quint16 did = streamDid(s)) m_streamKeys[did] << it.key();
    }
    for (auto it = m_sigs.cbegin(); it != m_sigs.cend(); ++it) {
        const Sig &s = it.value();
        const bool on = s.item->checkState(ColName) == Qt::Checked;
        const bool overlay = onOverlay(s);
        if (on) {
            m_checked.insert(s.persistId);
            m_plot->addSeries(it.key(), s.name, s.unit, s.rangeMin, s.rangeMax);
            m_plot->setDisplay(it.key(), s.conv.unit, s.conv.scale, s.conv.offset);
        } else {
            m_checked.remove(s.persistId);
            m_plot->removeSeries(it.key());
            if (!overlay) continue;
        }
        if (m_scope && !overlay) continue;   // scope mode: only the overlay values are asked
        if (s.source == Source::Broadcast) continue;
        if (const quint16 did = streamDid(s); did && streamed.contains(did)) continue;
        if (s.source == Source::Oe) {
            // One request per manufacturer command, decoding every ticked value in its reply.
            auto pos = oeItemOfCommand.find(s.oeCommand);
            if (pos == oeItemOfCommand.end()) {
                pos = oeItemOfCommand.insert(s.oeCommand, items.size());
                items.append(oeItem(s.oeCommand));
            }
            items[*pos].oeSignals.append({it.key(), s.oe});
            items[*pos].priority |= overlay;
            continue;
        }
        if (s.source == Source::Obd) {
            // One request per PID, decoding every ticked value it carries.
            auto pos = obdItemOfPid.find(s.pid);
            if (pos == obdItemOfPid.end()) {
                DiagPoller::Item pi;
                pi.pid = s.pid;
                pos = obdItemOfPid.insert(s.pid, items.size());
                items.append(pi);
            }
            items[*pos].obdValues.append({it.key(), s.obd});
            items[*pos].priority |= overlay;
            continue;
        }
        DiagPoller::Item pi;
        pi.key = it.key();
        pi.isDid = true;
        pi.did = s.did;
        pi.priority = overlay;
        items.append(pi);
    }
    // Keep selections for signals not loaded right now (other vehicle, broadcast not seen yet).
    QSettings().setValue(selectionKey(), QStringList(m_checked.cbegin(), m_checked.cend()));
    // Fast mode: ask the engine computer to stream what it can while polling runs. While a stream
    // is set up or running, standard values still polled go to it directly (22 F4xx): a broadcast
    // OBD request on 7DF makes the Ford PCM drop the streaming session. Without a stream — not
    // started yet, nothing streamable (the INFINITI refuses 22 F4xx), or given up — the normal way.
    const bool fast = m_fastBox->isChecked() && m_poller->isRunning() && !m_stream->unavailable();
    if (fast)
        m_stream->setSources(m_streamKeys.keys());
    else
        m_stream->stop();
    m_poller->setStandardAsUds(m_stream->active());
    m_poller->setItems(items);
}

int LiveDataDock::filterTree(QTreeWidgetItem *node, const QString &text) {
    int visible = 0;
    for (int i = 0; i < node->childCount(); ++i) {
        QTreeWidgetItem *c = node->child(i);
        if (c->data(ColName, Qt::UserRole).isValid()) {  // a parameter
            const bool unsupported = c->data(ColName, kUnsupportedRole).toBool();
            const bool show = !unsupported &&
                              (text.isEmpty() || c->text(ColName).contains(text, Qt::CaseInsensitive));
            c->setHidden(!show);
            visible += show;
        } else if (c->childCount() > 0) {                // a group
            const int n = filterTree(c, text);
            c->setHidden(n == 0);
            if (!text.isEmpty() && n > 0) c->setExpanded(true);
            visible += n;
        } else {                                         // placeholder line
            c->setHidden(!text.isEmpty());
        }
    }
    return visible;
}

void LiveDataDock::applySearch() {
    if (!m_workTop) return;
    const QString t = m_search->text().trimmed();
    for (QTreeWidgetItem *g : {m_workTop, m_restTop}) {
        const int n = filterTree(g, t);
        g->setHidden(n == 0 && (!t.isEmpty() || g == m_restTop));
        if (!t.isEmpty() && n > 0) g->setExpanded(true);
    }
    // Each section heading (Standard OBD-II, Engine (7E0), …) says how many values it shows.
    std::function<int(QTreeWidgetItem *)> usable = [&](QTreeWidgetItem *node) {
        int n = 0;
        for (int i = 0; i < node->childCount(); ++i) {
            QTreeWidgetItem *c = node->child(i);
            if (c->data(ColName, Qt::UserRole).isValid())
                n += !c->data(ColName, kUnsupportedRole).toBool();
            else
                n += usable(c);
        }
        return n;
    };
    for (QTreeWidgetItem *top : {m_workTop, m_restTop})
        for (int i = 0; i < top->childCount(); ++i) {
            QTreeWidgetItem *section = top->child(i);
            const QString title = section->data(ColName, Qt::UserRole + 2).toString();
            if (title.isEmpty()) continue;
            const int n = usable(section);
            section->setText(ColName, QString("%1   ·   %2 value%3").arg(title).arg(n).arg(n == 1 ? "" : "s"));
        }
}

QString LiveDataDock::vehicleText() const {
    if (m_make.isEmpty()) return {};
    QString v = m_make;
    if (!m_model.isEmpty()) v += " " + m_model;
    if (m_year > 0) v += ", " + QString::number(m_year);
    return v;
}

QString LiveDataDock::snapshotText() const {
    static const QHash<int, QString> sourceName = {{int(Source::Obd), "OBD-II"}, {int(Source::Oe), "manufacturer"},
                                                   {int(Source::Broadcast), "broadcast"},
                                                   {int(Source::Enhanced), "my item"}};
    QStringList ticked, others;
    for (const Sig &s : m_sigs) {
        if (!s.hasValue) continue;
        QString line = QString("- %1: %2").arg(s.name, s.valueText.isEmpty() ? fmtValue(s.conv.apply(s.value)) : s.valueText);
        if (!s.conv.unit.isEmpty() && s.valueText.isEmpty()) line += " " + s.conv.unit;
        if (s.valueText.isEmpty() && s.min != s.max)
            line += QString(" (min %1, max %2)").arg(fmtValue(s.conv.apply(s.min)), fmtValue(s.conv.apply(s.max)));
        line += " [" + sourceName.value(int(s.source)) + "]";
        (s.item->checkState(ColName) == Qt::Checked ? ticked : others) << line;
    }
    ticked.sort();
    others.sort();
    const int room = qMax(0, 150 - int(ticked.size()));
    if (others.size() > room) others = others.mid(0, room) << QString("- …and %1 more values").arg(others.size() - room);
    QString out;
    if (!ticked.isEmpty()) out += "Values the technician is watching:\n" + ticked.join('\n') + "\n";
    if (!others.isEmpty()) out += "Other values read this session:\n" + others.join('\n') + "\n";
    return out;
}

void LiveDataDock::onBusStateChanged(bool connected) {
    m_startBtn->setEnabled(connected);
    m_discoverBtn->setEnabled(connected);
    m_scanBtn->setEnabled(connected);
    if (!connected) {
        m_poller->stop();
        m_busLengths.clear();          // next connection may be another vehicle
        m_trafficMatch.stop();
        m_dbc->clearTrafficMatch();
        m_discoverBtn->setText("Check supported");
        m_startBtn->setText("▶  Start polling");
        m_rateLabel->setText("Not connected");
    } else {
        m_rateLabel->setText("Connected");
    }
}

void LiveDataDock::onStartStop() {
    if (m_poller->isRunning()) {
        m_poller->stop();
        m_stream->stop();
        m_startBtn->setText("▶  Start polling");
        m_rateLabel->setText("Polling stopped");
        return;
    }
    m_lastReplies = 0;
    m_streamValues = 0;
    m_stream->allowRetry();   // refused earlier (e.g. engine off)? try fast mode again
    m_poller->start();
    applySelection();   // after start: fast mode only sets up while polling runs
    relocateAll();      // what you ticked moves up to "Working on this vehicle" now
    m_startBtn->setText("■  Stop polling");
}

void LiveDataDock::ensurePolling() {
    if (m_bridge->isConnected() && !m_poller->isRunning() && !m_poller->isProbing()) onStartStop();
}

QStringList LiveDataDock::overlayIds() const {
    QStringList out;   // never a raw id on screen: only values this vehicle's list has
    for (const QString &id : m_overlayOrder)
        if (m_keyOf.contains(id)) out << id;
    return out;
}

double LiveDataDock::now() const {
    return m_poller->elapsed();
}

LiveDataDock::ValueInfo LiveDataDock::valueInfo(const QString &id) const {
    ValueInfo v;
    v.id = id;
    auto k = m_keyOf.constFind(id);
    if (k == m_keyOf.constEnd() || !m_sigs.contains(*k)) return v;
    const Sig &s = m_sigs[*k];
    v.name = s.item->text(ColName);   // with "(source 1 of 2)" when two requests give the same name
    v.unit = s.conv.unit;
    v.available = !s.item->data(ColName, kUnsupportedRole).toBool() &&
                  !s.item->data(ColName, kNotReadableRole).toBool();
    QStringList path;   // section path, without the "Working / Everything else" top
    for (QTreeWidgetItem *p = s.item->parent(); p && p->parent(); p = p->parent())
        path.prepend(p->data(ColName, Qt::UserRole + 2).toString());
    v.where = path.join(" › ");
    return v;
}

QList<LiveDataDock::ValueInfo> LiveDataDock::availableValues() const {
    QList<ValueInfo> out;
    for (auto it = m_keyOf.cbegin(); it != m_keyOf.cend(); ++it) {
        const ValueInfo v = valueInfo(it.key());
        if (v.available) out << v;
    }
    std::sort(out.begin(), out.end(), [](const ValueInfo &a, const ValueInfo &b) {
        return a.where == b.where ? a.name.localeAwareCompare(b.name) < 0 : a.where < b.where;
    });
    return out;
}

void LiveDataDock::setOverlayIds(const QStringList &ids) {
    QStringList clean;
    for (const QString &id : ids)
        if (!clean.contains(id)) clean << id;
    m_overlayOrder = clean.mid(0, kMaxOverlay);
    QSettings().setValue(overlayKey(), m_overlayOrder);
    applySelection();
    updateScope();   // no overlay values left: scope mode ends
    emit overlayChanged();
}

void LiveDataDock::onTreeMenu(const QPoint &pos) {
    QTreeWidgetItem *item = m_tree->itemAt(pos);
    if (!item || !item->data(ColName, Qt::UserRole).isValid()) return;
    auto it = m_sigs.constFind(item->data(ColName, Qt::UserRole).toInt());
    if (it == m_sigs.constEnd()) return;
    const QString id = it->persistId;
    QMenu menu(this);
    if (m_overlayOrder.contains(id)) {
        menu.addAction("Remove from overlay", this, [this, id] {
            QStringList ids = overlayIds();
            ids.removeAll(id);
            setOverlayIds(ids);
        });
    } else {
        const QStringList now = overlayIds();
        QAction *add = menu.addAction(QString("Add to overlay (%1 of %2 used)").arg(now.size()).arg(kMaxOverlay),
                                      this, [this, id, now] { setOverlayIds(now + QStringList{id}); });
        add->setEnabled(now.size() < kMaxOverlay && valueInfo(id).available);
    }
    menu.exec(m_tree->viewport()->mapToGlobal(pos));
}

void LiveDataDock::checkSupported(bool force) {
    QSettings s;
    const bool known = s.contains(standardCacheKey()) && (m_oeCommands.isEmpty() || s.contains(probeCacheKey()));
    if (!force && known) {
        // Checked before: the list was set up from memory when the vehicle loaded. Nothing to ask.
        int values = 0;
        for (const Sig &sig : std::as_const(m_sigs))
            values += sig.source == Source::Oe && !sig.item->data(ColName, kUnsupportedRole).toBool();
        m_rateLabel->setText(QString("Vehicle supports %1 standard and %2 manufacturer values (remembered)")
                                 .arg(m_lastStandardCount).arg(values));
        emit supportDone(m_lastStandardCount, values, {});
        return;
    }
    onDiscover();
}

void LiveDataDock::onDiscover() {
    m_stream->stop();
    m_discoverBtn->setEnabled(false);
    m_startBtn->setText("▶  Start polling");
    m_poller->discoverPids();
}

void LiveDataDock::onDiscovered(const QSet<quint8> &supported) {
    m_discoverBtn->setEnabled(m_bridge->isConnected());
    m_discoverBtn->setText("Check supported");
    QStringList remembered;
    for (quint8 p : supported) remembered << QString::number(p, 16);
    QSettings().setValue(standardCacheKey(), remembered);
    applyStandardSupport(supported);
    const int count = m_lastStandardCount;
    applySearch();
    applySelection();
    m_rateLabel->setText(QString("Vehicle supports %1 of %2 standard values").arg(count).arg(obdPids().size()));
    emit supportProgress(QString("%1 standard values supported · checking manufacturer values…").arg(count));

    // Then ask once for every manufacturer request of the chosen vehicle (each request once,
    // even when several files list it; standard values read the UDS way are already covered).
    m_probeCommands.clear();
    QList<DiagPoller::Item> items;
    QSet<QString> asked;
    const int year = m_year;
    for (int ci = 0; ci < m_oeCommands.size(); ++ci) {
        const OeCommand &c = m_oeCommands[ci];
        if (!c.supported || !c.matchesYear(year) || OeLibrary::isStandardValue(c)) continue;
        if (asked.contains(commandId(c))) continue;
        asked.insert(commandId(c));
        items << oeItem(ci);
        m_probeCommands << ci;
    }
    DiagLog::write("requests", QString("check %1: %2 standard values supported, asking %3 manufacturer requests")
                                   .arg(m_vehicleLabel).arg(count).arg(items.size()));
    if (items.isEmpty()) {   // no vehicle chosen, or nothing this adapter mode can ask
        QSettings().setValue(probeCacheKey(), QStringList());
        m_probeChecked = true;
        relocateAll();
        emit supportDone(count, 0, {});
        return;
    }
    m_discoverBtn->setText("Checking…");
    m_discoverBtn->setEnabled(false);
    m_startBtn->setEnabled(false);   // one request at a time: polling waits for the check
    m_poller->probe(items);
}

void LiveDataDock::openScanner() {
    const bool wasRunning = m_poller->isRunning();
    m_poller->stop();
    m_stream->stop();
    m_startBtn->setText("▶  Start polling");
    DidScanDialog dlg(m_bridge, m_enhanced, this);
    dlg.exec();
    if (wasRunning && m_bridge->isConnected()) onStartStop();
}
