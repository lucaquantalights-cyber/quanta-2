@echo off
setlocal
cd /d "%~dp0"

echo Starting Quanta Firmware Installer...

if not exist "Quanta_Installer.py" (
    echo ERROR: Quanta_Installer.py not found.
    pause
    exit /b 1
)

if exist ".quanta_env\Scripts\python.exe" (
    ".quanta_env\Scripts\python.exe" Quanta_Installer.py
) else (
    echo Creating Python environment...
    py -3 -m venv .quanta_env

    if errorlevel 1 (
        echo ERROR: Python 3 is required.
        pause
        exit /b 1
    )

    ".quanta_env\Scripts\python.exe" -m pip install esptool==4.8.1 pyserial

    if errorlevel 1 (
        echo ERROR: Package installation failed.
        pause
        exit /b 1
    )

    ".quanta_env\Scripts\python.exe" Quanta_Installer.py
)

if errorlevel 1 (
    echo.
    echo Quanta installer encountered an error.
)

pause