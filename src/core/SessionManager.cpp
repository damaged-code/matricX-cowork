#include "core/SessionManager.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QUuid>

#include "core/SettingsManager.h"

namespace mcw {

static QString storageFilePath() {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    return QDir(dir).filePath("sessions.json");
}

SessionManager::SessionManager() {
    load();
    if (m_sessions.isEmpty()) {
        createSession("Current Session");
    } else {
        // Find first active session
        for (const auto &s : m_sessions) {
            if (!s.isArchived) {
                m_currentId = s.id;
                break;
            }
        }
        if (m_currentId.isEmpty()) {
            createSession("Current Session");
        }
    }
}

SessionManager &SessionManager::instance() {
    static SessionManager s_inst;
    return s_inst;
}

QList<SessionItem> SessionManager::activeSessions() const {
    QList<SessionItem> list;
    for (const auto &s : m_sessions) {
        if (!s.isArchived) list.append(s);
    }
    return list;
}

QList<SessionItem> SessionManager::archivedSessions() const {
    QList<SessionItem> list;
    for (const auto &s : m_sessions) {
        if (s.isArchived) list.append(s);
    }
    return list;
}

SessionItem SessionManager::currentSession() const {
    for (const auto &s : m_sessions) {
        if (s.id == m_currentId) return s;
    }
    if (!m_sessions.isEmpty()) return m_sessions.first();
    return SessionItem();
}

QString SessionManager::currentSessionId() const { return m_currentId; }

QString SessionManager::createSession(const QString &title) {
    SessionItem s;
    s.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    s.title = title.isEmpty() ? "New Session" : title;
    s.createdAt = QDateTime::currentSecsSinceEpoch();
    s.updatedAt = s.createdAt;
    s.isArchived = false;

    auto &sm = SettingsManager::instance();
    s.provider = sm.provider();
    s.model = sm.model(s.provider);

    m_sessions.prepend(s);
    m_currentId = s.id;
    save();

    emit sessionsChanged();
    emit currentSessionChanged(m_currentId);
    return s.id;
}

void SessionManager::setCurrentSession(const QString &sessionId) {
    if (m_currentId == sessionId) return;
    for (const auto &s : m_sessions) {
        if (s.id == sessionId && !s.isArchived) {
            m_currentId = sessionId;
            emit currentSessionChanged(m_currentId);
            return;
        }
    }
}

void SessionManager::renameSession(const QString &sessionId, const QString &newTitle) {
    for (auto &s : m_sessions) {
        if (s.id == sessionId) {
            s.title = newTitle;
            s.updatedAt = QDateTime::currentSecsSinceEpoch();
            save();
            emit sessionsChanged();
            return;
        }
    }
}

void SessionManager::setSessionModel(const QString &sessionId, const QString &provider, const QString &model) {
    for (auto &s : m_sessions) {
        if (s.id == sessionId) {
            s.provider = provider;
            s.model = model;
            s.updatedAt = QDateTime::currentSecsSinceEpoch();
            save();
            emit sessionsChanged();
            return;
        }
    }
}

void SessionManager::archiveSession(const QString &sessionId) {
    for (auto &s : m_sessions) {
        if (s.id == sessionId) {
            s.isArchived = true;
            s.updatedAt = QDateTime::currentSecsSinceEpoch();
            break;
        }
    }

    if (m_currentId == sessionId) {
        m_currentId.clear();
        for (const auto &s : m_sessions) {
            if (!s.isArchived) {
                m_currentId = s.id;
                break;
            }
        }
        if (m_currentId.isEmpty()) {
            createSession("Current Session");
        } else {
            emit currentSessionChanged(m_currentId);
        }
    }

    save();
    emit sessionsChanged();
}

void SessionManager::restoreSession(const QString &sessionId) {
    for (auto &s : m_sessions) {
        if (s.id == sessionId) {
            s.isArchived = false;
            s.updatedAt = QDateTime::currentSecsSinceEpoch();
            m_currentId = s.id;
            break;
        }
    }

    save();
    emit sessionsChanged();
    emit currentSessionChanged(m_currentId);
}

void SessionManager::deleteSessionPermanently(const QString &sessionId) {
    for (int i = 0; i < m_sessions.size(); ++i) {
        if (m_sessions[i].id == sessionId) {
            m_sessions.removeAt(i);
            break;
        }
    }

    if (m_currentId == sessionId) {
        m_currentId.clear();
        for (const auto &s : m_sessions) {
            if (!s.isArchived) {
                m_currentId = s.id;
                break;
            }
        }
        if (m_currentId.isEmpty()) {
            createSession("Current Session");
        } else {
            emit currentSessionChanged(m_currentId);
        }
    }

    save();
    emit sessionsChanged();
}

void SessionManager::appendMessage(const QString &sessionId, const QString &role, const QString &content,
                                   const QString &imagePath) {
    for (auto &s : m_sessions) {
        if (s.id == sessionId) {
            ChatMessageItem msg;
            msg.role = role;
            msg.content = content;
            msg.imagePath = imagePath;
            msg.timestamp = QDateTime::currentSecsSinceEpoch();
            s.messages.append(msg);
            s.updatedAt = msg.timestamp;

            // Auto-title from the first real user message when the session
            // still has a default title. Tool results and slash commands
            // never make a title (they are plumbing, not the user's intent).
            if ((s.title == "New Session" || s.title == "Current Session" || s.title == "New Chat") &&
                role == "user" &&
                !content.startsWith("Tool result") &&
                !content.startsWith("Tool execution") &&
                !content.startsWith("/")) {
                QString t = content.trimmed();
                if (t.length() > 30) t = t.left(27) + "...";
                s.title = t;
            }

            save();
            emit messagesChanged(sessionId);
            emit sessionsChanged();
            return;
        }
    }
}

QList<ChatMessageItem> SessionManager::getMessages(const QString &sessionId) const {
    for (const auto &s : m_sessions) {
        if (s.id == sessionId) return s.messages;
    }
    return {};
}

void SessionManager::load() {
    m_sessions.clear();
    QFile file(storageFilePath());
    if (!file.open(QIODevice::ReadOnly)) return;

    const auto doc = QJsonDocument::fromJson(file.readAll());
    const auto arr = doc.array();

    for (const auto &v : arr) {
        const auto obj = v.toObject();
        SessionItem s;
        s.id = obj.value("id").toString();
        s.title = obj.value("title").toString();
        s.createdAt = obj.value("createdAt").toVariant().toLongLong();
        s.updatedAt = obj.value("updatedAt").toVariant().toLongLong();
        s.isArchived = obj.value("isArchived").toBool(false);
        s.provider = obj.value("provider").toString();
        s.model = obj.value("model").toString();

        const auto msgs = obj.value("messages").toArray();
        for (const auto &mv : msgs) {
            const auto mObj = mv.toObject();
            ChatMessageItem m;
            m.role = mObj.value("role").toString();
            m.content = mObj.value("content").toString();
            m.imagePath = mObj.value("imagePath").toString();
            m.timestamp = mObj.value("timestamp").toVariant().toLongLong();
            s.messages.append(m);
        }
        if (!s.id.isEmpty()) m_sessions.append(s);
    }
}

void SessionManager::save() {
    QJsonArray arr;
    for (const auto &s : m_sessions) {
        QJsonObject obj;
        obj["id"] = s.id;
        obj["title"] = s.title;
        obj["createdAt"] = s.createdAt;
        obj["updatedAt"] = s.updatedAt;
        obj["isArchived"] = s.isArchived;
        obj["provider"] = s.provider;
        obj["model"] = s.model;

        QJsonArray msgs;
        for (const auto &m : s.messages) {
            QJsonObject mObj;
            mObj["role"] = m.role;
            mObj["content"] = m.content;
            mObj["timestamp"] = m.timestamp;
            if (!m.imagePath.isEmpty()) mObj["imagePath"] = m.imagePath;
            msgs.append(mObj);
        }
        obj["messages"] = msgs;
        arr.append(obj);
    }

    QFile file(storageFilePath());
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(arr).toJson(QJsonDocument::Compact));
    }
}

}  // namespace mcw
