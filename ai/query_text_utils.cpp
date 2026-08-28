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

} // namespace killai
