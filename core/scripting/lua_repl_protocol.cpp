#include "scripting/lua_repl_protocol.h"

#include <QRegularExpression>
#include <QSet>

#include <algorithm>

namespace killcore {

ReplExtraction extractReplOutput(const QByteArray& buffer, const QByteArray& sentinel) {
    ReplExtraction result;
    if (sentinel.isEmpty()) {
        return result;
    }
    const qsizetype idx = buffer.indexOf(sentinel);
    if (idx < 0) {
        return result;
    }
    result.found = true;
    result.output = QString::fromUtf8(buffer.left(idx));
    result.remaining = buffer.mid(idx + sentinel.size());
    return result;
}

QStringList extractKeCompletions(const QString& helperSource, const QString& prefix) {
    static const QRegularExpression pattern(QStringLiteral(R"(function\s+ke\.([A-Za-z_][A-Za-z0-9_]*)\s*\()"));

    QSet<QString> seen;
    QStringList names;
    auto it = pattern.globalMatch(helperSource);
    while (it.hasNext()) {
        const auto match = it.next();
        const QString name = QStringLiteral("ke.") + match.captured(1);
        if (!seen.contains(name)) {
            seen.insert(name);
            names.append(name);
        }
    }

    const QString trimmedPrefix = prefix.trimmed();
    if (!trimmedPrefix.isEmpty()) {
        QStringList filtered;
        for (const auto& name : names) {
            if (name.startsWith(trimmedPrefix, Qt::CaseInsensitive)) {
                filtered.append(name);
            }
        }
        names = filtered;
    }

    std::sort(names.begin(), names.end());
    return names;
}

} // namespace killcore
