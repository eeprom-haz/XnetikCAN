#pragma once
#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>

// Streaming client for the Claude Messages API (raw HTTPS; there is no C++ SDK).
// Keeps the whole conversation and echoes every assistant content block back
// unchanged (thinking blocks included), so follow-up questions have context.
class ClaudeClient : public QObject {
    Q_OBJECT
public:
    struct ModelInfo {
        QString id;
        QString label;
        double inPerMTok;    // USD per million input tokens
        double outPerMTok;   // USD per million output tokens
        double cacheReadPerMTok;
        bool serverFallback; // supports fallbacks: "default"
    };
    static const QList<ModelInfo> &models();

    explicit ClaudeClient(QObject *parent = nullptr);

    void setApiKey(const QString &key) { m_apiKey = key.trimmed(); }
    QString apiKey() const { return m_apiKey; }
    bool hasApiKey() const { return !m_apiKey.isEmpty(); }
    void setModel(const QString &id) { m_model = id; }
    QString model() const { return m_model; }

    void ask(const QString &systemPrompt, const QString &userContent);  // appends to the conversation
    void cancel();
    void newConversation();
    bool busy() const { return m_reply != nullptr; }

signals:
    void thinking();                          // model is reasoning before it writes
    void textDelta(const QString &text);
    void finished(const QString &note);       // note: "", "cut off", fallback info...
    void errorOccurred(const QString &msg);
    void usage(int inputTokens, int cacheReadTokens, int outputTokens, double approxUsd);

private slots:
    void onReadyRead();
    void onReplyFinished();

private:
    void handleEvent(const QJsonObject &ev);
    QString errorText(int httpStatus, const QByteArray &body) const;

    QNetworkAccessManager m_nam;
    QNetworkReply *m_reply = nullptr;
    QString m_apiKey;
    QString m_model;
    QByteArray m_sseBuffer;
    QByteArray m_errorBody;                // body of a non-2xx reply (JSON error)

    QJsonArray m_messages;                 // full conversation sent each turn
    QMap<int, QJsonObject> m_blocks;       // blocks of the answer being streamed, by index
    QMap<int, QString> m_partialText;      // text/thinking/signature accumulators
    QMap<int, QString> m_partialThinking;
    QMap<int, QString> m_partialSignature;
    QString m_servedBy;
    QString m_stopReason;
    QString m_stopDetail;
    QString m_streamError;
    int m_inTokens = 0, m_cacheRead = 0, m_cacheWrite = 0, m_outTokens = 0;
    bool m_cancelled = false;
};
