#include "core/SettingsManager.h"

#include <QSettings>

namespace mcw {

SettingsManager::SettingsManager() = default;

SettingsManager &SettingsManager::instance() {
    static SettingsManager s_inst;
    return s_inst;
}

const QList<ProviderConfig> &SettingsManager::availableProviders() {
    static const QList<ProviderConfig> s_providers = {
        {
            "gemini",
            "Google Gemini / Antigravity",
            "https://generativelanguage.googleapis.com",
            {"gemini-2.5-flash", "gemini-3.1-pro-preview", "gemini-3.8-flash", "gemini-flash-latest", "gemini-pro-latest"},
            "gemini-2.5-flash",
            "https://aistudio.google.com/apikey"
        },
        {
            "deepseek",
            "DeepSeek",
            "https://api.deepseek.com",
            {"deepseek-chat", "deepseek-reasoner"},
            "deepseek-chat",
            "https://platform.deepseek.com/api_keys"
        },
        {
            "openrouter",
            "OpenRouter",
            "https://openrouter.ai/api/v1",
            {"openrouter/free", "deepseek/deepseek-chat", "deepseek/deepseek-r1", "anthropic/claude-3.7-sonnet", "google/gemini-2.5-flash", "openai/gpt-4o"},
            "deepseek/deepseek-chat",
            "https://openrouter.ai/keys"
        },
        {
            "openai",
            "OpenAI",
            "https://api.openai.com/v1",
            {"gpt-4o", "gpt-4o-mini", "o3-mini", "gpt-4.5-preview"},
            "gpt-4o",
            "https://platform.openai.com/api-keys"
        },
        {
            "anthropic",
            "Anthropic Claude",
            "https://api.anthropic.com/v1",
            {"claude-3-7-sonnet-20250219", "claude-3-5-sonnet-20241022", "claude-3-5-haiku-20241022"},
            "claude-3-7-sonnet-20250219",
            "https://console.anthropic.com/settings/keys"
        },
        {
            "groq",
            "Groq (OpenAI-compatible)",
            "https://api.groq.com/openai/v1",
            {"llama-3.3-70b-versatile", "llama-3.1-8b-instant", "mixtral-8x7b-32768"},
            "llama-3.3-70b-versatile",
            "https://console.groq.com/keys"
        },
        {
            "ollama",
            "Ollama / Local (OpenAI-compatible)",
            "http://localhost:11434/v1",
            {"llama3.3", "qwen2.5-coder", "mistral", "deepseek-r1"},
            "llama3.3",
            "https://ollama.com"
        }
    };
    return s_providers;
}

ProviderConfig SettingsManager::providerConfig(const QString &providerId) {
    for (const auto &p : availableProviders()) {
        if (p.id == providerId) return p;
    }
    return availableProviders().first();
}

QString SettingsManager::provider() const {
    QSettings s("MCowork", "MCowork");
    return s.value("ai/provider", "gemini").toString();
}

void SettingsManager::setProvider(const QString &provider) {
    QSettings s("MCowork", "MCowork");
    s.setValue("ai/provider", provider);
}

QString SettingsManager::apiKey(const QString &providerId) const {
    const QString pid = providerId.isEmpty() ? provider() : providerId;
    QSettings s("MCowork", "MCowork");
    return s.value(QString("ai/%1/apiKey").arg(pid), QString()).toString();
}

void SettingsManager::setApiKey(const QString &key, const QString &providerId) {
    const QString pid = providerId.isEmpty() ? provider() : providerId;
    QSettings s("MCowork", "MCowork");
    s.setValue(QString("ai/%1/apiKey").arg(pid), key);
}

QString SettingsManager::model(const QString &providerId) const {
    const QString pid = providerId.isEmpty() ? provider() : providerId;
    const auto cfg = providerConfig(pid);
    QSettings s("MCowork", "MCowork");
    return s.value(QString("ai/%1/model").arg(pid), cfg.defaultModel).toString();
}

void SettingsManager::setModel(const QString &model, const QString &providerId) {
    const QString pid = providerId.isEmpty() ? provider() : providerId;
    QSettings s("MCowork", "MCowork");
    s.setValue(QString("ai/%1/model").arg(pid), model);
}

QString SettingsManager::customBaseUrl(const QString &providerId) const {
    const QString pid = providerId.isEmpty() ? provider() : providerId;
    const auto cfg = providerConfig(pid);
    QSettings s("MCowork", "MCowork");
    const QString val = s.value(QString("ai/%1/baseUrl").arg(pid), QString()).toString().trimmed();
    return val.isEmpty() ? cfg.defaultBaseUrl : val;
}

QString SettingsManager::rawCustomBaseUrl(const QString &providerId) const {
    const QString pid = providerId.isEmpty() ? provider() : providerId;
    QSettings s("MCowork", "MCowork");
    return s.value(QString("ai/%1/baseUrl").arg(pid), QString()).toString();
}

void SettingsManager::setCustomBaseUrl(const QString &url, const QString &providerId) {
    const QString pid = providerId.isEmpty() ? provider() : providerId;
    QSettings s("MCowork", "MCowork");
    s.setValue(QString("ai/%1/baseUrl").arg(pid), url);
}

QString SettingsManager::activeBaseUrl() const {
    return customBaseUrl(provider());
}

bool SettingsManager::requireConfirmation() const {
    QSettings s("MCowork", "MCowork");
    return s.value("safety/requireConfirmation", false).toBool();
}

void SettingsManager::setRequireConfirmation(bool req) {
    QSettings s("MCowork", "MCowork");
    s.setValue("safety/requireConfirmation", req);
}

QString SettingsManager::toolSafetyPolicy(const QString &tool) const {
    QSettings s("MCowork", "MCowork");
    const QString policy = s.value("safety/toolPolicies/" + tool, "inherit").toString();
    if (policy == "inherit" || policy == "ask" || policy == "session" || policy == "allow") return policy;
    return "inherit";
}

void SettingsManager::setToolSafetyPolicy(const QString &tool, const QString &policy) {
    if (tool.trimmed().isEmpty()) return;
    if (policy != "inherit" && policy != "ask" && policy != "session" && policy != "allow") return;
    QSettings s("MCowork", "MCowork");
    s.setValue("safety/toolPolicies/" + tool, policy);
}

QString SettingsManager::windowControlsPlace() const {
    QSettings s("MCowork", "MCowork");
    return s.value("ui/windowControlsPlace", "titlebar").toString();
}

void SettingsManager::setWindowControlsPlace(const QString &place) {
    QSettings s("MCowork", "MCowork");
    s.setValue("ui/windowControlsPlace", place);
}

QString SettingsManager::windowControlsPosition() const {
    QSettings s("MCowork", "MCowork");
    return s.value("ui/windowControlsPosition", "right").toString();
}

void SettingsManager::setWindowControlsPosition(const QString &position) {
    QSettings s("MCowork", "MCowork");
    s.setValue("ui/windowControlsPosition", position);
}

QString SettingsManager::windowControlsShape() const {
    QSettings s("MCowork", "MCowork");
    return s.value("ui/windowControlsShape", "circle").toString();
}

void SettingsManager::setWindowControlsShape(const QString &shape) {
    QSettings s("MCowork", "MCowork");
    s.setValue("ui/windowControlsShape", shape);
}

QString SettingsManager::windowControlsSize() const {
    QSettings s("MCowork", "MCowork");
    return s.value("ui/windowControlsSize", "medium").toString();
}

void SettingsManager::setWindowControlsSize(const QString &size) {
    QSettings s("MCowork", "MCowork");
    s.setValue("ui/windowControlsSize", size);
}

bool SettingsManager::showReasoning() const {
    QSettings s("MCowork", "MCowork");
    return s.value("ui/showReasoning", true).toBool();
}

void SettingsManager::setShowReasoning(bool show) {
    QSettings s("MCowork", "MCowork");
    s.setValue("ui/showReasoning", show);
}

}  // namespace mcw

