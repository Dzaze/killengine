#include "stealth_profiler.h"

#include <QVariantList>

#include <algorithm>

namespace killcore {

QVariantMap StealthThreat::toVariantMap() const {
    QVariantMap map;
    map["name"] = name;
    map["source"] = source;
    map["detail"] = detail;
    map["riskPoints"] = riskPoints;
    return map;
}

QVariantMap StealthAnalysis::toVariantMap() const {
    QVariantMap map;
    map["riskScore"] = riskScore;
    map["riskLevel"] = riskLevel;
    QVariantList threatList;
    for (const auto& threat : threats) {
        threatList.append(threat.toVariantMap());
    }
    map["threats"] = threatList;
    map["recommendations"] = recommendations;
    return map;
}

const std::vector<StealthProfiler::Signature>& StealthProfiler::knownSignatures() {
    static const std::vector<Signature> signatures = {
        {"beclient", "BattlEye"},
        {"beservice", "BattlEye"},
        {"easyanticheat", "Easy Anti-Cheat"},
        {"vgc.exe", "Riot Vanguard"},
        {"vgk.sys", "Riot Vanguard (kernel)"},
        {"vgtray", "Riot Vanguard"},
        {"pnkbstr", "PunkBuster"},
        {"gamemon.des", "nProtect GameGuard"},
        {"npggsvc", "nProtect GameGuard"},
        {"xigncode", "Xigncode3"},
        {"ahnrpc", "nProtect AhnLab (HackShield)"},
        {"mhyprot", "miHoYo Protect (mhyprot)"},
        {"denuvo", "Denuvo Anti-Cheat"},
    };
    return signatures;
}

StealthAnalysis StealthProfiler::analyze(
    const QStringList& targetModuleNames,
    bool debuggerVisibleToTarget,
    bool antiDebugActive,
    bool processMaskActive,
    bool dllMaskActive) {

    StealthAnalysis result;

    // Détection de modules de protection connus -- une entrée par produit
    // trouvé (pas par module : BattlEye a 2 signatures, on ne veut qu'une
    // seule menace "BattlEye" même si les deux matchent).
    QStringList matchedProducts;
    for (const auto& moduleName : targetModuleNames) {
        const QString lower = moduleName.toLower();
        for (const auto& sig : knownSignatures()) {
            if (lower.contains(sig.moduleSubstring) && !matchedProducts.contains(sig.displayName)) {
                matchedProducts.append(sig.displayName);
                StealthThreat threat;
                threat.name = sig.displayName;
                threat.source = "module";
                threat.detail = moduleName;
                threat.riskPoints = 30;
                result.threats.push_back(threat);
                result.riskScore += threat.riskPoints;
            }
        }
    }

    const bool antiCheatDetected = !matchedProducts.isEmpty();

    if (antiCheatDetected) {
        result.recommendations.append(
            QString("Protection détectée (%1) — activer le profil 'sc2' (antiDebug+processMask+dllMask) si ce n'est pas déjà fait.")
                .arg(matchedProducts.join(", ")));
    }

    // Visibilité du débogueur côté cible.
    if (debuggerVisibleToTarget) {
        StealthThreat threat;
        threat.source = "debugger_visible";
        if (antiDebugActive) {
            // Contradiction : le module est actif mais le flag reste visible
            // -- soit les hooks ont échoué, soit la cible vérifie un chemin
            // non couvert (lecture directe du PEB par exemple).
            threat.name = "Débogueur visible malgré antiDebug actif";
            threat.detail = "antiDebug est actif mais CheckRemoteDebuggerPresent (ou équivalent) retourne toujours vrai côté cible.";
            threat.riskPoints = 40;
            result.recommendations.append(
                "antiDebug actif mais le débogueur reste détectable — les hooks n'ont peut-être pas pris, ou la cible utilise une vérification non couverte (lecture directe du PEB). Envisager le driver kernel si disponible.");
        } else {
            threat.name = "Débogueur visible côté cible";
            threat.detail = "IsDebuggerPresent/CheckRemoteDebuggerPresent retournerait actuellement vrai côté cible.";
            threat.riskPoints = 25;
            result.recommendations.append("Activer le module antiDebug — le débogueur est actuellement détectable par la cible.");
        }
        result.threats.push_back(threat);
        result.riskScore += threat.riskPoints;
    }

    // Masquage insuffisant si une protection est présente.
    if (antiCheatDetected && !processMaskActive) {
        StealthThreat threat;
        threat.name = "Process KillEngine non masqué";
        threat.source = "coverage_gap";
        threat.detail = "processMask inactif alors qu'une protection est présente — KillEngine.exe peut apparaître dans l'énumération de processus de la cible.";
        threat.riskPoints = 15;
        result.threats.push_back(threat);
        result.riskScore += threat.riskPoints;
        result.recommendations.append("Activer processMask — le nom de process KillEngine.exe reste visible alors qu'une protection est présente.");
    }
    if (antiCheatDetected && !dllMaskActive) {
        StealthThreat threat;
        threat.name = "DLLs injectées non masquées";
        threat.source = "coverage_gap";
        threat.detail = "dllMask inactif alors qu'une protection est présente — les DLLs injectées par KillEngine restent visibles dans la liste de modules de la cible.";
        threat.riskPoints = 15;
        result.threats.push_back(threat);
        result.riskScore += threat.riskPoints;
        result.recommendations.append("Activer dllMask — les DLLs injectées par KillEngine restent visibles dans la liste de modules de la cible.");
    }

    if (result.threats.empty()) {
        result.recommendations.append(
            "Aucune signature de protection connue détectée parmi les modules chargés, et débogueur non visible côté cible. "
            "L'absence de signature connue ne garantit pas l'absence de protection : certaines (ex. Warden de Blizzard, utilisé par StarCraft II) "
            "sont non documentées et n'exposent pas de nom de module distinct.");
    }

    result.riskScore = std::clamp(result.riskScore, 0, 100);
    if (result.riskScore < 25) {
        result.riskLevel = "low";
    } else if (result.riskScore < 60) {
        result.riskLevel = "medium";
    } else {
        result.riskLevel = "high";
    }

    return result;
}

} // namespace killcore
