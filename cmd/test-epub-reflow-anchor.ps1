param([string]$Exe = 'out/dbg64/SumatraPDF-Plus.exe')

$ErrorActionPreference = 'Stop'
$repoDir = Split-Path $PSScriptRoot -Parent
$testDir = Join-Path $repoDir 'out/epub-anchor-test'
New-Item -ItemType Directory -Path $testDir -Force | Out-Null
$epubPath = Join-Path $testDir 'long-chapters.epub'
$zipStream = [IO.File]::Open($epubPath, [IO.FileMode]::Create)
$archive = [IO.Compression.ZipArchive]::new($zipStream, [IO.Compression.ZipArchiveMode]::Create)
try {
    function Add-EpubEntry([string]$Name, [string]$Text) {
        $entry = $archive.CreateEntry($Name)
        $writer = [IO.StreamWriter]::new($entry.Open(), [Text.UTF8Encoding]::new($false))
        try { $writer.Write($Text) } finally { $writer.Dispose() }
    }
    Add-EpubEntry 'mimetype' 'application/epub+zip'
    Add-EpubEntry 'META-INF/container.xml' '<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles><rootfile full-path="content.opf" media-type="application/oebps-package+xml"/></rootfiles></container>'
    Add-EpubEntry 'content.opf' '<package xmlns="http://www.idpf.org/2007/opf" version="2.0" unique-identifier="id"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="id">anchor-test</dc:identifier><dc:title>Long chapter anchor regression</dc:title><dc:language>en</dc:language></metadata><manifest><item id="en" href="en.xhtml" media-type="application/xhtml+xml"/><item id="cn" href="cn.xhtml" media-type="application/xhtml+xml"/></manifest><spine><itemref idref="en"/><itemref idref="cn"/></spine></package>'
    foreach ($lang in @('en', 'cn')) {
        $body = [Text.StringBuilder]::new()
        [void]$body.Append('<html xmlns="http://www.w3.org/1999/xhtml"><head><title>Anchor test</title></head><body>')
        for ($i = 0; $i -lt 5000; $i++) {
            if ($lang -eq 'en') {
                [void]$body.Append("<p>Paragraph $i contains a unique anchor. This <b>inline</b> phrase crosses formatting and spaces. Reading position should survive font changes without extracting every preceding page.</p>")
            } else {
                [void]$body.Append("<p>第${i}段：这里是用于验证电子书字号调整的独特段落。中文与<b>行内样式</b>混排，包含空格以及&nbsp;不换行空格。阅读位置应保持在原来的内容附近，不需要逐页提取前面的全部文字。</p>")
            }
        }
        [void]$body.Append('</body></html>')
        Add-EpubEntry "$lang.xhtml" $body.ToString()
    }
} finally { $archive.Dispose(); $zipStream.Dispose() }

$exePath = Join-Path $repoDir $Exe
$logPath = Join-Path $testDir 'run.log'
$errPath = Join-Path $testDir 'run.err.log'
foreach ($fontSize in @(14, 22)) {
    $settingsDir = Join-Path $testDir "settings-$fontSize"
    New-Item -ItemType Directory -Path $settingsDir -Force | Out-Null
    [IO.File]::WriteAllText((Join-Path $settingsDir 'SumatraPDF-settings.txt'), "EBookUI [`n FontSize = $fontSize`n]`n")
    $startedAt = Get-Date
    $proc = Start-Process -FilePath $exePath -ArgumentList "-bench-epub `"$epubPath`" -appdata `"$settingsDir`" -lang en -console" -WindowStyle Hidden -PassThru -Wait -RedirectStandardOutput $logPath -RedirectStandardError $errPath
    if ($proc.ExitCode -ne 0) { throw "Benchmark failed with exit code $($proc.ExitCode). See $logPath" }
    $metricsPath = Get-ChildItem (Join-Path $repoDir 'out/perf') -Filter '*.jsonl' | Where-Object LastWriteTime -GE $startedAt | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    $anchors = Get-Content $metricsPath.FullName | ForEach-Object { $_ | ConvertFrom-Json } | Where-Object op -EQ 'reflow_anchor'
    if (@($anchors).Count -ne 3) { throw 'Expected three reading-position samples.' }
    foreach ($anchor in $anchors) {
        if ($anchor.found -ne 1 -or $anchor.page -ne $anchor.scan_page) { throw "Anchor lookup disagrees with structured-text search: $($anchor | ConvertTo-Json -Compress)" }
    }
    Write-Output "Font size: $fontSize"
    $anchors | Format-Table sample,page,ms,scan_ms
}
