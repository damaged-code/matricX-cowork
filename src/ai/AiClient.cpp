#include "ai/AiClient.h"
#include "ai/AntigravityAuth.h"

#include <QFile>
#include <QHttpMultiPart>
#include <QHttpPart>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QMap>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

#include <algorithm>

#include "core/SettingsManager.h"

namespace mcw {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

// Retry policy: at most 3 attempts, only for transient failures, and never
// once any streaming output has been received (re-sending would duplicate it).
constexpr int kMaxAttempts = 3;
constexpr qint64 kRetryDelaysMs[2] = {1200, 3200};  // after 1st and 2nd failed attempt

bool isRetriableStatus(int status) {
    return status == 0 || status == 429 || status >= 500;  // no HTTP status => transport-level failure
}

QJsonObject parseJsonObjectString(const QString &s, QString *err = nullptr) {
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(s.toUtf8(), &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        if (err) {
            *err = (pe.error != QJsonParseError::NoError) ? pe.errorString()
                                                          : QString("expected a JSON object");
        }
        return {};
    }
    return doc.object();
}

QString openAiErrorMessage(const QByteArray &body) {
    const auto doc = QJsonDocument::fromJson(body);
    if (!doc.isObject()) return QString();
    const auto root = doc.object();
    const auto errVal = root.value("error");
    if (errVal.isObject()) return errVal.toObject().value("message").toString();
    if (errVal.isString()) return errVal.toString();
    return QString();
}

QString geminiErrorMessage(const QByteArray &body) {
    const auto doc = QJsonDocument::fromJson(body);
    if (doc.isObject() && doc.object().contains("error")) {
        return doc.object().value("error").toObject().value("message").toString();
    }
    return QString();
}

QString anthropicErrorMessage(const QByteArray &body) {
    return geminiErrorMessage(body);
}

// OpenAI streaming tool calls arrive as fragments grouped by `index`.
struct OpenAiToolAcc {
    QString id;
    QString name;
    QString args;  // raw JSON fragments, parsed once complete
};

// Anthropic streaming blocks are addressed by `index` in content_block_* events.
struct AnthropicBlockAcc {
    int kind = 0;  // 0 text, 1 thinking, 2 tool_use
    QString id;
    QString name;
    QString args;
};

}  // namespace

AiClient::AiClient() {
    m_nam = new QNetworkAccessManager;
}

AiClient::~AiClient() {
    abort();
    delete m_nam;
}

void AiClient::abort() {
    if (!m_current) return;
    m_current->cancelled = true;
    if (m_current->reply) {
        // Aborting triggers finished() with OperationCanceledError; the
        // handler sees `cancelled` and drops the request without a callback.
        m_current->reply->abort();
    }
    m_current.clear();
}

void AiClient::sendMessage(const QList<ChatMessage> &history,
                           const QJsonArray &toolDefinitions,
                           ResponseCallback callback,
                           StreamCallback stream) {
    const QString provider = SettingsManager::instance().provider();
    if (provider == "gemini") {
        sendGemini(history, toolDefinitions, callback, stream);
    } else if (provider == "anthropic") {
        sendAnthropic(history, toolDefinitions, callback, stream);
    } else {
        // openai, groq, ollama all use OpenAI-compatible format
        sendOpenAiCompatible(history, toolDefinitions, callback, stream);
    }
}

// ---------------------------------------------------------------------------
// Shared request engine: POST + SSE parsing + full-body fallback + retry
// ---------------------------------------------------------------------------

