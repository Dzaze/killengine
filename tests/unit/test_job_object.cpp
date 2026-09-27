// Tests unitaires du Job Object Windows (UX-PRODUIT-15,
// docs/PHASE_TRACKER.md) : vraie création de job, vraie assignation d'un
// process réellement spawné (powershell.exe en sommeil, sans fenêtre ni
// dépendance à la redirection stdin -- contrairement à `timeout`, qui échoue
// sous stdin redirigé), et vérification RÉELLE que fermer le handle du job
// termine effectivement le process assigné (JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE),
// pas juste que l'appel Win32 a réussi sans erreur.

#include "process/job_object.h"

#include <gtest/gtest.h>

#include <QProcess>

#include <windows.h>

using namespace killcore;

namespace {

// Process de longue durée sans fenêtre, sans dépendance à un stdin
// interactif (contrairement à `cmd /c timeout`, qui échoue sous stdin
// redirigé -- ce que fait QProcess::startDetached par défaut).
bool spawnLongRunningProcess(qint64* outPid) {
    return QProcess::startDetached(
        "powershell.exe",
        {"-NoProfile", "-NonInteractive", "-Command", "Start-Sleep -Seconds 30"},
        QString(),
        outPid);
}

bool isProcessAlive(qint64 pid) {
    HANDLE handle = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (!handle) {
        return false; // process introuvable = déjà terminé
    }
    const DWORD result = WaitForSingleObject(handle, 0);
    CloseHandle(handle);
    return result == WAIT_TIMEOUT; // encore signalé "non terminé"
}

} // namespace

TEST(JobObjectTest, CreatesValidJobByDefault) {
    JobObject job;
    EXPECT_TRUE(job.isValid());
}

TEST(JobObjectTest, AssignProcessFailsForInvalidPid) {
    JobObject job;
    ASSERT_TRUE(job.isValid());
    // PID très improbable d'être un process réel au moment du test.
    EXPECT_FALSE(job.assignProcess(999999999));
}

TEST(JobObjectTest, AssignProcessSucceedsForRealRunningProcess) {
    qint64 pid = 0;
    ASSERT_TRUE(spawnLongRunningProcess(&pid));
    ASSERT_GT(pid, 0);
    ASSERT_TRUE(isProcessAlive(pid));

    JobObject job;
    ASSERT_TRUE(job.isValid());
    EXPECT_TRUE(job.assignProcess(pid));

    // Nettoyage explicite (ne dépend pas du comportement testé ci-dessous).
    HANDLE handle = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
    if (handle) {
        TerminateProcess(handle, 0);
        CloseHandle(handle);
    }
}

TEST(JobObjectTest, ClosingTheJobHandleTerminatesTheAssignedProcess) {
    qint64 pid = 0;
    ASSERT_TRUE(spawnLongRunningProcess(&pid));
    ASSERT_GT(pid, 0);
    ASSERT_TRUE(isProcessAlive(pid)) << "Le process de test aurait dû démarrer et tourner encore.";

    {
        JobObject job;
        ASSERT_TRUE(job.isValid());
        ASSERT_TRUE(job.assignProcess(pid));
    } // job détruit ici : JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE doit terminer pid

    // Le noyau termine le process de façon asynchrone -- attendre un court
    // instant avant de conclure à un échec.
    bool stillAlive = isProcessAlive(pid);
    for (int attempt = 0; stillAlive && attempt < 20; ++attempt) {
        Sleep(100);
        stillAlive = isProcessAlive(pid);
    }
    EXPECT_FALSE(stillAlive) << "Le process assigné aurait dû être terminé par la fermeture du JobObject (test du point 5 de la fiche UX-PRODUIT-15 : survie d'un enfant à un parent tué/crashé).";
}
