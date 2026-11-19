[CmdletBinding(DefaultParameterSetName = 'File')]
param(
    [Parameter(Mandatory)]
    [string]$ProgramDirectory,

    [Parameter(Mandatory, ParameterSetName = 'File')]
    [string]$Package,

    [Parameter(Mandatory, ParameterSetName = 'Download')]
    [uri]$PackageUrl
)

$ErrorActionPreference = 'Stop'
$voiceId = 'yunyi-hd'
$expectedHash = 'AA01993F1FEE3B51931B2FCD1A23286C00DCC8CC3A7D6D0A1D7A638F7DDA01D6'
$root = Join-Path (Resolve-Path -LiteralPath $ProgramDirectory).Path 'tts'
$destination = Join-Path $root $voiceId
New-Item -ItemType Directory -Path $root -Force | Out-Null
if (Test-Path -LiteralPath $destination) {
    throw "The $voiceId directory already exists at $destination."
}

$stage = Join-Path $root ('.install-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $stage | Out-Null
try {
    if ($PSCmdlet.ParameterSetName -eq 'Download') {
        if (!$PackageUrl.IsAbsoluteUri -or $PackageUrl.Scheme -ne 'https') {
            throw 'The model URL must use HTTPS.'
        }
        $Package = Join-Path $stage 'voice.msix'
        Invoke-WebRequest -Uri $PackageUrl -OutFile $Package
    }

    $packagePath = (Resolve-Path -LiteralPath $Package).Path
    $actualHash = (Get-FileHash -LiteralPath $packagePath -Algorithm SHA256).Hash
    if ($actualHash -ne $expectedHash) {
        throw 'The package does not match the verified x64 Yunyi HD 1.0.1.0 package.'
    }

    $extracted = Join-Path $stage 'model'
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::ExtractToDirectory($packagePath, $extracted)
    foreach ($file in @('AppxManifest.xml', 'Tokens.xml')) {
        if (!(Test-Path -LiteralPath (Join-Path $extracted $file) -PathType Leaf)) {
            throw "Model package is missing $file."
        }
    }

    [IO.Directory]::Move($extracted, $destination)
    Write-Output $destination
} finally {
    $resolvedStage = (Resolve-Path -LiteralPath $stage).Path
    $resolvedRoot = (Resolve-Path -LiteralPath $root).Path.TrimEnd('\') + '\'
    if ($resolvedStage.StartsWith($resolvedRoot, [StringComparison]::OrdinalIgnoreCase)) {
        Remove-Item -LiteralPath $resolvedStage -Recurse -Force
    }
}
