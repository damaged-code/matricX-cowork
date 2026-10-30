#pragma once

#include <QString>
#include <QStringList>

namespace mcw {

struct ProviderConfig {
    QString id;
    QString name;
    QString defaultBaseUrl;
    QStringList models;
    QString defaultModel;
    QString docsUrl;
};

class SettingsManager {
public:
    static SettingsManager &instance();

    static const QList<ProviderConfig> &availableProviders();
    static ProviderConfig providerConfig(const QString &providerId);

    QString provider() const;
    void setProvider(const QString &provider);

    // API keys live in the SecretStore (OS keyring / DPAPI), never in plain
    // QSettings. Legacy plaintext values migrate on first read.
    QString apiKey(const QString &providerId = QString()) const;
    void setApiKey(const QString &key, const QString &providerId = QString());

    QString model(const QString &providerId = QString()) const;
    void setModel(const QString &model, const QString &providerId = QString());

    QString customBaseUrl(const QString &providerId = QString()) const;
    QString rawCustomBaseUrl(const QString &providerId = QString()) const;
    void setCustomBaseUrl(const QString &url, const QString &providerId = QString());

    QString activeBaseUrl() const;

    bool requireConfirmation() const;   // default: true
    void setRequireConfirmation(bool req);

    // Gate for the engine's shell_exec tool. Off by default; when enabled the
    // engine is launched with --enable-shell and the AI is offered the tool.
    bool allowShell() const;
    void setAllowShell(bool allow);

    // Title-bar chrome configuration
    QString windowControlsPlace() const;     // "titlebar" | "sidebar"
    void setWindowControlsPlace(const QString &place);
    QString windowControlsPosition() const;  // "left" | "right"
    void setWindowControlsPosition(const QString &position);
    QString windowControlsShape() const;     // "circle" | "rounded" | "square"
    void setWindowControlsShape(const QString &shape);
    QString windowControlsSize() const;      // "small" | "medium" | "large"
    void setWindowControlsSize(const QString &size);

    // Show the model's reasoning inside a thinking bubble
    bool showReasoning() const;
    void setShowReasoning(bool show);

private:
    SettingsManager();
};

}  // namespace mcw

