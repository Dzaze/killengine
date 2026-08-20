#pragma once

#include <QString>

namespace killengine {

class CrashHandler {
public:
    static void install();
    static QString crashDirectory();

    /// Écrit un rapport texte (toujours) et, sur Windows, un minidump `.dmp`
    /// pairé (même horodatage) via MiniDumpWriteDump — un vrai crash dump
    /// chargeable dans WinDbg/Visual Studio, bien plus exploitable que le
    /// texte seul pour diagnostiquer un crash après coup.
    /// `exceptionPointers` : passer le `EXCEPTION_POINTERS*` reçu par un
    /// handler SEH (le seul cas où on a le contexte exact du crash) ; nullptr
    /// sinon (terminate/signal), le dump reste utile (piles, modules) mais
    /// sans le point de faute précis.
    static QString writeReport(const QString& reason, const QString& detail = QString(), void* exceptionPointers = nullptr);
};

} // namespace killengine
