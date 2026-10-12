@echo off
call "D:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
msbuild "d:\Users\Desktop\code\SystemCleaner\SystemCleaner.sln" /t:Build /p:Configuration=%1 /p:Platform=%2 /m /nologo /v:m
