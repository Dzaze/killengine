#pragma once

#include <QString>
#include <QStringList>

namespace killai {

struct ModelInfo {
    bool found{false};
    QString path;
    QString source;
    QString errorMessage;
};

class ModelLocator {
public:
    static ModelInfo findQwenGguf();
    static QStringList candidateModelPaths();
    static QStringList discoverModelFiles();

    /// PORT-4 (docs/PORTABILITY_ROADMAP.md, 18/09/2026) : convertit un chemin
    /// choisi par l'utilisateur (ex. via un sélecteur de fichier, toujours
    /// absolu) en référence portable si son appartenance au paquet est
    /// démontrée (sous killcore::PortablePaths::root()) -- retourne alors un
    /// chemin relatif à cette racine. Un chemin déjà relatif est laissé tel
    /// quel (sera résolu par resolveModelReference()). Un chemin absolu HORS
    /// du paquet n'est jamais deviné comme lui appartenant : reste absolu,
    /// référence externe explicite.
    static QString toPortableModelReference(const QString& rawPath);

    /// Résout une valeur stockée (ancien absolu, référence portable relative,
    /// ou référence externe absolue) en chemin absolu utilisable. Un chemin
    /// relatif se résout TOUJOURS contre killcore::PortablePaths::root(),
    /// jamais contre le répertoire courant du process.
    static QString resolveModelReference(const QString& storedValue);
};

} // namespace killai
