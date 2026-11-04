#include "DiagnosePanel.h"
#include "../can/CanBridge.h"
#include "../filter/FilterEngine.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSettings>
#include <QInputDialog>
#include <QScrollBar>
#include <QFont>

DiagnosePanel::DiagnosePanel(CanBridge *bridge, QWidget *parent)
    : QDockWidget("Diagnose (Claude)", parent)
    , m_bridge(bridge)
    , m_client(new ClaudeClient(this))
{
    setObjectName("DiagnosePanel");

    QSettings s;
    // Fall back to the environment variable if no key is stored yet.
    QString key = s.value("claude/api_key").toString();
    if (key.isEmpty()) key = QString::fromUtf8(qgetenv("ANTHROPIC_API_KEY"));
    m_client->setApiKey(key);

    auto *container = new QWidget;
    auto *vlay = new QVBoxLayout(container);
    vlay->setContentsMargins(4, 4, 4, 4);
    vlay->setSpacing(4);

    auto *top = new QHBoxLayout;
    auto *keyBtn = new QPushButton("API Key…");
    auto *newBtn = new QPushButton("New chat");
    for (QPushButton *b : {keyBtn, newBtn})
        b->setStyleSheet("QPushButton { background: transparent; color: #0078d4; border: none; padding: 2px 6px; min-width: 0; }"
                         "QPushButton:hover { text-decoration: underline; }");
    m_modelCombo = new QComboBox;
    for (const auto &m : ClaudeClient::models()) m_modelCombo->addItem(m.label, m.id);
    const int saved = m_modelCombo->findData(s.value("claude/model", ClaudeClient::models().first().id));
    m_modelCombo->setCurrentIndex(saved >= 0 ? saved : 0);
    m_client->setModel(m_modelCombo->currentData().toString());
    top->addWidget(keyBtn);
    top->addWidget(newBtn);
    top->addStretch();
    top->addWidget(m_modelCombo);
    vlay->addLayout(top);

    m_output = new QTextBrowser;
    m_output->setOpenExternalLinks(true);
    m_output->setFont(QFont("Segoe UI", 9));
    m_output->setPlaceholderText(
        "Ask about the vehicle, e.g. \"Why is the check engine light on?\" or "
        "\"Which of these readings look wrong for a warm idle?\". "
        "Your codes, readiness, live values and bus summary are sent with each question.");
    vlay->addWidget(m_output, 1);

    m_status = new QLabel;
    m_status->setStyleSheet("color:#777;");
    m_status->setWordWrap(true);
    vlay->addWidget(m_status);

    m_includeData = new QCheckBox("Include vehicle data with my question");
    m_includeData->setChecked(s.value("claude/includeData", true).toBool());
    vlay->addWidget(m_includeData);

    auto *hlay = new QHBoxLayout;
    m_input = new QLineEdit;
    m_input->setPlaceholderText("Describe the problem or ask a question…");
    m_sendBtn = new QPushButton("Send");
    hlay->addWidget(m_input);
    hlay->addWidget(m_sendBtn);
    vlay->addLayout(hlay);

    setWidget(container);

    m_renderTimer.setSingleShot(true);
    connect(&m_renderTimer, &QTimer::timeout, this, &DiagnosePanel::render);
    connect(m_sendBtn, &QPushButton::clicked, this, &DiagnosePanel::onSend);
    connect(m_input, &QLineEdit::returnPressed, this, &DiagnosePanel::onSend);
    connect(keyBtn, &QPushButton::clicked, this, &DiagnosePanel::showApiKeyDialog);
    connect(newBtn, &QPushButton::clicked, this, [this] {
        m_client->newConversation();
        m_transcript.clear();
        m_sessionUsd = 0;
        m_status->clear();
        render();
    });
    connect(m_modelCombo, &QComboBox::currentIndexChanged, this, [this] {
        m_client->setModel(m_modelCombo->currentData().toString());
        QSettings().setValue("claude/model", m_modelCombo->currentData().toString());
    });
    connect(m_includeData, &QCheckBox::toggled, this, [](bool on) { QSettings().setValue("claude/includeData", on); });

    connect(m_client, &ClaudeClient::thinking, this, &DiagnosePanel::onThinking);
    connect(m_client, &ClaudeClient::textDelta, this, &DiagnosePanel::onTextDelta);
    connect(m_client, &ClaudeClient::finished, this, &DiagnosePanel::onFinished);
    connect(m_client, &ClaudeClient::errorOccurred, this, &DiagnosePanel::onError);
    connect(m_client, &ClaudeClient::usage, this, &DiagnosePanel::onUsage);
}

