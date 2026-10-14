param(
    [Parameter(Mandatory = $true)][string]$Pdf,
    [int]$Pages = 7,
    [string]$Output = "tmp/issues107-108-memory",
    [string]$Exe = "out/dbg64/SumatraPDF-Plus.exe",
    [switch]$Arena,
    [switch]$NoArena,
    [int]$IdleSeconds = 0
)

$ErrorActionPreference = "Stop"
if ($Arena -and $NoArena) { throw "Choose either -Arena or -NoArena" }
$benchmarkPdf = (Resolve-Path -LiteralPath $Pdf).Path
$benchmarkExe = (Resolve-Path -LiteralPath $Exe).Path
$benchmarkOutput = [IO.Path]::GetFullPath($Output)
New-Item -ItemType Directory -Force -Path $benchmarkOutput | Out-Null
$envNames = @("SUMATRA_OCR_BENCH_PDF", "SUMATRA_OCR_BENCH_DIR", "SUMATRA_OCR_BENCH_PAGES", "SUMATRA_OCR_BENCH_ARENA", "SUMATRA_OCR_BENCH_IDLE_SECONDS")
$savedEnv = @{}
foreach ($name in $envNames) { $savedEnv[$name] = [Environment]::GetEnvironmentVariable($name) }
try {
    $env:SUMATRA_OCR_BENCH_PDF = $benchmarkPdf
    $env:SUMATRA_OCR_BENCH_DIR = $benchmarkOutput
    $env:SUMATRA_OCR_BENCH_PAGES = "$Pages"
    $env:SUMATRA_OCR_BENCH_ARENA = $(if ($NoArena) { "0" } else { "1" })
    $env:SUMATRA_OCR_BENCH_IDLE_SECONDS = "$IdleSeconds"
    $benchmarkProcess = Start-Process -FilePath $benchmarkExe -ArgumentList "-new-window" -WindowStyle Hidden -PassThru
    $timer = [Diagnostics.Stopwatch]::StartNew()
    $samples = [Collections.Generic.List[object]]::new()
    while (!$benchmarkProcess.WaitForExit(250)) {
        $benchmarkProcess.Refresh()
        $samples.Add([pscustomobject]@{
            Seconds = [Math]::Round($timer.Elapsed.TotalSeconds, 2)
            PrivateMiB = [Math]::Round($benchmarkProcess.PrivateMemorySize64 / 1MB, 2)
            WorkingSetMiB = [Math]::Round($benchmarkProcess.WorkingSet64 / 1MB, 2)
            Phase = $(if (Test-Path -LiteralPath (Join-Path $benchmarkOutput "idle-end.txt")) { "resume" }
                      elseif (Test-Path -LiteralPath (Join-Path $benchmarkOutput "idle-start.txt")) { "idle" }
                      else { "recognize" })
        })
    }
    $samples | Export-Csv -NoTypeInformation -LiteralPath (Join-Path $benchmarkOutput "memory.csv")
    Write-Output "Exit code: $($benchmarkProcess.ExitCode); samples: $($samples.Count)"
    $samples | Measure-Object -Property PrivateMiB -Minimum -Maximum
    Get-Content -LiteralPath (Join-Path $benchmarkOutput "summary.csv")
    if ($benchmarkProcess.ExitCode -ne 0) { throw "OCR benchmark failed" }
} finally {
    foreach ($name in $envNames) { [Environment]::SetEnvironmentVariable($name, $savedEnv[$name]) }
}
