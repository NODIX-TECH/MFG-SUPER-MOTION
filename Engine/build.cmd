@echo off
rem The MFG SUPER MOTION engine (nvngx.dll - an executable; NGX's runtimes check that file name of their caller)
rem and the files it runs with, built from the sources in this folder.
rem   build.cmd                 into work\build
rem   build.cmd work\r86-out    into the folder named (relative to this one, or a whole path)
rem Never into out\ over an engine already there (FIX PACING.md, rule 3): an engine goes there only through
rem install-engine.ps1, after the gate. (A fresh copy of the project with no engine at all: the app's build makes it there.)
setlocal
cd /d "%~dp0"
set "OUT=%~1"
if "%OUT%"=="" set "OUT=work\build"
if /i "%OUT%"=="out" if exist out\nvngx.dll (echo build.cmd: not over the engine in out\ - build elsewhere, then install-engine.ps1 & exit /b 2)
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
if not exist "%OUT%" mkdir "%OUT%"
if not exist obj mkdir obj
cl /nologo /O2 /EHsc /W3 /MD /utf-8 /std:c++17 /Iinclude /Iinclude\ort sm_engine.cpp /Foobj\ /Fe:"%OUT%\nvngx.dll" /link lib\nvsdk_ngx_d.lib version.lib kernel32.lib user32.lib gdi32.lib advapi32.lib ole32.lib d3d11.lib d3d12.lib dxgi.lib d3dcompiler.lib WindowsApp.lib dwmapi.lib setupapi.lib
if errorlevel 1 exit /b 1
del /q "%OUT%\nvngx.exp" "%OUT%\nvngx.lib" 2>nul
xcopy /y /q /d runtime\*.dll "%OUT%\" >nul
if not exist "%OUT%\dlssg_compat" mkdir "%OUT%\dlssg_compat"
xcopy /y /q /d runtime\dlssg_compat\* "%OUT%\dlssg_compat\" >nul
if not exist "%OUT%\xess" mkdir "%OUT%\xess"
for %%f in (libxess_fg.dll libxell.dll XeFGUnlock.asi XeFGUnlock.ini) do copy /y runtime\xess\%%f "%OUT%\xess\%%f" >nul
rem SMFG AI (msm_ai.h): ONNX Runtime, DirectML and the network, with their licenses.
if not exist "%OUT%\ai" mkdir "%OUT%\ai"
xcopy /y /q /d runtime\ai\* "%OUT%\ai\" >nul
exit /b 0
