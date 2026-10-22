@echo off
cd /d "%~dp0"
if not exist bin mkdir bin
"%WINDIR%\Microsoft.NET\Framework64\v4.0.30319\csc.exe" -nologo -target:exe ^
  -out:bin\icon_gen.exe -r:System.Drawing.dll icon_gen.cs
if errorlevel 1 exit /b 1
bin\icon_gen.exe "..\SystemCleaner\SystemCleaner.ico" "bin"
