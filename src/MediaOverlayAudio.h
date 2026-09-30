/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Single audio player for EPUB media overlay narration (IMFMediaEngine, audio only).
// All functions must be called on the UI thread.

// Takes ownership of data (freed with free()). name is only used as a format hint (extension).
bool MoAudioSetSource(u8* data, size_t size, const char* name);
void MoAudioClose();

// true once the source's metadata is loaded, so seeking and position are meaningful
bool MoAudioIsReady();
bool MoAudioHasError();
bool MoAudioEnded();

// Seek and play requests made before the source is ready are applied once it is.
void MoAudioPlay();
void MoAudioPause();
bool MoAudioIsPlaying();
void MoAudioSeekUs(i64 us);
i64 MoAudioPositionUs();
i64 MoAudioDurationUs();
void MoAudioSetRate(double rate);

// applies deferred seek/play; call from the playback timer
void MoAudioTick();
