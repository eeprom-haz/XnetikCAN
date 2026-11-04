#pragma once
#include <QObject>
#include <QString>
#include <QStringList>

class CanBridge;
class IsoTp;

// Reads the VIN with the standard OBD request (Mode 09 PID 02) and decodes what can be decoded
// offline: the make (from the manufacturer code, the first 3 characters) and the model year
// (10th character). The model isn't reliably in the VIN, so it's asked once and remembered.
class VehicleId : public QObject {
    Q_OBJECT
public:
    explicit VehicleId(CanBridge *bridge, QObject *parent = nullptr);

    void readVin();   // emits vinRead or vinFailed

    // Make names as OBDb calls them (first that exists in the library wins), "" if unknown.
    static QStringList makesForVin(const QString &vin);
    static int yearForVin(const QString &vin);   // 0 if not decodable

    // Vehicles set up before, remembered by VIN.
    struct Saved { QString make, model; int year = 0; bool found = false; };
    static Saved saved(const QString &vin);
    static void save(const QString &vin, const QString &make, const QString &model, int year);

signals:
    void vinRead(const QString &vin);
    void vinFailed(const QString &why);

private:
    void send();

    CanBridge *m_bridge;
    IsoTp *m_tp;
    int m_tries = 0;
};
