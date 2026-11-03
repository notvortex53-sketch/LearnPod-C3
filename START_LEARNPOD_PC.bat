@echo off
title LearnPod PC Controller
cd /d "%~dp0"

echo.
echo  ============================================
echo    LearnPod PC Controller  -  One-Click
echo  ============================================
echo.

REM ─── 1. Find Python (py launcher first, then "python") ──────────────────────
set "PY="
py -3 --version >nul 2>&1
if not errorlevel 1 set "PY=py -3"
if not defined PY (
    python --version >nul 2>&1
    if not errorlevel 1 set "PY=python"
)
if not defined PY (
    echo  [!] Python is not installed on this PC.
    echo.
    echo  Opening the Python download page for you...
    echo  Install it ^(tick "Add Python to PATH"^), then run this file again.
    timeout /t 3 >nul
    start https://www.python.org/downloads/windows/
    pause
    exit /b 1
)

REM ─── 2. One-time automatic install of required components ────────────────────
%PY% -c "import bleak, pyautogui" >nul 2>&1
if errorlevel 1 (
    echo  First time setup: installing components, please wait...
    %PY% -m pip install --quiet --disable-pip-version-check bleak pyautogui
    if errorlevel 1 (
        echo  [!] Install failed. Check your internet connection and try again.
        pause
        exit /b 1
    )
    echo  Done!
)

REM ─── 3. Make sure learnpod_ble_pc.py is next to this .bat ────────────────────
if not exist "learnpod_ble_pc.py" (
    echo  The controller file was not found next to this launcher.
    echo  Trying to download it from GitHub automatically...
    powershell -NoProfile -Command "try { [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12; Invoke-WebRequest -UseBasicParsing 'https://raw.githubusercontent.com/notvortex53-sketch/LearnPod-C3/main/learnpod_ble_pc.py' -OutFile 'learnpod_ble_pc.py'; Write-Host 'Downloaded OK.' } catch { Write-Host ('Download failed: ' + $_.Exception.Message) }"
)

if not exist "learnpod_ble_pc.py" (
    echo.
    echo  [!] Could not get learnpod_ble_pc.py automatically.
    echo      Fix: put BOTH files in the same folder:
    echo        - START_LEARNPOD_PC.bat
    echo        - learnpod_ble_pc.py
    echo      (or let this PC use the internet and run this again)
    echo.
    pause
    exit /b 1
)

REM ─── 4. Start the controller ─────────────────────────────────────────────────
echo.
echo  Ready. Starting the controller...
echo.
echo  GETTING STARTED:
echo   1) Make sure this PC has Bluetooth turned ON.
echo   2) On the LearnPod: HOLD the PTT button for 3 seconds.
echo   3) The light "PC Connected!" on its screen means we are talking.
echo.
echo  Keep this window OPEN - closing it stops PC control.
echo  _______________________________________________
echo.
%PY% -u learnpod_ble_pc.py

REM ─── 5. If the script crashed, keep the window open with the error ───────────
echo.
echo  The controller stopped or crashed. The error is shown above.
pause
