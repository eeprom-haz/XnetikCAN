#include "DidScanDialog.h"
#include "../can/CanBridge.h"
#include "../diag/IsoTp.h"
#include "../diag/EnhancedLibrary.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGridLayout>
#include <QHeaderView>
#include <QDialogButtonBox>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QMessageBox>

static QString hexBytes(const QByteArray &b) { return QString::fromLatin1(b.toHex(' ').toUpper()); }

DidScanDialog::DidScanDialog(CanBridge *bridge, EnhancedLibrary *library, QWidget *parent)
    : QDialog(parent), m_bridge(bridge), m_tp(new IsoTp(bridge, this)), m_library(library) {
    setWindowTitle("Find enhanced data (Mode 22)");
    resize(820, 620);
    auto *v = new QVBoxLayout(this);

    auto *intro = new QLabel(
        "Asks the module which manufacturer data items it has. This only <b>reads</b> — "
        "nothing is changed in the module. A full scan takes a while; you can stop at any time.<br>"
        "Then tick <b>Watch live</b>, change something on the vehicle (temperature, throttle, key position) "
        "and see which items move. Select one and click <b>Add to library</b> to give it a name and scaling.");
    intro->setWordWrap(true);
    v->addWidget(intro);

    auto *grid = new QGridLayout;
    m_module = new QComboBox;
    m_module->addItem("Engine (ECM)", QStringList{"7E0", "7E8"});
    m_module->addItem("Transmission (TCM)", QStringList{"7E1", "7E9"});
    m_module->addItem("Custom", QStringList{});
    m_reqEdit = new QLineEdit("7E0");
    m_respEdit = new QLineEdit("7E8");
    m_fromEdit = new QLineEdit("0000");
    m_toEdit = new QLineEdit("FFFF");
    for (QLineEdit *e : {m_reqEdit, m_respEdit, m_fromEdit, m_toEdit}) e->setMaximumWidth(80);
    m_reqEdit->setEnabled(false);
    m_respEdit->setEnabled(false);
    grid->addWidget(new QLabel("Module:"), 0, 0);
    grid->addWidget(m_module, 0, 1);
    grid->addWidget(new QLabel("Request / reply ID:"), 0, 2);
    grid->addWidget(m_reqEdit, 0, 3);
    grid->addWidget(m_respEdit, 0, 4);
    grid->addWidget(new QLabel("Scan from:"), 1, 0);
    grid->addWidget(m_fromEdit, 1, 1, Qt::AlignLeft);
    grid->addWidget(new QLabel("to:"), 1, 2, Qt::AlignRight);
    grid->addWidget(m_toEdit, 1, 3);
    grid->setColumnStretch(5, 1);
    v->addLayout(grid);

    auto *row = new QHBoxLayout;
    m_startBtn = new QPushButton("▶  Start scan");
    m_watch = new QCheckBox("Watch live");
    m_watch->setEnabled(false);
    m_progress = new QProgressBar;
    m_progress->setTextVisible(true);
    row->addWidget(m_startBtn);
    row->addWidget(m_watch);
    row->addWidget(m_progress, 1);
    v->addLayout(row);
    m_status = new QLabel("Ready");
    m_status->setStyleSheet("color:#666;");
    v->addWidget(m_status);

    m_table = new QTableWidget(0, 4);
    m_table->setHorizontalHeaderLabels({"Data ID", "Bytes", "Raw data", "Changing"});
    m_table->verticalHeader()->hide();
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_table->setFont(QFont("Consolas", 9));
    v->addWidget(m_table, 1);

    auto *bottom = new QHBoxLayout;
    m_addBtn = new QPushButton("Add to library…");
    m_addBtn->setEnabled(false);
    auto *close = new QPushButton("Close");
    bottom->addWidget(m_addBtn);
    bottom->addStretch();
    bottom->addWidget(close);
    v->addLayout(bottom);

    m_gap.setSingleShot(true);
    connect(&m_gap, &QTimer::timeout, this, &DidScanDialog::next);
    connect(m_tp, &IsoTp::response, this, &DidScanDialog::onResponse);
    connect(m_tp, &IsoTp::timedOut, this, &DidScanDialog::onTimeout);
    connect(m_startBtn, &QPushButton::clicked, this, &DidScanDialog::onStartStop);
    connect(m_watch, &QCheckBox::toggled, this, &DidScanDialog::onWatchToggled);
    connect(m_addBtn, &QPushButton::clicked, this, &DidScanDialog::onAddToLibrary);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    connect(m_table, &QTableWidget::itemSelectionChanged, this, [this] {
        m_addBtn->setEnabled(!m_table->selectedItems().isEmpty());
    });
    connect(m_module, &QComboBox::currentIndexChanged, this, [this] {
        const QStringList ids = m_module->currentData().toStringList();
        const bool custom = ids.isEmpty();
        m_reqEdit->setEnabled(custom);
        m_respEdit->setEnabled(custom);
        if (!custom) { m_reqEdit->setText(ids[0]); m_respEdit->setText(ids[1]); }
    });
    connect(m_bridge, &CanBridge::stateChanged, this, [this](bool connected) {
        if (!connected) { stopAll(); m_status->setText("Disconnected"); }
    });
}

