#pragma once

#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <vector>

namespace killcore {

/// Une menace de détectabilité identifiée (module de protection connu,
/// débogueur visible côté cible, etc.).
struct StealthThreat {
    QString name;      ///< Nom du produit détecté (ex: "BattlEye"), ou de la catégorie de risque.
    QString source;     ///< "module" | "debugger_visible" | "coverage_gap"
    QString detail;     ///< Détail concret (ex: nom de module trouvé).
    int riskPoints{0};

    QVariantMap toVariantMap() const;
};

/// Résultat complet d'une analyse de détectabilité.
struct StealthAnalysis {
    int riskScore{0};            ///< 0-100, clampé.
    QString riskLevel;           ///< "low" | "medium" | "high"
    std::vector<StealthThreat> threats;
    QStringList recommendations;

    QVariantMap toVariantMap() const;
};

/// Analyse la détectabilité de l'attache KillEngine sur un process cible et
/// recommande quels modules stealth activer -- couche au-dessus des 3
/// modules existants (antiDebug/processMask/dllMask, voir
/// ApplicationController::applyStealthMode), qui restent un choix binaire de
/// profil fixe ("sc2"/"default"/"minimal") sans analyse de la cible réelle.
///
/// Logique pure, sans aucun appel Win32 : le caller (ApplicationController)
/// rassemble les entrées réelles (énumération de modules du process cible via
/// ProcessEnumerator, CheckRemoteDebuggerPresent) et cette fonction ne fait
/// que du scoring déterministe -- directement testable en isolation.
class StealthProfiler {
public:
    /// @param targetModuleNames Noms des modules chargés dans le process cible
    ///   (juste le nom de fichier, ex: "BEService.exe" -- pas le chemin complet).
    /// @param debuggerVisibleToTarget Résultat d'un CheckRemoteDebuggerPresent
    ///   (ou équivalent) sur le process cible -- true si un débogueur serait
    ///   actuellement visible par une vérification standard côté cible.
    /// @param antiDebugActive État courant du module antiDebug de KillEngine.
    /// @param processMaskActive État courant du module processMask.
    /// @param dllMaskActive État courant du module dllMask.
    static StealthAnalysis analyze(
        const QStringList& targetModuleNames,
        bool debuggerVisibleToTarget,
        bool antiDebugActive,
        bool processMaskActive,
        bool dllMaskActive);

    /// Table des signatures anti-cheat/protection connues (sous-chaîne de nom
    /// de module -> nom affiché), exposée pour inspection/tests. Connaissance
    /// publique standard de la communauté sécurité/reverse (BattlEye,
    /// EasyAntiCheat, Riot Vanguard, PunkBuster, nProtect GameGuard,
    /// Xigncode3, Denuvo, mhyprot) -- volontairement pas exhaustive : plusieurs
    /// protections (ex. Warden de Blizzard, utilisé par SC2) sont non
    /// documentées et n'exposent pas de nom de module distinct, d'où la
    /// recommandation "coverage_gap" quand rien n'est détecté (voir analyze()).
    struct Signature {
        QString moduleSubstring;
        QString displayName;
    };
    static const std::vector<Signature>& knownSignatures();
};

} // namespace killcore
