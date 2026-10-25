#pragma once
// Optional in-process COM API. No SDK, private engine pointer or named pipe is
// required by the client. Absence of this registered class means normal SAPI.
#include <windows.h>
#include <unknwn.h>
#include <sapi.h>
#pragma pack(push, 8)
inline const CLSID CLSID_HdSpeechPreparer = {0xd8a46b78,
                                             0x87df,
                                             0x4dde,
                                             {0x9e, 0x2a, 0x46, 0x8c, 0xe2, 0xf3, 0x1d, 0x8e}};
inline const CLSID CLSID_HdSpeechPreparerTest = {0xd8a46b78,
                                                 0x87df,
                                                 0x4dde,
                                                 {0x9e, 0x2a, 0x46, 0x8c, 0xe2, 0xf3, 0x1d, 0x90}};
enum HD_PREPARE_STATE : ULONG {
    HD_QUEUED = 1,
    HD_RUNNING = 2,
    HD_READY = 3,
    HD_CANCELLED = 4,
    HD_FAILED = 5
};
struct HD_PREPARE_LIMITS {
    ULONG cbSize = sizeof(HD_PREPARE_LIMITS), maxJobs = 8, maxTextChars = 32768, maxTotalTextChars = 65536;
    ULONGLONG maxCacheBytes = 64ull * 1024 * 1024, maxEntryBytes = 16ull * 1024 * 1024,
              maxResidentBytes = 64ull * 1024 * 1024;
};
struct HD_PREPARE_CAPS {
    ULONG cbSize = sizeof(HD_PREPARE_CAPS), version = 1, flags = 0, reserved = 0;
    HD_PREPARE_LIMITS limits;
};
constexpr ULONG HD_CAP_PLAIN_TEXT = 1, HD_CAP_ASYNC = 2, HD_CAP_SAPI_AUDIO_EVENTS = 4, HD_CAP_FILE_BACKED_CACHE = 8,
                HD_CAP_PLAYBACK_VOLUME = 16;
struct HD_PREPARE_STATUS {
    ULONG cbSize = sizeof(HD_PREPARE_STATUS), state = 0;
    HRESULT error = E_PENDING;
    LONG rate = 0;
    ULONGLONG generation = 0, bytes = 0, cacheHits = 0;
};
static_assert(sizeof(HD_PREPARE_LIMITS) == 40 && sizeof(HD_PREPARE_CAPS) == 56 && sizeof(HD_PREPARE_STATUS) == 40,
              "Stable x86/x64 API layout");