void AiClient::postWithRetry(const QNetworkRequest &request, const QByteArray &body,
                             const Adapter &adapter, ResponseCallback callback) {
    abort();  // one logical request at a time; a newer send cancels the older one

    auto state = QSharedPointer<SendState>::create();
    m_current = state;

    struct Ctx {
        QNetworkRequest req;
        QByteArray body;
        Adapter ad;
        ResponseCallback cb;
        int attempt = 0;
    };
    auto ctx = QSharedPointer<Ctx>::create();
    ctx->req = request;
    ctx->body = body;
    ctx->ad = adapter;
    ctx->cb = std::move(callback);

    // One attempt's streaming state. Kept in a shared holder so both the
    // readyRead and finished handlers see the same accumulator.
    struct StreamRun {
        AiResponse acc;
        QByteArray raw;   // full response body (error payloads / fallback parse)
        QByteArray buf;   // unterminated SSE tail
        bool sseSeen = false;
        bool anyDelta = false;  // any text/reasoning fragment emitted
    };

    auto attemptFn = QSharedPointer<std::function<void()>>::create();
    *attemptFn = [this, state, ctx, attemptFn]() {
        if (state->cancelled) return;
        ctx->attempt++;

        QNetworkReply *reply = m_nam->post(ctx->req, ctx->body);
        state->reply = reply;

        const auto run = QSharedPointer<StreamRun>::create();

        // SSE line handling: `data: <json>` events only; comments, event types
        // and OpenAI's `data: [DONE]` sentinel carry no content. Both helpers
        // capture by value (shared state) so the connect-lambdas remain valid
        // for the whole network lifetime, not just this stack frame.
        auto feedLine = [run, ctx](const QByteArray &lineIn) {
            const QByteArray line = lineIn.trimmed();
            if (line.isEmpty() || !line.startsWith("data:")) return;
            const QByteArray payload = line.mid(5).trimmed();
            if (payload.isEmpty() || payload == "[DONE]") return;
            run->sseSeen = true;
            const QJsonDocument evDoc = QJsonDocument::fromJson(payload);
            if (!evDoc.isObject() || !ctx->ad.parseStreamEvent) return;
            QString textDiff, reasoningDiff;
            if (ctx->ad.parseStreamEvent(evDoc.object(), run->acc, textDiff, reasoningDiff)) {
                if (!textDiff.isEmpty() || !reasoningDiff.isEmpty()) run->anyDelta = true;
                if (ctx->ad.stream) ctx->ad.stream(textDiff, reasoningDiff);
            }
        };
        auto feedChunk = [run, feedLine](const QByteArray &data) {
            run->buf += data;
            int nl;
            while ((nl = run->buf.indexOf('\n')) >= 0) {
                feedLine(run->buf.left(nl));
                run->buf.remove(0, nl + 1);
            }
        };

        connect(reply, &QNetworkReply::readyRead, this, [run, reply, feedChunk]() {
            const QByteArray data = reply->readAll();
            if (data.isEmpty()) return;
            run->raw += data;
            feedChunk(data);
        });

        connect(reply, &QNetworkReply::finished, this, [this, state, ctx, reply, run, feedChunk, attemptFn]() {
            reply->deleteLater();
            if (state->cancelled) return;
            state->reply = nullptr;

            const QByteArray remaining = reply->readAll();
            if (!remaining.isEmpty()) {
                run->raw += remaining;
                feedChunk(remaining);
            }
            // A provider/proxy may drop the trailing newline between events.
            if (!run->buf.isEmpty()) {
                const QByteArray tail = run->buf;
                run->buf.clear();
                feedChunk(tail + "\n");
            }

            const int status =
                reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

            if (reply->error() != QNetworkReply::NoError) {
                const bool canRetry =
                    !state->cancelled && ctx->attempt < kMaxAttempts &&
                    !run->sseSeen && isRetriableStatus(status);
                if (canRetry) {
                    const qint64 delay = kRetryDelaysMs[std::min(ctx->attempt - 1, 1)];
                    QTimer::singleShot(delay, this, [this, ctx, attemptFn]() {
                        (*attemptFn)();
                    });
                    return;
                }
                AiResponse resp;
                resp.success = false;
                if (ctx->ad.parseError) {
                    const QString parsed = ctx->ad.parseError(run->raw);
                    if (!parsed.isEmpty()) {
                        resp.errorMessage = parsed;
                    }
                }
                if (resp.errorMessage.isEmpty()) {
                    resp.errorMessage = reply->errorString();
                }
                ctx->cb(resp);
                return;
            }

            // Success: prefer streamed accumulation; fall back to parsing the
            // whole body when the server replied without streaming at all
            // (proxies sometimes ignore stream mode).
            const bool streamedContent =
                run->anyDelta || !run->acc.toolCalls.isEmpty();
            if (!streamedContent) {
                run->acc = AiResponse();
                const bool parsed = ctx->ad.parseFull && ctx->ad.parseFull(run->raw, run->acc);
                if (parsed && (!run->acc.text.isEmpty() || !run->acc.toolCalls.isEmpty() ||
                               !run->acc.reasoning.isEmpty())) {
                    run->acc.success = true;
                    ctx->cb(run->acc);
                    return;
                }
                AiResponse resp;
                resp.success = false;
                if (ctx->ad.parseError) resp.errorMessage = ctx->ad.parseError(run->raw);
                if (resp.errorMessage.isEmpty()) {
                    resp.errorMessage = run->acc.errorMessage.isEmpty()
                                            ? QStringLiteral("Provider returned an empty or unparseable response.")
                                            : run->acc.errorMessage;
                }
                ctx->cb(resp);
                return;
            }

            // Streaming worked and produced content: hand the accumulator to the callback.
            if (ctx->ad.finalize) {
                ctx->ad.finalize(run->acc);  // flush provider-local fragments
            }
            run->acc.success = true;
            ctx->cb(run->acc);
        });
    };
    (*attemptFn)();
}

