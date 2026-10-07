# Builds the download people install from: out\GModLight-<version>.zip, holding
#   Install.bat / Uninstall.bat / install.ps1   (double-click to install)
#   files\                                      (what gets copied into the games)
#   README.txt, LICENSE, THIRD_PARTY_NOTICES.md, CHANGELOG.md
#
#   powershell -ExecutionPolicy Bypass -File tools\package.ps1            # build, then package
#   powershell -ExecutionPolicy Bypass -File tools\package.ps1 -NoBuild   # package an existing build
# GitHub Actions runs this for every release (.github/workflows/build.yml).
param([switch]$NoBuild)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$version = (Get-Content (Join-Path $root "VERSION") -Raw).Trim()
$build = Join-Path $root "build"

if (-not $NoBuild) {
    cmake -S $root -B $build -A x64  # newest Visual Studio installed
    if ($LASTEXITCODE) { throw "cmake configure failed" }
    cmake --build $build --config Release --target GModLight gmodlight_module
    if ($LASTEXITCODE) { throw "build failed" }
}
$rel = Join-Path $build "Release"
foreach ($f in "GModLight.asi", "gmcl_gmodlight_win64.dll") {
    if (-not (Test-Path (Join-Path $rel $f))) { throw "$f not built" }
}

$name = "GModLight-$version"
$out = Join-Path $root "out"
$stage = Join-Path $out $name
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
$files = Join-Path $stage "files"
New-Item -ItemType Directory -Force $files | Out-Null

Copy-Item (Join-Path $rel "GModLight.asi") $files
Copy-Item (Join-Path $rel "gmcl_gmodlight_win64.dll") $files
Copy-Item (Join-Path $root "third_party\asi_loader\dinput8.dll") (Join-Path $files "ultimate_asi_loader.dll")
Copy-Item (Join-Path $root "dist\GModLight.ini") $files
Copy-Item (Join-Path $root "gmod_addon\gmodlight") (Join-Path $files "gmodlight") -Recurse

foreach ($f in "install.ps1", "Install.bat", "Uninstall.bat", "LICENSE", "THIRD_PARTY_NOTICES.md", "CHANGELOG.md") {
    Copy-Item (Join-Path $root $f) $stage
}
Copy-Item (Join-Path $root "dist\README.txt") (Join-Path $stage "README.txt")

$zip = Join-Path $out "$name.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path $stage -DestinationPath $zip
"Packaged $zip"
