param(
    [Parameter(Mandatory = $true)][string]$Book,
    [string]$Exe = 'out/dbg64/SumatraPDF-Plus.exe'
)

$ErrorActionPreference = 'Stop'
$repoDir = Split-Path $PSScriptRoot -Parent
$testDir = Join-Path $repoDir 'out/epub-font-position-test'
$settingsDir = Join-Path $testDir 'settings'
New-Item -ItemType Directory -Path $settingsDir -Force | Out-Null
[IO.File]::WriteAllText((Join-Path $settingsDir 'SumatraPDF-settings.txt'), "RestoreSession = false`nEBookUI [`n FontSize = 14`n]`n")
$exePath = if ([IO.Path]::IsPathRooted($Exe)) { $Exe } else { Join-Path $repoDir $Exe }
$startedAt = Get-Date
$oldBenchmark = $env:SUMATRA_EPUB_POSITION_BENCH
try {
    $env:SUMATRA_EPUB_POSITION_BENCH = '1'
    $proc = Start-Process -FilePath $exePath -ArgumentList "-bench-epub `"$Book`" -appdata `"$settingsDir`" -lang en -console" -WindowStyle Hidden -PassThru -Wait -RedirectStandardOutput (Join-Path $testDir 'run.log') -RedirectStandardError (Join-Path $testDir 'run.err.log')
    if ($proc.ExitCode -ne 0) { throw "Regression process failed: $($proc.ExitCode)." }
} finally {
    $env:SUMATRA_EPUB_POSITION_BENCH = $oldBenchmark
}
$metricsPath = Get-ChildItem (Join-Path $repoDir 'out/perf') -Filter '*.jsonl' | Where-Object LastWriteTime -GE $startedAt | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (!$metricsPath) { throw 'No regression metrics were written.' }
$metrics = @(Get-Content $metricsPath.FullName | ForEach-Object { $_ | ConvertFrom-Json })
$positions = @($metrics | Where-Object op -EQ 'font_position')
$summary = @($metrics | Where-Object op -EQ 'font_position_summary')
$positions | Export-Csv (Join-Path $testDir 'positions.csv') -NoTypeInformation -Encoding utf8
if ($positions.Count -ne 72 -or $summary.Count -ne 1 -or !$summary[0].passed) {
    $positions | Where-Object { !$_.passed } | Format-Table scenario,step,phase,size,page,screen_y,captured_same,visible,at_top,mode_kept,cache_kept
    throw "Reading-position regression failed: $($positions.Count) checks. See $($metricsPath.FullName)."
}
Write-Output "PASS: $($positions.Count) checks across 36 font changes, six reading positions, Fit Width and Fit Single Page, including navigation and completed background pagination."
Write-Output $metricsPath.FullName
