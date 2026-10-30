#include "core/SecretStore.h"

#include <cstdio>

#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QIODevice>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QSysInfo>

#ifdef Q_OS_WIN
#include <windows.h>
#include <wincrypt.h>
#pragma comment(lib, "crypt32.lib")
#else
#include <dlfcn.h>
#include <sys/stat.h>
#endif

namespace mcw {

// ---------------------------------------------------------------------------
// File locations
// ---------------------------------------------------------------------------

static QString secretsDir() {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    return dir;
}

static QString secretsFilePath() {
    return QDir(secretsDir()).filePath("secrets.json");
}

// ---------------------------------------------------------------------------
// DPAPI (Windows)
// ---------------------------------------------------------------------------

#ifdef Q_OS_WIN

static QByteArray dpapiProtectRaw(const QByteArray &plain) {
    DATA_BLOB in;
    in.cbData = (DWORD)plain.size();
    in.pbData = (BYTE *)plain.data();
    DATA_BLOB out;
    if (!CryptProtectData(&in, L"M-Cowork secret", nullptr, nullptr, nullptr, 0, &out)) {
        return QByteArray();
    }
    QByteArray result(reinterpret_cast<const char *>(out.pbData), (int)out.cbData);
    LocalFree(out.pbData);
    return result;
}

static QByteArray dpapiUnprotectRaw(const QByteArray &blob) {
    DATA_BLOB in;
    in.cbData = (DWORD)blob.size();
    in.pbData = (BYTE *)blob.data();
    DATA_BLOB out;
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &out)) {
        return QByteArray();
    }
    QByteArray result(reinterpret_cast<const char *>(out.pbData), (int)out.cbData);
    LocalFree(out.pbData);
    return result;
}

#endif  // Q_OS_WIN

// ---------------------------------------------------------------------------
// Linux: libsecret via dlopen (no hard dependency)
// ---------------------------------------------------------------------------

#ifndef Q_OS_WIN

// Subset of the libsecret v1 API called through function pointers. Everything
// is resolved at runtime so libsecret stays an optional dependency.
struct GErrorT {
    unsigned long domain;
    int code;
    char *message;
};

// ABI-compatible SecretSchema (libsecret-1): attributes are {const char*,
// int} records terminated by a nullptr name, plus trailing padding the
// library never reads.
struct SecretSchemaAttr {
    const char *name;  // nullptr terminates the table
    int type;          // 0 = SECRET_SCHEMA_ATTRIBUTE_STRING
};

struct SecretSchema {
    const char *name;
    int flags;
    SecretSchemaAttr attributes[32];
};

typedef void *(*SecretLookupSyncFn)(const void *, void *, GErrorT **, ...);
typedef int (*SecretStoreSyncFn)(const void *, const void *, const void *, const void *,
                                 void *, GErrorT **, ...);
typedef int (*SecretClearSyncFn)(const void *, void *, GErrorT **, ...);
typedef void (*SecretFreeFn)(char *);

static SecretSchema *builtinSchema() {
    static SecretSchema s = {};
    static bool inited = false;
    if (!inited) {
        s.name = "xyz.xonra.matricx.cowork";
        s.flags = 0;
        s.attributes[0] = {"application", 0};
        s.attributes[1] = {"account", 0};
        // s.attributes[2..] stay zeroed: a nullptr name ends the table.
        inited = true;
    }
    return &s;
}

#endif  // !Q_OS_WIN

// ---------------------------------------------------------------------------
// Fallback: obfuscated at-rest file (chmod 600)
// ---------------------------------------------------------------------------

#ifndef Q_OS_WIN

static QByteArray fallbackKeyMaterial(const QString &key) {
    // A machine + entry specific keystream. This is obfuscation, not KMS-grade
    // crypto: it keeps casual or backup scans from revealing secrets at rest,
    // while the keyring path is the real protection whenever it is available.
    QByteArray nonce = QSysInfo::machineUniqueId();
    if (nonce.isEmpty()) nonce = "matricx-cowork";
    nonce += ':';
    nonce += key.toUtf8();
    return QCryptographicHash::hash(nonce, QCryptographicHash::Sha256);
}

