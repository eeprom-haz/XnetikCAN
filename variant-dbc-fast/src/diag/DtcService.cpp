#include "DtcService.h"
#include "IsoTp.h"
#include "../can/CanBridge.h"
#include "../obd/ObdPids.h"
#include "OeLibrary.h"
#include <QTimer>
#include <QCanBusFrame>
#include <cmath>
#include <memory>

static constexpr int kSettleMs = 150;        // let a live-data request in flight finish first
static constexpr int kScanTimeoutMs = 60;    // per address during the module scan
static constexpr int kListenFirstMs = 2000;  // watch the bus this long before scanning

DtcService::DtcService(CanBridge *bridge, QObject *parent)
    : QObject(parent), m_bridge(bridge), m_tp(new IsoTp(bridge, this)) {
    connect(m_tp, &IsoTp::response, this, [this](quint32 rxId, const QByteArray &p) {
        m_running = false;
        m_lastRxId = rxId;
        // A negative response (7F <sid> <nrc>) means "no data" for our purposes.
        m_lastNegative = !p.isEmpty() && quint8(p[0]) == 0x7F;
        m_lastNrc = m_lastNegative && p.size() >= 3 ? quint8(p[2]) : 0;
        if (m_current.done) m_current.done(m_lastNegative ? nullptr : &p);
        runNext();
    });
    connect(m_tp, &IsoTp::timedOut, this, [this] {
        m_running = false;
        m_lastNegative = false;
        if (m_current.done) m_current.done(nullptr);
        runNext();
    });
    connect(m_bridge, &CanBridge::stateChanged, this, [this](bool connected) {
        m_seenIds.clear();
        m_listening.start();
        if (!connected) { m_steps.clear(); m_tp->cancel(); m_running = false; finished(); }
    });
    connect(m_bridge, &CanBridge::liveFrame, this, [this](const QCanBusFrame &f) {
        if (!f.hasExtendedFrameFormat() && !IsoTp::ourId(f.frameId())) m_seenIds.insert(f.frameId());
    });
    m_listening.start();
}

void DtcService::start() {
    if (m_talking) { runNext(); return; }
    m_talking = true;
    emit talking(true);
    QTimer::singleShot(kSettleMs, this, &DtcService::runNext);
}

void DtcService::finished() {
    if (!m_talking) return;
    m_talking = false;
    emit talking(false);
}

void DtcService::runNext() {
    if (m_running) return;
    if (m_steps.isEmpty() || !m_bridge->isConnected()) { finished(); return; }
    m_current = m_steps.takeFirst();
    m_running = m_tp->request(m_current.tx, m_current.rxMin, m_current.rxMax, m_current.req,
                              m_current.timeoutMs);
    if (!m_running) {  // couldn't send (bus busy/disconnected): treat as no answer
        m_lastNegative = false;
        if (m_current.done) m_current.done(nullptr);
        runNext();
    }
}

QString DtcService::moduleName(quint32 id, const QString &make) {
    switch (id) {
    case 0x7E0: return "Engine (ECM)";
    case 0x7E1: return "Transmission (TCM)";
    case 0x7DF: return "All modules";
    default: break;
    }
    // Only names confirmed for the make; everything else stays a plain address.
    const QString known = OeLibrary::knownModuleName(make, id);
    if (!known.isEmpty()) return known;
    return QString("Module %1").arg(QString::number(id, 16).rightJustified(3, '0').toUpper());
}

QString DtcService::decodeDtc(quint8 hi, quint8 lo) {
    static const char letters[] = {'P', 'C', 'B', 'U'};
    return QString("%1%2%3%4%5")
        .arg(letters[hi >> 6])
        .arg((hi >> 4) & 0x3)
        .arg(hi & 0xF, 1, 16)
        .arg(lo >> 4, 1, 16)
        .arg(lo & 0xF, 1, 16)
        .toUpper();
}

