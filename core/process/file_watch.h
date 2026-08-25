#pragma once

#include <QString>

namespace killcore {

class CancellationToken;

struct FileWatchOutcome {
    bool changed = false;
    bool cancelled = false;
    QString changeType;   ///< "modified" / "renamed" / "removed" / "added" / "" si rien
    QString error;
};

/**
 * @brief Surveille UN fichier précis (pas un dossier entier) pour une écriture/
 *        suppression/renommage, via ReadDirectoryChangesW (Win32) sur son dossier
 *        parent — l'API Windows ne permet pas de surveiller un fichier isolé
 *        directement, seulement un répertoire, donc on filtre les notifications
 *        sur le nom du fichier ciblé.
 *
 *        Remplace l'usage manuel de Process Monitor fait en PHASE 90 (docs/
 *        PHASE_TRACKER.md, investigation Solitaire "Bulles") — Procmon est un outil
 *        externe téléchargé à l'exécution, pas adapté à une capacité livrée dans le
 *        produit ; ce mécanisme est natif, sans dépendance externe.
 *
 *        Bloquant, jusqu'à `timeoutMs` ou jusqu'à ce que `cancellation` soit annulé
 *        (vérifié périodiquement pendant l'attente, même logique que les autres
 *        captures bloquantes du projet, ex. `findWhatWrites`).
 *
 * @param path Chemin absolu du fichier à surveiller (doit exister au moment de l'appel
 *        pour qu'on puisse résoudre son dossier parent).
 * @param timeoutMs Durée maximale d'attente en millisecondes.
 * @param cancellation Optionnel — permet une annulation externe pendant l'attente.
 * @param outcome Résultat : `changed=false` sans erreur si le timeout est atteint sans
 *        notification pertinente (ce n'est pas un échec, juste "rien ne s'est passé").
 * @return false uniquement en cas d'erreur d'installation de la surveillance
 *        (chemin invalide, dossier introuvable, échec API Win32) — pas en cas de timeout.
 */
bool watchFileForChanges(
    const QString& path,
    int timeoutMs,
    CancellationToken* cancellation,
    FileWatchOutcome* outcome,
    QString* error);

} // namespace killcore
