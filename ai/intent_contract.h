#pragma once

#include <QString>
#include <QStringList>
#include <QVariantMap>

namespace killai {

class IntentContract {
public:
    static QStringList supportedIntents();
    static bool isSupportedIntent(const QString& intent);
    static bool validate(const QVariantMap& intentObject, QString* error = nullptr);
};

} // namespace killai
