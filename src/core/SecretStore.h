#pragma once

#include <QByteArray>
#include <QString>

namespace mcw {

// Central place for API secrets so they are never stored as plain text.
//
// Backends, in priority order:
//   1. Windows DPAPI (CryptProtectData) — values encrypted inside secrets.json.
//   2. Linux Secret Service (libsecret) — resolved at runtime via dlopen, so
//      libsecret stays optional; only present in secret-service environments.
//   3. Fallback for headless/other Unix systems — an obfuscated, chmod-0600
//      JSON file (reasonable at-rest protection; the keyring path is used
//      whenever libsecret is actually available).
//
// Keys are addressed with stable ids such as "providerKey/openai" and
// "pkmKey/<uuid>". Secrets must be non-empty opaque strings.
class SecretStore {
public:
    static SecretStore &instance();

    void store(const QString &key, const QString &secret);
    QString load(const QString &key) const;  // "" when absent
    void remove(const QString &key);

    // Human-readable backend identity for settings hints and logs.
    enum class Backend { WindowsDPAPI, LinuxKeyring, FallbackFile };
    Backend backend() const;
    QString backendName() const;
    bool usesKeyring() const;

private:
    SecretStore();
    ~SecretStore();

    SecretStore(const SecretStore &) = delete;
    SecretStore &operator=(const SecretStore &) = delete;

    struct Impl;
    Impl *d;
};

}  // namespace mcw