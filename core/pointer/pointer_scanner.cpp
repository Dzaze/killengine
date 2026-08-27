#include "pointer_scanner.h"

#include "memory/memory_map.h"
#include "memory/memory_reader.h"
#include "process/process_enumerator.h"
#include "logging/logger.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace killcore {

namespace {

const ProcessModuleInfo* findModuleForAddress(
    const QList<ProcessModuleInfo>& modules,
    uint64_t address) {
    for (const auto& mod : modules) {
        if (address >= mod.baseAddress && address < mod.baseAddress + mod.size) {
            return &mod;
        }
    }
    return nullptr;
}

bool isAllowedBaseModule(
    const QList<ProcessModuleInfo>& modules,
    uint64_t address,
    const QStringList& baseModules) {
    const ProcessModuleInfo* mod = findModuleForAddress(modules, address);
    if (!mod) return false;
    if (baseModules.isEmpty()) return true;
    for (const auto& allowed : baseModules) {
        if (mod->name.compare(allowed, Qt::CaseInsensitive) == 0) return true;
    }
    return false;
}

/// Résultat d'un niveau de scan : pour chaque pointeur trouvé, on conserve
/// l'offset ET la cible exacte pointée (nécessaire pour reconstruire les chemins).
struct LevelHit {
    uint64_t pointerLocation{0}; ///< Où se trouve le pointeur dans la mémoire.
    uint64_t offset{0};          ///< Offset entre la valeur pointée et la cible.
    uint64_t pointedTarget{0};   ///< Cible exacte pointée (permet le matching multi-niveau).
};

/// Étape de scan d'un niveau : pour chaque cible, trouve tous les pointeurs
/// (alignés) qui pointent vers [target, target + maxOffset].
/// Retourne les hits avec la cible exacte pour reconstruction des chemins.
QList<LevelHit> scanLevel(
    const ProcessHandle& handle,
    const std::unordered_set<uint64_t>& targets,
    const PointerScanOptions& options,
    PointerScanProgress* progress,
    bool* cancelled,
    bool* partial,
    size_t maxLevelResults) {

    QList<LevelHit> results;
    if (targets.empty()) return results;

    auto regions = MemoryMap::snapshot(handle);
    MemoryReader reader(handle);

    // Bug trouve en investigant PHASE 162/163 (voir docs/PHASE_TRACKER.md) :
    // les regions sont visitees dans l'ordre naturel de VirtualQueryEx (adresses
    // croissantes), donc le tas (adresses generalement basses) est scanne AVANT
    // l'image des modules (adresses generalement hautes). Avec onlyModuleBase=true,
    // seul un hit dont le pointerLocation tombe dans un module compte -- mais le
    // plafond maxLevelResults (ci-dessous) compte TOUT hit, y compris ceux situes
    // dans le tas qui seront de toute facon rejetes plus loin. Sur un tas dense
    // (ex: objet realloue plusieurs fois, cluster LFH), le nombre de correspondances
    // fortuites dans le tas (a portee de maxOffset de la cible) peut a lui seul
    // atteindre le plafond avant que le scan n'atteigne jamais la region module qui
    // contient la vraie reponse -- reproduit et confirme en direct (0 chaine trouvee
    // alors qu'une lecture directe montre que le pointeur existe bien). Corrige en
    // priorisant les regions de type Image (modules charges) en premier quand
    // onlyModuleBase est actif : les hits utiles sont alors trouves avant que le
    // bruit du tas n'epuise le plafond.
    if (options.onlyModuleBase) {
        std::stable_partition(regions.begin(), regions.end(), [](const MemoryRegion& region) {
            return region.type == MemoryType::Image;
        });
    }

    for (const auto& region : regions) {
        if (*cancelled) break;
        if (!region.readable || region.guarded) continue;
        if (region.size < sizeof(uint64_t)) continue;

        uint64_t offset = 0;
        while (offset < region.size) {
            const size_t remaining = static_cast<size_t>(std::min<uint64_t>(
                region.size - offset,
                static_cast<uint64_t>(options.chunkSize)));
            const auto read = reader.readChunked(
                region.baseAddress + offset,
                remaining,
                options.chunkSize,
                options.cancellation);

            if (read.cancelled) {
                *cancelled = true;
                return results;
            }
            if (read.bytesRead == 0) break;

            const char* data = read.data.constData();
            const qsizetype pointerCount = static_cast<qsizetype>(read.bytesRead / sizeof(uint64_t));

            for (qsizetype i = 0; i < pointerCount; ++i) {
                const uint64_t pointerLocation = region.baseAddress + offset
                    + static_cast<uint64_t>(i * sizeof(uint64_t));

                uint64_t pointerValue = 0;
                std::memcpy(&pointerValue, data + i * sizeof(uint64_t), sizeof(uint64_t));

                progress->pointersScanned++;

                // Ce pointeur pointe-t'il vers une cible (avec offset <= maxOffset) ?
                for (uint64_t delta = 0; delta <= options.maxOffset; delta += options.alignment) {
                    const uint64_t candidateTarget = pointerValue + delta;
                    if (candidateTarget < pointerValue) break; // overflow
                    auto it = targets.find(candidateTarget);
                    if (it != targets.end()) {
                        LevelHit hit;
                        hit.pointerLocation = pointerLocation;
                        hit.offset = delta;
                        hit.pointedTarget = candidateTarget;
                        results.append(hit);
                        progress->candidatesFound++;
                        if (static_cast<size_t>(results.size()) >= maxLevelResults) {
                            *partial = true;
                            if (options.progressCallback) options.progressCallback(*progress);
                            return results;
                        }
                        break; // Un match suffit par pointeur.
                    }
                }
            }

            progress->bytesScanned += read.bytesRead;
            if (progress->pointersScanned % 200000 == 0) {
                if (options.progressCallback) options.progressCallback(*progress);
            }
            offset += static_cast<uint64_t>(read.bytesRead);
            if (read.partial) {
                *partial = true;
                break;
            }
        }
    }

    if (options.progressCallback) options.progressCallback(*progress);
    return results;
}

} // namespace

