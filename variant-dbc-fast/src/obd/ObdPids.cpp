#include "ObdPids.h"
#include <QStringList>
#include <cmath>

static double ab(const quint8 *d, int i = 0) { return d[i] * 256.0 + d[i + 1]; }
static double pct(const quint8 *d) { return d[0] * 100.0 / 255.0; }
static double temp(const quint8 *d) { return d[0] - 40.0; }
static double trim(const quint8 *d) { return (d[0] - 128.0) * 100.0 / 128.0; }
static double byteA(const quint8 *d) { return double(d[0]); }
static double torque(const quint8 *d) { return d[0] - 125.0; }
static double s16(const quint8 *d, int i = 0) { return double(qint16(quint16(d[i] << 8 | d[i + 1]))); }

static QString fuelSystem(quint8 v) {
    switch (v) {
    case 0:  return "Not used";
    case 1:  return "Open loop (warming up)";
    case 2:  return "Closed loop";
    case 4:  return "Open loop (load / decel)";
    case 8:  return "Open loop (system fault)";
    case 16: return "Closed loop (feedback fault)";
    default: return QString("Code %1").arg(v);
    }
}

static QString secondaryAir(quint8 v) {
    switch (v) {
    case 1: return "Upstream";
    case 2: return "Downstream of catalyst";
    case 4: return "Outside air / off";
    case 8: return "Pump on (diagnostics)";
    default: return QString("Code %1").arg(v);
    }
}

static QString obdStandard(quint8 v) {
    static const QStringList names = {
        "", "OBD-II (CARB)", "OBD (EPA)", "OBD and OBD-II", "OBD-I", "Not OBD compliant", "EOBD (Europe)",
        "EOBD and OBD-II", "EOBD and OBD", "EOBD, OBD and OBD-II", "JOBD (Japan)", "JOBD and OBD-II",
        "JOBD and EOBD", "JOBD, EOBD and OBD-II", "", "", "", "EMD", "EMD+", "HD OBD-C", "HD OBD",
        "WWH OBD", "", "HD EOBD-I", "HD EOBD-I N", "HD EOBD-II", "HD EOBD-II N", "", "OBDBr-1 (Brazil)",
        "OBDBr-2 (Brazil)", "KOBD (Korea)", "IOBD I (India)", "IOBD II (India)", "HD EOBD-IV"};
    return v < names.size() && !names[v].isEmpty() ? names[v] : QString("Code %1").arg(v);
}

static QString fuelType(quint8 v) {
    static const QStringList names = {
        "Not available", "Gasoline", "Methanol", "Ethanol", "Diesel", "LPG", "CNG", "Propane", "Electric",
        "Bi-fuel gasoline", "Bi-fuel methanol", "Bi-fuel ethanol", "Bi-fuel LPG", "Bi-fuel CNG",
        "Bi-fuel propane", "Bi-fuel electric", "Bi-fuel electric / combustion", "Hybrid gasoline",
        "Hybrid ethanol", "Hybrid diesel", "Hybrid electric", "Hybrid electric / combustion",
        "Hybrid regenerative", "Bi-fuel diesel"};
    return v < names.size() ? names[v] : QString("Code %1").arg(v);
}

// O2 sensor position for sensor slot i (0..7) in the 2-bank (PID 13) or 4-bank (PID 1D) layout.
static QString o2Pos(int i, bool fourBank) {
    return fourBank ? QString("B%1S%2").arg(i / 2 + 1).arg(i % 2 + 1)
                    : QString("B%1S%2").arg(i / 4 + 1).arg(i % 4 + 1);
}

static QString o2Present(quint8 bits, bool fourBank) {
    QStringList out;
    for (int i = 0; i < 8; ++i)
        if (bits & (1 << i)) out << o2Pos(i, fourBank);
    return out.isEmpty() ? QStringLiteral("None") : out.join(' ');
}

