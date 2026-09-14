#include "visual_change_correlator.h"
#include "scanner/value_variants.h"
#include <QHash>
#include <QSet>

namespace killcore {
namespace {
QString key(const QVariantMap& item) {
    bool ok = false;
    const auto address = item.value("address").toString().toULongLong(&ok, 16);
    if (!ok || !address) return {};
    return QString::number(address, 16) + "|" + item.value("type").toString()
        + "|" + item.value("variantLabel").toString();
}
QByteArray expected(const QString& value, const QVariantMap& item) {
    ValueType type;
    if (!parseValueType(item.value("type").toString(), &type)) return {};
    const QString label = item.value("variantLabel").toString();
    if (!label.isEmpty()) {
        for (const auto& variant : generateScanVariants(value, type, true)) {
            if (variant.value.type == type && variant.label == label)
                return scanValueToBytes(variant.value);
        }
        return {}; // An unknown variant must not silently become an unscaled value.
    }
    ScanValue parsed;
    if (!parseScanValue(value, type, &parsed)) return {};
    return scanValueToBytes(parsed);
}
}

QVariantMap correlateVisualChange(const QVariantMap& observation,
                                 const QVariantMap& before, const QVariantMap& after) {
    QVariantMap result{{"success", false}, {"causalityProven", false},
        {"observationUncertain", true}, {"candidates", QVariantList{}},
        {"ambiguous", false}, {"error", ""}, {"status", "invalid_observation"}};
    result["observation"] = observation;
    if (observation.value("description").toString().trimmed().isEmpty()) return result;
    const qint64 start = before.value("startedMs").toLongLong();
    const qint64 endBefore = before.value("finishedMs").toLongLong();
    const qint64 startAfter = after.value("startedMs").toLongLong();
    const qint64 end = after.value("finishedMs").toLongLong();
    if (!before.value("success").toBool() || !after.value("success").toBool()
        || before.value("processInstance").toString().isEmpty()
        || before.value("processInstance") != after.value("processInstance")) {
        result["status"] = "process_changed";
        return result;
    }
    if (start <= 0 || endBefore < start || startAfter < endBefore || end < startAfter
        || end - start > 600000) {
        result["status"] = "expired_window";
        return result;
    }
    result["startedMs"] = start;
    result["finishedMs"] = end;
    result["success"] = true;
    result["partial"] = before.value("partial").toBool() || after.value("partial").toBool()
        || before.value("candidates").toList().size() > 256
        || after.value("candidates").toList().size() > 256;
    QHash<QString, QVariantMap> baseline;
    for (const auto& value : before.value("candidates").toList().mid(0, 256)) {
        const auto item = value.toMap();
        if (!key(item).isEmpty()) baseline.insert(key(item), item);
    }
    QVariantList candidates;
    QSet<QString> seen, plausibleAddresses;
    int strong = 0;
    const QString previous = observation.value("previousValue").toString().trimmed();
    const QString current = observation.value("currentValue").toString().trimmed();
    for (const auto& value : after.value("candidates").toList().mid(0, 256)) {
        auto item = value.toMap();
        const QString id = key(item);
        if (id.isEmpty() || seen.contains(id)) continue;
        seen.insert(id);
        const auto old = baseline.value(id);
        const QByteArray oldBytes = QByteArray::fromHex(old.value("bytesHex").toByteArray());
        const QByteArray newBytes = QByteArray::fromHex(item.value("bytesHex").toByteArray());
        const QByteArray wantedOld = expected(previous, item), wantedNew = expected(current, item);
        QString status = "unavailable";
        if (old.value("readable").toBool() && item.value("readable").toBool()
            && !oldBytes.isEmpty() && !newBytes.isEmpty()) {
            if (previous.isEmpty() || current.isEmpty())
                status = oldBytes != newBytes ? "weak" : "unchanged";
            else if (wantedOld.isEmpty() || wantedNew.isEmpty()) status = "unsupported";
            else if (oldBytes == wantedOld && newBytes == wantedNew)
                status = wantedOld != wantedNew ? "strong" : "weak";
            else status = "contradicted";
        }
        item["status"] = status;
        item["beforeHex"] = old.value("bytesHex");
        candidates.append(item);
        if (status == "strong") ++strong;
        if (status == "strong" || status == "weak") plausibleAddresses.insert(id.section('|', 0, 0));
    }
    result["candidates"] = candidates;
    result["strongCount"] = strong;
    result["ambiguous"] = plausibleAddresses.size() > 1;
    result["status"] = plausibleAddresses.isEmpty() ? "inconclusive"
        : plausibleAddresses.size() > 1 ? "ambiguous" : "correlated";
    const QString hypothesis = observation.value("hypothesis").toString();
    result["experiment"] = hypothesis == "maximum" ? "change_maximum_keep_current"
        : hypothesis == "animation" ? "wait_for_animation_then_repeat"
        : "change_one_quantity_then_reverse";
    return result;
}
}