// ISO 14229-1 Annex D failure type (third DTC byte).
QString DtcService::failureTypeText(quint8 f) {
    static const QHash<int, QString> t = {
        {0x00, "no sub-type"}, {0x01, "general electrical failure"}, {0x02, "general signal failure"},
        {0x03, "FM/PWM failure"}, {0x04, "system internal failure"}, {0x05, "system programming failure"},
        {0x06, "algorithm based failure"}, {0x07, "mechanical failure"}, {0x08, "bus signal / message failure"},
        {0x09, "component failure"}, {0x11, "circuit short to ground"}, {0x12, "circuit short to battery"},
        {0x13, "circuit open"}, {0x14, "circuit short to ground or open"}, {0x15, "circuit short to battery or open"},
        {0x16, "circuit voltage below threshold"}, {0x17, "circuit voltage above threshold"},
        {0x18, "circuit current below threshold"}, {0x19, "circuit current above threshold"},
        {0x1A, "circuit resistance below threshold"}, {0x1B, "circuit resistance above threshold"},
        {0x1C, "circuit voltage out of range"}, {0x1D, "circuit current out of range"},
        {0x1E, "circuit resistance out of range"}, {0x1F, "circuit intermittent"},
        {0x21, "signal amplitude below minimum"}, {0x22, "signal amplitude above maximum"},
        {0x23, "signal stuck low"}, {0x24, "signal stuck high"}, {0x25, "signal shape / waveform failure"},
        {0x26, "signal rate of change below threshold"}, {0x27, "signal rate of change above threshold"},
        {0x28, "signal bias level out of range"}, {0x29, "signal invalid"}, {0x2A, "signal stuck in range"},
        {0x2F, "signal erratic"}, {0x31, "no signal"}, {0x36, "signal frequency too low"},
        {0x37, "signal frequency too high"}, {0x38, "signal frequency incorrect"},
        {0x41, "checksum failure"}, {0x42, "memory failure"}, {0x44, "data memory failure"},
        {0x45, "program memory failure"}, {0x46, "calibration memory failure"},
        {0x47, "watchdog / safety processor failure"}, {0x48, "supervision software failure"},
        {0x49, "internal electronic failure"}, {0x4A, "incorrect component installed"},
        {0x4B, "over temperature"}, {0x51, "not programmed"}, {0x52, "not activated"}, {0x53, "deactivated"},
        {0x54, "missing calibration"}, {0x55, "not configured"}, {0x61, "signal calculation failure"},
        {0x62, "signal compare failure"}, {0x63, "circuit / component protection time-out"},
        {0x64, "signal plausibility failure"}, {0x65, "signal has too few transitions"},
        {0x66, "signal has too many transitions"}, {0x67, "signal incorrect after event"},
        {0x68, "event information"}, {0x71, "actuator stuck"}, {0x72, "actuator stuck open"},
        {0x73, "actuator stuck closed"}, {0x74, "actuator slipping"}, {0x75, "emergency position not reachable"},
        {0x76, "wrong mounting position"}, {0x77, "commanded position not reachable"},
        {0x78, "alignment or adjustment incorrect"}, {0x79, "mechanical linkage failure"},
        {0x7A, "fluid leak or seal failure"}, {0x7B, "low fluid level"},
        {0x81, "invalid serial data received"}, {0x82, "alive / sequence counter incorrect"},
        {0x83, "signal protection value incorrect"}, {0x84, "signal below allowable range"},
        {0x85, "signal above allowable range"}, {0x86, "invalid signal"}, {0x87, "missing message"},
        {0x88, "bus off"}, {0x8F, "erratic"}, {0x91, "parametric"}, {0x92, "performance or incorrect operation"},
        {0x93, "no operation"}, {0x94, "unexpected operation"}, {0x95, "incorrect assembly"},
        {0x96, "component internal failure"}, {0x97, "operation obstructed or blocked"},
        {0x98, "component or system over temperature"},
    };
    return t.value(f, QString("failure type %1").arg(QString::number(f, 16).rightJustified(2, '0').toUpper()));
}

QString DtcService::statusText(quint8 s) {
    if (s & 0x01) return "Active now";
    if (s & 0x08) return "Stored";
    if (s & 0x04) return "Pending";
    return "History";
}

void DtcService::parseDtcs(const QByteArray &r, const QString &kind, quint32 module, QList<Dtc> *out) {
    // CAN (ISO 15765-4) replies are "4x NN d1hi d1lo ...". Be tolerant of ECUs
    // that leave out the count byte.
    // With the count byte the reply length is even (sid + count + pairs); without it, odd.
    if (r.size() < 2) return;
    const int start = (r.size() % 2 == 0) ? 2 : 1;
    for (int i = start; i + 1 < r.size(); i += 2) {
        const quint8 hi = quint8(r[i]), lo = quint8(r[i + 1]);
        if (hi == 0 && lo == 0) continue;  // padding
        out->append({decodeDtc(hi, lo), kind, module});
    }
}

