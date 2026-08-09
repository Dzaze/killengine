#pragma once
#include <QObject>
#include <QVariantMap>
namespace killai {
class EvidenceSummary : public QObject {
    Q_OBJECT
public:
    explicit EvidenceSummary(QObject* parent = nullptr) : QObject(parent) {}
    QVariantMap summarize() const { return {}; }
};
} // namespace killai