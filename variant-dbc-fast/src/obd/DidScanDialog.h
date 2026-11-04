#pragma once
#include <QDialog>
#include <QTableWidget>
#include <QComboBox>
#include <QLineEdit>
#include <QPushButton>
#include <QProgressBar>
#include <QLabel>
#include <QCheckBox>
#include <QElapsedTimer>
#include <QTimer>
#include <QHash>
#include <QVector>

class CanBridge;
class IsoTp;
class EnhancedLibrary;

// Finds which manufacturer data identifiers (UDS 0x22 DIDs) a module answers,
// lets the tech watch them change, and saves named/scaled ones to the library.
// Read-only: it only ever sends ReadDataByIdentifier requests.
class DidScanDialog : public QDialog {
    Q_OBJECT
public:
    DidScanDialog(CanBridge *bridge, EnhancedLibrary *library, QWidget *parent = nullptr);
    ~DidScanDialog() override;

private slots:
    void onStartStop();
    void onResponse(quint32 rxId, const QByteArray &payload);
    void onTimeout();
    void next();
    void onWatchToggled(bool on);
    void onAddToLibrary();

private:
    struct Found {
        quint16 did;
        QByteArray data;
        QVector<int> changes;  // per byte: how many times it changed while watched
        int row;
    };
    enum class Mode { Idle, Scanning, Watching };

    quint32 requestId() const;
    quint32 responseId() const;
    void send(quint16 did);
    void updateRow(Found &f);
    void stopAll();

    CanBridge *m_bridge;
    IsoTp *m_tp;
    EnhancedLibrary *m_library;

    QComboBox *m_module;
    QLineEdit *m_reqEdit, *m_respEdit, *m_fromEdit, *m_toEdit;
    QPushButton *m_startBtn, *m_addBtn;
    QCheckBox *m_watch;
    QProgressBar *m_progress;
    QLabel *m_status;
    QTableWidget *m_table;
    QTimer m_gap;
    QElapsedTimer m_clock;

    Mode m_mode = Mode::Idle;
    quint32 m_cur = 0, m_from = 0, m_to = 0;
    quint16 m_pending = 0;
    QHash<quint16, int> m_index;   // did -> m_found index
    QVector<Found> m_found;
    int m_watchPos = 0;
};
