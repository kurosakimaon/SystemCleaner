@echo off
call "D:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "d:\Users\Desktop\code\SystemCleaner\tests"
if not exist bin mkdir bin
cl /nologo /EHsc /std:c++20 /utf-8 /MD /W3 /D_CRT_SECURE_NO_WARNINGS ^
   /Fo:bin\ /Fe:bin\baseline_tests.exe ^
   baseline_tests.cpp ^
   shell32.lib ole32.lib advapi32.lib shlwapi.lib user32.lib
if errorlevel 1 exit /b 1
