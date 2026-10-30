#include "tools/CoworkTool.h"

#include <QFileInfo>
#include <QJsonDocument>
#include <QStringList>

#include "core/Assets.h"
#include "core/SettingsManager.h"

namespace mcw {

static QString toolExecutablePath() {
#ifdef Q_OS_WIN
    return assets::toolPath("matricX-cowork/matricX-cowork.exe");
#else
    const QString p = assets::toolPath("matricX-cowork/matricX-cowork");
    if (QFileInfo::exists(p)) return p;
    const QString fallback = assets::toolPath("matricX-cowork");
    if (QFileInfo::exists(fallback)) return fallback;
    return p;
#endif
}

CoworkTool::CoworkTool() = default;

CoworkTool::~CoworkTool() {
    if (!m_proc) return;
    if (m_proc->state() == QProcess::Running) {
        m_proc->write("{\"cmd\":\"end_session\"}\n");
        m_proc->closeWriteChannel();
        if (!m_proc->waitForFinished(3000)) {
            m_proc->kill();
            m_proc->waitForFinished(1000);
        }
    }
    delete m_proc;
}

bool CoworkTool::available() const {
    return QFileInfo::exists(toolExecutablePath());
}

QJsonArray CoworkTool::definitions() {
    if (!m_defs.isEmpty() || !available()) return m_defs;
    QProcess p;
    p.start(toolExecutablePath(), {"--describe"});
    if (p.waitForFinished(5000)) {
        m_defs = QJsonDocument::fromJson(p.readAllStandardOutput()).array();
        // shell_exec stays invisible until the user opted in via settings.
        if (!SettingsManager::instance().allowShell()) {
            QJsonArray filtered;
            for (const auto &v : m_defs) {
                if (v.toObject().value("name").toString() != "shell_exec") {
                    filtered.append(v);
                }
            }
            m_defs = filtered;
        }
    }
    return m_defs;
}

bool CoworkTool::sessionActive() const { return m_proc && m_proc->state() == QProcess::Running; }

bool CoworkTool::ensureStarted() {
    if (sessionActive()) return true;
    if (!available()) return false;
    if (m_proc) {
        delete m_proc;
        m_proc = nullptr;
    }
    m_buf.clear();
    m_proc = new QProcess;
    m_proc->setProcessChannelMode(QProcess::SeparateChannels);
    QObject::connect(m_proc, &QProcess::readyReadStandardOutput, m_proc, [this] { onOutput(); });
    QObject::connect(m_proc, &QProcess::finished, m_proc, [this] {
        onOutput();
        failPending("tool session ended");
    });
    QStringList args = {"--glow", "green", "--speed", "normal", "--idle-timeout", "120"};
    if (SettingsManager::instance().allowShell()) args << "--enable-shell";
    m_proc->start(toolExecutablePath(), args);
    return m_proc->waitForStarted(3000);
}

void CoworkTool::call(const QString &tool, const QJsonObject &args, Callback cb) {
    if (!ensureStarted()) {
        cb({{"ok", false}, {"error", "matricX-cowork tool is not installed or built yet"}});
        return;
    }
    QJsonObject req = args;
    const qint64 id = m_nextId++;
    req["id"] = double(id);
    req["cmd"] = tool;
    m_pending.insert(id, std::move(cb));
    m_proc->write(QJsonDocument(req).toJson(QJsonDocument::Compact) + "\n");
}

void CoworkTool::endSession() {
    if (sessionActive()) m_proc->write("{\"cmd\":\"end_session\"}\n");
}

void CoworkTool::onOutput() {
    if (!m_proc) return;
    m_buf += m_proc->readAllStandardOutput();
    int nl;
    while ((nl = m_buf.indexOf('\n')) >= 0) {
        const QByteArray line = m_buf.left(nl).trimmed();
        m_buf.remove(0, nl + 1);
        const QJsonObject obj = QJsonDocument::fromJson(line).object();
        if (obj.isEmpty() || !obj.contains("id") || obj["id"].isNull()) continue;  // events
        const qint64 id = qint64(obj["id"].toDouble());
        auto it = m_pending.find(id);
        if (it == m_pending.end()) continue;
        Callback cb = it.value();
        m_pending.erase(it);
        cb(obj);
    }
}

void CoworkTool::failPending(const QString &why) {
    const auto pending = m_pending;
    m_pending.clear();
    for (const Callback &cb : pending) cb({{"ok", false}, {"error", why}});
}

}  // namespace mcw
