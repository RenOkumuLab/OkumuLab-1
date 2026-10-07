param([string[]]$Secs)
$j = Split-Path -Parent $PSScriptRoot          # this project folder (tools\..)
New-Item -ItemType Directory -Force "$j\test_out\sections" | Out-Null
foreach ($s in $Secs) {
  $t0 = Get-Date
  $p = Start-Process -FilePath "$j\build-dsp\labium_check.exe" -ArgumentList $s -PassThru -NoNewWindow -RedirectStandardOutput "$j\test_out\sections\$s.txt" -RedirectStandardError "$j\test_out\sections\${s}_err.txt"
  $peak = 0
  while (-not $p.HasExited) {
    try { $p.Refresh(); $m = $p.PrivateMemorySize64; if ($m -gt $peak) { $peak = $m } } catch {}
    if ($peak -gt 2GB) { Stop-Process -Id $p.Id -Force; "$s stopped: private memory over 2 GB"; break }
    if (((Get-Date) - $t0).TotalMinutes -gt 40) { Stop-Process -Id $p.Id -Force; "$s stopped after 40 min"; break }
    Start-Sleep -Milliseconds 300
  }
  $p.WaitForExit()
  $res = (Get-Content "$j\test_out\sections\$s.txt" -Encoding utf8 | Select-String -Pattern '^OK|^FAILED' | Select-Object -Last 1).Line
  "{0,-10} {1,7:N0} s  peak private {2,7:N0} MB  {3}" -f $s, ((Get-Date) - $t0).TotalSeconds, ($peak / 1MB), $res
}
