#include "ai/AiClient.h"
#include "ai/AntigravityAuth.h"

#include <QFile>
#include <QHttpMultiPart>
#include <QHttpPart>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonArray>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

#include "core/SettingsManager.h"

namespace mcw {

AiClient::AiClient() {
    m_nam = new QNetworkAccessManager;
}

AiClient::~AiClient() {
    delete m_nam;
}

void AiClient::sendMessage(const QList<ChatMessage> &history,
                           const QJsonArray &toolDefinitions,
                           ResponseCallback callback) {
    const QString provider = SettingsManager::instance().provider();
    if (provider == "gemini") {
        sendGemini(history, toolDefinitions, callback);
    } else if (provider == "anthropic") {
        sendAnthropic(history, toolDefinitions, callback);
    } else {
        // openai, groq, ollama all use OpenAI-compatible format
        sendOpenAiCompatible(history, toolDefinitions, callback);
    }
}

void AiClient::sendGemini(const QList<ChatMessage> &history,
                          const QJsonArray &toolDefinitions,
                          ResponseCallback callback) {
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

    QString urlStr;
    QByteArray postData;
    QNetworkRequest request;
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    // Never let a stalled provider spin the UI forever.
    request.setTransferTimeout(120000);

    if (useOAuth) {
        // Google Cloud Code / Antigravity internal endpoint
        urlStr = "https://daily-cloudcode-pa.googleapis.com/v1internal:generateContent";
        request.setUrl(QUrl(urlStr));
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
        // Public Google AI Studio endpoint
        urlStr = QString("%1/v1beta/models/%2:generateContent?key=%3").arg(baseUrl, model, token);
        request.setUrl(QUrl(urlStr));
        postData = QJsonDocument(innerReq).toJson();
    }

    QNetworkReply *reply = m_nam->post(request, postData);
    QObject::connect(reply, &QNetworkReply::finished, [reply, callback, useOAuth]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            QString err = reply->errorString();
            const QByteArray body = reply->readAll();
            const auto doc = QJsonDocument::fromJson(body);
            if (doc.isObject() && doc.object().contains("error")) {
                const auto eObj = doc.object().value("error").toObject();
                err = eObj.value("message").toString(err);
            }
            callback({false, QString(), {}, err});
            return;
        }

        const QByteArray body = reply->readAll();
        const auto doc = QJsonDocument::fromJson(body);
        const auto root = doc.object();

        // If inside an Antigravity response envelope, unpack 'response' object if present
        const QJsonObject payload = (useOAuth && root.contains("response"))
                                        ? root.value("response").toObject()
                                        : root;

        const auto candidates = payload.value("candidates").toArray();
        if (candidates.isEmpty()) {
            callback({false, QString(), {}, "No response generated by Gemini."});
            return;
        }

        const auto firstCand = candidates.first().toObject();
        const auto content = firstCand.value("content").toObject();
        const auto parts = content.value("parts").toArray();

        AiResponse resp;
        resp.success = true;

        for (const auto &pVal : parts) {
            const auto pObj = pVal.toObject();
            if (pObj.contains("text")) {
                const QString piece = pObj.value("text").toString();
                if (pObj.value("thought").toBool(false)) {
                    if (!resp.reasoning.isEmpty()) resp.reasoning += "\n";
                    resp.reasoning += piece;
                } else {
                    if (!resp.text.isEmpty()) resp.text += "\n";
                    resp.text += piece;
                }
            }
            if (pObj.contains("functionCall")) {
                const auto fc = pObj.value("functionCall").toObject();
                ToolCallRequest tcr;
                tcr.id = QString();
                tcr.name = fc.value("name").toString();
                tcr.arguments = fc.value("args").toObject();
                resp.toolCalls.append(tcr);
            }
        }

        callback(resp);
    });
}

