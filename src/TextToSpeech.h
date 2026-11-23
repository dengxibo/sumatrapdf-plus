#pragma once

struct TtsVoiceInfo {
    char* id;
    char* name;
    char* lang;
    // SAPI voices (e.g. from NaturalVoiceSAPIAdapter) are separate from WinRT voices.
    bool isSapi = false;
};

bool TtsSpeakUtf8(const char* text);
struct OfflineVoice;
struct EdgeReadAloudVoice;
// Short dictionary utterances can wait for the complete response to avoid
// gaps between streamed audio blocks. Document reading streams by default.
bool TtsSpeakGeneratedUtf8(const char* text, const OfflineVoice* offline, const EdgeReadAloudVoice* edge = nullptr,
                           bool bufferUntilComplete = false);
void TtsPrepareGeneratedPlayback();
void TtsQueueNextGeneratedUtf8(const char* text);
bool TtsConsumeSynthesisFailure();
// Queue one following SAPI chunk; the next matching Speak adopts it.
// Other backends do not queue. TtsStop purges both chunks.
bool TtsQueueNextUtf8(const char* text);
// Speak Chinese with an explicit reading from dictionary pinyin (tone marks or digits).
// Builds SSML <phoneme alphabet="sapi" ph="..."> for WinRT/SAPI; falls back to plain text.
bool TtsSpeakUtf8WithChinesePinyin(const char* text, const char* pinyin);
void TtsStop();
void TtsRelease();

// Reload tts-pronunciation.json from exe dir or AppData (for tests / manual refresh).
void TtsPronunciationReloadFromDisk();

bool TtsIsSpeaking();

// returns true once when the current speak chunk has fully finished (SAPI end-of-stream or WinRT playback done)
bool TtsConsumeChunkFinished();

// utf8 offset of the most recently spoken word within the text passed
// to TtsSpeakUtf8, -1 if not known
int TtsGetSpokenPosUtf8();

// utf8 offset of the end of the word currently being spoken, or -1 if unknown
int TtsGetSpokenWordEndUtf8();

void TtsSetNotifyWindow(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
void TtsProcessEvents();

Vec<TtsVoiceInfo> TtsGetVoices();
void TtsFreeVoices(Vec<TtsVoiceInfo>& voices);

bool TtsSetVoiceById(const char* voiceId);
const char* TtsGetVoiceId();

bool TtsSetSpeakingRate(float rate);
float TtsGetSpeakingRate();
