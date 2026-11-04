#pragma once
#include <QString>
#include <QList>
#include <QByteArray>
#include <functional>

// SAE J1979 Mode 01 values. One PID can carry several values (e.g. 0x34 = lambda
// and sensor current), so each entry is one value: several entries share a PID.
struct ObdPid {
    quint8 pid;
    int sub;                                          // 0 = first value of this PID, 1, 2, ...
    QString group;                                    // tree folder, e.g. "Oxygen sensors"
    QString name;
    QString unit;
    int bytes;                                        // data bytes the reply must carry (A, B, ...)
    std::function<double(const quint8 *d)> decode;    // d[0] = A, d[1] = B, ...; NaN = no value
    double rangeMin;                                  // typical display range for graphs
    double rangeMax;
    std::function<QString(const quint8 *d)> text;     // optional: status / on-off text instead of a number
    QString name4Bank;                                // O2 sensor name when the ECU uses the 4-bank layout (PID 1D)
};

const QList<ObdPid> &obdPids();
QList<const ObdPid *> findObdPid(quint8 pid);         // every value carried by this PID

// Mode 01 reply (41 PID data [PID data]...) split into its PIDs, using each PID's length from
// the table above. Stops at a PID it doesn't know. Returns how many PIDs were found.
int parseMode01Reply(const QByteArray &reply, const std::function<void(quint8 pid, const quint8 *data, int len)> &each);
