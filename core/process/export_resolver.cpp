#include "process/export_resolver.h"

#include "process/process_enumerator.h"
#include "memory/memory_reader.h"

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#include <QByteArray>

#include <cstring>

namespace killcore {

#ifdef Q_OS_WIN
namespace {

bool moduleNameMatches(const QString& candidate, const QString& query) {
    const QString a = candidate.trimmed().toLower();
    const QString b = query.trimmed().toLower();
    if (a == b) return true;

    auto stripExtension = [](const QString& s) {
        const int dot = s.lastIndexOf('.');
        return dot > 0 ? s.left(dot) : s;
    };
    return stripExtension(a) == stripExtension(b);
}

template <typename T>
bool readRemoteStruct(const MemoryReader& reader, uint64_t address, T* out) {
    const auto result = reader.read(address, sizeof(T));
    if (!(result.success || result.partial) || result.bytesRead != sizeof(T)) {
        return false;
    }
    std::memcpy(out, result.data.constData(), sizeof(T));
    return true;
}

// Lit une string C distante par petits blocs, bornée à maxLen octets par
// sécurité (les noms d'export font quelques dizaines d'octets en pratique).
bool readRemoteCString(const MemoryReader& reader, uint64_t address, QString* out, int maxLen = 256) {
    QByteArray bytes;
    bytes.reserve(maxLen);
    const size_t chunk = 32;
    while (bytes.size() < maxLen) {
        const auto result = reader.read(address + static_cast<uint64_t>(bytes.size()), chunk);
        if (!(result.success || result.partial) || result.bytesRead == 0) {
            return false;
        }
        const int nul = result.data.indexOf('\0');
        if (nul >= 0) {
            bytes.append(result.data.left(nul));
            *out = QString::fromLatin1(bytes);
            return true;
        }
        bytes.append(result.data);
    }
    return false; // Pas de terminateur trouvé dans la borne de sécurité.
}

} // namespace
#endif

bool resolveRemoteExportAddress(
    const ProcessHandle& process,
    const QString& moduleName,
    const QString& functionName,
    uint64_t* address,
    QString* error) {

    if (address) *address = 0;

#ifdef Q_OS_WIN
    if (!address) {
        return false;
    }
    if (!process.isValid()) {
        if (error) *error = "Process handle invalide.";
        return false;
    }
    if (functionName.trimmed().isEmpty()) {
        if (error) *error = "Nom de fonction vide.";
        return false;
    }

    const auto modules = ProcessEnumerator::enumerateModules(process.pid());
    const ProcessModuleInfo* target = nullptr;
    for (const auto& mod : modules) {
        if (moduleNameMatches(mod.name, moduleName)) {
            target = &mod;
            break;
        }
    }
    if (!target) {
        if (error) *error = QString("Module '%1' introuvable dans le process.").arg(moduleName);
        return false;
    }

    MemoryReader reader(process);

    IMAGE_DOS_HEADER dos{};
    if (!readRemoteStruct(reader, target->baseAddress, &dos) || dos.e_magic != IMAGE_DOS_SIGNATURE) {
        if (error) *error = QString("En-tête DOS invalide pour '%1'.").arg(target->name);
        return false;
    }

    IMAGE_NT_HEADERS64 nt{};
    const uint64_t ntAddress = target->baseAddress + static_cast<uint64_t>(dos.e_lfanew);
    if (!readRemoteStruct(reader, ntAddress, &nt) || nt.Signature != IMAGE_NT_SIGNATURE) {
        if (error) *error = QString("En-tête NT invalide pour '%1'.").arg(target->name);
        return false;
    }
    if (nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        if (error) *error = QString("'%1' n'est pas un module x64 (KillEngine cible uniquement x64).").arg(target->name);
        return false;
    }

    const auto& exportDir = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (exportDir.VirtualAddress == 0 || exportDir.Size == 0) {
        if (error) *error = QString("Le module '%1' n'exporte aucune fonction.").arg(target->name);
        return false;
    }

    IMAGE_EXPORT_DIRECTORY exportTable{};
    const uint64_t exportTableAddress = target->baseAddress + exportDir.VirtualAddress;
    if (!readRemoteStruct(reader, exportTableAddress, &exportTable)) {
        if (error) *error = "Lecture de IMAGE_EXPORT_DIRECTORY échouée.";
        return false;
    }

    const uint64_t namesArrayAddress = target->baseAddress + exportTable.AddressOfNames;
    const uint64_t ordinalsArrayAddress = target->baseAddress + exportTable.AddressOfNameOrdinals;
    const uint64_t functionsArrayAddress = target->baseAddress + exportTable.AddressOfFunctions;

    for (uint32_t i = 0; i < exportTable.NumberOfNames; ++i) {
        uint32_t nameRva = 0;
        if (!readRemoteStruct(reader, namesArrayAddress + static_cast<uint64_t>(i) * sizeof(uint32_t), &nameRva)) {
            continue;
        }
        QString exportedName;
        if (!readRemoteCString(reader, target->baseAddress + nameRva, &exportedName)) {
            continue;
        }
        if (exportedName != functionName) {
            continue;
        }

        uint16_t ordinal = 0;
        if (!readRemoteStruct(reader, ordinalsArrayAddress + static_cast<uint64_t>(i) * sizeof(uint16_t), &ordinal)) {
            if (error) *error = "Lecture de AddressOfNameOrdinals échouée.";
            return false;
        }

        uint32_t functionRva = 0;
        if (!readRemoteStruct(reader, functionsArrayAddress + static_cast<uint64_t>(ordinal) * sizeof(uint32_t), &functionRva)) {
            if (error) *error = "Lecture de AddressOfFunctions échouée.";
            return false;
        }

        // Un forwarder pointe une string "AutreModule.AutreFonction" à
        // l'intérieur de la plage de la table d'export elle-même plutôt que du
        // code exécutable — la résoudre demanderait de rappeler cette même
        // fonction récursivement sur un autre module. Pas fait ici : retourne
        // une erreur claire plutôt qu'une fausse adresse.
        if (functionRva >= exportDir.VirtualAddress && functionRva < exportDir.VirtualAddress + exportDir.Size) {
            QString forwardTarget;
            readRemoteCString(reader, target->baseAddress + functionRva, &forwardTarget);
            if (error) {
                *error = QString("'%1!%2' est un forwarder vers '%3' — résolution automatique du forward non supportée.")
                             .arg(target->name, functionName, forwardTarget.isEmpty() ? QStringLiteral("?") : forwardTarget);
            }
            return false;
        }

        *address = target->baseAddress + functionRva;
        return true;
    }

    if (error) *error = QString("Fonction '%1' introuvable dans les exports de '%2'.").arg(functionName, target->name);
    return false;
#else
    (void)process;
    (void)moduleName;
    (void)functionName;
    if (error) *error = "Non supporté sur cette plateforme.";
    return false;
#endif
}

bool listRemoteExportNames(
    const ProcessHandle& process,
    const QString& moduleName,
    const QString& filterSubstring,
    int maxNames,
    QStringList* names,
    QString* error) {

    if (names) names->clear();

#ifdef Q_OS_WIN
    if (!names) {
        return false;
    }
    if (!process.isValid()) {
        if (error) *error = "Process handle invalide.";
        return false;
    }

    const auto modules = ProcessEnumerator::enumerateModules(process.pid());
    const ProcessModuleInfo* target = nullptr;
    for (const auto& mod : modules) {
        if (moduleNameMatches(mod.name, moduleName)) {
            target = &mod;
            break;
        }
    }
    if (!target) {
        if (error) *error = QString("Module '%1' introuvable dans le process.").arg(moduleName);
        return false;
    }

    MemoryReader reader(process);

    IMAGE_DOS_HEADER dos{};
    if (!readRemoteStruct(reader, target->baseAddress, &dos) || dos.e_magic != IMAGE_DOS_SIGNATURE) {
        if (error) *error = QString("En-tête DOS invalide pour '%1'.").arg(target->name);
        return false;
    }

    IMAGE_NT_HEADERS64 nt{};
    const uint64_t ntAddress = target->baseAddress + static_cast<uint64_t>(dos.e_lfanew);
    if (!readRemoteStruct(reader, ntAddress, &nt) || nt.Signature != IMAGE_NT_SIGNATURE) {
        if (error) *error = QString("En-tête NT invalide pour '%1'.").arg(target->name);
        return false;
    }
    if (nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        if (error) *error = QString("'%1' n'est pas un module x64 (KillEngine cible uniquement x64).").arg(target->name);
        return false;
    }

    const auto& exportDir = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (exportDir.VirtualAddress == 0 || exportDir.Size == 0) {
        if (error) *error = QString("Le module '%1' n'exporte aucune fonction.").arg(target->name);
        return false;
    }

    IMAGE_EXPORT_DIRECTORY exportTable{};
    const uint64_t exportTableAddress = target->baseAddress + exportDir.VirtualAddress;
    if (!readRemoteStruct(reader, exportTableAddress, &exportTable)) {
        if (error) *error = "Lecture de IMAGE_EXPORT_DIRECTORY échouée.";
        return false;
    }

    const uint64_t namesArrayAddress = target->baseAddress + exportTable.AddressOfNames;
    const QString filterLower = filterSubstring.trimmed().toLower();
    const int cap = maxNames > 0 ? maxNames : 500;

    for (uint32_t i = 0; i < exportTable.NumberOfNames && names->size() < cap; ++i) {
        uint32_t nameRva = 0;
        if (!readRemoteStruct(reader, namesArrayAddress + static_cast<uint64_t>(i) * sizeof(uint32_t), &nameRva)) {
            continue;
        }
        QString exportedName;
        if (!readRemoteCString(reader, target->baseAddress + nameRva, &exportedName)) {
            continue;
        }
        if (!filterLower.isEmpty() && !exportedName.toLower().contains(filterLower)) {
            continue;
        }
        names->append(exportedName);
    }

    return true;
#else
    (void)process;
    (void)moduleName;
    (void)filterSubstring;
    (void)maxNames;
    if (error) *error = "Non supporté sur cette plateforme.";
    return false;
#endif
}

} // namespace killcore
