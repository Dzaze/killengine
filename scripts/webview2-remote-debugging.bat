@echo off
REM ============================================================================
REM webview2-remote-debugging.bat
REM
REM Chantier "Inspection WebView2/JS (CDP)" - docs/PHASE_TRACKER.md, sous-phase
REM WEBVIEW-A. Pose ou retire la policy WebView2 qui force un port de debug
REM Chrome DevTools Protocol (CDP) sur les apps hote WebView2 (ex: Solitaire).
REM
REM POURQUOI : une app tierce (Store/UWP, Electron via CEF, etc.) n'expose pas
REM de port de debug par defaut. Sans ce port, impossible d'inspecter son etat
REM JavaScript (variables, breakpoints) -- seule option restante est le scan
REM memoire brut, qui ne marche pas sur les valeurs JS (V8 les stocke de facon
REM taguee/boxee, pas comme un nombre natif a une adresse previsible).
REM
REM CE QUE CA FAIT CONCRETEMENT : ecrit la cle de registre
REM   HKCU\SOFTWARE\Policies\Microsoft\Edge\WebView2\AdditionalBrowserArguments
REM   = --remote-debugging-port=9333
REM Cette cle s'applique a TOUS les hotes WebView2 lances par l'utilisateur
REM Windows courant apres la pose (pas seulement la cible visee) -- portee
REM large, a assumer en connaissance de cause. Elle ne prend effet qu'au
REM PROCHAIN lancement d'un process WebView2 : ferme completement l'app cible
REM (et son process hote WebView2 parent, pas juste sa fenetre) avant de la
REM rouvrir.
REM
REM Une fois la cible relancee, verifier que le port repond :
REM   http://127.0.0.1:9333/json
REM (doit lister une ou plusieurs pages avec un champ "webSocketDebuggerUrl")
REM
REM USAGE :
REM   webview2-remote-debugging.bat on     -> pose la cle
REM   webview2-remote-debugging.bat off    -> retire la cle (retour a l'etat par defaut)
REM ============================================================================

setlocal
set KEY=HKCU\SOFTWARE\Policies\Microsoft\Edge\WebView2
set PORT=9333

if "%~1"=="on" goto :enable
if "%~1"=="off" goto :disable
goto :usage

:enable
reg add "%KEY%" /v AdditionalBrowserArguments /t REG_SZ /d "--remote-debugging-port=%PORT%" /f
echo.
echo Cle posee. Port CDP force a %PORT% pour TOUS les hotes WebView2 du user courant.
echo Ferme completement l'app cible (process hote WebView2 y compris) puis relance-la.
echo Verifie ensuite : http://127.0.0.1:%PORT%/json
goto :eof

:disable
reg delete "%KEY%" /v AdditionalBrowserArguments /f
echo.
echo Cle retiree. Les hotes WebView2 relances desormais n'exposeront plus de port de debug.
goto :eof

:usage
echo Usage: %~nx0 [on^|off]
echo   on   - force --remote-debugging-port=%PORT% sur tous les hotes WebView2 (HKCU)
echo   off  - retire la cle, retour au comportement par defaut
goto :eof
