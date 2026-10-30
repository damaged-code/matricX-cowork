#pragma once

#include <QDateTime>
#include <QObject>
#include <QString>
#include <functional>

class QTcpServer;
class QTcpSocket;
class QNetworkAccessManager;
class QTimer;

namespace mcw {

struct GoogleTokenInfo {
    bool valid = false;
    QString accessToken;
    QString refreshToken;
    QString email;
    qint64 expirySec = 0;
    QString error;
};

// Implements Google OAuth authorization_code loopback flow for Antigravity/Cloud Code:
// 1. Spawns local HTTP server on http://localhost:<port>/oauth-callback
// 2. Opens system browser to Google OAuth consent screen
// 3. Receives authorization code and exchanges it for tokens using Google OAuth endpoints
class AntigravityAuth : public QObject {
    Q_OBJECT
public:
    using AuthCallback = std::function<void(const GoogleTokenInfo &info)>;

    static AntigravityAuth &instance();

    // Start local loopback OAuth server and launch system browser
    void startAuthFlow(AuthCallback callback);

    // Refresh existing token
    void refreshToken(const QString &refreshToken, AuthCallback callback);

    // Stored credentials
    static bool hasToken();
    static QString accessToken();
    static QString refreshToken();
    static QString email();
    static void saveToken(const GoogleTokenInfo &info);
    static void clearToken();

private:
    AntigravityAuth();
    ~AntigravityAuth();

    void exchangeCodeForTokens(const QString &code, const QString &redirectUri);
    void handleIncomingConnection();
    void sendHtmlResponse(QTcpSocket *socket, bool success, const QString &message);

    QTcpServer *m_server = nullptr;
    QNetworkAccessManager *m_nam = nullptr;
    QTimer *m_authTimer = nullptr;
    AuthCallback m_pendingCallback;
    QString m_expectedState;
    quint16 m_port = 0;
};

}  // namespace mcw
