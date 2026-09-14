/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Formal prompts for the body-structure TOC flow ("从正文生成目录"). Prompts
// are the data protocol of the feature, so they live here rather than being
// scattered across the browser automation code.

#pragma once

// First-round prompt: classify the whole-document candidate digest. Returns a
// newly allocated UTF-8 string. totalPages is the physical PDF page count;
// candidateDigest is the <CANDIDATE> block from BuildBodyTocDigest().
char* BuildBodyTocPrompt(int totalPages, const char* candidateDigest);
