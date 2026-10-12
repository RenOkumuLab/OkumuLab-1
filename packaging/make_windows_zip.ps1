# OkumuLab 1 - the Windows package from the build of build.bat: dist\OkumuLab1-<version>-win64.zip
#   powershell -ExecutionPolicy Bypass -File packaging\make_windows_zip.ps1 [-Out <folder>]
# Contents: the VST3 bundle, the standalone app, LICENSE, THIRD_PARTY_NOTICES.md, licenses\ (without the Audio Unit
# SDK's, macOS only) and README.txt (packaging\README_Windows.txt with the version of CMakeLists.txt).
# The zip's entries use "/" so that every unzip tool reads them.
param([string]$Out = "")
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $Out) { $Out = Join-Path $root 'dist' }
$ver = [regex]::Match([IO.File]::ReadAllText((Join-Path $root 'CMakeLists.txt')), 'project\(OkumuLab1 VERSION ([0-9.]+)').Groups[1].Value
if (-not $ver) { throw 'no version in CMakeLists.txt' }
$a = Join-Path $root 'build\OkumuLab1_artefacts\Release'
$name = "OkumuLab1-$ver-win64"
$d = Join-Path $Out $name
$zip = Join-Path $Out "$name.zip"
if ((Test-Path $d) -or (Test-Path $zip)) { throw "$d or its zip exists already: move it away first" }
New-Item -ItemType Directory -Force $d | Out-Null

Copy-Item -Recurse -LiteralPath (Join-Path $a 'VST3\OkumuLab 1.vst3') -Destination $d
Copy-Item -LiteralPath (Join-Path $a 'Standalone\OkumuLab 1.exe') -Destination $d
Copy-Item -LiteralPath (Join-Path $root 'LICENSE'), (Join-Path $root 'THIRD_PARTY_NOTICES.md') -Destination $d
Copy-Item -Recurse -LiteralPath (Join-Path $root 'licenses') -Destination $d
Remove-Item -LiteralPath (Join-Path $d 'licenses\AudioUnitSDK-LICENSE.txt')
$readme = [IO.File]::ReadAllText((Join-Path $root 'packaging\README_Windows.txt'), [Text.Encoding]::UTF8)
$readme = $readme.Replace("`r`n", "`n").Replace('@VERSION@', $ver).Replace("`n", "`r`n")
[IO.File]::WriteAllText((Join-Path $d 'README.txt'), $readme, (New-Object Text.UTF8Encoding($true)))

Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$fs = [IO.File]::Open($zip, [IO.FileMode]::CreateNew)
$z = New-Object IO.Compression.ZipArchive($fs, [IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($f in Get-ChildItem -LiteralPath $d -Recurse -File) {
        $rel = $name + '/' + $f.FullName.Substring($d.Length + 1).Replace('\', '/')
        [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($z, $f.FullName, $rel, [IO.Compression.CompressionLevel]::Optimal)
    }
} finally { $z.Dispose(); $fs.Dispose() }
Get-Item -LiteralPath $zip | ForEach-Object { '{0}  {1:N0} bytes' -f $_.FullName, $_.Length }
