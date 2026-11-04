#pragma once
#include <QDockWidget>
#include <QTextBrowser>
#include <QLineEdit>
#include <QPushButton>
#include <QComboBox>
#include <QCheckBox>
#include <QLabel>
#include <QTimer>
#include <functional>
#include "ClaudeClient.h"

class CanBridge;
struct CanFilter;

// Chat with Claude about the vehicle on the bench. Each question carries a
// snapshot of what the app knows right now (vehicle, codes, readiness, live
// values, bus summary), supplied by MainWindow through the context provider.
class DiagnosePanel : public QDockWidget {
    Q_OBJECT
public:
    explicit DiagnosePanel(CanBridge *bridge, QWidget *parent = nullptr);

    void setFilter(const CanFilter *filter) { m_filter = filter; }
    void setContextProvider(std::function<QString()> provider) { m_context = std::move(provider); }

private slots:
    void onSend();
    void onThinking();
    void onTextDelta(const QString &text);
    void onFinished(const QString &note);
    void onError(const QString &msg);
    void onUsage(int in, int cacheRead, int out, double usd);
    void showApiKeyDialog();
    void render();

private:
    static QString systemPrompt();
    void appendMarkdown(const QString &md);
    void setBusy(bool busy);

    CanBridge *m_bridge;
    const CanFilter *m_filter = nullptr;
    ClaudeClient *m_client;
    std::function<QString()> m_context;

    QTextBrowser *m_output;
    QLineEdit *m_input;
    QPushButton *m_sendBtn;
    QComboBox *m_modelCombo;
    QCheckBox *m_includeData;
    QLabel *m_status;
    QString m_transcript;       // markdown of the whole chat
    QTimer m_renderTimer;
    bool m_responding = false;
    double m_sessionUsd = 0;
};
