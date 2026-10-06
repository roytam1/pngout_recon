@echo off
rem ===========================================================================
rem  Run a command inside the repository, mapping the UNC share to a drive
rem  first because cmd.exe cannot chdir into a UNC path.  The command's exit
rem  code is propagated, so this can be used from scripts.
rem
rem    tools\run.cmd dumpbin /imports build\pngout.exe
rem ===========================================================================
setlocal
pushd "%~dp0.."
%*
set "RC=%ERRORLEVEL%"
popd
endlocal & exit /b %RC%