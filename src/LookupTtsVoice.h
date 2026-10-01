/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#pragma once

// Voice id the dictionary speaker should use for the current Read Aloud mode.
// Returns "" for the system default voice.
// Returns nullptr when this mode has no voice for the requested language:
// the caller must leave the synthesizer unchanged (never borrow another mode's voice).
//
// Mode strings match SumatraPDF.h: "online:multilingual", "smart:zh-en", "smart-online:zh-en".
struct LookupTtsVoicePrefs {
    const char* mode = nullptr;
    const char* multilingual = nullptr;
    const char* localZh = nullptr;
    const char* localEn = nullptr;
    const char* onlineZh = nullptr;
    const char* onlineEn = nullptr;
};

inline const char* LookupPickVoiceId(const LookupTtsVoicePrefs& p, bool chinese) {
    const char* mode = p.mode ? p.mode : "";
    if (str::Eq(mode, "online:multilingual")) {
        return str::IsEmpty(p.multilingual) ? nullptr : p.multilingual;
    }
    if (str::Eq(mode, "smart:zh-en")) {
        const char* id = chinese ? p.localZh : p.localEn;
        return str::IsEmpty(id) ? nullptr : id;
    }
    if (str::Eq(mode, "smart-online:zh-en")) {
        const char* id = chinese ? p.onlineZh : p.onlineEn;
        return str::IsEmpty(id) ? nullptr : id;
    }
    if (!mode[0]) {
        return "";
    }
    return mode;
}
