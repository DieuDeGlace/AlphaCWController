@echo off
setlocal
cd /d "%~dp0"
if not exist build\Mods mkdir build\Mods
if not exist build\tests mkdir build\tests
pushd src
cl /nologo /std:c++17 /O2 /MT /EHsc /DUNICODE /D_UNICODE /DALPHA_CONTROLLER_DLL /LD AlphaController.cpp /Fe..\build\Mods\AlphaController.dll /link user32.lib shell32.lib
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /O2 /MT /EHsc /DUNICODE /D_UNICODE AlphaController.cpp /Fe..\build\tests\AlphaController.exe /link /SUBSYSTEM:WINDOWS user32.lib shell32.lib
set "result=%ERRORLEVEL%"
popd
exit /b %result%
