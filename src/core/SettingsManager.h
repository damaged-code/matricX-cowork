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

    QString apiKey(const QString &providerId = QString()) const;
    void setApiKey(const QString &key, const QString &providerId = QString());

    QString model(const QString &providerId = QString()) const;
    void setModel(const QString &model, const QString &providerId = QString());

    QString customBaseUrl(const QString &providerId = QString()) const;
    QString rawCustomBaseUrl(const QString &providerId = QString()) const;
    void setCustomBaseUrl(const QString &url, const QString &providerId = QString());

    QString activeBaseUrl() const;

    bool requireConfirmation() const;
    void setRequireConfirmation(bool req);

    // Per-tool safety: inherit | ask | session | allow
    QString toolSafetyPolicy(const QString &tool) const;
    void setToolSafetyPolicy(const QString &tool, const QString &policy);

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

