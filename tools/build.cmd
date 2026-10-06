@echo off
rem ===========================================================================
rem  Build pngout.exe with Visual C++ 6.0.
rem
rem    tools\build.cmd            build everything
rem    tools\build.cmd clean      remove objects
rem
rem  The repository may live on a UNC share, which cmd.exe cannot chdir into;
rem  pushd maps it to a temporary drive for us.
rem ===========================================================================
setlocal

set "VS=C:\Program Files (x86)\Microsoft Visual Studio"
set "VC=%VS%\VC98"
set "PATH=%VC%\BIN;%VS%\Common\MSDev98\Bin;%PATH%"
set "INCLUDE=%VC%\INCLUDE"
set "LIB=%VC%\LIB"
set "LIB32="

pushd "%~dp0.."
set "ROOT=%CD%"
pushd build

if /i "%~1"=="clean" goto :clean

rem Compile only the real translation units; tools\selftest.cmd leaves its
rem own objects here and they must not end up in the program.
cl.exe /nologo /c /W3 /O2 /MD /D_CRT_SECURE_NO_DEPRECATE /I"%ROOT%\src" ^
    "%ROOT%\src\pngout.c" "%ROOT%\src\chunk.c" "%ROOT%\src\clipboard.c" ^
    "%ROOT%\src\globals.c" "%ROOT%\src\stubs.c" "%ROOT%\src\predict.c" "%ROOT%\src\cpuid.c" "%ROOT%\src\optimize.c" > build.log 2>&1
if errorlevel 1 goto :failed

link.exe /nologo /SUBSYSTEM:CONSOLE /OUT:pngout.exe ^
    pngout.obj chunk.obj clipboard.obj globals.obj stubs.obj predict.obj cpuid.obj optimize.obj ^
    user32.lib gdi32.lib >> build.log 2>&1
if errorlevel 1 goto :failed

echo build ok - build\pngout.exe
type build.log
popd
popd
endlocal
exit /b 0

:failed
echo BUILD FAILED
type build.log
popd
popd
endlocal
exit /b 1

:clean
del /q *.obj *.exe *.idb *.pdb *.map *.res 2>nul
popd
popd
endlocal
exit /b 0