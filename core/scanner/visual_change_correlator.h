#pragma once

#include <QVariantMap>

namespace killcore {
// Pure comparison of two read-only source snapshots. Never validates an effect.
QVariantMap correlateVisualChange(const QVariantMap& observation,
                                const QVariantMap& before, const QVariantMap& after);
}
