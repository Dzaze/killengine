@echo off
:: KillEngine - Desactiver le mode Test Signing Windows
:: Annule enable_test_signing.bat une fois que tu n'as plus besoin de charger
:: KillEngineKernel.sys. Supprime le watermark "Test Mode" du bureau.
:: Un redemarrage Windows est requis pour que le changement prenne effet.
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
echo   KillEngine - Desactiver Test Signing
echo ============================================
echo.

bcdedit /set testsigning off
if %errorLevel% equ 0 (
    echo [OK] Test Signing desactive. Redemarre Windows pour appliquer le changement.
    echo Note : KillEngineKernel.sys ne pourra plus se charger tant que Test Signing est desactive.
) else (
    echo [ERREUR] Echec de bcdedit (code %errorLevel%^)
)

echo.
pause
