#include "smart_search_manager.h"

#include "application_controller.h"
#include "auto_resolver.h"
#include "claude_chat_manager.h"
#include "freeze_hotkey_overlay_manager.h"
#include "logging/logger.h"
#include "memory/memory_reader.h"
#include "pointer/pointer_chain.h"
#include "process/process_enumerator.h"
#include "profiles/profile_store.h"
#include "query_text_utils.h"
#include "scanner/scan_engine.h"
#include "scanner/scan_types.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QHash>
#include <QRegularExpression>
#include <QSettings>

#include <algorithm>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <iphlpapi.h>
#endif

namespace killengine {

namespace {

constexpr size_t kAutoWriteCandidateLimit = 4;

enum class SmartSearchIntentKind {
    Unknown,
    ResetContext,
    ExactScan,
    GuidedScan,
    RefineScan,
    ActivateMemoryTargets,
    WriteMemoryTargets,
    FreezeMemoryTargets,
    RewriteLastTargets,
    WriteProfileTargets,
    ClearActiveTargets,
    ReportBadTargets,
    ReportGoodTargets,
    AnswerTraceUiStringPrompt,
    AnswerTraceUiFilterPrompt,
    AnswerWriteTargetPrompt,
};

struct SmartSearchIntent {
    SmartSearchIntentKind kind{SmartSearchIntentKind::Unknown};
    QStringList numbers;
    QStringList addresses;
    bool resetContext{false};
    QString rationale;
};

QString smartSearchIntentKindToString(SmartSearchIntentKind kind) {
    switch (kind) {
        case SmartSearchIntentKind::Unknown:
            return "Unknown";
        case SmartSearchIntentKind::ResetContext:
            return "ResetContext";
        case SmartSearchIntentKind::ExactScan:
            return "ExactScan";
        case SmartSearchIntentKind::GuidedScan:
            return "GuidedScan";
        case SmartSearchIntentKind::RefineScan:
            return "RefineScan";
        case SmartSearchIntentKind::ActivateMemoryTargets:
            return "ActivateMemoryTargets";
        case SmartSearchIntentKind::WriteMemoryTargets:
            return "WriteMemoryTargets";
        case SmartSearchIntentKind::FreezeMemoryTargets:
            return "FreezeMemoryTargets";
        case SmartSearchIntentKind::RewriteLastTargets:
            return "RewriteLastTargets";
        case SmartSearchIntentKind::WriteProfileTargets:
            return "WriteProfileTargets";
        case SmartSearchIntentKind::ClearActiveTargets:
            return "ClearActiveTargets";
        case SmartSearchIntentKind::ReportBadTargets:
            return "ReportBadTargets";
        case SmartSearchIntentKind::ReportGoodTargets:
            return "ReportGoodTargets";
        case SmartSearchIntentKind::AnswerTraceUiStringPrompt:
            return "AnswerTraceUiStringPrompt";
        case SmartSearchIntentKind::AnswerTraceUiFilterPrompt:
            return "AnswerTraceUiFilterPrompt";
        case SmartSearchIntentKind::AnswerWriteTargetPrompt:
            return "AnswerWriteTargetPrompt";
    }
    return "Unknown";
}


QVariantMap autoResolveStepToVariantMap(const killai::AutoResolveStep& step, int index) {
    QVariantMap entry;
    entry["index"] = index;
    entry["type"] = killai::stepTypeToString(step.type);
    entry["description"] = step.description;
    entry["params"] = step.params;
    entry["completed"] = step.completed;
    entry["success"] = step.success;
    entry["result"] = step.result;
    return entry;
}

QVariantList autoResolveStepsToVariantList(const QList<killai::AutoResolveStep>& steps) {
    QVariantList list;
    for (int i = 0; i < steps.size(); ++i) {
        list.append(autoResolveStepToVariantMap(steps[i], i + 1));
    }
    return list;
}


QString autoResolverGameKey(QString processName) {
    processName = processName.trimmed().toLower();
    if (processName.isEmpty()) {
        return "unknown";
    }
    processName.replace(QRegularExpression("[^a-z0-9_.-]+"), "_");
    return processName.left(80);
}

bool isStarCraftLikeProcessName(const QString& processName) {
    const QString normalized = processName.toLower();
    return normalized.contains("sc2") || normalized.contains("starcraft");
}

bool parseHexAddress(const QString& addressHex, uint64_t* address) {
    if (!address) {
        return false;
    }

    QString normalized = addressHex.trimmed();
    if (normalized.startsWith("0x", Qt::CaseInsensitive)) {
        normalized = normalized.mid(2);
    }

    bool ok = false;
    const uint64_t parsed = normalized.toULongLong(&ok, 16);
    if (!ok || parsed == 0) {
        return false;
    }

    *address = parsed;
    return true;
}

// Resout une adresse absolue en (module, offset relatif) — la seule forme qui
// survit a un redemarrage/ASLR. Utilise pour convertir les adresses brutes que
// remonte le pipeline auto-write (valables uniquement pour la session en
// cours) en quelque chose de reutilisable au prochain lancement du meme jeu
// (cf. logAiAudit / rememberedPatterns).
bool resolveModuleOffset(uint32_t pid, uint64_t address, QString* module, uint64_t* offset) {
    if (!module || !offset || address == 0) {
        return false;
    }
    const auto modules = killcore::ProcessEnumerator::enumerateModules(pid);
    QString bestModule;
    uint64_t bestOffset = 0;
    uint64_t bestSize = 0;
    for (const auto& mod : modules) {
        if (address >= mod.baseAddress && address < mod.baseAddress + mod.size) {
            if (bestModule.isEmpty() || mod.size < bestSize) {
                bestModule = mod.name;
                bestOffset = address - mod.baseAddress;
                bestSize = mod.size;
            }
        }
    }
    if (bestModule.isEmpty()) {
        return false;
    }
    *module = bestModule;
    *offset = bestOffset;
    return true;
}

QStringList hexAddressesFromText(const QString& text) {
    QStringList addresses;
    const QRegularExpression re(R"(\b0x[0-9a-fA-F]{5,16}\b)");
    auto it = re.globalMatch(text);
    while (it.hasNext()) {
        addresses.append(it.next().captured(0));
    }
    return addresses;
}

QString textWithoutHexAddresses(QString text) {
    const QRegularExpression re(R"(\b0x[0-9a-fA-F]{5,16}\b)");
    return text.replace(re, " ");
}

QString textWithoutTypeTokens(QString text) {
    static const QRegularExpression typeRe(
        R"(\b(?:u?int(?:8|16|32|64)?|float(?:32|64)?|double|long)\b)",
        QRegularExpression::CaseInsensitiveOption);
    return text.replace(typeRe, " ");
}

QStringList numbersFromText(const QString& text) {
    QStringList values;
    // L'ecriture francaise groupe les milliers par espace ("100 000" = cent
    // mille). Sans l'alternative groupee ci-dessous (essayee en premier),
    // "100 000" se lit comme DEUX nombres distincts "100" et "000" : dans
    // "je veux a 100 000", GuidedScan ne voit que numbers.at(1) = "100" et la
    // cible reelle (100000) disparait silencieusement, sans aucune erreur.
    // Espace normal ET insecable (U+00A0, que Windows/le clavier FR produisent
    // parfois) sont acceptes comme separateur de groupe.
    // Compromis assume, non resolu : deux nombres tapes cote a cote sans mot
    // de liaison ni ponctuation, ou le second fait exactement 3 chiffres
    // (ex: "100 200" sans "à" entre les deux), fusionnent en un seul nombre
    // "100200" au lieu de rester deux valeurs distinctes — GuidedScan (qui
    // veut numbers.size() >= 2) verrait alors une seule cible. Un connecteur
    // ("à", "vers", "et"...) entre les deux nombres empeche la fusion (le
    // groupe exige un espace suivi directement de 3 chiffres), donc "de 100 à
    // 200" n'est pas affecte ; seule la forme rare "100 200" sans connecteur
    // l'est. Pas de correctif ici : resoudre parfaitement cette ambiguite
    // demanderait de la vraie comprehension du langage naturel, et le risque
    // inverse (rater "100 000" pour cent mille, bien plus frequent en usage
    // reel) serait pire.
    const QRegularExpression re(
        QStringLiteral("[-+]?\\d{1,3}(?:[ \\x{00A0}]\\d{3})+(?:[.,]\\d+)?|[-+]?\\d+(?:[.,]\\d+)?"));
    auto it = re.globalMatch(textWithoutTypeTokens(textWithoutHexAddresses(text)));
    while (it.hasNext()) {
        QString captured = it.next().captured(0);
        captured.remove(' ');
        captured.remove(QChar(0x00A0));
        values.append(captured.replace(',', '.'));
    }
    return values;
}

QString explicitValueTypeFromText(const QString& text) {
    const QString q = text.toLower();
    if (q.contains("uint8") || q.contains("u8") || q.contains("byte")) return "UInt8";
    if (q.contains("int8") || q.contains("i8")) return "Int8";
    if (q.contains("uint16") || q.contains("u16")) return "UInt16";
    if (q.contains("int16") || q.contains("i16") || q.contains("short")) return "Int16";
    if (q.contains("uint32") || q.contains("u32")) return "UInt32";
    if (q.contains("uint64") || q.contains("u64")) return "UInt64";
    if (q.contains("float64") || q.contains("double")) return "Float64";
    if (q.contains("float32") || q.contains("float")) return "Float32";
    if (q.contains("int64") || q.contains("long")) return "Int64";
    if (q.contains("int32") || q.contains("int")) return "Int32";
    return {};
}

QString moduleNameFromText(const QString& text) {
    const QRegularExpression explicitModuleRe(R"(([A-Za-z0-9_.-]+\.(?:dll|exe)))", QRegularExpression::CaseInsensitiveOption);
    const auto match = explicitModuleRe.match(text);
    if (match.hasMatch()) {
        return match.captured(1);
    }

    const QString q = text.toLower();
    if (q.contains("webview")) {
        return "WebView";
    }
    if (q.contains("solitaire")) {
        return "Solitaire";
    }
    return {};
}

bool wantsModuleExploration(const QString& text) {
    const QString q = text.toLower();
    const bool mentionsModule = q.contains("dll") || q.contains("module") || q.contains("modules");
    const bool rejectsModuleScan = q.contains("ne relance pas de scan module")
        || q.contains("ne lance pas de scan module")
        || q.contains("pas de scan module")
        || q.contains("sans scan module")
        || q.contains("no module scan")
        || q.contains("do not scan module");
    const bool asksDiscovery = q.contains("trouve") || q.contains("trouver")
        || q.contains("cherche") || q.contains("chercher") || q.contains("travaille")
        || q.contains("travailler") || q.contains("localise") || q.contains("localiser")
        || q.contains("falloir") || q.contains("find");
    const bool mentionsTarget = q.contains("xp") || q.contains("experience") || q.contains("expérience")
        || q.contains("score") || q.contains("niveau") || q.contains("level")
        || q.contains("argent") || q.contains("money") || q.contains("minerai")
        || q.contains("mineral") || q.contains("ressource");
    if (rejectsModuleScan && !q.contains("liste les modules") && !q.contains("list modules")) {
        return false;
    }
    return mentionsModule && asksDiscovery && mentionsTarget;
}

bool wantsExplicitTraceUiString(const QString& text) {
    const QString q = text.toLower();
    const bool traceWords = q.contains("trace ui")
        || q.contains("ui string")
        || q.contains("string ui")
        || q.contains("trace le texte")
        || q.contains("tracer le texte")
        || q.contains("texte affich")
        || q.contains("valeur affich")
        || (q.contains("affich") && (q.contains("string") || q.contains("texte") || q.contains("source")));
    const bool negated = q.contains("ne trace pas")
        || q.contains("pas trace ui")
        || q.contains("sans trace ui")
        || q.contains("do not trace");
    return traceWords && !negated;
}

bool wantsExplicitChangedPages(const QString& text) {
    const QString q = text.toLower();
    const bool changedPagesWords = q.contains("changed pages")
        || q.contains("diff pages")
        || q.contains("pages modifiees")
        || q.contains("pages modifiées")
        || q.contains("pages qui changent")
        || q.contains("pages changées")
        || q.contains("pages changees")
        || q.contains("comparaison de pages")
        || q.contains("compare les pages")
        || q.contains("comparer les pages");
    const bool negated = q.contains("ne fais pas changed pages")
        || q.contains("pas changed pages")
        || q.contains("sans changed pages")
        || q.contains("do not use changed pages");
    return changedPagesWords && !negated;
}

bool wantsCandidateRefinement(const QString& text) {
    const QString q = text.toLower();
    return q.contains("reduis") || q.contains("réduis")
        || q.contains("reduire") || q.contains("réduire")
        || q.contains("reduit") || q.contains("réduit")
        || q.contains("affine") || q.contains("affiner")
        || q.contains("filtre") || q.contains("filtrer")
        || q.contains("nouvelle valeur");
}

QString observedRefinementValueFromText(const QString& text, const QStringList& numbers) {
    static const QRegularExpression explicitObservedRe(
        QStringLiteral("(?:xp|score|niveau|level|affich\\w*|maintenant)[^0-9+-]{0,40}([-+]?\\d+(?:[.,]\\d+)?)"),
        QRegularExpression::CaseInsensitiveOption);
    const auto match = explicitObservedRe.match(text);
    if (match.hasMatch()) {
        return match.captured(1).replace(',', '.');
    }
    return numbers.size() == 1 ? numbers.first() : QString();
}

QStringList lastTwoObservedValues(const QStringList& numbers) {
    QStringList values;
    if (numbers.size() >= 2) {
        values << numbers.at(numbers.size() - 2) << numbers.at(numbers.size() - 1);
    }
    return values;
}

bool isLikelyRuntimeModule(const QString& lower) {
    return lower.contains("vcruntime")
        || lower.contains("msvcp")
        || lower.contains("vccorlib")
        || lower.contains("concrt")
        || lower.contains("telemetry")
        || lower.contains("ucrtbase")
        || lower.contains("api-ms-win")
        || lower.contains("ext-ms-win")
        || lower.contains("kernelbase")
        || lower.contains("kernel32")
        || lower.contains("ntdll")
        || lower.contains("qt6")
        || lower.contains("d3d")
        || lower.contains("dxgi");
}

int moduleGameplayRelevanceScore(const QVariantMap& module) {
    const QString name = module.value("name").toString();
    const QString path = module.value("path").toString();
    const QString lower = (name + " " + path).toLower();
    if (isLikelyRuntimeModule(lower)) {
        return 0;
    }

    int score = 0;
    if (lower.contains("microsoftsolitaire") || lower.contains("solitaire.exe")) score += 120;
    if (lower.contains("solitaire")) score += 80;
    if (lower.contains("webview")) score += 55;
    if (lower.contains("windowsapps")) score += 30;
    if (lower.contains("microsoft.ui.xaml")) score += 20;
    if (lower.contains("mrt100")) score += 15;
    if (lower.contains("sharedlibrary")) score += 8;
    return score;
}

QString normalizedProfileText(QString value) {
    return value.toLower().trimmed();
}

QString profileTargetGroupName(QString name) {
    name = normalizedProfileText(name);
    static const QRegularExpression numberedSuffix(R"(\s+\d+$)");
    return name.remove(numberedSuffix).trimmed();
}

bool looksLikeLastAutoWriteRewrite(const QString& query) {
    const QString q = query.toLower();
    return q.contains("plutot")
        || q.contains("plutôt")
        || q.contains("change")
        || q.contains("changer")
        || q.contains("modifie")
        || q.contains("modifier")
        || q.contains("mettre")
        || q.contains("mets")
        || q.contains("met ")
        || q.contains("passe")
        || q.contains("passé")
        || q.contains("passer")
        || q.contains("remet")
        || q.contains("remets")
        || q.contains("veux")
        || q.contains("veut")
        || q.contains("voulais")
        || q.contains("voudrais")
        || q.contains("augmente")
        || q.contains("augmenter")
        || q.contains("remplace")
        || q.contains("remplacer")
        || q.contains("fixe")
        || q.contains("définis")
        || q.contains("definis")
        || q.contains("définir")
        || q.contains("definir")
        || q.contains("ces adresse")
        || q.contains("ces adresses")
        || q.contains("les adresse")
        || q.contains("les adresses")
        || q.contains(" le ")
        || q.contains(" les ")
        || q.contains(" ça ")
        || q.contains(" ca ")
        || q.contains("les mettre");
}

bool looksLikeMemoryTargetWriteRequest(const QString& query) {
    const QString q = query.toLower();
    return looksLikeLastAutoWriteRewrite(q)
        || q.contains("passer")
        || q.contains("mets")
        || q.contains("met ");
}

bool looksLikeNewSearchRequest(const QString& query) {
    const QString q = query.toLower();
    return q.contains("nouvelle recherche")
        || q.contains("autre recherche")
        || q.contains("nouveau scan")
        || q.contains("nouvelle valeur")
        || q.contains("autre valeur")
        || q.contains("autre chose")
        || q.contains("valeur a chercher")
        || q.contains("valeur à chercher")
        || q.contains("autre que")
        || q.contains("pas ces adresse")
        || q.contains("pas ces adresses")
        || q.contains("repart")
        || q.contains("recommence")
        || q.contains("reset")
        // Abandon pur et simple, sans mot-cle "nouvelle recherche" explicite.
        // Sans ca, une reponse comme "laisse tomber" a une relance en attente
        // (trace_ui_string/trace_ui_filter/write_target_value) est traitee
        // comme si c'etait la reponse demandee : le texte litteral finit
        // dans m_smartSearchTargetValue ou comme valeur observee, au lieu
        // d'annuler proprement.
        || q.contains("laisse tomber")
        || q.contains("j'abandonne")
        || q.contains("j abandonne")
        || q.contains("oublie ça")
        || q.contains("oublie ca")
        || q.contains("peu importe");
}

bool looksLikeClearActiveTargetsRequest(const QString& query) {
    const QString q = query.toLower();
    const bool clearVerb = q.contains("oublie")
        || q.contains("oublier")
        || q.contains("efface")
        || q.contains("supprime")
        || q.contains("retire")
        || q.contains("vide");
    const bool targetWord = q.contains("adresse")
        || q.contains("memoire")
        || q.contains("mémoire")
        || q.contains("cible")
        || q.contains("profil")
        || q.contains("contexte");
    return clearVerb && targetWord;
}

bool looksLikeBadTargetReport(const QString& query) {
    const QString q = query.toLower();
    return q.contains("marche pas")
        || q.contains("marché pas")
        || q.contains("pas marche")
        || q.contains("pas marché")
        || q.contains("n'a pas marché")
        || q.contains("n a pas marche")
        || q.contains("ne marche pas")
        // "fonctionne/fonctionné" est le synonyme le plus courant de "marche"
        // et n'etait pas couvert : "ça n'a pas fonctionné" tombait sur Unknown
        // et partait vers l'IA locale au lieu de ReportBadTargets (vu en test
        // reel le 17/08/2026 - ~90s perdues sur un appel LLM qui n'aboutit a
        // rien, puis le message suivant reecrivait sur les adresses jamais
        // invalidees puisque ce chemin n'avait jamais ete declenche).
        || q.contains("fonctionne pas")
        || q.contains("fonctionné pas")
        || q.contains("pas fonctionne")
        || q.contains("pas fonctionné")
        || q.contains("n'a pas fonctionné")
        || q.contains("n a pas fonctionne")
        || q.contains("ne fonctionne pas")
        || q.contains("rien fait")
        || q.contains("aucun effet")
        || q.contains("toujours pareil")
        || q.contains("pas bon")
        || q.contains("pas la bonne")
        || q.contains("mauvaise adresse")
        || q.contains("mauvaises adresses")
        || q.contains("ca change pas")
        || q.contains("ça change pas")
        || q.contains("rien change")
        || q.contains("rien ne change")
        || q.contains("crash")
        || q.contains("crashé")
        || q.contains("crashe")
        || q.contains("planté")
        || q.contains("plante")
        || q.contains("jeu s'est fermé")
        || q.contains("jeu s est ferme");
}

bool looksLikeGoodTargetReport(const QString& query) {
    // "c'est ça" est volontairement absent : c'est une confirmation
    // conversationnelle generique ("d'accord, c'est ça le prochain objectif")
    // qui n'a le plus souvent aucun rapport avec une adresse. La declencher a
    // tort sauvegarde silencieusement l'adresse active dans le Profil et
    // l'immunise pour toujours contre le filtre anti-bruit (voir
    // flagNoisyCandidates / everConfirmed plus haut) : une fausse confirmation
    // ici est quasi irreversible, donc seuls des motifs sans ambiguite
    // raisonnable sont acceptes.
    const QString q = query.toLower();
    return q.contains("ça a marché")
        || q.contains("ca a marche")
        || q.contains("ça marche")
        || q.contains("ca marche")
        || q.contains("ça a fonctionné")
        || q.contains("ca a fonctionne")
        || q.contains("ça fonctionne")
        || q.contains("ca fonctionne")
        || q.contains("c'est la bonne")
        || q.contains("c est la bonne")
        || q.contains("bonne adresse")
        || q.contains("ça a changé")
        || q.contains("ca a change")
        || q.contains("nickel")
        || q.contains("parfait");
}

bool looksLikeFreezeRequest(const QString& query) {
    const QString q = query.toLower();
    const bool negated = q.contains("sans freeze") || q.contains("sans freezer")
        || q.contains("sans geler") || q.contains("sans figer")
        || q.contains("ni freeze") || q.contains("ni freezer")
        || q.contains("ni geler") || q.contains("ni figer")
        || q.contains("pas de freeze") || q.contains("pas freeze")
        || q.contains("ne freeze pas") || q.contains("ne pas freeze")
        || q.contains("ne pas freezer") || q.contains("without freeze")
        || q.contains("without freezing") || q.contains("no freeze")
        || q.contains("do not freeze") || q.contains("don't freeze");
    if (negated) {
        return false;
    }

    return q.contains("freeze")
        || q.contains("freezer")
        || q.contains("fige")
        || q.contains("figer")
        || q.contains("bloque")
        || q.contains("bloquer")
        || q.contains("verrouille")
        || q.contains("verrouiller")
        || q.contains("garde a")
        || q.contains("garde à")
        || q.contains("maintien")
        || q.contains("maintenir");
}

SmartSearchIntent classifySmartSearchIntent(
    const QString& query,
    const QStringList& numbers,
    const QStringList& addresses,
    bool hasChatMemoryTargets,
    bool hasLastAutoWriteTargets,
    bool hasCandidates,
    bool smartSearchActive,
    bool awaitingUiStringTraceValue,
    bool awaitingUiStringFilterValue,
    bool awaitingWriteTargetValue) {
    SmartSearchIntent intent;
    intent.numbers = numbers;
    intent.addresses = addresses;
    intent.resetContext = looksLikeNewSearchRequest(query);

    const bool hasOneNumber = numbers.size() == 1;
    const bool wantsMemoryWrite = looksLikeMemoryTargetWriteRequest(query);
    const bool wantsLastRewrite = looksLikeLastAutoWriteRewrite(query);
    const bool wantsClearTargets = looksLikeClearActiveTargetsRequest(query);
    const bool reportsBadTargets = looksLikeBadTargetReport(query);
    // Verifie reportsBadTargets d'abord dans la branche ci-dessous : certains
    // tours ("ça marche pas") sont un sous-ensemble textuel de motifs positifs
    // ("ça marche"), l'ordre de l'if/else suffit a lever l'ambiguite sans que
    // les deux listes de mots-cles aient besoin d'etre mutuellement exclusives.
    const bool reportsGoodTargets = looksLikeGoodTargetReport(query);
    const bool wantsFreeze = looksLikeFreezeRequest(query);

    if (wantsClearTargets) {
        intent.kind = SmartSearchIntentKind::ClearActiveTargets;
        intent.rationale = "L'utilisateur demande d'oublier les adresses, profils ou cibles actives.";
    } else if (reportsBadTargets && (hasLastAutoWriteTargets || hasChatMemoryTargets)) {
        intent.kind = SmartSearchIntentKind::ReportBadTargets;
        intent.rationale = "L'utilisateur indique que les dernières adresses écrites ne donnent pas le résultat attendu.";
    } else if (reportsGoodTargets && (hasLastAutoWriteTargets || hasChatMemoryTargets)) {
        intent.kind = SmartSearchIntentKind::ReportGoodTargets;
        intent.rationale = "L'utilisateur confirme que les dernières adresses écrites fonctionnent.";
    } else if (awaitingUiStringTraceValue && !intent.resetContext && (hasOneNumber || !query.trimmed().isEmpty())) {
        // L'assistant vient de proposer "Tracer le texte affiché" et attend la
        // valeur affichée en reponse. Sans cette interception, une reponse en
        // langage libre contenant un nombre (ex: "le texte affiche est 180")
        // retombe sur WriteMemoryTargets plus bas et reecrit betement ce
        // nombre sur les adresses deja invalidees, au lieu de tracer.
        intent.kind = SmartSearchIntentKind::AnswerTraceUiStringPrompt;
        intent.rationale = "L'utilisateur répond à la proposition de tracer le texte affiché.";
    } else if (awaitingUiStringFilterValue && !intent.resetContext && (hasOneNumber || !query.trimmed().isEmpty())) {
        // Meme interception pour l'etape 2 du pipeline Trace UI string
        // (filtrer les strings survivantes puis analyser les sources
        // numeriques autour) : sans elle, la reponse "nouvelle valeur
        // affichee" retombe elle aussi sur l'ancien pipeline numerique.
        intent.kind = SmartSearchIntentKind::AnswerTraceUiFilterPrompt;
        intent.rationale = "L'utilisateur répond à la proposition de filtrer les strings suivies.";
    } else if (awaitingWriteTargetValue && !intent.resetContext && (hasOneNumber || !query.trimmed().isEmpty())) {
        // L'assistant a demande la valeur a ecrire (candidats reduits mais
        // aucune cible connue). Sans cette interception, la reponse retombe
        // sur ExactScan et repart sur un scan complet, abandonnant la
        // reduction deja faite.
        intent.kind = SmartSearchIntentKind::AnswerWriteTargetPrompt;
        intent.rationale = "L'utilisateur donne la valeur à écrire sur les candidats déjà réduits.";
    } else if (smartSearchActive && hasCandidates && hasOneNumber) {
        intent.kind = SmartSearchIntentKind::RefineScan;
        intent.rationale = "Un scan guidé est actif et l'utilisateur donne une nouvelle valeur observée.";
    } else if (intent.resetContext && numbers.isEmpty()) {
        intent.kind = SmartSearchIntentKind::ResetContext;
        intent.rationale = "L'utilisateur demande un nouveau contexte sans donner encore de valeur.";
    } else if (!addresses.isEmpty()) {
        intent.kind = hasOneNumber && wantsFreeze
            ? SmartSearchIntentKind::FreezeMemoryTargets
            : (hasOneNumber && wantsMemoryWrite
                ? SmartSearchIntentKind::WriteMemoryTargets
                : SmartSearchIntentKind::ActivateMemoryTargets);
        intent.rationale = "Le message contient une ou plusieurs adresses mémoire explicites.";
    } else if (hasChatMemoryTargets && hasOneNumber && !intent.resetContext && wantsFreeze) {
        intent.kind = SmartSearchIntentKind::FreezeMemoryTargets;
        intent.rationale = "Des adresses mémoire sont actives et l'utilisateur demande de freezer la valeur.";
    } else if (hasLastAutoWriteTargets && hasOneNumber && !intent.resetContext && wantsLastRewrite) {
        intent.kind = SmartSearchIntentKind::RewriteLastTargets;
        intent.rationale = "L'utilisateur demande de modifier les dernières adresses écrites.";
    } else if (hasChatMemoryTargets && hasOneNumber && !intent.resetContext) {
        intent.kind = SmartSearchIntentKind::WriteMemoryTargets;
        intent.rationale = "Des adresses mémoire sont actives dans la conversation.";
    } else if (hasOneNumber && !intent.resetContext && wantsMemoryWrite) {
        intent.kind = SmartSearchIntentKind::WriteProfileTargets;
        intent.rationale = "L'utilisateur formule une intention d'écriture sur une cible nommée.";
    } else if (numbers.size() >= 2) {
        intent.kind = SmartSearchIntentKind::GuidedScan;
        intent.rationale = "Le message contient une valeur actuelle et une valeur cible.";
    } else if (hasOneNumber) {
        intent.kind = SmartSearchIntentKind::ExactScan;
        intent.rationale = intent.resetContext
            ? "Nouvelle recherche demandée avec une valeur."
            : "Recherche exacte depuis une valeur unique.";
    }

    return intent;
}

// Detecte si le processus a des connexions TCP ETABLIES vers un hote distant
// (hors loopback). Utilise l'API Windows standard en lecture seule (aucune
// capture de paquets, aucun droit admin requis) : ce n'est PAS une preuve
// qu'une valeur donnee est synchronisee avec un serveur, juste un indice
// supplementaire a proposer une fois toutes les pistes memoire locales
// epuisees (ex: XP/monnaie lies a un compte en ligne plutot qu'a une simple
// variable de session).
#ifdef _WIN32
bool processHasActiveRemoteConnections(int pid) {
    ULONG size = 0;
    if (GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0) != ERROR_INSUFFICIENT_BUFFER
        || size == 0) {
        return false;
    }
    QByteArray buffer(static_cast<int>(size), 0);
    auto* table = reinterpret_cast<MIB_TCPTABLE_OWNER_PID*>(buffer.data());
    if (GetExtendedTcpTable(table, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0) != NO_ERROR) {
        return false;
    }
    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        const auto& row = table->table[i];
        if (static_cast<int>(row.dwOwningPid) != pid || row.dwState != MIB_TCP_STATE_ESTAB) {
            continue;
        }
        const uint32_t remote = ntohl(row.dwRemoteAddr);
        const bool isLoopback = (remote >> 24) == 127;
        if (remote != 0 && !isLoopback) {
            return true;
        }
    }
    return false;
}
#else
bool processHasActiveRemoteConnections(int) {
    return false;
}
#endif

QString confidenceLabel(double confidence) {
    if (confidence >= 0.85) {
        return "fiabilité élevée";
    }
    if (confidence >= 0.65) {
        return "fiabilité moyenne";
    }
    return "fiabilité faible";
}

// Historique inter-sessions des adresses ecrites par le pipeline auto-write,
// par jeu. Une adresse qui revient comme "candidat final" sur des recherches
// avec des paires (valeur initiale, cible) differentes et sans rapport est
// tres probablement un compteur interne (timer, animation, allocation
// reutilisee) qui satisfait le test de transition par pure coincidence, pas
// la vraie donnee cherchee. Persiste via QSettings, comme le reste du
// "profil appris" par gameKey (cf. getAutoResolveReport).
constexpr int kCandidateHistoryLimit = 60;

QVariantList loadCandidateHistory(const QString& gameKey) {
    QSettings settings;
    settings.beginGroup(QString("autoResolver/process/%1").arg(gameKey));
    const QByteArray raw = settings.value("candidateHistory").toByteArray();
    settings.endGroup();
    if (raw.isEmpty()) {
        return {};
    }
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isArray()) {
        return {};
    }
    return doc.array().toVariantList();
}

void appendCandidateHistory(const QString& gameKey, const QVariantList& newEntries) {
    if (newEntries.isEmpty()) {
        return;
    }
    QVariantList history = loadCandidateHistory(gameKey);
    history.append(newEntries);
    while (history.size() > kCandidateHistoryLimit) {
        history.removeFirst();
    }
    QSettings settings;
    settings.beginGroup(QString("autoResolver/process/%1").arg(gameKey));
    settings.setValue("candidateHistory", QJsonDocument(QJsonArray::fromVariantList(history)).toJson(QJsonDocument::Compact));
    settings.endGroup();
}