void DtcService::readCodes() {
    if (busy()) return;
    m_codes.clear();
    m_answered.clear();
    emit progress("Reading codes…");

    // Probe each module with "stored codes"; only modules that answer get the
    // pending/permanent requests, which keeps a full read to a couple of seconds.
    for (quint32 tx = 0x7E0; tx <= 0x7E7; ++tx) {
        const quint32 rx = tx + 8;
        m_steps.append({tx, rx, rx, QByteArray(1, 0x03), 150, [this, tx, rx](const QByteArray *r) {
            if (!r || quint8((*r)[0]) != 0x43) return;
            m_answered.append(tx);
            parseDtcs(*r, "Stored", tx, &m_codes);
            emit progress(QString("Reading codes… %1 answered").arg(moduleName(tx)));
            // Queue this module's pending + permanent reads right after it.
            m_steps.prepend({tx, rx, rx, QByteArray(1, 0x0A), 300, [this, tx](const QByteArray *r2) {
                if (r2 && quint8((*r2)[0]) == 0x4A) parseDtcs(*r2, "Permanent", tx, &m_codes);
            }});
            m_steps.prepend({tx, rx, rx, QByteArray(1, 0x07), 300, [this, tx](const QByteArray *r2) {
                if (r2 && quint8((*r2)[0]) == 0x47) parseDtcs(*r2, "Pending", tx, &m_codes);
            }});
        }});
    }
    // Final step has an empty request, so it never touches the bus: IsoTp refuses
    // it and runNext() calls done() straight away, after all module reads above.
    m_steps.append({0, 0, 0, {}, 0, [this](const QByteArray *) { emit codesRead(m_codes, m_answered); }});
    start();
}

void DtcService::clearCodes() {
    if (busy()) return;
    emit progress("Clearing codes…");
    m_steps.append({0x7DF, 0x7E8, 0x7EF, QByteArray(1, 0x04), 1000, [this](const QByteArray *r) {
        const bool ok = r && quint8((*r)[0]) == 0x44;
        emit cleared(ok, ok ? "Codes cleared. Readiness monitors are now reset."
                            : "No module confirmed the clear. Check the key is ON and engine OFF, then try again.");
    }});
    start();
}

void DtcService::readReadiness() {
    if (busy()) return;
    emit progress("Checking readiness…");
    m_ready = Readiness();

    auto pid = [](quint8 p) { QByteArray q(2, 0x01); q[1] = char(p); return q; };
    auto value16 = [](const QByteArray *r) { return (quint8((*r)[2]) << 8) | quint8((*r)[3]); };

    m_steps.append({0x7E0, 0x7E8, 0x7E8, pid(0x01), 300, [this](const QByteArray *r) {
        if (!r || r->size() < 6 || quint8((*r)[0]) != 0x41) return;
        const quint8 A = quint8((*r)[2]), B = quint8((*r)[3]), C = quint8((*r)[4]), D = quint8((*r)[5]);
        Readiness &R = m_ready;
        R.valid = true;
        R.milOn = A & 0x80;
        R.dtcCount = A & 0x7F;
        R.compressionIgnition = B & 0x08;
        // Continuous monitors: supported bits 0-2, incomplete bits 4-6 of B.
        const char *continuous[] = {"Misfire", "Fuel system", "Comprehensive components"};
        for (int i = 0; i < 3; ++i)
            R.monitors.append({continuous[i], bool(B & (1 << i)), !(B & (1 << (i + 4)))});
        // Non-continuous monitors: supported in C, incomplete in D (SAE J1979).
        const char *spark[] = {"Catalyst", "Heated catalyst", "Evaporative system", "Secondary air",
                               "A/C refrigerant", "Oxygen sensor", "Oxygen sensor heater", "EGR / VVT"};
        const char *diesel[] = {"NMHC catalyst", "NOx / SCR aftertreatment", nullptr, "Boost pressure",
                                nullptr, "Exhaust gas sensor", "Particulate filter", "EGR / VVT"};
        for (int i = 0; i < 8; ++i) {
            const char *name = R.compressionIgnition ? diesel[i] : spark[i];
            if (!name) continue;
            R.monitors.append({name, bool(C & (1 << i)), !(D & (1 << i))});
        }
    }});
    m_steps.append({0x7E0, 0x7E8, 0x7E8, pid(0x31), 200, [this, value16](const QByteArray *r) {
        if (r && r->size() >= 4 && quint8((*r)[0]) == 0x41) m_ready.distanceSinceClearKm = value16(r);
    }});
    m_steps.append({0x7E0, 0x7E8, 0x7E8, pid(0x30), 200, [this](const QByteArray *r) {
        if (r && r->size() >= 3 && quint8((*r)[0]) == 0x41) m_ready.warmUpsSinceClear = quint8((*r)[2]);
    }});
    m_steps.append({0x7E0, 0x7E8, 0x7E8, pid(0x21), 200, [this, value16](const QByteArray *r) {
        if (r && r->size() >= 4 && quint8((*r)[0]) == 0x41) m_ready.distanceMilOnKm = value16(r);
    }});
    m_steps.append({0x7E0, 0x7E8, 0x7E8, pid(0x4E), 200, [this, value16](const QByteArray *r) {
        if (r && r->size() >= 4 && quint8((*r)[0]) == 0x41) m_ready.minutesSinceClear = value16(r);
        emit readinessRead(m_ready);
    }});
    start();
}

