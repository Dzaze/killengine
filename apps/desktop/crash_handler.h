#pragma once

#include <QString>

namespace killengine {

class CrashHandler {
public:
    static void install();
    static QString crashDirectory();
    static QString writeReport(const QString& reason, const QString& detail = QString());
};

} // namespace killengine
