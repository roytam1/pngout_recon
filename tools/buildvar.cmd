@echo off
rem ===========================================================================
rem  Build pngout.exe into a variant directory with extra defines, so an
rem  instrumented build does not disturb build\pngout.exe.
rem
rem    tools\buildvar.cmd <name> [/D<DEF> ...]
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
set "NAME=%~1"
shift
set "DEFS="
:args
if "%~1"=="" goto :argsdone
set "DEFS=%DEFS% %1"
shift
goto :args

:argsdone
if not exist "build\%NAME%" mkdir "build\%NAME%"
pushd "build\%NAME%"

cl.exe /nologo /c /W3 /O2 /G6 /MD /D_CRT_SECURE_NO_DEPRECATE%DEFS% /I"%ROOT%\src" ^
    "%ROOT%\src\pngout.c" "%ROOT%\src\chunk.c" "%ROOT%\src\clipboard.c" ^
    "%ROOT%\src\globals.c" "%ROOT%\src\stubs.c" "%ROOT%\src\decode.c" "%ROOT%\src\predict.c" "%ROOT%\src\cpuid.c" "%ROOT%\src\optimize.c" "%ROOT%\src\lz.c" "%ROOT%\src\trials.c" ^
    "%ROOT%\tools\lzfootprint.c" > build.log 2>&1
if errorlevel 1 goto :failed

link.exe /nologo /SUBSYSTEM:CONSOLE /OUT:pngout.exe ^
    pngout.obj chunk.obj clipboard.obj globals.obj stubs.obj decode.obj predict.obj cpuid.obj optimize.obj lz.obj trials.obj lzfootprint.obj ^
    user32.lib gdi32.lib >> build.log 2>&1
if errorlevel 1 goto :failed

echo build ok - build\%NAME%\pngout.exe
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
