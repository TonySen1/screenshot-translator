@echo off
setlocal

rem NOTE: this file must stay pure ASCII with CRLF line endings.
rem cmd.exe mis-parses UTF-8 text and LF-only line breaks.

set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"

if not exist "%VCVARS%" goto :novc

call "%VCVARS%" >nul
if errorlevel 1 goto :nomsvc

if not exist build mkdir build
del /q build\*.obj 2>nul

echo [1/3] rc
rc /nologo /fo build\app.res src\resource.rc
if errorlevel 1 goto :failed

echo [2/3] cl (C)
cl /nologo /c /std:c11 /utf-8 /W4 /O2 /MT /GS- /Gy ^
   /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
   /Ithird_party /Fo:build\ ^
   src\*.c third_party\cJSON.c
if errorlevel 1 goto :failed

echo [3/3] cl (C++) + link
cl /nologo /c /std:c++17 /utf-8 /W4 /O2 /MT /GS- /Gy ^
   /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
   /Ithird_party /Fo:build\ ^
   src\*.cpp
if errorlevel 1 goto :failed

link /nologo /SUBSYSTEM:WINDOWS /ENTRY:wWinMainCRTStartup ^
     /OPT:REF /OPT:ICF /INCREMENTAL:NO ^
     /OUT:build\screenshot-translator.exe ^
     build\*.obj build\app.res ^
     user32.lib gdi32.lib msimg32.lib gdiplus.lib winhttp.lib comctl32.lib shell32.lib ^
     dwmapi.lib shcore.lib advapi32.lib ole32.lib
if errorlevel 1 goto :failed

echo.
for %%F in (build\screenshot-translator.exe) do (
    echo Built: %%~fF
    echo Size : %%~zF bytes
)
goto :done

:novc
echo [ERROR] vcvars64.bat not found.
echo         Expected under: Program Files ^(x86^)\Microsoft Visual Studio\2022\BuildTools
exit /b 1

:nomsvc
echo [ERROR] failed to initialize MSVC environment.
exit /b 1

:failed
echo [ERROR] build failed.
exit /b 1

:done
endlocal
