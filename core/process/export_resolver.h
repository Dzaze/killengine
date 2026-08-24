#pragma once

#include "process/process_handle.h"

#include <QString>
#include <QStringList>
#include <cstdint>

namespace killcore {

/**
 * @brief Résout une adresse absolue distante à partir d'un nom de module et de
 *        fonction, en parcourant la table d'export PE lue directement dans la
 *        mémoire du process cible (pas le fichier sur disque : le mapping en
 *        mémoire diffère du fichier, les RVA doivent être résolus contre
 *        l'image réellement mappée).
 *
 * Contrairement à getRemoteProcAddress() (core/inject/dll_injector.cpp), qui
 * suppose que le module est une DLL système chargée à la même base locale et
 * distante (vrai uniquement pour ntdll/kernel32 et assimilés sur une même
 * session Windows), cette fonction marche pour n'importe quel module du
 * process cible — y compris son propre exécutable — au prix d'un parcours PE
 * distant via ReadProcessMemory.
 *
 * @param process Handle du process cible (doit être ouvert avec au moins VM_READ).
 * @param moduleName Nom du module, avec ou sans extension (.exe/.dll), insensible à la casse.
 * @param functionName Nom exact (sensible à la casse) de la fonction exportée.
 * @param[out] address Adresse absolue résolue.
 * @param[out] error Message d'erreur en cas d'échec (ex. forwarder non résolu, module introuvable).
 * @return true si la résolution a réussi.
 */
bool resolveRemoteExportAddress(
    const ProcessHandle& process,
    const QString& moduleName,
    const QString& functionName,
    uint64_t* address,
    QString* error);

/**
 * @brief Liste les noms exportés d'un module chargé dans le process cible, en
 *        parcourant la même table d'export PE que resolveRemoteExportAddress
 *        (voir sa doc pour le "pourquoi lire la mémoire distante et pas le
 *        fichier disque"). Sert à découvrir les noms d'export réels d'une
 *        DLL sans deviner à l'aveugle nom par nom.
 *
 * @param filterSubstring Optionnel, insensible à la casse : ne garde que les
 *        noms contenant cette sous-chaîne (vide = tout retourner).
 * @param maxNames Borne dure sur le nombre de noms retournés (module système
 *        peut exporter plusieurs milliers de symboles).
 */
bool listRemoteExportNames(
    const ProcessHandle& process,
    const QString& moduleName,
    const QString& filterSubstring,
    int maxNames,
    QStringList* names,
    QString* error);

} // namespace killcore