// Display ranges are what a tech expects to see on a scan tool graph, not the
// theoretical encoding range (e.g. RPM 0–7000, not 0–16383). 0/0 = auto-scale.
const QList<ObdPid> &obdPids() {
    static const QList<ObdPid> pids = [] {
        QList<ObdPid> l;
        auto add = [&l](quint8 pid, const QString &group, const QString &name, const QString &unit, int bytes,
                        std::function<double(const quint8 *)> decode, double lo, double hi,
                        std::function<QString(const quint8 *)> text = {}, const QString &name4 = {}) {
            int sub = 0;
            for (const ObdPid &p : l) sub += p.pid == pid;
            l.append({pid, sub, group, name, unit, bytes, std::move(decode), lo, hi, std::move(text), name4});
        };
        const QString eng = "Engine", fuel = "Fuel", o2 = "Oxygen / air-fuel sensors", air = "Air & throttle",
                      tmp = "Temperatures", emis = "Emissions (EGR / EVAP / catalyst)",
                      stat = "Status & distance", dsl = "Diesel / turbo", veh = "Vehicle & transmission";

        // ── Status ──
        add(0x01, stat, "Check engine lamp (MIL)", "", 4, [](const quint8 *d) { return (d[0] & 0x80) ? 1.0 : 0.0; },
            0, 1, [](const quint8 *d) { return QString((d[0] & 0x80) ? "On" : "Off"); });
        add(0x01, stat, "Codes stored (count)", "", 4, [](const quint8 *d) { return double(d[0] & 0x7F); }, 0, 0);
        add(0x1C, stat, "OBD standard", "", 1, byteA, 0, 0, [](const quint8 *d) { return obdStandard(d[0]); });
        add(0x51, stat, "Fuel type", "", 1, byteA, 0, 0, [](const quint8 *d) { return fuelType(d[0]); });
        add(0x1F, stat, "Run time since start", "s", 2, [](const quint8 *d) { return ab(d); }, 0, 0);
        add(0x7F, stat, "Total engine run time", "s", 13,
            [](const quint8 *d) { return double(quint32(d[1]) << 24 | quint32(d[2]) << 16 | quint32(d[3]) << 8 | d[4]); },
            0, 0);
        add(0x21, stat, "Distance with MIL on", "km", 2, [](const quint8 *d) { return ab(d); }, 0, 0);
        add(0x4D, stat, "Time with MIL on", "min", 2, [](const quint8 *d) { return ab(d); }, 0, 0);
        add(0x30, stat, "Warm-ups since codes cleared", "", 1, byteA, 0, 0);
        add(0x31, stat, "Distance since codes cleared", "km", 2, [](const quint8 *d) { return ab(d); }, 0, 0);
        add(0x4E, stat, "Time since codes cleared", "min", 2, [](const quint8 *d) { return ab(d); }, 0, 0);
        add(0x1E, stat, "PTO (power take-off)", "", 1, [](const quint8 *d) { return double(d[0] & 1); }, 0, 1,
            [](const quint8 *d) { return QString((d[0] & 1) ? "Active" : "Off"); });

        // ── Engine ──
        add(0x0C, eng, "Engine RPM", "rpm", 2, [](const quint8 *d) { return ab(d) / 4.0; }, 0, 7000);
        add(0x04, eng, "Engine load (calculated)", "%", 1, pct, 0, 100);
        add(0x43, eng, "Absolute load", "%", 2, [](const quint8 *d) { return ab(d) * 100.0 / 255.0; }, 0, 200);
        add(0x0E, eng, "Timing advance", "°", 1, [](const quint8 *d) { return d[0] / 2.0 - 64.0; }, -10, 50);
        add(0x42, eng, "Module voltage", "V", 2, [](const quint8 *d) { return ab(d) / 1000.0; }, 8, 16);
        add(0x61, eng, "Driver demand torque", "%", 1, torque, -125, 130);
        add(0x62, eng, "Actual engine torque", "%", 1, torque, -125, 130);
        add(0x63, eng, "Reference torque", "Nm", 2, [](const quint8 *d) { return ab(d); }, 0, 0);
        add(0x8E, eng, "Friction torque", "%", 1, torque, -125, 130);

        // ── Fuel ──
        add(0x03, fuel, "Fuel system 1 status", "", 2, byteA, 0, 0, [](const quint8 *d) { return fuelSystem(d[0]); });
        add(0x03, fuel, "Fuel system 2 status", "", 2, [](const quint8 *d) { return double(d[1]); }, 0, 0,
            [](const quint8 *d) { return fuelSystem(d[1]); });
        add(0x06, fuel, "Short fuel trim B1", "%", 1, trim, -25, 25);
        add(0x07, fuel, "Long fuel trim B1", "%", 1, trim, -25, 25);
        add(0x08, fuel, "Short fuel trim B2", "%", 1, trim, -25, 25);
        add(0x09, fuel, "Long fuel trim B2", "%", 1, trim, -25, 25);
        add(0x55, fuel, "Short secondary O2 trim B1", "%", 2, trim, -25, 25);
        add(0x55, fuel, "Short secondary O2 trim B3", "%", 2, [](const quint8 *d) { return trim(d + 1); }, -25, 25);
        add(0x56, fuel, "Long secondary O2 trim B1", "%", 2, trim, -25, 25);
        add(0x56, fuel, "Long secondary O2 trim B3", "%", 2, [](const quint8 *d) { return trim(d + 1); }, -25, 25);
        add(0x57, fuel, "Short secondary O2 trim B2", "%", 2, trim, -25, 25);
        add(0x57, fuel, "Short secondary O2 trim B4", "%", 2, [](const quint8 *d) { return trim(d + 1); }, -25, 25);
        add(0x58, fuel, "Long secondary O2 trim B2", "%", 2, trim, -25, 25);
        add(0x58, fuel, "Long secondary O2 trim B4", "%", 2, [](const quint8 *d) { return trim(d + 1); }, -25, 25);
        add(0x0A, fuel, "Fuel pressure (gauge)", "kPa", 1, [](const quint8 *d) { return d[0] * 3.0; }, 0, 765);
        add(0x22, fuel, "Fuel rail pressure (vs manifold)", "kPa", 2, [](const quint8 *d) { return ab(d) * 0.079; }, 0, 0);
        add(0x23, fuel, "Fuel rail pressure (gauge)", "kPa", 2, [](const quint8 *d) { return ab(d) * 10.0; }, 0, 0);
        add(0x59, fuel, "Fuel rail pressure (absolute)", "kPa", 2, [](const quint8 *d) { return ab(d) * 10.0; }, 0, 0);
        add(0x2F, fuel, "Fuel level", "%", 1, pct, 0, 100);
        add(0x52, fuel, "Ethanol content", "%", 1, pct, 0, 100);
        add(0x5D, fuel, "Fuel injection timing", "°", 2, [](const quint8 *d) { return ab(d) / 128.0 - 210.0; }, -30, 30);
        add(0x5E, fuel, "Engine fuel rate", "L/h", 2, [](const quint8 *d) { return ab(d) / 20.0; }, 0, 0);
        add(0x9D, fuel, "Engine fuel rate (mass)", "g/s", 4, [](const quint8 *d) { return ab(d) * 0.02; }, 0, 0);
        add(0x9D, fuel, "Vehicle fuel rate (mass)", "g/s", 4, [](const quint8 *d) { return ab(d, 2) * 0.02; }, 0, 0);
        add(0xA2, fuel, "Cylinder fuel rate", "mg/stroke", 2, [](const quint8 *d) { return ab(d) / 32.0; }, 0, 0);

        // ── Oxygen / air-fuel sensors ──
        add(0x13, o2, "O2 sensors fitted", "", 1, byteA, 0, 0, [](const quint8 *d) { return o2Present(d[0], false); });
        add(0x1D, o2, "O2 sensors fitted (4-bank layout)", "", 1, byteA, 0, 0,
            [](const quint8 *d) { return o2Present(d[0], true); });
        add(0x44, o2, "Commanded lambda (equivalence ratio)", "λ", 2, [](const quint8 *d) { return ab(d) * 2.0 / 65536.0; },
            0.7, 1.3);
        for (int i = 0; i < 8; ++i) {   // narrowband: voltage + the trim it feeds (B = 0xFF: not used for trim)
            const QString p2 = o2Pos(i, false), p4 = o2Pos(i, true);
            add(quint8(0x14 + i), o2, "O2 " + p2 + " voltage", "V", 2, [](const quint8 *d) { return d[0] / 200.0; },
                0, 1.1, {}, "O2 " + p4 + " voltage");
            add(quint8(0x14 + i), o2, "O2 " + p2 + " fuel trim", "%", 2,
                [](const quint8 *d) { return d[1] == 0xFF ? std::nan("") : trim(d + 1); }, -25, 25, {},
                "O2 " + p4 + " fuel trim");
        }
        for (int i = 0; i < 8; ++i) {   // wideband, voltage type
            const QString p2 = o2Pos(i, false), p4 = o2Pos(i, true);
            add(quint8(0x24 + i), o2, "O2 " + p2 + " lambda (voltage-type wideband)", "λ", 4,
                [](const quint8 *d) { return ab(d) * 2.0 / 65536.0; }, 0.7, 1.3, {},
                "O2 " + p4 + " lambda (voltage-type wideband)");
            add(quint8(0x24 + i), o2, "O2 " + p2 + " voltage (wideband)", "V", 4,
                [](const quint8 *d) { return ab(d, 2) * 8.0 / 65536.0; }, 0, 8, {}, "O2 " + p4 + " voltage (wideband)");
        }
        for (int i = 0; i < 8; ++i) {   // wideband, current type
            const QString p2 = o2Pos(i, false), p4 = o2Pos(i, true);
            add(quint8(0x34 + i), o2, "O2 " + p2 + " lambda (wideband)", "λ", 4,
                [](const quint8 *d) { return ab(d) * 2.0 / 65536.0; }, 0.7, 1.3, {}, "O2 " + p4 + " lambda (wideband)");
            // ("current" sensors are the common wideband / air-fuel ratio type)
            add(quint8(0x34 + i), o2, "O2 " + p2 + " current", "mA", 4,
                [](const quint8 *d) { return ab(d, 2) / 256.0 - 128.0; }, -2, 2, {}, "O2 " + p4 + " current");
        }

        // ── Air & throttle ──
        add(0x10, air, "MAF air flow", "g/s", 2, [](const quint8 *d) { return ab(d) / 100.0; }, 0, 250);
        add(0x66, air, "MAF sensor A", "g/s", 5, [](const quint8 *d) { return ab(d, 1) / 32.0; }, 0, 250);
        add(0x66, air, "MAF sensor B", "g/s", 5, [](const quint8 *d) { return ab(d, 3) / 32.0; }, 0, 250);
        add(0x0B, air, "Intake MAP", "kPa", 1, byteA, 0, 105);
        add(0x33, air, "Barometric pressure", "kPa", 1, byteA, 60, 105);
        add(0x11, air, "Throttle position", "%", 1, pct, 0, 100);
        add(0x45, air, "Relative throttle", "%", 1, pct, 0, 100);
        add(0x47, air, "Throttle position B", "%", 1, pct, 0, 100);
        add(0x48, air, "Throttle position C", "%", 1, pct, 0, 100);
        add(0x8D, air, "Throttle position G", "%", 1, pct, 0, 100);
        add(0x4C, air, "Commanded throttle", "%", 1, pct, 0, 100);
        add(0x49, air, "Accel pedal D", "%", 1, pct, 0, 100);
        add(0x4A, air, "Accel pedal E", "%", 1, pct, 0, 100);
        add(0x4B, air, "Accel pedal F", "%", 1, pct, 0, 100);
        add(0x5A, air, "Relative accel pedal", "%", 1, pct, 0, 100);
        add(0x12, air, "Secondary air status", "", 1, byteA, 0, 0, [](const quint8 *d) { return secondaryAir(d[0]); });

        // ── Temperatures ──
        add(0x05, tmp, "Coolant temp", "°C", 1, temp, -20, 120);
        add(0x67, tmp, "Coolant temp sensor 1", "°C", 3, [](const quint8 *d) { return temp(d + 1); }, -20, 120);
        add(0x67, tmp, "Coolant temp sensor 2", "°C", 3, [](const quint8 *d) { return temp(d + 2); }, -20, 120);
        add(0x0F, tmp, "Intake air temp", "°C", 1, temp, -20, 80);
        add(0x68, tmp, "Intake air temp B1 sensor 1", "°C", 7, [](const quint8 *d) { return temp(d + 1); }, -20, 80);
        add(0x68, tmp, "Intake air temp B1 sensor 2", "°C", 7, [](const quint8 *d) { return temp(d + 2); }, -20, 80);
        add(0x68, tmp, "Intake air temp B2 sensor 1", "°C", 7, [](const quint8 *d) { return temp(d + 4); }, -20, 80);
        add(0x46, tmp, "Ambient air temp", "°C", 1, temp, -20, 50);
        add(0x5C, tmp, "Oil temp", "°C", 1, temp, -20, 150);
        add(0x84, tmp, "Manifold surface temp", "°C", 1, temp, -20, 150);
        add(0x3C, tmp, "Catalyst temp B1S1", "°C", 2, [](const quint8 *d) { return ab(d) / 10.0 - 40.0; }, 0, 1000);
        add(0x3D, tmp, "Catalyst temp B2S1", "°C", 2, [](const quint8 *d) { return ab(d) / 10.0 - 40.0; }, 0, 1000);
        add(0x3E, tmp, "Catalyst temp B1S2", "°C", 2, [](const quint8 *d) { return ab(d) / 10.0 - 40.0; }, 0, 1000);
        add(0x3F, tmp, "Catalyst temp B2S2", "°C", 2, [](const quint8 *d) { return ab(d) / 10.0 - 40.0; }, 0, 1000);

        // ── Emissions ──
        add(0x2C, emis, "Commanded EGR", "%", 1, pct, 0, 100);
        add(0x2D, emis, "EGR error", "%", 1, trim, -100, 100);
        add(0x2E, emis, "Commanded EVAP purge", "%", 1, pct, 0, 100);
        add(0x32, emis, "EVAP vapour pressure", "Pa", 2, [](const quint8 *d) { return s16(d) / 4.0; }, 0, 0);
        add(0x53, emis, "EVAP vapour pressure (absolute)", "kPa", 2, [](const quint8 *d) { return ab(d) / 200.0; }, 0, 0);
        add(0x54, emis, "EVAP vapour pressure (wide range)", "Pa", 2, [](const quint8 *d) { return s16(d); }, 0, 0);
        add(0x9E, emis, "Exhaust flow rate", "kg/h", 2, [](const quint8 *d) { return ab(d) / 5.0; }, 0, 0);

        // ── Diesel / turbo ──
        for (int bank = 0; bank < 2; ++bank)
            for (int s = 0; s < 4; ++s)
                add(quint8(0x78 + bank), dsl, QString("Exhaust gas temp B%1 sensor %2").arg(bank + 1).arg(s + 1), "°C", 9,
                    [s](const quint8 *d) { return ab(d, 1 + 2 * s) / 10.0 - 40.0; }, 0, 1000);
        add(0x7C, dsl, "DPF inlet temp B1", "°C", 9, [](const quint8 *d) { return ab(d, 1) / 10.0 - 40.0; }, 0, 1000);
        add(0x7C, dsl, "DPF outlet temp B1", "°C", 9, [](const quint8 *d) { return ab(d, 3) / 10.0 - 40.0; }, 0, 1000);

        // ── Vehicle & transmission ──
        add(0x0D, veh, "Vehicle speed", "km/h", 1, byteA, 0, 200);
        add(0xA6, veh, "Odometer", "km", 4,
            [](const quint8 *d) { return double(quint32(d[0]) << 24 | quint32(d[1]) << 16 | quint32(d[2]) << 8 | d[3]) / 10.0; },
            0, 0);
        add(0xA4, veh, "Transmission gear", "", 4, [](const quint8 *d) { return double(d[1] >> 4); }, 0, 10);
        add(0xA4, veh, "Transmission gear ratio", "", 4, [](const quint8 *d) { return ab(d, 2) / 1000.0; }, 0, 0);
        add(0x5B, veh, "Hybrid battery remaining", "%", 1, pct, 0, 100);
        return l;
    }();
    return pids;
}

QList<const ObdPid *> findObdPid(quint8 pid) {
    QList<const ObdPid *> out;
    for (const ObdPid &p : obdPids())
        if (p.pid == pid) out << &p;
    return out;
}

int parseMode01Reply(const QByteArray &p, const std::function<void(quint8, const quint8 *, int)> &each) {
    if (p.size() < 2 || quint8(p[0]) != 0x41) return 0;
    int found = 0;
    int i = 1;
    while (i < p.size()) {
        const quint8 pid = quint8(p[i]);
        const QList<const ObdPid *> defs = findObdPid(pid);
        if (defs.isEmpty()) break;
        int len = defs.first()->bytes;
        // A lone PID may come back shorter than the table says (some ECUs trim it): take what's there.
        if (i + 1 + len > p.size()) {
            if (found > 0) break;
            len = int(p.size()) - i - 1;
        }
        each(pid, reinterpret_cast<const quint8 *>(p.constData()) + i + 1, len);
        ++found;
        i += 1 + defs.first()->bytes;
    }
    return found;
}
