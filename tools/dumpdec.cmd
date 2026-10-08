@echo off
rem ===========================================================================
rem  Build build\dumpdec.exe, the standalone decoder probe.
rem
rem    tools\dumpdec.cmd          build it
rem    tools\dumpdec.cmd clean    remove the objects
rem ===========================================================================
setlocal
set "VS=C:\Program Files (x86)\Microsoft Visual Studio"
set "VC=%VS%\VC98"
set "PATH=%VC%\BIN;%VS%\Common\MSDev98\Bin;%PATH%"
set "INCLUDE=%VC%\INCLUDE"
set "LIB=%VC%\LIB"

pushd "%~dp0.."
set "ROOT=%CD%"
if not exist build mkdir build
pushd build

if /i "%~1"=="clean" goto :clean

cl.exe /nologo /MD /W3 /Od /D_CRT_SECURE_NO_DEPRECATE /I"%ROOT%\src" /c ^
    "%ROOT%\tools\dumpdec.c" ^
    "%ROOT%\tools\test_support.c" ^
    "%ROOT%\src\chunk.c" ^
    "%ROOT%\src\predict.c" ^
    "%ROOT%\src\cpuid.c" ^
    "%ROOT%\src\optimize.c" ^
    "%ROOT%\src\lz.c" ^
    "%ROOT%\src\trials.c" ^
    "%ROOT%\src\globals.c" ^
    "%ROOT%\src\stubs.c" ^
    "%ROOT%\src\decode.c" > dumpdec.log 2>&1
if errorlevel 1 goto :failed

link.exe /nologo /OUT:dumpdec.exe ^
    dumpdec.obj test_support.obj chunk.obj predict.obj cpuid.obj ^
    optimize.obj lz.obj trials.obj globals.obj stubs.obj decode.obj ^
    user32.lib gdi32.lib >> dumpdec.log 2>&1
if errorlevel 1 goto :failed

echo build ok - build\dumpdec.exe
type dumpdec.log
popd
popd
endlocal
exit /b 0

:clean
del /q dumpdec.obj dumpdec.exe dumpdec.log 2>nul
popd
popd
endlocal
exit /b 0

:failed
echo BUILD FAILED
type dumpdec.log
popd
popd
endlocal
exit /b 1