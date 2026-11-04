#pragma once
#include <QDockWidget>
#include <QTreeWidget>
#include <QPushButton>
#include <QComboBox>
#include <QLineEdit>
#include <QLabel>
#include <QTimer>
#include <QElapsedTimer>
#include <QHash>
#include <QSet>
#include <QCanBusFrame>
#include "../dbc/DbcFile.h"
#include "../diag/EnhancedLibrary.h"
#include "../diag/OeLibrary.h"
#include "../diag/DiagPoller.h"
#include "Units.h"

class CanBridge;
class StreamSession;
class QCheckBox;
class LivePlot;
class DbcLibrary;
struct ObdPid;

// Live data for a tech: standard OBD-II PIDs, manufacturer data (OBDb) for the
// chosen vehicle, broadcast signals decoded from DBC files, and the user's own
// enhanced items — one list, one graph.
class LiveDataDock : public QDockWidget {
    Q_OBJECT
public:
    LiveDataDock(CanBridge *bridge, DbcLibrary *dbc, EnhancedLibrary *enhanced, OeLibrary *oe,
                 QWidget *parent = nullptr);

    QString vehicleText() const;   // "Chevrolet Silverado 1500, 2016" or ""
    QString snapshotText() const;  // current values (ticked first) for the AI context

    // The vehicle (from Auto ID or set by hand in the menu bar). The VIN keys what is remembered
    // for this vehicle: which values it supports and which ones you ticked.
    void setVehicle(const QString &make, const QString &model, int year, const QString &vin = {});
    QString make() const { return m_make; }
    QString model() const { return m_model; }
    int year() const { return m_year; }
    // Standard values, then manufacturer values. Remembered per vehicle: without force, a vehicle
    // checked before is set up from memory at once (no waiting, no bus traffic).
    void checkSupported(bool force = false);
    // Another part of the app (the Codes window) is talking to the modules: polling waits.
    void setOthersTalking(bool busy);
    // Modules the library has data for (this make family), and one module's value names:
    // after Check supported only what it answered (*checked = true), else everything documented.
    QList<OeModule> modules() const { return m_modules; }
    QStringList moduleValues(quint32 requestId, bool *checked = nullptr) const;

    void ensurePolling();          // start polling if connected and not running
    // ── Overlay: up to 10 values the tech picked, polled first, sent to the overlay window ──
    static constexpr int kMaxOverlay = 10;
    struct ValueInfo {
        QString id;                // persist id
        QString name, unit;        // unit as shown (imperial / metric)
        bool available = false;    // this vehicle has it (not refused, readable)
        QString where;             // "Standard OBD-II › Engine"
    };
    QList<ValueInfo> availableValues() const;   // everything that can be picked right now
    ValueInfo valueInfo(const QString &id) const;
    QStringList overlayIds() const;   // this vehicle's overlay values that are in the list right now
    void setOverlayIds(const QStringList &ids);
    double now() const;            // the clock overlay samples are stamped with
    // Scope mode: overlay open and the main window minimized (over PicoScope) — only the overlay
    // values are polled and the list stops refreshing; everything resumes when the window is back.
    void setOverlayShown(bool shown);
    void setMainMinimized(bool minimized);
    bool scopeMode() const { return m_scope; }

signals:
    void scopeModeChanged(bool on);
    // Value in the units shown (imperial / metric), for the overlay.
    void overlaySample(const QString &id, double t, double value, const QString &text);
    void overlayChanged();         // list, names or units changed
    void vehicleChanged();
    void modulesChanged();   // modules or their supported values changed
    void supportProgress(const QString &text);              // during Check supported
    void supportDone(int standardValues, int manufacturerValues, const QString &problem);

private slots:
    void onBusStateChanged(bool connected);
    void onStartStop();
    void onDiscover();
    void onDiscovered(const QSet<quint8> &supported);
    void onProbeResult(int index, bool answered, const QString &why);
    void onProbeFinished();
    void onSample(int key, double t, double value, const QString &text);
    void onItemFailed(int key, const QString &why);
    void onItemChanged(QTreeWidgetItem *item, int column);
    void onLiveFrame(const QCanBusFrame &f);
    void refreshValues();
    void applySearch();
    void rebuildEnhanced();
    void loadVehicle();
    void openScanner();
    void onStreamData(quint16 did, const QByteArray &bytes);
    void onTreeMenu(const QPoint &pos);
    void applyUnits();               // View → Units changed

private:
    enum Col { ColName, ColValue, ColUnit, ColMin, ColMax, ColCount };
    enum class Source { Obd, Broadcast, Enhanced, Oe };
    static constexpr int kUnsupportedRole = Qt::UserRole + 3;  // the vehicle refused / doesn't have it: hidden
    static constexpr int kNotReadableRole = Qt::UserRole + 4;  // this adapter can't ask it: shown greyed

    struct Sig {
        Source source;
        QString persistId;     // "obd:0C", "obd:34:1", "bc:201:EngineRPM", "did:7E0:1940", "oe:7E0:221940:ID"
        QString name;
        QString unit;          // as read (metric for standard / library values)
        Units::Conv conv;      // how it is shown
        double rangeMin = 0, rangeMax = 0;
        quint8 pid = 0;
        const ObdPid *obd = nullptr;   // standard OBD-II value definition
        DbcSignal dbc;
        EnhancedDid did;
        int oeCommand = -1;    // index into m_oeCommands
        OeSignal oe;
        QTreeWidgetItem *item = nullptr;
        QStringList section;   // where it sits: {"Standard OBD-II", "Engine"}, {"Engine (7E0)", "Engine"}, …
        QString dbcFile;       // broadcast: the definition file it comes from
        // live state
        bool hasValue = false;
        double value = 0, min = 0, max = 0;
        double lastT = -1;     // time of the last value (poller clock, s)
        QString valueText;     // enum / on-off text
        QString feedNote;      // "Polled" / "Streamed…" (value tooltip)
        bool dirty = false;
    };
    QString valueTip(const Sig &s) const;   // value tooltip: how it is read, and the reading as read

