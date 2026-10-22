@echo off
rem Build script for functional tests (real files, HKCU keys, processes).
rem Console subsystem, no requireAdministrator manifest.
setlocal
set "VCVARS=%VS_VCVARS%"
if not defined VCVARS (
    for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
)
if not defined VCVARS (
    echo [ERROR] Visual Studio with C++ tools not found. Set VS_VCVARS to your vcvars64.bat path.
    exit /b 1
)
call "%VCVARS%" >nul
cd /d "%~dp0"
if not exist bin mkdir bin
cl /nologo /EHsc /std:c++20 /utf-8 /MD /W3 /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE ^
   /Fo:bin\func ^
   /Fe:bin\functional_tests.exe ^
   functional_tests.cpp ^
   /link /SUBSYSTEM:CONSOLE ^
   shell32.lib ole32.lib uuid.lib advapi32.lib shlwapi.lib user32.lib ^
   comctl32.lib comdlg32.lib shcore.lib wininet.lib gdi32.lib gdiplus.lib
if errorlevel 1 exit /b 1
@echo off
