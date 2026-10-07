/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#pragma once

// Collect installed Windows font family names suitable for ebook body text.
// Caller owns strings in *families and must str::Free each entry.
void CollectInstalledLatinFontFamilies(Vec<char*>* families);
void CollectInstalledCjkFontFamilies(Vec<char*>* families);

// Map GDI family names (often English) to Chinese menu labels. Returns family unchanged when unknown.
const char* GetInstalledCjkFontMenuLabel(const char* family);

// True when the display name already contains Han / kana / hangul characters.
bool FontFamilyNameHasCjkScript(const char* family);