// ---------------------------------------------------------------------------
// Gemini (public AI Studio endpoint streams; the OAuth envelope does not)
// ---------------------------------------------------------------------------

void AiClient::sendGemini(const QList<ChatMessage> &history,
                          const QJsonArray &toolDefinitions,
                          ResponseCallback callback,
                          StreamCallback stream) {
    auto &sm = SettingsManager::instance();
    const QString apiKey = sm.apiKey("gemini");
    const QString model = sm.model("gemini");
    QString baseUrl = sm.customBaseUrl("gemini");
    if (baseUrl.endsWith('/')) baseUrl.chop(1);

    const bool useOAuth = apiKey.isEmpty() && AntigravityAuth::hasToken();
    const QString token = useOAuth ? AntigravityAuth::accessToken() : apiKey;

    if (token.isEmpty()) {
        callback({false, QString(), {}, "Gemini API key or Google Antigravity login is required. Click the gear icon."});
        return;
    }

    QJsonObject innerReq;

    // Convert history to Gemini contents format
    QJsonArray contents;
    for (const auto &msg : history) {
        QJsonObject contentItem;
        contentItem["role"] = (msg.role == "assistant" || msg.role == "model") ? "model" : "user";
        QJsonArray parts;
        QJsonObject part;
        part["text"] = msg.content;
        parts.append(part);

        if (!msg.imagePath.isEmpty()) {
            QFile imgFile(msg.imagePath);
            if (imgFile.open(QIODevice::ReadOnly)) {
                QJsonObject inlineData;
                inlineData["mimeType"] = "image/png";
                inlineData["data"] = QString::fromLatin1(imgFile.readAll().toBase64());
                QJsonObject imgPart;
                imgPart["inlineData"] = inlineData;
                parts.append(imgPart);
            }
        }

        contentItem["parts"] = parts;
        contents.append(contentItem);
    }
    innerReq["contents"] = contents;

    // Ask Gemini for readable thought summaries (2.5+ thinking models only; older
    // models reject thinkingConfig and would fail the whole request). The Cloud Code /
    // Antigravity envelope does not accept thinkingConfig, so OAuth requests skip it.
    if (!useOAuth && (model.contains("2.5") || model.contains("3."))) {
        QJsonObject thinking;
        thinking["includeThoughts"] = true;
        QJsonObject genCfg;
        genCfg["thinkingConfig"] = thinking;
        innerReq["generationConfig"] = genCfg;
    }

    // Add tools (functionDeclarations)
    if (!toolDefinitions.isEmpty()) {
        QJsonArray funcs;
        for (const auto &tdVal : toolDefinitions) {
            const QJsonObject td = tdVal.toObject();
            QJsonObject f;
            f["name"] = td.value("name").toString();
            f["description"] = td.value("description").toString();
            if (td.contains("input_schema")) {
                f["parameters"] = td.value("input_schema").toObject();
            }
            funcs.append(f);
        }
        QJsonArray toolsArray;
        QJsonObject toolContainer;
        toolContainer["functionDeclarations"] = funcs;
        toolsArray.append(toolContainer);
        innerReq["tools"] = toolsArray;
    }

    Adapter ad;
    ad.stream = stream;
    ad.parseError = [](const QByteArray &body) { return geminiErrorMessage(body); };
    ad.parseFull = [useOAuth](const QByteArray &body, AiResponse &acc) {
        const auto doc = QJsonDocument::fromJson(body);
        if (!doc.isObject()) return false;
        QJsonObject root = doc.object();
        // If inside an Antigravity response envelope, unpack 'response' object if present
        if (useOAuth && root.contains("response")) {
            root = root.value("response").toObject();
        }
        const auto candidates = root.value("candidates").toArray();
        if (candidates.isEmpty()) return false;
        const auto parts = candidates.first().toObject()
                               .value("content").toObject()
                               .value("parts").toArray();
        for (const auto &pVal : parts) {
            const auto pObj = pVal.toObject();
            if (pObj.contains("text")) {
                const QString piece = pObj.value("text").toString();
                if (pObj.value("thought").toBool(false)) {
                    if (!acc.reasoning.isEmpty()) acc.reasoning += "\n";
                    acc.reasoning += piece;
                } else {
                    if (!acc.text.isEmpty()) acc.text += "\n";
                    acc.text += piece;
                }
            }
            if (pObj.contains("functionCall")) {
                const auto fc = pObj.value("functionCall").toObject();
                ToolCallRequest tcr;
                tcr.name = fc.value("name").toString();
                tcr.arguments = fc.value("args").toObject();
                acc.toolCalls.append(tcr);
            }
        }
        return true;
    };
    ad.parseStreamEvent = [](const QJsonObject &ev, AiResponse &acc,
                             QString &textDiff, QString &reasoningDiff) {
        const auto candidates = ev.value("candidates").toArray();
        if (candidates.isEmpty()) return true;  // e.g. usage-only final chunk
        const auto parts = candidates.first().toObject()
                               .value("content").toObject()
                               .value("parts").toArray();
        for (const auto &pVal : parts) {
            const auto pObj = pVal.toObject();
            if (pObj.contains("text")) {
                const QString piece = pObj.value("text").toString();
                const bool thought = pObj.value("thought").toBool(false);
                if (thought) {
                    if (!acc.reasoning.isEmpty()) acc.reasoning += "\n";
                    if (!reasoningDiff.isEmpty()) reasoningDiff += "\n";
                    acc.reasoning += piece;
                    reasoningDiff += piece;
                } else {
                    if (!acc.text.isEmpty()) acc.text += "\n";
                    if (!textDiff.isEmpty()) textDiff += "\n";
                    acc.text += piece;
                    textDiff += piece;
                }
            }
            if (pObj.contains("functionCall")) {
                const auto fc = pObj.value("functionCall").toObject();
                ToolCallRequest tcr;
                tcr.name = fc.value("name").toString();
                tcr.arguments = fc.value("args").toObject();
                acc.toolCalls.append(tcr);
            }
        }
        return true;
    };

    QNetworkRequest request;
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    // Never let a stalled provider spin the UI forever.
    request.setTransferTimeout(120000);

    QByteArray postData;
    if (useOAuth) {
        // Google Cloud Code / Antigravity internal endpoint (no streaming)
        request.setUrl(QUrl("https://daily-cloudcode-pa.googleapis.com/v1internal:generateContent"));
        request.setRawHeader("Authorization", "Bearer " + token.toUtf8());
        request.setRawHeader("User-Agent", "antigravity/4.3.0 (cmdc-pak)");
        request.setRawHeader("x-client-name", "antigravity");
        request.setRawHeader("x-client-version", "4.3.0");
        request.setRawHeader("x-machine-id", "cmdc-pak");
        request.setRawHeader("x-vscode-sessionid", "proxy");

        // Envelope wrapper required by Cloud Code
        QJsonObject envelope;
        envelope["model"] = model;
        envelope["project"] = "aicode-consumers";
        envelope["request"] = innerReq;
        envelope["userAgent"] = "antigravity";
        postData = QJsonDocument(envelope).toJson();
    } else {
        // Public Google AI Studio endpoint, Server-Sent-Events streaming
        const QString urlStr = QString("%1/v1beta/models/%2:streamGenerateContent?alt=sse&key=%3")
                                   .arg(baseUrl, model, token);
        request.setUrl(QUrl(urlStr));
        postData = QJsonDocument(innerReq).toJson();
    }

    postWithRetry(request, postData, ad, callback);
}

