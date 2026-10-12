@echo off
rem Build the main solution with msbuild.
rem Usage: build_app.cmd <Configuration> <Platform>   e.g. build_app.cmd Release x64
rem VS location resolution: set VS_VCVARS to your vcvars64.bat path to override;
rem otherwise auto-detect via vswhere.
setlocal
if "%~1"=="" (
    echo Usage: build_app.cmd ^<Configuration^> ^<Platform^>   e.g. build_app.cmd Release x64
    exit /b 1
)
set "VCVARS=%VS_VCVARS%"
if not defined VCVARS (
    for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
)
if not defined VCVARS (
    echo [ERROR] Visual Studio with C++ tools not found. Set VS_VCVARS to your vcvars64.bat path.
    exit /b 1
)
call "%VCVARS%" >nul 2>&1
msbuild "%~dp0..\SystemCleaner.sln" /t:Build /p:Configuration=%1 /p:Platform=%2 /m /nologo /v:m
