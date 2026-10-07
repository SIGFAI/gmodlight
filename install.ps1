# Installs (or with -Uninstall removes) GModLight into Dying Light and Garry's Mod.
#
# Works from a release download (files\ next to this script) and from a source
# checkout after building (build\Release). Finds both games through Steam's
# library list; pass -DyingLight / -GarrysMod to point at them yourself.
param(
    [switch]$Uninstall,
    [string]$DyingLight = "",
    [string]$GarrysMod = ""
)
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot

# ---------- where the files come from ----------
$pkg = Join-Path $root "files"
if (Test-Path (Join-Path $pkg "GModLight.asi")) {
    # Release download.
    $asi = Join-Path $pkg "GModLight.asi"
    $module = Join-Path $pkg "gmcl_gmodlight_win64.dll"
    $ual = Join-Path $pkg "ultimate_asi_loader.dll"
    $ini = Join-Path $pkg "GModLight.ini"
    $addon = Join-Path $pkg "gmodlight"
} else {
    # Source checkout.
    $build = Join-Path $root "build\Release"
    $asi = Join-Path $build "GModLight.asi"
    $module = Join-Path $build "gmcl_gmodlight_win64.dll"
    $ual = Join-Path $root "third_party\asi_loader\dinput8.dll"
    $ini = Join-Path $root "dist\GModLight.ini"
    $addon = Join-Path $root "gmod_addon\gmodlight"
    if (-not $Uninstall -and -not (Test-Path $asi)) {
        throw "No build found. Build first (see README), or use a release download."
    }
}

# ---------- where the games are ----------
function Get-SteamLibraries {
    $libs = @()
    foreach ($key in "HKCU:\Software\Valve\Steam", "HKLM:\SOFTWARE\WOW6432Node\Valve\Steam") {
        try {
            $p = (Get-ItemProperty $key -ErrorAction Stop)
            $steam = if ($p.SteamPath) { $p.SteamPath } else { $p.InstallPath }
            if ($steam) { $libs += ($steam -replace "/", "\") }
        } catch {}
    }
    $libs += "C:\Program Files (x86)\Steam"
    foreach ($s in @($libs)) {
        $vdf = Join-Path $s "steamapps\libraryfolders.vdf"
        if (Test-Path $vdf) {
            foreach ($m in [regex]::Matches((Get-Content $vdf -Raw), '"path"\s+"([^"]+)"')) {
                $libs += ($m.Groups[1].Value -replace "\\\\", "\")
            }
        }
    }
    $libs | Select-Object -Unique
}

function Find-Game($folder, $exe) {
    foreach ($lib in Get-SteamLibraries) {
        $dir = Join-Path $lib "steamapps\common\$folder"
        if (Test-Path (Join-Path $dir $exe)) { return $dir }
    }
    return $null
}

if (-not $DyingLight) { $DyingLight = Find-Game "Dying Light" "DyingLightGame.exe" }
if (-not $GarrysMod) { $GarrysMod = Find-Game "GarrysMod" "gmod.exe" }
if (-not $DyingLight -or -not (Test-Path "$DyingLight\DyingLightGame.exe")) {
    throw "Dying Light not found. Run again with -DyingLight `"<its folder>`"."
}
if (-not $GarrysMod -or -not (Test-Path "$GarrysMod\gmod.exe")) {
    throw "Garry's Mod not found. Run again with -GarrysMod `"<its folder>`"."
}
"Dying Light: $DyingLight"
"Garry's Mod: $GarrysMod"

# The ASI loader goes in as xinput1_3.dll. (As dsound.dll it crashes Epic Online
# Services, which calls DirectSound functions the loader doesn't forward.)
$loaderName = "xinput1_3.dll"
$gmBin = Join-Path $GarrysMod "garrysmod\lua\bin"
$gmAddon = Join-Path $GarrysMod "garrysmod\addons\gmodlight"
$gmModules = @("gmcl_gmodlight_win64.dll", "gmsv_gmodlight_win64.dll")

function IsOurLoader($path) {
    (Test-Path $path) -and (Test-Path $ual) -and ((Get-FileHash $path).Hash -eq (Get-FileHash $ual).Hash)
}

foreach ($p in "DyingLightGame", "gmod") {
    if (Get-Process $p -ErrorAction SilentlyContinue) { throw "Close Dying Light and Garry's Mod first." }
}

# Earlier GModLight builds installed the loader as dsound.dll.
$oldLoader = Join-Path $DyingLight "dsound.dll"
if (IsOurLoader $oldLoader) { Remove-Item $oldLoader }

if ($Uninstall) {
    if (IsOurLoader (Join-Path $DyingLight $loaderName)) { Remove-Item (Join-Path $DyingLight $loaderName) }
    Remove-Item (Join-Path $DyingLight "GModLight.asi") -ErrorAction SilentlyContinue
    foreach ($f in $gmModules) { Remove-Item (Join-Path $gmBin $f) -ErrorAction SilentlyContinue }
    if (Test-Path $gmAddon) { Remove-Item $gmAddon -Recurse }
    "GModLight removed. (Your GModLight.ini and GModLight.log stay in the Dying Light folder.)"
    return
}

if (-not (Test-Path (Join-Path $GarrysMod "bin\win64\gmod.exe"))) {
    Write-Warning ("Garry's Mod isn't on the 64-bit branch. In Steam: Garry's Mod > Properties > Betas > " +
                   "choose 'x86-64 - Chromium + 64-bit binaries', let it update, then run this again.")
}

# Never replace a loader DLL that isn't ours (another mod loader, say).
$target = Join-Path $DyingLight $loaderName
if ((Test-Path $target) -and -not (IsOurLoader $target)) {
    throw "$target already exists and isn't the ASI loader GModLight uses. Move it away first."
}
Copy-Item $ual $target -Force
Copy-Item $asi $DyingLight -Force
# Keep the user's settings if they already have an ini.
if (-not (Test-Path "$DyingLight\GModLight.ini")) { Copy-Item $ini $DyingLight }

New-Item -ItemType Directory -Force $gmBin | Out-Null
foreach ($f in $gmModules) { Copy-Item $module (Join-Path $gmBin $f) -Force }
# A fresh copy, so files a newer version dropped don't linger.
if (Test-Path $gmAddon) { Remove-Item $gmAddon -Recurse -Force }
Copy-Item $addon $gmAddon -Recurse

"GModLight installed. Start Dying Light from Steam (single player); press F2 in game."
