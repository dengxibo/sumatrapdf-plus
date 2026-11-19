/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#pragma once

struct EdgeSpeechWord {
    i64 offset100ns = 0;
    int start = 0;
    int end = 0;
};

enum class EdgeSpeechMessage {
    Invalid,
    Other,
    Audio,
    Metadata,
    End,
};

// One response per synthesis turn. Offsets refer to the UTF-8 text submitted for synthesis.
struct EdgeSpeechResponse {
    explicit EdgeSpeechResponse(const char* submittedText) : text(submittedText) {}
    EdgeSpeechMessage Receive(bool binary, const void* message, size_t size);

    StrBuilder mp3;
    Vec<EdgeSpeechWord> words;
    void* audioContext = nullptr;
    void (*audioReceived)(void*, EdgeSpeechResponse&) = nullptr;

  private:
    const char* text;
    int textOffset = 0;
};