// ---------------------------------------------------------------- freeze frame (Mode 02)

static QString formatNumber(double v) {
    const double a = std::fabs(v);
    const int decimals = a >= 100 ? 0 : a >= 10 ? 1 : a >= 1 ? 2 : 3;
    return QString::number(v, 'f', decimals);
}

void DtcService::readFreezeFrames(const QList<quint32> &modules) {
    if (busy()) return;
    m_frames.clear();
    emit progress("Reading freeze frame…");
    for (quint32 m : modules.isEmpty() ? QList<quint32>{0x7E0} : modules) queueFreezeFrame(m);
    m_steps.append({0, 0, 0, {}, 0, [this](const QByteArray *) {
        emit freezeFramesRead(m_frames);
        emit progress(m_frames.isEmpty() ? "No freeze frame stored" : "Freeze frame read");
    }});
    start();
}

void DtcService::queueFreezeFrame(quint32 tx) {
    const quint32 rx = tx + 8;
    auto req = [](quint8 pid) { QByteArray q(3, 0); q[0] = 0x02; q[1] = char(pid); return q; };  // frame 0
    // 02 02 00: the code that stored the frame. 0000 = no frame stored.
    m_steps.append({tx, rx, rx, req(0x02), 300, [this, tx, rx, req](const QByteArray *r) {
        if (!r || r->size() < 5 || quint8((*r)[0]) != 0x42) return;
        const quint8 hi = quint8((*r)[3]), lo = quint8((*r)[4]);
        if (hi == 0 && lo == 0) return;
        FreezeFrame ff;
        ff.module = tx;
        ff.dtc = decodeDtc(hi, lo);
        m_frames.append(ff);
        const int index = m_frames.size() - 1;

        // Ask one value. Each answer is decoded with the Mode 01 table (same PIDs, same scaling).
        auto value = [this, index, tx, rx, req](quint8 pid) {
            return Step{tx, rx, rx, req(pid), 200, [this, index, pid](const QByteArray *v) {
                if (!v || v->size() < 4 || quint8((*v)[0]) != 0x42 || quint8((*v)[1]) != pid) return;
                const QByteArray data = v->mid(3);
                for (const ObdPid *p : findObdPid(pid)) {
                    if (data.size() < p->bytes) continue;
                    const quint8 *d = reinterpret_cast<const quint8 *>(data.constData());
                    if (p->text) {
                        m_frames[index].values.append({p->name, p->text(d), {}});
                        continue;
                    }
                    const double x = p->decode(d);
                    if (std::isnan(x)) continue;
                    m_frames[index].values.append({p->name, formatNumber(x), p->unit, x, true});
                }
            }};
        };

        // Which values the frame holds: 02 00 00, 02 20 00, ... like Mode 01's support bitmaps.
        // Some ECUs don't answer those; then ask the values every freeze frame normally has.
        auto supported = std::make_shared<std::function<void(quint8)>>();
        *supported = [this, tx, rx, req, value, supported](quint8 base) {
            m_steps.prepend({tx, rx, rx, req(base), 200, [this, base, value, supported](const QByteArray *s) {
                QList<quint8> pids;
                bool more = false;
                if (s && s->size() >= 7 && quint8((*s)[0]) == 0x42 && quint8((*s)[1]) == base) {
                    const quint32 bits = (quint8((*s)[3]) << 24) | (quint8((*s)[4]) << 16) |
                                         (quint8((*s)[5]) << 8) | quint8((*s)[6]);
                    for (int i = 0; i < 32; ++i) {
                        if (!(bits & (0x80000000u >> i))) continue;
                        const quint8 pid = quint8(base + i + 1);
                        if (i == 31) more = true;          // next bitmap
                        else if (pid != 0x02) pids << pid;
                    }
                } else if (base == 0) {
                    pids = {0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11};
                }
                // Prepend in reverse so the values come out in PID order, before the rest of the queue.
                if (more && base < 0xE0) (*supported)(quint8(base + 0x20));
                for (int i = pids.size() - 1; i >= 0; --i) m_steps.prepend(value(pids[i]));
            }});
        };
        (*supported)(0x00);
    }});
}

