#pragma once
#include <QObject>
#include <QList>
#include <QHash>
#include <QSet>
#include <QByteArray>
#include <QElapsedTimer>
#include <functional>

class CanBridge;
class IsoTp;
class QCanBusFrame;

struct Dtc {
    QString code;        // "P0301"
    QString kind;        // "Stored", "Pending", "Permanent"
    quint32 module = 0;  // request ID that reported it (0x7E0 = engine)
};

struct ReadinessMonitor {
    QString name;
    bool supported = false;
    bool complete = false;
};

struct Readiness {
    bool valid = false;
    bool milOn = false;
    int dtcCount = 0;
    bool compressionIgnition = false;  // diesel
    QList<ReadinessMonitor> monitors;
    int distanceSinceClearKm = -1;     // PID 31
    int warmUpsSinceClear = -1;        // PID 30
    int distanceMilOnKm = -1;          // PID 21
    int minutesSinceClear = -1;        // PID 4E
};

// Mode 02: the values an OBD module stored when it set a code.
struct FreezeValue {
    QString name, value, unit;     // value as text, in the unit read
    double number = 0;             // the number itself (numeric values), for unit conversion
    bool numeric = false;
};
struct FreezeFrame {
    quint32 module = 0;
    QString dtc;                       // code that caused it
    QList<FreezeValue> values;
};

// Mode 06: one on-board monitor test result with its limits.
struct MonitorTest {
    enum Status { Pass, Close, Fail, NotRun };
    quint32 module = 0;
    quint8 mid = 0, tid = 0, uas = 0;
    QString monitor;                   // "Catalyst monitor bank 1"
    QString test;                      // "Rich to lean switch time", "Manufacturer test 85"
    double value = 0, min = 0, max = 0;
    QString unit;
    bool unitsKnown = true;
    Status status = Pass;
};

// One module found by the all-module scan and the codes it reported (UDS service 19).
struct ModuleDtc {
    QString code;                      // "U0100"
    quint8 failureType = 0;            // third DTC byte, e.g. 0x87 = missing message
    quint8 status = 0;                 // UDS status bits
};
struct ModuleCodes {
    quint32 requestId = 0;
    quint32 responseId = 0;
    bool reportsCodes = true;          // false: answered, but refused the code request
    QString refusal;                   // "NRC 11" when refused
    QList<ModuleDtc> codes;
};

// Standard OBD-II code, clear and readiness services over ISO-TP, plus freeze frame (Mode 02),
// monitor tests (Mode 06) and a read-only code scan of every module on the bus (UDS 19 02).
// Codes are read physically from each engine-bay module (0x7E0..0x7E7) so every
// responder's list is collected; clearing is functional (0x7DF) as the standard requires.
class DtcService : public QObject {
    Q_OBJECT
public:
    explicit DtcService(CanBridge *bridge, QObject *parent = nullptr);

    void readCodes();
    void clearCodes();
    void readReadiness();
    void readFreezeFrames(const QList<quint32> &modules);
    void readMonitorTests(const QList<quint32> &modules);
    // make decides which address ranges are safe to ask; known = documented (request, reply)
    // pairs from the vehicle library, asked as well.
    void scanModules(const QString &make, const QList<QPair<quint32, quint32>> &known = {});
    bool busy() const { return !m_steps.isEmpty() || m_running; }

    static QString moduleName(quint32 requestId, const QString &make = {});
    static QString decodeDtc(quint8 hi, quint8 lo);
    static QString failureTypeText(quint8 ftb);
    static QString statusText(quint8 status);  // "Active now", "Stored", "Pending"

signals:
    void codesRead(const QList<Dtc> &codes, const QList<quint32> &modulesAnswered);
    void cleared(bool confirmed, const QString &message);
    void readinessRead(const Readiness &r);
    void freezeFramesRead(const QList<FreezeFrame> &frames);
    void monitorTestsRead(const QList<MonitorTest> &tests, bool anyAnswer);
    void modulesScanned(const QList<ModuleCodes> &modules, int addressesTried, int skippedBusy);
    void progress(const QString &text);
    // True while this service is talking to the modules: live-data polling should wait.
    void talking(bool busy);

private:
    struct Step {
        quint32 tx, rxMin, rxMax;
        QByteArray req;
        int timeoutMs;
        std::function<void(const QByteArray *reply)> done;  // nullptr reply = no answer
    };
    void start();
    void runNext();
    void finished();
    static void parseDtcs(const QByteArray &reply, const QString &kind, quint32 module, QList<Dtc> *out);
    void queueFreezeFrame(quint32 module);
    void queueMonitorTests(quint32 module);

    CanBridge *m_bridge;
    IsoTp *m_tp;
    QList<Step> m_steps;
    Step m_current;
    bool m_running = false;
    bool m_talking = false;
    bool m_lastNegative = false;     // last reply was 7F <sid> <nrc> (module there, refused)
    quint8 m_lastNrc = 0;
    quint32 m_lastRxId = 0;          // ID the last reply came from

    // collection state
    QList<Dtc> m_codes;
    QList<quint32> m_answered;
    Readiness m_ready;
    QList<FreezeFrame> m_frames;
    QList<MonitorTest> m_tests;
    bool m_testsAnswered = false;
    QList<ModuleCodes> m_modules;

    // IDs seen on the bus that we didn't send: never transmit a request on one of them.
    QSet<quint32> m_seenIds;
    QElapsedTimer m_listening;       // since connect: the scan waits until it has watched the bus
};
