@echo off
rem ===========================================================================
rem  Regenerate reference/ from the two IDA exports.  Only needed once;
rem  everything under reference\ is derived data.
rem
rem  The .lst is 924MB, of which ~22M lines are the byte dump of the 22MB
rem  .data section, so the scan takes a few minutes.
rem ===========================================================================
setlocal
pushd "%~dp0.."

python tools\scan_landmarks.py > build\landmarks.txt 2>&1
python tools\extract_lst.py
python tools\split_fn.py
python tools\split_decomp.py
python tools\bss_refs.py
python tools\callgraph.py > build\callgraph.txt 2>&1

popd
endlocal
echo reference\ regenerated