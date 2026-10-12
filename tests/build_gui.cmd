@echo off
rem Build script for GUI interaction tests.
rem Console subsystem and no requireAdministrator manifest, so tests run unelevated.
rem UNICODE/_UNICODE must be defined explicitly: the app sources rely on the wide
rem variants of the common-controls macros (ListView_SetItemText etc.).
rem VS location resolution: set VS_VCVARS to your vcvars64.bat path to override;
rem otherwise auto-detect via vswhere.
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
   /Fo:bin\gui ^
   /Fe:bin\gui_tests.exe ^
   gui_tests.cpp ^
   /link /SUBSYSTEM:CONSOLE ^
   shell32.lib ole32.lib uuid.lib advapi32.lib shlwapi.lib user32.lib ^
   comctl32.lib comdlg32.lib shcore.lib wininet.lib
if errorlevel 1 exit /b 1
