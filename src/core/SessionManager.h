#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>

namespace mcw {

struct ChatMessageItem {
    QString role;      // "user", "assistant", "tool", or "thinking" (transcript-only)
    QString content;
    QString imagePath; // optional attachment (tool screenshots)
    qint64 timestamp = 0;
};

struct SessionItem {
    QString id;
    QString title;
    qint64 createdAt = 0;
    qint64 updatedAt = 0;
    bool isArchived = false;
    QString provider;
    QString model;
    QList<ChatMessageItem> messages;
};

class SessionManager : public QObject {
    Q_OBJECT
public:
    static SessionManager &instance();

    // Session listing
    QList<SessionItem> activeSessions() const;
    QList<SessionItem> archivedSessions() const;
    SessionItem currentSession() const;
    QString currentSessionId() const;

    // Session operations
    QString createSession(const QString &title = "New Session");
    void setCurrentSession(const QString &sessionId);
    void renameSession(const QString &sessionId, const QString &newTitle);
    void setSessionModel(const QString &sessionId, const QString &provider, const QString &model);

    // Lifecycle
    void archiveSession(const QString &sessionId);
    void restoreSession(const QString &sessionId);
    void deleteSessionPermanently(const QString &sessionId);

    // Messages
    void appendMessage(const QString &sessionId, const QString &role, const QString &content,
                       const QString &imagePath = QString());
    QList<ChatMessageItem> getMessages(const QString &sessionId) const;

signals:
    void sessionsChanged();
    void currentSessionChanged(const QString &sessionId);
    void messagesChanged(const QString &sessionId);

private:
    SessionManager();
    void load();
    void save();

    QList<SessionItem> m_sessions;
    QString m_currentId;
};

}  // namespace mcw
