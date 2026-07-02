#pragma once

struct TtsVoiceInfo {
    char* id;
    char* name;
    char* lang;
};

bool TtsSpeakUtf8(const char* text);
void TtsStop();
void TtsRelease();

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
