# Publishes a new version: sets VERSION, adds a CHANGELOG.md entry, commits,
# tags and pushes. GitHub Actions then builds the zip and creates the release.
#
#   powershell -ExecutionPolicy Bypass -File tools\release.ps1 0.1.1 "Fixed grenade drift" "Smoother physgun"
#
# Each extra argument becomes one line of the changelog entry.
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$Notes
)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root

if ($Version -notmatch '^\d+\.\d+\.\d+$') { throw "Version must look like 0.1.1" }
if (git tag --list "v$Version") { throw "v$Version already exists" }
if ((git rev-parse --abbrev-ref HEAD) -ne "main") { throw "Release from the main branch" }

Set-Content VERSION $Version -NoNewline

$log = Get-Content CHANGELOG.md -Raw
if ($log -notmatch "(?m)^## $([regex]::Escape($Version))\s*$") {
    $lines = if ($Notes) { ($Notes | ForEach-Object { "- $_" }) -join "`r`n" } else { "- Fixes and improvements." }
    $log = $log -replace "^# Changelog\s*", "# Changelog`r`n`r`n## $Version`r`n`r`n$lines`r`n`r`n"
    Set-Content CHANGELOG.md $log -NoNewline
}

git add -A
git commit -m "Release v$Version"
if ($LASTEXITCODE) { throw "commit failed" }
git tag "v$Version"
git push origin main
git push origin "v$Version"
"Pushed v$Version. GitHub Actions is building it; the release appears on the repository's Releases page in a few minutes."
