#pragma once
#include <QDialog>
#include <QListWidget>
#include <QLabel>
#include <QLineEdit>

struct J2534Device {
    QString name;
    QString vendor;
    QString dllPath;
};

class ConnectDialog : public QDialog {
    Q_OBJECT
public:
    explicit ConnectDialog(QWidget *parent = nullptr);
    QString selectedDllPath() const;

private slots:
    void onSelectionChanged();
    void onBrowse();

private:
    QList<J2534Device> scanRegistry();

    QListWidget *m_list;
    QLabel *m_dllLabel;
    QList<J2534Device> m_devices;
};
