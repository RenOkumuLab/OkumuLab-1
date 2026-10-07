# OkumuLab 1 - screen and audio performance test (Standalone, muted)
#
#   powershell -ExecutionPolicy Bypass -File tools\perftest.ps1 [-Exe "path\to\OkumuLab 1.exe"]
#
# Starts the Standalone with OKL_PERFTEST: the screen plays a scripted session by itself
# (chord at 2 s, knob sweeps 4-10 s, pad A1 7-8.5 s, release 10.5 s), then writes the report
# and quits (about 15 s). OKL_MUTE keeps the speakers silent. Keep the window visible:
# a window hidden behind others is not drawn, and other programs using the memory or the
# processor lower the frame rate.
param([string]$Exe = '')
$root = Split-Path -Parent $PSScriptRoot
if (-not $Exe) {
  $Exe = Join-Path $root 'dist\OkumuLab 1.exe'
  if (-not (Test-Path -LiteralPath $Exe)) { $Exe = Join-Path $root 'build\OkumuLab1_artefacts\Release\Standalone\OkumuLab 1.exe' }
}
$outDir = Join-Path $root 'test_out'
New-Item -ItemType Directory -Force $outDir | Out-Null
$rep = Join-Path $outDir ('perftest_' + (Get-Date -Format 'yyyyMMdd_HHmmss') + '.json')
$os = Get-CimInstance Win32_OperatingSystem
'free memory {0:N0} MB of {1:N0} MB' -f ($os.FreePhysicalMemory / 1KB), ($os.TotalVisibleMemorySize / 1KB)

$env:OKL_PERFTEST = $rep; $env:OKL_MUTE = '1'
$p = Start-Process -FilePath $Exe -PassThru
$env:OKL_PERFTEST = $null; $env:OKL_MUTE = $null
if (-not $p.WaitForExit(60000)) { Stop-Process -Id $p.Id -Force; 'the Standalone did not finish in 60 s' }
if (-not (Test-Path -LiteralPath $rep)) { 'no report'; exit 1 }

$r = Get-Content -LiteralPath $rep -Raw | ConvertFrom-Json
''
'screen  {0}x{1}: {2:N1} fps mean, {3:N1} fps lowest second, worst frame {4:N1} ms, {5} frames over 25 ms' -f `
  $r.screen.w, $r.screen.h, $r.fps.mean, $r.fps.min, $r.fps.worst_frame_ms, $r.fps.frames_over_25ms
$r.fps.per_second | ForEach-Object { '        ' + $_ }
'audio   {0} callbacks ({1:N0} Hz, {2} samples): overruns {3}, late {4}, max load {5:N2}, max gap {6:N2} blocks' -f `
  $r.audio.callbacks, $r.audio.sample_rate, $r.audio.block, $r.audio.overruns, $r.audio.late_callbacks, $r.audio.max_load, $r.audio.max_gap_blocks
'plugin -> page  {0:N2} ms per frame on the message thread (max {1:N2})' -f $r.cpp_send_ms_avg, $r.cpp_send_ms_max
$r.js_messages | Where-Object { $_ -like 'JS *' } | ForEach-Object { 'page    ' + $_ }
'report  ' + $rep
