/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#pragma once

enum class RoughLang {
    Unknown = 0,
    Zh = 1,
    En = 2,
    Ja = 3,
    Ko = 4,
    Other = 5,
};

struct TranslateTargetOption {
    const char* mode = nullptr;  // stored in TranslateTargetMode
    const char* label = nullptr; // English UI label for _TRA
};

// Rough source-language guess from script. Used only to pick a translate target.
RoughLang DetectSourceLangRough(const char* text);

// Maps Sumatra UI lang codes (cn, tw, kr, vn, ...) to Volc/translate codes (zh, en, ko, ...).
const char* UiLangToTranslateCode(const char* uiLangCode);

// Human-readable English name for LLM prompts / diagnostics.
const char* TranslateCodeDisplayName(const char* code);

// Resolves the translate target code for Volc/LLM.
// targetMode: "auto" | "ui" | language code (zh, en, ja, ...).
const char* ResolveTranslateTarget(const char* text, const char* targetMode, const char* uiLangCode);

const TranslateTargetOption* GetTranslateTargetOptions(int* countOut);
int FindTranslateTargetOptionIndex(const char* mode);

void InlineTranslateLang_UnitTests();