// ---------------------------------------------------------------------------
// OpenAI-compatible (OpenAI, Groq, Ollama, DeepSeek, OpenRouter — streamed)
// ---------------------------------------------------------------------------

void AiClient::sendOpenAiCompatible(const QList<ChatMessage> &history,
                                    const QJsonArray &toolDefinitions,
                                    ResponseCallback callback,
                                    StreamCallback stream) {
    const QString provider = SettingsManager::instance().provider();
    auto &sm = SettingsManager::instance();
    const QString apiKey = sm.apiKey(provider);
    const QString model = sm.model(provider);
    QString baseUrl = sm.customBaseUrl(provider);
    if (baseUrl.endsWith('/')) baseUrl.chop(1);

    if (apiKey.isEmpty() && provider != "ollama") {
        callback({false, QString(), {}, QString("%1 API key is missing. Click the gear icon to set it.").arg(provider)});
        return;
    }

    const QString urlStr = baseUrl + "/chat/completions";

    QJsonObject reqObj;
    reqObj["model"] = model;
    if (stream) reqObj["stream"] = true;

    QJsonArray messages;
    for (const auto &msg : history) {
        QJsonObject m;
        m["role"] = (msg.role == "tool") ? "user" : msg.role;
        if (!msg.imagePath.isEmpty()) {
            QFile imgFile(msg.imagePath);
            if (imgFile.open(QIODevice::ReadOnly)) {
                QJsonArray contentArr;
                QJsonObject textPart;
                textPart["type"] = "text";
                textPart["text"] = msg.content;
                contentArr.append(textPart);

                QJsonObject imgPart;
                imgPart["type"] = "image_url";
                QJsonObject urlObj;
                urlObj["url"] = "data:image/png;base64," + QString::fromLatin1(imgFile.readAll().toBase64());
                imgPart["image_url"] = urlObj;
                contentArr.append(imgPart);

                m["content"] = contentArr;
            } else {
                m["content"] = msg.content;
            }
        } else {
            m["content"] = msg.content;
        }
        messages.append(m);
    }
    reqObj["messages"] = messages;

    if (!toolDefinitions.isEmpty()) {
        QJsonArray tools;
        for (const auto &tdVal : toolDefinitions) {
            const QJsonObject td = tdVal.toObject();
            QJsonObject toolItem;
            toolItem["type"] = "function";
            QJsonObject f;
            f["name"] = td.value("name").toString();
            f["description"] = td.value("description").toString();
            if (td.contains("input_schema")) {
                f["parameters"] = td.value("input_schema").toObject();
            }
            toolItem["function"] = f;
            tools.append(toolItem);
        }
        reqObj["tools"] = tools;
    }

    Adapter ad;
    ad.stream = stream;
    const auto toolAcc = QSharedPointer<QMap<int, OpenAiToolAcc>>::create();

    ad.parseError = [](const QByteArray &body) { return openAiErrorMessage(body); };
    ad.parseFull = [toolAcc](const QByteArray &body, AiResponse &acc) {
        const auto doc = QJsonDocument::fromJson(body);
        if (!doc.isObject()) return false;
        const auto choices = doc.object().value("choices").toArray();
        if (choices.isEmpty()) return false;

        const auto msg = choices.first().toObject().value("message").toObject();
        acc.text = msg.value("content").toString();
        // DeepSeek exposes reasoning_content; OpenRouter uses reasoning.
        acc.reasoning = msg.value("reasoning_content").toString();
        if (acc.reasoning.isEmpty()) acc.reasoning = msg.value("reasoning").toString();

        const auto toolCallsArr = msg.value("tool_calls").toArray();
        for (const auto &tcVal : toolCallsArr) {
            const auto tc = tcVal.toObject();
            const auto fn = tc.value("function").toObject();
            ToolCallRequest tcr;
            tcr.id = tc.value("id").toString();
            tcr.name = fn.value("name").toString();
            QString argsErr;
            tcr.arguments = parseJsonObjectString(fn.value("arguments").toString(), &argsErr);
            if (tcr.arguments.isEmpty()) {
                acc.errorMessage = QString("Invalid arguments for tool '%1': %2").arg(tcr.name, argsErr);
                acc.toolCalls.clear();
                return false;
            }
            acc.toolCalls.append(tcr);
        }
        return true;
    };
    ad.parseStreamEvent = [toolAcc](const QJsonObject &ev, AiResponse &acc,
                                    QString &textDiff, QString &reasoningDiff) {
        const auto choices = ev.value("choices").toArray();
        if (choices.isEmpty()) return true;  // keepalive / usage-only chunks
        const auto delta = choices.first().toObject().value("delta").toObject();

        const QString piece = delta.value("content").toString();
        if (!piece.isEmpty()) {
            acc.text += piece;
            textDiff = piece;
        }
        QString think = delta.value("reasoning_content").toString();
        if (think.isEmpty()) think = delta.value("reasoning").toString();
        if (!think.isEmpty()) {
            acc.reasoning += think;
            reasoningDiff = think;
        }

        const auto tcs = delta.value("tool_calls").toArray();
        for (const auto &tcVal : tcs) {
            const auto tc = tcVal.toObject();
            const int idx = tc.value("index").toInt();
            OpenAiToolAcc &slot = (*toolAcc)[idx];
            const QString id = tc.value("id").toString();
            if (!id.isEmpty() && slot.id.isEmpty()) slot.id = id;
            const auto fn = tc.value("function").toObject();
            const QString fname = fn.value("name").toString();
            if (!fname.isEmpty() && slot.name.isEmpty()) slot.name = fname;
            slot.args += fn.value("arguments").toString();
        }
        return true;
    };
    ad.finalize = [toolAcc](AiResponse &acc) {
        // Streamed tool calls are accumulated as raw argument fragments; parse
        // them now that every chunk has arrived.
        acc.toolCalls.clear();
        for (auto it = toolAcc->constBegin(); it != toolAcc->constEnd(); ++it) {
            ToolCallRequest tcr;
            tcr.id = it->id;
            tcr.name = it->name;
            tcr.arguments = parseJsonObjectString(it->args);
            acc.toolCalls.append(tcr);
        }
    };

    QNetworkRequest request{QUrl(urlStr)};
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setTransferTimeout(120000);
    if (!apiKey.isEmpty()) {
        request.setRawHeader("Authorization", "Bearer " + apiKey.toUtf8());
    }

    postWithRetry(request, QJsonDocument(reqObj).toJson(), ad, callback);
}

