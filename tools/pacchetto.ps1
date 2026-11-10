# pacchetto.ps1 - builds the release zip: version.dll, SacredPad.ini, README, LICENSE, CHANGELOG, CONTROLS, TESTING
# Usage, from the repository root after building Release x86:
#   powershell -ExecutionPolicy Bypass -File tools\pacchetto.ps1
param([string]$Out = "pacchetto")

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$dll = Join-Path $root 'Release\version.dll'
$ini = Join-Path $root 'SacredPad\SacredPad.ini'
if (-not (Test-Path $dll)) { throw "Missing ${dll}: build Release x86 first" }

$text = Get-Content $ini -Raw
foreach ($want in @('PadSource=Auto', 'MemoryDiag=false', 'KeyLog=false')) {
    if ($text -notmatch "(?m)^$([regex]::Escape($want))\s*$") { throw "SacredPad.ini is not clean: ${want} missing" }
}

# version: SACREDPAD_VERSION in SacredPad.h, and the DLL must contain it
$hdr = Get-Content (Join-Path $root 'SacredPad\SacredPad.h') -Raw
$m = [regex]::Match($hdr, '#define SACREDPAD_VERSION "([^"]+)"')
if (-not $m.Success) { throw "SACREDPAD_VERSION not found" }
$full = $m.Groups[1].Value
$version = ($full -split ' ')[0]
$bytes = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($dll))
if (-not $bytes.Contains($full)) { throw "version.dll does not contain version '$full': rebuild" }
# no path of the build PC inside the DLL (the project links with /PDBALTPATH:%_PDB%, so only "version.pdb" is left)
if ($bytes -match '(?i)[a-z]:\\users\\') { throw "version.dll contains a user folder path (PDB path?): check /PDBALTPATH in the project" }

$dir = Join-Path $root $Out
if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }
New-Item -ItemType Directory -Force $dir | Out-Null
Copy-Item $dll $dir
Copy-Item $ini $dir
foreach ($f in @('README.md', 'LICENSE', 'CHANGELOG.md', 'TESTING.md', 'docs\CONTROLS.md')) { Copy-Item (Join-Path $root $f) $dir }
$zip = Join-Path $root "SacredPad-TwoHeroes-$version.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path (Join-Path $dir '*') -DestinationPath $zip
Write-Output "Package: $zip ($full)"
