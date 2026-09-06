@echo off
setlocal
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT exit /b 2
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b %errorlevel%
pushd "%~dp0.."
if not exist build mkdir build
cl /nologo /std:c11 /W4 /WX /DCLIMATE_HOST_TEST main\heater_climate_policy.c main\heater_climate_selftest.c /Fo:build\ /Fe:build\climate-policy-test.exe
if errorlevel 1 (popd & exit /b 1)
build\climate-policy-test.exe
if errorlevel 1 (popd & exit /b 1)
cl /nologo /std:c11 /W4 /WX main\heater_policy.c tools\heater-policy-host.c /Fo:build\ /Fe:build\heater-policy-test.exe
if errorlevel 1 (popd & exit /b 1)
build\heater-policy-test.exe
set "RESULT=%errorlevel%"
popd
exit /b %RESULT%
