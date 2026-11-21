/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#pragma once

struct EdgeSpeechResponse;

enum class EdgeVoiceLanguage {
    Multilingual,
    Chinese,
    English
};

struct EdgeReadAloudVoice {
    const char* id;
    const char* name;
    const char* shortName;
    EdgeVoiceLanguage language;
};

extern const EdgeReadAloudVoice gEdgeReadAloudVoices[];
extern const int gEdgeReadAloudVoiceCount;
const EdgeReadAloudVoice* FindEdgeReadAloudVoice(const char* id);
const EdgeReadAloudVoice* ConfiguredCloudSpeechVoice(int index);
bool ReadCloudSpeechSetting(int index, WCHAR* value, DWORD capacity);
bool IsJoySpeechBotIdValid(const WCHAR* botId);
void GetJoySpeechVoices(Vec<const EdgeReadAloudVoice*>& voices);
constexpr int kCloudSpeechSettingCount = 7;
char* ProtectCloudSpeechSettings(WCHAR* const (&values)[kCloudSpeechSettingCount]);
void LoadCloudSpeechSettings(const char* protectedSettings);
void EdgeSpeechPrepare();
void EdgeSpeechRefreshPreparation();

struct EdgeSpeechTurn {
    const WCHAR* resource = nullptr; // Request target, including provider-supplied query parameters.
    const WCHAR* headers = nullptr;
    const char* configMessage = nullptr;
    const char* ssmlMessage = nullptr;
    bool allowPreparedConnection = false;
};

struct EdgeSpeechTransportResult {
    DWORD error = ERROR_SUCCESS;
    DWORD httpStatus = 0;
};

// Blocking. Call on a worker. cancelEvent is a manual-reset event owned by the caller.
EdgeSpeechTransportResult EdgeSpeechReceiveTurn(const EdgeSpeechTurn&, HANDLE cancelEvent, EdgeSpeechResponse&);

// The caller passes the connection credentials supplied for this turn.
// Construct the response with the same text and keep it alive through the call.
EdgeSpeechTransportResult EdgeSpeechSynthesize(const WCHAR* resource, const WCHAR* headers, const char* text,
                                               const char* voice, float rate, HANDLE cancelEvent, EdgeSpeechResponse&,
                                               bool allowPreparedConnection = false);
EdgeSpeechTransportResult EdgeSpeechSynthesizeOnline(const char* text, const char* voice, float rate,
                                                     HANDLE cancelEvent, EdgeSpeechResponse&, bool usePrepared = true);