// ---------------------------------------------------------------- monitor tests (Mode 06)

namespace {
struct Uas { double scale; double offset; const char *unit; };

// SAE J1979 Appendix E unit and scaling IDs (as listed by python-OBD). 0x80+ are signed.
bool unitAndScaling(quint8 id, Uas *out) {
    static const QHash<int, Uas> t = {
        {0x01, {1, 0, ""}}, {0x02, {0.1, 0, ""}}, {0x03, {0.01, 0, ""}}, {0x04, {0.001, 0, ""}},
        {0x05, {0.0000305, 0, ""}}, {0x06, {0.000305, 0, ""}}, {0x07, {0.25, 0, "rpm"}},
        {0x08, {0.01, 0, "km/h"}}, {0x09, {1, 0, "km/h"}}, {0x0A, {0.122, 0, "mV"}}, {0x0B, {0.001, 0, "V"}},
        {0x0C, {0.01, 0, "V"}}, {0x0D, {0.00390625, 0, "mA"}}, {0x0E, {0.001, 0, "A"}}, {0x0F, {0.01, 0, "A"}},
        {0x10, {1, 0, "ms"}}, {0x11, {100, 0, "ms"}}, {0x12, {1, 0, "s"}}, {0x13, {1, 0, "mΩ"}},
        {0x14, {1, 0, "Ω"}}, {0x15, {1, 0, "kΩ"}}, {0x16, {0.1, -40, "°C"}}, {0x17, {0.01, 0, "kPa"}},
        {0x18, {0.0117, 0, "kPa"}}, {0x19, {0.079, 0, "kPa"}}, {0x1A, {1, 0, "kPa"}}, {0x1B, {10, 0, "kPa"}},
        {0x1C, {0.01, 0, "°"}}, {0x1D, {0.5, 0, "°"}}, {0x1E, {0.0000305, 0, "λ"}}, {0x1F, {0.05, 0, "AFR"}},
        {0x20, {0.00390625, 0, "ratio"}}, {0x21, {1, 0, "mHz"}}, {0x22, {1, 0, "Hz"}}, {0x23, {1, 0, "kHz"}},
        {0x24, {1, 0, "counts"}}, {0x25, {1, 0, "km"}}, {0x26, {0.1, 0, "mV/ms"}}, {0x27, {0.01, 0, "g/s"}},
        {0x28, {1, 0, "g/s"}}, {0x29, {0.25, 0, "Pa/s"}}, {0x2A, {0.001, 0, "kg/h"}}, {0x2B, {1, 0, "switches"}},
        {0x2C, {0.01, 0, "g/cyl"}}, {0x2D, {0.01, 0, "mg/stroke"}}, {0x2E, {1, 0, ""}}, {0x2F, {0.01, 0, "%"}},
        {0x30, {0.001526, 0, "%"}}, {0x31, {0.001, 0, "L"}}, {0x32, {0.0000305, 0, "in"}},
        {0x33, {0.00024414, 0, "ratio"}}, {0x34, {1, 0, "min"}}, {0x35, {10, 0, "ms"}}, {0x36, {0.01, 0, "g"}},
        {0x37, {0.1, 0, "g"}}, {0x38, {1, 0, "g"}}, {0x39, {0.01, -327.68, "%"}}, {0x3A, {0.001, 0, "g"}},
        {0x3B, {0.0001, 0, "g"}}, {0x3C, {0.1, 0, "µs"}}, {0x3D, {0.01, 0, "mA"}}, {0x3E, {0.00006103516, 0, "mm²"}},
        {0x3F, {0.01, 0, "L"}}, {0x40, {1, 0, "ppm"}}, {0x41, {0.01, 0, "µA"}},
        {0x81, {1, 0, ""}}, {0x82, {0.1, 0, ""}}, {0x83, {0.01, 0, ""}}, {0x84, {0.001, 0, ""}},
        {0x85, {0.0000305, 0, ""}}, {0x86, {0.000305, 0, ""}}, {0x87, {1, 0, "ppm"}}, {0x8A, {0.122, 0, "mV"}},
        {0x8B, {0.001, 0, "V"}}, {0x8C, {0.01, 0, "V"}}, {0x8D, {0.00390625, 0, "mA"}}, {0x8E, {0.001, 0, "A"}},
        {0x90, {1, 0, "ms"}}, {0x96, {0.1, 0, "°C"}}, {0x99, {0.1, 0, "kPa"}}, {0xA8, {1, 0, "g/s"}},
        {0xA9, {0.25, 0, "Pa/s"}}, {0xAD, {0.01, 0, "mg/stroke"}}, {0xAE, {0.1, 0, "mg/stroke"}},
        {0xAF, {0.01, 0, "%"}}, {0xB0, {0.003052, 0, "%"}}, {0xB1, {2, 0, "mV/s"}}, {0xFC, {0.01, 0, "kPa"}},
        {0xFD, {0.001, 0, "kPa"}}, {0xFE, {0.25, 0, "Pa"}},
    };
    auto it = t.find(id);
    if (it == t.end()) return false;
    *out = *it;
    return true;
}

QString monitorName(quint8 mid) {
    auto bankSensor = [](int n) { return QString("bank %1 sensor %2").arg(n / 4 + 1).arg(n % 4 + 1); };
    if (mid >= 0x01 && mid <= 0x10) return "Oxygen sensor " + bankSensor(mid - 0x01);
    if (mid >= 0x21 && mid <= 0x24) return QString("Catalyst bank %1").arg(mid - 0x20);
    if (mid >= 0x31 && mid <= 0x34) return QString("EGR bank %1").arg(mid - 0x30);
    if (mid >= 0x35 && mid <= 0x38) return QString("VVT bank %1").arg(mid - 0x34);
    switch (mid) {
    case 0x39: return "EVAP leak (cap off / 0.150\")";
    case 0x3A: return "EVAP leak (0.090\")";
    case 0x3B: return "EVAP leak (0.040\")";
    case 0x3C: return "EVAP leak (0.020\")";
    case 0x3D: return "EVAP purge flow";
    default: break;
    }
    if (mid >= 0x41 && mid <= 0x50) return "Oxygen sensor heater " + bankSensor(mid - 0x41);
    if (mid >= 0x61 && mid <= 0x64) return QString("Heated catalyst bank %1").arg(mid - 0x60);
    if (mid >= 0x71 && mid <= 0x74) return QString("Secondary air %1").arg(mid - 0x70);
    if (mid >= 0x81 && mid <= 0x84) return QString("Fuel system bank %1").arg(mid - 0x80);
    if (mid >= 0x85 && mid <= 0x86) return QString("Boost pressure control bank %1").arg(mid - 0x84);
    if (mid >= 0x90 && mid <= 0x91) return QString("NOx absorber bank %1").arg(mid - 0x8F);
    if (mid >= 0x98 && mid <= 0x99) return QString("NOx catalyst bank %1").arg(mid - 0x97);
    if (mid == 0xA1) return "Misfire, all cylinders";
    if (mid >= 0xA2 && mid <= 0xAD) return QString("Misfire cylinder %1").arg(mid - 0xA1);
    if (mid >= 0xB0 && mid <= 0xB1) return QString("Particulate filter bank %1").arg(mid - 0xAF);
    return QString("Monitor %1").arg(QString::number(mid, 16).rightJustified(2, '0').toUpper());
}

QString testName(quint8 mid, quint8 tid) {
    if (tid >= 0x80) return QString("Manufacturer test %1").arg(QString::number(tid, 16).rightJustified(2, '0').toUpper());
    if (mid >= 0x01 && mid <= 0x10) {
        static const char *o2[] = {nullptr, "Rich to lean threshold voltage", "Lean to rich threshold voltage",
                                   "Low voltage for switch time", "High voltage for switch time",
                                   "Rich to lean switch time", "Lean to rich switch time",
                                   "Minimum sensor voltage", "Maximum sensor voltage",
                                   "Time between transitions", "Sensor period"};
        if (tid >= 1 && tid <= 0x0A) return o2[tid];
    }
    if (mid >= 0xA1 && mid <= 0xAD) {
        if (tid == 0x0B) return "Misfires, average of last 10 drive cycles";
        if (tid == 0x0C) return "Misfires, this / last drive cycle";
    }
    return QString("Standard test %1").arg(QString::number(tid, 16).rightJustified(2, '0').toUpper());
}
}  // namespace

