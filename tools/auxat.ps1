param([int]$Pos, [string]$Tag = "p$Pos")
# Dump the recon's aux at position Pos (loop top) and the original's aux frozen
# just past it, from *fresh copies* (pngout rewrites its input in place), then
# compare by position.  The freeze overshoots by a few hundred positions, which
# the by-position comparator tolerates.
$ErrorActionPreference = 'Stop'
$T = 'C:\Users\Roy\AppData\Local\Temp\opencode'
$ROOT = '\\Roy-xp\c\devel\pngout-reconstruct'

Copy-Item "$ROOT\150650258_p0.png" "$T\$Tag`_o.png" -Force
Copy-Item "$ROOT\150650258_p0.png" "$T\$Tag`_r.png" -Force

$env:DUMPSCAN = [string]($Pos - 300)
$env:DUMPAUX = "$T\$Tag`_o.bin"
cmd /c "$ROOT\build\dumpsteps.exe $ROOT\pngout_original.exe $T\$Tag`_o.png /b0 2> $T\$Tag`_o.log" | Out-Null
Remove-Item Env:\DUMPSCAN, Env:\DUMPAUX
$frozen = (Select-String -Path "$T\$Tag`_o.log" -Pattern 'frozen scan=(\d+)' |
           Select-Object -First 1).Matches[0].Groups[1].Value
Write-Host "orig frozen at scan $frozen"

Remove-Item "$T\lzdump_aux_$($Pos+1).bin" -ErrorAction SilentlyContinue
$env:DUMPPOS = [string]($Pos + 1)
$env:DUMPPOSAT = '1'
$proc = Start-Process -FilePath "$ROOT\build\dumpstate\pngout.exe" `
        -ArgumentList "$T\$Tag`_r.png /b0" -WorkingDirectory $T -PassThru
$deadline = (Get-Date).AddSeconds(900)
while ((Get-Date) -lt $deadline) {
    if (Test-Path "$T\lzdump_aux_$($Pos+1).bin") { Start-Sleep 5; break }
    if ($proc.HasExited) { break }
    Start-Sleep 4
}
if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
Remove-Item Env:\DUMPPOS, Env:\DUMPPOSAT
if (-not (Test-Path "$T\lzdump_aux_$($Pos+1).bin")) { Write-Host "RECON DUMP MISSING"; exit 1 }

python "$ROOT\tools\auxcmp.py" "$T\$Tag`_o.bin" $frozen "$T\lzdump_aux_$($Pos+1).bin" ($Pos+1)