// Marque (sans les retirer) les suggestions dont l'adresse apparait deja dans
// l'historique pour une paire de valeurs differente. Retourne le nombre de
// suggestions non suspectes restantes, pour decider s'il faut filtrer.
int flagNoisyCandidates(
    QVariantList* suggestions,
    const QString& gameKey,
    const QString& initialValue,
    const QString& targetValue) {
    if (!suggestions || suggestions->isEmpty()) {
        return suggestions ? suggestions->size() : 0;
    }
    const QVariantList history = loadCandidateHistory(gameKey);
    int cleanCount = 0;
    for (int i = 0; i < suggestions->size(); ++i) {
        QVariantMap suggestion = suggestions->at(i).toMap();
        const QString address = suggestion.value("address").toString();

        // Une adresse explicitement confirmee par l'utilisateur ("ça a
        // marché") un jour n'est plus jamais consideree comme du bruit,
        // meme si elle revient plus tard avec une autre paire de valeurs :
        // une vraie donnee (XP, argent...) est justement testee avec des
        // cibles differentes a chaque farming, ce n'est pas un signe de bruit.
        bool everConfirmed = false;
        for (const auto& entryVariant : history) {
            const QVariantMap entry = entryVariant.toMap();
            if (entry.value("address").toString() == address && entry.value("confirmed").toBool()) {
                everConfirmed = true;
                break;
            }
        }
        if (everConfirmed) {
            ++cleanCount;
            continue;
        }

        int unrelatedHits = 0;
        for (const auto& entryVariant : history) {
            const QVariantMap entry = entryVariant.toMap();
            if (entry.value("address").toString() != address) {
                continue;
            }
            const bool sameSearch = entry.value("initialValue").toString() == initialValue
                && entry.value("targetValue").toString() == targetValue;
            if (!sameSearch) {
                ++unrelatedHits;
            }
        }
        if (unrelatedHits > 0) {
            suggestion["noisyHistoryHits"] = unrelatedHits;
            suggestion["confidenceReason"] = QString("⚠ vue dans %1 recherche(s) différente(s) sans rapport — probablement du bruit · %2")
                .arg(unrelatedHits)
                .arg(suggestion.value("confidenceReason").toString());
        } else {
            ++cleanCount;
        }
        (*suggestions)[i] = suggestion;
    }
    return cleanCount;
}

QVariantList suggestedWritesForCandidates(const killcore::CandidateStore& candidates, const QString& value, size_t limit) {
    QVariantList suggestions;
    if (value.isEmpty() || candidates.isEmpty()) {
        return suggestions;
    }

    const auto& all = candidates.candidates();
    for (qsizetype i = 0; i < all.size() && static_cast<size_t>(suggestions.size()) < limit; ++i) {
        const auto& candidate = all.at(i);

        // Une valeur cible qui ne rentre pas dans le type detecte du candidat
        // (ex: 100000 sur un candidat UInt16, max 65535) echoue a coup sur a
        // l'ecriture. Sans ce filtre, le candidat etait quand meme presente
        // comme suggestion "fiabilite elevee" et l'echec n'apparaissait qu'au
        // moment d'ecrire (auto_write_partial_or_failed) — trop tard pour
        // etre utile a l'utilisateur ou a l'IA.
        killcore::ScanValue parsedValue;
        if (!killcore::parseScanValue(value, candidate.type, &parsedValue)) {
            continue;
        }

        QVariantMap suggestion;
        suggestion["address"] = QString::number(candidate.address, 16);
        suggestion["type"] = killcore::valueTypeToString(candidate.type);
        suggestion["value"] = value;
        suggestion["confidence"] = candidate.confidence;
        suggestion["confidenceLabel"] = confidenceLabel(candidate.confidence);
        suggestion["confidenceReason"] = candidate.variantLabel.isEmpty()
            ? QString("adresse survivante des réductions")
            : QString("%1 · %2").arg(confidenceLabel(candidate.confidence), candidate.variantLabel);
        if (!candidate.variantLabel.isEmpty()) {
            suggestion["variantLabel"] = candidate.variantLabel;
        }
        suggestions.append(suggestion);
    }
    return suggestions;
}

void appendDistinctText(QStringList* values, const QString& value, int maxCount) {
    if (!values) {
        return;
    }
    const QString trimmed = value.trimmed();
    if (trimmed.isEmpty()) {
        return;
    }
    if (values->isEmpty() || values->last() != trimmed) {
        values->append(trimmed);
    }
    while (values->size() > maxCount) {
        values->removeFirst();
    }
}

QVariantList writeHistoryToVariantList(const QStringList& values) {
    QVariantList result;
    for (const auto& value : values) {
        result.append(value);
    }
    return result;
}


} // namespace

SmartSearchManager::SmartSearchManager(ApplicationController& controller)
    : m_controller(controller) {}


QVariantMap SmartSearchManager::activateChatMemoryTargetsFromQuery(const QString& query) {
    QVariantMap result;
    QVariantList suggestions;
    const QStringList addressTexts = hexAddressesFromText(query);

    result["query"] = query;
    result["aiReady"] = m_controller.m_ai.isReady();
    result["status"] = "memory_targets_activated";
    result["actionStatus"] = "not_executed";
    result["workflowStatus"] = "memory_targets_ready";

    auto writeState = m_controller.autoWriteState();
    writeState.clearChatTargets();
    writeState.clearLastTargets();
    m_controller.m_autoWriteValueHistory.clear();
    m_controller.m_smartSearchActive = false;
    m_controller.m_smartSearchInitialValue.clear();
    m_controller.m_smartSearchTargetValue.clear();

    for (const auto& addressText : addressTexts) {
        uint64_t address = 0;
        if (!parseHexAddress(addressText, &address)) {
            continue;
        }

        bool alreadyAdded = false;
        for (const auto& target : writeState.chatTargets()) {
            if (target.address == address) {
                alreadyAdded = true;
                break;
            }
        }
        if (alreadyAdded) {
            continue;
        }

        const AutoWriteTarget target{address, killcore::ValueType::Int32, /*chatOrigin=*/true};
        writeState.appendChatTarget(target);
        writeState.appendLastTarget(target);

        QVariantMap suggestion;
        suggestion["source"] = "chat_address";
        suggestion["address"] = QString::number(address, 16);
        suggestion["type"] = killcore::valueTypeToString(target.type);
        suggestions.append(suggestion);
    }

    result["success"] = writeState.hasChatTargets();
    result["targetCount"] = writeState.chatTargetCount();
    result["suggestedWrites"] = suggestions;
    result["message"] = !writeState.hasChatTargets()
        ? QString("Je n'ai pas reconnu d'adresse mémoire valide dans ton message.")
        : QString("J'ai sélectionné %1 adresse(s) mémoire depuis ton message. Donne-moi maintenant la valeur à écrire dessus.")
              .arg(writeState.chatTargetCount());
    m_controller.appendSmartSearchDebug("chat_memory_targets_activated", result);
    return result;
}

QVariantMap SmartSearchManager::writeChatMemoryTargetsFromQuery(const QString& query, const QString& value) {
    QVariantMap result;
    QVariantList suggestions;
    QVariantList writeResults;

    result["query"] = query;
    result["aiReady"] = m_controller.m_ai.isReady();
    result["status"] = "tool_call";
    result["tool"] = "chat_memory_write";
    result["actionStatus"] = "executed";
    result["workflowStatus"] = "auto_write_done";
    result["targetValue"] = value;

    bool allWritesOk = true;
    const QString previousTargetValue = m_controller.m_smartSearchTargetValue;
    m_controller.m_smartSearchTargetValue = value;
    auto writeState = m_controller.autoWriteState();
    m_controller.m_lastBatchStartIndex = writeState.writeHistorySize();
    writeState.clearLastTargets();

    for (const auto& target : writeState.chatTargets()) {
        QVariantMap suggestion;
        suggestion["source"] = "chat_address";
        suggestion["address"] = QString::number(target.address, 16);
        suggestion["type"] = killcore::valueTypeToString(target.type);
        suggestion["value"] = value;
        const auto history = m_controller.candidateValueHistory(target.address);
        if (!history.isEmpty()) {
            suggestion["valueHistory"] = history;
        }
        suggestions.append(suggestion);

        auto writeResult = m_controller.writeMemoryValueConfirmed(
            suggestion.value("address").toString(),
            suggestion.value("type").toString(),
            value);
        writeResult.insert("source", suggestion.value("source"));
        writeResult.insert("address", suggestion.value("address"));
        writeResult.insert("value", value);
        writeResult.insert("type", suggestion.value("type"));
        if (suggestion.contains("valueHistory")) {
            writeResult.insert("valueHistory", suggestion.value("valueHistory"));
        }
        allWritesOk = allWritesOk && writeResult.value("success").toBool();
        writeResults.append(writeResult);

        if (writeResult.value("success").toBool()) {
            writeState.appendLastTarget(target);
        }
    }

    m_controller.m_lastBatchEndIndex = writeState.writeHistorySize();
    if (m_controller.m_lastBatchEndIndex == m_controller.m_lastBatchStartIndex) {
        m_controller.m_lastBatchStartIndex = -1;
        m_controller.m_lastBatchEndIndex = -1;
        writeState.clearLastTargets();
    }
    if (allWritesOk && writeState.hasLastTargets()) {
        m_controller.m_smartSearchActive = false;
        writeState.replaceChatTargetsWithLastTargets();
        resetFailureEscalationState();
        if (m_controller.m_autoWriteValueHistory.isEmpty() && !previousTargetValue.isEmpty()) {
            appendDistinctText(&m_controller.m_autoWriteValueHistory, previousTargetValue, 12);
        }
        appendDistinctText(&m_controller.m_autoWriteValueHistory, value, 12);
    }

    QVariantMap actionResult;
    actionResult["success"] = allWritesOk;
    actionResult["remaining"] = static_cast<qulonglong>(writeState.chatTargetCount());
    actionResult["error"] = allWritesOk ? QString() : QString("Au moins une écriture sur adresse donnée a échoué.");

    result["success"] = allWritesOk;
    result["actionResult"] = actionResult;
    result["suggestedWrites"] = suggestions;
    result["suggestedWrite"] = suggestions.isEmpty() ? QVariantMap{} : suggestions.first().toMap();
    result["autoWriteResults"] = writeResults;
    result["autoWriteResult"] = writeResults.isEmpty() ? QVariantMap{} : writeResults.last().toMap();
    result["autoWriteCount"] = writeResults.size();
    result["activeTargetCount"] = writeState.chatTargetCount();
    result["previousTargetValue"] = previousTargetValue;
    result["writeHistory"] = writeHistoryToVariantList(m_controller.m_autoWriteValueHistory);
    result["rollbackNote"] = "Tu peux annuler cette écriture via le bouton rollback batch dans l'assistant.";
    result["message"] = allWritesOk
        ? QString("J'ai écrit %1 sur %2 adresse(s) mémoire sélectionnée(s) dans la conversation. Je garde ces adresses actives pour les prochaines modifications.")
              .arg(value)
              .arg(writeState.chatTargetCount())
        : QString("J'ai essayé d'écrire %1 sur les adresses mémoire sélectionnées, mais au moins une écriture a échoué.")
              .arg(value);
    m_controller.appendSmartSearchDebug("chat_memory_write", result);
    return result;
}

QVariantMap SmartSearchManager::freezeChatMemoryTargetsFromQuery(const QString& query, const QString& value) {
    QVariantMap result;
    QVariantList suggestions;
    QVariantList freezeResults;

    result["query"] = query;
    result["aiReady"] = m_controller.m_ai.isReady();
    result["status"] = "tool_call";
    result["tool"] = "chat_memory_freeze";
    result["actionStatus"] = "executed";
    result["workflowStatus"] = "freeze_done";
    result["targetValue"] = value;

    bool allFreezeOk = true;
    int frozenCount = 0;

    for (const auto& target : m_controller.m_chatMemoryTargets) {
        QVariantMap suggestion;
        suggestion["source"] = "chat_address";
        suggestion["address"] = QString::number(target.address, 16);
        suggestion["type"] = killcore::valueTypeToString(target.type);
        suggestion["value"] = value;
        suggestions.append(suggestion);

        auto freezeResult = m_controller.setFreezeValue(
            suggestion.value("address").toString(),
            suggestion.value("type").toString(),
            value,
            true);
        freezeResult.insert("source", suggestion.value("source"));
        freezeResult.insert("address", suggestion.value("address"));
        freezeResult.insert("value", value);
        freezeResult.insert("type", suggestion.value("type"));
        freezeResults.append(freezeResult);

        allFreezeOk = allFreezeOk && freezeResult.value("success").toBool();
        if (freezeResult.value("success").toBool()) {
            ++frozenCount;
        }
    }

    result["success"] = allFreezeOk && frozenCount > 0;
    result["suggestedWrites"] = suggestions;
    result["freezeResults"] = freezeResults;
    result["activeTargetCount"] = m_controller.m_chatMemoryTargets.size();
    result["message"] = frozenCount > 0
        ? QString("Freeze activé sur %1/%2 adresse(s) active(s) à %3. Je garde ces adresses actives pour pouvoir modifier ensuite.")
              .arg(frozenCount)
              .arg(m_controller.m_chatMemoryTargets.size())
              .arg(value)
        : QString("Je n'ai pas pu activer le freeze sur les adresses actives.");
    if (!allFreezeOk) {
        result["workflowStatus"] = "freeze_partial_or_failed";
        result["error"] = "Au moins un freeze a échoué.";
    }

    m_controller.appendSmartSearchDebug("chat_memory_freeze", result);
    return result;
}

// RiskGate chat (29/08/2026) : points d'entree publics, appeles UNIQUEMENT
// apres un clic explicite sur le recoveryAction renvoye par startSmartSearch
// (le clic EST la confirmation, pas de second modal confirmRiskAction --
// retire a la demande du proprietaire, juge redondant avec la carte chat qui
// affiche deja l'avertissement + le libelle exact de l'action). N'ajoutent
// aucune logique d'ecriture : appellent directement les fonctions privees
// existantes, qui n'ont pas change. Query vide car ces fonctions ne
// re-parsent pas d'adresse depuis la query -- elles utilisent
// m_controller.m_chatMemoryTargets/m_controller.m_lastAutoWriteTargets deja peuples cote serveur.
QVariantMap SmartSearchManager::confirmChatMemoryWrite(const QString& value) {
    return writeChatMemoryTargetsFromQuery(QString(), value);
}

QVariantMap SmartSearchManager::confirmChatMemoryFreeze(const QString& value) {
    return freezeChatMemoryTargetsFromQuery(QString(), value);
}

QVariantMap SmartSearchManager::confirmRewriteLastAutoWrite(const QString& value) {
    return m_controller.rewriteLastAutoWriteTargets(value, QString());
}

QVariantMap SmartSearchManager::getActiveChatMemoryTargets() const {
    QVariantMap result;
    QVariantList targets;

    for (const auto& target : m_controller.m_chatMemoryTargets) {
        QVariantMap entry;
        entry["address"] = QString::number(target.address, 16);
        entry["type"] = killcore::valueTypeToString(target.type);
        targets.append(entry);
    }

    result["success"] = true;
    result["count"] = targets.size();
    result["targets"] = targets;
    return result;
}

QVariantMap SmartSearchManager::clearActiveChatMemoryTargets() {
    const int cleared = m_controller.m_chatMemoryTargets.size();
    m_controller.m_chatMemoryTargets.clear();
    m_controller.m_lastAutoWriteTargets.clear();
    m_controller.m_autoWriteValueHistory.clear();
    // Bouton "clear_targets" de l'echelle de secours (buildFailureEscalationRecovery) :
    // appele en direct depuis le frontend, ne passe pas par startSmartSearch,
    // donc rien d'autre ne remet a zero l'etat d'attente d'une relance en
    // cours. Sans ca, le prochain message sans rapport de l'utilisateur est
    // intercepte a tort par AnswerTraceUiStringPrompt/AnswerTraceUiFilterPrompt.
    resetFailureEscalationState();
    m_controller.m_pendingUiStringCandidates.clear();

    QVariantMap result;
    result["success"] = true;
    result["cleared"] = cleared;
    result["targets"] = QVariantList{};
    m_controller.appendSmartSearchDebug("chat_memory_targets_cleared", result);
    return result;
}

QVariantMap SmartSearchManager::clearScanContext() {
    QVariantMap result;
    auto state = m_controller.scanState();
    const auto candidateCount = static_cast<qulonglong>(state.candidates().size());
    const bool hadUndo = m_controller.m_hasPreviousCandidates;
    const bool hadSnapshot = !state.snapshot().isEmpty();
    const bool wasSmartSearchActive = m_controller.m_smartSearchActive;

    state.clearCandidates();
    m_controller.clearCandidateUndo();
    m_controller.clearCandidateValueHistory();
    state.clearSnapshot();
    m_controller.m_smartSearchActive = false;
    m_controller.m_smartSearchInitialValue.clear();
    m_controller.m_smartSearchTargetValue.clear();
    m_controller.m_smartSearchValueType = "Int32";
    m_controller.m_lastBatchStartIndex = -1;
    m_controller.m_lastBatchEndIndex = -1;

    result["success"] = true;
    result["clearedCandidates"] = candidateCount;
    result["hadUndoReduction"] = hadUndo;
    result["hadUnknownSnapshot"] = hadSnapshot;
    result["wasSmartSearchActive"] = wasSmartSearchActive;
    result["message"] = QString("Contexte de scan vidé : %1 candidat(s) supprimé(s).").arg(candidateCount);
    m_controller.appendSmartSearchDebug("scan_context_cleared", result);
    return result;
}

void SmartSearchManager::acknowledgePendingSmartSearchRecovery() {
    if (m_controller.m_pendingRecoveryAction.isEmpty() && m_controller.m_pendingUiStringCandidates.isEmpty()) {
        return;
    }
    m_controller.appendSmartSearchDebug("smart_search_recovery_acknowledged_via_button", {
        {"pendingRecoveryAction", m_controller.m_pendingRecoveryAction},
    });
    m_controller.m_pendingRecoveryAction.clear();
    m_controller.m_pendingUiStringCandidates.clear();
}

QVariantMap SmartSearchManager::getSmartSearchContext() const {
    QVariantMap result;
    QVariantList chatTargets;
    QVariantList profileTargets;

    for (const auto& target : m_controller.m_chatMemoryTargets) {
        QVariantMap entry;
        entry["address"] = QString::number(target.address, 16);
        entry["type"] = killcore::valueTypeToString(target.type);
        chatTargets.append(entry);
    }

    for (const auto& target : m_controller.m_activeProfileTargets) {
        QVariantMap entry;
        entry["profile"] = target.profileName;
        entry["target"] = target.targetName;
        entry["group"] = target.groupName;
        entry["address"] = QString::number(target.address, 16);
        entry["type"] = killcore::valueTypeToString(target.type);
        entry["locatorKind"] = target.locatorKind == killcore::LocatorKind::ClrField ? "clr_field" : "memory";
        if (target.locatorKind == killcore::LocatorKind::ClrField) {
            entry["clrTypeSubstring"] = target.clrTypeSubstring;
            entry["clrIdentityField"] = target.clrIdentityField;
            entry["clrIdentityValue"] = target.clrIdentityValue;
            entry["clrFieldName"] = target.clrFieldName;
        }
        profileTargets.append(entry);
    }

    result["success"] = true;
    result["active"] = m_controller.m_smartSearchActive || !m_controller.m_chatMemoryTargets.isEmpty() || !m_controller.m_activeProfileTargets.isEmpty();
    result["workflow"] = m_controller.m_smartSearchActive
        ? "guided_scan"
        : (!m_controller.m_chatMemoryTargets.isEmpty() ? "active_addresses" : (!m_controller.m_activeProfileTargets.isEmpty() ? "active_profile" : "idle"));
    result["initialValue"] = m_controller.m_smartSearchInitialValue;
    result["targetValue"] = m_controller.m_smartSearchTargetValue;
    result["valueType"] = m_controller.m_smartSearchValueType;
    result["candidateCount"] = static_cast<qulonglong>(m_controller.scanState().candidates().size());
    result["hasUndoReduction"] = m_controller.m_hasPreviousCandidates;
    result["chatTargets"] = chatTargets;
    result["profileTargets"] = profileTargets;
    result["lastAutoWriteCount"] = m_controller.m_lastAutoWriteTargets.size();
    result["writeHistory"] = writeHistoryToVariantList(m_controller.m_autoWriteValueHistory);
    return result;
}

QVariantMap SmartSearchManager::getAutoResolveReport(int maxEvents) const {
    QVariantMap result;
    result["success"] = true;
    result["attached"] = m_controller.m_handle.isValid();
    result["processName"] = m_controller.processName();
    result["candidateCount"] = static_cast<qulonglong>(m_controller.scanState().candidates().size());
    result["workflow"] = m_controller.m_smartSearchActive ? QString("guided_scan") : QString("idle");
    result["initialValue"] = m_controller.m_smartSearchInitialValue;
    result["targetValue"] = m_controller.m_smartSearchTargetValue;
    result["valueType"] = m_controller.m_smartSearchValueType;
    result["activeChatTargetCount"] = m_controller.m_chatMemoryTargets.size();
    result["activeProfileTargetCount"] = m_controller.m_activeProfileTargets.size();

    QSettings settings;
    const QString gameKey = autoResolverGameKey(m_controller.processName());
    settings.beginGroup(QString("autoResolver/process/%1").arg(gameKey));
    QVariantMap learnedProfile;
    learnedProfile["gameKey"] = gameKey;
    learnedProfile["starts"] = settings.value("starts", 0).toInt();
    learnedProfile["reductions"] = settings.value("reductions", 0).toInt();
    learnedProfile["noCandidateCount"] = settings.value("noCandidateCount", 0).toInt();
    learnedProfile["lowCandidateCheckpoints"] = settings.value("lowCandidateCheckpoints", 0).toInt();
    learnedProfile["lastWorkflow"] = settings.value("lastWorkflow").toString();
    learnedProfile["lastCandidateCount"] = settings.value("lastCandidateCount", 0).toULongLong();
    learnedProfile["lastUpdated"] = settings.value("lastUpdated").toString();
    learnedProfile["lastSuccessfulAuditEvent"] = settings.value("lastSuccessfulAuditEvent").toString();
    learnedProfile["lastSuccessfulAuditAt"] = settings.value("lastSuccessfulAuditAt").toString();
    learnedProfile["lastSuccessfulAddress"] = settings.value("lastSuccessfulAddress").toString();
    learnedProfile["lastSuccessfulValueType"] = settings.value("lastSuccessfulValueType").toString();
    learnedProfile["lastSuccessfulAobPattern"] = settings.value("lastSuccessfulAobPattern").toString();
    QVariantMap strategyWins;
    settings.beginGroup("strategyWins");
    const QStringList strategyKeys = settings.childKeys();
    for (const QString& key : strategyKeys) {
        strategyWins[key] = settings.value(key, 0).toInt();
    }
    settings.endGroup();
    learnedProfile["strategyWins"] = strategyWins;
    settings.endGroup();
    result["learnedProfile"] = learnedProfile;

    const int boundedMaxEvents = std::clamp(maxEvents, 5, 200);
    auto readEvents = [](const QString& path, int limit) {
        QVariantList events;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            return events;
        }

        QList<QByteArray> lines;
        while (!file.atEnd()) {
            const QByteArray line = file.readLine().trimmed();
            if (!line.isEmpty()) {
                lines.append(line);
                if (lines.size() > limit) {
                    lines.removeFirst();
                }
            }
        }

        for (const QByteArray& line : lines) {
            QJsonParseError parseError;
            const QJsonDocument doc = QJsonDocument::fromJson(line, &parseError);
            if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
                continue;
            }
            events.append(doc.object().toVariantMap());
        }
        return events;
    };

    QVariantList recentEvents = readEvents(m_controller.scanTelemetryFilePath(), boundedMaxEvents);
    const QVariantList debugEvents = readEvents(m_controller.smartSearchDebugFilePath(), boundedMaxEvents);
    for (const QVariant& event : debugEvents) {
        recentEvents.append(event);
    }

    QVariantMap eventCounts;
    QVariantList lastSignals;
    for (const QVariant& item : recentEvents) {
        const QVariantMap event = item.toMap();
        const QString name = event.value("event").toString();
        if (!name.isEmpty()) {
            eventCounts[name] = eventCounts.value(name).toInt() + 1;
        }

        if (lastSignals.size() >= 12) {
            lastSignals.removeFirst();
        }

        QVariantMap signal;
        signal["event"] = name;
        signal["timestamp"] = event.value("timestamp").toString();
        signal["candidateStoreSize"] = event.value("candidateStoreSize", event.value("candidateCount"));
        signal["matchesFound"] = event.value("matchesFound", event.value("matchCount"));
        signal["remaining"] = event.value("remaining", event.value("stored"));
        signal["globalValueHits"] = event.value("globalValueHits");
        signal["error"] = event.value("error").toString();
        lastSignals.append(signal);
    }

    // Analyse pure de la telemetry (insights + rapport valeur affichee),
    // testee independamment dans tests/unit/test_auto_resolver.cpp. Seule
    // implementation de cette logique — voir la note d'architecture dans
    // ai/auto_resolver.h.
    QList<QVariantMap> eventMaps;
    eventMaps.reserve(recentEvents.size());
    for (const QVariant& item : recentEvents) {
        eventMaps.append(item.toMap());
    }
    const auto telemetryReport = killai::computeAutoResolveTelemetryReport(eventMaps);

    QVariantList telemetryInsights;
    for (const auto& insight : telemetryReport.insights) {
        telemetryInsights.append(QVariantMap{
            {"id", insight.id},
            {"label", insight.label},
            {"reason", insight.reason},
            {"nextAction", insight.nextAction},
            {"safe", insight.safe},
            {"requiresConfirmation", !insight.safe},
        });
    }

    QVariantMap displayValueReport;
    displayValueReport["enabled"] = telemetryReport.displayValueSignals;
    displayValueReport["pattern"] = telemetryReport.displayValuePattern;
    displayValueReport["traceUiSourceCount"] = telemetryReport.traceUiSourceCount;
    displayValueReport["globalValueHits"] = telemetryReport.traceUiGlobalHits;
    displayValueReport["exactZeroCount"] = telemetryReport.exactZeroCount;
    displayValueReport["recommendation"] = telemetryReport.displayValueRecommendation;
    displayValueReport["warnings"] = QVariantList{
        "Les strings UI peuvent etre des copies d'affichage, pas la source gameplay.",
        "Ne pas ecrire globalValueHits en masse; tester Top 5/Top 25 seulement.",
        "Si exact=0 et Trace UI donne des strings, privilegier source analysis avant patch/debug."
    };

    QVariantList recommendations;
    QVariantList guardrails;
    guardrails.append(QVariantMap{{"id", "no_auto_write"}, {"label", "Aucune écriture automatique sans confirmation"}, {"risk", "write"}});
    guardrails.append(QVariantMap{{"id", "no_auto_debug"}, {"label", "Aucun debugger/hardware breakpoint sans confirmation"}, {"risk", "debug"}});
    guardrails.append(QVariantMap{{"id", "no_auto_patch"}, {"label", "Aucun patch/injection sans confirmation"}, {"risk", "patch"}});

    const auto& candidates = m_controller.scanState().candidates();

    if (!m_controller.m_handle.isValid()) {
        recommendations.append(QVariantMap{{"id", "attach_process"}, {"label", "Attacher un processus"}, {"safe", true}, {"reason", "Aucun processus actif."}});
    } else if (!m_controller.m_chatMemoryTargets.isEmpty() || !m_controller.m_activeProfileTargets.isEmpty()) {
        recommendations.append(QVariantMap{{"id", "guarded_write"}, {"label", "Proposer une écriture confirmée"}, {"safe", false}, {"reason", "Des cibles mémoire sont déjà actives."}});
        recommendations.append(QVariantMap{{"id", "guarded_freeze"}, {"label", "Proposer un freeze confirmé"}, {"safe", false}, {"reason", "Des cibles mémoire sont déjà actives."}});
    } else if (m_controller.m_smartSearchActive && !candidates.isEmpty()) {
        recommendations.append(QVariantMap{{"id", "reduce_with_new_value"}, {"label", "Réduire avec la nouvelle valeur observée"}, {"safe", true}, {"reason", "Une recherche guidée contient encore des candidats."}});
        if (candidates.size() <= kAutoWriteCandidateLimit) {
            recommendations.append(QVariantMap{{"id", "review_top_candidates"}, {"label", "Préparer un test d'écriture confirmé"}, {"safe", false}, {"reason", "Le nombre de candidats est assez bas."}});
        }
    } else {
        recommendations.append(QVariantMap{{"id", "exact_or_multitype"}, {"label", "Lancer un scan exact multi-type"}, {"safe", true}, {"reason", "Aucun contexte actif exploitable."}});
        recommendations.append(QVariantMap{{"id", "encrypted_scan"}, {"label", "Essayer un scan chiffré borné"}, {"safe", true}, {"reason", "Utile si le scan exact ne trouve rien."}});
        recommendations.append(QVariantMap{{"id", "unknown_capture"}, {"label", "Capturer unknown initial value"}, {"safe", true}, {"reason", "Utile quand la valeur réelle n'est pas connue ou transformée."}});
    }

    if (eventCounts.value("ui_string_investigation_finish").toInt() > 0 || eventCounts.value("ui_string_sources_analyze").toInt() > 0) {
        recommendations.append(QVariantMap{{"id", "trace_ui_sources"}, {"label", "Exploiter les sources Trace UI string"}, {"safe", true}, {"reason", "La télémétrie récente contient des pistes UI/string."}});
    }
    if (eventCounts.value("find_what_writes").toInt() > 0 || eventCounts.value("aob_signature").toInt() > 0) {
        recommendations.append(QVariantMap{{"id", "trainer_checkpoint"}, {"label", "Préparer checkpoint AOB/patch"}, {"safe", false}, {"reason", "Des signaux debugger/AOB existent déjà."}});
    }
    if (learnedProfile.value("noCandidateCount").toInt() >= 2) {
        recommendations.prepend(QVariantMap{{"id", "trace_ui_string"}, {"label", "Privilégier Trace UI string"}, {"safe", true}, {"reason", "Les scans exacts récents de ce processus ont souvent fini sans candidat."}});
        recommendations.prepend(QVariantMap{{"id", "encrypted_scan"}, {"label", "Privilégier scan chiffré"}, {"safe", true}, {"reason", "Mémoire locale : plusieurs scans sans candidat sur ce processus."}});
    }
    if (learnedProfile.value("noCandidateCount").toInt() >= 4) {
        // Signal fort de réallocation/instabilité mémoire persistante malgré
        // plusieurs stratégies déjà tentées (scan classique, Trace UI string,
        // scan chiffré) : suggérer d'isoler une éventuelle synchro serveur en
        // arrière-plan comme cause, avant de conclure à une réallocation
        // purement locale — voir blockProcessNetwork() et
        // docs/STRATEGY_ROOM.md, 24/08/2026 (cas Solitaire "Bulles").
        recommendations.prepend(QVariantMap{
            {"id", "block_process_network"},
            {"label", "Couper le réseau du processus (diagnostic)"},
            {"safe", false},
            {"requiresConfirmation", true},
            {"reason", "Plusieurs stratégies de scan ont échoué sur ce processus — la valeur est peut-être resynchronisée depuis un serveur en arrière-plan plutôt que purement locale."}
        });
    }
    if (learnedProfile.value("noCandidateCount").toInt() >= 6) {
        // PHASE 91 : au-dela de la coupure reseau (deja suggeree ci-dessus a
        // 4 echecs), une instabilite memoire qui persiste encore apres
        // isolation reseau suggere que la valeur affichee n'est peut-etre
        // meme pas fiablement en memoire — voir docs/PHASE_TRACKER.md
        // PHASE 90 (investigation Solitaire "Bulles") ou la vraie percee a
        // ete de chercher un fichier de sauvegarde sur disque apres l'echec
        // de toutes les pistes memoire.
        recommendations.prepend(QVariantMap{
            {"id", "discover_save_files"},
            {"label", "Chercher fichiers et paramètres UWP sur le disque"},
            {"safe", true},
            {"requiresConfirmation", false},
            {"reason", "De nombreuses strategies memoire ont echoue meme apres isolation reseau — la valeur affichee vient peut-etre d'un fichier de sauvegarde ou de LocalSettings plutot que d'une adresse memoire stable."}
        });
    }
    const QString lastSuccessfulAudit = learnedProfile.value("lastSuccessfulAuditEvent").toString();
    if (!lastSuccessfulAudit.isEmpty()) {
        recommendations.prepend(QVariantMap{
            {"id", "reuse_successful_strategy"},
            {"label", "Réutiliser la dernière stratégie gagnante"},
            {"safe", lastSuccessfulAudit.contains("write") || lastSuccessfulAudit.contains("freeze") ? false : true},
            {"requiresConfirmation", lastSuccessfulAudit.contains("write") || lastSuccessfulAudit.contains("freeze")},
            {"reason", QString("Dernière action validée pour ce processus : %1.").arg(lastSuccessfulAudit)}
        });
    }

    QVariantList strategyScores;
    auto addStrategyScore = [&strategyScores](const QString& id, const QString& label, int score, const QString& reason) {
        strategyScores.append(QVariantMap{{"id", id}, {"label", label}, {"score", score}, {"reason", reason}});
    };

    int exactScore = m_controller.m_handle.isValid() ? 55 : 0;
    int reduceScore = (m_controller.m_smartSearchActive && !candidates.isEmpty()) ? 95 : 0;
    int unknownScore = m_controller.m_handle.isValid() ? 45 : 0;
    int encryptedScore = m_controller.m_handle.isValid() ? 35 : 0;
    int traceUiScore = m_controller.m_handle.isValid() ? 30 : 0;

    const QString processLower = m_controller.processName().toLower();
    if (isStarCraftLikeProcessName(processLower)) {
        traceUiScore += 25;
        encryptedScore += 10;
    }
    if (learnedProfile.value("noCandidateCount").toInt() >= 2) {
        encryptedScore += 35;
        traceUiScore += 30;
        exactScore -= 20;
    }
    if (lastSuccessfulAudit.contains("aob")) {
        traceUiScore += 10;
    }
    if (lastSuccessfulAudit.contains("write") || lastSuccessfulAudit.contains("freeze")) {
        reduceScore += 10;
    }
    if (eventCounts.value("ui_string_scan").toInt() > 0 || eventCounts.value("ui_string_sources_analyze").toInt() > 0) {
        traceUiScore += 25;
    }
    if (candidates.size() > 50000) {
        unknownScore += 10;
        traceUiScore += 10;
    }

    addStrategyScore("reduce_with_new_value", "Réduire candidats existants", reduceScore, "Meilleur choix quand une recherche guidée est active.");
    addStrategyScore("exact_or_multitype", "Scan exact multi-type", exactScore, "Point d'entrée le plus rapide quand la valeur réelle est connue.");
    addStrategyScore("unknown_capture", "Unknown initial value", unknownScore, "Bon choix si la valeur bouge mais la représentation mémoire est inconnue.");
    addStrategyScore("encrypted_scan", "Scan chiffré borné", encryptedScore, "Bon choix si le scan exact échoue souvent.");
    addStrategyScore("trace_ui_string", "Trace UI string", traceUiScore, "Bon choix si le jeu affiche une copie UI plutôt que la source gameplay.");

    std::sort(strategyScores.begin(), strategyScores.end(), [](const QVariant& a, const QVariant& b) {
        return a.toMap().value("score").toInt() > b.toMap().value("score").toInt();
    });

    const QVariantMap preferredStrategy = strategyScores.isEmpty() ? QVariantMap{} : strategyScores.first().toMap();
    auto proactiveAction = [](const QString& id,
                              const QString& label,
                              const QString& tool,
                              int confidence,
                              bool safe,
                              const QString& risk,
                              const QString& reason) {
        QVariantMap action;
        action["id"] = id;
        action["label"] = label;
        action["tool"] = tool;
        action["confidence"] = std::clamp(confidence, 0, 100);
        action["safe"] = safe;
        action["requiresConfirmation"] = !safe;
        action["risk"] = risk;
        action["reason"] = reason;
        action["proactive"] = true;
        return action;
    };

    QVariantMap nextBestAction;
    if (!m_controller.m_handle.isValid()) {
        nextBestAction = proactiveAction(
            "attach_process",
            "Attacher un processus",
            "attach_process",
            100,
            true,
            "safe",
            "Aucun processus actif : l'assistant ne peut pas scanner tant qu'une cible autorisée n'est pas attachée.");
    } else if (!m_controller.m_chatMemoryTargets.isEmpty() || !m_controller.m_activeProfileTargets.isEmpty()) {
        nextBestAction = proactiveAction(
            "guarded_write",
            "Préparer une écriture confirmée",
            "chat_memory_write",
            88,
            false,
            "write",
            "Des cibles mémoire sont déjà actives; la prochaine action utile est un test confirmé, pas une nouvelle recherche.");
    } else if (m_controller.m_smartSearchActive && !candidates.isEmpty()) {
        const int confidence = candidates.size() <= kAutoWriteCandidateLimit ? 90 : 82;
        nextBestAction = proactiveAction(
            candidates.size() <= kAutoWriteCandidateLimit ? "review_top_candidates" : "reduce_with_new_value",
            candidates.size() <= kAutoWriteCandidateLimit ? "Préparer test d'écriture confirmé" : "Réduire avec nouvelle valeur",
            candidates.size() <= kAutoWriteCandidateLimit ? "prepare_guarded_write" : "next_scan",
            confidence,
            candidates.size() > kAutoWriteCandidateLimit,
            candidates.size() <= kAutoWriteCandidateLimit ? "write" : "safe",
            candidates.size() <= kAutoWriteCandidateLimit
                ? "Le nombre de candidats est assez bas; il faut passer par une confirmation avant écriture/freeze."
                : "Une recherche est déjà active; refaire varier la valeur donnera la réduction la plus rentable.");
    } else {
        const QString strategyId = preferredStrategy.value("id").toString();
        const int confidence = preferredStrategy.value("score", 50).toInt();
        if (strategyId == "trace_ui_string") {
            nextBestAction = proactiveAction("trace_ui_string", "Lancer Trace UI string", "scan_ui_strings", confidence, true, "safe", preferredStrategy.value("reason").toString());
        } else if (strategyId == "encrypted_scan") {
            nextBestAction = proactiveAction("encrypted_scan", "Lancer scan chiffré borné", "scan_encrypted_value", confidence, true, "safe", preferredStrategy.value("reason").toString());
        } else if (strategyId == "unknown_capture") {
            nextBestAction = proactiveAction("unknown_capture", "Capturer Unknown initial value", "unknown_capture", confidence, true, "safe", preferredStrategy.value("reason").toString());
        } else {
            nextBestAction = proactiveAction("exact_or_multitype", "Lancer scan exact multi-type", "exact_scan_multi_type", confidence, true, "safe", preferredStrategy.value("reason").toString());
        }
    }

    if (!lastSuccessfulAudit.isEmpty()) {
        nextBestAction["learnedFrom"] = lastSuccessfulAudit;
    }
    if (telemetryReport.displayValueSignals) {
        nextBestAction["displayValueAware"] = true;
    }

    result["strategyScores"] = strategyScores;
    result["preferredStrategy"] = preferredStrategy;
    result["nextBestAction"] = nextBestAction;

    result["eventCounts"] = eventCounts;
    result["recentSignals"] = lastSignals;
    result["telemetryInsights"] = telemetryInsights;
    result["displayValueReport"] = displayValueReport;
    QVariantMap aobReport;
    aobReport["multiMatchCount"] = telemetryReport.aobMultiMatchCount;
    aobReport["weakQualityCount"] = telemetryReport.aobWeakQualityCount;
    aobReport["trainerBlockedCount"] = telemetryReport.trainerBlockedCount;
    if (telemetryReport.aobMultiMatchCount > 0) {
        aobReport["matchesFound"] = 2;
    } else if (eventCounts.value("aob_scan").toInt() > 0 || eventCounts.value("aob_signature").toInt() > 0) {
        aobReport["matchesFound"] = 1;
    }
    aobReport["qualityReady"] = telemetryReport.aobWeakQualityCount == 0 && telemetryReport.trainerBlockedCount == 0 && telemetryReport.aobMultiMatchCount == 0;
    result["aob"] = aobReport;
    result["recommendations"] = recommendations;
    result["guardrails"] = guardrails;
    result["summary"] = QString("%1 candidat(s), %2 cible(s) active(s), %3 événement(s) récent(s).")
        .arg(candidates.size())
        .arg(m_controller.m_chatMemoryTargets.size() + m_controller.m_activeProfileTargets.size())
        .arg(recentEvents.size());
    return result;
}

