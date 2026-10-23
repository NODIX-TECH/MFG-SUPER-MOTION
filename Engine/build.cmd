@echo off
rem The MFG SUPER MOTION engine (out\nvngx.dll - an executable; NGX's runtimes
rem check that file name of their caller) and the files it runs with.
setlocal
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
if not exist out mkdir out
if not exist obj mkdir obj
cl /nologo /O2 /EHsc /W3 /MD /utf-8 /std:c++17 /Iinclude /Iinclude\ort sm_engine.cpp /Foobj\ /Fe:out\nvngx.dll /link lib\nvsdk_ngx_d.lib version.lib kernel32.lib user32.lib gdi32.lib advapi32.lib ole32.lib d3d11.lib d3d12.lib dxgi.lib d3dcompiler.lib WindowsApp.lib dwmapi.lib setupapi.lib
if errorlevel 1 exit /b 1
del /q out\nvngx.exp out\nvngx.lib 2>nul
xcopy /y /q /d runtime\*.dll out\ >nul
if not exist out\dlssg_compat mkdir out\dlssg_compat
xcopy /y /q /d runtime\dlssg_compat\* out\dlssg_compat\ >nul
if not exist out\xess mkdir out\xess
for %%f in (libxess_fg.dll libxell.dll XeFGUnlock.asi XeFGUnlock.ini) do copy /y runtime\xess\%%f out\xess\%%f >nul
rem SMFG AI (msm_ai.h): ONNX Runtime, DirectML and the network, with their licenses.
if not exist out\ai mkdir out\ai
xcopy /y /q /d runtime\ai\* out\ai\ >nul
exit /b 0
