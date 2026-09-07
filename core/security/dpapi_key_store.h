#pragma once

#include <QByteArray>
#include <QString>

namespace killcore {

// PHASE (Backend IA externe, T1) : chiffrement/déchiffrement d'un secret
// (ex: clé API Anthropic) via l'API Windows DPAPI (CryptProtectData/
// CryptUnprotectData), liée au compte Windows de l'utilisateur courant.
// Pas de mot de passe supplémentaire à gérer : seul le compte Windows qui a
// chiffré le blob peut le déchiffrer. Le blob chiffré retourné est destiné à
// être stocké tel quel (ex: dans QSettings) — voir docs/EXTERNAL_AI_BACKEND_ROADMAP.md.
class DpapiKeyStore {
public:
    // Chiffre `plaintext` pour le compte Windows courant. Retourne le blob
    // chiffré, ou un QByteArray vide en cas d'échec — `ok` (si fourni) et
    // `errorMessage` (si fourni) rapportent alors la cause explicite,
    // jamais de fallback silencieux (voir décision roadmap #6).
    static QByteArray encrypt(const QByteArray& plaintext, bool* ok = nullptr, QString* errorMessage = nullptr);

    // Déchiffre un blob produit par encrypt() pour le compte Windows courant.
    // Retourne un QByteArray vide en cas d'échec (blob corrompu, chiffré par
    // un autre compte, etc.) — mêmes paramètres de diagnostic que encrypt().
    static QByteArray decrypt(const QByteArray& encryptedBlob, bool* ok = nullptr, QString* errorMessage = nullptr);
};

} // namespace killcore
