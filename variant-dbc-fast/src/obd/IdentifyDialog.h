#pragma once
#include <QDialog>
#include <QTimer>

class QComboBox;
class QLabel;
class QPushButton;
class QWidget;
class LiveDataDock;
class OeLibrary;
class VehicleId;

// "Identifying vehicle": reads the VIN, decodes make and year, applies a vehicle remembered for
// that VIN or asks for the model once, then runs Check supported — showing each step as it goes.
// Manual mode (Change…) skips the VIN and goes straight to choosing.
class IdentifyDialog : public QDialog {
    Q_OBJECT
public:
    IdentifyDialog(VehicleId *id, LiveDataDock *live, OeLibrary *oe, QWidget *parent = nullptr);

    void startAuto();     // VIN → vehicle → Check supported
    void startManual();   // choose make / model / year
    void startRecheck();  // ask the vehicle again what it supports (e.g. now the engine runs)

    QString lastVin() const { return m_vin; }

signals:
    void vehicleSet(const QString &text, const QString &vin);

private:
    void setStep(QLabel *label, const QString &text, bool done = false);
    void showChooser(const QString &make, int year, const QString &why);
    void fillModels();
    void applyChoice();
    void apply(const QString &make, const QString &model, int year);

    VehicleId *m_id;
    LiveDataDock *m_live;
    OeLibrary *m_oe;
    QString m_vin;
    QLabel *m_stepVin, *m_stepVehicle, *m_stepSupport;
    QWidget *m_chooser;
    QComboBox *m_make, *m_model, *m_year;
    QPushButton *m_useBtn, *m_closeBtn;
    QTimer m_autoClose;
    bool m_waitingForSupport = false;
};
