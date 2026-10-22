# Run after bun cmd/build.ts. No Word installation or private documents required.
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root
$testDir = Join-Path $root 'out/word-format-test'
New-Item -ItemType Directory -Force $testDir | Out-Null
$parts = @{
    '[Content_Types].xml' = '<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Default Extension="xml" ContentType="application/xml"/><Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/><Override PartName="/word/styles.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml"/></Types>'
    '_rels/.rels' = '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="doc" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/></Relationships>'
    'word/_rels/document.xml.rels' = '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="styles" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles" Target="styles.xml"/></Relationships>'
    'word/styles.xml' = '<w:styles xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"><w:style w:type="paragraph" w:default="1" w:styleId="Normal"><w:name w:val="Normal"/><w:rPr><w:sz w:val="32"/><w:szCs w:val="24"/></w:rPr></w:style></w:styles>'
    'word/document.xml' = @'
<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main" xmlns:v="urn:schemas-microsoft-com:vml"><w:body>
<w:p><w:r><w:rPr><w:szCs w:val="22"/></w:rPr><w:t>INHERITED</w:t></w:r></w:p>
<w:p><w:r><w:rPr><w:sz w:val="20"/><w:szCs w:val="22"/></w:rPr><w:t>EXPLICIT</w:t></w:r></w:p>
<w:p><w:r><w:rPr><w:cs/><w:szCs w:val="22"/></w:rPr><w:t>COMPLEX</w:t></w:r></w:p>
<w:p><w:r><w:pict><v:shape type="#_x0000_t136" style="width:200pt;height:30pt" fillcolor="red"><v:textpath style="font-family:serif;font-size:24pt" string="Banner"/></v:shape></w:pict></w:r></w:p>
<w:p><w:r><w:pict><v:line from="0pt,0pt" to="200pt,0pt" strokecolor="red" strokeweight="1.3pt"/></w:pict></w:r></w:p>
</w:body></w:document>
'@
}
Add-Type -AssemblyName System.IO.Compression
$fixture = Join-Path $testDir 'fixture.docx'
$stream = [IO.File]::Open($fixture, [IO.FileMode]::Create)
$zip = [IO.Compression.ZipArchive]::new($stream, [IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($part in $parts.GetEnumerator()) {
        $writer = [IO.StreamWriter]::new($zip.CreateEntry($part.Key).Open())
        try { $writer.Write($part.Value) } finally { $writer.Dispose() }
    }
} finally { $zip.Dispose(); $stream.Dispose() }

# Reuse the existing compiler/library setup; substitute only the regression source.
bun cmd/test-html-outline.ts
if ($LASTEXITCODE) { throw 'Outline regression failed' }
$compile = Get-Content (Join-Path $root 'out/build-html-outline-test.cmd') -Raw
$compile = $compile.Replace('src/tools/html-outline-test.c', 'src/tools/word-format-test.c').Replace('html-outline-test.obj', 'word-format-test.obj').Replace('html-outline-test.exe', 'word-format-test.exe')
$batch = Join-Path $testDir 'build.cmd'
[IO.File]::WriteAllText($batch, $compile)
cmd /c $batch
if ($LASTEXITCODE) { throw 'Regression compilation failed' }
& (Join-Path $root 'out/word-format-test.exe') $fixture
if ($LASTEXITCODE) { throw 'Word formatting regression failed' }
