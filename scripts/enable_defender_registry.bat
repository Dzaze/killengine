@echo off
:: KillEngine - Reactiver Windows Defender via registre
:: Ce script supprime les cles registre de desactivation de Defender.
:: Il demande automatiquement l'elevation administrateur.

:: Verifier les droits admin
net session >nul 2>&1
if %errorLevel% neq 0 (
    :: Pas admin - relancer avec elevation
    powershell -Command "Start-Process '%~f0' -Verb RunAs"
    exit /b
)

echo.
echo ============================================
echo   KillEngine - Reactiver Windows Defender
echo ============================================
echo.

:: Etape 1 : Supprimer DisableAntiSpyware
echo [1/2] Suppression de DisableAntiSpyware...
reg delete "HKLM\SOFTWARE\Policies\Microsoft\Windows Defender" /v DisableAntiSpyware /f
if %errorLevel% equ 0 (
    echo [OK] DisableAntiSpyware supprime
) else (
    echo [ERREUR] Echec de la suppression (cle peut-etre deja absente)
)

echo.

:: Etape 2 : Supprimer DisableBehaviorMonitoring
echo [2/2] Suppression de DisableBehaviorMonitoring...
reg delete "HKLM\SOFTWARE\Policies\Microsoft\Windows Defender\Real-Time Protection" /v DisableBehaviorMonitoring /f
if %errorLevel% equ 0 (
    echo [OK] DisableBehaviorMonitoring supprime
) else (
    echo [ERREUR] Echec de la suppression (cle peut-etre deja absente)
)

echo.
echo Verification des cles...
echo.

:: Verifier DisableAntiSpyware
powershell -Command "$v = Get-ItemPropertyValue 'HKLM:\SOFTWARE\Policies\Microsoft\Windows Defender' -Name DisableAntiSpyware -ErrorAction SilentlyContinue; if ($null -eq $v) { Write-Output '[OK] DisableAntiSpyware absent (Defender actif par defaut)' } else { Write-Output '[ATTENTION] DisableAntiSpyware = $v' }"

:: Verifier DisableBehaviorMonitoring
powershell -Command "$v = Get-ItemPropertyValue 'HKLM:\SOFTWARE\Policies\Microsoft\Windows Defender\Real-Time Protection' -Name DisableBehaviorMonitoring -ErrorAction SilentlyContinue; if ($null -eq $v) { Write-Output '[OK] DisableBehaviorMonitoring absent (surveillance active par defaut)' } else { Write-Output '[ATTENTION] DisableBehaviorMonitoring = $v' }"

echo.
echo Un redemarrage Windows est requis pour que les modifications prennent effet.
echo.
pause