void DtcService::readMonitorTests(const QList<quint32> &modules) {
    if (busy()) return;
    m_tests.clear();
    m_testsAnswered = false;
    emit progress("Reading monitor tests (Mode 06)…");
    for (quint32 m : modules.isEmpty() ? QList<quint32>{0x7E0} : modules) queueMonitorTests(m);
    m_steps.append({0, 0, 0, {}, 0, [this](const QByteArray *) {
        emit monitorTestsRead(m_tests, m_testsAnswered);
        emit progress(QString("Monitor tests read: %1 results").arg(m_tests.size()));
    }});
    start();
}

void DtcService::queueMonitorTests(quint32 tx) {
    const quint32 rx = tx + 8;
    auto req = [](quint8 mid) { QByteArray q(2, 0x06); q[1] = char(mid); return q; };

    // One monitor's results: 46 [MID TID UASID value(2) min(2) max(2)]...
    auto results = [this, tx, rx, req](quint8 mid) {
        return Step{tx, rx, rx, req(mid), 300, [this, tx, mid](const QByteArray *r) {
            if (!r || r->size() < 10 || quint8((*r)[0]) != 0x46) return;
            for (int i = 1; i + 9 <= r->size(); i += 9) {
                const quint8 m = quint8((*r)[i]);
                if (m != mid) break;
                MonitorTest t;
                t.module = tx;
                t.mid = m;
                t.tid = quint8((*r)[i + 1]);
                t.uas = quint8((*r)[i + 2]);
                auto word = [r, i](int off) { return (quint8((*r)[i + off]) << 8) | quint8((*r)[i + off + 1]); };
                const bool isSigned = t.uas >= 0x80;
                auto raw = [isSigned](int w) { return isSigned ? int(qint16(quint16(w))) : w; };
                const int rv = raw(word(3)), rmin = raw(word(5)), rmax = raw(word(7));
                Uas u{1, 0, ""};
                t.unitsKnown = unitAndScaling(t.uas, &u);
                t.value = rv * u.scale + u.offset;
                t.min = rmin * u.scale + u.offset;
                t.max = rmax * u.scale + u.offset;
                t.unit = QString::fromUtf8(u.unit);
                t.monitor = monitorName(m);
                t.test = testName(m, t.tid);

                // All zero = the test hasn't run since codes were cleared / this drive cycle.
                if (rv == 0 && rmin == 0 && rmax == 0) {
                    t.status = MonitorTest::NotRun;
                } else if (rv < rmin || rv > rmax) {
                    t.status = MonitorTest::Fail;
                } else {
                    // A limit at the very end of the number range isn't a real limit.
                    const bool lowReal = isSigned ? rmin != -32768 : rmin != 0;
                    const bool highReal = isSigned ? rmax != 32767 : rmax != 0xFFFF;
                    bool close = false;
                    if (lowReal && highReal && rmax > rmin) {
                        const double span = rmax - rmin;
                        close = (rv - rmin) < 0.1 * span || (rmax - rv) < 0.1 * span;
                    } else if (highReal && rmax > 0 && !lowReal) {
                        close = rv > 0.9 * rmax;
                    } else if (lowReal && rmin > 0 && !highReal) {
                        close = rv < 1.1 * rmin;
                    }
                    t.status = close ? MonitorTest::Close : MonitorTest::Pass;
                }
                m_tests.append(t);
            }
        }};
    };

    // Supported monitors: 06 00, 06 20, ... each a 32-bit map like Mode 01's.
    auto supported = std::make_shared<std::function<void(quint8)>>();
    *supported = [this, tx, rx, req, results, supported](quint8 base) {
        m_steps.prepend({tx, rx, rx, req(base), 300, [this, base, results, supported](const QByteArray *s) {
            if (!s || s->size() < 6 || quint8((*s)[0]) != 0x46 || quint8((*s)[1]) != base) return;
            m_testsAnswered = true;
            const quint32 bits = (quint8((*s)[2]) << 24) | (quint8((*s)[3]) << 16) |
                                 (quint8((*s)[4]) << 8) | quint8((*s)[5]);
            QList<quint8> mids;
            bool more = false;
            for (int i = 0; i < 32; ++i) {
                if (!(bits & (0x80000000u >> i))) continue;
                if (i == 31) more = true;
                else mids << quint8(base + i + 1);
            }
            if (more && base < 0xE0) (*supported)(quint8(base + 0x20));
            for (int i = mids.size() - 1; i >= 0; --i) m_steps.prepend(results(mids[i]));
        }});
    };
    (*supported)(0x00);
}

