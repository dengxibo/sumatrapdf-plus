Bundled fonts for Sumatra PDF Plus distribution
=========================================

Place font files here before building, or copy this entire "fonts"
directory next to SumatraPDF.exe when distributing.

The build copies *.ttf / *.otf / *.ttc from this folder into
out/dbg64/fonts/ (debug) or out/rel64/fonts/ (release). If
SourceHanSerif-Regular.ttc is missing here, the build copies it
from mupdf/resources/fonts/han/ automatically.

Recommended files:

Literata (Latin EPUB body text)
  Literata-Regular.ttf
  Literata-Bold.ttf
  Literata-Italic.ttf
  Literata-BoldItalic.ttf
  https://fonts.google.com/specimen/Literata

Source Han Serif SC (Chinese EPUB body text)
  SourceHanSerif-Regular.ttc
  or SourceHanSerifSC-Regular.otf
  https://github.com/adobe-fonts/source-han-serif

At runtime SumatraPDF loads these from:
  <exe directory>\fonts\

If a file is not found there, it falls back to Windows system fonts.
