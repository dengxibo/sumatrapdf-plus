/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#pragma once

// A bounded cutoff for JoyInside's female dictionary clips. Frame counts keep
// all channels aligned; the short fade avoids a click at the new endpoint.
inline int TrimLookupFemaleSpeechTail(short* samples, int frames, int channels, int sampleRate) {
    if (!samples || frames <= 0 || channels <= 0 || sampleRate <= 0) {
        return frames;
    }
    int trim = sampleRate * 80 / 1000;
    if (trim > frames / 12) trim = frames / 12;
    int kept = frames - trim;
    int fade = sampleRate * 12 / 1000;
    if (fade > kept / 4) fade = kept / 4;
    if (fade < 2) return kept;
    for (int i = 0; i < fade; i++) {
        for (int ch = 0; ch < channels; ch++) {
            int pos = (kept - fade + i) * channels + ch;
            samples[pos] = (short)((int)samples[pos] * (fade - 1 - i) / (fade - 1));
        }
    }
    return kept;
}