PointerScanResult scanForPointerChains(
    const ProcessHandle& handle,
    uint64_t targetAddress,
    const PointerScanOptions& options) {

    PointerScanResult result;
    PointerScanProgress progress;
    progress.maxLevel = options.maxDepth;

    if (!handle.isValid()) {
        result.errorMessage = "Process handle is not valid.";
        return result;
    }
    if (options.maxDepth < 1) {
        result.errorMessage = "maxDepth must be >= 1.";
        return result;
    }

    const auto modules = ProcessEnumerator::enumerateModules(handle.pid());
    if (modules.isEmpty()) {
        result.errorMessage = "No modules enumerated for process.";
        return result;
    }

    // Algorithme BFS par niveaux avec reconstruction correcte des chemins.
    //
    // Chaque chemin partiel : (pointerLocation, offsets[])
    //   - pointerLocation = adresse où se trouve le dernier pointeur trouvé
    //   - offsets = chemin de la base jusqu'à targetAddress, ordre de résolution
    //     offsets[last] = offset du dernier pointeur vers sa cible
    //
    // À chaque niveau :
    //   1. On scanne pour trouver les pointeurs qui pointent vers les pointerLocation
    //      des chemins du niveau précédent (ou targetAddress au niveau 1).
    //   2. Pour chaque hit, on retrouve le chemin précédent via pointedTarget,
    //      on prépend l'offset du nouveau niveau.
    //   3. Si le pointerLocation du chemin est dans un module → chaîne complète.

    struct PartialChain {
        uint64_t pointerLocation{0};
        QList<uint64_t> offsets; ///< ordre de résolution : [niv1, niv2, ..., vers target]
    };

    QList<PartialChain> currentPaths;
    std::unordered_set<uint64_t> currentTargets;
    currentTargets.insert(targetAddress);

    // Index pour retrouver rapidement le chemin précédent depuis pointedTarget.
    // On stocke les chemins par valeur pour éviter les pointeurs invalidés par
    // les réallocations internes de QList pendant les append.
    std::unordered_map<uint64_t, PartialChain> pathByLocation;

    for (int level = 1; level <= options.maxDepth; ++level) {
        progress.currentLevel = level;
        bool cancelled = false;
        bool partial = false;
        const size_t maxLevelResults = options.maxResults * 20;

        const auto hits = scanLevel(
            handle, currentTargets, options, &progress, &cancelled, &partial, maxLevelResults);

        result.pointersScanned = progress.pointersScanned;
        result.bytesScanned = progress.bytesScanned;

        if (cancelled) {
            result.cancelled = true;
            result.partial = true;
            result.errorMessage = "Scan cancelled.";
            return result;
        }

        if (hits.isEmpty()) break;

        QList<PartialChain> nextPaths;
        std::unordered_map<uint64_t, PartialChain> nextPathByLocation;
        std::unordered_set<uint64_t> nextTargets;

        for (const auto& hit : hits) {
            PartialChain newChain;
            newChain.pointerLocation = hit.pointerLocation;

            if (level == 1) {
                // Niveau 1 : la cible est targetAddress elle-même.
                newChain.offsets.append(hit.offset);
            } else {
                // Niveau > 1 : retrouver le chemin précédent correspondant à pointedTarget.
                auto it = pathByLocation.find(hit.pointedTarget);
                if (it == pathByLocation.end()) continue;
                newChain.offsets = it->second.offsets;
                newChain.offsets.prepend(hit.offset);
            }

            nextPaths.append(newChain);
            nextPathByLocation[newChain.pointerLocation] = newChain;
            nextTargets.insert(newChain.pointerLocation);

            // Vérifier si ce chemin ferme une chaîne vers un module statique.
            const ProcessModuleInfo* mod = findModuleForAddress(modules, newChain.pointerLocation);
            if (mod && (!options.onlyModuleBase ||
                        isAllowedBaseModule(modules, newChain.pointerLocation, options.baseModules))) {
                PointerChain fullChain;
                fullChain.module = mod->name;
                fullChain.baseOffset = newChain.pointerLocation - mod->baseAddress;
                fullChain.offsets = newChain.offsets;
                result.chains.append(fullChain);
                progress.chainsFound++;

                if (result.chains.size() >= static_cast<qsizetype>(options.maxResults)) {
                    result.partial = true;
                    break;
                }
            }
        }

        if (result.chains.size() >= static_cast<qsizetype>(options.maxResults)) break;

        // Mode debug : accepter les chaînes non-module à la profondeur max.
        if (!options.onlyModuleBase && level == options.maxDepth) {
            for (const auto& chain : nextPaths) {
                const ProcessModuleInfo* mod = findModuleForAddress(modules, chain.pointerLocation);
                if (mod) continue;
                PointerChain fullChain;
                fullChain.module = "<heap>";
                fullChain.baseOffset = chain.pointerLocation;
                fullChain.offsets = chain.offsets;
                result.chains.append(fullChain);
                progress.chainsFound++;
                if (result.chains.size() >= static_cast<qsizetype>(options.maxResults)) break;
            }
        }

        currentPaths = std::move(nextPaths);
        pathByLocation = std::move(nextPathByLocation);
        currentTargets = std::move(nextTargets);
        if (currentTargets.empty()) break;
    }

    // Trier par profondeur croissante (chaînes courtes = plus fiables).
    std::sort(result.chains.begin(), result.chains.end(),
              [](const PointerChain& a, const PointerChain& b) {
                  return a.depth() < b.depth();
              });

    result.success = true;
    KE_LOG_INFO() << "Pointer scan completed: target=0x" << QString::number(targetAddress, 16).toStdString()
                  << " chains=" << result.chains.size()
                  << " pointers=" << result.pointersScanned;
    return result;
}

} // namespace killcore
