/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Convert Traditional Chinese UTF-8 text to Simplified Chinese using OpenCC data.
// Returns a newly allocated string. If conversion is unavailable or unchanged,
// returns a duplicate of the input.
char* TraditionalToSimplified(const char* utf8);

bool CharConvIsReady();
