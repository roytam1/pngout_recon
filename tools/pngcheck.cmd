@echo off
rem ===========================================================================
rem  Round-trip check: run Optimize() over a set of images and hand the
rem  results to tools\pngcheck.py, which inflates them with zlib and compares
rem  every sample against the source pixels.
rem
rem    tools\pngcheck.cmd            build pngcheck.exe if it is missing,
rem                                   run it, then verify
rem    tools\pngcheck.cmd build      just build pngcheck.exe
rem    tools\pngcheck.cmd run        skip the build
rem
rem  The unit test in tools\selftest.cmd checks Optimize()'s output
rem  structurally.  This checks it actually inflates and carries the right
rem  pixels, which is the failure mode a structural check cannot see: a
rem  perfectly shaped file with a bad filter, a broken Huffman code or a wrong
rem  channel order still looks right to a byte comparison.
rem ===========================================================================
setlocal

set "VS=C:\Program Files (x86)\Microsoft Visual Studio"
set "VC=%VS%\VC98"
rem Two separate `set`s, one variable per line.  Putting both of these on one
rem line makes cmd.exe mis-pair the percent signs when the value contains
rem parentheses, and cl.exe then fails with STATUS_DLL_NOT_FOUND and no
rem diagnostic at all - mspdb60.dll, in Common\MSDev98\Bin, never gets found.
set "PATH=%VC%\BIN;%PATH%"
set "PATH=%VS%\Common\MSDev98\Bin;%PATH%"
set "INCLUDE=%VC%\INCLUDE"
set "LIB=%VC%\LIB"

pushd "%~dp0.."
set "ROOT=%CD%"
if not exist build mkdir build
pushd build

if /i "%~1"=="run" goto :run
if /i "%~1"=="build" goto :build
if exist pngcheck.exe goto :run

rem test_support.c stands in for the few globals chunk.c wants, so the driver
rem links without dragging in main() and its command line parser.
rem
rem cl.exe and link.exe are run as two steps, the same way tools\build.cmd does
rem it: asking cl.exe to link in one go fails on this machine with
rem STATUS_DLL_NOT_FOUND and no diagnostic at all.
:build
cl.exe /nologo /c /MD /W3 /O2 /D_CRT_SECURE_NO_DEPRECATE /I"%ROOT%\src" ^
    "%ROOT%\src\trials.c" "%ROOT%\src\optimize.c" "%ROOT%\src\lz.c" ^
    "%ROOT%\src\globals.c" "%ROOT%\src\chunk.c" "%ROOT%\src\predict.c" ^
    "%ROOT%\src\cpuid.c" "%ROOT%\src\stubs.c" ^
    "%ROOT%\tools\pngcheck.c" "%ROOT%\tools\test_support.c" > pngbuild.log 2>&1
if errorlevel 1 goto :failed

link.exe /nologo /SUBSYSTEM:CONSOLE /OUT:pngcheck.exe ^
    trials.obj optimize.obj lz.obj globals.obj chunk.obj predict.obj cpuid.obj stubs.obj ^
    pngcheck.obj test_support.obj ^
    user32.lib gdi32.lib >> pngbuild.log 2>&1
if errorlevel 1 goto :failed
if /i "%~1"=="build" goto :done

:run
del /q png_*.png png_*.ref >nul 2>&1
pngcheck.exe > pngcheck.log 2>&1
if errorlevel 1 goto :driverfail
type pngcheck.log

python "%ROOT%\tools\pngcheck.py"
set "RC=%ERRORLEVEL%"

popd
popd
endlocal & exit /b %RC%

:done
popd
popd
endlocal & exit /b 0

:failed
echo BUILD FAILED
type pngbuild.log
popd
popd
endlocal & exit /b 2

:driverfail
echo.
echo the driver itself failed
type pngcheck.log
popd
popd
endlocal & exit /b 3