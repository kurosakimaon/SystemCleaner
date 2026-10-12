@echo off
rem Build script for GUI interaction tests.
rem Console subsystem and no requireAdministrator manifest, so tests run unelevated.
rem UNICODE/_UNICODE must be defined explicitly: the app sources rely on the wide
rem variants of the common-controls macros (ListView_SetItemText etc.).
call "D:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "d:\Users\Desktop\code\SystemCleaner\tests"
if not exist bin mkdir bin
cl /nologo /EHsc /std:c++20 /utf-8 /MD /W3 /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE ^
   /Fo:bin\gui ^
   /Fe:bin\gui_tests.exe ^
   gui_tests.cpp ^
   /link /SUBSYSTEM:CONSOLE ^
   shell32.lib ole32.lib uuid.lib advapi32.lib shlwapi.lib user32.lib ^
   comctl32.lib comdlg32.lib shcore.lib wininet.lib
if errorlevel 1 exit /b 1
