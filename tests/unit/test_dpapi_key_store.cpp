#include <gtest/gtest.h>

#include "security/dpapi_key_store.h"

using killcore::DpapiKeyStore;

#ifdef Q_OS_WIN

TEST(DpapiKeyStore, RoundTripsAnApiKeyForTheCurrentWindowsAccount) {
    const QByteArray plaintext = "sk-ant-test-1234567890abcdef";

    bool encryptOk = false;
    QString encryptError;
    const QByteArray encrypted = DpapiKeyStore::encrypt(plaintext, &encryptOk, &encryptError);
    ASSERT_TRUE(encryptOk) << encryptError.toStdString();
    EXPECT_FALSE(encrypted.isEmpty());
    EXPECT_NE(encrypted, plaintext);

    bool decryptOk = false;
    QString decryptError;
    const QByteArray decrypted = DpapiKeyStore::decrypt(encrypted, &decryptOk, &decryptError);
    ASSERT_TRUE(decryptOk) << decryptError.toStdString();
    EXPECT_EQ(decrypted, plaintext);
}

TEST(DpapiKeyStore, RoundTripsEmptyPlaintext) {
    bool encryptOk = false;
    const QByteArray encrypted = DpapiKeyStore::encrypt(QByteArray(), &encryptOk);
    ASSERT_TRUE(encryptOk);

    bool decryptOk = false;
    const QByteArray decrypted = DpapiKeyStore::decrypt(encrypted, &decryptOk);
    ASSERT_TRUE(decryptOk);
    EXPECT_TRUE(decrypted.isEmpty());
}

TEST(DpapiKeyStore, DecryptingCorruptedBlobFailsExplicitlyWithoutCrashing) {
    const QByteArray garbage = "this is not a DPAPI blob";

    bool ok = true;
    QString errorMessage;
    const QByteArray decrypted = DpapiKeyStore::decrypt(garbage, &ok, &errorMessage);

    EXPECT_FALSE(ok);
    EXPECT_TRUE(decrypted.isEmpty());
    EXPECT_FALSE(errorMessage.isEmpty());
}

TEST(DpapiKeyStore, DecryptingEmptyBlobFailsExplicitly) {
    bool ok = true;
    QString errorMessage;
    const QByteArray decrypted = DpapiKeyStore::decrypt(QByteArray(), &ok, &errorMessage);

    EXPECT_FALSE(ok);
    EXPECT_TRUE(decrypted.isEmpty());
    EXPECT_FALSE(errorMessage.isEmpty());
}

#else

TEST(DpapiKeyStore, ReportsUnavailableOnNonWindowsPlatforms) {
    bool ok = true;
    QString errorMessage;
    const QByteArray encrypted = DpapiKeyStore::encrypt("secret", &ok, &errorMessage);

    EXPECT_FALSE(ok);
    EXPECT_TRUE(encrypted.isEmpty());
    EXPECT_FALSE(errorMessage.isEmpty());
}

#endif
