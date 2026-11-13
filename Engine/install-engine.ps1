# install-engine.ps1 (round 85) - the one way an engine goes into Engine\out and the app's Debug folder (FIX PACING.md,
# rule 3): only an nvngx.dll whose hash pacing-gate.ps1 wrote down as a whole PASS (tests\gate-passed.txt), or one the
# owner said to install - then -OwnerWord '<what they said>' is required and written down beside it. The engine that was
# there is kept in Engine\archive\installed\<date> first, so going back is one call:
#   .\install-engine.ps1 -From work\build                                  (a build that passed the gate)
#   .\install-engine.ps1 -From work\build -OwnerWord 'install it, I try it' (the owner's word)
#   .\install-engine.ps1 -Back                                             (the engine installed before this one)
param([string]$From = '', [string]$OwnerWord = '', [switch]$Back)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$out = Join-Path $here 'out'
$debug = Join-Path $here '..\ARMO\bin\Debug\net8.0\Engine'
$kept = Join-Path $here 'archive\installed'
$record = Join-Path $here 'tests\gate-passed.txt'

# An engine still running holds its file: nothing is copied under it.
$running = Get-CimInstance Win32_Process -Filter "Name = 'nvngx.dll'" -ErrorAction SilentlyContinue |
    Where-Object { $_.ExecutablePath -and ($_.ExecutablePath.StartsWith($out, 'OrdinalIgnoreCase') -or $_.ExecutablePath -like '*\Debug\net8.0\Engine\*') }
if ($running) { throw "The engine is running ($($running.ProcessId -join ', ')) - stop it in the app first." }

if ($Back) {
    $last = Get-ChildItem $kept -Directory -ErrorAction SilentlyContinue | Sort-Object Name | Select-Object -Last 1
    if (-not $last) { throw "Nothing kept in $kept to go back to." }
    $From = $last.FullName
    $OwnerWord = "back to the engine kept in $($last.Name)"
}
if (-not $From) { throw 'Give -From <folder with nvngx.dll>, or -Back.' }
if (-not [IO.Path]::IsPathRooted($From)) { $From = Join-Path $here $From }
$engine = Join-Path $From 'nvngx.dll'
if (-not (Test-Path $engine)) { throw "No nvngx.dll in $From." }
$hash = (Get-FileHash $engine -Algorithm SHA256).Hash
$passed = (Test-Path $record) -and (Select-String -Path $record -Pattern $hash -SimpleMatch -Quiet)
if (-not $passed -and -not $OwnerWord) {
    throw "This engine ($hash) has no PASS in tests\gate-passed.txt. Run tests\pacing-gate.ps1 on it, or give -OwnerWord."
}

# What is there now is kept first.
$stamp = Get-Date -Format 'yyyy-MM-dd_HHmmss'
if (Test-Path (Join-Path $out 'nvngx.dll')) {
    New-Item -ItemType Directory -Force (Join-Path $kept $stamp) | Out-Null
    Copy-Item (Join-Path $out 'nvngx.dll') (Join-Path $kept $stamp) -Force
}
# The whole build: the engine and the files it runs with (a round may change those too - 0.4.1's compatibility files).
# (-Back keeps only nvngx.dll, as it was kept.)
foreach ($place in $out, $debug) {
    if (-not (Test-Path $place)) { continue }
    if ($Back) { Copy-Item $engine (Join-Path $place 'nvngx.dll') -Force }
    else {
        # (robocopy merges into folders already there; Copy-Item -Recurse nested them - out\xess\xess, dlssg_compat\dlssg_compat.)
        robocopy $From $place /E /XF *.log /NFL /NDL /NJH /NJS /NP | Out-Null
        if ($LASTEXITCODE -ge 8) { throw "copying into $place failed (robocopy $LASTEXITCODE)" }
    }
    "installed: $place"
}
$why = if ($passed) { 'gate PASS' } else { "owner's word: $OwnerWord" }
Add-Content (Join-Path $here 'archive\installed.log') ("{0:yyyy-MM-dd HH:mm}  {1}  from {2}  ({3})" -f (Get-Date), $hash, $From, $why)
"engine $hash installed ($why); the one before is in archive\installed\$stamp"
