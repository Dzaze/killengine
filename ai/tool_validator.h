#pragma once

#include "tool_registry.h"

#include <QObject>
#include <QVariantMap>

namespace killai {

class ToolValidator : public QObject {
    Q_OBJECT
public:
    explicit ToolValidator(QObject* parent = nullptr);

    bool validate(const QVariantMap& toolCall, QString* error = nullptr) const;

private:
    ToolRegistry m_registry;
};

} // namespace killai
