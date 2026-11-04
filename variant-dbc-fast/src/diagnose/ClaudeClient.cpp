#include "ClaudeClient.h"
#include <QNetworkRequest>
#include <QJsonDocument>

// Prices: Anthropic first-party API, USD per million tokens (input / output / cache read).
const QList<ClaudeClient::ModelInfo> &ClaudeClient::models() {
    static const QList<ModelInfo> list = {
        {"claude-opus-5-5",   "Claude Opus 5.5 (best)",     4.00, 20.00, 0.20, true},
        {"claude-sonnet-5-5", "Claude Sonnet 5.5 (faster)", 2.00, 10.00, 0.20, true},
        {"claude-haiku-5-5",  "Claude Haiku 5.5 (cheapest)", 0.10, 0.50, 0.01, false},
    };
    return list;
}

static const ClaudeClient::ModelInfo *findModel(const QString &id) {
    for (const auto &m : ClaudeClient::models())
        if (m.id == id) return &m;
    return &ClaudeClient::models().first();
}

ClaudeClient::ClaudeClient(QObject *parent) : QObject(parent), m_model(models().first().id) {}

void ClaudeClient::newConversation() {
    cancel();
    m_messages = QJsonArray();
}

void ClaudeClient::ask(const QString &systemPrompt, const QString &userContent) {
    if (m_reply) cancel();
    const ModelInfo *mi = findModel(m_model);

    QNetworkRequest req(QUrl("https://api.anthropic.com/v1/messages"));
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setRawHeader("x-api-key", m_apiKey.toUtf8());
    req.setRawHeader("anthropic-version", "2026-10-21");
    if (mi->serverFallback)
        req.setRawHeader("anthropic-beta", "server-side-fallback-2026-07-01");

    QJsonObject user;
    user["role"] = "user";
    user["content"] = userContent;
    m_messages.append(user);

    // Fixed instructions go in a cached system block; the per-question vehicle
    // snapshot rides in the user turn, so the history stays append-only and cacheable.
    QJsonObject sysBlock{{"type", "text"}, {"text", systemPrompt},
                         {"cache_control", QJsonObject{{"type", "ephemeral"}}}};
    QJsonObject body;
    body["model"] = mi->id;
    body["max_tokens"] = 16000;
    body["stream"] = true;
    body["system"] = QJsonArray{sysBlock};
    body["messages"] = m_messages;
    body["output_config"] = QJsonObject{{"effort", "medium"}};
    body["cache_control"] = QJsonObject{{"type", "ephemeral"}};  // caches the growing conversation
    if (mi->serverFallback) body["fallbacks"] = "default";       // a safety decline is re-run on another model

    m_sseBuffer.clear();
    m_errorBody.clear();
    m_blocks.clear();
    m_partialText.clear();
    m_partialThinking.clear();
    m_partialSignature.clear();
    m_servedBy.clear();
    m_stopReason.clear();
    m_stopDetail.clear();
    m_streamError.clear();
    m_inTokens = m_cacheRead = m_cacheWrite = m_outTokens = 0;
    m_cancelled = false;

    m_reply = m_nam.post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(m_reply, &QNetworkReply::readyRead, this, &ClaudeClient::onReadyRead);
    connect(m_reply, &QNetworkReply::finished, this, &ClaudeClient::onReplyFinished);
}

void ClaudeClient::cancel() {
    if (!m_reply) return;
    m_cancelled = true;
    m_reply->abort();  // finished() fires synchronously and cleans up
}

void ClaudeClient::onReadyRead() {
    const int status = m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status >= 400) {  // error replies are plain JSON, not an event stream
        m_errorBody += m_reply->readAll();
        return;
    }
    m_sseBuffer += m_reply->readAll();
    while (true) {
        const int nl = m_sseBuffer.indexOf('\n');
        if (nl < 0) break;
        const QByteArray line = m_sseBuffer.left(nl).trimmed();
        m_sseBuffer.remove(0, nl + 1);
        if (!line.startsWith("data:")) continue;  // "event:" lines repeat the type in the JSON
        const QJsonObject ev = QJsonDocument::fromJson(line.mid(5).trimmed()).object();
        if (!ev.isEmpty()) handleEvent(ev);
    }
}

