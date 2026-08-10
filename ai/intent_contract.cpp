#include "intent_contract.h"

#include <QRegularExpression>
#include <QVariantList>

namespace killai {

namespace {

bool isValidHexAddress(const QString& value) {
    static const QRegularExpression re(R"(^0x[0-9a-fA-F]{5,16}$)");
    return re.match(value.trimmed()).hasMatch();
}

bool hasScalarValue(const QVariantMap& intentObject, const QString& key) {
    const QVariant value = intentObject.value(key);
    return value.isValid() && !value.toString().trimmed().isEmpty();
}

} // namespace

QStringList IntentContract::supportedIntents() {
    return {
        "Unknown",
        "ResetContext",
        "ExactScan",
        "GuidedScan",
        "RefineScan",
        "ActivateMemoryTargets",
        "WriteMemoryTargets",
        "RewriteLastTargets",
        "WriteProfileTargets",
        "ReportBadTargets",
    };
}

bool IntentContract::isSupportedIntent(const QString& intent) {
    return supportedIntents().contains(intent);
}

bool IntentContract::validate(const QVariantMap& intentObject, QString* error) {
    const QString intent = intentObject.value("intent").toString();
    if (intent.isEmpty()) {
        if (error) *error = "Intent object missing 'intent'.";
        return false;
    }

    if (!isSupportedIntent(intent)) {
        if (error) *error = QString("Unsupported intent '%1'.").arg(intent);
        return false;
    }

    if ((intent == "ExactScan" || intent == "RefineScan" || intent == "WriteMemoryTargets"
         || intent == "RewriteLastTargets" || intent == "WriteProfileTargets")
        && !hasScalarValue(intentObject, "value")) {
        if (error) *error = QString("Intent '%1' missing scalar 'value'.").arg(intent);
        return false;
    }

    if (intent == "GuidedScan"
        && (!hasScalarValue(intentObject, "value") || !hasScalarValue(intentObject, "targetValue"))) {
        if (error) *error = "Intent 'GuidedScan' requires 'value' and 'targetValue'.";
        return false;
    }

    if (intent == "ActivateMemoryTargets" || intent == "WriteMemoryTargets") {
        const QVariantList addresses = intentObject.value("addresses").toList();
        if (addresses.isEmpty()) {
            if (error) *error = QString("Intent '%1' requires at least one address.").arg(intent);
            return false;
        }
        for (const auto& address : addresses) {
            if (!isValidHexAddress(address.toString())) {
                if (error) *error = QString("Invalid address '%1'.").arg(address.toString());
                return false;
            }
        }
    }

    if (error) error->clear();
    return true;
}

} // namespace killai