#pragma pack(pop)
MIDL_INTERFACE("E90E40B1-28E6-4E7D-922D-7622F55219E6") IHdPreparedAudio : IUnknown {
    // The returned stream retains the cache lease itself. It can be passed
    // directly to ISpVoice::SpeakStream; preparation never plays any audio.
    virtual HRESULT STDMETHODCALLTYPE GetStream(ISpStream * *stream) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetStatus(HD_PREPARE_STATUS * status) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPlaybackVolume(USHORT volume) = 0;
    virtual HRESULT STDMETHODCALLTYPE Validate(LPCWSTR actualText, LPCWSTR voiceTokenId, LONG rate,
                                               ULONGLONG generation) = 0;
};
MIDL_INTERFACE("9148E195-896C-455C-91CE-07FDF7DCDA28") IHdSpeechPreparer : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetCapabilities(HD_PREPARE_CAPS * caps) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetLimits(const HD_PREPARE_LIMITS* limits) = 0;
    // Strictly increasing, per service instance. Clears all older queued/ready
    // entries; in-flight work cannot publish after this returns. Never purges
    // the client's playback voice: the client separately calls SAPI purge.
    virtual HRESULT STDMETHODCALLTYPE AdvanceGeneration(ULONGLONG generation) = 0;
    // Copy of the actual dictionary-rewritten text, exact voice token ID and
    // rate. S_FALSE means a ready cache hit; S_OK means queued/pending reuse.
    virtual HRESULT STDMETHODCALLTYPE Prepare(LPCWSTR text, LPCWSTR voiceTokenId, LONG rate, ULONGLONG generation,
                                              ULONGLONG * ticket) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetStatus(ULONGLONG ticket, HD_PREPARE_STATUS * status) = 0;
    virtual HRESULT STDMETHODCALLTYPE Acquire(ULONGLONG ticket, IHdPreparedAudio * *audio) = 0;
    virtual HRESULT STDMETHODCALLTYPE Cancel(ULONGLONG ticket) = 0;
};
inline HRESULT HdConnectPreparer(IHdSpeechPreparer** service) {
    return CoCreateInstance(CLSID_HdSpeechPreparer, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IHdSpeechPreparer),
                            reinterpret_cast<void**>(service));
}
// Standard SAPI only; use before the first Speak, on this voice's own device.
// Does not change global audio settings or make synthesis parallel.
inline HRESULT HdConfigureSapiQueue(ISpVoice* voice, ULONG bufferMs = 5000) {
    if (!voice || bufferMs < 500 || bufferMs > 10000) return E_INVALIDARG;
    ISpStreamFormat* output = nullptr;
    HRESULT hr = voice->GetOutputStream(&output);
    if (FAILED(hr)) return hr;
    ISpAudio* audio = nullptr;
    hr = output->QueryInterface(__uuidof(ISpAudio), reinterpret_cast<void**>(&audio));
    output->Release();
    if (FAILED(hr)) return hr;
    SPAUDIOBUFFERINFO info{};
    hr = audio->GetBufferInfo(&info);
    if (SUCCEEDED(hr)) {
        info.ulMsBufferSize = bufferMs;
        hr = audio->SetBufferInfo(&info);
    }
    audio->Release();
    return hr;
}
// Audio-stream playback does not use the TTS engine's GetVolume callback.
// Keep this lease to update SetPlaybackVolume when the user's volume changes.
inline HRESULT HdQueuePrepared(ISpVoice* voice, IHdPreparedAudio* audio, LPCWSTR actualText, ULONGLONG generation,
                               DWORD flags, ULONG* streamNumber) {
    if (!voice || !audio || !actualText) return E_POINTER;
    ISpObjectToken* token = nullptr;
    HRESULT hr = voice->GetVoice(&token);
    if (FAILED(hr)) return hr;
    LPWSTR id = nullptr;
    hr = token->GetId(&id);
    token->Release();
    if (FAILED(hr)) return hr;
    LONG rate = 0;
    hr = voice->GetRate(&rate);
    if (SUCCEEDED(hr)) hr = audio->Validate(actualText, id, rate, generation);
    CoTaskMemFree(id);
    if (hr != S_OK) return FAILED(hr) ? hr : HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
    USHORT volume = 100;
    hr = voice->GetVolume(&volume);
    if (FAILED(hr)) return hr;
    hr = audio->SetPlaybackVolume(volume);
    if (FAILED(hr)) return hr;
    ISpStream* stream = nullptr;
    hr = audio->GetStream(&stream);
    if (FAILED(hr)) return hr;
    hr = voice->SpeakStream(stream, flags, streamNumber);
    stream->Release();
    return hr;
}
// Run on the serialized SAPI worker, not the UI thread. This waits for abort
// acknowledgement, not for old audio to finish playing. Drain before reusing
// a stream number for a new generation; queued UI notifications still need
// the generation tag captured by the worker when they were received.
inline HRESULT HdPurgeAndDrain(ISpVoice* voice, DWORD timeoutMs = 3000) {
    if (!voice) return E_POINTER;
    ULONG previous = 0;
    HRESULT hr = voice->GetSyncSpeakTimeout(&previous);
    if (FAILED(hr)) return hr;
    hr = voice->SetSyncSpeakTimeout(timeoutMs);
    if (FAILED(hr)) return hr;
    hr = voice->Speak(nullptr, SPF_PURGEBEFORESPEAK | SPF_IS_NOT_XML, nullptr);
    voice->SetSyncSpeakTimeout(previous);
    if (FAILED(hr)) return hr;
    hr = voice->WaitUntilDone(timeoutMs);
    if (hr != S_OK) return FAILED(hr) ? hr : HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    SPEVENT event{};
    ULONG count = 0;
    while (voice->GetEvents(1, &event, &count) == S_OK && count) {
        if (event.elParamType == SPET_LPARAM_IS_TOKEN || event.elParamType == SPET_LPARAM_IS_OBJECT) {
            if (event.lParam) reinterpret_cast<IUnknown*>(event.lParam)->Release();
        } else if (event.elParamType == SPET_LPARAM_IS_POINTER || event.elParamType == SPET_LPARAM_IS_STRING)
            CoTaskMemFree(reinterpret_cast<void*>(event.lParam));
        event = {};
    }
    return S_OK;
}
