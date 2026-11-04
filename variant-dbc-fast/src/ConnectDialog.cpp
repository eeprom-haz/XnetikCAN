#include "ConnectDialog.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QPushButton>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QSettings>
#include <QFileInfo>
#include <QSysInfo>
#include <QComboBox>
#include <QCheckBox>
#include <QFormLayout>
#include "can/BridgeDevice.h"
#include "can/GsUsbDevice.h"

ConnectDialog::ConnectDialog(QWidget *parent) : QDialog(parent) {
    setWindowTitle("Connect to adapter");
    setMinimumWidth(480);

    auto *vlay = new QVBoxLayout(this);

    // Device list
    auto *group = new QGroupBox("Installed J2534 devices");
    auto *glay = new QVBoxLayout(group);

    m_list = new QListWidget;
    m_list->setAlternatingRowColors(true);
    m_list->setMinimumHeight(160);
    glay->addWidget(m_list);

    m_dllLabel = new QLabel("No device selected");
    m_dllLabel->setWordWrap(true);
    m_dllLabel->setStyleSheet("color: #666; font-size: 10px; padding: 2px;");
    glay->addWidget(m_dllLabel);

    vlay->addWidget(group);

    // Browse fallback
    auto *browseBtn = new QPushButton("Browse for DLL manually…");
    browseBtn->setStyleSheet(
        "QPushButton { background: transparent; color: #0078d4; border: none; "
        "text-decoration: underline; padding: 4px; } "
        "QPushButton:hover { color: #005a9e; }");
    vlay->addWidget(browseBtn, 0, Qt::AlignLeft);

    auto *bbox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    bbox->button(QDialogButtonBox::Ok)->setText("Connect");
    bbox->button(QDialogButtonBox::Ok)->setEnabled(false);
    vlay->addWidget(bbox);

    connect(bbox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bbox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(browseBtn, &QPushButton::clicked, this, &ConnectDialog::onBrowse);
    connect(m_list, &QListWidget::currentRowChanged, this, &ConnectDialog::onSelectionChanged);
    connect(m_list, &QListWidget::currentRowChanged, bbox->button(QDialogButtonBox::Ok),
            [bbox](int row) { bbox->button(QDialogButtonBox::Ok)->setEnabled(row >= 0); });
    connect(m_list, &QListWidget::itemDoubleClicked, this, &QDialog::accept);

    // Populate
    m_devices = scanRegistry();
    if (m_devices.isEmpty()) {
        auto *item = new QListWidgetItem("No J2534 devices found in registry");
        item->setFlags(Qt::NoItemFlags);
        m_list->addItem(item);
    } else {
        for (const J2534Device &dev : m_devices) {
            QString label = dev.vendor.isEmpty()
                ? dev.name
                : QString("%1  —  %2").arg(dev.vendor, dev.name);
            m_list->addItem(label);
        }
        m_list->setCurrentRow(0);
    }
    // The SH-C31G sniffer is NOT here — it's the "Snort" button, independent of J2534 diagnostics.
}

QString ConnectDialog::selectedDllPath() const {
    int row = m_list->currentRow();
    if (row >= 0 && row < m_devices.size())
        return m_devices[row].dllPath;
    return {};
}

void ConnectDialog::onSelectionChanged() {
    int row = m_list->currentRow();
    if (row >= 0 && row < m_devices.size()) {
        const QString &path = m_devices[row].dllPath;
        const int bits = BridgeDevice::dllBitness(path);
        QString note;
        if (bits == 32 && QSysInfo::WordSize == 64)
            note = "  ·  32-bit driver (runs via J2534Bridge)";
        else if (bits)
            note = QString("  ·  %1-bit driver").arg(bits);
        else
            note = "  ·  DLL not found or unreadable";
        m_dllLabel->setText(path + note);
    } else
        m_dllLabel->setText("No device selected");
}

void ConnectDialog::onBrowse() {
    QSettings s;
    QString last = s.value("can/dll_path").toString();
    QString path = QFileDialog::getOpenFileName(
        this, "Select J2534 DLL", last, "DLL files (*.dll)");
    if (path.isEmpty()) return;

    // Add as a custom entry
    J2534Device dev;
    dev.name = QFileInfo(path).fileName();
    dev.dllPath = path;
    m_devices.append(dev);
    m_list->addItem(dev.name + "  (manual)");
    m_list->setCurrentRow(m_devices.size() - 1);
}

QList<J2534Device> ConnectDialog::scanRegistry() {
    QList<J2534Device> devices;

    // J2534 v04.04 devices register under these two paths (64-bit and 32-bit).
    const QStringList regPaths = {
        "HKEY_LOCAL_MACHINE\\SOFTWARE\\PassThruSupport.04.04",
        "HKEY_LOCAL_MACHINE\\SOFTWARE\\WOW6432Node\\PassThruSupport.04.04"
    };

    for (const QString &regPath : regPaths) {
        QSettings reg(regPath, QSettings::NativeFormat);
        for (const QString &group : reg.childGroups()) {
            reg.beginGroup(group);
            J2534Device dev;
            dev.name    = reg.value("Name", group).toString();
            dev.vendor  = reg.value("Vendor", "").toString();
            dev.dllPath = reg.value("FunctionLibrary", "").toString();
            reg.endGroup();

            if (dev.dllPath.isEmpty()) continue;

            // Skip duplicates (same DLL can appear in both registry paths).
            bool dupe = std::any_of(devices.begin(), devices.end(),
                [&](const J2534Device &d) {
                    return d.dllPath.compare(dev.dllPath, Qt::CaseInsensitive) == 0;
                });
            if (!dupe) devices.append(dev);
        }
    }
    return devices;
}
