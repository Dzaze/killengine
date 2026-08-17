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
};

} // namespace killai
