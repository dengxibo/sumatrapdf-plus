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

Optional custom CJK font (e.g. LXGW WenKai / 霞鹜文楷):
  1. Copy the .ttf/.otf into fonts\
  2. In SumatraPDF-settings.txt, under EBookUI:
       CjkFontFamily = LXGW WenKai
  3. Restart and re-open the ebook

FontFamily and CjkFontFamily accept the font family name shown in Windows font
properties (installed system fonts work; fonts\ is optional for portable bundling).
All .ttf / .otf files in fonts\ are also loaded automatically at startup.
CjkFontFile is only needed as an override when auto-detect fails.

Source Serif 4 (example)
  SourceSerif4-Regular.ttf
  SourceSerif4-Bold.ttf
  SourceSerif4-Italic.ttf
  SourceSerif4-BoldItalic.ttf
  from static\ in the Google Fonts download; then set FontFamily = Source Serif 4
Bilingual MOBI uses both: English runs use FontFamily, Chinese runs use CjkFontFamily.
CHM is compiled HTML with its own CSS fonts; EBookUI font settings do not override CHM styling.

If a file is not found there, it falls back to Windows system fonts.