void AiClient::sendOpenAiCompatible(const QList<ChatMessage> &history,
                                    const QJsonArray &toolDefinitions,
                                    ResponseCallback callback) {
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

    QJsonArray messages;
    for (const auto &msg : history) {
        QJsonObject m;
        m["role"] = msg.role;
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

    QNetworkRequest request{QUrl(urlStr)};
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setTransferTimeout(120000);
    if (!apiKey.isEmpty()) {
        request.setRawHeader("Authorization", "Bearer " + apiKey.toUtf8());
    }

    QNetworkReply *reply = m_nam->post(request, QJsonDocument(reqObj).toJson());
    QObject::connect(reply, &QNetworkReply::finished, [reply, callback]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            QString err = reply->errorString();
            const QByteArray body = reply->readAll();
            const auto doc = QJsonDocument::fromJson(body);
            if (doc.isObject() && doc.object().contains("error")) {
                const auto eObj = doc.object().value("error").toObject();
                err = eObj.value("message").toString(err);
            }
            callback({false, QString(), {}, err});
            return;
        }

        const QByteArray body = reply->readAll();
        const auto doc = QJsonDocument::fromJson(body);
        const auto root = doc.object();
        const auto choices = root.value("choices").toArray();
        if (choices.isEmpty()) {
            callback({false, QString(), {}, "No choices returned by AI provider."});
            return;
        }

        const auto firstChoice = choices.first().toObject();
        const auto msg = firstChoice.value("message").toObject();

        AiResponse resp;
        resp.success = true;
        resp.text = msg.value("content").toString();
        // DeepSeek exposes reasoning_content; OpenRouter uses reasoning.
        resp.reasoning = msg.value("reasoning_content").toString();
        if (resp.reasoning.isEmpty()) resp.reasoning = msg.value("reasoning").toString();

        const auto toolCallsArr = msg.value("tool_calls").toArray();
        for (const auto &tcVal : toolCallsArr) {
            const auto tc = tcVal.toObject();
            const auto fn = tc.value("function").toObject();
            ToolCallRequest tcr;
            tcr.id = tc.value("id").toString();
            tcr.name = fn.value("name").toString();
            const QByteArray argsJson = fn.value("arguments").toString().toUtf8();
            QJsonParseError argsError;
            const QJsonDocument argsDoc = QJsonDocument::fromJson(argsJson, &argsError);
            if (argsError.error != QJsonParseError::NoError || !argsDoc.isObject()) {
                const QString detail = argsError.error != QJsonParseError::NoError
                                           ? argsError.errorString()
                                           : QString("expected a JSON object");
                callback({false, QString(), {},
                          QString("Invalid arguments for tool '%1': %2").arg(tcr.name, detail)});
                return;
            }
            tcr.arguments = argsDoc.object();
            resp.toolCalls.append(tcr);
        }

        callback(resp);
    });
}

void AiClient::sendAnthropic(const QList<ChatMessage> &history,
                             const QJsonArray &toolDefinitions,
                             ResponseCallback callback) {
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

    QJsonArray messages;
    for (const auto &msg : history) {
        QJsonObject m;
        m["role"] = (msg.role == "user") ? "user" : "assistant";
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

    QNetworkRequest request{QUrl(urlStr)};
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setTransferTimeout(120000);
    request.setRawHeader("x-api-key", apiKey.toUtf8());
    request.setRawHeader("anthropic-version", "2023-06-01");

    QNetworkReply *reply = m_nam->post(request, QJsonDocument(reqObj).toJson());
    QObject::connect(reply, &QNetworkReply::finished, [reply, callback]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            QString err = reply->errorString();
            const QByteArray body = reply->readAll();
            const auto doc = QJsonDocument::fromJson(body);
            if (doc.isObject() && doc.object().contains("error")) {
                const auto eObj = doc.object().value("error").toObject();
                err = eObj.value("message").toString(err);
            }
            callback({false, QString(), {}, err});
            return;
        }

        const QByteArray body = reply->readAll();
        const auto doc = QJsonDocument::fromJson(body);
        const auto root = doc.object();
        const auto contentArr = root.value("content").toArray();

        AiResponse resp;
        resp.success = true;

        for (const auto &cVal : contentArr) {
            const auto cObj = cVal.toObject();
            const QString type = cObj.value("type").toString();
            if (type == "text") {
                if (!resp.text.isEmpty()) resp.text += "\n";
                resp.text += cObj.value("text").toString();
            } else if (type == "thinking") {
                if (!resp.reasoning.isEmpty()) resp.reasoning += "\n";
                resp.reasoning += cObj.value("thinking").toString();
            } else if (type == "tool_use") {
                ToolCallRequest tcr;
                tcr.id = cObj.value("id").toString();
                tcr.name = cObj.value("name").toString();
                tcr.arguments = cObj.value("input").toObject();
                resp.toolCalls.append(tcr);
            }
        }

        callback(resp);
    });
}

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
        QObject::connect(reply, &QNetworkReply::finished, [reply, callback]() {
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
        QObject::connect(reply, &QNetworkReply::finished, [reply, callback]() {
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
    QObject::connect(reply, &QNetworkReply::finished, [reply, callback]() {
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

    QObject::connect(reply, &QNetworkReply::finished, [reply, callback]() {
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