static QByteArray fallbackXor(const QByteArray &data, const QString &key) {
    const QByteArray pad = fallbackKeyMaterial(key);
    QByteArray out(data.size(), '\0');
    for (int i = 0; i < data.size(); ++i) {
        out[i] = data[i] ^ pad[i % pad.size()];
    }
    return out;
}

#endif  // !Q_OS_WIN

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

struct SecretStore::Impl {
    Backend backend = Backend::FallbackFile;
    QHash<QString, QString> cache;

#ifndef Q_OS_WIN
    // dlopen state (Linux only); the typedefs live in the #ifndef block above.
    void *lib = nullptr;
    SecretLookupSyncFn lookup = nullptr;
    SecretStoreSyncFn store = nullptr;
    SecretClearSyncFn clear = nullptr;
    SecretFreeFn freePassword = nullptr;
#endif

    bool scanFile(QJsonObject *out) const {
        QFile f(secretsFilePath());
        if (!f.open(QIODevice::ReadOnly)) return false;
        QJsonObject obj = QJsonDocument::fromJson(f.readAll()).object();
        if (out) *out = obj;
        return true;
    }

    bool writeFile(const QJsonObject &obj) {
        QFile f(secretsFilePath());
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
        f.write(QJsonDocument(obj).toJson(QJsonDocument::Compact));
        f.close();
        // Only the user should read the secrets file.
        QFile::setPermissions(secretsFilePath(), QFile::ReadOwner | QFile::WriteOwner);
        return true;
    }
};

SecretStore::SecretStore() {
    d = new Impl;
#ifdef Q_OS_WIN
    d->backend = Backend::WindowsDPAPI;
#else
    for (const char *candidate : {"libsecret-1.so.0", "libsecret-1.so"}) {
        if ((d->lib = dlopen(candidate, RTLD_LAZY | RTLD_LOCAL))) break;
    }
    if (d->lib) {
        d->lookup = (SecretLookupSyncFn)dlsym(d->lib, "secret_password_lookup_sync");
        d->store = (SecretStoreSyncFn)dlsym(d->lib, "secret_password_store_sync");
        d->clear = (SecretClearSyncFn)dlsym(d->lib, "secret_password_clear_sync");
        d->freePassword = (SecretFreeFn)dlsym(d->lib, "secret_password_free");
        const bool ok = d->lookup && d->store && d->clear && d->freePassword;
        if (!ok) {
            dlclose(d->lib);
            d->lib = nullptr;
        }
    }
    d->backend = d->lib ? Backend::LinuxKeyring : Backend::FallbackFile;

    if (d->backend == Backend::FallbackFile) {
        ::chmod(secretsFilePath().toUtf8().constData(), 0600);
    }
#endif
}

SecretStore::~SecretStore() {
#ifndef Q_OS_WIN
    if (d->lib) {
        dlclose(d->lib);
    }
#endif
    delete d;
}

SecretStore &SecretStore::instance() {
    static SecretStore s_inst;
    return s_inst;
}

SecretStore::Backend SecretStore::backend() const { return d->backend; }

QString SecretStore::backendName() const {
    switch (d->backend) {
        case Backend::WindowsDPAPI: return "Windows DPAPI";
        case Backend::LinuxKeyring: return "Linux keyring (libsecret)";
        default: return "obfuscated file";
    }
}

bool SecretStore::usesKeyring() const {
    return d->backend == Backend::LinuxKeyring;
}

