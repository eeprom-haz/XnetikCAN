#include "IdentifyDialog.h"
#include "LiveDataDock.h"
#include "VehicleId.h"
#include "../diag/OeLibrary.h"
#include "../diag/DiagLog.h"
#include <QComboBox>
#include <QDate>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

IdentifyDialog::IdentifyDialog(VehicleId *id, LiveDataDock *live, OeLibrary *oe, QWidget *parent)
    : QDialog(parent), m_id(id), m_live(live), m_oe(oe) {
    setWindowTitle("Identifying vehicle");
    setMinimumWidth(460);
    auto *lay = new QVBoxLayout(this);
    m_stepVin = new QLabel;
    m_stepVehicle = new QLabel;
    m_stepSupport = new QLabel;
    for (QLabel *l : {m_stepVin, m_stepVehicle, m_stepSupport}) {
        l->setWordWrap(true);
        lay->addWidget(l);
    }

    m_chooser = new QWidget;
    auto *form = new QFormLayout(m_chooser);
    form->setContentsMargins(0, 8, 0, 0);
    m_make = new QComboBox;
    m_make->addItems(m_oe->makes());
    m_model = new QComboBox;
    m_year = new QComboBox;
    m_year->addItem("Any year", 0);
    for (int y = QDate::currentDate().year() + 1; y >= 1996; --y) m_year->addItem(QString::number(y), y);
    form->addRow("Make:", m_make);
    form->addRow("Model:", m_model);
    form->addRow("Year:", m_year);
    lay->addWidget(m_chooser);
    m_chooser->hide();

    auto *buttons = new QHBoxLayout;
    m_useBtn = new QPushButton("Use this vehicle");
    m_closeBtn = new QPushButton("Close");
    buttons->addStretch();
    buttons->addWidget(m_useBtn);
    buttons->addWidget(m_closeBtn);
    lay->addLayout(buttons);
    m_useBtn->hide();

    connect(m_make, &QComboBox::currentIndexChanged, this, &IdentifyDialog::fillModels);
    connect(m_useBtn, &QPushButton::clicked, this, &IdentifyDialog::applyChoice);
    connect(m_closeBtn, &QPushButton::clicked, this, &QDialog::close);

    connect(m_id, &VehicleId::vinRead, this, [this](const QString &vin) {
        m_vin = vin;
        DiagLog::write("requests", "Auto ID: VIN " + vin);
        setStep(m_stepVin, "VIN: " + vin, true);
        const VehicleId::Saved s = VehicleId::saved(vin);
        if (s.found) {   // seen before: set it straight away
            apply(s.make, s.model, s.year);
            return;
        }
        QString make;
        for (const QString &candidate : VehicleId::makesForVin(vin)) {
            for (const QString &m : m_oe->makes())
                if (m.compare(candidate, Qt::CaseInsensitive) == 0 || m.startsWith(candidate, Qt::CaseInsensitive)) {
                    make = m;
                    break;
                }
            if (!make.isEmpty()) break;
        }
        const int year = VehicleId::yearForVin(vin);
        showChooser(make, year, make.isEmpty() ? "Make not recognised from the VIN — choose it:"
                                               : "New vehicle — choose the model (remembered for this VIN):");
    });
    connect(m_id, &VehicleId::vinFailed, this, [this](const QString &why) {
        DiagLog::write("requests", "Auto ID: VIN not read (" + why + ")");
        setStep(m_stepVin, "Couldn't read the VIN (" + why + ").");
        showChooser(m_live->make(), m_live->year(), "Choose the vehicle:");
    });
    connect(m_live, &LiveDataDock::supportProgress, this, [this](const QString &text) {
        if (m_waitingForSupport) setStep(m_stepSupport, text);
    });
    connect(m_live, &LiveDataDock::supportDone, this, [this](int standard, int manufacturer, const QString &problem) {
        if (!m_waitingForSupport) return;
        m_waitingForSupport = false;
        if (!problem.isEmpty()) {
            setStep(m_stepSupport, "Check supported: " + problem);
            return;
        }
        Q_UNUSED(standard)
        Q_UNUSED(manufacturer)
        // What this vehicle gives, in plain words (per module, sessions opened, what can't be read).
        setStep(m_stepSupport, m_live->capabilitySummary(), true);
        m_autoClose.start();
    });
    m_autoClose.setSingleShot(true);
    m_autoClose.setInterval(6000);   // long enough to read the summary
    connect(&m_autoClose, &QTimer::timeout, this, &QDialog::close);
}

void IdentifyDialog::setStep(QLabel *label, const QString &text, bool done) {
    label->setText((done ? QStringLiteral("✓  ") : QStringLiteral("•  ")) + text);
}

void IdentifyDialog::startAuto() {
    m_autoClose.stop();
    m_chooser->hide();
    m_useBtn->hide();
    m_stepVehicle->clear();
    m_stepSupport->clear();
    setStep(m_stepVin, "Reading VIN…");
    m_id->readVin();
}

void IdentifyDialog::startManual() {
    m_autoClose.stop();
    m_stepVin->setText(m_vin.isEmpty() ? QString() : "VIN: " + m_vin);
    m_stepSupport->clear();
    showChooser(m_live->make(), m_live->year(), "Choose the vehicle:");
    const int mi = m_model->findText(m_live->model());
    if (mi >= 0) m_model->setCurrentIndex(mi);
}

void IdentifyDialog::showChooser(const QString &make, int year, const QString &why) {
    setStep(m_stepVehicle, why);
    const int mk = m_make->findText(make);
    m_make->setCurrentIndex(mk >= 0 ? mk : 0);
    fillModels();
    const int yi = m_year->findData(year);
    m_year->setCurrentIndex(yi >= 0 ? yi : 0);
    m_chooser->show();
    m_useBtn->show();
    m_useBtn->setDefault(true);
}

void IdentifyDialog::fillModels() {
    m_model->clear();
    m_model->addItem("All models");
    m_model->addItems(m_oe->models(m_make->currentText()));
    // One model in the library for this make: nothing to choose.
    if (m_model->count() == 2) m_model->setCurrentIndex(1);
}

void IdentifyDialog::applyChoice() {
    const QString model = m_model->currentIndex() > 0 ? m_model->currentText() : QString();
    const int year = m_year->currentData().toInt();
    if (!m_vin.isEmpty()) VehicleId::save(m_vin, m_make->currentText(), model, year);
    m_chooser->hide();
    m_useBtn->hide();
    apply(m_make->currentText(), model, year);
}

void IdentifyDialog::startRecheck() {
    m_autoClose.stop();
    m_chooser->hide();
    m_useBtn->hide();
    m_stepVin->setText(m_vin.isEmpty() ? QString() : "VIN: " + m_vin);
    setStep(m_stepVehicle, "Vehicle: " + QString("%1 %2 %3").arg(m_live->year() > 0 ? QString::number(m_live->year())
                                                                                   : QString(),
                                                             m_live->make(), m_live->model()).simplified(), true);
    setStep(m_stepSupport, "Checking supported values again…");
    m_waitingForSupport = true;
    m_live->checkSupported(true);
}

void IdentifyDialog::apply(const QString &make, const QString &model, int year) {
    m_live->setVehicle(make, model, year, m_vin);
    const QString text = QString("%1 %2 %3").arg(year > 0 ? QString::number(year) : QString(), make, model).simplified();
    setStep(m_stepVehicle, "Vehicle: " + text, true);
    emit vehicleSet(text, m_vin);
    setStep(m_stepSupport, "Checking supported values…");
    m_waitingForSupport = true;
    m_live->checkSupported();
}