QString DiagnosePanel::systemPrompt() {
    return QStringLiteral(
        "You are the diagnostic assistant inside XnetikCan, a Windows CAN bus and OBD-II tool used by a "
        "professional automotive technician who diagnoses vehicles for a living.\n\n"
        "Each question may include a <vehicle_data> block captured by the tool at that moment: the vehicle "
        "selected, connection state, trouble codes with descriptions, emissions readiness, the live values the "
        "technician is watching (with min/max since they started), and a per-message summary of bus traffic. "
        "Treat it as the current state of the car; earlier blocks in the conversation are older snapshots.\n\n"
        "How to answer:\n"
        "- Lead with the most likely causes, ranked, and the specific next test for each (what to measure, where, "
        "and what reading confirms or rules it out). Practical shop steps, not theory.\n"
        "- Tie conclusions to the actual values in the data and quote them. If a value looks implausible "
        "(e.g. -40 °C, 0 kPa oil pressure with the engine running) say it may be a bench setup, an unplugged "
        "sensor or a definition mismatch rather than a failed part.\n"
        "- If the data needed to decide isn't there, say exactly what to capture or which value to watch next.\n"
        "- Don't invent manufacturer specs, pin numbers or wire colours you aren't sure of; say to confirm them "
        "in service information.\n"
        "- Manufacturer data (Mode 22) comes from community definitions (OBDb) and may occasionally be scaled "
        "wrong; mention that when a reading is the deciding factor.\n"
        "- Keep it concise. Use short headings and lists. Plain language; hex only when it helps.");
}

void DiagnosePanel::appendMarkdown(const QString &md) {
    m_transcript += md;
    if (!m_renderTimer.isActive()) m_renderTimer.start(60);
}

void DiagnosePanel::render() {
    QScrollBar *sb = m_output->verticalScrollBar();
    const bool atBottom = sb->value() >= sb->maximum() - 4;
    m_output->setMarkdown(m_transcript);
    if (atBottom || m_responding) sb->setValue(sb->maximum());
}

void DiagnosePanel::setBusy(bool busy) {
    m_responding = busy;
    m_sendBtn->setText(busy ? "Stop" : "Send");
    m_modelCombo->setEnabled(!busy);
}

void DiagnosePanel::onSend() {
    if (m_responding) {
        m_client->cancel();
        return;
    }
    const QString q = m_input->text().trimmed();
    if (q.isEmpty()) return;
    if (!m_client->hasApiKey()) {
        showApiKeyDialog();
        if (!m_client->hasApiKey()) return;
    }
    m_input->clear();

    QString content = q;
    if (m_includeData->isChecked() && m_context)
        content = "<vehicle_data>\n" + m_context() + "</vehicle_data>\n\n" + q;

    if (!m_transcript.isEmpty()) m_transcript += "\n\n---\n\n";
    appendMarkdown("**You:** " + q + "\n\n**Claude:** ");
    m_status->setText("Sending…");
    setBusy(true);
    m_client->ask(systemPrompt(), content);
}

void DiagnosePanel::onThinking() {
    m_status->setText("Thinking…");
}

void DiagnosePanel::onTextDelta(const QString &text) {
    if (m_status->text() != "Answering…") m_status->setText("Answering…");
    appendMarkdown(text);
}

void DiagnosePanel::onFinished(const QString &note) {
    setBusy(false);
    if (!note.isEmpty()) appendMarkdown("\n\n*(" + note + ")*");
    m_renderTimer.stop();
    render();
}

void DiagnosePanel::onError(const QString &msg) {
    setBusy(false);
    appendMarkdown("\n\n**Error:** " + msg);
    m_status->setText("Not answered.");
    m_renderTimer.stop();
    render();
}

void DiagnosePanel::onUsage(int in, int cacheRead, int out, double usd) {
    m_sessionUsd += usd;
    m_status->setText(QString("Last answer: %1 tokens in (%2 from cache), %3 out · ≈ $%4 · this chat ≈ $%5")
                          .arg(in + cacheRead).arg(cacheRead).arg(out)
                          .arg(usd, 0, 'f', 3).arg(m_sessionUsd, 0, 'f', 2));
}

void DiagnosePanel::showApiKeyDialog() {
    bool ok;
    const QString key = QInputDialog::getText(
        this, "Anthropic API Key",
        "Paste your Claude API key (from console.anthropic.com → API keys).\n"
        "A Claude Pro subscription doesn't include API access; the API is billed separately per use.\n"
        "The key is stored for your Windows user only.",
        QLineEdit::Password, m_client->apiKey(), &ok);
    if (!ok) return;
    m_client->setApiKey(key);
    QSettings().setValue("claude/api_key", key.trimmed());
}
