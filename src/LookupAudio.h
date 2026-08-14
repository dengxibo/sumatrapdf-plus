/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#pragma once

// Takes ownership of data and frees it after playback finishes or fails.
// ext is the audio file extension without dot, e.g. "mp3" or "wav".
bool LookupAudioPlayOwned(u8* data, size_t size, const char* ext);

void LookupAudioStop();
bool LookupAudioIsPlaying();

// Optional caller identity for toggle-to-stop (e.g. PDF Sound annot). Cleared on stop/end.
void LookupAudioSetPlayToken(u64 token);
u64 LookupAudioPlayToken();
