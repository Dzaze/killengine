#include "query_text_utils.h"

#include <QRegularExpression>
#include <QSet>

namespace killai {

QString firstDecimalOutsideHex(QString text) {
    text.replace(QRegularExpression(R"(0x[0-9a-fA-F]+)"), " ");
    const QRegularExpression re(R"([-+]?\d+(?:[\.,]\d+)?)");
    const auto match = re.match(text);
    return match.hasMatch() ? match.captured(0).replace(',', '.') : QString();
}

QString firstHexAddressIn(const QString& text) {
    const QRegularExpression re(R"(0x[0-9a-fA-F]+)");
    const auto match = re.match(text);
    return match.hasMatch() ? match.captured(0) : QString();
}

bool looksLikePureSocialQuery(
    const QString& query,
    const QStringList& knownNumbers,
    const QStringList& knownHexAddresses) {
    if (!knownNumbers.isEmpty() || !knownHexAddresses.isEmpty()) {
        return false;
    }

    QString q = query.toLower().trimmed();
    q.replace(QRegularExpression(R"([!?.;,:\-_/\\()\[\]{}"'`]+)"), " ");
    q = q.simplified();
    if (q.isEmpty()) {
        return false;
    }

    static const QSet<QString> kSocialOnlyPhrases = {
        "salut", "bonjour", "bonsoir", "coucou", "hello", "hi", "hey", "yo",
        "merci", "merci beaucoup", "thanks", "thank you", "ok merci",
        "salut merci", "bonjour merci", "salut mon pote", "merci mon pote",
        "ca va", "ça va"
    };
    return kSocialOnlyPhrases.contains(q);
}

bool looksLikePureSocialQuery(const QString& query) {
    const QString number = firstDecimalOutsideHex(query);
    const QString address = firstHexAddressIn(query);
    return looksLikePureSocialQuery(
        query,
        number.isEmpty() ? QStringList{} : QStringList{number},
        address.isEmpty() ? QStringList{} : QStringList{address});
}

bool wantsTrainerQuery(const QString& query) {
    const QString q = query.toLower();
    return q.contains("trainer") || q.contains("cheat table")
        || q.contains("feature trainer") || q.contains("fonction trainer");
}

bool wantsFieldStabilityQuery(const QString& query) {
    const QString q = query.toLower();
    return q.contains("champ affiché") || q.contains("champ affiche")
        || q.contains("valeur affichée") || q.contains("valeur affichee")
        || q.contains("affichage dérivé") || q.contains("affichage derive")
        || q.contains("vraie source") || q.contains("source événementielle")
        || q.contains("source evenementielle") || q.contains("displayed field")
        || q.contains("display field") || q.contains("derived display")
        || q.contains("real source") || q.contains("field stability")
        || q.contains("stabilité du champ") || q.contains("stabilite du champ")
        || q.contains("stabilité de cette adresse") || q.contains("stabilite de cette adresse");
}

bool wantsUiSourcesQuery(const QString& query) {
    const QString q = query.toLower();
    return q.contains("analyse les sources") || q.contains("analyser les sources")
        || q.contains("analyse la source") || q.contains("sources numériques")
        || q.contains("sources numeriques") || q.contains("analyze sources")
        || q.contains("analyze the sources") || q.contains("numeric sources")
        || q.contains("numeric source");
}

bool wantsGenerateAobQuery(const QString& query) {
    const QString q = query.toLower();
    return q.contains("signature aob") || q.contains("aob signature")
        || q.contains("génère une signature") || q.contains("genere une signature")
        || q.contains("generate aob") || q.contains("generate a signature")
        || q.contains("generate signature");
}

bool wantsSuggestPatchQuery(const QString& query) {
    const QString q = query.toLower();
    return q.contains("suggère un patch") || q.contains("suggere un patch")
        || q.contains("suggestion de patch") || q.contains("suggest a patch")
        || q.contains("suggest patch") || q.contains("patch suggestions");
}

bool wantsDisassembleBackwardQuery(const QString& query) {
    const QString q = query.toLower();
    return q.contains("désassemble en arrière") || q.contains("desassemble en arriere")
        || q.contains("désassemblage arrière") || q.contains("desassemblage arriere")
        || q.contains("disassemble backward") || q.contains("champs sources")
        || q.contains("champ source de") || q.contains("source fields");
}

bool wantsFindWhatWritesQuery(const QString& query) {
    const QString q = query.toLower();
    return q.contains("capture ce qui écrit") || q.contains("capture ce qui ecrit")
        || q.contains("qu'est-ce qui écrit") || q.contains("qu'est-ce qui ecrit")
        || q.contains("qui écrit cette adresse") || q.contains("qui ecrit cette adresse")
        || q.contains("find what writes") || q.contains("what writes to");
}

bool wantsTestCandidateFieldsQuery(const QString& query) {
    const QString q = query.toLower();
    return q.contains("teste les champs candidats") || q.contains("test candidate fields")
        || q.contains("teste automatiquement") || q.contains("test the candidate fields")
        || q.contains("vérifie quel champ tient") || q.contains("verifie quel champ tient");
}

bool wantsAobOrPatchWorkflowQuery(const QString& query) {
    return wantsGenerateAobQuery(query) || wantsSuggestPatchQuery(query) || wantsDisassembleBackwardQuery(query);
}

bool wantsFindWhatWritesOrTestFieldsQuery(const QString& query) {
    return wantsFindWhatWritesQuery(query) || wantsTestCandidateFieldsQuery(query);
}

QString investigationPlaybookTopic(const QString& query) {
    const QString q = query.toLower();
    const bool hasDecimal = !firstDecimalOutsideHex(query).isEmpty();
    const bool hasAddress = !firstHexAddressIn(query).isEmpty();
    const bool directReadOnlyTool =
        wantsTrainerQuery(query)
        || wantsFieldStabilityQuery(query)
        || wantsUiSourcesQuery(query)
        || wantsGenerateAobQuery(query)
        || wantsSuggestPatchQuery(query)
        || wantsDisassembleBackwardQuery(query)
        || wantsFindWhatWritesQuery(query)
        || wantsTestCandidateFieldsQuery(query);

    if (!hasDecimal
        && (q.contains("valeur simple") || q.contains("simple value")
            || ((q.contains("comment") || q.contains("quoi faire") || q.contains("par ou commencer"))
                && (q.contains("chercher") || q.contains("scan"))))) {
        return "simple_visible_value";
    }

    if (!hasDecimal
        && (q.contains("ne trouve pas") || q.contains("trouve pas") || q.contains("introuvable")
            || q.contains("not found") || q.contains("cannot find") || q.contains("can't find"))
        && (q.contains("valeur affich") || q.contains("displayed value") || q.contains("valeur a l'ecran")
            || q.contains("valeur à l'écran"))) {
        return "displayed_value_not_found";
    }

    if ((q.contains("freeze") || q.contains("gel") || q.contains("fige"))
        && (q.contains("clignote") || q.contains("ne tient pas") || q.contains("tient pas")
            || q.contains("flicker") || q.contains("does not hold") || q.contains("doesn't hold"))) {
        return "freeze_flickers";
    }

    if ((q.contains("adresse") || q.contains("address") || hasAddress)
        && (q.contains("redémarrage") || q.contains("redemarrage") || q.contains("relance")
            || q.contains("restart") || q.contains("relaunch") || q.contains("survit pas")
            || q.contains("ne survit pas") || q.contains("does not survive") || q.contains("doesn't survive"))) {
        return "unstable_address";
    }

    if (!directReadOnlyTool
        && (q.contains("patcher") || q.contains("patch le code") || q.contains("modifier le code")
            || q.contains("nop") || q.contains("forcer un saut") || q.contains("force jump"))) {
        return "code_patch_request";
    }

    if (!wantsFindWhatWritesQuery(query)
        && (q.contains("qui écrit") || q.contains("qui ecrit") || q.contains("what writes")
            || q.contains("origine de l'écriture") || q.contains("origine de l'ecriture"))) {
        return "what_writes_value";
    }

    if (!directReadOnlyTool
        && (q.contains("fichier de sauvegarde") || q.contains("save file") || q.contains("localsettings")
            || q.contains("local settings") || q.contains("registre uwp"))
        && (q.contains("valeur") || q.contains("value") || q.contains("peut etre")
            || q.contains("peut-être") || q.contains("vit dans"))) {
        return "save_file_or_uwp";
    }

    return {};
}

bool wantsInvestigationPlaybookQuery(const QString& query) {
    return !investigationPlaybookTopic(query).isEmpty();
}

} // namespace killai
