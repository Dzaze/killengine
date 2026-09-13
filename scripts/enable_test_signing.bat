@echo off
:: KillEngine - Activer le mode Test Signing Windows
:: Necessaire pour charger le driver noyau KillEngineKernel.sys, qui n'est
:: pas signe WHQL/EV (voir docs/KILLENGINE_KERNEL_DRIVER_ARCHITECTURE.md).
:: Affiche un watermark permanent "Test Mode" sur le bureau et necessite un
:: redemarrage Windows pour prendre effet. Reversible via disable_test_signing.bat.
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
echo   KillEngine - Activer Test Signing
echo ============================================
echo.
echo ATTENTION : ce mode affiche un watermark permanent "Test Mode" en bas a
echo droite du bureau et reduit legerement la verification des pilotes charges
echo par Windows. Un redemarrage est requis pour que le changement prenne effet.
echo.

bcdedit /set testsigning on
if %errorLevel% equ 0 (
    echo [OK] Test Signing active. Redemarre Windows pour appliquer le changement.
) else (
    echo [ERREUR] Echec de bcdedit (code %errorLevel%^) - Secure Boot doit peut-etre etre desactive dans le BIOS/UEFI.
)

echo.
pause
