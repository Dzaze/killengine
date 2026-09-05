@echo off
:: KillEngine - Desactiver Windows Defender via registre
:: Ce script ecrit les cles registre pour desactiver Defender.
:: Il demande automatiquement l'elevation administrateur.
:: NOTE : Tamper Protection doit etre desactive dans Windows Security.

:: Verifier les droits admin
net session >nul 2>&1
if %errorLevel% neq 0 (
    :: Pas admin - relancer avec elevation
    powershell -Command "Start-Process '%~f0' -Verb RunAs"
    exit /b
)

echo.
echo ============================================
echo   KillEngine - Desactiver Windows Defender
echo ============================================
echo.
echo ATTENTION : Tamper Protection doit etre desactive dans :
echo   Windows Security -^> Protection virus et menaces -^> Parametres
echo   -^> Tamper Protection -^> Desactiver
echo.
pause

:: Etape 1 : DisableAntiSpyware
echo [1/2] Ecriture de DisableAntiSpyware...
reg add "HKLM\SOFTWARE\Policies\Microsoft\Windows Defender" /v DisableAntiSpyware /t REG_DWORD /d 1 /f
if %errorLevel% equ 0 (
    echo [OK] DisableAntiSpyware = 1
) else (
    echo [ERREUR] Echec - Tamper Protection est peut-etre actif.
)

echo.

:: Etape 2 : DisableBehaviorMonitoring
echo [2/2] Ecriture de DisableBehaviorMonitoring...
reg add "HKLM\SOFTWARE\Policies\Microsoft\Windows Defender\Real-Time Protection" /v DisableBehaviorMonitoring /t REG_DWORD /d 1 /f
if %errorLevel% equ 0 (
    echo [OK] DisableBehaviorMonitoring = 1
) else (
    echo [ERREUR] Echec - Tamper Protection est peut-etre actif.
)

echo.
echo Verification des cles...
echo.

:: Verifier DisableAntiSpyware
powershell -Command "$v = Get-ItemPropertyValue 'HKLM:\SOFTWARE\Policies\Microsoft\Windows Defender' -Name DisableAntiSpyware -ErrorAction SilentlyContinue; if ($v -eq 1) { Write-Output '[OK] DisableAntiSpyware = 1' } else { Write-Output '[MANQUANT] DisableAntiSpyware non present' }"

:: Verifier DisableBehaviorMonitoring
powershell -Command "$v = Get-ItemPropertyValue 'HKLM:\SOFTWARE\Policies\Microsoft\Windows Defender\Real-Time Protection' -Name DisableBehaviorMonitoring -ErrorAction SilentlyContinue; if ($v -eq 1) { Write-Output '[OK] DisableBehaviorMonitoring = 1' } else { Write-Output '[MANQUANT] DisableBehaviorMonitoring non present' }"

echo.
echo Si les cles sont manquantes, Tamper Protection les a supprimees.
echo Desactive Tamper Protection dans Windows Security et reessaie.
echo.
echo Un redemarrage Windows est requis pour que les modifications prennent effet.
echo.
pause
