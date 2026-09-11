#include "stealth_profiler.h"

#include "localization/localization.h"

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
            KE_TXT("Protection détectée (%1) — activer le profil 'sc2' (antiDebug+processMask+dllMask) si ce n'est pas déjà fait.",
                "Protection detected (%1) — activate the 'sc2' profile (antiDebug+processMask+dllMask) if not already done.")
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
            threat.name = KE_TXT("Débogueur visible malgré antiDebug actif", "Debugger visible despite antiDebug being active");
            threat.detail = KE_TXT("antiDebug est actif mais CheckRemoteDebuggerPresent (ou équivalent) retourne toujours vrai côté cible.",
                "antiDebug is active but CheckRemoteDebuggerPresent (or equivalent) still returns true on the target side.");
            threat.riskPoints = 40;
            result.recommendations.append(
                KE_TXT("antiDebug actif mais le débogueur reste détectable — les hooks n'ont peut-être pas pris, ou la cible utilise une vérification non couverte (lecture directe du PEB). Envisager le driver kernel si disponible.",
                    "antiDebug is active but the debugger remains detectable — the hooks may not have taken effect, or the target uses an uncovered check (direct PEB read). Consider the kernel driver if available."));
        } else {
            threat.name = KE_TXT("Débogueur visible côté cible", "Debugger visible on the target side");
            threat.detail = KE_TXT("IsDebuggerPresent/CheckRemoteDebuggerPresent retournerait actuellement vrai côté cible.",
                "IsDebuggerPresent/CheckRemoteDebuggerPresent would currently return true on the target side.");
            threat.riskPoints = 25;
            result.recommendations.append(KE_TXT("Activer le module antiDebug — le débogueur est actuellement détectable par la cible.",
                "Activate the antiDebug module — the debugger is currently detectable by the target."));
        }
        result.threats.push_back(threat);
        result.riskScore += threat.riskPoints;
    }

    // Masquage insuffisant si une protection est présente.
    if (antiCheatDetected && !processMaskActive) {
        StealthThreat threat;
        threat.name = KE_TXT("Process KillEngine non masqué", "KillEngine process not masked");
        threat.source = "coverage_gap";
        threat.detail = KE_TXT("processMask inactif alors qu'une protection est présente — KillEngine.exe peut apparaître dans l'énumération de processus de la cible.",
            "processMask inactive while a protection is present — KillEngine.exe may show up in the target's process enumeration.");
        threat.riskPoints = 15;
        result.threats.push_back(threat);
        result.riskScore += threat.riskPoints;
        result.recommendations.append(KE_TXT("Activer processMask — le nom de process KillEngine.exe reste visible alors qu'une protection est présente.",
            "Activate processMask — the KillEngine.exe process name remains visible while a protection is present."));
    }
    if (antiCheatDetected && !dllMaskActive) {
        StealthThreat threat;
        threat.name = KE_TXT("DLLs injectées non masquées", "Injected DLLs not masked");
        threat.source = "coverage_gap";
        threat.detail = KE_TXT("dllMask inactif alors qu'une protection est présente — les DLLs injectées par KillEngine restent visibles dans la liste de modules de la cible.",
            "dllMask inactive while a protection is present — DLLs injected by KillEngine remain visible in the target's module list.");
        threat.riskPoints = 15;
        result.threats.push_back(threat);
        result.riskScore += threat.riskPoints;
        result.recommendations.append(KE_TXT("Activer dllMask — les DLLs injectées par KillEngine restent visibles dans la liste de modules de la cible.",
            "Activate dllMask — DLLs injected by KillEngine remain visible in the target's module list."));
    }

    if (result.threats.empty()) {
        result.recommendations.append(
            KE_TXT("Aucune signature de protection connue détectée parmi les modules chargés, et débogueur non visible côté cible. "
            "L'absence de signature connue ne garantit pas l'absence de protection : certaines (ex. Warden de Blizzard, utilisé par StarCraft II) "
            "sont non documentées et n'exposent pas de nom de module distinct.",
            "No known protection signature detected among the loaded modules, and the debugger is not visible on the target side. "
            "The absence of a known signature doesn't guarantee the absence of protection: some (e.g. Blizzard's Warden, used by StarCraft II) "
            "are undocumented and don't expose a distinct module name."));
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
