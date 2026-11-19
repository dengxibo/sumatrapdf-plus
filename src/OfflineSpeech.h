#pragma once

struct OfflineSpeechCue {
    int utf16Position = 0;
    int timeMs = 0;
};
struct OfflineSpeechStream {
    void* context = nullptr;
    void (*audio)(void*, const char*, DWORD) = nullptr;
    void (*word)(void*, int, int) = nullptr;
};

struct OfflineVoice {
    const char* id;
    const char* directory;
    const char* name;
    const char* locale;
    const char* runtimeName;
    i64 packageSize;
    const char* sha256;
    const char* storeId = nullptr;
    const char* storeFile = nullptr;
};

extern const OfflineVoice gOfflineVoices[32];
const OfflineVoice* FindOfflineVoice(const char* id);
bool OfflineSpeechInstalled(const OfflineVoice&);
bool RemoveOfflineSpeech(const OfflineVoice&);
bool OfflineSpeechAvailable(const OfflineVoice&);
void OfflineSpeechPrepare(const OfflineVoice&);
char* OfflineSpeechSynthesize(const OfflineVoice&, const char* text, float rate, HANDLE cancel, DWORD* waveSize,
                              Vec<OfflineSpeechCue>&, HANDLE firstAudio, const OfflineSpeechStream* stream = nullptr);
