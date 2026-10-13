@echo off
rem ===========================================================================
rem  Build build\dumpsteps.exe, tools\dumpsteps.c.
rem ===========================================================================
setlocal
set "VS=C:\Program Files (x86)\Microsoft Visual Studio"
set "VC=%VS%\VC98"
set "PATH=%VC%\BIN;%VS%\Common\MSDev98\Bin;%PATH%"
set "INCLUDE=%VC%\INCLUDE"
set "LIB=%VC%\LIB"
pushd "%~dp0.."
set "ROOT=%CD%"
pushd build
cl.exe /nologo /c /W3 /O2 /D_CRT_SECURE_NO_DEPRECATE "%ROOT%\tools\dumpsteps.c" > ds.log 2>&1
if errorlevel 1 goto :failed
link.exe /nologo /SUBSYSTEM:CONSOLE /OUT:dumpsteps.exe dumpsteps.obj >> ds.log 2>&1
if errorlevel 1 goto :failed
echo build ok - build\dumpsteps.exe
popd
popd
endlocal
exit /b 0
:failed
echo BUILD FAILED
type ds.log
popd
popd
endlocal
exit /b 1