DidScanDialog::~DidScanDialog() { stopAll(); }

quint32 DidScanDialog::requestId() const { return m_reqEdit->text().toUInt(nullptr, 16); }
quint32 DidScanDialog::responseId() const { return m_respEdit->text().toUInt(nullptr, 16); }

void DidScanDialog::stopAll() {
    m_mode = Mode::Idle;
    m_gap.stop();
    m_tp->cancel();
    m_startBtn->setText("▶  Start scan");
}

void DidScanDialog::onStartStop() {
    if (m_mode == Mode::Scanning) {
        stopAll();
        m_status->setText(QString("Stopped at 0x%1 — found %2")
                              .arg(QString::number(m_cur, 16).rightJustified(4, '0').toUpper())
                              .arg(m_found.size()));
        m_watch->setEnabled(!m_found.isEmpty());
        return;
    }
    bool ok1, ok2;
    m_from = m_fromEdit->text().toUInt(&ok1, 16);
    m_to = m_toEdit->text().toUInt(&ok2, 16);
    if (!ok1 || !ok2 || m_from > m_to || m_to > 0xFFFF || !requestId() || !responseId()) {
        QMessageBox::warning(this, "Scan", "Check the IDs and range (hex, 0000–FFFF).");
        return;
    }
    m_watch->setChecked(false);
    m_watch->setEnabled(false);
    m_cur = m_from;
    m_progress->setRange(0, int(m_to - m_from + 1));
    m_progress->setValue(0);
    m_clock.start();
    m_mode = Mode::Scanning;
    m_startBtn->setText("■  Stop scan");
    next();
}

void DidScanDialog::send(quint16 did) {
    QByteArray req(3, '\0');
    req[0] = 0x22;
    req[1] = char(did >> 8);
    req[2] = char(did & 0xFF);
    m_pending = did;
    if (!m_tp->request(requestId(), responseId(), responseId(), req, 120))
        m_gap.start(20);  // bus busy or disconnected; retry shortly
}

void DidScanDialog::next() {
    if (m_mode == Mode::Scanning) {
        if (m_cur > m_to) {
            stopAll();
            m_progress->setValue(m_progress->maximum());
            m_status->setText(QString("Scan complete — found %1 data items. Tick “Watch live” to see them change.")
                                  .arg(m_found.size()));
            m_watch->setEnabled(!m_found.isEmpty());
            return;
        }
        send(quint16(m_cur));
    } else if (m_mode == Mode::Watching) {
        if (m_found.isEmpty()) return;
        if (m_watchPos >= m_found.size()) m_watchPos = 0;
        send(m_found[m_watchPos++].did);
    }
}

void DidScanDialog::onResponse(quint32, const QByteArray &p) {
    const bool positive = p.size() >= 3 && quint8(p[0]) == 0x62 &&
                          ((quint8(p[1]) << 8) | quint8(p[2])) == m_pending;
    if (positive) {
        const QByteArray data = p.mid(3);
        auto it = m_index.find(m_pending);
        if (it == m_index.end()) {
            Found f{m_pending, data, QVector<int>(data.size(), 0), m_table->rowCount()};
            m_table->insertRow(f.row);
            m_index.insert(f.did, m_found.size());
            m_found.append(f);
            updateRow(m_found.last());
        } else {
            Found &f = m_found[*it];
            if (f.changes.size() < data.size()) f.changes.resize(data.size());
            for (int i = 0; i < data.size(); ++i)
                if (i >= f.data.size() || f.data[i] != data[i]) ++f.changes[i];
            f.data = data;
            updateRow(f);
        }
    }
    if (m_mode == Mode::Scanning) {
        ++m_cur;
        const int done = int(m_cur - m_from);
        m_progress->setValue(done);
        if (done % 64 == 0) {
            const double perItem = m_clock.elapsed() / double(done);
            const int left = int((m_to - m_cur + 1) * perItem / 1000.0);
            m_status->setText(QString("Scanning 0x%1 … found %2 · about %3 min %4 s left")
                                  .arg(QString::number(m_cur, 16).rightJustified(4, '0').toUpper()).arg(m_found.size())
                                  .arg(left / 60).arg(left % 60));
        }
    }
    m_gap.start(2);
}

void DidScanDialog::onTimeout() {
    onResponse(0, {});  // no answer = not supported; move on
}

void DidScanDialog::updateRow(Found &f) {
    auto set = [&](int col, const QString &text) {
        QTableWidgetItem *it = m_table->item(f.row, col);
        if (!it) { it = new QTableWidgetItem; m_table->setItem(f.row, col, it); }
        it->setText(text);
        return it;
    };
    set(0, QString("%1").arg(f.did, 4, 16, QChar('0')).toUpper())->setData(Qt::UserRole, f.did);
    set(1, QString::number(f.data.size()));
    set(2, hexBytes(f.data));
    QStringList moving;
    for (int i = 0; i < f.changes.size(); ++i)
        if (f.changes[i] > 0) moving << QString::number(i);
    QTableWidgetItem *ch = set(3, moving.isEmpty() ? QString() : "byte " + moving.join(", "));
    const QColor bg = moving.isEmpty() ? QColor(Qt::transparent) : QColor("#fff4ce");
    for (int c = 0; c < 4; ++c) m_table->item(f.row, c)->setBackground(bg);
    Q_UNUSED(ch);
}