void ClaudeClient::handleEvent(const QJsonObject &ev) {
    const QString type = ev.value("type").toString();
    if (type == "message_start") {
        const QJsonObject msg = ev.value("message").toObject();
        m_servedBy = msg.value("model").toString();
        const QJsonObject u = msg.value("usage").toObject();
        m_inTokens = u.value("input_tokens").toInt();
        m_cacheRead = u.value("cache_read_input_tokens").toInt();
        m_cacheWrite = u.value("cache_creation_input_tokens").toInt();
    } else if (type == "content_block_start") {
        const int idx = ev.value("index").toInt();
        const QJsonObject block = ev.value("content_block").toObject();
        m_blocks.insert(idx, block);
        const QString bt = block.value("type").toString();
        if (bt == "thinking" || bt == "redacted_thinking") emit thinking();
    } else if (type == "content_block_delta") {
        const int idx = ev.value("index").toInt();
        const QJsonObject d = ev.value("delta").toObject();
        const QString dt = d.value("type").toString();
        if (dt == "text_delta") {
            const QString t = d.value("text").toString();
            m_partialText[idx] += t;
            if (!t.isEmpty()) emit textDelta(t);
        } else if (dt == "thinking_delta") {
            m_partialThinking[idx] += d.value("thinking").toString();
        } else if (dt == "signature_delta") {
            m_partialSignature[idx] += d.value("signature").toString();
        }
    } else if (type == "content_block_stop") {
        const int idx = ev.value("index").toInt();
        QJsonObject block = m_blocks.value(idx);
        const QString bt = block.value("type").toString();
        if (bt == "text") block["text"] = block.value("text").toString() + m_partialText.value(idx);
        if (bt == "thinking") {
            block["thinking"] = block.value("thinking").toString() + m_partialThinking.value(idx);
            if (m_partialSignature.contains(idx)) block["signature"] = m_partialSignature.value(idx);
        }
        m_blocks.insert(idx, block);
    } else if (type == "message_delta") {
        const QJsonObject d = ev.value("delta").toObject();
        if (d.contains("stop_reason")) m_stopReason = d.value("stop_reason").toString();
        const QJsonObject sd = d.contains("stop_details") ? d.value("stop_details").toObject()
                                                          : ev.value("stop_details").toObject();
        if (!sd.isEmpty())
            m_stopDetail = sd.value("explanation").toString(sd.value("category").toString());
        m_outTokens = ev.value("usage").toObject().value("output_tokens").toInt(m_outTokens);
    } else if (type == "error") {
        const QJsonObject e = ev.value("error").toObject();
        m_streamError = e.value("message").toString(e.value("type").toString());
    }
}

QString ClaudeClient::errorText(int status, const QByteArray &body) const {
    const QJsonObject err = QJsonDocument::fromJson(body).object().value("error").toObject();
    const QString apiMsg = err.value("message").toString();
    switch (status) {
    case 401: return "API key was rejected. Check the key from console.anthropic.com (API Key… button).";
    case 403: return "This API key isn't allowed to use that model. " + apiMsg;
    case 429: return "Rate limit reached — wait a moment and try again. " + apiMsg;
    case 529: return "Claude is overloaded right now — try again in a minute.";
    case 402: return "Billing problem on the API account (out of credit?). " + apiMsg;
    default:
        return apiMsg.isEmpty() ? QString("HTTP %1").arg(status) : QString("HTTP %1: %2").arg(status).arg(apiMsg);
    }
}

void ClaudeClient::onReplyFinished() {
    QNetworkReply *reply = m_reply;
    m_reply = nullptr;
    reply->deleteLater();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status >= 400) m_errorBody += reply->readAll();
    else if (reply->bytesAvailable()) { m_sseBuffer += reply->readAll(); m_sseBuffer += '\n'; }

    auto dropUnansweredQuestion = [this] {
        if (!m_messages.isEmpty() && m_messages.last().toObject().value("role") == "user")
            m_messages.removeLast();
    };

    if (m_cancelled) {
        dropUnansweredQuestion();
        emit finished("stopped");
        return;
    }
    if (status >= 400) {
        dropUnansweredQuestion();
        emit errorOccurred(errorText(status, m_errorBody));
        return;
    }
    if (reply->error() != QNetworkReply::NoError) {
        dropUnansweredQuestion();
        emit errorOccurred("Network problem: " + reply->errorString());
        return;
    }
    if (!m_streamError.isEmpty()) {
        dropUnansweredQuestion();
        emit errorOccurred("Claude reported an error: " + m_streamError);
        return;
    }

    const ModelInfo *mi = findModel(m_model);
    const double usd = ((m_inTokens + m_cacheWrite * 1.25) * mi->inPerMTok + m_cacheRead * mi->cacheReadPerMTok +
                        m_outTokens * mi->outPerMTok) / 1e6;
    emit usage(m_inTokens + m_cacheWrite, m_cacheRead, m_outTokens, usd);

    if (m_stopReason == "refusal") {
        dropUnansweredQuestion();
        emit finished("Claude declined to answer this one" +
                      (m_stopDetail.isEmpty() ? QString() : " (" + m_stopDetail + ")") +
                      ". Try rephrasing the question.");
        return;
    }

    // Echo the answer back next turn exactly as received. After a mid-answer safety
    // fallback, reasoning blocks before the last fallback marker are left out (API rule).
    int lastFallback = -1;
    const QList<int> order = m_blocks.keys();
    for (int idx : order)
        if (m_blocks.value(idx).value("type") == "fallback") lastFallback = idx;
    QJsonArray content;
    for (int idx : order) {
        const QJsonObject b = m_blocks.value(idx);
        const QString bt = b.value("type").toString();
        if (bt == "fallback") continue;
        if (idx < lastFallback && (bt == "thinking" || bt == "redacted_thinking" || bt == "tool_use")) continue;
        if (bt == "text" && b.value("text").toString().isEmpty()) continue;
        content.append(b);
    }
    bool hasText = false;
    for (const QJsonValue &b : content) hasText |= b.toObject().value("type") == "text";
    if (!hasText) {
        dropUnansweredQuestion();
        emit finished(m_stopReason == "max_tokens" ? "No answer — ran out of length while thinking. Ask a narrower question."
                                                   : "No answer came back. Try again.");
        return;
    }
    m_messages.append(QJsonObject{{"role", "assistant"}, {"content", content}});

    QStringList notes;
    if (m_stopReason == "max_tokens") notes << "answer was cut off (length limit)";
    if (!m_servedBy.isEmpty() && m_servedBy != mi->id)
        notes << "answered by " + m_servedBy + " (automatic safety fallback)";
    emit finished(notes.join("; "));
}
