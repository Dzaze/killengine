#pragma once

#include "process/process_handle.h"

#include <QString>
#include <QVector>
#include <cstdint>

namespace killcore {

struct PackageSaveFileEntry {
    QString path;
    qint64 sizeBytes = 0;
    QString lastWriteTimeIso;
};

struct PackageLocalSettingsEntry {
    QString keyPath;
    QString name;
    QString type;
    QString preview;
    qint64 dataSizeBytes = 0;
};

/**
 * @brief Résout le "package family name" (ex: Microsoft.MicrosoftSolitaireCollection_8wekyb3d8bbwe)
 *        d'un processus UWP/AppContainer attaché, via GetPackageFamilyName (appmodel.h).
 *        C'est le nom de dossier utilisé sous %LOCALAPPDATA%\Packages\ pour stocker les
 *        fichiers de sauvegarde/état de l'application (LocalState, RoamingState, etc.) —
 *        voir docs/PHASE_TRACKER.md PHASE 90 (investigation Solitaire "Bulles",
 *        24-25/08/2026) où cette zone a été explorée manuellement avant d'en faire un outil.
 *
 * @return false avec un message explicite si le process n'est pas un package UWP
 *         (process Win32 classique — GetPackageFamilyName renvoie APPMODEL_ERROR_NO_PACKAGE).
 */
bool resolvePackageFamilyName(const ProcessHandle& process, QString* familyName, QString* error);

/**
 * @brief Liste les fichiers sous %LOCALAPPDATA%\Packages\<familyName>\ triés par date de
 *        modification la plus récente d'abord — pour repérer un fichier de sauvegarde
 *        pertinent sans avoir à deviner son nom/chemin.
 *
 * @param excludeNoise Si vrai (recommandé), exclut les sous-dossiers volumineux et
 *        rarement pertinents pour une investigation de valeur de jeu (cache navigateur
 *        embarqué EBWebView, dossiers Cache génériques, archives de téléchargement) —
 *        appris lors de PHASE 90 où ces dossiers polluaient une recherche naïve avec des
 *        dizaines de milliers de fichiers sans rapport.
 * @param maxResults Borne dure sur le nombre de fichiers retournés (défaut 50 si <= 0).
 */
bool listPackageSaveFiles(
    const QString& familyName,
    int maxResults,
    bool excludeNoise,
    QVector<PackageSaveFileEntry>* files,
    QString* error);

/**
 * @brief Lit le contenu d'un fichier de sauvegarde découvert via listPackageSaveFiles,
 *        décodé best-effort en texte imprimable (borné en taille pour rester utilisable
 *        dans une réponse IA). N'échoue pas sur du contenu binaire : les octets non
 *        imprimables sont remplacés par '.', comme fait manuellement en PHASE 90 — les
 *        fichiers .sgi sont un conteneur binaire enveloppant un blob JSON directement
 *        lisible une fois décodé ainsi.
 *
 * @param maxBytes Borne dure sur le nombre d'octets lus (défaut 65536 si <= 0).
 */
bool readPackageSaveFileText(
    const QString& path,
    int maxBytes,
    QString* text,
    bool* truncated,
    QString* error);

/**
 * @brief Ouvre en lecture seule la ruche UWP
 *        %LOCALAPPDATA%\Packages\<familyName>\Settings\settings.dat avec
 *        RegLoadAppKeyW, puis enumere les valeurs bornées sous forme lisible.
 *
 * Ne monte pas la ruche dans HKCU/HKLM et ne donne aucune primitive d'ecriture
 * registre. maxValues vaut 200 par defaut si <= 0.
 */
bool inspectPackageLocalSettings(
    const QString& familyName,
    int maxValues,
    QVector<PackageLocalSettingsEntry>* entries,
    QString* settingsPath,
    QString* error);

bool patchPackageSaveFileBytes(
    const QString& path,
    const QString& findHex,
    const QString& replaceHex,
    QString* error,
    int* occurrencesFound = nullptr);

} // namespace killcore