// ---------------------------------------------------------------------------
// Anthropic Claude (streamed); consecutive same-role messages are merged
// ---------------------------------------------------------------------------

void AiClient::sendAnthropic(const QList<ChatMessage> &history,
                             const QJsonArray &toolDefinitions,
                             ResponseCallback callback,
                             StreamCallback stream) {
    auto &sm = SettingsManager::instance();
    const QString apiKey = sm.apiKey("anthropic");
    const QString model = sm.model("anthropic");
    QString baseUrl = sm.customBaseUrl("anthropic");
    if (baseUrl.endsWith('/')) baseUrl.chop(1);

    if (apiKey.isEmpty()) {
        callback({false, QString(), {}, "Anthropic API key is missing. Click the gear icon to set it."});
        return;
    }

    const QString urlStr = baseUrl + "/messages";

    QJsonObject reqObj;
    reqObj["model"] = model;
    reqObj["max_tokens"] = 4096;
    if (stream) reqObj["stream"] = true;

    QJsonArray messages;
    for (const auto &msg : history) {
        const bool userSide = (msg.role == "user" || msg.role == "tool");
        const QString role = userSide ? "user" : "assistant";

        // Anthropic requires strictly alternating roles; merge consecutive
        // same-role messages by joining their text (images keep their own).
        if (msg.imagePath.isEmpty() && !messages.isEmpty()) {
            QJsonObject last = messages.last().toObject();
            if (last.value("role").toString() == role && last.value("content").isString()) {
                last["content"] = last.value("content").toString() + "\n\n" + msg.content;
                messages.replace(messages.size() - 1, last);
                continue;
            }
        }

        QJsonObject m;
        m["role"] = role;
        if (!msg.imagePath.isEmpty()) {
            QFile imgFile(msg.imagePath);
            if (imgFile.open(QIODevice::ReadOnly)) {
                QJsonArray contentArr;
                QJsonObject textPart;
                textPart["type"] = "text";
                textPart["text"] = msg.content;
                contentArr.append(textPart);

                QJsonObject imgPart;
                imgPart["type"] = "image";
                QJsonObject srcObj;
                srcObj["type"] = "base64";
                srcObj["media_type"] = "image/png";
                srcObj["data"] = QString::fromLatin1(imgFile.readAll().toBase64());
                imgPart["source"] = srcObj;
                contentArr.append(imgPart);

                m["content"] = contentArr;
            } else {
                m["content"] = msg.content;
            }
        } else {
            m["content"] = msg.content;
        }
        messages.append(m);
    }
    reqObj["messages"] = messages;

    if (!toolDefinitions.isEmpty()) {
        QJsonArray tools;
        for (const auto &tdVal : toolDefinitions) {
            const QJsonObject td = tdVal.toObject();
            QJsonObject t;
            t["name"] = td.value("name").toString();
            t["description"] = td.value("description").toString();
            if (td.contains("input_schema")) {
                t["input_schema"] = td.value("input_schema").toObject();
            }
            tools.append(t);
        }
        reqObj["tools"] = tools;
    }

    Adapter ad;
    ad.stream = stream;
    const auto blocks = QSharedPointer<QMap<int, AnthropicBlockAcc>>::create();

    ad.parseError = [](const QByteArray &body) { return anthropicErrorMessage(body); };
    ad.parseFull = [](const QByteArray &body, AiResponse &acc) {
        const auto doc = QJsonDocument::fromJson(body);
        if (!doc.isObject()) return false;
        const auto contentArr = doc.object().value("content").toArray();
        for (const auto &cVal : contentArr) {
            const auto cObj = cVal.toObject();
            const QString type = cObj.value("type").toString();
            if (type == "text") {
                if (!acc.text.isEmpty()) acc.text += "\n";
                acc.text += cObj.value("text").toString();
            } else if (type == "thinking") {
                if (!acc.reasoning.isEmpty()) acc.reasoning += "\n";
                acc.reasoning += cObj.value("thinking").toString();
            } else if (type == "tool_use") {
                ToolCallRequest tcr;
                tcr.id = cObj.value("id").toString();
                tcr.name = cObj.value("name").toString();
                tcr.arguments = cObj.value("input").toObject();
                acc.toolCalls.append(tcr);
            }
        }
        return true;
    };
    ad.parseStreamEvent = [blocks](const QJsonObject &ev, AiResponse &acc,
                                   QString &textDiff, QString &reasoningDiff) {
        const QString type = ev.value("type").toString();
        if (type == "content_block_start") {
            const int index = ev.value("index").toInt();
            const auto block = ev.value("content_block").toObject();
            AnthropicBlockAcc b;
            b.id = block.value("id").toString();
            b.name = block.value("name").toString();
            const QString bt = block.value("type").toString();
            if (bt == "tool_use") b.kind = 2;
            else if (bt == "thinking") b.kind = 1;
            else b.kind = 0;
            blocks->insert(index, b);
            return true;
        }
        if (type == "content_block_delta") {
            const int index = ev.value("index").toInt();
            const auto delta = ev.value("delta").toObject();
            AnthropicBlockAcc &b = (*blocks)[index];
            const QString dt = delta.value("type").toString();
            if (dt == "text_delta") {
                const QString piece = delta.value("text").toString();
                acc.text += piece;
                textDiff += piece;
            } else if (dt == "thinking_delta") {
                const QString piece = delta.value("thinking").toString();
                acc.reasoning += piece;
                reasoningDiff += piece;
            } else if (dt == "input_json_delta") {
                b.args += delta.value("partial_json").toString();
            }
            return true;
        }
        if (type == "content_block_stop") {
            const int index = ev.value("index").toInt();
            auto it = blocks->find(index);
            if (it != blocks->end() && it->kind == 2) {
                ToolCallRequest tcr;
                tcr.id = it->id;
                tcr.name = it->name;
                tcr.arguments = parseJsonObjectString(it->args);
                acc.toolCalls.append(tcr);
                blocks->erase(it);
            }
            return true;
        }
        return true;  // message_start / message_delta / message_stop / ping
    };
    ad.finalize = [blocks](AiResponse &acc) {
        // Flush tool blocks whose stop event never arrived (truncated stream).
        for (auto it = blocks->constBegin(); it != blocks->constEnd(); ++it) {
            if (it->kind == 2) {
                ToolCallRequest tcr;
                tcr.id = it->id;
                tcr.name = it->name;
                tcr.arguments = parseJsonObjectString(it->args);
                acc.toolCalls.append(tcr);
            }
        }
        blocks->clear();
    };

    QNetworkRequest request{QUrl(urlStr)};
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setTransferTimeout(120000);
    request.setRawHeader("x-api-key", apiKey.toUtf8());
    request.setRawHeader("anthropic-version", "2023-06-01");

    postWithRetry(request, QJsonDocument(reqObj).toJson(), ad, callback);
}