// ---------------------------------------------------------------- all-module code scan (UDS 19 02)

void DtcService::scanModules(const QString &make, const QList<QPair<quint32, quint32>> &known) {
    if (busy()) return;
    if (m_listening.elapsed() < kListenFirstMs) {
        // Watch the bus a moment first, so busy IDs are known before anything is sent.
        emit progress("Watching the bus before scanning…");
        QTimer::singleShot(int(kListenFirstMs - m_listening.elapsed()) + 50, this,
                           [this, make, known] { scanModules(make, known); });
        return;
    }
    m_modules.clear();

    // Request IDs to try, with the reply ID range. ISO 15765-4 style: requests on 7x0..7x7,
    // answers 8 higher (Ford, most makes). Nissan/INFINITI modules mostly answer 20 higher
    // (743 -> 763), so there any reply from +8 to +20 counts. GM modules also use 241..25F with
    // answers 400 higher; only tried on a GM, because other makes send normal traffic there.
    struct Target { quint32 tx, rxMin, rxMax; };
    QList<Target> targets;
    const QString m = make.toLower();
    const bool nissan = m == "nissan" || m == "infiniti" || m == "nissan/infiniti" || m == "datsun";
    for (quint32 block = 0x700; block <= 0x7F0; block += 0x10)
        for (quint32 id = block; id <= block + 7; ++id)
            targets.append({id, id + 8, nissan ? qMin<quint32>(id + 0x20, 0x7FF) : id + 8});
    if (m == "chevrolet" || m == "gmc" || m == "buick" || m == "cadillac" || m == "pontiac" ||
        m == "saturn" || m == "hummer" || m == "oldsmobile")
        for (quint32 id = 0x241; id <= 0x25F; ++id) targets.append({id, id + 0x400, id + 0x400});
    // Documented pairs from the library: their exact reply ID wins over the guess above.
    for (const auto &k : known) {
        bool found = false;
        for (Target &t : targets)
            if (t.tx == k.first) { t.rxMin = t.rxMax = k.second; found = true; }
        if (!found && k.first != 0x7DF && k.first <= 0x7FF) targets.append({k.first, k.second, k.second});
    }

    // Never send on an ID some module already uses for its own messages.
    int skipped = 0;
    QList<Target> safe;
    for (const Target &t : targets) {
        if (m_seenIds.contains(t.tx)) ++skipped;
        else safe.append(t);
    }

    // 19 02 0D: codes with status "failed now", "pending" or "confirmed". Answering at all
    // (even "not supported") means a module lives at that address.
    const QByteArray req = QByteArray::fromHex("19020D");
    const int total = safe.size();
    for (int n = 0; n < total; ++n) {
        const Target t = safe[n];
        const quint32 tx = t.tx;
        m_steps.append({tx, t.rxMin, t.rxMax, req, kScanTimeoutMs, [this, tx, n, total, make](const QByteArray *r) {
            if (n % 8 == 0)
                emit progress(QString("Looking for modules… %1%  (%2 found)")
                                  .arg(100 * n / qMax(1, total)).arg(m_modules.size()));
            if (!r && !m_lastNegative) return;   // nobody there
            ModuleCodes mc;
            mc.requestId = tx;
            mc.responseId = m_lastRxId;
            if (!r) {
                mc.reportsCodes = false;
                mc.refusal = "NRC " + QString::number(m_lastNrc, 16).rightJustified(2, '0').toUpper();
            } else if (r->size() >= 3 && quint8((*r)[0]) == 0x59) {
                // 59 02 <status availability> [DTC hi, mid, low, status]...
                for (int i = 3; i + 3 < r->size(); i += 4) {
                    const quint8 hi = quint8((*r)[i]), mid = quint8((*r)[i + 1]);
                    const quint8 ftb = quint8((*r)[i + 2]), st = quint8((*r)[i + 3]);
                    if ((hi | mid | ftb) == 0) continue;
                    if (!(st & 0x0D)) continue;
                    mc.codes.append({decodeDtc(hi, mid), ftb, st});
                }
            }
            m_modules.append(mc);
            emit progress(QString("Looking for modules… %1 answered").arg(moduleName(tx, make)));
        }});
    }
    m_steps.append({0, 0, 0, {}, 0, [this, total, skipped](const QByteArray *) {
        emit modulesScanned(m_modules, total, skipped);
    }});
    start();
}