    QTreeWidgetItem *makeGroup(QTreeWidgetItem *parent, const QString &title, const QString &hint = {});
    int addSignal(Sig s);   // placed by place() from s.section
    // The list has two parts: "Working on this vehicle" (answered values, and everything ticked)
    // on top, "Everything else" collapsed below. Values move between them as checks come in.
    bool isWorking(const Sig &s) const;
    QTreeWidgetItem *groupFor(bool working, const QStringList &section);
    void place(Sig &s);
    void relocateAll();              // place every value, drop empty groups, update headings
    void pruneGroups();
    void updateHeaders();
    void checkBroadcast(const Sig &bc, double value);   // against the vehicle's standard values
public:
    QString capabilitySummary() const;   // plain-language "what this vehicle gives", after a check
private:
    void removeSignals(Source source);
    void addBroadcastMessage(const DbcMessage &m);
    void resetBroadcast();   // empty the Broadcast group (definitions changed)
    void applySelection();
    void record(Sig &s, double t, double value, const QString &text = {});
    int filterTree(QTreeWidgetItem *node, const QString &text);  // returns visible leaves
    void markOeCommand(int command, bool answered, const QString &why);
    QString vehicleKey() const;      // VIN, or make|model|year without one
    QString probeCacheKey() const;   // settings key for this vehicle's "doesn't answer" list
    QString standardCacheKey() const;   // settings key for this vehicle's supported standard PIDs
    QString selectionKey() const;    // settings key for this vehicle's ticked values
    QString overlayKey() const;      // settings key for this vehicle's overlay values
    void applyStandardSupport(const QSet<quint8> &supported);
    void loadSelection();            // this vehicle's ticks (or the defaults) onto the list
    void updateSourceLabels();       // same name from two sources that both answer: say which
    static QString commandId(const OeCommand &c);
    void updateOeHeader();
    quint16 streamDid(const Sig &s) const;   // DID it can be streamed as from the engine computer, 0 = none
    DiagPoller::Item oeItem(int command) const;   // a library command as a poll/check request

    CanBridge *m_bridge;
    DbcLibrary *m_dbc;
    EnhancedLibrary *m_enhanced;
    OeLibrary *m_oe;
    DiagPoller *m_poller;
    StreamSession *m_stream;              // fast mode: engine computer streams, the rest is polled
    QCheckBox *m_fastBox = nullptr;
    QHash<quint16, QList<int>> m_streamKeys;   // streamable DID -> ticked signal keys
    int m_streamValues = 0;               // values received by stream (for the rate)
    bool m_streamTalking = false;         // stream being set up / taken down
    bool m_othersTalking = false;         // Codes window reading
    LivePlot *m_plot = nullptr;
    QTreeWidget *m_tree;
    QTreeWidgetItem *m_workTop = nullptr, *m_restTop = nullptr;
    QHash<QString, QTreeWidgetItem *> m_groups;   // "W|section|sub" / "R|…" -> group item
    QTimer m_relocate;                            // deferred relocateAll()
    bool m_stdChecked = false;                    // standard support known for this vehicle
    QHash<QString, quint8> m_moduleSessions;      // module -> session its values need (library)
    QHash<QString, int> m_bcBad, m_bcGood;        // broadcast file -> disagreeing / agreeing samples
    QLineEdit *m_search;
    QString m_make, m_model;              // "" model = all models
    QString m_vin;                        // "" = not read (vehicle chosen by hand)
    int m_year = 0;                       // 0 = any year
    int m_lastStandardCount = 0;          // from the last Check supported
    QPushButton *m_startBtn, *m_discoverBtn, *m_scanBtn;
    QLabel *m_rateLabel;
    QTimer m_refresh;
    int m_lastReplies = 0;
    int m_rateTick = 0;

    QList<OeCommand> m_oeCommands;
    QStringList m_cmdIds;                 // commandId() of each of m_oeCommands (cached)
    QList<OeModule> m_modules;
    bool m_probeChecked = false;          // Check supported has run for this vehicle
    QList<int> m_probeCommands;           // probe item index -> command index
    QStringList m_probeSilent;            // command ids the vehicle didn't answer (cached per vehicle)
    QString m_vehicleLabel;               // "Ford Transit Connect"
    QHash<int, Sig> m_sigs;
    QHash<QString, int> m_keyOf;          // persistId -> key
    QSet<quint32> m_seenBroadcast;        // CAN IDs already listed
    QHash<quint32, int> m_busLengths;     // every broadcast ID seen this connection -> data length
    QTimer m_trafficMatch;                // debounce for DbcLibrary::matchTraffic
    QString m_dbcMake;                    // make the definition files were last set for
    QHash<quint32, QList<int>> m_bcKeys;  // CAN ID -> signal keys
    QSet<QString> m_checked;              // persisted selection
    QStringList m_overlayOrder;           // overlay values, in display order (persisted)
    bool m_overlayShown = false, m_mainMinimized = false, m_scope = false;
    void updateScope();
    // Freeze check: a 100 ms tick that logs whenever the app didn't respond for over 0.5 s, and
    // every 10 s how many overlay values arrived (requests log).
    QTimer m_stallTick;
    QElapsedTimer m_stallClock;
    qint64 m_lastTick = 0, m_lastOverlayLog = 0;
    int m_overlaySamples = 0;
    int m_nextKey = 1;
    bool m_building = false;
};
