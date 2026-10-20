@echo off
chcp 65001 >nul
setlocal

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSPATH="
if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
)
if not defined VSPATH (
    echo [ERROR] Visual Studio C++ build tools not found.
    echo         Install "Visual Studio Build Tools" with "Desktop development with C++".
    pause
    exit /b 1
)
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d "%~dp0"

echo ============================================
echo   Building countdown.cpp (console version)
echo ============================================
cl /nologo /utf-8 /EHsc /O2 /std:c++17 countdown.cpp /Fe:countdown.exe /link winmm.lib user32.lib
if errorlevel 1 (
    echo.
    echo [FAILED] compile error.
    pause
    exit /b 1
)
del /q countdown.obj >nul 2>&1
echo.
echo [OK] countdown.exe
pause