// ---------------------------------------------------------------------------
// Model listing / audio transcription (unchanged aside from being here)
// ---------------------------------------------------------------------------

void AiClient::fetchAvailableModels(const QString &providerId, ModelsCallback callback) {
    auto &sm = SettingsManager::instance();
    const QString apiKey = sm.apiKey(providerId);
    QString baseUrl = sm.customBaseUrl(providerId);
    if (baseUrl.endsWith('/')) baseUrl.chop(1);

    if (providerId == "gemini") {
        const bool useOAuth = apiKey.isEmpty() && AntigravityAuth::hasToken();
        const QString token = useOAuth ? AntigravityAuth::accessToken() : apiKey;
        if (token.isEmpty()) {
            callback(false, {}, "Missing Gemini API key or Antigravity login.");
            return;
        }

        if (useOAuth) {
            // Antigravity models supported by Cloud Code daily upstream
            QStringList models = {
                "gemini-2.5-flash",
                "gemini-2.5-pro",
                "gemini-3.1-pro-preview",
                "gemini-3.8-flash",
                "gemini-flash-latest",
                "gemini-pro-latest"
            };
            callback(true, models, QString());
            return;
        }

        const QString url = QString("%1/v1beta/models?key=%2").arg(baseUrl, token);
        QNetworkRequest req{QUrl(url)};
        req.setTransferTimeout(120000);
        QNetworkReply *reply = m_nam->get(req);
        QObject::connect(reply, &QNetworkReply::finished, reply, [reply, callback]() {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) {
                callback(false, {}, reply->errorString());
                return;
            }
            const auto doc = QJsonDocument::fromJson(reply->readAll());
            const auto arr = doc.object().value("models").toArray();
            QStringList models;
            for (const auto &mVal : arr) {
                QString name = mVal.toObject().value("name").toString();
                if (name.startsWith("models/")) name = name.mid(7);
                models.append(name);
            }
            callback(true, models, QString());
        });
        return;
    }

    if (providerId == "anthropic") {
        if (apiKey.isEmpty()) {
            callback(false, {}, "Missing Anthropic API key.");
            return;
        }
        QNetworkRequest req{QUrl(baseUrl + "/models")};
        req.setTransferTimeout(120000);
        req.setRawHeader("x-api-key", apiKey.toUtf8());
        req.setRawHeader("anthropic-version", "2023-06-01");
        QNetworkReply *reply = m_nam->get(req);
        QObject::connect(reply, &QNetworkReply::finished, reply, [reply, callback]() {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) {
                callback(false, {}, reply->errorString());
                return;
            }
            const auto doc = QJsonDocument::fromJson(reply->readAll());
            const auto data = doc.object().value("data").toArray();
            QStringList models;
            for (const auto &mVal : data) {
                models.append(mVal.toObject().value("id").toString());
            }
            callback(true, models, QString());
        });
        return;
    }

    // OpenAI, DeepSeek, OpenRouter, Groq, Ollama
    if (apiKey.isEmpty() && providerId != "ollama") {
        callback(false, {}, QString("Missing %1 API key.").arg(providerId));
        return;
    }

    QNetworkRequest req{QUrl(baseUrl + "/models")};
    req.setTransferTimeout(120000);
    if (!apiKey.isEmpty()) {
        req.setRawHeader("Authorization", "Bearer " + apiKey.toUtf8());
    }
    QNetworkReply *reply = m_nam->get(req);
    QObject::connect(reply, &QNetworkReply::finished, reply, [reply, callback]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            callback(false, {}, reply->errorString());
            return;
        }
        const auto doc = QJsonDocument::fromJson(reply->readAll());
        const auto root = doc.object();
        QJsonArray data = root.value("data").toArray();
        if (data.isEmpty() && root.contains("models")) {
            data = root.value("models").toArray();
        }
        QStringList models;
        for (const auto &mVal : data) {
            const auto obj = mVal.toObject();
            QString id = obj.value("id").toString();
            if (id.isEmpty()) id = obj.value("name").toString();
            if (!id.isEmpty()) models.append(id);
        }
        callback(true, models, QString());
    });
}

