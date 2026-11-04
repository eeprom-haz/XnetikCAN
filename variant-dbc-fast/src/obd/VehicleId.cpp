#include "VehicleId.h"
#include "../diag/IsoTp.h"
#include "../can/CanBridge.h"
#include <QHash>
#include <QSettings>
#include <QTimer>

VehicleId::VehicleId(CanBridge *bridge, QObject *parent)
    : QObject(parent), m_bridge(bridge), m_tp(new IsoTp(bridge, this)) {
    connect(m_tp, &IsoTp::response, this, [this](quint32, const QByteArray &p) {
        // 49 02 01 + 17 characters (some ECUs pad with zeros or omit the 01).
        QString vin;
        for (char c : p.mid(2)) {
            const QChar ch = QChar::fromLatin1(c);
            if (ch.isLetterOrNumber()) vin += ch.toUpper();
        }
        if (vin.size() > 17) vin = vin.right(17);
        if (quint8(p[0]) == 0x49 && vin.size() == 17) emit vinRead(vin);
        else emit vinFailed("the engine computer sent an unreadable VIN");
    });
    connect(m_tp, &IsoTp::timedOut, this, [this] {
        // The code read on connect may still be talking to the ECU: keep trying for ~10 s.
        if (++m_tries < 8) QTimer::singleShot(500, this, &VehicleId::send);
        else emit vinFailed("no answer to the VIN request (Mode 09)");
    });
}

void VehicleId::readVin() {
    m_tries = 0;
    send();
}

void VehicleId::send() {
    if (!m_bridge->isConnected()) { emit vinFailed("not connected"); return; }
    if (!m_tp->request(0x7DF, 0x7E8, 0x7EF, QByteArray::fromHex("0902"), 800))
        QTimer::singleShot(200, this, &VehicleId::send);   // busy: shortly again
}

