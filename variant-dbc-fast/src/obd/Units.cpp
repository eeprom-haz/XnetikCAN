#include "Units.h"
#include <QSettings>
#include <QSet>

namespace Units {

namespace {
int g_imperial = -1;   // -1 = not read from the settings yet

// Pressures that pull or sit near the air are read in inHg on a US shop floor; the rest in psi.
// "Push" names are checked first: "fuel rail pressure (vs manifold)" is still a rail pressure.
bool isVacuumSide(const QString &name) {
    const QString n = name.toLower();
    for (const char *w : {"rail", "fuel pressure", "fuel press", "oil", "boost", "turbo", "tire", "tyre",
                          "brake", "hydraulic", "pump", "inject", "a/c", "refrigerant", "line pressure",
                          "clutch", "trans", "exhaust", "dpf", "particulate", "egr"})
        if (n.contains(QLatin1String(w))) return false;
    for (const char *w : {"vacuum", "evap", "vapor", "vapour", "tank", "purge", "canister", "intake",
                          "manifold", "map", "baro", "atmospher", "ambient air pressure"})
        if (n.contains(QLatin1String(w))) return true;
    return false;
}

Conv make(const QString &unit, double scale, double offset = 0) {
    Conv c;
    c.unit = unit;
    c.scale = scale;
    c.offset = offset;
    c.changed = true;
    return c;
}
}  // namespace

bool imperial() {
    if (g_imperial < 0) g_imperial = QSettings().value("view/imperial", true).toBool() ? 1 : 0;
    return g_imperial == 1;
}

void setImperial(bool on) {
    if (g_imperial == int(on)) return;
    g_imperial = on ? 1 : 0;
    QSettings().setValue("view/imperial", on);
    emit notifier()->changed();
}

Notifier *notifier() {
    static Notifier n;
    return &n;
}

Conv forUnit(const QString &unit, const QString &name) {
    return forUnit(unit, name, imperial());
}

Conv forUnit(const QString &unit, const QString &name, bool imp) {
    Conv same;
    same.unit = unit;
    if (!imp) return same;
    const QString u = unit.trimmed().toLower();
    static const QSet<QString> celsius = {"°c", "degc", "deg c", "c", "℃", "celsius", "deg_c", "degrees c"};
    if (celsius.contains(u)) return make("°F", 1.8, 32);
    if (u == "km/h" || u == "kph" || u == "kmh" || u == "km/hr") return make("mph", 0.621371);
    if (u == "km") return make("mi", 0.621371);
    if (u == "l" || u == "liter" || u == "liters" || u == "litre" || u == "litres") return make("gal", 0.264172);
    if (u == "l/h" || u == "l/hr") return make("gal/h", 0.264172);
    if (u == "nm" || u == "n·m" || u == "n.m" || u == "n-m" || u == "newtonmeters") return make("lb·ft", 0.737562);
    const bool vac = isVacuumSide(name);
    if (u == "kpa") return vac ? make("inHg", 0.2952998) : make("psi", 0.1450377);
    if (u == "bar") return vac ? make("inHg", 29.52998) : make("psi", 14.50377);
    if (u == "mbar" || u == "hpa") return vac ? make("inHg", 0.02952998) : make("psi", 0.01450377);
    if (u == "pa") return make("inHg", 0.0002952998);   // only small pressures (EVAP) come in Pa
    return same;
}

}  // namespace Units
