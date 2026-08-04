/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#pragma once

#include "utils/BaseUtil.h"

// Chinese (and general) TTS pronunciation overrides.
// Current TTS backends (WinRT SpeechSynthesis + SAPI 5) only accept plain text —
// no SSML/phoneme — so we rewrite the speak string before synthesis. Display text
// and read-aloud highlight maps stay on the original string via spoken→source offsets.

struct TtsPronunciationEntry {
    char* from = nullptr;
    char* to = nullptr;
    bool wholeWord = true;
};

void TtsPronunciationClear();
// Replace dictionary from a JSON document. Returns false on parse failure (keeps old dict).
bool TtsPronunciationLoadFromJson(const char* json);
int TtsPronunciationEntryCount();

// Apply longest-match replacements. Returns an owned UTF-8 string (caller frees).
// If spokenToSourceUtf8Out is set, fills spokenLen+1 ints: spokenToSourceUtf8Out[i]
// is the UTF-8 offset in `text` corresponding to spoken offset i.
char* TtsPronunciationApply(const char* text, Vec<int>* spokenToSourceUtf8Out = nullptr);

// Convert tone-marked or numbered pinyin to SAPI phoneme form.
// "qiào" / "qiao4" / "yín háng" → "qiao 4" / "qiao 4" / "yin 2 hang 2". Owned; nullptr if empty.
char* TtsPinyinToSapiPh(const char* pinyin);

// Debug / tests: expose sorted entries.
const Vec<TtsPronunciationEntry>& TtsPronunciationEntriesForTest();
