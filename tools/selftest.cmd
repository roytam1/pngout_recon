@echo off
rem ===========================================================================
rem  Build and run the unit tests for the reconstructed support code.
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

rem test_support.c stands in for the few globals the chunk layer needs so the
rem test can link without dragging in main() or the optimiser stub.
cl.exe /nologo /MD /W3 /D_CRT_SECURE_NO_DEPRECATE /I"%ROOT%\src" /c ^
    "%ROOT%\tools\test_chunk.c" ^
    "%ROOT%\tools\test_support.c" ^
    "%ROOT%\src\chunk.c" ^
    "%ROOT%\src\predict.c" ^
    "%ROOT%\src\cpuid.c" ^
    "%ROOT%\src\optimize.c" ^
    "%ROOT%\src\lz.c" ^
    "%ROOT%\src\globals.c" ^
    "%ROOT%\src\stubs.c" > test.log 2>&1
if errorlevel 1 goto :failed

link.exe /nologo /OUT:test_chunk.exe ^
    test_chunk.obj test_support.obj chunk.obj ^
    predict.obj cpuid.obj optimize.obj lz.obj globals.obj stubs.obj ^
    user32.lib gdi32.lib >> test.log 2>&1
if errorlevel 1 goto :failed

echo running tests
echo.
test_chunk.exe
set "RC=%ERRORLEVEL%"

popd
popd
endlocal & exit /b %RC%

:failed
echo BUILD FAILED
type test.log
popd
popd
endlocal & exit /b 1