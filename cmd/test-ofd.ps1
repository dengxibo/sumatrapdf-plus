# Run after bun cmd/build.ts. Additional arguments may name OFDRW sample files.
param([string[]]$Samples = @())
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root
$testDir = Join-Path $root 'out/ofd-test'
New-Item -ItemType Directory -Force $testDir | Out-Null
$ns = 'xmlns="http://www.ofdspec.org/2016"'
$parts = @{
    'OFD.xml' = "<OFD $ns><DocBody><DocRoot>Doc/Document.xml</DocRoot></DocBody></OFD>"
    'Doc/Document.xml' = "<Document $ns><CommonData><PageArea><PhysicalBox>0 0 100 80</PhysicalBox></PageArea><PublicRes>Res.xml</PublicRes></CommonData><Pages><Page ID=`"1`" BaseLoc=`"Page.xml`"/><Page ID=`"2`" BaseLoc=`"Page.xml`"/></Pages><Outlines><OutlineElem Title=`"Second page`"><Actions><Action><Goto><Dest PageID=`"2`"/></Goto></Action></Actions></OutlineElem></Outlines></Document>"
    'Doc/Res.xml' = "<Res $ns><Fonts><Font ID=`"5`" FontName=`"Arial`"/></Fonts></Res>"
    'Doc/Page.xml' = "<Page $ns><Content><Layer ID=`"3`"><PathObject ID=`"4`" Boundary=`"10 10 20 20`" Fill=`"true`" Stroke=`"false`"><FillColor Value=`"255 0 0`"/><AbbreviatedData>M 0 0 L 20 0 L 20 20 L 0 20 C</AbbreviatedData></PathObject><TextObject ID=`"6`" Boundary=`"10 40 80 10`" Font=`"5`" Size=`"5`"><TextCode X=`"0`" Y=`"6`">OFD regression</TextCode></TextObject></Layer></Content></Page>"
}
Add-Type -AssemblyName System.IO.Compression
$fixture = Join-Path $testDir 'fixture.ofd'
$stream = [IO.File]::Open($fixture, [IO.FileMode]::Create)
$zip = [IO.Compression.ZipArchive]::new($stream, [IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($part in $parts.GetEnumerator()) {
        $writer = [IO.StreamWriter]::new($zip.CreateEntry($part.Key).Open())
        try { $writer.Write($part.Value) } finally { $writer.Dispose() }
    }
} finally { $zip.Dispose(); $stream.Dispose() }
$renamed = Join-Path $testDir 'fixture.zip'
Copy-Item -LiteralPath $fixture -Destination $renamed -Force
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vsRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vsRoot) { throw 'Visual Studio C++ tools not found' }
$libraries = @('mupdf', 'mupdf-libs', 'libarchive', 'zlib', 'libwebp', 'libheif', 'dav1d') | ForEach-Object { '"' + (Join-Path $root "out/dbg64/$_.lib") + '"' }
$cmark = Join-Path $root 'out/dbg64/cmark-gfm.lib'
if (!(Test-Path $cmark)) { $cmark = Join-Path $root 'out/dbg64/obj/cmark-gfm.lib' }
$libraries += '"' + $cmark + '"'
$exe = Join-Path $testDir 'ofd-test.exe'
$batch = Join-Path $testDir 'build.cmd'
$commands = @(
    '@echo off'
    "call `"$vsRoot/VC/Auxiliary/Build/vcvars64.bat`""
    'if errorlevel 1 exit /b 1'
    "cl /nologo /std:clatest /MT /DDEBUG /Imupdf/include src/tools/ofd-test.c /Fo`"$testDir/ofd-test.obj`" /Fe`"$exe`" /link /DEBUG:NONE $($libraries -join ' ') windowscodecs.lib gdiplus.lib comctl32.lib shlwapi.lib version.lib wininet.lib shcore.lib wintrust.lib crypt32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib advapi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib"
    'exit /b %errorlevel%'
)
[IO.File]::WriteAllText($batch, ($commands -join "`r`n") + "`r`n")
cmd /c $batch
if ($LASTEXITCODE) { throw 'OFD test compilation failed' }
& $exe $fixture $renamed @Samples
if ($LASTEXITCODE) { throw 'OFD regression failed' }

$invalid = Join-Path $testDir 'invalid.ofd'
[IO.File]::WriteAllText($invalid, 'This is not an OFD archive.')
& $exe $invalid 2> (Join-Path $testDir 'invalid.log')
if ($LASTEXITCODE -ne 1) { throw 'Malformed OFD was not rejected cleanly' }
Write-Output 'PASS: malformed OFD rejected cleanly'
