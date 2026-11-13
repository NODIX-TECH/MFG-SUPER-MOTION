# collect-shaders.ps1 (1.1.0) - the compiled shaders shipped beside the engine (lf_shader_cache.inl): the engine built in
# <From> run with LF_SHADER_COLLECT=<From>\shaders through every shader it has - --warm-shaders, the whole smoke test
# (every generator, SMT, SMSR, the counter, HDR, bloom ...) and two Super Video jobs (SMFG + SMSR, SMAI) - so that every
# compiled shader lands there. The smoke test's own result is printed: this is also the build's check.
#   .\collect-shaders.ps1 -From work\r111-out
param([Parameter(Mandatory = $true)][string]$From)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$From = (Resolve-Path (Join-Path $here $From) -ErrorAction SilentlyContinue).Path
if (-not $From) { $From = (Resolve-Path $PSBoundParameters['From']).Path }
$engine = Join-Path $From 'nvngx.dll'
if (-not (Test-Path $engine)) { throw "no engine in $From" }
$shaders = Join-Path $From 'shaders'
New-Item -ItemType Directory -Force $shaders | Out-Null
$env:LF_SHADER_COLLECT = $shaders
try {
    # (nvngx.dll is the engine's program under NGX's own name: started as a process, not run as a document.)
    $warm = Start-Process -FilePath $engine -ArgumentList '--warm-shaders' -WorkingDirectory $From -NoNewWindow -Wait -PassThru
    "warm-shaders: exit $($warm.ExitCode)"
    # (Super Video: a short video of the tests' own made larger, with SMFG and SMSR, then with SMAI.)
    $tests = Join-Path $here 'tests'
    $work = Join-Path $tests 'dump\collect'
    New-Item -ItemType Directory -Force $work | Out-Null
    & (Join-Path $tests 'videotool.exe') make (Join-Path $work 'in.mp4') 960 540 30 2 | Out-Null
    & (Join-Path $tests 'video.ps1') -In (Join-Path $work 'in.mp4') -Out (Join-Path $work 'smsr.mp4') -Multiplier 2 -Height 1080 -Options 'upscaler=5' -Engine $engine | Select-Object -Last 1 | Out-Host
    & (Join-Path $tests 'video.ps1') -In (Join-Path $work 'in.mp4') -Out (Join-Path $work 'smai.mp4') -Multiplier 2 -Height 0 -Options 'msm_ai=1' -Engine $engine | Select-Object -Last 1 | Out-Host
    $result = Join-Path $tests 'smooth\runs\collect-smoke.txt'
    & (Join-Path $tests 'xess-smoke.ps1') -Engine $engine *> $result
    $pass = (Select-String -Path $result -Pattern ': PASS' | Measure-Object).Count
    $fail = (Select-String -Path $result -Pattern 'FAIL' | Measure-Object).Count
    "smoke: $pass PASS, $fail FAIL"
    Select-String -Path $result -Pattern 'FAIL' | ForEach-Object { $_.Line }
} finally {
    Remove-Item Env:LF_SHADER_COLLECT -ErrorAction SilentlyContinue
}
$files = Get-ChildItem $shaders -Filter *.cso
"{0} compiled shaders in {1} ({2:N1} MB)" -f $files.Count, $shaders, (($files | Measure-Object Length -Sum).Sum / 1MB)
