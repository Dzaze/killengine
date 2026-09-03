#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace killcore {

/// Live Lua REPL (PROPOSITIONS-1 #4) — logique pure du protocole
/// stdin/stdout entre `LuaReplManager` (apps/desktop) et
/// `scripts/killengine_repl_driver.lua` : aucun accès process, testable
/// sans lua.exe réel.

struct ReplExtraction {
    bool found{false};      ///< false si le marqueur sentinelle n'est pas (encore) dans `buffer`.
    QString output;         ///< Sortie de la commande courante (sentinelle exclue), UTF-8 décodé.
    QByteArray remaining;   ///< Octets déjà reçus après la sentinelle (début de la prochaine réponse).
};

/// Cherche `sentinel` dans `buffer` et, si présent, sépare la sortie de la
/// commande courante (tout ce qui précède) du reliquat déjà reçu pour la
/// commande suivante (tout ce qui suit). `found=false` signifie "pas encore
/// assez de données" — le driver n'a pas fini d'écrire, il faut relire.
ReplExtraction extractReplOutput(const QByteArray& buffer, const QByteArray& sentinel);

/// Extrait les noms de fonctions "ke.<nom>" déclarées dans le texte du
/// helper Lua (scripts/killengine.lua) — motif `function ke.<nom>(`, même
/// convention que toutes les fonctions du wrapper. Retourne les noms AVEC
/// le préfixe "ke." (directement utilisables), triés, dédupliqués, filtrés
/// par `prefix` (insensible à la casse ; vide = tout). Utilisé pour
/// l'autocomplétion REPL — reconstruit la liste depuis le fichier source
/// plutôt qu'une liste codée en dur, pour rester à jour automatiquement
/// quand `scripts/killengine.lua` gagne de nouveaux wrappers.
QStringList extractKeCompletions(const QString& helperSource, const QString& prefix = QString());

} // namespace killcore