void SecretStore::store(const QString &key, const QString &secret) {
    if (key.isEmpty() || secret.isEmpty()) return;

#ifdef Q_OS_WIN
    QJsonObject root;
    d->scanFile(&root);
    const QByteArray blob = dpapiProtectRaw(secret.toUtf8());
    if (blob.isEmpty()) {
        qWarning("SecretStore: DPAPI protect failed for %s", qUtf8Printable(key));
        return;
    }
    root.insert(key, QString::fromLatin1(blob.toBase64()));
    d->writeFile(root);
    d->cache.insert(key, secret);
#else
    if (d->backend == Backend::LinuxKeyring) {
        GErrorT *err = nullptr;
        const QByteArray label = QString("M-Cowork %1").arg(key).toUtf8();
        const QByteArray password = secret.toUtf8();
        const QByteArray app = "M-Cowork";
        const QByteArray account = key.toUtf8();
        const int rc = d->store(builtinSchema(), /*collection*/ nullptr,
                                label.constData(), password.constData(),
                                /*cancellable*/ nullptr, &err,
                                "application", app.constData(),
                                "account", account.constData(),
                                static_cast<const char *>(nullptr));
        if (rc != 0) {
            d->cache.insert(key, secret);
            return;  // stored in the keyring
        }
        if (err && err->message) {
            qWarning("SecretStore: libsecret store failed (%s); using file fallback",
                     err->message);
        }
        // Fall through to the file store on failure.
    }

    // Fallback / Linux file store: one JSON file of obfuscated-by-key values.
    QJsonObject root;
    d->scanFile(&root);
    root.insert(key, QString::fromLatin1(
                         fallbackXor(secret.toUtf8(), key).toBase64()));
    d->writeFile(root);
    d->cache.insert(key, secret);
#endif
}

QString SecretStore::load(const QString &key) const {
    if (key.isEmpty()) return QString();
    if (d->cache.contains(key)) return d->cache.value(key);

#ifdef Q_OS_WIN
    QJsonObject root;
    if (!d->scanFile(&root)) return QString();
    const QString blob = root.value(key).toString();
    if (blob.isEmpty()) return QString();
    const QByteArray raw = dpapiUnprotectRaw(QByteArray::fromBase64(blob.toLatin1()));
    if (raw.isEmpty()) return QString();
    const QString secret = QString::fromUtf8(raw);
    d->cache.insert(key, secret);
    return secret;
#else
    if (d->backend == Backend::LinuxKeyring) {
        GErrorT *err = nullptr;
        const QByteArray app = "M-Cowork";
        const QByteArray account = key.toUtf8();
        char *value = (char *)d->lookup(builtinSchema(), /*cancellable*/ nullptr, &err,
                                        "application", app.constData(),
                                        "account", account.constData(),
                                        static_cast<const char *>(nullptr));
        if (value && *value) {
            const QString secret = QString::fromUtf8(value);
            d->freePassword(value);
            d->cache.insert(key, secret);
            return secret;
        }
        if (value) d->freePassword(value);
        // Mirror what store() did: keyring write failures fell back to the
        // file, so a keyring miss must try the file before giving up.
    }

    QJsonObject root;
    if (!d->scanFile(&root)) return QString();
    const QString blob = root.value(key).toString();
    if (blob.isEmpty()) return QString();
    const QByteArray raw = fallbackXor(QByteArray::fromBase64(blob.toLatin1()), key);
    const QString secret = QString::fromUtf8(raw);
    d->cache.insert(key, secret);
    return secret;
#endif
}

void SecretStore::remove(const QString &key) {
    if (key.isEmpty()) return;

#ifdef Q_OS_WIN
    QJsonObject root;
    if (d->scanFile(&root) && root.contains(key)) {
        root.remove(key);
        d->writeFile(root);
    }
    d->cache.remove(key);
#else
    if (d->backend == Backend::LinuxKeyring) {
        GErrorT *err = nullptr;
        const QByteArray app = "M-Cowork";
        const QByteArray account = key.toUtf8();
        d->clear(builtinSchema(), /*cancellable*/ nullptr, &err,
                 "application", app.constData(),
                 "account", account.constData(),
                 static_cast<const char *>(nullptr));
    }
    QJsonObject root;
    if (d->scanFile(&root) && root.contains(key)) {
        root.remove(key);
        d->writeFile(root);
    }
    d->cache.remove(key);
#endif
}

}  // namespace mcw
