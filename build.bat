@echo off
cd /d "%~dp0"
echo ================================================
echo  Building Screenshot Translator (onefile exe)
echo ================================================
echo.

if exist ".venv\Scripts\python.exe" (
    ".venv\Scripts\python.exe" build.py
) else (
    py -3 build.py
    if errorlevel 1 python build.py
)

echo.
pause