QStringList VehicleId::makesForVin(const QString &vin) {
    if (vin.size() < 3) return {};
    // Manufacturer codes (WMI) of the makes in the library; longest match wins.
    static const QHash<QString, QStringList> wmi = {
        // Ford / Lincoln / Mercury
        {"1FA", {"Ford"}}, {"1FB", {"Ford"}}, {"1FC", {"Ford"}}, {"1FD", {"Ford"}}, {"1FM", {"Ford"}}, {"1FT", {"Ford"}},
        {"1FV", {"Ford"}}, {"2FA", {"Ford"}}, {"2FM", {"Ford"}}, {"2FT", {"Ford"}}, {"3FA", {"Ford"}}, {"3FM", {"Ford"}},
        {"3FT", {"Ford"}}, {"NM0", {"Ford"}}, {"WF0", {"Ford"}}, {"MAJ", {"Ford"}}, {"1LN", {"Lincoln"}},
        {"5LM", {"Lincoln"}}, {"2LM", {"Lincoln"}}, {"3LN", {"Lincoln"}}, {"1ME", {"Mercury"}},
        // GM
        {"1G1", {"Chevrolet"}}, {"1GC", {"Chevrolet"}}, {"1GN", {"Chevrolet"}}, {"1GB", {"Chevrolet"}},
        {"2G1", {"Chevrolet"}}, {"2GC", {"Chevrolet"}}, {"2GN", {"Chevrolet"}}, {"3G1", {"Chevrolet"}},
        {"3GC", {"Chevrolet"}}, {"3GN", {"Chevrolet"}}, {"KL7", {"Chevrolet"}}, {"KL8", {"Chevrolet"}},
        {"1GT", {"GMC"}}, {"1GK", {"GMC"}}, {"2GT", {"GMC"}}, {"2GK", {"GMC"}}, {"3GT", {"GMC"}}, {"3GK", {"GMC"}},
        {"1G4", {"Buick"}}, {"2G4", {"Buick"}}, {"5GA", {"Buick"}}, {"KL4", {"Buick"}}, {"LRB", {"Buick"}},
        {"1G6", {"Cadillac"}}, {"1GY", {"Cadillac"}}, {"2G6", {"Cadillac"}}, {"1G2", {"Pontiac"}}, {"6G2", {"Pontiac"}},
        // Stellantis (US)
        {"1C3", {"Chrysler", "Dodge"}}, {"2C3", {"Chrysler", "Dodge"}}, {"2C4", {"Chrysler", "Dodge"}},
        {"1C4", {"Jeep", "Chrysler", "Dodge"}}, {"1J4", {"Jeep"}}, {"1J8", {"Jeep"}}, {"3C4", {"Jeep", "Chrysler"}},
        {"ZAC", {"Jeep"}}, {"1C6", {"Ram"}}, {"3C6", {"Ram"}}, {"3C7", {"Ram"}}, {"1D7", {"Ram", "Dodge"}},
        {"3D7", {"Ram", "Dodge"}}, {"2B3", {"Dodge"}}, {"1B3", {"Dodge"}}, {"2D4", {"Dodge"}},
        // Asian / European
        {"JT", {"Toyota", "Lexus"}}, {"4T", {"Toyota"}}, {"5T", {"Toyota"}}, {"2T", {"Toyota", "Lexus"}},
        {"JTH", {"Lexus"}}, {"JTJ", {"Lexus"}}, {"2T2", {"Lexus"}}, {"58A", {"Lexus"}},
        {"1HG", {"Honda"}}, {"2HG", {"Honda"}}, {"JHM", {"Honda"}}, {"5FN", {"Honda"}}, {"5J6", {"Honda"}},
        {"19U", {"Acura"}}, {"JH4", {"Acura"}}, {"5J8", {"Acura"}},
        {"1N4", {"Nissan"}}, {"1N6", {"Nissan"}}, {"3N1", {"Nissan"}}, {"3N6", {"Nissan"}}, {"5N1", {"Nissan"}},
        {"JN1", {"Nissan"}}, {"JN8", {"Nissan", "INFINITI"}}, {"JNK", {"INFINITI"}}, {"5N3", {"INFINITI"}},
        {"KMH", {"Hyundai"}}, {"5NP", {"Hyundai"}}, {"5NM", {"Hyundai"}}, {"KM8", {"Hyundai"}}, {"KMU", {"Genesis"}},
        {"KNA", {"Kia"}}, {"KND", {"Kia"}}, {"5XY", {"Kia"}}, {"3KP", {"Kia"}},
        {"JF1", {"Subaru"}}, {"JF2", {"Subaru"}}, {"4S3", {"Subaru"}}, {"4S4", {"Subaru"}},
        {"JM1", {"Mazda"}}, {"JM3", {"Mazda"}}, {"4F2", {"Mazda"}}, {"3MZ", {"Mazda"}},
        {"JA3", {"Mitsubishi"}}, {"JA4", {"Mitsubishi"}}, {"4A3", {"Mitsubishi"}},
        {"WVW", {"Volkswagen", "VW"}}, {"WVG", {"Volkswagen", "VW"}}, {"3VW", {"Volkswagen", "VW"}},
        {"1VW", {"Volkswagen", "VW"}}, {"WAU", {"Audi"}}, {"WA1", {"Audi"}},
        {"WBA", {"BMW"}}, {"WBS", {"BMW"}}, {"5UX", {"BMW"}}, {"WMW", {"MINI"}},
        {"WDD", {"Mercedes-Benz", "Mercedes"}}, {"WDC", {"Mercedes-Benz", "Mercedes"}}, {"4JG", {"Mercedes-Benz", "Mercedes"}},
        {"55S", {"Mercedes-Benz", "Mercedes"}}, {"W1K", {"Mercedes-Benz", "Mercedes"}}, {"W1N", {"Mercedes-Benz", "Mercedes"}},
        {"WP0", {"Porsche"}}, {"WP1", {"Porsche"}}, {"YV1", {"Volvo"}}, {"YV4", {"Volvo"}},
        {"SAL", {"Land Rover", "Landrover"}}, {"SAJ", {"Jaguar"}},
        {"5YJ", {"Tesla"}}, {"7SA", {"Tesla"}}, {"LRW", {"Tesla"}}, {"7FC", {"Rivian"}}, {"7PD", {"Rivian"}},
        {"ZFA", {"FIAT"}}, {"3C3", {"FIAT"}},
    };
    const QString up = vin.toUpper();
    if (wmi.contains(up.left(3))) return wmi.value(up.left(3));
    return wmi.value(up.left(2));
}

int VehicleId::yearForVin(const QString &vin) {
    if (vin.size() < 10) return 0;
    static const QString codes = "ABCDEFGHJKLMNPRSTVWXY123456789";   // 2010..2039 cycle (A=2010)
    const int i = codes.indexOf(vin.at(9).toUpper());
    if (i < 0) return 0;
    // Letters A–Y cover 1980–2000 and again 2010–2030, digits 2001–2009 and 2031–2039:
    // pick the one closest to now that isn't in the future.
    const int a = 2010 + i, b = a - 30;
    return a <= 2027 ? a : b;
}

VehicleId::Saved VehicleId::saved(const QString &vin) {
    const QStringList v = QSettings().value("vehicles/" + vin).toStringList();
    Saved s;
    if (v.size() == 3) {
        s.make = v[0];
        s.model = v[1];
        s.year = v[2].toInt();
        s.found = !s.make.isEmpty();
    }
    return s;
}

void VehicleId::save(const QString &vin, const QString &make, const QString &model, int year) {
    QSettings().setValue("vehicles/" + vin, QStringList{make, model, QString::number(year)});
}
