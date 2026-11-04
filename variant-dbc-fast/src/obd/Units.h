#pragma once
#include <QObject>
#include <QString>

// Display units. Everything is read and kept in the vehicle's own (metric) units; only what is
// shown changes. Imperial: °F, mph, mi, gal, lb·ft, psi for pressures that push (fuel rail,
// boost, oil, tires…) and inHg for the ones that pull or float near the air (vacuum, intake /
// MAP, baro, EVAP / fuel tank). Volts, %, rpm, degrees, ms, g/s, lambda stay as they are.
namespace Units {

struct Conv {
    QString unit;              // unit to show
    double scale = 1, offset = 0;
    bool changed = false;      // false: shown as read
    double apply(double v) const { return v * scale + offset; }
};

bool imperial();
void setImperial(bool on);     // saved; Notifier::changed() is emitted
// How a value read in `unit` is shown right now. `name` picks psi or inHg for pressures.
Conv forUnit(const QString &unit, const QString &name = {});
Conv forUnit(const QString &unit, const QString &name, bool imperial);

class Notifier : public QObject {
    Q_OBJECT
signals:
    void changed();
};
Notifier *notifier();

}  // namespace Units