void DidScanDialog::onWatchToggled(bool on) {
    if (on) {
        m_mode = Mode::Watching;
        m_watchPos = 0;
        m_startBtn->setEnabled(false);
        m_status->setText("Watching — change something on the vehicle and look for highlighted rows");
        next();
    } else if (m_mode == Mode::Watching) {
        stopAll();
        m_startBtn->setEnabled(true);
        m_status->setText("Watching stopped");
    }
}

void DidScanDialog::onAddToLibrary() {
    const auto sel = m_table->selectedItems();
    if (sel.isEmpty()) return;
    const quint16 did = quint16(m_table->item(sel.first()->row(), 0)->data(Qt::UserRole).toUInt());
    const int idx = m_index.value(did, -1);
    if (idx < 0) return;

    QDialog dlg(this);
    dlg.setWindowTitle(QString("Add data ID %1").arg(did, 4, 16, QChar('0')).toUpper());
    auto *form = new QFormLayout(&dlg);
    auto *name = new QLineEdit;
    name->setPlaceholderText("e.g. Knock retard");
    auto *unit = new QLineEdit;
    unit->setPlaceholderText("e.g. °, °C, kPa, %");
    auto *start = new QSpinBox;
    start->setRange(0, qMax(0, int(m_found[idx].data.size()) - 1));
    auto *bytes = new QSpinBox;
    bytes->setRange(1, 4);
    bytes->setValue(qMin(2, int(m_found[idx].data.size())));
    auto *isSigned = new QCheckBox("Value can be negative (signed)");
    auto *factor = new QDoubleSpinBox;
    factor->setDecimals(6);
    factor->setRange(-1e6, 1e6);
    factor->setValue(1.0);
    auto *offset = new QDoubleSpinBox;
    offset->setDecimals(3);
    offset->setRange(-1e6, 1e6);
    auto *rmin = new QDoubleSpinBox;
    auto *rmax = new QDoubleSpinBox;
    for (QDoubleSpinBox *b : {rmin, rmax}) { b->setRange(-1e6, 1e6); b->setDecimals(1); }
    auto *preview = new QLabel;
    preview->setStyleSheet("font-weight:600;");

    form->addRow("Name:", name);
    form->addRow("Unit:", unit);
    form->addRow("Start at byte:", start);
    form->addRow("Number of bytes:", bytes);
    form->addRow("", isSigned);
    form->addRow("Multiply by:", factor);
    form->addRow("Then add:", offset);
    form->addRow("Graph range from:", rmin);
    form->addRow("Graph range to:", rmax);
    form->addRow("Value now:", preview);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    form->addRow(bb);

    auto build = [&] {
        EnhancedDid d;
        d.module = m_module->currentIndex() == 2 ? "Custom" : m_module->currentText();
        d.requestId = requestId();
        d.responseId = responseId();
        d.did = did;
        d.name = name->text().trimmed();
        d.unit = unit->text().trimmed();
        d.startByte = start->value();
        d.bytes = bytes->value();
        d.isSigned = isSigned->isChecked();
        d.factor = factor->value();
        d.offset = offset->value();
        d.rangeMin = rmin->value();
        d.rangeMax = rmax->value();
        return d;
    };
    auto updatePreview = [&] {
        double v;
        if (build().decode(m_found[idx].data, &v))
            preview->setText(QString::number(v, 'f', 2) + ' ' + unit->text());
        else
            preview->setText("(not enough bytes)");
    };
    for (QSpinBox *s : {start, bytes}) connect(s, &QSpinBox::valueChanged, &dlg, updatePreview);
    for (QDoubleSpinBox *s : {factor, offset}) connect(s, &QDoubleSpinBox::valueChanged, &dlg, updatePreview);
    connect(isSigned, &QCheckBox::toggled, &dlg, updatePreview);
    connect(unit, &QLineEdit::textChanged, &dlg, updatePreview);
    QTimer live;  // keep the preview current while watching
    connect(&live, &QTimer::timeout, &dlg, updatePreview);
    live.start(250);
    updatePreview();
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    connect(bb, &QDialogButtonBox::accepted, &dlg, [&] {
        if (name->text().trimmed().isEmpty()) {
            QMessageBox::warning(&dlg, "Add to library", "Give it a name first.");
            return;
        }
        dlg.accept();
    });
    if (dlg.exec() != QDialog::Accepted) return;

    QString err;
    if (!m_library->addUserDid(build(), &err))
        QMessageBox::critical(this, "Add to library", "Could not save: " + err);
    else
        m_status->setText(QString("Saved “%1” — it is now in Live Data under Enhanced").arg(name->text()));
}