QVariantMap SmartSearchManager::clearAutoResolveMemory(bool allProcesses) {
    QSettings settings;
    QVariantMap result;
    result["success"] = true;
    result["allProcesses"] = allProcesses;
    if (allProcesses) {
        settings.remove("autoResolver");
        result["message"] = "Mémoire Auto vidée pour tous les processus.";
    } else {
        const QString gameKey = autoResolverGameKey(m_controller.processName());
        settings.remove(QString("autoResolver/process/%1").arg(gameKey));
        result["gameKey"] = gameKey;
        result["message"] = QString("Mémoire Auto vidée pour %1.").arg(gameKey);
    }
    m_controller.appendSmartSearchDebug("auto_resolve_memory_cleared", result);
    return result;
}

QVariantMap SmartSearchManager::logAiAudit(const QString& event, const QVariantMap& payload) {
    const QString cleanEvent = event.trimmed().isEmpty()
        ? QString("ai_audit")
        : event.trimmed().left(80);
    QVariantMap entry = payload;
    entry["processName"] = m_controller.processName();
    entry["pid"] = m_controller.m_pid;
    entry["auditEvent"] = cleanEvent;
    m_controller.appendScanTelemetry("ai_audit", entry);

    const bool successfulAction = payload.value("success").toBool()
        || cleanEvent == "risk_confirmed"
        || cleanEvent.endsWith("_prepared");
    if (successfulAction) {
        QSettings settings;
        const QString gameKey = autoResolverGameKey(m_controller.processName());
        settings.beginGroup(QString("autoResolver/process/%1").arg(gameKey));
        settings.setValue("lastSuccessfulAuditEvent", cleanEvent);
        settings.setValue("lastSuccessfulAuditAt", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
        settings.setValue(
            QString("strategyWins/%1").arg(cleanEvent),
            settings.value(QString("strategyWins/%1").arg(cleanEvent), 0).toInt() + 1);
        const QString address = payload.value("address").toString();
        if (!address.isEmpty()) {
            settings.setValue("lastSuccessfulAddress", address);
        }
        const QString type = payload.value("type").toString();
        if (!type.isEmpty()) {
            settings.setValue("lastSuccessfulValueType", type);
        }
        const QString aobPattern = payload.value("aobPattern").toString();
        if (!aobPattern.isEmpty()) {
            settings.setValue("lastSuccessfulAobPattern", aobPattern.left(512));
        }

        // Memoire de pattern structuree (module + offset relatif, pas
        // l'adresse absolue ci-dessus qui ne survit pas a l'ASLR) — plusieurs
        // entrees distinctes par jeu au lieu d'un seul "dernier succes"
        // ecrase a chaque fois. Reutilisable au prochain lancement du meme
        // executable via getRememberedPatterns().
        uint64_t rawAddress = 0;
        if (!address.isEmpty() && m_controller.m_pid > 0 && parseHexAddress(address, &rawAddress)) {
            QString module;
            uint64_t moduleOffset = 0;
            if (resolveModuleOffset(static_cast<uint32_t>(m_controller.m_pid), rawAddress, &module, &moduleOffset)) {
                QJsonArray patterns = QJsonDocument::fromJson(
                    settings.value("rememberedPatterns").toByteArray()).array();

                const QString offsetHex = QString::number(moduleOffset, 16);
                int existingIndex = -1;
                for (int i = 0; i < patterns.size(); ++i) {
                    const QJsonObject entry = patterns.at(i).toObject();
                    if (entry.value("module").toString().compare(module, Qt::CaseInsensitive) == 0 &&
                        entry.value("moduleOffset").toString() == offsetHex) {
                        existingIndex = i;
                        break;
                    }
                }

                // Role semantique "infere" (roadmap H.2 point 3 / STRATEGY_ROOM.md,
                // tranche le 19/08/2026 en faveur de l'option 2 deja recommandee) :
                // reutilise "objective" — deja transmis a CHAQUE appel logAiAudit
                // via le wrapper frontend (ui/src/stores/app.ts, searchQuery.value ou
                // activeInvestigation.objective) — comme libelle lisible, plutot que
                // d'ajouter une question explicite qui casserait le flux sans friction.
                // Quelques objectifs generiques (placeholders de reset de contexte,
                // pas une vraie phrase utilisateur) sont exclus pour ne pas figer un
                // faux "role" du type "nouvelle recherche" a la place d'un vrai libelle.
                const QString objective = payload.value("objective").toString().trimmed();
                const QString objectiveLower = objective.toLower();
                const bool objectiveIsGeneric = objective.isEmpty()
                    || objectiveLower == "investigation manuelle"
                    || objectiveLower == "nouvelle recherche"
                    || objectiveLower.startsWith("nouvelle recherche ")
                    || objectiveLower.startsWith("j'utilise ces mémoires");
                const QString previousQueryLabel = existingIndex >= 0
                    ? patterns.at(existingIndex).toObject().value("queryLabel").toString()
                    : QString();

                QJsonObject entry;
                entry["module"] = module;
                entry["moduleOffset"] = offsetHex;
                entry["valueType"] = type;
                entry["aobPattern"] = aobPattern.left(512);
                entry["auditEvent"] = cleanEvent;
                entry["queryLabel"] = objectiveIsGeneric ? previousQueryLabel : objective.left(120);
                entry["confirmedAt"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
                entry["confirmCount"] = (existingIndex >= 0
                    ? patterns.at(existingIndex).toObject().value("confirmCount").toInt(0)
                    : 0) + 1;

                if (existingIndex >= 0) {
                    patterns.removeAt(existingIndex);
                }
                patterns.append(entry);
                constexpr int kRememberedPatternLimit = 20;
                while (patterns.size() > kRememberedPatternLimit) {
                    patterns.removeAt(0);
                }
                settings.setValue("rememberedPatterns", QJsonDocument(patterns).toJson(QJsonDocument::Compact));
            }
        }

        settings.endGroup();
    }

    QVariantMap result;
    result["success"] = true;
    result["event"] = cleanEvent;
    return result;
}

QVariantMap SmartSearchManager::getRememberedPatterns() const {
    QVariantMap result;
    result["success"] = true;

    const QString gameKey = autoResolverGameKey(m_controller.processName());
    result["gameKey"] = gameKey;

    QSettings settings;
    settings.beginGroup(QString("autoResolver/process/%1").arg(gameKey));
    const QJsonArray patterns = QJsonDocument::fromJson(settings.value("rememberedPatterns").toByteArray()).array();
    settings.endGroup();

    // Modules du processus attache, pour resoudre module+offset -> adresse
    // live sans redemander un scan. Vide si rien n'est attache : la liste
    // reste utile en lecture seule (voir ce qui a deja marche sur ce jeu).
    QHash<QString, uint64_t> moduleBases;
    if (m_controller.m_handle.isValid() && m_controller.m_pid > 0) {
        for (const auto& mod : killcore::ProcessEnumerator::enumerateModules(static_cast<uint32_t>(m_controller.m_pid))) {
            moduleBases.insert(mod.name.toLower(), mod.baseAddress);
        }
    }

    QVariantList entries;
    for (const auto& item : patterns) {
        const QJsonObject obj = item.toObject();
        QVariantMap entry;
        const QString module = obj.value("module").toString();
        const QString offsetHex = obj.value("moduleOffset").toString();
        entry["module"] = module;
        entry["moduleOffset"] = offsetHex;
        entry["valueType"] = obj.value("valueType").toString();
        entry["aobPattern"] = obj.value("aobPattern").toString();
        entry["auditEvent"] = obj.value("auditEvent").toString();
        entry["queryLabel"] = obj.value("queryLabel").toString();
        entry["confirmedAt"] = obj.value("confirmedAt").toString();
        entry["confirmCount"] = obj.value("confirmCount").toInt(1);

        const auto baseIt = moduleBases.constFind(module.toLower());
        if (baseIt != moduleBases.constEnd()) {
            bool ok = false;
            const uint64_t offset = offsetHex.toULongLong(&ok, 16);
            if (ok) {
                entry["resolved"] = true;
                entry["liveAddress"] = QString::number(baseIt.value() + offset, 16).toUpper();
            } else {
                entry["resolved"] = false;
            }
        } else {
            entry["resolved"] = false;
        }
        entries.append(entry);
    }
    // Les plus recemment confirmes en premier — les plus susceptibles d'etre
    // encore pertinents (un role peut avoir change d'offset entre deux
    // versions du jeu, la confirmation la plus fraiche est le meilleur signal).
    std::sort(entries.begin(), entries.end(), [](const QVariant& a, const QVariant& b) {
        return a.toMap().value("confirmedAt").toString() > b.toMap().value("confirmedAt").toString();
    });

    result["patterns"] = entries;
    result["patternCount"] = entries.size();
    return result;
}

QVariantMap SmartSearchManager::getWriteHistorySequence() const {
    QVariantMap result;
    result["success"] = true;

    const QString gameKey = autoResolverGameKey(m_controller.processName());
    result["gameKey"] = gameKey;

    QSettings settings;
    settings.beginGroup(QString("writeHistory/process/%1").arg(gameKey));
    const QJsonArray sequence = QJsonDocument::fromJson(settings.value("sequence").toByteArray()).array();
    settings.endGroup();

    // Modules du processus attaché, pour résoudre module+offset -> adresse
    // live sans redemander un scan — même démarche que getRememberedPatterns.
    QHash<QString, uint64_t> moduleBases;
    if (m_controller.m_handle.isValid() && m_controller.m_pid > 0) {
        for (const auto& mod : killcore::ProcessEnumerator::enumerateModules(static_cast<uint32_t>(m_controller.m_pid))) {
            moduleBases.insert(mod.name.toLower(), mod.baseAddress);
        }
    }

    QVariantList entries;
    for (const auto& item : sequence) {
        const QJsonObject obj = item.toObject();
        QVariantMap entry;
        const QString module = obj.value("module").toString();
        const QString offsetHex = obj.value("moduleOffset").toString();
        entry["module"] = module;
        entry["moduleOffset"] = offsetHex;
        entry["valueType"] = obj.value("valueType").toString();
        entry["value"] = obj.value("value").toString();
        entry["writtenAt"] = obj.value("writtenAt").toString();

        const auto baseIt = moduleBases.constFind(module.toLower());
        bool resolved = false;
        if (baseIt != moduleBases.constEnd()) {
            bool ok = false;
            const uint64_t offset = offsetHex.toULongLong(&ok, 16);
            if (ok) {
                resolved = true;
                entry["liveAddress"] = QString::number(baseIt.value() + offset, 16).toUpper();
            }
        }
        entry["resolved"] = resolved;
        entries.append(entry);
    }

    result["sequence"] = entries;
    result["sequenceCount"] = entries.size();
    return result;
}

QVariantMap SmartSearchManager::replayWriteHistorySequence() {
    QVariantMap result;
    result["success"] = false;

    if (!m_controller.m_attached || !m_controller.m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        return result;
    }

    const QVariantMap sequenceResult = getWriteHistorySequence();
    const QVariantList entries = sequenceResult.value("sequence").toList();
    if (entries.isEmpty()) {
        result["error"] = "Aucune séquence d'écritures à rejouer pour cet exécutable.";
        return result;
    }

    QVariantList details;
    int replayedCount = 0;
    int skippedCount = 0;
    int failedCount = 0;
    for (const auto& item : entries) {
        const QVariantMap entry = item.toMap();
        QVariantMap detail;
        detail["module"] = entry.value("module");
        detail["moduleOffset"] = entry.value("moduleOffset");
        detail["valueType"] = entry.value("valueType");
        detail["value"] = entry.value("value");
        if (!entry.value("resolved").toBool()) {
            detail["success"] = false;
            detail["error"] = "Module non chargé dans le processus attaché.";
            skippedCount++;
            details.append(detail);
            continue;
        }
        const QString liveAddress = entry.value("liveAddress").toString();
        // persistHistory=false : on rejoue une sequence deja persistee, ne pas
        // la re-logger a chaque replay (sinon croissance/duplication a chaque appel).
        const auto writeResult = m_controller.writeMemoryValueConfirmed(
            liveAddress, entry.value("valueType").toString(), entry.value("value").toString(), false);
        detail["liveAddress"] = liveAddress;
        detail["success"] = writeResult.value("success").toBool();
        if (writeResult.value("success").toBool()) {
            replayedCount++;
        } else {
            detail["error"] = writeResult.value("error");
            failedCount++;
        }
        details.append(detail);
    }

    result["success"] = replayedCount > 0;
    result["replayedCount"] = replayedCount;
    result["skippedCount"] = skippedCount;
    result["failedCount"] = failedCount;
    result["details"] = details;
    m_controller.appendScanTelemetry("write_history_replay", result);
    return result;
}

QVariantMap SmartSearchManager::clearWriteHistorySequence() {
    const QString gameKey = autoResolverGameKey(m_controller.processName());
    QSettings settings;
    settings.beginGroup(QString("writeHistory/process/%1").arg(gameKey));
    settings.remove("sequence");
    settings.endGroup();

    QVariantMap result;
    result["success"] = true;
    result["gameKey"] = gameKey;
    return result;
}

QVariantMap SmartSearchManager::writeProfileTargetsFromQuery(const QString& query, const QString& value) {
    QVariantMap result;
    result["success"] = false;

    if (!m_controller.m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        return result;
    }

    const QString normalizedQuery = normalizedProfileText(query);
    const auto profileNames = killcore::ProfileStore::listProfiles();
    QList<ApplicationController::ActiveProfileTarget> resolvedTargets;
    QString matchedGroupName;

    for (const auto& target : m_controller.m_activeProfileTargets) {
        if (!target.groupName.isEmpty() && normalizedQuery.contains(target.groupName)) {
            matchedGroupName = target.groupName;
            resolvedTargets.append(target);
        }
    }

    for (const auto& profileName : profileNames) {
        killcore::Profile profile;
        if (!killcore::ProfileStore::load(killcore::ProfileStore::profilePath(profileName), &profile)) {
            continue;
        }

        if (!profile.executableName.isEmpty()
            && !m_controller.m_processName.isEmpty()
            && profile.executableName.compare(m_controller.m_processName, Qt::CaseInsensitive) != 0) {
            continue;
        }

        for (const auto& target : profile.targets) {
            const QString groupName = profileTargetGroupName(target.name);
            if (groupName.isEmpty() || !normalizedQuery.contains(groupName)) {
                continue;
            }

            ApplicationController::ActiveProfileTarget active{
                profileName,
                target.name,
                groupName,
                0,
                target.type,
                target.locator.kind,
            };

            uint64_t address = 0;
            if (target.locator.kind == killcore::LocatorKind::ClrField) {
                const auto& locator = target.locator.clrField;
                auto locatorResult = m_controller.findClrObjectsByFieldValue(
                    locator.typeSubstring,
                    locator.identityField,
                    locator.identityValue,
                    1);
                const QVariantMap payload = locatorResult.value("result").toMap();
                const QVariantList matches = payload.value("matches").toList();
                if (!locatorResult.value("success").toBool() || matches.isEmpty()) {
                    continue;
                }
                const QString objectAddress = matches.first().toMap().value("address").toString();
                if (!parseHexAddress(objectAddress, &address)) {
                    continue;
                }
                active.clrTypeSubstring = locator.typeSubstring;
                active.clrIdentityField = locator.identityField;
                active.clrIdentityValue = locator.identityValue;
                active.clrFieldName = locator.targetField;
            } else if (!killcore::resolveLocatorAddress(m_controller.m_handle, target.locator, &address)) {
                continue;
            }
            active.address = address;

            matchedGroupName = groupName;
            bool alreadyResolved = false;
            for (const auto& existing : resolvedTargets) {
                if (existing.profileName == profileName && existing.targetName == target.name) {
                    alreadyResolved = true;
                    break;
                }
            }
            if (!alreadyResolved) {
                resolvedTargets.append(active);
            }
        }
    }

    if (resolvedTargets.isEmpty()) {
        return result;
    }

    QVariantList suggestions;
    QVariantList writeResults;
    bool allWritesOk = true;
    const QString previousTargetValue = m_controller.m_smartSearchTargetValue;
    m_controller.m_smartSearchTargetValue = value;
    m_controller.m_lastBatchStartIndex = m_controller.m_writeHistory.size();
    m_controller.m_lastAutoWriteTargets.clear();
    bool wroteRawMemoryTarget = false;

    for (const auto& target : resolvedTargets) {
        QVariantMap suggestion;
        suggestion["profile"] = target.profileName;
        suggestion["target"] = target.targetName;
        suggestion["address"] = QString::number(target.address, 16);
        suggestion["type"] = killcore::valueTypeToString(target.type);
        suggestion["value"] = value;
        suggestion["locatorKind"] = target.locatorKind == killcore::LocatorKind::ClrField ? "clr_field" : "memory";
        if (target.locatorKind == killcore::LocatorKind::ClrField) {
            suggestion["clrTypeSubstring"] = target.clrTypeSubstring;
            suggestion["clrIdentityField"] = target.clrIdentityField;
            suggestion["clrIdentityValue"] = target.clrIdentityValue;
            suggestion["clrFieldName"] = target.clrFieldName;
        }
        const QVariantList history = target.locatorKind == killcore::LocatorKind::ClrField
            ? QVariantList{}
            : m_controller.candidateValueHistory(target.address);
        if (!history.isEmpty()) {
            suggestion["valueHistory"] = history;
        }
        suggestions.append(suggestion);

        QVariantMap writeResult;
        if (target.locatorKind == killcore::LocatorKind::ClrField) {
            writeResult = m_controller.writeClrPrimitiveField(
                suggestion.value("address").toString(),
                target.clrFieldName,
                value);
        } else {
            writeResult = m_controller.writeMemoryValueConfirmed(
                suggestion.value("address").toString(),
                suggestion.value("type").toString(),
                value);
        }
        writeResult.insert("profile", suggestion.value("profile"));
        writeResult.insert("target", suggestion.value("target"));
        writeResult.insert("address", suggestion.value("address"));
        writeResult.insert("value", value);
        writeResult.insert("type", suggestion.value("type"));
        writeResult.insert("locatorKind", suggestion.value("locatorKind"));
        if (target.locatorKind == killcore::LocatorKind::ClrField) {
            writeResult.insert("clrFieldName", target.clrFieldName);
            writeResult.insert("clrTypeSubstring", target.clrTypeSubstring);
            writeResult.insert("clrIdentityField", target.clrIdentityField);
            writeResult.insert("clrIdentityValue", target.clrIdentityValue);
        }
        if (suggestion.contains("valueHistory")) {
            writeResult.insert("valueHistory", suggestion.value("valueHistory"));
        }
        allWritesOk = allWritesOk && writeResult.value("success").toBool();
        writeResults.append(writeResult);

        if (writeResult.value("success").toBool()) {
            if (target.locatorKind != killcore::LocatorKind::ClrField) {
                m_controller.m_lastAutoWriteTargets.append({target.address, target.type});
                wroteRawMemoryTarget = true;
            }
            bool updatedActiveTarget = false;
            for (auto& activeTarget : m_controller.m_activeProfileTargets) {
                if (activeTarget.profileName == target.profileName && activeTarget.targetName == target.targetName) {
                    activeTarget = target;
                    updatedActiveTarget = true;
                    break;
                }
            }
            if (!updatedActiveTarget) {
                m_controller.m_activeProfileTargets.append(target);
            }
        }
    }

    m_controller.m_lastBatchEndIndex = m_controller.m_writeHistory.size();
    if (m_controller.m_lastBatchEndIndex == m_controller.m_lastBatchStartIndex) {
        m_controller.m_lastBatchStartIndex = -1;
        m_controller.m_lastBatchEndIndex = -1;
        m_controller.m_lastAutoWriteTargets.clear();
    }
    if (allWritesOk) {
        m_controller.m_smartSearchActive = false;
        if (wroteRawMemoryTarget) {
            m_controller.m_chatMemoryTargets = m_controller.m_lastAutoWriteTargets;
        }
        resetFailureEscalationState();
        if (m_controller.m_autoWriteValueHistory.isEmpty() && !previousTargetValue.isEmpty()) {
            appendDistinctText(&m_controller.m_autoWriteValueHistory, previousTargetValue, 12);
        }
        appendDistinctText(&m_controller.m_autoWriteValueHistory, value, 12);
    }

    QVariantMap actionResult;
    actionResult["success"] = allWritesOk;
    actionResult["remaining"] = static_cast<qulonglong>(resolvedTargets.size());
    actionResult["error"] = allWritesOk ? QString() : QString("Au moins une écriture depuis le profil a échoué.");

    result["success"] = allWritesOk;
    result["query"] = query;
    result["aiReady"] = m_controller.m_ai.isReady();
    result["status"] = "tool_call";
    result["tool"] = "profile_write";
    result["actionStatus"] = "executed";
    result["workflowStatus"] = allWritesOk ? "auto_write_done" : "auto_write_partial_or_failed";
    result["targetValue"] = value;
    result["actionResult"] = actionResult;
    result["suggestedWrites"] = suggestions;
    result["suggestedWrite"] = suggestions.isEmpty() ? QVariantMap{} : suggestions.first().toMap();
    result["autoWriteResults"] = writeResults;
    result["autoWriteResult"] = writeResults.isEmpty() ? QVariantMap{} : writeResults.last().toMap();
    result["autoWriteCount"] = writeResults.size();
    result["activeTargetCount"] = m_controller.m_chatMemoryTargets.size() + m_controller.m_activeProfileTargets.size();
    result["previousTargetValue"] = previousTargetValue;
    result["writeHistory"] = writeHistoryToVariantList(m_controller.m_autoWriteValueHistory);
    result["rollbackNote"] = wroteRawMemoryTarget
        ? "Tu peux annuler les écritures mémoire brutes via le bouton rollback batch dans l'assistant. Les champs CLR passent par ClrMD et ne sont pas ajoutés au rollback mémoire."
        : "Écriture CLR effectuée via ClrMD : aucun rollback mémoire brut n'a été ajouté.";
    result["message"] = allWritesOk
        ? QString("J'ai utilisé le profil et j'ai mis %1 sur %2 cible(s) \"%3\". Les cibles CLR restent reliées à leur locator logique.")
              .arg(value)
              .arg(resolvedTargets.size())
              .arg(matchedGroupName)
        : QString("J'ai trouvé %1 cible(s) \"%2\" dans le profil, mais au moins une écriture vers %3 a échoué.")
              .arg(resolvedTargets.size())
              .arg(matchedGroupName)
              .arg(value);
    m_controller.appendSmartSearchDebug("profile_write", result);
    return result;
}

QVariantMap SmartSearchManager::startAutoResolve(const QString& query, const QVariantMap& options) {
    QVariantMap result;
    result["success"] = false;
    result["query"] = query;
    result["status"] = "auto_resolve";
    result["workflowStatus"] = "auto_resolve_planned";
    result["aiReady"] = m_controller.m_ai.isReady();

    const QString trimmed = query.trimmed();
    if (trimmed.isEmpty()) {
        result["error"] = "Objectif vide.";
        result["message"] = "Donne-moi un objectif avec une valeur, par exemple : minéraux 41250 vers 99999.";
        return result;
    }
    if (!m_controller.m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        result["message"] = "Attache d'abord un processus, puis relance l'auto-résolution.";
        return result;
    }

    const QStringList numbers = numbersFromText(trimmed);
    if (numbers.isEmpty()) {
        result["error"] = "Aucune valeur numérique détectée.";
        result["message"] = "J'ai besoin au minimum d'une valeur actuelle pour démarrer le plan.";
        return result;
    }

    const QString explicitType = explicitValueTypeFromText(trimmed);
    const QString valueType = options.value("valueType", explicitType.isEmpty() ? QString("Int32") : explicitType).toString();
    const bool executeSafe = options.value("executeSafe", true).toBool();

    bool targetOk = false;
    const qlonglong targetValue = numbers.first().toLongLong(&targetOk);
    if (!targetOk) {
        result["error"] = "Valeur numérique invalide.";
        return result;
    }

    killai::AutoResolveGoal goal;
    goal.description = trimmed;
    goal.targetValue = targetValue;
    goal.valueType = valueType;
    goal.gameContext = m_controller.processName();

    killai::AutoResolver resolver;
    const auto plan = resolver.planForGoal(goal);
    result["plan"] = autoResolveStepsToVariantList(plan);
    result["planStepCount"] = plan.size();
    result["targetValue"] = numbers.size() > 1 ? numbers.at(1) : numbers.first();
    result["initialValue"] = numbers.first();
    result["valueType"] = valueType;

    auto rememberAutoResolverProgress = [this](const QString& workflow, qulonglong candidateCount) {
        QSettings settings;
        const QString gameKey = autoResolverGameKey(m_controller.processName());
        settings.beginGroup(QString("autoResolver/process/%1").arg(gameKey));
        settings.setValue("starts", settings.value("starts", 0).toInt() + 1);
        if (workflow.contains("reduce", Qt::CaseInsensitive) || workflow.contains("refinement", Qt::CaseInsensitive)) {
            settings.setValue("reductions", settings.value("reductions", 0).toInt() + 1);
        }
        if (candidateCount == 0) {
            settings.setValue("noCandidateCount", settings.value("noCandidateCount", 0).toInt() + 1);
        }
        if (candidateCount > 0 && candidateCount <= kAutoWriteCandidateLimit) {
            settings.setValue("lowCandidateCheckpoints", settings.value("lowCandidateCheckpoints", 0).toInt() + 1);
        }
        settings.setValue("lastWorkflow", workflow);
        settings.setValue("lastCandidateCount", candidateCount);
        settings.setValue("lastUpdated", QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
        settings.endGroup();
    };

    QVariantList actions;
    actions.append(QVariantMap{{"id", "run_exact"}, {"label", "Scan Auto multi-type"}, {"safe", true}});
    actions.append(QVariantMap{{"id", "try_unknown_increased"}, {"label", "Passer en Unknown"}, {"safe", true}});
    actions.append(QVariantMap{{"id", "open_expert"}, {"label", "Ouvrir Expert"}, {"safe", true}});
    QVariantList executedSafeSteps;
    const int maxSafeSteps = std::clamp(options.value("maxSafeSteps", 2).toInt(), 1, 5);
    auto appendSafeStep = [&executedSafeSteps](const QString& tool, const QString& status, const QString& detail, const QVariantMap& payload = {}) {
        QVariantMap step;
        step["tool"] = tool;
        step["status"] = status;
        step["detail"] = detail;
        step["safe"] = true;
        if (!payload.isEmpty()) {
            step["payload"] = payload;
        }
        executedSafeSteps.append(step);
    };
    result["contextReport"] = getAutoResolveReport(80);

    if (!executeSafe) {
        result["success"] = true;
        result["actionStatus"] = "planned_only";
        result["nextActions"] = actions;
        result["message"] = QString("Plan auto prêt : %1 étapes. Je n'ai rien exécuté parce que le mode exécution sûre est désactivé.")
                                .arg(plan.size());
        m_controller.appendSmartSearchDebug("auto_resolve_plan", result);
        return result;
    }

    const auto& candidates = m_controller.scanState().candidates();
    if (m_controller.m_smartSearchActive && !candidates.isEmpty()) {
        QVariantMap reduction = m_controller.nextScan("exact", numbers.first());
        appendSafeStep(
            "next_scan",
            reduction.value("success").toBool() ? "success" : "error",
            QString("Réduction exacte avec la nouvelle valeur %1.").arg(numbers.first()),
            reduction);
        result["firstAction"] = reduction;
        result["safeAction"] = "next_scan";
        result["success"] = reduction.value("success").toBool();
        result["actionStatus"] = result.value("success").toBool() ? "safe_reduction_executed" : "safe_reduction_failed";
        const qulonglong remaining = reduction.value("remaining", reduction.value("candidateStoreSize")).toULongLong();
        result["candidateCount"] = remaining;
        result["workflowStatus"] = remaining == 0
            ? "auto_resolve_no_candidate"
            : (remaining <= kAutoWriteCandidateLimit ? "awaiting_write_confirmation" : "needs_more_refinement");
        rememberAutoResolverProgress(result.value("workflowStatus").toString(), remaining);
        result["contextReport"] = getAutoResolveReport(80);

        actions.clear();
        if (remaining == 0) {
            if (executedSafeSteps.size() < maxSafeSteps) {
                QVariantMap encryptedOptions{
                    {"mode", "xor"},
                    {"keySearchBits", 16},
                    {"maxResults", 200},
                    {"writableOnly", true},
                };
                QVariantMap encrypted = m_controller.scanEncryptedValue(numbers.first(), explicitType.isEmpty() ? QString("Int32") : valueType, encryptedOptions);
                appendSafeStep(
                    "scan_encrypted_value",
                    encrypted.value("success").toBool() ? "success" : "error",
                    QString("Fallback scan chiffré XOR borné après réduction vide."),
                    encrypted);
                result["fallbackAction"] = encrypted;
                result["encryptedMatches"] = encrypted.value("matches");
                const int encryptedCount = encrypted.value("matchesFound", encrypted.value("matchesReturned")).toInt();
                if (encrypted.value("success").toBool() && encryptedCount > 0) {
                    result["workflowStatus"] = "awaiting_encrypted_review";
                    result["candidateCount"] = encryptedCount;
                    actions.append(QVariantMap{{"id", "trace_ui_string"}, {"label", "Comparer avec Trace UI string"}, {"safe", true}});
                    actions.append(QVariantMap{{"id", "review_encrypted_hits"}, {"label", "Inspecter hits chiffrés"}, {"safe", true}});
                    result["message"] = QString("La réduction a vidé les candidats, donc j'ai enchaîné un scan chiffré XOR borné : %1 hit(s). On inspecte ces pistes avant tout write.")
                                            .arg(encryptedCount);
                } else {
                    int uiStringCount = 0;
                    if (executedSafeSteps.size() < maxSafeSteps) {
                        QVariantMap traceOptions{
                            {"ascii", true},
                            {"utf16", true},
                            {"numericBoundary", true},
                            {"writableOnly", true},
                            {"maxResults", 200},
                        };
                        QVariantMap trace = m_controller.scanUiStrings(numbers.first(), traceOptions);
                        appendSafeStep(
                            "scan_ui_strings",
                            trace.value("success").toBool() ? "success" : "error",
                            "Fallback Trace UI string borné après scan chiffré vide.",
                            trace);
                        result["fallbackTraceUiAction"] = trace;
                        result["uiStringMatches"] = trace.value("matches");
                        uiStringCount = trace.value("matchesFound", trace.value("matchesReturned")).toInt();
                    }
                    if (uiStringCount > 0) {
                        result["workflowStatus"] = "awaiting_trace_ui_review";
                        result["candidateCount"] = uiStringCount;
                        actions.append(QVariantMap{{"id", "trace_ui_sources"}, {"label", "Analyser sources UI"}, {"safe", true}});
                        actions.append(QVariantMap{{"id", "try_unknown_changed"}, {"label", "Basculer en Unknown"}, {"safe", true}});
                        result["message"] = QString("La réduction et le scan chiffré sont vides, mais Trace UI string a trouvé %1 string(s). Prochaine étape : analyser les sources UI.")
                                                .arg(uiStringCount);
                    } else {
                        QVariantMap unknownOptions{
                            {"unknownSnapshotMaxMb", 128},
                            {"writableOnly", true},
                            {"executableOnly", false},
                            {"copyOnWriteOnly", false},
                        };
                        QVariantMap unknown = m_controller.captureUnknownSnapshotWithOptions(unknownOptions);
                        appendSafeStep(
                            "unknown_capture",
                            unknown.value("success").toBool() ? "success" : "error",
                            "Capture Unknown bornée après fallbacks vides; attente d'une variation utilisateur.",
                            unknown);
                        result["unknownCaptureAction"] = unknown;
                        result["workflowStatus"] = unknown.value("success").toBool() ? "awaiting_unknown_observation" : "auto_resolve_no_candidate";
                        actions.append(QVariantMap{{"id", "continue_unknown_observation"}, {"label", "Continuer après variation"}, {"safe", true}});
                        actions.append(QVariantMap{{"id", "trace_ui_string"}, {"label", "Relancer Trace UI string"}, {"safe", true}});
                        result["message"] = unknown.value("success").toBool()
                            ? QString("La réduction, le scan chiffré et Trace UI string sont vides. J'ai capturé un snapshot Unknown borné : fais varier la valeur, puis donne-moi la nouvelle observation.")
                            : QString("Les fallbacks safe sont vides et la capture Unknown a échoué : %1").arg(unknown.value("error").toString());
                    }
                }
            } else {
                actions.append(QVariantMap{{"id", "try_unknown_changed"}, {"label", "Basculer en Unknown"}, {"safe", true}});
                actions.append(QVariantMap{{"id", "try_encrypted_scan"}, {"label", "Essayer scan chiffré"}, {"safe", true}});
                result["message"] = "J'ai réduit avec la nouvelle valeur, mais il ne reste aucun candidat. Je propose de passer en Unknown ou en scan chiffré borné.";
            }
        } else if (remaining <= kAutoWriteCandidateLimit) {
            const QString writeValue = !m_controller.m_smartSearchTargetValue.isEmpty()
                ? m_controller.m_smartSearchTargetValue
                : (numbers.size() > 1 ? numbers.at(1) : numbers.first());
            const QVariantList suggestions = suggestedWritesForCandidates(candidates, writeValue, kAutoWriteCandidateLimit);
            actions.append(QVariantMap{{"id", "confirm_test_write"}, {"label", "Tester l'écriture sur les candidats"}, {"safe", false}, {"requiresConfirmation", true}});
            actions.append(QVariantMap{{"id", "confirm_breakpoint_freeze"}, {"label", "Préparer freeze BP confirmé"}, {"safe", false}, {"requiresConfirmation", true}});
            result["requiresConfirmation"] = true;
            result["confirmationReason"] = "Écriture/freeze sur mémoire de processus : je prépare, tu confirmes avant action.";
            result["suggestedWrites"] = suggestions;
            result["suggestedWrite"] = suggestions.isEmpty() ? QVariantMap{} : suggestions.first().toMap();
            result["message"] = QString("J'ai réduit à %1 candidat(s). On est dans la zone intéressante : prochaine étape, test d'écriture confirmé ou freeze BP confirmé.")
                                    .arg(remaining);
        } else {
            actions.append(QVariantMap{{"id", "reduce_again"}, {"label", "Réduire encore avec une nouvelle valeur"}, {"safe", true}});
            actions.append(QVariantMap{{"id", "trace_ui_string"}, {"label", "Chercher via Trace UI string"}, {"safe", true}});
            result["message"] = QString("J'ai réduit à %1 candidat(s). Fais encore varier la valeur et donne-moi la nouvelle valeur pour continuer automatiquement.")
                                    .arg(remaining);
        }

        result["nextActions"] = actions;
        result["executedSafeSteps"] = executedSafeSteps;
        m_controller.appendSmartSearchDebug("auto_resolve_reduce", result);
        return result;
    }

    QVariantMap firstScan = m_controller.startExactScanMultiType(numbers.first(), "Auto");
    appendSafeStep(
        "exact_scan_multi_type",
        firstScan.value("success").toBool() ? "success" : "error",
        QString("Scan initial multi-type pour %1.").arg(numbers.first()),
        firstScan);
    result["firstAction"] = firstScan;
    result["success"] = firstScan.value("success").toBool();
    result["actionStatus"] = result.value("success").toBool() ? "safe_step_executed" : "safe_step_failed";
    const qulonglong candidateCount = firstScan.value("candidateStoreSize", firstScan.value("matchesFound")).toULongLong();
    result["candidateCount"] = candidateCount;

    if (result.value("success").toBool()) {
        m_controller.m_smartSearchActive = candidateCount > 0;
        m_controller.m_smartSearchInitialValue = numbers.first();
        m_controller.m_smartSearchTargetValue = numbers.size() > 1 ? numbers.at(1) : QString();
        m_controller.m_smartSearchValueType = "Auto";
        result["workflowStatus"] = candidateCount > 0 ? "awaiting_value_change" : "auto_resolve_no_candidate";
        if (candidateCount == 0) {
            if (executedSafeSteps.size() < maxSafeSteps) {
                QVariantMap encryptedOptions{
                    {"mode", "xor"},
                    {"keySearchBits", 16},
                    {"maxResults", 200},
                    {"writableOnly", true},
                };
                QVariantMap encrypted = m_controller.scanEncryptedValue(numbers.first(), explicitType.isEmpty() ? QString("Int32") : valueType, encryptedOptions);
                appendSafeStep(
                    "scan_encrypted_value",
                    encrypted.value("success").toBool() ? "success" : "error",
                    "Fallback scan chiffré XOR borné après scan exact vide.",
                    encrypted);
                result["fallbackAction"] = encrypted;
                result["encryptedMatches"] = encrypted.value("matches");
                const int encryptedCount = encrypted.value("matchesFound", encrypted.value("matchesReturned")).toInt();
                if (encrypted.value("success").toBool() && encryptedCount > 0) {
                    result["workflowStatus"] = "awaiting_encrypted_review";
                    result["candidateCount"] = encryptedCount;
                    actions.prepend(QVariantMap{{"id", "review_encrypted_hits"}, {"label", "Inspecter hits chiffrés"}, {"safe", true}});
                    actions.prepend(QVariantMap{{"id", "trace_ui_string"}, {"label", "Comparer Trace UI string"}, {"safe", true}});
                    result["message"] = QString("Le scan exact n'a rien trouvé. J'ai enchaîné automatiquement un scan chiffré XOR borné : %1 hit(s). On valide ces pistes avant toute action risquée.")
                                            .arg(encryptedCount);
                } else {
                    int uiStringCount = 0;
                    if (executedSafeSteps.size() < maxSafeSteps) {
                        QVariantMap traceOptions{
                            {"ascii", true},
                            {"utf16", true},
                            {"numericBoundary", true},
                            {"writableOnly", true},
                            {"maxResults", 200},
                        };
                        QVariantMap trace = m_controller.scanUiStrings(numbers.first(), traceOptions);
                        appendSafeStep(
                            "scan_ui_strings",
                            trace.value("success").toBool() ? "success" : "error",
                            "Fallback Trace UI string borné après scan chiffré vide.",
                            trace);
                        result["fallbackTraceUiAction"] = trace;
                        result["uiStringMatches"] = trace.value("matches");
                        uiStringCount = trace.value("matchesFound", trace.value("matchesReturned")).toInt();
                    }
                    if (uiStringCount > 0) {
                        result["workflowStatus"] = "awaiting_trace_ui_review";
                        result["candidateCount"] = uiStringCount;
                        actions.prepend(QVariantMap{{"id", "trace_ui_sources"}, {"label", "Analyser sources UI"}, {"safe", true}});
                        result["message"] = QString("Le scan exact et le scan chiffré sont vides, mais Trace UI string a trouvé %1 string(s). Prochaine étape : analyser les sources UI.")
                                            .arg(uiStringCount);
                    } else {
                        QVariantMap unknownOptions{
                            {"unknownSnapshotMaxMb", 128},
                            {"writableOnly", true},
                            {"executableOnly", false},
                            {"copyOnWriteOnly", false},
                        };
                        QVariantMap unknown = m_controller.captureUnknownSnapshotWithOptions(unknownOptions);
                        appendSafeStep(
                            "unknown_capture",
                            unknown.value("success").toBool() ? "success" : "error",
                            "Capture Unknown bornée après fallbacks vides; attente d'une variation utilisateur.",
                            unknown);
                        result["unknownCaptureAction"] = unknown;
                        result["workflowStatus"] = unknown.value("success").toBool() ? "awaiting_unknown_observation" : "auto_resolve_no_candidate";
                        actions.prepend(QVariantMap{{"id", "continue_unknown_observation"}, {"label", "Continuer après variation"}, {"safe", true}});
                        result["message"] = unknown.value("success").toBool()
                            ? QString("Le scan exact, le scan chiffré et Trace UI string sont vides. J'ai capturé un snapshot Unknown borné : fais varier la valeur, puis donne-moi la nouvelle observation.")
                            : QString("Les fallbacks safe sont vides et la capture Unknown a échoué : %1").arg(unknown.value("error").toString());
                    }
                }
            } else {
                actions.prepend(QVariantMap{{"id", "try_encrypted_scan"}, {"label", "Essayer scan chiffré"}, {"safe", true}});
                result["message"] = "J'ai lancé le scan initial, mais il n'a rien trouvé. Prochaine piste : Unknown ou scan chiffré.";
            }
        } else {
            const QString writeValue = numbers.size() > 1 ? numbers.at(1) : numbers.first();
            const QVariantList suggestions = suggestedWritesForCandidates(candidates, writeValue, kAutoWriteCandidateLimit);
            if (candidateCount <= kAutoWriteCandidateLimit) {
                result["requiresConfirmation"] = true;
                result["confirmationReason"] = "Petit nombre de candidats : confirme avant tout test d'écriture ou freeze.";
                result["suggestedWrites"] = suggestions;
                result["suggestedWrite"] = suggestions.isEmpty() ? QVariantMap{} : suggestions.first().toMap();
                actions.prepend(QVariantMap{{"id", "confirm_test_write"}, {"label", "Tester l'écriture sur les candidats"}, {"safe", false}, {"requiresConfirmation", true}});
            }
            result["message"] = QString("J'ai lancé le plan auto et trouvé %1 candidat(s). Fais varier la valeur dans le jeu, puis donne-moi la nouvelle valeur pour réduire.")
                                    .arg(candidateCount);
        }
    } else {
        result["workflowStatus"] = "action_failed";
        result["error"] = firstScan.value("error").toString();
        result["message"] = QString("Le plan est prêt, mais le premier scan a échoué : %1").arg(result.value("error").toString());
    }

    result["nextActions"] = actions;
    result["executedSafeSteps"] = executedSafeSteps;
    rememberAutoResolverProgress(result.value("workflowStatus").toString(), candidateCount);
    result["contextReport"] = getAutoResolveReport(80);
    m_controller.appendSmartSearchDebug("auto_resolve_start", result);
    return result;
}

// Un nouveau lot d'adresses (ecriture auto reussie, nouveau scan, contexte
// efface, ou confirmation explicite de l'utilisateur) rend obsolete
// l'echelle de secours du lot precedent : remet a zero le palier ET toute
// relance en attente, pour que le prochain message de l'utilisateur ne soit
// pas mal interprete comme la reponse a une question qui ne concerne plus ce
// lot. Factorise ici car deuplique a plus de 10 sites avant extraction —
// chacun devait se souvenir des deux lignes independamment.
void SmartSearchManager::resetFailureEscalationState() {
    m_controller.m_failureEscalationLevel = 0;
    m_controller.m_pendingRecoveryAction.clear();
}

// Echelle de secours quand l'utilisateur signale qu'un lot d'adresses ecrit
// automatiquement n'a pas fonctionne. Plutot que de reboucler indefiniment
// sur "fais varier la valeur, redonne-la moi" (le meme scan numerique qui a
// deja echoue), chaque nouveau signalement sur le meme lot fait avancer d'un
// palier vers une methode differente. Le message "valeur probablement
// protegee/calculee" n'arrive qu'en tout dernier, une fois l'arsenal epuise.
QVariantMap SmartSearchManager::buildFailureEscalationRecovery(const QString& query, const QStringList& numbers) {
    QVariantMap recovery;
    recovery["success"] = true;
    recovery["query"] = query;
    recovery["aiReady"] = m_controller.m_ai.isReady();
    recovery["status"] = "bad_targets_reported";
    recovery["actionStatus"] = "needs_recovery_choice";
    recovery["workflowStatus"] = "auto_write_problem";
    recovery["targetValue"] = m_controller.m_smartSearchTargetValue;
    recovery["activeTargetCount"] = m_controller.m_lastAutoWriteTargets.size();
    recovery["candidateStoreSize"] = static_cast<qulonglong>(m_controller.scanState().candidates().size());
    recovery["failureEscalationLevel"] = m_controller.m_failureEscalationLevel;

    // Le message de signalement d'echec contient parfois la valeur
    // actuellement affichee (ex: "ca n'a pas marche, j'ai maintenant 180xp") :
    // c'est cette valeur-la qu'il faut tracer en priorite. A defaut, on
    // retombe sur la derniere valeur RAPPORTEE comme affichee
    // (m_controller.m_smartSearchLastObservedValue) — jamais sur m_controller.m_smartSearchTargetValue,
    // qui est le but jamais atteint et n'a par definition aucune chance
    // d'exister litteralement en memoire/texte a tracer.
    const QString lastValue = !numbers.isEmpty()
        ? numbers.first()
        : (!m_controller.m_smartSearchLastObservedValue.isEmpty() ? m_controller.m_smartSearchLastObservedValue : m_controller.m_smartSearchInitialValue);

    QVariantList invalidatedAddresses;
    for (const auto& target : m_controller.m_lastAutoWriteTargets) {
        invalidatedAddresses.append(QString::number(target.address, 16));
    }
    recovery["invalidatedAddresses"] = invalidatedAddresses;

    QVariantList actions;
    switch (m_controller.m_failureEscalationLevel) {
    case 1:
        m_controller.m_pendingRecoveryAction = "trace_ui_string";
        recovery["message"] = QString(
            "D'accord, ces adresses ne sont pas les bonnes. On change de méthode : au lieu de continuer à deviner par "
            "essais numériques, je vais tracer le texte affiché à l'écran (\"%1\") pour remonter à la vraie source — "
            "l'adresse trouvée était peut-être une simple copie d'affichage. Donne-moi la valeur actuellement affichée "
            "dans le jeu (tu peux juste me répondre par la valeur, pas besoin de cliquer le bouton).")
            .arg(lastValue);
        actions.append(QVariantMap{
            {"id", "trace_ui_string"}, {"label", "Tracer le texte affiché"},
            {"value", lastValue},
            {"reason", "Étape 1/4 : chercher la vraie source derrière la valeur affichée."}});
        break;
    case 2: {
        m_controller.m_pendingRecoveryAction.clear();
        QString address;
        QString type = "Int32";
        if (!m_controller.m_lastAutoWriteTargets.isEmpty()) {
            address = QString::number(m_controller.m_lastAutoWriteTargets.first().address, 16);
            type = killcore::valueTypeToString(m_controller.m_lastAutoWriteTargets.first().type);
        }
        recovery["message"] = "Toujours pas la bonne piste. Étape suivante : je capture directement l'instruction qui "
                               "écrit sur la dernière adresse pendant que tu fais varier la valeur dans le jeu — ça dit "
                               "si cette adresse est vraiment utilisée par le jeu ou non.";
        actions.append(QVariantMap{
            {"id", "find_what_writes_targets"}, {"label", "Capturer qui écrit dessus"},
            {"address", address}, {"type", type},
            {"reason", "Étape 2/4 : pose un point d'arrêt matériel et capture les prochaines écritures."}});
        break;
    }
    case 3:
        m_controller.m_pendingRecoveryAction.clear();
        // Contrairement a trace_ui_string, ce pending n'est pas interprete par
        // le classifieur C++ : c'est un signal pour le frontend (doSearch),
        // qui route directement vers runAutoEncryptedScan si la reponse
        // suivante est en texte libre plutot qu'un clic de bouton.
        recovery["pendingRecoveryAction"] = "encrypted_scan";
        recovery["message"] = "On passe aux pistes avancées. Je commence par un scan chiffré (XOR/Add/Sub/NOT) : "
                               "donne-moi la valeur actuellement affichée dans le jeu (pas besoin de cliquer le bouton). "
                               "Si ça ne donne rien non plus, il restera la piste du pointeur stable, pour le cas où "
                               "l'adresse bouge d'une partie à l'autre.";
        actions.append(QVariantMap{
            {"id", "try_encrypted_scan"}, {"label", "Scan chiffré (XOR)"}, {"value", lastValue},
            {"reason", "Étape 3/4 : la valeur est peut-être stockée sous une forme chiffrée simple."}});
        actions.append(QVariantMap{
            {"id", "open_pointer_scan"}, {"label", "Chercher un pointeur stable"},
            {"reason", "Étape 3/4 : l'adresse change peut-être à chaque partie, un pointeur la retrouve automatiquement."}});
        break;
    default: {
        m_controller.m_pendingRecoveryAction.clear();
        const bool hasRemoteConnection = m_controller.m_handle.isValid() && processHasActiveRemoteConnections(m_controller.m_pid);
        recovery["hasActiveRemoteConnection"] = hasRemoteConnection;
        recovery["message"] = hasRemoteConnection
            ? QString(
                  "On a maintenant essayé la recherche directe, le traçage du texte affiché, la capture des écritures "
                  "et les pistes avancées (pointeur/chiffré). Il est probable que cette valeur soit protégée, calculée "
                  "par le jeu à la volée, ou synchronisée avec un serveur — d'ailleurs %1 a actuellement une connexion "
                  "réseau active vers un serveur distant, ce qui renforce cette hypothèse (indice, pas une preuve). Si "
                  "c'est bien ça, la modifier localement ne suffira probablement pas. Tu peux repartir sur une autre "
                  "valeur, ou continuer manuellement dans l'onglet Expert.")
                  .arg(m_controller.processName())
            : "On a maintenant essayé la recherche directe, le traçage du texte affiché, la capture des écritures et "
              "les pistes avancées (pointeur/chiffré). Il est probable que cette valeur soit protégée, calculée par le "
              "jeu à la volée, ou synchronisée avec un serveur — ce qui la rend difficile à modifier directement avec "
              "KillEngine. Tu peux repartir sur une autre valeur, ou continuer manuellement dans l'onglet Expert.";
        actions.append(QVariantMap{{"id", "open_expert"}, {"label", "Continuer dans Expert"}});
        break;
    }
    }

    actions.append(QVariantMap{{"id", "rollback_batch"}, {"label", "Rollback dernier lot"}});
    actions.append(QVariantMap{{"id", "clear_targets"}, {"label", "Oublier ces adresses"}});
    actions.append(QVariantMap{{"id", "new_search"}, {"label", "Nouvelle recherche"}});
    if (!m_controller.scanState().candidates().isEmpty()) {
        actions.append(QVariantMap{{"id", "continue_candidates"}, {"label", "Continuer avec les autres candidats"}});
    }
    recovery["recoveryActions"] = actions;
    return recovery;
}

QVariantMap SmartSearchManager::startSmartSearch(const QString& query) {
    KE_LOG_INFO() << "startSmartSearch(\"" << query.toStdString() << "\")";
    if (m_controller.m_smartSearchBusy) {
        // Un appel precedent est encore dans une section bloquante qui pompe
        // processEvents() (appel IA ou analyse de sources Trace UI string,
        // cf. commentaire de m_controller.m_smartSearchBusy dans le header). Sans ce
        // garde-fou, ce second appel s'executerait sur la meme pile et
        // muterait m_candidates / m_controller.m_lastAutoWriteTargets pendant que le
        // premier appel les lit encore. On rejette proprement plutot que de
        // risquer un etat incoherent.
        QVariantMap busy;
        busy["success"] = false;
        busy["error"] = "Une requête est déjà en cours, réessaie dans un instant.";
        busy["status"] = "ai_busy";
        return busy;
    }

    // Backend IA externe (T4, docs/EXTERNAL_AI_BACKEND_ROADMAP.md) : bascule
    // manuelle globale (decision roadmap #2, pas de routage automatique par
    // tache) -- quand le backend actif est "claude", on court-circuite TOUTE
    // l'heuristique locale ci-dessous (intents/pre-intents specifiques au
    // modele embarque Qwen) et on delegue entierement au backend Claude, qui
    // fait son propre raisonnement multi-tours cote API. m_smartSearchBusy
    // protege ce chemin comme le chemin local (ClaudeChatManager::sendMessage
    // pompe processEvents() en attendant l'API/les confirmations, meme risque
    // de reentrance qu'un appel IA local).
    if (m_controller.getActiveAiBackend() == "claude") {
        m_controller.m_smartSearchBusy = true;
        const QVariantMap claudeResult = m_controller.m_claudeChatManager->sendMessage(query);
        m_controller.m_smartSearchBusy = false;

        QVariantMap result;
        result["query"] = query;
        result["aiReady"] = true;
        result["aiBackend"] = "claude";
        if (claudeResult.value("success").toBool()) {
            result["actionStatus"] = "executed";
            result["message"] = claudeResult.value("message").toString();
        } else {
            result["actionStatus"] = "failed";
            result["error"] = claudeResult.value("error").toString();
            result["message"] = claudeResult.value("error").toString();
        }
        result["toolCallsExecuted"] = claudeResult.value("toolCallsExecuted");
        result["requestCount"] = claudeResult.value("requestCount");
        return result;
    }

    const QStringList numbers = numbersFromText(query);
    const QStringList chatAddresses = hexAddressesFromText(query);
    const QString explicitValueType = explicitValueTypeFromText(query);
    const QString defaultValueType = explicitValueType.isEmpty() ? QString("Int32") : explicitValueType;
    const bool smartSearchTrainerQuery = killai::wantsTrainerQuery(query);
    // PHASE 130 : meme piege que smartSearchTrainerQuery (PHASE 129) -- une
    // question contenant une adresse 0x... ("est-ce que 0x1234 est un champ
    // affiche...") etait interceptee trop tot par les pre-intents memoire
    // (ActivateMemoryTargets) avant d'atteindre le fast-path analyze_field_stability
    // (ai/ai_engine.cpp::matchFieldStabilityTool, meme liste de mots-cles).
    const bool smartSearchFieldStabilityQuery = killai::wantsFieldStabilityQuery(query) && !chatAddresses.isEmpty();
    // PHASE 140 : meme piege, pour les 5 outils restants d'analyze_field_stability
    // qui prennent une adresse (get_auto_report/analyze_ui_sources n'en ont pas
    // besoin en pratique, pas concernes). Verifications volontairement plus
    // grossieres que leurs matchXxxTool respectifs (ai/ai_engine.cpp) -- servent
    // seulement a eviter que le pre-intent memoire les intercepte avant que
    // processQuery() ait une chance de les router correctement.
    const bool smartSearchAobOrPatchWorkflowQuery = killai::wantsAobOrPatchWorkflowQuery(query);
    const bool smartSearchFindWhatWritesOrTestFieldsQuery = killai::wantsFindWhatWritesOrTestFieldsQuery(query);
    const bool smartSearchModuleScanQuery = !moduleNameFromText(query).isEmpty()
        && (query.contains("dll", Qt::CaseInsensitive) || query.contains("module", Qt::CaseInsensitive)
            || query.contains("utilise", Qt::CaseInsensitive) || query.contains("use ", Qt::CaseInsensitive)
            || query.contains(".exe", Qt::CaseInsensitive));
    const bool smartSearchExplicitTraceUiStringQuery = wantsExplicitTraceUiString(query);
    const bool smartSearchExplicitChangedPagesQuery = wantsExplicitChangedPages(query);
    const bool smartSearchModuleExplorationQuery = wantsModuleExploration(query);
    // PHASE 140 : consolide en un seul flag plutot que de continuer a "&&" une
    // liste croissante sur les 8 points de bypass ci-dessous -- prochain outil
    // a router : ajouter sa condition ici, pas un neuvieme "&& !smartSearchXQuery"
    // sur chaque ligne.
    const bool smartSearchBypassesMemoryPreIntent = smartSearchTrainerQuery
        || smartSearchFieldStabilityQuery
        || smartSearchAobOrPatchWorkflowQuery
        || smartSearchFindWhatWritesOrTestFieldsQuery
        || smartSearchModuleScanQuery
        || smartSearchExplicitTraceUiStringQuery
        || smartSearchExplicitChangedPagesQuery
        || smartSearchModuleExplorationQuery;
    // PHASE 148 : meme liste de mots-cles que matchUiSourcesTool
    // (ai/ai_engine.cpp), duplication grossiere volontaire -- meme convention
    // que les flags smartSearchXxxQuery ci-dessus. Sert a un guard DIFFERENT
    // (pas smartSearchBypassesMemoryPreIntent) : contourne specifiquement le
    // bloc AnswerTraceUiFilterPrompt de startSmartSearch, pas le pre-intent
    // ActivateMemoryTargets -- ces deux outils n'ont pas d'adresse a router
    // via le meme mecanisme que les 5 outils ci-dessus.
    const bool smartSearchExplicitUiSourcesQuery = killai::wantsUiSourcesQuery(query);
    auto& candidates = m_controller.scanState().candidates();
    const QString explicitRefinementValue = observedRefinementValueFromText(query, numbers);
    const bool smartSearchExplicitRefinementQuery = m_controller.m_smartSearchActive
        && !candidates.isEmpty()
        && wantsCandidateRefinement(query)
        && !explicitRefinementValue.isEmpty();
    const SmartSearchIntent intent = classifySmartSearchIntent(
        query,
        numbers,
        chatAddresses,
        !m_controller.m_chatMemoryTargets.isEmpty(),
        !m_controller.m_lastAutoWriteTargets.isEmpty(),
        !candidates.isEmpty(),
        m_controller.m_smartSearchActive,
        m_controller.m_pendingRecoveryAction == "trace_ui_string",
        m_controller.m_pendingRecoveryAction == "trace_ui_filter",
        m_controller.m_pendingRecoveryAction == "write_target_value");
    if (intent.kind != SmartSearchIntentKind::AnswerTraceUiStringPrompt
        && intent.kind != SmartSearchIntentKind::AnswerTraceUiFilterPrompt
        && intent.kind != SmartSearchIntentKind::AnswerWriteTargetPrompt) {
        // Ce message ne repond pas a la relance en attente (l'utilisateur a
        // peut-etre clique le bouton correspondant a la place, ou envoye tout
        // autre chose) : on ne laisse pas l'etat "en attente" fausser un futur
        // message sans rapport.
        m_controller.m_pendingRecoveryAction.clear();
    }
    m_controller.appendSmartSearchDebug("smart_search_query", {
        {"query", query},
        {"numbers", numbers},
        {"addresses", chatAddresses},
        {"intent", smartSearchIntentKindToString(intent.kind)},
        {"intentRationale", intent.rationale},
        {"smartSearchActive", m_controller.m_smartSearchActive},
        {"candidateCount", static_cast<qulonglong>(candidates.size())},
        {"initialValue", m_controller.m_smartSearchInitialValue},
        {"targetValue", m_controller.m_smartSearchTargetValue},
        {"valueType", m_controller.m_smartSearchValueType},
        {"explicitValueType", explicitValueType},
    });

    auto stampIntent = [&](QVariantMap* payload) {
        if (!payload) return;
        (*payload)["intent"] = smartSearchIntentKindToString(intent.kind);
        (*payload)["intentRationale"] = intent.rationale;
    };

    if (smartSearchExplicitChangedPagesQuery) {
        m_controller.m_pendingRecoveryAction.clear();
        m_controller.m_pendingUiStringCandidates.clear();

        QVariantMap changed;
        QVariantMap args;
        QVariantMap actionResult;
        const QStringList observed = lastTwoObservedValues(numbers);
        changed["query"] = query;
        changed["aiReady"] = m_controller.m_ai.isReady();
        changed["status"] = "tool_call";
        changed["state"] = m_controller.m_smartSearchActive ? QString("Refining") : QString("Idle");
        changed["rationale"] = observed.size() >= 2
            ? QString("La phrase demande explicitement Changed Pages avec deux valeurs : je compare les pages modifiées sur les deux dernières valeurs observées.")
            : QString("La phrase demande explicitement Changed Pages : je capture un snapshot lecture seule avant la prochaine variation.");
        changed["error"] = "";
        if (observed.size() >= 2) {
            args["previousValue"] = observed.at(0);
            args["currentValue"] = observed.at(1);
            changed["tool"] = "finish_changed_pages_diff";
            m_controller.m_smartSearchInitialValue = observed.at(0);
            m_controller.m_smartSearchLastObservedValue = observed.at(1);
            QVariantMap diffOptions;
            actionResult = m_controller.finishChangedPagesDiff(observed.at(0), observed.at(1), diffOptions);
            changed["workflowStatus"] = actionResult.value("success").toBool()
                ? (actionResult.value("hitCount", actionResult.value("hits").toList().size()).toInt() > 0 ? "diff_hits_found" : "no_candidate")
                : "action_failed";
            changed["message"] = actionResult.value("success").toBool()
                ? QString("Mode Inspecteur : comparaison Changed Pages %1 → %2 effectuée. %3 piste(s) trouvée(s) dans les pages réellement modifiées.")
                      .arg(observed.at(0), observed.at(1))
                      .arg(actionResult.value("hitCount", actionResult.value("hits").toList().size()).toInt())
                : QString("Changed Pages : comparaison impossible pour l'instant (%1). Lance d'abord Changed Pages avant la prochaine variation, puis redonne l'ancienne et la nouvelle valeur.")
                      .arg(actionResult.value("error").toString());
        } else {
            if (!numbers.isEmpty()) {
                m_controller.m_smartSearchInitialValue = numbers.last();
                m_controller.m_smartSearchLastObservedValue = numbers.last();
                changed["initialValue"] = numbers.last();
            }
            changed["tool"] = "start_changed_pages_diff";
            QVariantMap diffOptions;
            diffOptions["maxBytesMb"] = 64;
            diffOptions["blockSize"] = 64 * 1024;
            diffOptions["privateOnly"] = true;
            diffOptions["writableOnly"] = true;
            actionResult = m_controller.startChangedPagesDiff(diffOptions);
            changed["workflowStatus"] = actionResult.value("success").toBool() ? "awaiting_observed_variation" : "action_failed";
            changed["message"] = actionResult.value("success").toBool()
                ? QString("Mode Inspecteur : snapshot Changed Pages capturé (%1 blocs, 64 Mo max). Fais varier l'XP, puis donne-moi l'ancienne et la nouvelle valeur.")
                      .arg(actionResult.value("blocksCaptured").toInt())
                : QString("Changed Pages : snapshot impossible (%1).").arg(actionResult.value("error").toString());
        }
        changed["args"] = args;
        changed["actionResult"] = actionResult;
        changed["actionStatus"] = actionResult.value("success").toBool() ? "executed" : "failed";
        if (!actionResult.value("hits").isNull()) {
            changed["changedPageHits"] = actionResult.value("hits");
        }
        stampIntent(&changed);
        m_controller.appendSmartSearchDebug("smart_search_explicit_changed_pages", changed);
        return changed;
    }

    if (killai::looksLikePureSocialQuery(query, numbers, chatAddresses)) {
        QVariantMap social;
        social["success"] = true;
        social["query"] = query;
        social["aiReady"] = m_controller.m_ai.isReady();
        social["status"] = "needs_clarification";
        social["actionStatus"] = "not_executed";
        social["workflowStatus"] = "idle";
        social["message"] =
            "Salut ! Dis-moi ce que tu veux chercher ou comprendre : une valeur affichée, une adresse, "
            "un freeze, un trainer, un script Lua, ou une investigation plus guidée.";
        social["debugFile"] = m_controller.smartSearchDebugFilePath();
        stampIntent(&social);
        m_controller.appendSmartSearchDebug("smart_search_social_guard", social);
        return social;
    }

    const bool shouldClearSearchContext = intent.resetContext
        && (intent.kind == SmartSearchIntentKind::ResetContext
            || intent.kind == SmartSearchIntentKind::ExactScan
            || intent.kind == SmartSearchIntentKind::GuidedScan);
    if (shouldClearSearchContext) {
        const bool hadCandidates = !candidates.isEmpty();
        const int chatCount = m_controller.m_chatMemoryTargets.size();
        const int profileCount = m_controller.m_activeProfileTargets.size();
        m_controller.m_smartSearchActive = false;
        m_controller.m_smartSearchInitialValue.clear();
        m_controller.m_smartSearchTargetValue.clear();
        m_controller.scanState().clearCandidates();
        m_controller.clearCandidateUndo();
        m_controller.clearCandidateValueHistory();
        m_controller.m_lastAutoWriteTargets.clear();
        m_controller.m_autoWriteValueHistory.clear();
        m_controller.m_chatMemoryTargets.clear();
        resetFailureEscalationState();
        m_controller.m_pendingUiStringCandidates.clear();
        m_controller.m_smartSearchLastObservedValue.clear();
        m_controller.m_activeProfileTargets.clear();
        m_controller.appendSmartSearchDebug("smart_search_reset", {
            {"query", query},
            {"reason", "new search request"},
            {"hadCandidates", hadCandidates},
            {"chatTargetsCleared", chatCount},
            {"profileTargetsCleared", profileCount},
        });
    }

    if (intent.kind == SmartSearchIntentKind::ClearActiveTargets) {
        const int chatCount = m_controller.m_chatMemoryTargets.size();
        const int profileCount = m_controller.m_activeProfileTargets.size();
        const int lastCount = m_controller.m_lastAutoWriteTargets.size();
        m_controller.m_chatMemoryTargets.clear();
        m_controller.m_activeProfileTargets.clear();
        m_controller.m_lastAutoWriteTargets.clear();
        m_controller.m_autoWriteValueHistory.clear();
        m_controller.m_lastBatchStartIndex = -1;
        m_controller.m_lastBatchEndIndex = -1;
        resetFailureEscalationState();
        m_controller.m_pendingUiStringCandidates.clear();
        m_controller.m_smartSearchLastObservedValue.clear();

        QVariantMap cleared;
        cleared["success"] = true;
        cleared["query"] = query;
        cleared["aiReady"] = m_controller.m_ai.isReady();
        cleared["status"] = "active_targets_cleared";
        cleared["actionStatus"] = "executed";
        cleared["workflowStatus"] = "idle";
        cleared["chatTargetsCleared"] = chatCount;
        cleared["profileTargetsCleared"] = profileCount;
        cleared["lastAutoWriteTargetsCleared"] = lastCount;
        cleared["message"] = QString("C'est fait, j'ai oublié les adresses et profils actifs de la conversation.");
        stampIntent(&cleared);
        m_controller.appendSmartSearchDebug("smart_search_clear_active_targets", cleared);
        return cleared;
    }

    if (intent.kind == SmartSearchIntentKind::ResetContext) {
        QVariantMap reset;
        reset["success"] = true;
        reset["query"] = query;
        reset["aiReady"] = m_controller.m_ai.isReady();
        reset["status"] = "context_reset";
        reset["actionStatus"] = "executed";
        reset["workflowStatus"] = "idle";
        reset["message"] = "D'accord, je repars sur une recherche propre. Donne-moi la nouvelle valeur à chercher.";
        stampIntent(&reset);
        m_controller.appendSmartSearchDebug("smart_search_context_reset", reset);
        return reset;
    }

    if (intent.kind == SmartSearchIntentKind::ReportBadTargets) {
        ++m_controller.m_failureEscalationLevel;
        // Les adresses invalidees ne doivent plus jamais etre la cible par
        // defaut d'un futur nombre isole (ex: l'utilisateur tape juste "240"
        // pour repondre a une toute autre relance) : sans ce clear, la regle
        // generique "des adresses sont actives + un nombre => on ecrit dessus"
        // rattrape silencieusement n'importe quel nombre ulterieur et reecrit
        // sur des adresses deja signalees comme mauvaises. m_controller.m_lastAutoWriteTargets
        // (utilise pour Find What Writes et l'historique) n'est PAS efface ici.
        m_controller.m_chatMemoryTargets.clear();
        if (!numbers.isEmpty()) {
            m_controller.m_smartSearchLastObservedValue = numbers.first();
        }
        QVariantMap recovery = buildFailureEscalationRecovery(query, numbers);
        stampIntent(&recovery);
        m_controller.appendSmartSearchDebug("smart_search_bad_targets_reported", recovery);
        return recovery;
    }

    if (intent.kind == SmartSearchIntentKind::ReportGoodTargets) {
        // Symetrique de ReportBadTargets : l'utilisateur confirme que le
        // dernier lot ecrit fonctionne vraiment. On sauvegarde chaque adresse
        // comme cible de Profil (locator module+offset si possible, pour
        // survivre a un redemarrage du jeu) et on la marque "confirmee" dans
        // l'historique anti-bruit pour qu'elle ne soit plus jamais retrogradee,
        // meme si elle revient plus tard avec une autre cible (farming normal).
        // Si m_controller.m_chatMemoryTargets est vide PARCE QUE ce lot vient d'etre
        // signale mauvais (ReportBadTargets vide m_controller.m_chatMemoryTargets mais
        // garde volontairement m_controller.m_lastAutoWriteTargets pour Find What Writes,
        // cf. commentaire plus haut), ne PAS retomber dessus ici : une
        // confirmation qui suit immediatement un signalement d'echec sur le
        // meme lot est presque toujours sans rapport (ou contradictoire),
        // et la sauvegarde Profil + l'immunisation anti-bruit sont quasi
        // irreversibles pour se tromper.
        const bool batchJustReportedBad = m_controller.m_chatMemoryTargets.isEmpty() && m_controller.m_failureEscalationLevel > 0;
        resetFailureEscalationState();

        const QList<AutoWriteTarget> emptyTargets;
        const auto& confirmedTargets = !m_controller.m_chatMemoryTargets.isEmpty()
            ? m_controller.m_chatMemoryTargets
            : (batchJustReportedBad ? emptyTargets : m_controller.m_lastAutoWriteTargets);
        QVariantMap recovery;
        recovery["success"] = true;
        recovery["query"] = query;
        recovery["aiReady"] = m_controller.m_ai.isReady();
        recovery["targetValue"] = m_controller.m_smartSearchTargetValue;

        if (confirmedTargets.isEmpty()) {
            recovery["workflowStatus"] = "idle";
            recovery["message"] = "Content que ça marche ! Je n'ai pas d'adresse active à sauvegarder pour le moment.";
            stampIntent(&recovery);
            m_controller.appendSmartSearchDebug("smart_search_good_targets_reported", recovery);
            return recovery;
        }

        const QString gameKey = autoResolverGameKey(m_controller.processName());
        const QString baseName = (!m_controller.m_smartSearchInitialValue.isEmpty() && !m_controller.m_smartSearchTargetValue.isEmpty())
            ? QString("Cible confirmée %1→%2").arg(m_controller.m_smartSearchInitialValue, m_controller.m_smartSearchTargetValue)
            : QString("Cible confirmée %1").arg(QDateTime::currentDateTime().toString("dd/MM HH:mm"));
        const QString description = QString(
            "Confirmée par l'utilisateur le %1 (recherche %2 → %3).")
            .arg(QDateTime::currentDateTime().toString("dd/MM/yyyy HH:mm"))
            .arg(m_controller.m_smartSearchInitialValue.isEmpty() ? QString("?") : m_controller.m_smartSearchInitialValue)
            .arg(m_controller.m_smartSearchTargetValue.isEmpty() ? QString("?") : m_controller.m_smartSearchTargetValue);

        QVariantList savedTargets;
        QVariantList historyEntries;
        bool anyModuleOffset = false;
        for (int i = 0; i < confirmedTargets.size(); ++i) {
            const auto& target = confirmedTargets.at(i);
            const QString addressHex = QString::number(target.address, 16);
            const QString typeStr = killcore::valueTypeToString(target.type);
            const QString targetName = confirmedTargets.size() > 1
                ? QString("%1 #%2").arg(baseName).arg(i + 1)
                : baseName;
            const QVariantMap saveResult = m_controller.saveProfileTarget(gameKey, targetName, addressHex, typeStr, description);
            if (saveResult.value("success").toBool()) {
                savedTargets.append(QVariantMap{
                    {"targetName", targetName},
                    {"address", addressHex},
                    {"type", typeStr},
                    {"locatorKind", saveResult.value("locatorKind")},
                });
                anyModuleOffset = anyModuleOffset || saveResult.value("locatorKind").toString() == "module_offset";
            }
            historyEntries.append(QVariantMap{
                {"address", addressHex},
                {"type", typeStr},
                {"initialValue", m_controller.m_smartSearchInitialValue},
                {"targetValue", m_controller.m_smartSearchTargetValue},
                {"confirmed", true},
                {"timestamp", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
            });
        }
        appendCandidateHistory(gameKey, historyEntries);

        recovery["workflowStatus"] = "idle";
        recovery["savedProfileTargets"] = savedTargets;
        recovery["profileName"] = gameKey;
        if (savedTargets.isEmpty()) {
            recovery["message"] = "Content que ça marche ! La sauvegarde en profil a échoué, mais l'adresse reste active pour cette session.";
        } else {
            recovery["message"] = anyModuleOffset
                ? QString(
                      "Nickel ! J'ai sauvegardé %1 dans le profil « %2 » (onglet Profils) — elle survivra à un "
                      "redémarrage du jeu, tu pourras la réactiver direct la prochaine fois sans tout rescanner.")
                      .arg(savedTargets.size() == 1 ? "cette adresse" : QString("ces %1 adresses").arg(savedTargets.size()))
                      .arg(gameKey)
                : QString(
                      "Nickel ! J'ai sauvegardé %1 dans le profil « %2 » (onglet Profils). Attention : elle est en "
                      "mémoire non associée à un module, donc l'adresse ne survivra probablement pas à un "
                      "redémarrage du jeu — il faudra la reconfirmer la prochaine fois.")
                      .arg(savedTargets.size() == 1 ? "cette adresse" : QString("ces %1 adresses").arg(savedTargets.size()))
                      .arg(gameKey);
        }
        stampIntent(&recovery);
        m_controller.appendSmartSearchDebug("smart_search_good_targets_reported", recovery);
        return recovery;
    }

    if (intent.kind == SmartSearchIntentKind::AnswerTraceUiStringPrompt) {
        const QString traceValue = !numbers.isEmpty() ? numbers.first() : query.trimmed();
        m_controller.m_smartSearchLastObservedValue = traceValue;

        QVariantMap traceOptions;
        traceOptions["ascii"] = true;
        traceOptions["utf16"] = true;
        traceOptions["writableOnly"] = true;
        const QVariantMap scanResult = m_controller.scanUiStrings(traceValue, traceOptions);

        QVariantMap recovery;
        recovery["success"] = true;
        recovery["query"] = query;
        recovery["aiReady"] = m_controller.m_ai.isReady();
        recovery["targetValue"] = m_controller.m_smartSearchTargetValue;
        recovery["actionStatus"] = scanResult.value("success").toBool() ? "executed" : "failed";
        if (scanResult.value("success").toBool()) {
            const auto stringsFound = scanResult.value("matchesFound").toULongLong();
            if (stringsFound > 0) {
                // On enchaine sur l'etape 2 (Filtrer + Analyser sources) au
                // prochain message : garder les candidats trouves et faire
                // suivre la relance en attente plutot que renvoyer
                // l'utilisateur cliquer manuellement dans Expert.
                m_controller.m_pendingRecoveryAction = "trace_ui_filter";
                m_controller.m_pendingUiStringCandidates = scanResult.value("candidates").toList();
                recovery["workflowStatus"] = "trace_ui_string_found";
                recovery["message"] = QString(
                    "Trace UI string : %1 occurrence(s) du texte \"%2\" trouvées en mémoire. Fais varier la valeur dans "
                    "le jeu, puis donne-moi la nouvelle valeur affichée — je filtre les bonnes pistes et je cherche la "
                    "source numérique derrière, automatiquement.")
                    .arg(stringsFound)
                    .arg(traceValue);
            } else {
                m_controller.m_pendingRecoveryAction.clear();
                recovery["workflowStatus"] = "no_candidate";
                recovery["message"] = QString(
                    "Trace UI string : le texte \"%1\" n'a pas été trouvé en mémoire. Vérifie la valeur affichée "
                    "exacte, ou passe en Unknown.")
                    .arg(traceValue);
                QVariantList recoveryActions;
                recoveryActions.append(QVariantMap{{"id", "try_encrypted_scan"}, {"label", "Scan chiffré (XOR)"}, {"value", traceValue}});
                recoveryActions.append(QVariantMap{{"id", "try_unknown_changed"}, {"label", "Unknown (valeur inconnue)"}});
                recoveryActions.append(QVariantMap{{"id", "new_search"}, {"label", "Nouvelle recherche"}});
                recovery["recoveryActions"] = recoveryActions;
            }
            if (!scanResult.value("candidates").isNull()) {
                recovery["uiStringCandidates"] = scanResult.value("candidates");
            }
        } else {
            m_controller.m_pendingRecoveryAction.clear();
            recovery["workflowStatus"] = "action_failed";
            recovery["message"] = QString("Le traçage du texte affiché a échoué : %1").arg(scanResult.value("error").toString());
        }
        stampIntent(&recovery);
        m_controller.appendSmartSearchDebug("smart_search_answer_trace_ui_string_prompt", recovery);
        return recovery;
    }

    // PHASE 148 : une demande EXPLICITE d'analyse de sources ("analyse les
    // sources numeriques...") ne doit pas etre traitee comme la reponse a la
    // relance trace_ui_filter en attente, meme si m_controller.m_pendingRecoveryAction ==
    // "trace_ui_filter" est actif -- sinon "analyse les sources, c'est
    // toujours 100" est avale par ce bloc (qui lance deja sa propre analyse
    // de sources, mais via le pipeline filter->survivors, pas via l'outil
    // analyze_ui_sources demande). Volontairement NE PAS toucher
    // intent.kind ni m_controller.m_pendingRecoveryAction ici : laisser tomber jusqu'au
    // repli generique m_controller.m_ai.processQuery() plus bas, qui route vers
    // matchUiSourcesTool (ai/ai_engine.cpp) -- m_controller.m_pendingUiStringCandidates
    // reste peuple (ce bloc ne s'execute pas), donc analyze_ui_sources peut
    // toujours s'en servir. Le flow existant "reponse simple = nouvelle
    // valeur" n'est pas touche : ce guard ne matche que sur des mots-cles
    // explicites, jamais sur une simple valeur numerique.
    if (intent.kind == SmartSearchIntentKind::AnswerTraceUiFilterPrompt && !smartSearchExplicitUiSourcesQuery) {
        m_controller.m_pendingRecoveryAction.clear();
        const QString filterValue = !numbers.isEmpty() ? numbers.first() : query.trimmed();
        m_controller.m_smartSearchLastObservedValue = filterValue;

        QVariantMap recovery;
        recovery["success"] = true;
        recovery["query"] = query;
        recovery["aiReady"] = m_controller.m_ai.isReady();
        recovery["targetValue"] = m_controller.m_smartSearchTargetValue;

        const QVariantMap trackResult = m_controller.trackUiStringCandidates(m_controller.m_pendingUiStringCandidates, filterValue);
        const QVariantList survivors = trackResult.value("survivors").toList();
        m_controller.m_pendingUiStringCandidates.clear();

        if (!trackResult.value("success").toBool() || survivors.isEmpty()) {
            recovery["workflowStatus"] = "no_candidate";
            recovery["actionStatus"] = "failed";
            recovery["message"] = QString(
                "Plus aucune string ne suit la valeur \"%1\" — on a perdu la piste du texte affiché.").arg(filterValue);
            QVariantList recoveryActions;
            recoveryActions.append(QVariantMap{{"id", "try_encrypted_scan"}, {"label", "Scan chiffré (XOR)"}, {"value", filterValue}});
            recoveryActions.append(QVariantMap{{"id", "new_search"}, {"label", "Nouvelle recherche"}});
            recovery["recoveryActions"] = recoveryActions;
            stampIntent(&recovery);
            m_controller.appendSmartSearchDebug("smart_search_answer_trace_ui_filter_prompt", recovery);
            return recovery;
        }

        // Analyse des sources numeriques autour de chaque string survivante,
        // rayon croissant (mime le pipeline "Auto origine" d'Expert) : on
        // s'arrete au premier rayon qui donne des resultats. Jusqu'a 3 rayons
        // x 20 candidats de ReadProcessMemory (jusqu'a 16 Mo par lecture) :
        // meme classe de risque AppHang que l'attente IA (cf. m_controller.m_smartSearchBusy
        // dans le header), donc meme remede : pomper processEvents() entre
        // chaque lecture, protege par le meme garde-fou de reentrance.
        const QList<int> radii = {1 * 1024 * 1024, 4 * 1024 * 1024, 16 * 1024 * 1024};
        const int survivorsToAnalyze = std::min<int>(static_cast<int>(survivors.size()), 20);
        QMap<QString, QVariantMap> mergedSources;
        m_controller.m_smartSearchBusy = true;
        for (int radius : radii) {
            mergedSources.clear();
            for (int i = 0; i < survivorsToAnalyze; ++i) {
                QVariantMap sourceOptions;
                sourceOptions["radiusBytes"] = radius;
                sourceOptions["maxResults"] = 300;
                sourceOptions["alignment"] = 1;
                const QVariantMap sourceResult = m_controller.analyzeUiStringSources(survivors.at(i).toMap(), filterValue, sourceOptions);
                QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                const QVariantList candidates = sourceResult.value("candidates").toList();
                for (const auto& item : candidates) {
                    const QVariantMap candidate = item.toMap();
                    const QString key = candidate.value("address").toString() + "|" + candidate.value("type").toString();
                    const auto existing = mergedSources.constFind(key);
                    if (existing == mergedSources.constEnd()
                        || candidate.value("confidence").toDouble() > existing->value("confidence").toDouble()) {
                        mergedSources[key] = candidate;
                    }
                }
            }
            if (!mergedSources.isEmpty()) break;
        }
        m_controller.m_smartSearchBusy = false;

        if (mergedSources.isEmpty()) {
            recovery["workflowStatus"] = "no_candidate";
            recovery["actionStatus"] = "failed";
            recovery["message"] = QString(
                "%1 string(s) suivent toujours \"%2\", mais aucune source numérique plausible autour, même en "
                "élargissant la recherche jusqu'à 16 Mo. La valeur est peut-être calculée par le jeu plutôt que "
                "stockée telle quelle.")
                .arg(survivors.size())
                .arg(filterValue);
            QVariantList recoveryActions;
            recoveryActions.append(QVariantMap{{"id", "open_expert"}, {"label", "Continuer dans Expert"}});
            recoveryActions.append(QVariantMap{{"id", "new_search"}, {"label", "Nouvelle recherche"}});
            recovery["recoveryActions"] = recoveryActions;
            stampIntent(&recovery);
            m_controller.appendSmartSearchDebug("smart_search_answer_trace_ui_filter_prompt", recovery);
            return recovery;
        }

        QVariantList sourceList;
        for (const auto& source : mergedSources) {
            sourceList.append(source);
        }
        std::sort(sourceList.begin(), sourceList.end(), [](const QVariant& a, const QVariant& b) {
            return a.toMap().value("confidence").toDouble() > b.toMap().value("confidence").toDouble();
        });
        if (static_cast<size_t>(sourceList.size()) > kAutoWriteCandidateLimit) {
            sourceList = sourceList.mid(0, static_cast<int>(kAutoWriteCandidateLimit));
        }

        const QString writeValue = m_controller.m_smartSearchTargetValue.isEmpty() ? filterValue : m_controller.m_smartSearchTargetValue;
        m_controller.m_lastBatchStartIndex = m_controller.m_writeHistory.size();
        m_controller.m_lastAutoWriteTargets.clear();
        QVariantList writeResults;
        bool allWritesOk = true;
        for (const auto& item : sourceList) {
            const QVariantMap candidate = item.toMap();
            const QString address = candidate.value("address").toString();
            const QString type = candidate.value("type").toString();
            auto writeResult = m_controller.writeMemoryValueConfirmed(address, type, writeValue);
            writeResult.insert("address", address);
            writeResult.insert("value", writeValue);
            writeResult.insert("type", type);
            allWritesOk = allWritesOk && writeResult.value("success").toBool();
            writeResults.append(writeResult);
            if (writeResult.value("success").toBool()) {
                uint64_t address64 = 0;
                killcore::ValueType valueType;
                if (parseHexAddress(address, &address64) && killcore::parseValueType(type, &valueType)) {
                    m_controller.m_lastAutoWriteTargets.append({address64, valueType});
                }
            }
        }
        m_controller.m_lastBatchEndIndex = m_controller.m_writeHistory.size();
        if (m_controller.m_lastBatchEndIndex == m_controller.m_lastBatchStartIndex) {
            m_controller.m_lastBatchStartIndex = -1;
            m_controller.m_lastBatchEndIndex = -1;
            m_controller.m_lastAutoWriteTargets.clear();
        }
        if (allWritesOk && !m_controller.m_lastAutoWriteTargets.isEmpty()) {
            m_controller.m_smartSearchActive = false;
            m_controller.m_chatMemoryTargets = m_controller.m_lastAutoWriteTargets;
            resetFailureEscalationState();
            m_controller.m_autoWriteValueHistory.clear();
            appendDistinctText(&m_controller.m_autoWriteValueHistory, filterValue, 12);
            appendDistinctText(&m_controller.m_autoWriteValueHistory, writeValue, 12);
        }

        recovery["workflowStatus"] = allWritesOk ? "auto_write_done" : "auto_write_partial_or_failed";
        recovery["actionStatus"] = allWritesOk ? "executed" : "failed";
        recovery["autoWriteResults"] = writeResults;
        recovery["autoWriteResult"] = writeResults.isEmpty() ? QVariantMap{} : writeResults.last().toMap();
        recovery["autoWriteCount"] = writeResults.size();
        recovery["activeTargetCount"] = m_controller.m_chatMemoryTargets.size();
        recovery["previousTargetValue"] = filterValue;
        recovery["writeHistory"] = writeHistoryToVariantList(m_controller.m_autoWriteValueHistory);
        recovery["rollbackNote"] = "Tu peux annuler toutes les écritures via le bouton rollback batch dans l'assistant.";
        recovery["message"] = allWritesOk
            ? QString(
                  "Traçage terminé : %1 source(s) numérique(s) trouvée(s) derrière le texte affiché, écriture de %2 "
                  "appliquée. Fais varier la valeur pour confirmer que ça tient, ou dis-moi si ça n'a pas marché.")
                  .arg(sourceList.size())
                  .arg(writeValue)
            : QString("Sources numériques trouvées, mais l'écriture a partiellement échoué sur certaines adresses.");
        stampIntent(&recovery);
        m_controller.appendSmartSearchDebug("smart_search_answer_trace_ui_filter_prompt", recovery);
        return recovery;
    }

    // RiskGate chat (29/08/2026) : ecrire/figer depuis une adresse tapee dans
    // le chat executait reellement la memoire sans jamais passer par
    // confirmRiskAction (constate en direct pendant PHASE 120-D). Corrige en
    // renvoyant desormais une confirmation + recoveryActions (meme patron que
    // write_value/kernel_write plus haut) au lieu d'appeler
    // writeChatMemoryTargetsFromQuery/freezeChatMemoryTargetsFromQuery
    // directement -- ces deux fonctions n'ont pas change, seul ce point
    // d'entree est desormais gate. Un seul recoveryAction regardless du
    // nombre d'adresses : elles restent server-side dans m_controller.m_chatMemoryTargets,
    // aucune serialisation necessaire.
    const auto makeChatMemoryConfirmation = [&](const QString& actionId, const QString& value,
                                                 const QString& verbInfinitive, const QString& actionRequestedLabel,
                                                 const QString& confirmationReason) {
        QVariantMap confirmResult;
        confirmResult["query"] = query;
        confirmResult["aiReady"] = m_controller.m_ai.isReady();
        confirmResult["actionStatus"] = "requires_confirmation";
        confirmResult["requiresConfirmation"] = true;
        confirmResult["confirmationReason"] = confirmationReason;
        confirmResult["message"] = QString("%1 : %2 sur %3 adresse(s). Confirme pour appliquer.")
            .arg(actionRequestedLabel, value)
            .arg(m_controller.m_chatMemoryTargets.size());
        QVariantList recoveryActions;
        recoveryActions.append(QVariantMap{
            {"id", actionId},
            {"label", QString("%1 %2").arg(verbInfinitive, value)},
            {"value", value},
            {"requiresConfirmation", true},
        });
        confirmResult["recoveryActions"] = recoveryActions;
        stampIntent(&confirmResult);
        return confirmResult;
    };

    if (!smartSearchBypassesMemoryPreIntent
        && (intent.kind == SmartSearchIntentKind::ActivateMemoryTargets
        || (intent.kind == SmartSearchIntentKind::WriteMemoryTargets && !chatAddresses.isEmpty())
        || (intent.kind == SmartSearchIntentKind::FreezeMemoryTargets && !chatAddresses.isEmpty()))) {
        auto activation = activateChatMemoryTargetsFromQuery(query);
        if (intent.kind == SmartSearchIntentKind::WriteMemoryTargets
            && activation.value("success").toBool()
            && numbers.size() == 1) {
            return makeChatMemoryConfirmation("chat_memory_write_confirm", numbers.first(), "écrire", "Écriture demandée",
                "Cette action modifie la mémoire de la cible attachée.");
        }
        if (intent.kind == SmartSearchIntentKind::FreezeMemoryTargets
            && activation.value("success").toBool()
            && numbers.size() == 1) {
            return makeChatMemoryConfirmation("chat_memory_freeze_confirm", numbers.first(), "figer", "Freeze demandé",
                "Fige cette/ces adresse(s) en mémoire (écriture répétée). Reste actif jusqu'à désactivation explicite.");
        }
        stampIntent(&activation);
        return activation;
    }

    if (!smartSearchBypassesMemoryPreIntent && intent.kind == SmartSearchIntentKind::WriteMemoryTargets && numbers.size() == 1) {
        return makeChatMemoryConfirmation("chat_memory_write_confirm", numbers.first(), "écrire", "Écriture demandée",
            "Cette action modifie la mémoire de la cible attachée.");
    }

    if (!smartSearchBypassesMemoryPreIntent && intent.kind == SmartSearchIntentKind::FreezeMemoryTargets && numbers.size() == 1) {
        return makeChatMemoryConfirmation("chat_memory_freeze_confirm", numbers.first(), "figer", "Freeze demandé",
            "Fige cette/ces adresse(s) en mémoire (écriture répétée). Reste actif jusqu'à désactivation explicite.");
    }

    if (!smartSearchBypassesMemoryPreIntent && intent.kind == SmartSearchIntentKind::RewriteLastTargets && numbers.size() == 1) {
        // Gate uniquement si TOUTES les cibles viennent du chat -- sinon
        // (mode Auto/UI-string-trace/profil, chatOrigin=false) comportement
        // inchange : jamais de regression sur l'UX de confiance deja
        // etablie du mode Auto (voir AutoWriteTarget::chatOrigin,
        // application_controller.h).
        const bool allChatOrigin = !m_controller.m_lastAutoWriteTargets.isEmpty()
            && std::all_of(m_controller.m_lastAutoWriteTargets.begin(), m_controller.m_lastAutoWriteTargets.end(),
                           [](const AutoWriteTarget& t) { return t.chatOrigin; });
        if (allChatOrigin) {
            return makeChatMemoryConfirmation("rewrite_last_auto_write_confirm", numbers.first(), "remettre", "Réécriture demandée",
                "Réécrit la dernière valeur sur les adresses actives (issues du chat).");
        }
        auto rewriteTargets = m_controller.rewriteLastAutoWriteTargets(numbers.first(), query);
        stampIntent(&rewriteTargets);
        return rewriteTargets;
    }

    if (!smartSearchBypassesMemoryPreIntent && intent.kind == SmartSearchIntentKind::WriteProfileTargets && numbers.size() == 1) {
        auto profileWrite = writeProfileTargetsFromQuery(query, numbers.first());
        if (profileWrite.value("tool").toString() == "profile_write") {
            stampIntent(&profileWrite);
            return profileWrite;
        }
    }

    QVariantMap result;
    if (intent.kind == SmartSearchIntentKind::ResetContext) {
        result["status"] = "reset_only";
        result["message"] = "D'accord, j'ai oublié le contexte actif. Donne-moi la nouvelle valeur à chercher.";
        result["workflowStatus"] = "idle";
        result["error"] = "";
    } else if (!smartSearchBypassesMemoryPreIntent && intent.kind == SmartSearchIntentKind::ExactScan && numbers.size() == 1) {
        // FirstScanRunning = nouveau lot de candidats, sans rapport avec un
        // eventuel echec signale sur le lot precedent. Sans ce reset, un
        // ExactScan lance sans le mot-cle "nouvelle recherche" (donc sans
        // passer par shouldClearSearchContext plus haut) heriterait du
        // palier d'escalade de secours du lot abandonne et sauterait des
        // etapes de l'echelle ("Tracer le texte affiché") des le premier
        // echec sur cette cible pourtant inedite.
        resetFailureEscalationState();
        QVariantMap args;
        args["value"] = numbers.first();
        args["valueType"] = explicitValueType.isEmpty() ? QString("SmartAuto") : defaultValueType;
        result["status"] = "tool_call";
        result["tool"] = explicitValueType.isEmpty() ? QString("exact_scan_multi_type") : QString("exact_scan");
        result["args"] = args;
        result["rationale"] = intent.rationale;
        result["state"] = "FirstScanRunning";
        result["error"] = "";
    } else if (smartSearchExplicitRefinementQuery) {
        m_controller.m_smartSearchLastObservedValue = explicitRefinementValue;
        QVariantMap args;
        args["mode"] = "exact";
        args["value"] = explicitRefinementValue;
        result["status"] = "tool_call";
        result["tool"] = "next_scan";
        result["args"] = args;
        result["rationale"] = "Recherche active : la phrase demande explicitement de réduire les candidats avec une nouvelle valeur observée, même si elle mentionne un module ou d'autres nombres de contexte.";
        result["state"] = "Refining";
        result["error"] = "";
    } else if (!smartSearchBypassesMemoryPreIntent && intent.kind == SmartSearchIntentKind::RefineScan && numbers.size() == 1) {
        m_controller.m_smartSearchLastObservedValue = numbers.first();
        QVariantMap args;
        args["mode"] = "exact";
        args["value"] = numbers.first();
        result["status"] = "tool_call";
        result["tool"] = "next_scan";
        result["args"] = args;
        result["rationale"] = intent.rationale;
        result["state"] = "Refining";
        result["error"] = "";
    } else if (smartSearchExplicitTraceUiStringQuery && !numbers.isEmpty()) {
        const QString traceValue = numbers.first();
        m_controller.m_smartSearchLastObservedValue = traceValue;
        QVariantMap args;
        args["value"] = traceValue;
        result["status"] = "tool_call";
        result["tool"] = "trace_ui_string";
        result["args"] = args;
        result["rationale"] = "La phrase demande explicitement Trace UI string : je cherche le texte affiché au lieu de relancer un scan module ou global.";
        result["state"] = m_controller.m_smartSearchActive ? QString("Refining") : QString("Idle");
        result["error"] = "";
    } else if (intent.kind == SmartSearchIntentKind::AnswerWriteTargetPrompt) {
        m_controller.m_pendingRecoveryAction.clear();
        m_controller.m_smartSearchTargetValue = !numbers.isEmpty() ? numbers.first() : query.trimmed();
        // Rejoue le dernier "next_scan" avec la meme valeur observee (les
        // candidats n'ont pas change) pour retomber dans le tool=="next_scan"
        // standard plus bas, qui gere deja tout : anti-bruit, double
        // confirmation, ecriture. On evite ainsi de dupliquer cette logique.
        QVariantMap args;
        args["mode"] = "exact";
        args["value"] = m_controller.m_smartSearchLastObservedValue;
        result["status"] = "tool_call";
        result["tool"] = "next_scan";
        result["args"] = args;
        result["rationale"] = intent.rationale;
        result["state"] = "Refining";
        result["error"] = "";
    } else if (smartSearchModuleExplorationQuery && moduleNameFromText(query).isEmpty()) {
        resetFailureEscalationState();
        if (!numbers.isEmpty()) {
            m_controller.m_smartSearchInitialValue = numbers.first();
            m_controller.m_smartSearchLastObservedValue = numbers.first();
        }
        if (numbers.size() >= 2) {
            m_controller.m_smartSearchTargetValue = numbers.at(1);
        }
        QVariantMap args;
        result["status"] = "tool_call";
        result["tool"] = "list_process_modules";
        result["args"] = args;
        result["rationale"] = "La requête cible les XP/score/niveau dans les DLL/modules sans nom de module précis : je liste d'abord les modules chargés au lieu de lancer un scan global.";
        result["state"] = m_controller.m_smartSearchActive ? QString("Refining") : QString("Idle");
        result["error"] = "";
    } else if (!smartSearchBypassesMemoryPreIntent
        && intent.kind == SmartSearchIntentKind::GuidedScan
        && numbers.size() >= 2
        && isStarCraftLikeProcessName(m_controller.processName())) {
        resetFailureEscalationState();
        m_controller.m_smartSearchActive = false;
        m_controller.m_smartSearchTargetValue.clear();
        m_controller.m_smartSearchInitialValue = numbers.at(0);
        result["success"] = true;
        result["status"] = "needs_guided_observation";
        result["actionStatus"] = "not_executed";
        result["workflowStatus"] = "sc2_guided_observation_required";
        result["rationale"] = intent.rationale;
        result["initialValue"] = numbers.at(0);
        result["requestedTargetValue"] = numbers.at(1);
        result["message"] = QString(
            "Pour %1, je ne lance pas un scan exact multi-type automatique depuis le chat : cette cible produit beaucoup "
            "de copies UI et l'appel peut bloquer l'interface. On reste en lecture seule : démarre une session Changed "
            "Pages multi-round à %2, ou cherche d'abord le texte affiché %2, puis fais varier la valeur et donne-moi les "
            "transitions observées.")
            .arg(m_controller.processName().isEmpty() ? QString("cette cible") : m_controller.processName())
            .arg(numbers.at(0));
        QVariantList recoveryActions;
        recoveryActions.append(QVariantMap{
            {"id", "start_changed_pages_session"},
            {"label", "Changed Pages multi-round"},
            {"value", numbers.at(0)},
            {"safe", true},
        });
        recoveryActions.append(QVariantMap{
            {"id", "trace_ui_string"},
            {"label", "Trace UI string"},
            {"value", numbers.at(0)},
            {"safe", true},
        });
        recoveryActions.append(QVariantMap{
            {"id", "try_unknown_changed"},
            {"label", "Unknown initial value"},
            {"safe", true},
        });
        result["recoveryActions"] = recoveryActions;
    } else if (!smartSearchBypassesMemoryPreIntent && intent.kind == SmartSearchIntentKind::GuidedScan && numbers.size() >= 2) {
        // Meme raisonnement que pour ExactScan ci-dessus : nouveau lot,
        // l'echelle de secours du lot precedent ne s'applique plus.
        resetFailureEscalationState();
        QVariantMap args;
        args["value"] = numbers.at(0);
        args["valueType"] = explicitValueType.isEmpty() ? QString("SmartAuto") : defaultValueType;
        result["status"] = "tool_call";
        result["tool"] = explicitValueType.isEmpty() ? QString("exact_scan_multi_type") : QString("exact_scan");
        result["args"] = args;
        result["rationale"] = intent.rationale;
        result["state"] = "FirstScanRunning";
        result["error"] = "";
        m_controller.m_smartSearchTargetValue = numbers.at(1);
        m_controller.m_smartSearchActive = true;
    } else {
        QVariantMap aiContext;
        aiContext["processAttached"] = m_controller.m_handle.isValid();
        aiContext["processName"] = m_controller.processName();
        aiContext["scanActive"] = m_controller.m_smartSearchActive;
        aiContext["candidateCount"] = static_cast<qulonglong>(candidates.size());
        aiContext["initialValue"] = m_controller.m_smartSearchInitialValue;
        aiContext["targetValue"] = m_controller.m_smartSearchTargetValue;
        aiContext["activeTargetCount"] = static_cast<qulonglong>(m_controller.m_chatMemoryTargets.size());
        aiContext["unknownSnapshotActive"] = !m_controller.scanState().snapshot().isEmpty();
        aiContext["freezeCount"] = static_cast<qulonglong>(m_controller.m_freezeHotkeyOverlayManager->freezeManager().entries().size());
        aiContext["valueType"] = m_controller.m_smartSearchValueType;
        m_controller.m_smartSearchBusy = true;
        result = m_controller.m_ai.processQuery(query, aiContext);
        m_controller.m_smartSearchBusy = false;
    }

    result["query"] = query;
    result["aiReady"] = m_controller.m_ai.isReady();
    result["debugFile"] = m_controller.smartSearchDebugFilePath();

    if (result.value("status").toString() != "tool_call") {
        result["actionStatus"] = "not_executed";
        if (result.value("message").toString().trimmed().isEmpty()
            && result.value("error").toString().trimmed().isEmpty()) {
            result["message"] = "D'accord. Donne-moi la valeur à chercher, ou précise que tu veux écrire sur une adresse active.";
        }
        stampIntent(&result);
        return result;
    }

    const QString tool = result.value("tool").toString();
    const QVariantMap args = result.value("args").toMap();
    QVariantMap actionResult;

    // Le modele connait l'issue des actions precedentes: apres un echec il
    // proposera une alternative (multi_type, encrypted, trace UI, unknown).
    const auto noteAiOutcome = [this, &query, &tool](bool success, const QString& detail) {
        m_controller.m_ai.noteOutcome(query, tool, success ? QStringLiteral("success") : QStringLiteral("failed"));
        Q_UNUSED(detail);
    };

    if (tool == "exact_scan") {
        actionResult = m_controller.startExactScan(args.value("value").toString(), args.value("valueType").toString());
    } else if (tool == "exact_scan_module") {
        const QString requestedModule = args.value("module").toString().trimmed();
        const QString scanValue = args.value("value").toString().trimmed();
        const QString scanType = args.value("valueType", explicitValueType.isEmpty() ? QString("Int32") : defaultValueType).toString();
        const auto modules = killcore::ProcessEnumerator::enumerateModules(static_cast<uint32_t>(m_controller.m_pid));
        killcore::ProcessModuleInfo matchedModule;
        bool foundModule = false;
        for (const auto& module : modules) {
            if (module.name.compare(requestedModule, Qt::CaseInsensitive) == 0) {
                matchedModule = module;
                foundModule = true;
                break;
            }
            if (!foundModule && module.name.contains(requestedModule, Qt::CaseInsensitive)) {
                matchedModule = module;
                foundModule = true;
            }
        }

        if (!foundModule) {
            actionResult["success"] = false;
            actionResult["error"] = QString("Module '%1' introuvable dans le processus attaché. Liste d'abord les modules/DLL.").arg(requestedModule);
            actionResult["module"] = requestedModule;
            result["workflowStatus"] = "module_not_found";
            result["message"] = actionResult.value("error").toString();
        } else if (scanValue.isEmpty()) {
            actionResult["success"] = false;
            actionResult["error"] = "Il me faut une valeur à scanner dans ce module.";
            result["actionStatus"] = "needs_clarification";
            result["workflowStatus"] = "missing_value";
            result["message"] = actionResult.value("error").toString();
        } else {
            QVariantMap expertOptions;
            expertOptions["startAddress"] = QString::number(matchedModule.baseAddress, 16);
            expertOptions["stopAddress"] = QString::number(matchedModule.baseAddress + matchedModule.size, 16);
            expertOptions["writableOnly"] = false;
            expertOptions["executableOnly"] = false;
            expertOptions["copyOnWriteOnly"] = false;
            actionResult = m_controller.startExactScanExpert(scanValue, scanType, expertOptions);
            actionResult["module"] = matchedModule.name;
            actionResult["moduleBase"] = QString::number(matchedModule.baseAddress, 16);
            actionResult["moduleEnd"] = QString::number(matchedModule.baseAddress + matchedModule.size, 16);
            actionResult["moduleSize"] = static_cast<qulonglong>(matchedModule.size);
            const qulonglong moduleCandidateCount = actionResult.value("matchesFound").toULongLong();
            result["workflowStatus"] = moduleCandidateCount > 0
                ? "module_scan_found"
                : "no_candidate";
            if (!args.value("targetValue").toString().isEmpty()) {
                m_controller.m_smartSearchTargetValue = args.value("targetValue").toString();
            }
            m_controller.m_smartSearchActive = true;
            m_controller.m_smartSearchInitialValue = scanValue;
            m_controller.m_smartSearchLastObservedValue = scanValue;
            m_controller.m_smartSearchValueType = scanType;
            QVariantList recoveryActions;
            if (moduleCandidateCount > 0) {
                result["message"] = QString(
                    "Scan module %1 : %2 candidat(s) pour %3 dans [%4..%5]. Fais changer l'XP dans le jeu, puis tape la nouvelle valeur affichée pour réduire ces candidats. Exemple : maintenant l'XP affichée est 1120.")
                    .arg(matchedModule.name)
                    .arg(moduleCandidateCount)
                    .arg(scanValue)
                    .arg(actionResult.value("moduleBase").toString())
                    .arg(actionResult.value("moduleEnd").toString());
                recoveryActions.append(QVariantMap{{"id", "reduce_again"}, {"label", "Réduire avec nouvelle XP"}, {"safe", true}});
                recoveryActions.append(QVariantMap{{"id", "trace_ui_string"}, {"label", "Trace UI string"}, {"value", scanValue}, {"safe", true}});
                recoveryActions.append(QVariantMap{{"id", "start_changed_pages_diff"}, {"label", "Changed Pages"}, {"safe", true}});
            } else {
                result["message"] = QString(
                    "Scan module %1 : aucun candidat pour %2 dans [%3..%4]. On évite de repartir en global : essaie Trace UI string avec %2 ou Changed Pages avant/après une variation d'XP.")
                    .arg(matchedModule.name)
                    .arg(scanValue)
                    .arg(actionResult.value("moduleBase").toString())
                    .arg(actionResult.value("moduleEnd").toString());
                recoveryActions.append(QVariantMap{{"id", "trace_ui_string"}, {"label", "Trace UI string"}, {"value", scanValue}, {"safe", true}});
                recoveryActions.append(QVariantMap{{"id", "start_changed_pages_diff"}, {"label", "Changed Pages"}, {"safe", true}});
                recoveryActions.append(QVariantMap{{"id", "try_unknown_changed"}, {"label", "Unknown"}, {"safe", true}});
            }
            result["candidateCount"] = moduleCandidateCount;
            result["recoveryActions"] = recoveryActions;
        }
    } else if (tool == "exact_scan_multi_type") {
        actionResult = m_controller.startExactScanMultiType(args.value("value").toString(), args.value("valueType").toString());
    } else if (tool == "next_scan") {
        actionResult = m_controller.nextScan(args.value("mode").toString(), args.value("value").toString());
    } else if (tool == "get_candidates") {
        // PHASE (T6, evaluation live 08/09/2026) : sans cet outil, aucun moyen
        // honnete de repondre "quelle est l'adresse ?" une fois la liste
        // reduite -- next_scan/exact_scan ne renvoient qu'un compteur, jamais
        // les adresses. Meme raisonnement cote backend Claude
        // (ClaudeChatManager::executeTool).
        actionResult = m_controller.getCandidates(0, 50, QString());
        actionResult["success"] = true;
        if (actionResult.value("displaySuppressed").toBool()) {
            result["message"] = QString("Trop de candidats pour les lister (%1 au total). Continue à réduire avec next_scan avant de rappeler get_candidates.")
                                     .arg(actionResult.value("totalCount").toULongLong());
        } else {
            const int shown = actionResult.value("candidates").toList().size();
            result["message"] = shown > 0
                ? QString("%1 candidat(s) affiché(s) sur %2 au total. Plusieurs adresses réelles peuvent légitimement correspondre à la même valeur logique (copies redondantes, checksums) — ne suppose pas qu'une seule est la bonne.")
                      .arg(shown)
                      .arg(actionResult.value("totalCount").toULongLong())
                : "Aucun candidat en mémoire actuellement. Lance d'abord un exact_scan.";
        }
    } else if (tool == "unknown_capture") {
        actionResult = m_controller.captureUnknownSnapshot();
    } else if (tool == "unknown_compare") {
        actionResult = m_controller.unknownNextScan(args.value("mode").toString(), args.value("valueType").toString());
    } else if (tool == "auto_resolve") {
        QVariantMap autoOptions;
        autoOptions["executeSafe"] = true;
        if (!explicitValueType.isEmpty()) autoOptions["valueType"] = defaultValueType;
        actionResult = startAutoResolve(args.value("query").toString(), autoOptions);
        if (!actionResult.value("message").toString().isEmpty()) {
            result["message"] = actionResult.value("message");
        }
        if (!actionResult.value("executedSafeSteps").isNull()) {
            result["executedSafeSteps"] = actionResult.value("executedSafeSteps");
        }
    } else if (tool == "encrypted_scan") {
        QVariantMap encryptedOptions;
        encryptedOptions["mode"] = args.value("mode", "xor");
        encryptedOptions["key"] = args.value("key", "0");
        encryptedOptions["keySearchBits"] = args.value("keySearchBits", 16);
        actionResult = m_controller.scanEncryptedValue(
            args.value("value").toString(),
            args.value("valueType", explicitValueType.isEmpty() ? QString("Int32") : defaultValueType).toString(),
            encryptedOptions);
        if (actionResult.value("success").toBool()) {
            const auto encryptedMatches = actionResult.value("matchesFound").toULongLong();
            result["workflowStatus"] = encryptedMatches > 0 ? "awaiting_value_change" : "no_candidate";
            result["message"] = encryptedMatches > 0
                ? QString("Scan chiffré : %1 adresse(s) correspondent à %2 sous une forme chiffrée (XOR/Add/Sub). Fais varier la valeur puis redonne-la moi pour affiner.")
                      .arg(encryptedMatches)
                      .arg(args.value("value").toString())
                : QString("Scan chiffré : aucune adresse ne correspond à %1 sous forme chiffrée. On peut tenter la Trace UI string ou Unknown.")
                      .arg(args.value("value").toString());
            if (encryptedMatches == 0) {
                QVariantList recoveryActions;
                recoveryActions.append(QVariantMap{{"id", "trace_ui_string"}, {"label", "Chercher le texte affiché"}, {"value", args.value("value")}});
                recoveryActions.append(QVariantMap{{"id", "try_unknown_changed"}, {"label", "Unknown (valeur inconnue)"}});
                recoveryActions.append(QVariantMap{{"id", "new_search"}, {"label", "Nouvelle recherche"}});
                result["recoveryActions"] = recoveryActions;
            }
        }
    } else if (tool == "prepare_write_checkpoint") {
        // Checkpoint safe: prepare les suggestions d'ecriture sans ecrire.
        const QString checkpointValue = args.value("value", m_controller.m_smartSearchTargetValue).toString();
        auto suggestions = suggestedWritesForCandidates(candidates, checkpointValue, kAutoWriteCandidateLimit);
        m_controller.enrichSuggestedWritesWithHistory(&suggestions);
        actionResult["success"] = !suggestions.isEmpty();
        actionResult["suggestedWrites"] = suggestions;
        result["suggestedWrites"] = suggestions;
        result["suggestedWrite"] = suggestions.isEmpty() ? QVariantMap{} : suggestions.first().toMap();
        result["workflowStatus"] = suggestions.isEmpty() ? "no_candidate" : "awaiting_write_confirmation";
        result["message"] = suggestions.isEmpty()
            ? QString("Aucun candidat fiable a preparer. Continue a reduire la liste avec de nouvelles valeurs observees.")
            : QString("Checkpoint pret: %1 adresse(s) candidate(s) pour ecrire %2. Confirme l'ecriture pour appliquer.")
                  .arg(suggestions.size())
                  .arg(checkpointValue);
    } else if (tool == "read_window_text") {
        QVariantMap windowOptions = args;
        if (!windowOptions.contains("includeAllVisible")) windowOptions["includeAllVisible"] = true;
        actionResult = m_controller.readAttachedWindowText(windowOptions);
        if (actionResult.value("success").toBool()) {
            result["workflowStatus"] = "window_text_observed";
            result["message"] = QString("Inspection fenêtre : %1 fenêtre(s) lue(s). On peut s'en servir pour synchroniser la prochaine variation avant de comparer la mémoire.")
                                  .arg(actionResult.value("windowCount").toInt());
        }
    } else if (tool == "list_process_modules") {
        const QVariantList modules = m_controller.getProcessModules(static_cast<int>(m_controller.m_pid));
        QVariantList highlights;
        QStringList highlightNames;
        QList<QPair<int, QVariantMap>> scoredHighlights;
        for (const QVariant& item : modules) {
            const QVariantMap module = item.toMap();
            const int score = moduleGameplayRelevanceScore(module);
            if (score <= 0) {
                continue;
            }
            scoredHighlights.append({score, module});
        }
        std::stable_sort(scoredHighlights.begin(), scoredHighlights.end(),
            [](const auto& lhs, const auto& rhs) {
                return lhs.first > rhs.first;
            });
        constexpr int kMaxDisplayedModuleHighlights = 8;
        for (const auto& item : scoredHighlights) {
            if (highlights.size() >= kMaxDisplayedModuleHighlights) {
                break;
            }
            highlights.append(item.second);
            highlightNames.append(item.second.value("name").toString());
        }

        actionResult["success"] = true;
        actionResult["modules"] = modules;
        actionResult["moduleCount"] = modules.size();
        actionResult["highlights"] = highlights;
        actionResult["highlightCount"] = highlights.size();
        result["modules"] = modules;
        result["moduleHighlights"] = highlights;
        result["workflowStatus"] = "process_modules_listed";
        result["message"] = highlightNames.isEmpty()
            ? QString("Modules/DLL : %1 module(s) chargés. Aucun module applicatif évident repéré automatiquement ; trie par chemin et privilégie les modules non système avant AOB/désassemblage.")
                  .arg(modules.size())
            : QString("Modules/DLL : %1 module(s) chargés. Pistes priorisées : %2. Les runtimes C++/telemetry sont ignorés dans cette sélection. Prochaine étape : si un module métier ressort, lance un scan module ciblé avec la valeur affichée ; sinon Trace UI string ou Changed Pages pour relier l'affichage XP à la source.")
                  .arg(modules.size())
                  .arg(highlightNames.join(", "));
        QVariantList recoveryActions;
        const QString traceValue = !m_controller.m_smartSearchLastObservedValue.isEmpty()
            ? m_controller.m_smartSearchLastObservedValue
            : m_controller.m_smartSearchInitialValue;
        recoveryActions.append(QVariantMap{{"id", "open_expert"}, {"label", "Inspecter les modules"}, {"expertStep", "inspect"}});
        recoveryActions.append(QVariantMap{{"id", "trace_ui_string"}, {"label", "Trace UI string"}, {"value", traceValue}, {"safe", true}});
        recoveryActions.append(QVariantMap{{"id", "start_changed_pages_diff"}, {"label", "Changed Pages"}, {"safe", true}});
        result["recoveryActions"] = recoveryActions;
    } else if (tool == "start_changed_pages_diff") {
        QVariantMap diffOptions = args;
        if (!diffOptions.contains("maxBytesMb")) diffOptions["maxBytesMb"] = 64;
        if (!diffOptions.contains("blockSize")) diffOptions["blockSize"] = 64 * 1024;
        if (!diffOptions.contains("privateOnly")) diffOptions["privateOnly"] = true;
        if (!diffOptions.contains("writableOnly")) diffOptions["writableOnly"] = true;
        actionResult = m_controller.startChangedPagesDiff(diffOptions);
        if (actionResult.value("success").toBool()) {
            result["workflowStatus"] = "awaiting_observed_variation";
            result["message"] = QString("Mode Inspecteur : snapshot lecture seule capturé (%1 blocs, %2 Mo max). Fais varier la valeur affichée, puis donne-moi l'ancienne et la nouvelle valeur pour comparer.")
                                  .arg(actionResult.value("blocksCaptured").toInt())
                                  .arg(diffOptions.value("maxBytesMb").toInt());
        }
    } else if (tool == "finish_changed_pages_diff") {
        QVariantMap diffOptions = args;
        diffOptions.remove("previousValue");
        diffOptions.remove("currentValue");
        actionResult = m_controller.finishChangedPagesDiff(
            args.value("previousValue").toString(),
            args.value("currentValue").toString(),
            diffOptions);
        if (actionResult.value("success").toBool()) {
            const int hitCount = actionResult.value("hitCount", actionResult.value("hits").toList().size()).toInt();
            result["workflowStatus"] = hitCount > 0 ? "diff_hits_found" : "no_candidate";
            result["message"] = hitCount > 0
                ? QString("Mode Inspecteur : %1 piste(s) trouvée(s) dans les pages réellement modifiées. À valider en watch ou par nouvelle variation avant toute écriture.")
                      .arg(hitCount)
                : QString("Mode Inspecteur : aucune piste numérique directe dans les pages modifiées. On garde l'hypothèse copie UI/buffer et on évite l'écriture directe.");
        }
    } else if (tool == "start_changed_pages_session") {
        QVariantMap sessionOptions = args;
        if (!sessionOptions.contains("maxBytesMb")) sessionOptions["maxBytesMb"] = 64;
        if (!sessionOptions.contains("blockSize")) sessionOptions["blockSize"] = 64 * 1024;
        if (!sessionOptions.contains("privateOnly")) sessionOptions["privateOnly"] = true;
        if (!sessionOptions.contains("writableOnly")) sessionOptions["writableOnly"] = true;
        actionResult = m_controller.startChangedPagesSession(sessionOptions);
        if (actionResult.value("success").toBool()) {
            result["workflowStatus"] = "changed_pages_session_started";
            result["message"] = QString("Session multi-round démarrée (%1 blocs, %2 Mo max). Fais varier la valeur affichée, puis donne-moi l'ancienne et la nouvelle valeur pour chaque round. Après 2-3 rounds, je te donnerai les adresses les plus stables.")
                                  .arg(actionResult.value("blocksCaptured").toInt())
                                  .arg(sessionOptions.value("maxBytesMb").toInt());
        }
    } else if (tool == "apply_changed_pages_round") {
        QVariantMap roundOptions = args;
        roundOptions.remove("previousValue");
        roundOptions.remove("currentValue");
        actionResult = m_controller.applyChangedPagesRound(
            args.value("previousValue").toString(),
            args.value("currentValue").toString(),
            roundOptions);
        if (actionResult.value("success").toBool()) {
            const int hitsFound = actionResult.value("hitsFound").toInt();
            const int confirmed = actionResult.value("entriesConfirmedAtLeast2").toInt();
            const int rounds = actionResult.value("roundsApplied").toInt();
            result["workflowStatus"] = confirmed > 0 ? "consensus_candidates_found" : "round_applied";
            result["message"] = confirmed > 0
                ? QString("Round %1 : %2 hit(s), %3 adresse(s) confirmée(s) sur au moins 2 rounds. Les adresses stables sont candidates pour Page Guard ou write.")
                      .arg(rounds).arg(hitsFound).arg(confirmed)
                : QString("Round %1 : %2 hit(s) trouvés, aucune adresse encore confirmée sur 2+ rounds. Continue à faire varier la valeur.")
                      .arg(rounds).arg(hitsFound);
            if (!actionResult.value("topEntries").isNull()) {
                result["topEntries"] = actionResult.value("topEntries");
            }
        }
    } else if (tool == "get_changed_pages_consensus") {
        QVariantMap consensusOptions = args;
        actionResult = m_controller.getChangedPagesConsensus(consensusOptions);
        if (actionResult.value("success").toBool()) {
            const int confirmed = actionResult.value("entriesConfirmed").toInt();
            const int total = actionResult.value("entriesTotal").toInt();
            const int eliminated = actionResult.value("entriesEliminated").toInt();
            const int rounds = actionResult.value("roundsApplied").toInt();
            result["workflowStatus"] = confirmed > 0 ? "consensus_candidates_found" : "no_candidate";
            result["message"] = confirmed > 0
                ? QString("Consensus : %1 adresse(s) confirmée(s) sur %2 total (%3 éliminées, %4 rounds). Les adresses stables sont candidates pour Page Guard ou write.")
                      .arg(confirmed).arg(total).arg(eliminated).arg(rounds)
                : QString("Consensus : aucune adresse confirmée sur %1 total (%2 éliminées, %3 rounds). Continue à faire varier la valeur.")
                      .arg(total).arg(eliminated).arg(rounds);
            if (!actionResult.value("confirmedEntries").isNull()) {
                result["confirmedEntries"] = actionResult.value("confirmedEntries");
            }
            if (!actionResult.value("topEntries").isNull()) {
                result["topEntries"] = actionResult.value("topEntries");
            }
        }
    } else if (tool == "stop_changed_pages_session") {
        actionResult = m_controller.stopChangedPagesSession();
        if (actionResult.value("success").toBool()) {
            const int confirmed = actionResult.value("entriesConfirmed").toInt();
            const int rounds = actionResult.value("roundsApplied").toInt();
            result["workflowStatus"] = "changed_pages_session_stopped";
            result["message"] = QString("Session multi-round arrêtée. %1 round(s) effectués, %2 adresse(s) confirmée(s).")
                                  .arg(rounds).arg(confirmed);
            if (!actionResult.value("confirmedEntriesList").isNull()) {
                result["confirmedEntries"] = actionResult.value("confirmedEntriesList");
            }
        }
    } else if (tool == "trace_ui_string") {
        QVariantMap traceOptions;
        traceOptions["ascii"] = true;
        traceOptions["utf16"] = true;
        traceOptions["writableOnly"] = true;
        actionResult = m_controller.scanUiStrings(args.value("value").toString(), traceOptions);
        if (actionResult.value("success").toBool()) {
            const auto stringsFound = actionResult.value("matchesFound").toULongLong();
            if (stringsFound > 0) {
                // Meme suite conversationnelle que AnswerTraceUiStringPrompt :
                // ce chemin est emprunte quand l'IA choisit directement l'outil
                // trace_ui_string (demande en langage libre, ex: "j'ai un texte
                // a 240 ou je veux trouver la cible") plutot que via l'echelle
                // d'escalade ReportBadTargets. Sans ce meme pending, la reponse
                // suivante de l'utilisateur (la nouvelle valeur affichee) ne
                // continue pas le traçage : elle retombe sur ExactScan et
                // abandonne silencieusement toute la piste deja trouvee.
                m_controller.m_pendingRecoveryAction = "trace_ui_filter";
                m_controller.m_pendingUiStringCandidates = actionResult.value("candidates").toList();
                result["workflowStatus"] = "trace_ui_string_found";
            } else {
                m_controller.m_pendingRecoveryAction.clear();
                result["workflowStatus"] = "no_candidate";
            }
            result["message"] = stringsFound > 0
                ? QString("Trace UI string : %1 occurrence(s) du texte \"%2\" trouvées en mémoire. Fais varier la "
                          "valeur dans le jeu, puis donne-moi la nouvelle valeur affichée — je filtre les bonnes "
                          "pistes et je cherche la source numérique derrière, automatiquement.")
                      .arg(stringsFound)
                      .arg(args.value("value").toString())
                : QString("Trace UI string : le texte \"%1\" n'a pas été trouvé en mémoire. Vérifie la valeur affichée exacte, ou passe en Unknown.")
                      .arg(args.value("value").toString());
            if (!actionResult.value("candidates").isNull()) {
                result["uiStringCandidates"] = actionResult.value("candidates");
            }
            if (stringsFound == 0) {
                QVariantList recoveryActions;
                recoveryActions.append(QVariantMap{{"id", "try_encrypted_scan"}, {"label", "Scan chiffré (XOR)"}, {"value", args.value("value")}});
                recoveryActions.append(QVariantMap{{"id", "try_unknown_changed"}, {"label", "Unknown (valeur inconnue)"}});
                recoveryActions.append(QVariantMap{{"id", "new_search"}, {"label", "Nouvelle recherche"}});
                result["recoveryActions"] = recoveryActions;
            }
        }
    } else if (tool == "trainer_list_features") {
        // PHASE 169 : callVueStoreAction() ici serait un appel a
        // runJavaScript() REENTRANT depuis l'interieur meme du Q_INVOKABLE
        // (startSmartSearch) que le JS de cette page est en train d'attendre
        // -- confirme en direct (PHASE 168/169, docs/PHASE_TRACKER.md) : le
        // callback JS n'arrive jamais dans les 5s, l'outil echoue a 100% par
        // timeout. getTrainerFeaturesSnapshot() est une lecture pure, deja
        // disponible directement dans le meme contexte JS que l'appelant --
        // on delegue donc la lecture + le message a ui/src/stores/app.ts
        // (sendMessage, juste apres le retour de startSmartSearch()) au lieu
        // de faire un aller-retour C++ inutile.
        result["needsLocalStoreAction"] = "trainer_list_features";
        result["actionStatus"] = "pending_local_action";
    } else if (tool == "trainer_create_write") {
        const QString trainerAddress = args.value("address").toString().trimmed();
        const QString trainerValue = args.value("value").toString().trimmed();
        const QString trainerValueType = args.value("valueType", "Int32").toString().trimmed();
        if (trainerAddress.isEmpty() || trainerValue.isEmpty()) {
            result["actionStatus"] = "needs_clarification";
            result["message"] = "Pour créer une feature Trainer depuis le chat, donne une adresse 0x... et la valeur à écrire.";
            stampIntent(&result);
            return result;
        }

        QVariantMap feature;
        feature["name"] = args.value("name", "Assistant Trainer write").toString();
        feature["action"] = "write";
        feature["address"] = trainerAddress;
        feature["valueType"] = trainerValueType.isEmpty() ? QString("Int32") : trainerValueType;
        feature["value"] = trainerValue;

        // PHASE 163 : chercher un locator resilient plutot que de figer
        // aveuglement en 'absolute' -- une adresse absolue promue en Trainer
        // sans locator resilient ne survit generalement pas a un relaunch/
        // changement de scene du process cible (retour d'experience Vampire
        // Survivors, voir docs/PHASE_TRACKER.md PHASE 160/162/163 et memoire
        // feedback_freeze_proposal_uses_trainer). On essaie dans l'ordre :
        // 1) AOB (adresse dans l'image statique du module -- code ou donnee
        //    statique, PAS un objet alloue dynamiquement) ;
        // 2) pointer chain (adresse dans un objet alloue dynamiquement,
        //    atteignable depuis une base statique -- cas heap le plus courant,
        //    reutilise scanPointerChains deja existant) ;
        // 3) sinon 'absolute', mais avec un avertissement honnete plutot que
        //    de laisser croire a une persistance qui n'existe pas.
        QString locatorKind = "absolute";
        QString locatorSummary;
        {
            const auto signature = m_controller.generateAobSignature(trainerAddress, QVariantMap{{"beforeBytes", 0}, {"length", 20}});
            if (signature.value("success").toBool() && !signature.value("codeReadProtected").toBool()) {
                const QString pattern = signature.value("pattern").toString();
                const auto quality = signature.value("signatureQuality").toMap();
                const int fixedBytes = quality.value("fixedBytes").toInt();
                const int score = quality.value("score").toInt();
                if (!pattern.isEmpty() && fixedBytes >= 3 && score >= 35) {
                    // executableOnly=false : une donnee (.data/.bss) n'est jamais
                    // executable -- meme correctif que PHASE 160 (voir plus haut
                    // dans ce fichier, resolveTrainerFeatureAddress cote frontend).
                    const auto scan = m_controller.scanAobPattern(pattern, QVariantMap{{"executableOnly", false}, {"imageOnly", true}, {"maxResults", 2}});
                    if (scan.value("success").toBool() && scan.value("matchesFound").toInt() == 1) {
                        locatorKind = "aob";
                        feature["aobPattern"] = pattern;
                        locatorSummary = "verrouillée sur une signature AOB stable (résiste à un relaunch tant que le code/la donnée statique ne change pas de version)";
                    }
                }
            }
        }
        if (locatorKind == "absolute") {
            const auto pointerScan = m_controller.scanPointerChains(trainerAddress, QVariantMap{{"maxDepth", 3}, {"maxResults", 5}, {"onlyModuleBase", true}});
            const QVariantList chains = pointerScan.value("chains").toList();
            if (pointerScan.value("success").toBool() && !chains.isEmpty()) {
                locatorKind = "pointer_chain";
                feature["pointerChain"] = chains.first();
                locatorSummary = "ancrée via une chaîne de pointeurs (résiste à une réallocation de l'objet en mémoire, ex. nouvelle partie)";
            }
        }
        feature["locatorKind"] = locatorKind;

        // PHASE 169 : la resolution de locator ci-dessus (generateAobSignature/
        // scanAobPattern/scanPointerChains) reste un appel C++ direct, aucun
        // probleme de reentrance -- seule la creation effective de la feature
        // (createTrainerFeature) passait par callVueStoreAction() et heurtait
        // le meme timeout systematique que trainer_list_features ci-dessus.
        // Meme delegation : le JS cree la feature localement et construit le
        // message de succes avec les memes donnees (locatorKind/locatorSummary).
        result["needsLocalStoreAction"] = "trainer_create_write";
        result["actionStatus"] = "pending_local_action";
        result["pendingFeature"] = feature;
        result["locatorSummary"] = locatorSummary;
        result["pendingAddress"] = trainerAddress;
        result["pendingValueType"] = feature.value("valueType");
        result["pendingValue"] = trainerValue;
    } else if (tool == "trainer_delete_feature") {
        const int trainerId = args.value("id").toInt();
        if (trainerId <= 0) {
            result["actionStatus"] = "needs_clarification";
            result["message"] = "Quelle feature Trainer veux-tu supprimer ? Donne son id, ou demande d'abord la liste du Trainer.";
            stampIntent(&result);
            return result;
        }
        // PHASE 169 : meme raison que trainer_list_features ci-dessus.
        result["needsLocalStoreAction"] = "trainer_delete_feature";
        result["actionStatus"] = "pending_local_action";
        result["pendingTrainerId"] = trainerId;
    } else if (tool == "trainer_apply_request" || tool == "trainer_restore_request") {
        // PHASE 120-D (29/08/2026) : la raison d'origine (PHASE 121/129,
        // "eviter timeout ou attente modale invisible") est perimee -- elle
        // date d'avant le patron recoveryActions de PHASE 148, qui resout deja
        // ce risque par construction : le modal confirmRiskAction() ne s'ouvre
        // JAMAIS "en autonome", seulement sur un clic explicite de l'utilisateur
        // sur le bouton recoveryAction. Meme patron que kernel_write/write_value
        // ci-dessus, reutilise les fonctions store deja confirmees par RiskGate
        // (applyTrainerFeature/restoreTrainerFeature, ordre de dependances
        // deja gere en interne).
        const bool restore = tool == "trainer_restore_request";
        const bool all = args.value("all").toBool();
        const QString trainerId = args.value("id").toString().trimmed();
        if (!all && (trainerId.isEmpty() || trainerId.toInt() <= 0)) {
            result["actionStatus"] = "needs_clarification";
            result["message"] = "Demande d'abord la liste du Trainer si tu ne connais pas l'id de la feature à "
                + QString(restore ? "restaurer" : "activer") + ".";
            stampIntent(&result);
            return result;
        }
        result["actionStatus"] = "requires_confirmation";
        result["requiresConfirmation"] = true;
        result["confirmationReason"] = restore
            ? "Restaure la feature Trainer à sa valeur d'origine (désactivation)."
            : "Active la feature Trainer : écrit/fige sa valeur configurée en mémoire.";
        result["message"] = all
            ? (restore ? "Restauration de tout le Trainer demandée. Confirme pour restaurer toutes les features actives."
                       : "Activation de tout le Trainer demandée. Confirme pour appliquer toutes les features.")
            : (restore ? QString("Restauration de la feature Trainer #%1 demandée. Confirme pour restaurer.").arg(trainerId)
                       : QString("Activation de la feature Trainer #%1 demandée. Confirme pour appliquer.").arg(trainerId));
        QVariantList recoveryActions;
        recoveryActions.append(QVariantMap{
            {"id", restore ? "trainer_restore_confirm" : "trainer_apply_confirm"},
            {"label", all
                ? QString(restore ? "Tout restaurer" : "Tout activer")
                : QString(restore ? "Restaurer #%1" : "Activer #%1").arg(trainerId)},
            {"id_target", trainerId},
            {"all", all},
            {"requiresConfirmation", true},
        });
        result["recoveryActions"] = recoveryActions;
        stampIntent(&result);
        return result;
    } else if (tool == "analyze_field_stability") {
        // PHASE 130 : jamais d'ecriture, s'execute directement (pas de
        // requiresConfirmation) comme discover_save_files/inspect_local_settings --
        // meme si ca attache brievement un debugger (comme find_what_writes,
        // deja utilise sans RiskGate modal dans l'UI derriere une simple case
        // a cocher "Debugger autorise").
        const QString stabilityAddress = args.value("address").toString().trimmed();
        if (stabilityAddress.isEmpty()) {
            result["actionStatus"] = "needs_clarification";
            result["message"] = "Il me faut une adresse (0x...) pour analyser si c'est un champ affiché ou une source.";
            stampIntent(&result);
            return result;
        }
        actionResult = m_controller.analyzeFieldStability(stabilityAddress, {});
        if (actionResult.value("success").toBool()) {
            // Le rationale (core/scanner/display_source_classifier.cpp) est
            // deja une phrase complete et actionnable -- pas besoin d'ajouter
            // de conclusion redondante ici.
            result["workflowStatus"] = "field_stability_analyzed";
            result["message"] = QString("Analyse de %1 : %2")
                .arg(stabilityAddress, actionResult.value("rationale").toString());
        } else {
            result["message"] = actionResult.value("error").toString();
        }
        stampIntent(&result);
        return result;
    } else if (tool == "get_auto_report") {
        // PHASE 140 : lecture seule, cout quasi nul (agrege des donnees deja
        // collectees) -- execute directement, meme categorie que
        // analyze_field_stability ci-dessus.
        const int maxEvents = std::clamp(args.value("maxEvents", 50).toInt(), 5, 200);
        actionResult = getAutoResolveReport(maxEvents);
        if (actionResult.value("success").toBool()) {
            result["workflowStatus"] = "auto_report_ready";
            result["message"] = actionResult.value("summary").toString().isEmpty()
                ? "Rapport d'auto-résolution généré."
                : actionResult.value("summary").toString();
        } else {
            result["message"] = actionResult.value("error").toString();
        }
        stampIntent(&result);
        return result;
    } else if (tool == "analyze_ui_sources") {
        // PHASE 140 : lecture seule (killcore::MemoryReader uniquement), suite
        // naturelle de trace_ui_string. Reutilise m_controller.m_pendingUiStringCandidates
        // (meme etat que le pending trace_ui_filter) si aucune adresse
        // explicite n'est fournie -- une phrase NL fournit rarement une
        // adresse ET une byteLength en une fois.
        const QString uiSourceValue = args.value("value").toString().trimmed();
        if (uiSourceValue.isEmpty()) {
            result["actionStatus"] = "needs_clarification";
            result["message"] = "Il me faut la valeur actuellement affichée pour analyser les sources numériques.";
            stampIntent(&result);
            return result;
        }

        QVariantMap uiStringCandidate;
        const QString explicitUiSourceAddress = args.value("address").toString().trimmed();
        if (!explicitUiSourceAddress.isEmpty()) {
            uiStringCandidate["address"] = explicitUiSourceAddress;
            uiStringCandidate["byteLength"] = args.value("byteLength", 0);
        } else if (!m_controller.m_pendingUiStringCandidates.isEmpty()) {
            uiStringCandidate = m_controller.m_pendingUiStringCandidates.first().toMap();
        } else {
            result["actionStatus"] = "needs_clarification";
            result["message"] = "Je n'ai pas de string UI récente à analyser — lance d'abord trace_ui_string, ou donne-moi directement une adresse.";
            stampIntent(&result);
            return result;
        }

        actionResult = m_controller.analyzeUiStringSources(uiStringCandidate, uiSourceValue, {});
        if (actionResult.value("success").toBool()) {
            const int matches = actionResult.value("matchesReturned").toInt();
            result["workflowStatus"] = "ui_sources_analyzed";
            result["message"] = matches > 0
                ? QString("Analyse des sources : %1 candidat(s) numérique(s) trouvé(s) près de la string.").arg(matches)
                : "Analyse des sources : aucun candidat numérique trouvé près de cette string.";
            if (!actionResult.value("candidates").isNull()) {
                result["uiSourceCandidates"] = actionResult.value("candidates");
            }
        } else {
            result["message"] = actionResult.value("error").toString();
        }
        stampIntent(&result);
        return result;
    } else if (tool == "generate_aob") {
        // PHASE 140 : lecture seule (voir tool_registry.cpp) -- execute
        // directement. Ne patche jamais rien, se contente de generer/qualifier
        // une signature.
        const QString aobAddress = args.value("address").toString().trimmed();
        if (aobAddress.isEmpty()) {
            result["actionStatus"] = "needs_clarification";
            result["message"] = "Il me faut une adresse (0x...) pour générer une signature AOB.";
            stampIntent(&result);
            return result;
        }
        actionResult = m_controller.generateAobSignature(aobAddress, {});
        if (actionResult.value("success").toBool()) {
            result["workflowStatus"] = "aob_signature_generated";
            result["message"] = QString("Signature AOB générée pour %1 (qualité : %2) : %3")
                .arg(aobAddress, actionResult.value("signatureRisk").toString(), actionResult.value("pattern").toString());
        } else {
            result["message"] = actionResult.value("error").toString();
        }
        stampIntent(&result);
        return result;
    } else if (tool == "suggest_patch") {
        // PHASE 140 : lecture seule -- suggere des patchs sans jamais les
        // appliquer (applyCodePatch reste un geste UI/pipe distinct, pas
        // expose comme tool Assistant).
        const QString suggestAddress = args.value("address").toString().trimmed();
        if (suggestAddress.isEmpty()) {
            result["actionStatus"] = "needs_clarification";
            result["message"] = "Il me faut une adresse (0x...) pour suggérer des patchs de code (aucune application automatique).";
            stampIntent(&result);
            return result;
        }
        actionResult = m_controller.suggestCodePatches(suggestAddress, {});
        if (actionResult.value("success").toBool()) {
            const int suggestionCount = actionResult.value("suggestions").toList().size();
            result["workflowStatus"] = "code_patches_suggested";
            result["message"] = suggestionCount > 0
                ? QString("%1 suggestion(s) de patch pour %2 — aucune n'est appliquée, vérifie dans l'onglet AOB/Patch avant d'agir.")
                      .arg(suggestionCount)
                      .arg(suggestAddress)
                : QString("Aucune suggestion de patch trouvée pour %1.").arg(suggestAddress);
        } else {
            result["message"] = actionResult.value("error").toString();
        }
        stampIntent(&result);
        return result;
    } else if (tool == "disassemble_backward") {
        // PHASE 140 : lecture seule (Q_INVOKABLE ... const cote header).
        const QString backwardAddress = args.value("address").toString().trimmed();
        if (backwardAddress.isEmpty()) {
            result["actionStatus"] = "needs_clarification";
            result["message"] = "Il me faut une adresse (0x...) pour désassembler en arrière (lecture seule).";
            stampIntent(&result);
            return result;
        }
        actionResult = m_controller.disassembleBackward(backwardAddress, {});
        if (actionResult.value("success").toBool()) {
            const int candidateCount = actionResult.value("candidateFields").toList().size();
            result["workflowStatus"] = "disassembled_backward";
            result["message"] = QString("Désassemblage en arrière de %1 : %2 champ(s) candidat(s) trouvé(s).")
                .arg(backwardAddress)
                .arg(candidateCount);
        } else {
            result["message"] = actionResult.value("error").toString();
        }
        stampIntent(&result);
        return result;
    } else if (tool == "find_what_writes") {
        // PHASE 140 : jamais d'execution directe depuis le chat. La variante
        // synchrone findWhatWrites() bloque le thread appelant jusqu'a
        // timeoutMs en attendant une ecriture reelle, et suppose que
        // l'utilisateur fait varier la valeur EN DIRECT dans le jeu pendant la
        // fenetre (voir AGENTS.md "Find What Writes"). Un declenchement
        // autonome depuis le chat attacherait un debugger a l'aveugle sans
        // que personne ne varie la valeur -- capture vide au mieux, risque de
        // crash sur une adresse "chaude" au pire (deja reproduit, PHASE 127).
        // Redirige vers l'UI plutot que d'executer, meme patron que
        // trainer_apply_request/trainer_restore_request.
        const QString fwwAddress = args.value("address").toString().trimmed();
        result["actionStatus"] = "requires_confirmation";
        result["requiresConfirmation"] = true;
        result["confirmationReason"] = "Attache un debugger Win32 et nécessite de faire varier la valeur en direct dans le jeu pendant la capture — pas adapté à une exécution autonome depuis le chat.";
        result["message"] = fwwAddress.isEmpty()
            ? "Pour capturer ce qui écrit une adresse, ouvre l'onglet Expert (section Trace UI string), coche « Debugger autorisé », clique « Écrit par » sur la source concernée, puis fais varier la valeur dans le jeu pendant la fenêtre de capture."
            : QString("Pour capturer ce qui écrit %1, ouvre l'onglet Expert (section Trace UI string), coche « Debugger autorisé », clique « Écrit par », puis fais varier la valeur dans le jeu pendant la fenêtre de capture.").arg(fwwAddress);
        stampIntent(&result);
        return result;
    } else if (tool == "test_candidate_fields") {
        // PHASE 140 : jamais d'execution directe depuis le chat. Ecrit une
        // vraie valeur test sur jusqu'a 5 adresses candidates puis tente de
        // restaurer (restauration non garantie en cas d'echec, voir
        // testCandidateFieldsAsync ci-dessous), et tourne ~60s en tache de
        // fond avec resultat livre par signal Qt (candidateFieldTestFinished)
        // -- ne correspond pas au patron requete/reponse en un seul tour de
        // startSmartSearch. Redirige vers l'UI, meme patron que find_what_writes.
        result["actionStatus"] = "requires_confirmation";
        result["requiresConfirmation"] = true;
        result["confirmationReason"] = "Écrit une vraie valeur test sur la cible (restauration non garantie) et tourne environ une minute en tâche de fond — pas adapté à une exécution autonome depuis le chat.";
        result["message"] = "Pour tester automatiquement quels champs candidats tiennent réellement, ouvre l'onglet Expert, section champ affiché/source, après un désassemblage en arrière — le bouton « Tester automatiquement » lance ce test avec un suivi visuel de la progression.";
        stampIntent(&result);
        return result;
    } else if (tool == "write_value" || tool == "freeze_value") {
        // PHASE 120-D (29/08/2026) : meme patron que kernel_write/speedhack_set/
        // block_process_network juste en dessous -- aucune raison technique ne
        // justifiait que ces deux tools, les plus basiques (simple ecriture/
        // freeze), restent un cul-de-sac texte sans recoveryActions alors que
        // des actions plus sensibles (ecriture kernel, injection speedhack)
        // avaient deja ce patron depuis PHASE 148. Ecart d'implementation
        // corrige, pas une nouvelle capacite : le clic reste obligatoire, le
        // vrai modal confirmRiskAction() cote frontend est inchange.
        const bool isFreeze = (tool == "freeze_value");
        const QString writeAddress = args.value("address").toString().trimmed();
        const QString writeValue = args.value("value").toString().trimmed();
        const QString writeValueType = args.value("valueType", "Int32").toString();
        if (writeAddress.isEmpty() || writeValue.isEmpty()) {
            result["actionStatus"] = "needs_clarification";
            result["message"] = isFreeze
                ? "Il me faut l'adresse (0x...) et la valeur à figer pour préparer le freeze."
                : "Il me faut l'adresse (0x...) et la valeur pour préparer l'écriture.";
            stampIntent(&result);
            return result;
        }
        result["actionStatus"] = "requires_confirmation";
        result["requiresConfirmation"] = true;
        result["confirmationReason"] = isFreeze
            ? "Fige cette adresse en mémoire (écriture répétée). Reste actif jusqu'à désactivation explicite."
            : "Cette action modifie la mémoire de la cible attachée.";
        result["message"] = isFreeze
            ? QString("Freeze demandé : %1 (%2) à 0x%3. Confirme pour figer la valeur.").arg(writeValue, writeValueType, writeAddress)
            : QString("Écriture demandée : %1 (%2) à 0x%3. Confirme pour appliquer.").arg(writeValue, writeValueType, writeAddress);
        QVariantList recoveryActions;
        recoveryActions.append(QVariantMap{
            {"id", isFreeze ? "freeze_value_confirm" : "write_value_confirm"},
            {"label", isFreeze ? QString("Figer %1").arg(writeValue) : QString("Écrire %1").arg(writeValue)},
            {"address", writeAddress},
            {"value", writeValue},
            {"valueType", writeValueType},
            {"requiresConfirmation", true},
        });
        result["recoveryActions"] = recoveryActions;
        stampIntent(&result);
        return result;
    } else if (tool == "kernel_write") {
        // Demande explicite ("écris via le kernel") : contrairement à
        // write_value/freeze_value ci-dessus, on propose une action cliquable
        // (recoveryActions) plutôt que de renvoyer vers l'onglet Mémoire —
        // l'écriture kernel n'a pas d'équivalent dans ce panneau usermode, et
        // c'est justement l'action que l'utilisateur vient de demander.
        // Reste toujours un clic de confirmation explicite, jamais automatique.
        const QString kernelAddress = args.value("address").toString().trimmed();
        const QString kernelValue = args.value("value").toString().trimmed();
        const QString kernelValueType = args.value("valueType", "Int32").toString();
        if (kernelAddress.isEmpty() || kernelValue.isEmpty()) {
            result["actionStatus"] = "needs_clarification";
            result["message"] = "Il me faut l'adresse (0x...) et la valeur pour écrire via le driver kernel.";
            stampIntent(&result);
            return result;
        }
        result["actionStatus"] = "requires_confirmation";
        result["requiresConfirmation"] = true;
        result["confirmationReason"] = "Écriture kernel : contourne les protections mémoire usermode (VirtualProtect/PAGE_GUARD). Action irréversible sans lecture préalable de la valeur d'origine.";
        result["message"] = QString("Écriture kernel demandée : %1 (%2) à 0x%3. Confirme pour appliquer via le driver noyau.")
                                 .arg(kernelValue, kernelValueType, kernelAddress);
        QVariantList recoveryActions;
        recoveryActions.append(QVariantMap{
            {"id", "kernel_write_targets"},
            {"label", QString("Écrire %1 via kernel").arg(kernelValue)},
            {"address", kernelAddress},
            {"value", kernelValue},
            {"valueType", kernelValueType},
            {"requiresConfirmation", true},
        });
        result["recoveryActions"] = recoveryActions;
        stampIntent(&result);
        return result;
    } else if (tool == "speedhack_set") {
        // Demande explicite en langage naturel ("ralentis le jeu", "accélère
        // le temps", "remets la vitesse normale") : même patron que
        // kernel_write ci-dessus, une action cliquable plutôt qu'un renvoi
        // vers un onglet — l'Assistant lui-même décide start/setFactor/stop
        // côté frontend selon l'état actuel (executeCheckpointSpeedhack).
        const QString modeArg = args.value("mode", "set").toString().trimmed().toLower();
        const bool isOff = (modeArg == "off" || modeArg == "stop");
        const double factor = args.value("factor", 1.0).toDouble();

        result["actionStatus"] = "requires_confirmation";
        result["requiresConfirmation"] = true;
        result["confirmationReason"] = isOff
            ? "Désactive le speedhack et remet la vitesse perçue à la normale."
            : "Injecte un composant dans le processus cible pour modifier la vitesse perçue du temps.";
        result["message"] = isOff
            ? "Désactivation du speedhack demandée. Confirme pour remettre la vitesse normale."
            : QString("Speedhack demandé : facteur %1x. Confirme pour appliquer.").arg(factor);
        QVariantList recoveryActions;
        recoveryActions.append(QVariantMap{
            {"id", "speedhack_apply"},
            {"label", isOff ? "Désactiver le speedhack" : QString("Appliquer %1x").arg(factor)},
            {"factor", factor},
            {"mode", isOff ? "off" : "set"},
            {"requiresConfirmation", true},
        });
        result["recoveryActions"] = recoveryActions;
        stampIntent(&result);
        return result;
    } else if (tool == "block_process_network") {
        // Meme patron que speedhack_set ci-dessus : action systeme (regle
        // pare-feu + invite UAC), le frontend est seul a detenir
        // confirmRiskAction -- on renvoie une action cliquable plutot que
        // d'appeler blockProcessNetwork()/unblockProcessNetwork() ici.
        const QString modeArg = args.value("mode", "on").toString().trimmed().toLower();
        const bool isOff = (modeArg == "off" || modeArg == "stop" || modeArg == "unblock");

        result["actionStatus"] = "requires_confirmation";
        result["requiresConfirmation"] = true;
        result["confirmationReason"] = isOff
            ? "Retire la règle pare-feu KillEngine posée pour ce processus."
            : "Ajoute une règle pare-feu Windows bloquant tout le trafic entrant/sortant du processus attaché (invite UAC requise).";
        result["message"] = isOff
            ? "Rétablissement du réseau demandé. Confirme pour retirer la règle pare-feu."
            : "Coupure réseau demandée, pour isoler une éventuelle synchro serveur en arrière-plan. Confirme pour appliquer.";
        QVariantList recoveryActions;
        recoveryActions.append(QVariantMap{
            {"id", "network_block_apply"},
            {"label", isOff ? "Rétablir le réseau" : "Couper le réseau"},
            {"mode", isOff ? "off" : "on"},
            {"requiresConfirmation", true},
        });
        result["recoveryActions"] = recoveryActions;
        stampIntent(&result);
        return result;
    } else if (tool == "discover_save_files") {
        const int maxResults = args.value("maxResults", 50).toInt();
        actionResult = m_controller.discoverProcessSaveFiles(maxResults);
        if (actionResult.value("success").toBool()) {
            result["workflowStatus"] = "save_files_discovered";
            result["message"] = QString("Fichiers de sauvegarde : %1 trouvé(s) sous le package '%2'. On peut en lire un avec read_save_file_text ou inspecter LocalSettings avec inspect_local_settings.")
                                  .arg(actionResult.value("count").toInt())
                                  .arg(actionResult.value("familyName").toString());
        }
    } else if (tool == "inspect_local_settings") {
        const int maxValues = args.value("maxValues", 200).toInt();
        actionResult = m_controller.inspectProcessLocalSettings(maxValues);
        if (actionResult.value("success").toBool()) {
            result["workflowStatus"] = "local_settings_inspected";
            result["message"] = QString("LocalSettings inspecté : %1 valeur(s) lue(s) dans settings.dat. Cherche un nom ou une valeur qui correspond à l'affichage du jeu.")
                                  .arg(actionResult.value("count").toInt());
        }
    } else if (tool == "read_save_file_text") {
        const QString path = args.value("path").toString();
        const int maxBytes = args.value("maxBytes", 65536).toInt();
        actionResult = m_controller.readProcessSaveFileText(path, maxBytes);
        if (actionResult.value("success").toBool()) {
            result["workflowStatus"] = "save_file_text_read";
            result["message"] = actionResult.value("truncated").toBool()
                ? "Fichier lu (tronqué à la taille maximale). Cherche le champ correspondant à la valeur affichée dans le texte."
                : "Fichier lu en entier. Cherche le champ correspondant à la valeur affichée dans le texte.";
        }
    } else if (tool == "patch_file_bytes") {
        // PHASE 148 : bug trouve en auditant la ligne schema -- ce cas
        // appelait patchProcessSaveFileBytes() directement malgre
        // requiresConfirmation=true dans le registre (ai/tool_registry.cpp),
        // un vrai contournement RiskGate pour une ecriture disque reelle.
        // Corrige avec le meme patron que find_what_writes/test_candidate_fields
        // (PHASE 146) : jamais d'execution directe depuis le chat, message
        // de redirection uniquement -- pas de recoveryActions cliquable ici
        // (contrairement a kernel_write/speedhack_set/block_process_network
        // ci-dessus) car cette action n'a pas d'equivalent UI existant vers
        // lequel pointer, et inventer un nouvel id de recoveryAction
        // demanderait du cablage frontend hors perimetre de ce chantier.
        const QString patchPath = args.value("path").toString().trimmed();
        const QString patchFindHex = args.value("findHex").toString().trimmed();
        const QString patchReplaceHex = args.value("replaceHex").toString().trimmed();
        result["actionStatus"] = "requires_confirmation";
        result["requiresConfirmation"] = true;
        result["confirmationReason"] = "Édite en place un fichier de sauvegarde réel sur disque — action non réversible automatiquement, pas d'exécution autonome depuis le chat.";
        result["message"] = (patchPath.isEmpty() || patchFindHex.isEmpty() || patchReplaceHex.isEmpty())
            ? "Édition de fichier de sauvegarde demandée, mais il manque le chemin exact et/ou les séquences hex find/replace. Utilise le pipe d'automatisation ou un script Lua avec patchFileBytes une fois la séquence exacte confirmée (read_save_file_text pour vérifier le contexte avant)."
            : QString("Édition de fichier demandée sur %1 (remplace %2 par %3). Pas d'exécution autonome depuis le chat : utilise le pipe d'automatisation ou un script Lua pour l'appliquer une fois sûr de la séquence exacte.")
                  .arg(patchPath, patchFindHex, patchReplaceHex);
        stampIntent(&result);
        return result;
    } else if (tool == "watch_save_file") {
        const QString path = args.value("path").toString();
        QVariantMap watchOptions;
        watchOptions["timeoutMs"] = args.value("timeoutMs", 5000);
        actionResult = m_controller.watchSaveFileForChanges(path, watchOptions);
        if (actionResult.value("success").toBool()) {
            result["workflowStatus"] = "save_file_watch_finished";
            result["message"] = actionResult.value("changed").toBool()
                ? QString("Le fichier a changé (%1) pendant la fenêtre d'observation.").arg(actionResult.value("changeType").toString())
                : "Aucun changement détecté pendant la fenêtre d'observation.";
        }
    // PHASE (couverture chat modèle local, 07/09/2026) : les outils
    // ci-dessous existaient dans ai/tool_registry.cpp (donc visibles du
    // modèle local, PHASE 271-272 -- aucun outil caché du schéma) mais
    // tombaient tous sur "unsupported_tool" faute d'avoir jamais été câblés
    // ici, contrairement au backend Claude (ClaudeChatManager::executeTool,
    // PHASE EXTERNAL-AI-BACKEND T4) qui les couvre déjà tous. Corrigé pour
    // que les deux backends se comportent de façon cohérente : un outil
    // annoncé au modèle doit réellement s'exécuter, jamais échouer
    // silencieusement. Lecture seule : exécution directe. Écriture/action
    // sensible : même patron requires_confirmation + recoveryActions que
    // kernel_write/speedhack_set/block_process_network plus haut -- le clic
    // reste obligatoire, réutilise les fonctions store déjà confirmées.
    } else if (tool == "get_stealth_status") {
        actionResult = m_controller.getStealthStatus();
        actionResult["success"] = true;
        result["message"] = actionResult.value("active").toBool()
            ? QString("Mode discret actif (profil %1).").arg(actionResult.value("profile").toString())
            : "Mode discret inactif.";
    } else if (tool == "get_process_network_modules") {
        actionResult = m_controller.getProcessNetworkModules();
        if (actionResult.value("success").toBool()) {
            result["message"] = QString("%1 module(s) réseau chargé(s) par le processus attaché.").arg(actionResult.value("modules").toList().size());
        }
    } else if (tool == "get_http_proxy_requests") {
        actionResult = m_controller.getHttpProxyRequests();
        if (actionResult.value("success").toBool()) {
            result["message"] = QString("%1 requête(s) HTTP interceptée(s).").arg(actionResult.value("requests").toList().size());
        }
    } else if (tool == "getWebView2InspectorStatus") {
        actionResult = m_controller.getWebView2InspectorStatus();
        if (actionResult.value("success").toBool()) {
            result["message"] = actionResult.value("connected").toBool()
                ? QString("Inspecteur WebView2 connecté (target %1).").arg(actionResult.value("target").toString())
                : "Inspecteur WebView2 non connecté.";
        }
    } else if (tool == "listWebView2CdpTargets") {
        actionResult = m_controller.listWebView2CdpTargets(args.value("browserProcessId").toInt(), {});
        if (actionResult.value("success").toBool()) {
            result["message"] = QString("%1 target(s) CDP WebView2 trouvée(s).").arg(actionResult.value("count").toInt());
        }
    } else if (tool == "disconnectWebView2Inspector") {
        actionResult = m_controller.disconnectWebView2Inspector();
        if (actionResult.value("success").toBool()) {
            result["message"] = "Inspecteur WebView2 déconnecté.";
        }
    } else if (tool == "findWebView2DisplayedValues") {
        actionResult = m_controller.findWebView2DisplayedValues(args.value("value").toString(), {});
        if (actionResult.value("success").toBool()) {
            result["message"] = QString("%1 correspondance(s) trouvée(s) dans le DOM pour %2.").arg(actionResult.value("count").toInt()).arg(args.value("value").toString());
        }
    } else if (tool == "findWebView2DisplayedText") {
        actionResult = m_controller.findWebView2DisplayedText(args.value("text").toString(), {});
        if (actionResult.value("success").toBool()) {
            result["message"] = QString("%1 correspondance(s) trouvée(s) dans le DOM pour ce texte.").arg(actionResult.value("count").toInt());
        }
    } else if (tool == "probeWebView2GlobalScope") {
        actionResult = m_controller.probeWebView2GlobalScope();
        if (actionResult.value("success").toBool()) {
            result["message"] = "Scope JS global (window) sondé — voir l'onglet WebView2 pour le détail.";
        }
    } else if (tool == "get_process_network_connections") {
        // getProcessNetworkConnectionsAsync ne bloque pas le thread GUI (la
        // resolution DNS inverse tourne sur un thread separe cote backend) --
        // le vrai resultat arrive plus tard via processNetworkConnectionsFinished
        // (deja branche vers networkStore dans app.ts). Meme patron que les
        // recoveryActions "en cours..." ci-dessus pour les tools *Async : on
        // renvoie l'accuse de demarrage tout de suite, resultat visible dans
        // le panneau Reseau.
        actionResult = {{"success", true}};
        m_controller.getProcessNetworkConnectionsAsync();
        result["workflowStatus"] = "network_connections_requested";
        result["message"] = "Récupération des connexions réseau en cours (résolution DNS inverse en tâche de fond)...";
        result["recoveryActions"] = QVariantList{QVariantMap{{"id", "open_network"}, {"label", "Ouvrir Réseau"}}};
    } else if (tool == "spoof_dns" || tool == "restore_dns") {
        const bool isRestore = (tool == "restore_dns");
        const QString domain = args.value("domain").toString().trimmed();
        const QString targetIp = args.value("targetIp").toString().trimmed();
        if (domain.isEmpty() || (!isRestore && targetIp.isEmpty())) {
            result["actionStatus"] = "needs_clarification";
            result["message"] = isRestore
                ? "Il me faut le domaine dont l'entrée DNS spoofée doit être retirée."
                : "Il me faut le domaine et l'IP locale cible pour rediriger le DNS.";
            stampIntent(&result);
            return result;
        }
        result["actionStatus"] = "requires_confirmation";
        result["requiresConfirmation"] = true;
        result["confirmationReason"] = isRestore
            ? "Retire l'entrée spoofée du fichier hosts Windows."
            : "Redirige un domaine vers une IP locale via le fichier hosts Windows (invite UAC).";
        result["message"] = isRestore
            ? QString("Restauration DNS demandée pour %1. Confirme pour retirer l'entrée du fichier hosts.").arg(domain)
            : QString("Redirection DNS demandée : %1 -> %2. Confirme pour appliquer.").arg(domain, targetIp);
        result["recoveryActions"] = QVariantList{QVariantMap{
            {"id", "dns_spoof_apply"},
            {"label", isRestore ? QString("Restaurer %1").arg(domain) : QString("Rediriger %1").arg(domain)},
            {"mode", isRestore ? "restore" : "spoof"},
            {"domain", domain},
            {"targetIp", targetIp},
            {"requiresConfirmation", true},
        }};
        stampIntent(&result);
        return result;
    } else if (tool == "start_http_proxy" || tool == "stop_http_proxy") {
        const bool isStop = (tool == "stop_http_proxy");
        const int port = args.value("port", 8080).toInt();
        const bool interceptHttps = args.value("interceptHttps", true).toBool();
        result["actionStatus"] = "requires_confirmation";
        result["requiresConfirmation"] = true;
        result["confirmationReason"] = isStop
            ? "Arrête le proxy HTTP local et retire les hooks du processus attaché."
            : "Intercepte les requêtes HTTP/HTTPS du processus attaché via injection DLL + hook WinINet/WinHTTP.";
        result["message"] = isStop
            ? "Arrêt du proxy HTTP demandé. Confirme pour arrêter."
            : QString("Démarrage du proxy HTTP demandé (port %1%2). Confirme pour appliquer.")
                  .arg(port)
                  .arg(interceptHttps ? ", HTTPS inclus" : "");
        result["recoveryActions"] = QVariantList{QVariantMap{
            {"id", "http_proxy_apply"},
            {"label", isStop ? "Arrêter le proxy HTTP" : QString("Démarrer le proxy HTTP (port %1)").arg(port)},
            {"mode", isStop ? "stop" : "start"},
            {"port", port},
            {"interceptHttps", interceptHttps},
            {"requiresConfirmation", true},
        }};
        stampIntent(&result);
        return result;
    } else if (tool == "modify_http_request") {
        const QString requestId = args.value("requestId").toString().trimmed();
        const QString newBody = args.value("newBody").toString();
        if (requestId.isEmpty()) {
            result["actionStatus"] = "needs_clarification";
            result["message"] = "Il me faut l'identifiant de la requête HTTP interceptée à modifier (voir get_http_proxy_requests).";
            stampIntent(&result);
            return result;
        }
        result["actionStatus"] = "requires_confirmation";
        result["requiresConfirmation"] = true;
        result["confirmationReason"] = "Modifie le body d'une requête HTTP interceptée avant qu'elle ne soit envoyée.";
        result["message"] = QString("Modification demandée pour la requête %1. Confirme pour appliquer.").arg(requestId);
        result["recoveryActions"] = QVariantList{QVariantMap{
            {"id", "modify_http_request_apply"},
            {"label", "Modifier la requête"},
            {"requestId", requestId},
            {"newBody", newBody},
            {"requiresConfirmation", true},
        }};
        stampIntent(&result);
        return result;
    } else if (tool == "set_lag_switch") {
        const bool enabled = args.value("enabled").toBool();
        const int delayMs = args.value("delayMs", 1000).toInt();
        result["actionStatus"] = "requires_confirmation";
        result["requiresConfirmation"] = true;
        result["confirmationReason"] = enabled
            ? "Retarde artificiellement recv/WSARecv du processus attaché via injection DLL + MinHook."
            : "Désactive le lag switch et retire le retard artificiel.";
        result["message"] = enabled
            ? QString("Lag switch demandé : %1 ms de retard. Confirme pour appliquer.").arg(delayMs)
            : "Désactivation du lag switch demandée. Confirme pour appliquer.";
        result["recoveryActions"] = QVariantList{QVariantMap{
            {"id", "lag_switch_apply"},
            {"label", enabled ? QString("Activer le lag switch (%1 ms)").arg(delayMs) : "Désactiver le lag switch"},
            {"enabled", enabled},
            {"delayMs", delayMs},
            {"requiresConfirmation", true},
        }};
        stampIntent(&result);
        return result;
    } else if (tool == "apply_stealth_mode" || tool == "restore_stealth_mode") {
        const bool isRestore = (tool == "restore_stealth_mode");
        const QString profile = args.value("profile", "default").toString();
        result["actionStatus"] = "requires_confirmation";
        result["requiresConfirmation"] = true;
        result["confirmationReason"] = isRestore
            ? "Désactive le mode discret et restaure l'état original des modules activés."
            : "Active le mode discret (anti-debug, masquage process/DLL) pour masquer KillEngine du processus attaché.";
        result["message"] = isRestore
            ? "Désactivation du mode discret demandée. Confirme pour restaurer."
            : QString("Activation du mode discret demandée (profil '%1'). Confirme pour appliquer.").arg(profile);
        result["recoveryActions"] = QVariantList{QVariantMap{
            {"id", "stealth_mode_apply"},
            {"label", isRestore ? "Désactiver le mode discret" : QString("Activer le mode discret (%1)").arg(profile)},
            {"mode", isRestore ? "restore" : "apply"},
            {"profile", profile},
            {"requiresConfirmation", true},
        }};
        stampIntent(&result);
        return result;
    } else if (tool == "connectWebView2Inspector") {
        result["actionStatus"] = "requires_confirmation";
        result["requiresConfirmation"] = true;
        result["confirmationReason"] = "Connecte l'inspecteur WebView2 à une target CDP — s'attache à un process externe.";
        result["message"] = "Connexion à l'inspecteur WebView2 demandée. Confirme pour t'attacher (voir l'onglet WebView2 pour choisir la target).";
        result["recoveryActions"] = QVariantList{QVariantMap{{"id", "open_webview2_inspector"}, {"label", "Ouvrir WebView2"}}};
        stampIntent(&result);
        return result;
    } else if (tool == "evaluateWebView2JavaScript") {
        const QString expression = args.value("expression").toString();
        if (expression.trimmed().isEmpty()) {
            result["actionStatus"] = "needs_clarification";
            result["message"] = "Il me faut l'expression JavaScript à évaluer dans la target WebView2 connectée.";
            stampIntent(&result);
            return result;
        }
        result["actionStatus"] = "requires_confirmation";
        result["requiresConfirmation"] = true;
        result["confirmationReason"] = "Évalue une expression JavaScript arbitraire dans la target WebView2 connectée — peut lire ou modifier l'état JS.";
        result["message"] = QString("Évaluation JS demandée : %1. Confirme pour exécuter.").arg(expression);
        result["recoveryActions"] = QVariantList{QVariantMap{
            {"id", "webview2_evaluate_apply"},
            {"label", "Évaluer le JavaScript"},
            {"expression", expression},
            {"requiresConfirmation", true},
        }};
        stampIntent(&result);
        return result;
    } else {
        result["actionStatus"] = "unsupported_tool";
        result["actionError"] = QString("Outil Smart Search non supporté: %1").arg(tool);
        stampIntent(&result);
        return result;
    }

    result["actionStatus"] = actionResult.value("success").toBool() ? "executed" : "failed";
    result["actionResult"] = actionResult;
    noteAiOutcome(actionResult.value("success").toBool(), QString());

    if (!actionResult.value("success").toBool()) {
        const QString actionError = actionResult.value("error").toString().trimmed();
        result["workflowStatus"] = "action_failed";
        result["error"] = actionError;
        result["message"] = actionError.isEmpty()
            ? QString("L'action %1 a échoué sans détail. Vérifie le processus attaché et le type de valeur.")
                  .arg(tool)
            : QString("Je voulais agir, mais l'action a échoué : %1").arg(actionError);
    } else if (tool == "exact_scan" || tool == "exact_scan_multi_type") {
        m_controller.m_smartSearchInitialValue = args.value("value").toString();
        m_controller.m_smartSearchValueType = args.value("valueType", "Int32").toString();
        if (numbers.size() >= 2) {
            m_controller.m_smartSearchTargetValue = numbers.at(1);
        }

        const auto count = actionResult.value("candidateStoreSize").toULongLong();
        m_controller.m_smartSearchActive = count > 0;
        result["targetValue"] = m_controller.m_smartSearchTargetValue;
        const QString prefix = intent.resetContext
            ? QString("Je repars sur une nouvelle recherche. ")
            : QString();
        const QString typeNote = tool == "exact_scan_multi_type"
            ? QString(" en Auto rapide")
            : QString();
        if (count > 0) {
            result["workflowStatus"] = "awaiting_value_change";
            result["message"] = prefix + QString("J'ai trouvé %1 candidats pour %2%3. Fais bouger la valeur dans le jeu, puis donne-moi la nouvelle valeur pour réduire la liste.")
                                    .arg(count)
                                    .arg(m_controller.m_smartSearchInitialValue)
                                    .arg(typeNote);
        } else {
            // Scan direct a vide : proposer tout de suite les strategies de
            // repli plutot que redemander une variation sur une recherche
            // qui n'a encore trouve aucun candidat.
            result["workflowStatus"] = "no_candidate";
            result["message"] = prefix + QString("Aucun candidat pour %1%2 en scan direct. Ce n'est pas forcement une impasse : ça peut être une valeur chiffrée/obfusquée, une valeur affichée en texte plutôt qu'en mémoire brute, ou une valeur qui varie déjà.")
                                    .arg(m_controller.m_smartSearchInitialValue)
                                    .arg(typeNote);
            QVariantList recoveryActions;
            recoveryActions.append(QVariantMap{{"id", "try_encrypted_scan"}, {"label", "Scan chiffré (XOR)"}, {"value", m_controller.m_smartSearchInitialValue}});
            recoveryActions.append(QVariantMap{{"id", "trace_ui_string"}, {"label", "Chercher le texte affiché"}, {"value", m_controller.m_smartSearchInitialValue}});
            recoveryActions.append(QVariantMap{{"id", "try_unknown_changed"}, {"label", "Unknown (valeur inconnue)"}});
            recoveryActions.append(QVariantMap{{"id", "new_search"}, {"label", "Nouvelle recherche"}});
            result["recoveryActions"] = recoveryActions;
        }
    } else if (tool == "next_scan" || tool == "unknown_compare") {
        const auto remaining = tool == "next_scan"
                                   ? actionResult.value("remaining").toULongLong()
                                   : actionResult.value("stored").toULongLong();
        result["targetValue"] = m_controller.m_smartSearchTargetValue;

        const bool withinAutoWriteRange = remaining >= 1 && remaining <= kAutoWriteCandidateLimit;

        if (withinAutoWriteRange && m_controller.m_smartSearchTargetValue.isEmpty()) {
            // Chemins comme Trace UI string ou un ExactScan a un seul nombre
            // ne definissent jamais m_controller.m_smartSearchTargetValue (seul GuidedScan,
            // "X que je veux a Y", le fait). Sans ce cas, un utilisateur qui
            // reduit correctement a 1-4 candidats se retrouvait bloque en
            // boucle infinie sur "trop de candidats, raffine encore" (faux :
            // 3 candidats n'est PAS trop, il manque juste la valeur a ecrire).
            m_controller.m_pendingRecoveryAction = "write_target_value";
            result["workflowStatus"] = "awaiting_new_value";
            result["message"] = QString(
                "Il reste %1 candidat(s), c'est peu — mais je ne sais pas encore quelle valeur écrire. Donne-moi la "
                "valeur que tu veux mettre (juste le nombre, ex: 3000).")
                .arg(remaining);
        } else if (withinAutoWriteRange && !m_controller.m_smartSearchTargetValue.isEmpty()) {
            auto suggestions = suggestedWritesForCandidates(
                candidates,
                m_controller.m_smartSearchTargetValue,
                kAutoWriteCandidateLimit);
            m_controller.enrichSuggestedWritesWithHistory(&suggestions);

            const QString gameKey = autoResolverGameKey(m_controller.processName());
            const int cleanCandidateCount = flagNoisyCandidates(
                &suggestions, gameKey, m_controller.m_smartSearchInitialValue, m_controller.m_smartSearchTargetValue);
            // Si au moins une adresse n'est jamais apparue ailleurs, on ecarte
            // celles deja vues sur une recherche sans rapport plutot que
            // d'ecrire dessus a l'aveugle. Si TOUTES sont suspectes, on ecrit
            // quand meme (rien de mieux a proposer) mais le message et
            // confidenceReason portent deja l'avertissement.
            const bool allSuggestionsNoisy = cleanCandidateCount == 0 && !suggestions.isEmpty();
            if (cleanCandidateCount > 0 && cleanCandidateCount < suggestions.size()) {
                QVariantList cleanOnly;
                for (const auto& item : suggestions) {
                    if (!item.toMap().contains("noisyHistoryHits")) {
                        cleanOnly.append(item);
                    }
                }
                suggestions = cleanOnly;
            }

            QVariantList rejectedSuggestions;
            const auto writeSuggestions = m_controller.filterAutoWriteSuggestionsByRegion(suggestions, &rejectedSuggestions);
            QVariantList writeResults;
            bool allWritesOk = true;
            m_controller.m_lastBatchStartIndex = m_controller.m_writeHistory.size();
            m_controller.m_lastAutoWriteTargets.clear();

            for (const auto& item : writeSuggestions) {
                const auto suggestion = item.toMap();
                auto writeResult = m_controller.writeMemoryValueConfirmed(
                    suggestion.value("address").toString(),
                    suggestion.value("type").toString(),
                    suggestion.value("value").toString());
                // Enrichit le résultat avec l'adresse/valeur pour l'affichage UI.
                writeResult.insert("address", suggestion.value("address"));
                writeResult.insert("value", suggestion.value("value"));
                writeResult.insert("type", suggestion.value("type"));
                if (suggestion.contains("valueHistory")) {
                    writeResult.insert("valueHistory", suggestion.value("valueHistory"));
                }
                allWritesOk = allWritesOk && writeResult.value("success").toBool();
                writeResults.append(writeResult);
                if (writeResult.value("success").toBool()) {
                    uint64_t address = 0;
                    killcore::ValueType type;
                    if (parseHexAddress(suggestion.value("address").toString(), &address)
                        && killcore::parseValueType(suggestion.value("type").toString(), &type)) {
                        m_controller.m_lastAutoWriteTargets.append({address, type});
                    }
                }
            }

            // Memorise ce lot dans l'historique inter-sessions, que l'ecriture
            // ait reussi ou non : meme une tentative sur une mauvaise adresse
            // sert a la reperer comme suspecte la prochaine fois.
            QVariantList historyEntries;
            for (const auto& item : writeSuggestions) {
                const auto suggestion = item.toMap();
                historyEntries.append(QVariantMap{
                    {"address", suggestion.value("address")},
                    {"type", suggestion.value("type")},
                    {"initialValue", m_controller.m_smartSearchInitialValue},
                    {"targetValue", m_controller.m_smartSearchTargetValue},
                    {"timestamp", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                });
            }
            appendCandidateHistory(gameKey, historyEntries);

            m_controller.m_lastBatchEndIndex = m_controller.m_writeHistory.size();
            if (m_controller.m_lastBatchEndIndex == m_controller.m_lastBatchStartIndex) {
                m_controller.m_lastBatchStartIndex = -1;
                m_controller.m_lastBatchEndIndex = -1;
                m_controller.m_lastAutoWriteTargets.clear();
            }
            if (allWritesOk && !m_controller.m_lastAutoWriteTargets.isEmpty()) {
                m_controller.m_smartSearchActive = false;
                m_controller.m_chatMemoryTargets = m_controller.m_lastAutoWriteTargets;
                resetFailureEscalationState();
                m_controller.m_autoWriteValueHistory.clear();
                appendDistinctText(&m_controller.m_autoWriteValueHistory, m_controller.m_smartSearchInitialValue, 12);
                appendDistinctText(&m_controller.m_autoWriteValueHistory, m_controller.m_smartSearchTargetValue, 12);
            }

            result["workflowStatus"] = allWritesOk ? "auto_write_done" : "auto_write_partial_or_failed";
            result["suggestedWrites"] = suggestions;
            result["suggestedWrite"] = suggestions.isEmpty() ? QVariantMap{} : suggestions.first().toMap();
            result["filteredWriteCandidates"] = rejectedSuggestions;
            result["autoWriteResults"] = writeResults;
            result["autoWriteResult"] = writeResults.isEmpty() ? QVariantMap{} : writeResults.last().toMap();
            result["autoWriteCount"] = writeResults.size();
            result["activeTargetCount"] = m_controller.m_chatMemoryTargets.size();
            result["previousTargetValue"] = m_controller.m_smartSearchInitialValue;
            result["writeHistory"] = writeHistoryToVariantList(m_controller.m_autoWriteValueHistory);
            result["rollbackNote"] = "Tu peux annuler toutes les écritures via le bouton rollback batch dans l'assistant.";
            if (writeSuggestions.isEmpty()) {
                result["workflowStatus"] = "auto_write_partial_or_failed";
                result["message"] = QString("Il reste %1 candidat(s), mais le filtre anti-bruit n'a gardé aucune adresse assez fiable pour une écriture automatique.")
                                        .arg(remaining);
            } else if (allSuggestionsNoisy) {
                result["message"] = QString(
                    "Il reste %1 candidat(s), mais %2 sont déjà apparues comme fiables sur une recherche différente et "
                    "sans rapport avant — probablement du bruit (un compteur interne, pas la vraie donnée). J'ai quand "
                    "même écrit %3 faute de meilleure piste : vérifie particulièrement bien si ça a marché.")
                    .arg(remaining)
                    .arg(writeSuggestions.size())
                    .arg(m_controller.m_smartSearchTargetValue);
            } else {
                result["message"] = allWritesOk
                                    ? QString("Il reste %1 candidat(s). J'ai écrit automatiquement %2 sur les adresses finales fiables. Je garde ces adresses actives pour les prochaines modifications.")
                                          .arg(remaining)
                                          .arg(m_controller.m_smartSearchTargetValue)
                                    : QString("Il reste %1 candidat(s), mais au moins une écriture automatique a échoué.")
                                          .arg(remaining);
            }
        } else if (remaining > 1) {
            result["workflowStatus"] = "needs_more_refinement";
            result["message"] = QString("Il reste %1 candidats. Refais varier le score, puis indique-moi la nouvelle valeur.")
                                    .arg(remaining);
        } else {
            result["workflowStatus"] = "no_candidate";
            const QString diagnostic = actionResult.value("diagnostic").toString();
            const QString observedValue = args.value("value").toString().trimmed();
            const QString retryValue = observedValue.isEmpty() ? m_controller.m_smartSearchInitialValue : observedValue;
            QVariantList recoveryActions;
            recoveryActions.append(QVariantMap{{"id", "undo_reduction"}, {"label", "Restaurer les candidats"}});
            recoveryActions.append(QVariantMap{{"id", "try_changed"}, {"label", "Essayer changed"}});
            recoveryActions.append(QVariantMap{{"id", "try_increased"}, {"label", "Essayer increased"}});
            recoveryActions.append(QVariantMap{{"id", "retry_float32"}, {"label", "Rechercher en Float32"}, {"value", retryValue}, {"target", m_controller.m_smartSearchTargetValue}});
            recoveryActions.append(QVariantMap{{"id", "retry_int64"}, {"label", "Rechercher en Int64"}, {"value", retryValue}, {"target", m_controller.m_smartSearchTargetValue}});
            recoveryActions.append(QVariantMap{{"id", "retry_int32_x100"}, {"label", "Rechercher valeur x100"}, {"value", retryValue}, {"target", m_controller.m_smartSearchTargetValue}});
            recoveryActions.append(QVariantMap{{"id", "try_unknown_increased"}, {"label", "Unknown + increased"}});
            recoveryActions.append(QVariantMap{{"id", "new_search"}, {"label", "Nouvelle recherche"}});
            result["diagnostic"] = diagnostic;
            result["observedValue"] = retryValue;
            result["recoveryActions"] = recoveryActions;
            result["message"] = diagnostic.isEmpty()
                ? QString("Aucun candidat restant. Restaure les candidats précédents, puis essaie changed/increased ou une autre représentation.")
                : QString("Aucun candidat restant. %1").arg(diagnostic);
        }
    }

    stampIntent(&result);
    if (result.value("message").toString().trimmed().isEmpty()
        && result.value("error").toString().trimmed().isEmpty()
        && result.value("actionStatus").toString().trimmed().isEmpty()) {
        result["actionStatus"] = "needs_clarification";
        result["message"] = "Je garde le contexte actuel. Donne-moi une valeur à chercher, une nouvelle valeur observée, ou une adresse mémoire à utiliser.";
    }

    m_controller.appendSmartSearchDebug("smart_search_result", result);
    return result;
}

} // namespace killengine
