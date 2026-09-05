@echo off
:: KillEngine - Ajouter exclusion Windows Defender
:: Ce script ajoute le dossier build\bin aux exclusions Defender.
:: Il demande automatiquement l'élévation administrateur.

:: Vérifier les droits admin
net session >nul 2>&1
if %errorLevel% neq 0 (
    :: Pas admin - relancer avec élévation
    powershell -Command "Start-Process '%~f0' -Verb RunAs"
    exit /b
)

:: Déterminer le chemin du dossier build\bin (parent du dossier scripts)
set "SCRIPT_DIR=%~dp0"
set "BUILD_BIN=%SCRIPT_DIR%..\build\bin"

:: Normaliser le chemin (supprimer le slash final)
for %%i in ("%BUILD_BIN%") do set "BUILD_BIN=%%~fi"

echo.
echo ============================================
echo   KillEngine - Exclusion Windows Defender
echo ============================================
echo.
echo Dossier: %BUILD_BIN%
echo.

:: Ajouter l'exclusion
powershell -Command "Add-MpPreference -ExclusionPath '%BUILD_BIN%'"
if %errorLevel% equ 0 (
    echo [OK] Exclusion ajoutee avec succes.
    echo.
    echo Verification...
    powershell -Command "Get-MpPreference | Select-Object -ExpandProperty ExclusionPath | Select-String '%BUILD_BIN%'"
    echo.
    echo Le test EDR dans KillEngine devrait maintenant passer au vert.
) else (
    echo [ERREUR] Echec de l'ajout de l'exclusion.
    echo Verifiez que Tamper Protection est desactive dans Windows Security.
)

echo.
pause
