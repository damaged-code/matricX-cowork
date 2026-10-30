#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QSharedPointer>
#include <QString>
#include <functional>

#include <QObject>

class QNetworkAccessManager;
class QNetworkReply;
class QNetworkRequest;

namespace mcw {

struct ChatMessage {
    QString role;      // "user", "assistant", "model", or "tool" (tool results)
    QString content;
    QString imagePath; // Optional path to image attachment (e.g. from screenshot tool)
};

struct ToolCallRequest {
    QString id;
    QString name;
    QJsonObject arguments;
};

struct AiResponse {
    bool success = false;
    QString text;
    QList<ToolCallRequest> toolCalls;
    QString errorMessage;
    QString reasoning;  // Provider-supplied "thinking" text, when available
};

// Wraps provider HTTP APIs. Plain QObject so timers/connects are cancelled
// automatically when the owning chat surface dies.
class AiClient : public QObject {
    Q_OBJECT
public:
    using ResponseCallback = std::function<void(const AiResponse &response)>;
    using ModelsCallback = std::function<void(bool success, const QStringList &models, const QString &error)>;
    using TranscriptionCallback = std::function<void(bool success, const QString &text, const QString &error)>;

    // Incremental assistant output while the reply streams. Each invocation
    // carries the text/reasoning fragments appended since the last one; empty
    // fragments are never emitted.
    using StreamCallback = std::function<void(const QString &textDiff, const QString &reasoningDiff)>;

    AiClient();
    ~AiClient();

    // Sends conversation history + tool definitions to configured provider.
    // `stream` is optional; when given and the provider supports it, partial
    // output is delivered as it arrives. `callback` always fires exactly once.
    void sendMessage(const QList<ChatMessage> &history,
                     const QJsonArray &toolDefinitions,
                     ResponseCallback callback,
                     StreamCallback stream = StreamCallback());

    // Cancels the request started by the last sendMessage(): its callback will
    // not run and any open network reply is aborted. Safe to call anytime.
    void abort();

    // Queries provider endpoint to fetch available models for current API key
    void fetchAvailableModels(const QString &providerId, ModelsCallback callback);

    // Transcribes voice audio recording via Whisper API (Groq / OpenAI)
    void transcribeAudio(const QString &audioFilePath, TranscriptionCallback callback);

private:
    Q_DISABLE_COPY(AiClient)

    // Shared per-request run state: abort() marks it cancelled, which makes
    // every scheduled continuation of that request drop out silently.
    struct SendState {
        bool cancelled = false;
        QPointer<QNetworkReply> reply;
        StreamCallback stream;
    };

    // Provider-specific wire parsing, supplied by each send*() to the shared
    // engine below. The per-request accumulator lives in the adapter lambdas.
    struct Adapter {
        // Streaming plumbing for the UI (may be empty; receives incremental
        // text/reasoning diffs while the reply streams).
        StreamCallback stream;
        // Error message from a non-OK response body ("" = use reply errorString).
        std::function<QString(const QByteArray &body)> parseError;
        // Parses a complete (non-streamed) response into acc; false = not parseable.
        std::function<bool(const QByteArray &body, AiResponse &acc)> parseFull;
        // One SSE "data:" JSON event; appends deltas to acc and diffs.
        std::function<bool(const QJsonObject &event, AiResponse &acc,
                           QString &textDiff, QString &reasoningDiff)> parseStreamEvent;
        // Flushes provider-local accumulation (e.g. raw tool-call fragments).
        std::function<void(AiResponse &acc)> finalize;
    };

    // POST + SSE streaming + fallback-to-full-body parse + retry/backoff.
    void postWithRetry(const QNetworkRequest &request, const QByteArray &body,
                       const Adapter &adapter, ResponseCallback callback);

    void sendGemini(const QList<ChatMessage> &history,
                    const QJsonArray &toolDefinitions,
                    ResponseCallback callback,
                    StreamCallback stream);

    void sendOpenAiCompatible(const QList<ChatMessage> &history,
                              const QJsonArray &toolDefinitions,
                              ResponseCallback callback,
                              StreamCallback stream);

    void sendAnthropic(const QList<ChatMessage> &history,
                       const QJsonArray &toolDefinitions,
                       ResponseCallback callback,
                       StreamCallback stream);

    QNetworkAccessManager *m_nam = nullptr;
    QSharedPointer<SendState> m_current;
};

}  // namespace mcw
