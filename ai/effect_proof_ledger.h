#pragma once

#include <QList>
#include <QString>
#include <QVariantMap>

namespace killai {

/// Niveau de preuve pour une cible suivie (PRODUIT-R, section R1).
///
/// L'ordre déclaré ici définit aussi le rang utilisé pour calculer le
/// "meilleur niveau atteint" par cible -- ne pas réordonner sans mettre à
/// jour `effectProofLevelRank` dans effect_proof_ledger.cpp en conséquence.
enum class EffectProofLevel {
    Unverified,       // aucune preuve enregistrée
    Inconclusive,      // preuve tentée, résultat non tranché
    WriteConfirmed,    // relecture après écriture : les octets/la valeur correspondent
    EffectConfirmed,   // comportement utilisateur attendu réellement observé
    DurableSolution,   // persistance (redémarrage, fin de partie, ...) effectivement testée
};

QString effectProofLevelToString(EffectProofLevel level);
/// Retourne Unverified si `text` ne correspond à aucun niveau connu ; utiliser
/// `effectProofLevelIsKnownString` pour distinguer ce cas d'un Unverified explicite.
EffectProofLevel effectProofLevelFromString(const QString& text);
bool effectProofLevelIsKnownString(const QString& text);

struct EffectProofRecord {
    QString id;
    QString targetLabel;
    QString address;      // optionnel ; sert de clé de regroupement si non vide
    EffectProofLevel level{EffectProofLevel::Unverified};
    QString source;       // ex: "relecture", "observation utilisateur", "test redémarrage"
    QString conditions;   // ex: "après fin de partie", "après redémarrage du jeu"
    QString sessionId;
    QString executableHash; // AM-5 : version du process attaché au moment de l'enregistrement,
                             // vide si aucun process attaché (même convention que
                             // core/profiles/profile_store.h KnowledgeNote::executableHash).
    QString recordedAt;     // AM-5 : horodatage ISO UTC, calculé par le registre.
    QString note;
};

struct EffectProofTargetStatus {
    QString targetKey;
    QString targetLabel;
    QString address;
    EffectProofLevel bestLevel{EffectProofLevel::Unverified};
    QString nextAction;
    QList<EffectProofRecord> history; // plus récent en tête
};

/// Registre déterministe des preuves d'effet (PRODUIT-R, section R1 de
/// docs/POWER_UP_ROADMAP.md).
///
/// Sépare explicitement trois niveaux de preuve pour qu'une simple relecture
/// après écriture (déjà couverte ailleurs par `MemoryWriteResult::verified`,
/// voir core/memory/memory_writer.*) ne soit jamais présentée comme équivalente
/// à un effet réellement observé ou à une persistance testée. Motivation directe :
/// investigation Solitaire XP (docs/PHASE_TRACKER.md, entrée
/// INVESTIGATION-SOLITAIRE-XP-2) où l'affichage manipulé (monté à 5555) divergeait
/// du gain réellement crédité en fin de partie (resté à 15) -- une relecture
/// correcte de l'écriture n'aurait jamais signalé cet écart.
///
/// Volontairement une structure de raisonnement pure, indépendante de l'attache
/// courante -- ne génère aucune preuve elle-même, se contente d'enregistrer ce
/// qu'on lui donne et de synthétiser l'état par cible. Même esprit que
/// killai::InvestigationNotebook (investigation_notebook.h), mais concept
/// distinct : le carnet pondère des hypothèses concurrentes, ce registre trace
/// le niveau de preuve atteint par une action déjà décidée.
class EffectProofLedger {
public:
    /// Enregistre une preuve pour une cible identifiée par `address` si non
    /// vide, sinon par `targetLabel`. Retourne l'id généré (ex. "P1").
    /// `executableHash` est optionnel (rétrocompatible : chaîne vide = aucun
    /// filtrage par version, comportement historique) -- voir AM-5,
    /// docs/PHASE_TRACKER.md, 16/09/2026.
    QString addRecord(const QString& targetLabel, const QString& address, EffectProofLevel level,
                       const QString& source, const QString& conditions, const QString& sessionId,
                       const QString& note, const QString& executableHash = QString());

    QList<EffectProofRecord> records() const { return m_records; }
    void clear();

    /// Vue synthétique : cibles avec effet confirmé ou solution durable
    /// ("known"), cibles avec seulement écriture confirmée / inconclusif /
    /// non vérifié ("uncertain"), et une action suivante suggérée par cible
    /// ainsi qu'une action globale (la plus urgente parmi les cibles incertaines).
    ///
    /// `currentExecutableHash` (AM-5, optionnel) : quand non vide, un
    /// enregistrement dont l'`executableHash` est renseigné et DIFFÉRENT est
    /// exclu du calcul du meilleur niveau/historique pour cette cible -- une
    /// adresse brute réutilisée par coïncidence par un autre exécutable ne doit
    /// jamais faire hériter silencieusement un effet confirmé d'une cible sans
    /// rapport. Un enregistrement sans `executableHash` (aucun process attaché
    /// au moment de l'enregistrement, ex. saisie manuelle historique) reste
    /// toujours inclus : on ne peut pas prouver qu'il est étranger.
    QVariantMap synthesis(const QString& currentExecutableHash = QString()) const;

    /// Statut calculé pour une seule cible (utile pour un affichage ciblé
    /// sans reconstruire toute la synthèse). Même filtrage `currentExecutableHash`
    /// que `synthesis()`.
    EffectProofTargetStatus statusForTarget(const QString& targetKey, const QString& currentExecutableHash = QString()) const;

    static QString targetKeyFor(const QString& targetLabel, const QString& address);

private:
    QList<EffectProofRecord> m_records;
    int m_nextId{1};

    QList<QString> orderedTargetKeys() const;
};

} // namespace killai