void AiClient::transcribeAudio(const QString &audioFilePath, TranscriptionCallback callback) {
    auto &sm = SettingsManager::instance();
    // Prefer Groq (whisper-large-v3-turbo), then OpenAI (whisper-1)
    QString apiKey = sm.apiKey("groq");
    QString endpoint = "https://api.groq.com/openai/v1/audio/transcriptions";
    QString modelName = "whisper-large-v3-turbo";

    if (apiKey.isEmpty()) {
        apiKey = sm.apiKey("openai");
        endpoint = "https://api.openai.com/v1/audio/transcriptions";
        modelName = "whisper-1";
    }

    if (apiKey.isEmpty()) {
        callback(false, QString(), "Please configure an OpenAI or Groq API key in Settings to use voice transcription.");
        return;
    }

    auto *multiPart = new QHttpMultiPart(QHttpMultiPart::FormDataType);

    QHttpPart modelPart;
    modelPart.setHeader(QNetworkRequest::ContentDispositionHeader, QVariant("form-data; name=\"model\""));
    modelPart.setBody(modelName.toUtf8());
    multiPart->append(modelPart);

    auto *file = new QFile(audioFilePath);
    if (!file->open(QIODevice::ReadOnly)) {
        delete file;
        delete multiPart;
        callback(false, QString(), "Failed to open audio recording file.");
        return;
    }

    QHttpPart filePart;
    filePart.setHeader(QNetworkRequest::ContentTypeHeader, QVariant("audio/m4a"));
    filePart.setHeader(QNetworkRequest::ContentDispositionHeader, QVariant("form-data; name=\"file\"; filename=\"recording.m4a\""));
    filePart.setBodyDevice(file);
    file->setParent(multiPart);
    multiPart->append(filePart);

    QNetworkRequest request{QUrl(endpoint)};
    request.setTransferTimeout(120000);
    request.setRawHeader("Authorization", "Bearer " + apiKey.toUtf8());

    QNetworkReply *reply = m_nam->post(request, multiPart);
    multiPart->setParent(reply);

    QObject::connect(reply, &QNetworkReply::finished, reply, [reply, callback]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            callback(false, QString(), reply->errorString());
            return;
        }

        const auto doc = QJsonDocument::fromJson(reply->readAll());
        const QString text = doc.object().value("text").toString().trimmed();
        if (text.isEmpty()) {
            callback(false, QString(), "No transcription text returned.");
        } else {
            callback(true, text, QString());
        }
    });
}

}  // namespace mcw
