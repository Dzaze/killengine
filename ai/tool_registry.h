#pragma once

#include <QObject>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

namespace killai {

class ToolRegistry : public QObject {
    Q_OBJECT
public:
    explicit ToolRegistry(QObject* parent = nullptr);

    QVariantList availableTools() const;
    bool hasTool(const QString& name) const;
    QStringList requiredArgs(const QString& name) const;
};

} // namespace killai
