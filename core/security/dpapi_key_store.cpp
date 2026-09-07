#include "security/dpapi_key_store.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincrypt.h>
#endif

namespace killcore {

namespace {

void setOutParams(bool* ok, QString* errorMessage, bool success, const QString& message) {
    if (ok) {
        *ok = success;
    }
    if (errorMessage) {
        *errorMessage = message;
    }
}

#ifdef _WIN32
QString systemErrorMessage(DWORD errorCode) {
    LPWSTR raw = nullptr;
    const DWORD size = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                         FORMAT_MESSAGE_IGNORE_INSERTS,
                                     nullptr,
                                     errorCode,
                                     MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                     reinterpret_cast<LPWSTR>(&raw),
                                     0,
                                     nullptr);
    QString message = size > 0 && raw ? QString::fromWCharArray(raw).trimmed() : QStringLiteral("Erreur Windows inconnue");
    if (raw) {
        LocalFree(raw);
    }
    return message;
}
#endif

} // namespace

QByteArray DpapiKeyStore::encrypt(const QByteArray& plaintext, bool* ok, QString* errorMessage) {
#ifndef _WIN32
    setOutParams(ok, errorMessage, false, QStringLiteral("DPAPI disponible uniquement sur Windows."));
    return {};
#else
    DATA_BLOB input;
    input.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plaintext.constData()));
    input.cbData = static_cast<DWORD>(plaintext.size());

    DATA_BLOB output{};
    const BOOL success = CryptProtectData(&input, L"KillEngine external AI API key", nullptr, nullptr, nullptr,
                                           CRYPTPROTECT_UI_FORBIDDEN, &output);
    if (!success) {
        setOutParams(ok, errorMessage, false,
                     QStringLiteral("Échec du chiffrement DPAPI: %1").arg(systemErrorMessage(GetLastError())));
        return {};
    }

    QByteArray result(reinterpret_cast<const char*>(output.pbData), static_cast<int>(output.cbData));
    LocalFree(output.pbData);
    setOutParams(ok, errorMessage, true, QString());
    return result;
#endif
}

QByteArray DpapiKeyStore::decrypt(const QByteArray& encryptedBlob, bool* ok, QString* errorMessage) {
#ifndef _WIN32
    setOutParams(ok, errorMessage, false, QStringLiteral("DPAPI disponible uniquement sur Windows."));
    return {};
#else
    if (encryptedBlob.isEmpty()) {
        setOutParams(ok, errorMessage, false, QStringLiteral("Blob chiffré vide."));
        return {};
    }

    DATA_BLOB input;
    input.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(encryptedBlob.constData()));
    input.cbData = static_cast<DWORD>(encryptedBlob.size());

    DATA_BLOB output{};
    const BOOL success = CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr,
                                             CRYPTPROTECT_UI_FORBIDDEN, &output);
    if (!success) {
        setOutParams(ok, errorMessage, false,
                     QStringLiteral("Échec du déchiffrement DPAPI: %1").arg(systemErrorMessage(GetLastError())));
        return {};
    }

    QByteArray result(reinterpret_cast<const char*>(output.pbData), static_cast<int>(output.cbData));
    LocalFree(output.pbData);
    setOutParams(ok, errorMessage, true, QString());
    return result;
#endif
}

} // namespace killcore
