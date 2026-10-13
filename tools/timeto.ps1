param([int[]]$Positions)
# Wall time for both binaries to reach a given scan position (and no further).
# The freeze + aux read are a constant overhead in both, so the *ratio* is what
# matters.  Fresh copies every run: pngout rewrites its input in place.
$ErrorActionPreference = 'Stop'
$T = 'C:\Users\Roy\AppData\Local\Temp\opencode'
$ROOT = '\\Roy-xp\c\devel\pngout-reconstruct'

foreach ($N in $Positions) {
    Copy-Item "$ROOT\150650258_p0.png" "$T\tm_o.png" -Force
    Copy-Item "$ROOT\150650258_p0.png" "$T\tm_r.png" -Force

    $env:DUMPSCAN = [string]($N - 200)
    $env:DUMPAUX = "$T\tm_o.bin"
    $sw = [Diagnostics.Stopwatch]::StartNew()
    cmd /c "$ROOT\build\dumpsteps.exe $ROOT\pngout_original.exe $T\tm_o.png /b0 2> $T\tm_o.log" | Out-Null
    $sw.Stop()
    $otime = $sw.Elapsed.TotalSeconds
    Remove-Item Env:\DUMPSCAN, Env:\DUMPAUX

    $env:DUMPPOS = [string]$N
    $env:DUMPPOSAT = '1'
    $env:DUMPAUXONLY = '1'
    Remove-Item "$T\lzdump_aux_$N.bin" -ErrorAction SilentlyContinue
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $proc = Start-Process -FilePath "$ROOT\build\dumpstate\pngout.exe" `
            -ArgumentList "$T\tm_r.png /b0" -WorkingDirectory $T -PassThru
    $deadline = (Get-Date).AddSeconds(1800)
    while ((Get-Date) -lt $deadline) {
        if (Test-Path "$T\lzdump_aux_$N.bin") { Start-Sleep 4; break }
        if ($proc.HasExited) { break }
        Start-Sleep 3
    }
    $sw.Stop()
    if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
    Remove-Item Env:\DUMPPOS, Env:\DUMPPOSAT, Env:\DUMPAUXONLY

    $frozen = (Select-String -Path "$T\tm_o.log" -Pattern 'frozen scan=(\d+)' |
               Select-Object -First 1).Matches[0].Groups[1].Value
    "{0,9}  orig {1,8:N1}s (scan {2})  recon {3,8:N1}s  ratio {4,5:N2}" -f `
        $N, $otime, $frozen, $sw.Elapsed.TotalSeconds, ($sw.Elapsed.TotalSeconds / $otime)
}
