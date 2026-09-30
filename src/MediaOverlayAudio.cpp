/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/ScopedWin.h"
#include "utils/Log.h"

#include "MediaOverlayAudio.h"

// The media engine interfaces are declared for Windows 8+ only; the project targets Windows 7,
// where creating the engine simply fails at runtime and narration reports that audio is unavailable.
#pragma push_macro("WINVER")
#pragma push_macro("_WIN32_WINNT")
#pragma push_macro("NTDDI_VERSION")
#undef WINVER
#undef _WIN32_WINNT
#undef NTDDI_VERSION
#define WINVER 0x0602
#define _WIN32_WINNT 0x0602
#define NTDDI_VERSION 0x06020000
#include <mfapi.h>
#include <mfidl.h>
#include <mfmediaengine.h>
#pragma pop_macro("NTDDI_VERSION")
#pragma pop_macro("_WIN32_WINNT")
#pragma pop_macro("WINVER")

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")

class MoEngineNotify : public IMFMediaEngineNotify {
    LONG refs = 1;

  public:
    volatile LONG metadataLoaded = 0;
    volatile LONG ended = 0;
    volatile LONG error = 0;

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFMediaEngineNotify)) {
            *ppv = static_cast<IMFMediaEngineNotify*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs); }
    STDMETHODIMP_(ULONG) Release() override {
        LONG n = InterlockedDecrement(&refs);
        if (n == 0) {
            delete this;
        }
        return n;
    }
    // called on a Media Foundation worker thread
    STDMETHODIMP EventNotify(DWORD event, DWORD_PTR param1, DWORD param2) override {
        switch (event) {
            case MF_MEDIA_ENGINE_EVENT_LOADEDMETADATA:
                InterlockedExchange(&metadataLoaded, 1);
                break;
            case MF_MEDIA_ENGINE_EVENT_ENDED:
                InterlockedExchange(&ended, 1);
                break;
            case MF_MEDIA_ENGINE_EVENT_ERROR:
                logf("MoAudio: media engine error %d hr=0x%x\n", (int)param1, (unsigned)param2);
                InterlockedExchange(&error, 1);
                break;
            case MF_MEDIA_ENGINE_EVENT_EMPTIED:
                InterlockedExchange(&metadataLoaded, 0);
                break;
        }
        return S_OK;
    }
};

static bool gMfStarted = false;
static IMFMediaEngine* gEngine = nullptr;
static IMFMediaEngineEx* gEngineEx = nullptr;
static MoEngineNotify* gNotify = nullptr;
static bool gWantPlay = false;
static i64 gPendingSeekUs = -1;
static double gRate = 1.0;

static void MoAudioApplyRateToEngine() {
    if (!gEngine) {
        return;
    }
    gEngine->SetDefaultPlaybackRate(gRate);
    gEngine->SetPlaybackRate(gRate);
}

static void ReleaseEngine() {
    if (gEngine) {
        gEngine->Shutdown();
    }
    if (gEngineEx) {
        gEngineEx->Release();
        gEngineEx = nullptr;
    }
    if (gEngine) {
        gEngine->Release();
        gEngine = nullptr;
    }
    if (gNotify) {
        gNotify->Release();
        gNotify = nullptr;
    }
}

static bool CreateEngine() {
    if (!gMfStarted) {
        HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
        if (FAILED(hr)) {
            logf("MoAudio: MFStartup failed 0x%x\n", (unsigned)hr);
            return false;
        }
        gMfStarted = true;
    }
    IMFMediaEngineClassFactory* factory = nullptr;
    HRESULT hr =
        CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        logf("MoAudio: media engine factory failed 0x%x\n", (unsigned)hr);
        return false;
    }
    IMFAttributes* attrs = nullptr;
    hr = MFCreateAttributes(&attrs, 2);
    if (FAILED(hr)) {
        factory->Release();
        return false;
    }
    gNotify = new MoEngineNotify();
    attrs->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, gNotify);
    // AudioCategory_Media (audiosessiontypes.h, Windows 8+)
    attrs->SetUINT32(MF_MEDIA_ENGINE_AUDIO_CATEGORY, 11);
    hr = factory->CreateInstance(MF_MEDIA_ENGINE_AUDIOONLY, attrs, &gEngine);
    attrs->Release();
    factory->Release();
    if (FAILED(hr) || !gEngine) {
        logf("MoAudio: CreateInstance failed 0x%x\n", (unsigned)hr);
        ReleaseEngine();
        return false;
    }
    hr = gEngine->QueryInterface(IID_PPV_ARGS(&gEngineEx));
    if (FAILED(hr)) {
        logf("MoAudio: IMFMediaEngineEx not available 0x%x\n", (unsigned)hr);
        ReleaseEngine();
        return false;
    }
    gEngine->SetAutoPlay(FALSE);
    gEngine->SetPreload(MF_MEDIA_ENGINE_PRELOAD_AUTOMATIC);
    return true;
}

// Format hint for the source resolver, from the data rather than the publisher's file name, so
// only plain audio containers are ever parsed (never playlists such as .m3u8 / .asx that could
// make Media Foundation fetch network resources). EPUB core audio types: MP3, MP4/AAC, Ogg Opus.
static const char* MoAudioSniffExt(const u8* d, size_t n) {
    if (n >= 3 && d[0] == 'I' && d[1] == 'D' && d[2] == '3') {
        return ".mp3";
    }
    if (n >= 2 && d[0] == 0xFF && (d[1] & 0xE0) == 0xE0) {
        // MPEG audio frame sync; ADTS AAC also starts with 0xFFF
        return (d[1] & 0x06) == 0 ? ".aac" : ".mp3";
    }
    if (n >= 12 && memcmp(d + 4, "ftyp", 4) == 0) {
        return ".m4a";
    }
    if (n >= 4 && memcmp(d, "OggS", 4) == 0) {
        return ".ogg";
    }
    if (n >= 12 && memcmp(d, "RIFF", 4) == 0 && memcmp(d + 8, "WAVE", 4) == 0) {
        return ".wav";
    }
    return nullptr;
}

bool MoAudioSetSource(u8* data, size_t size, const char* name) {
    gWantPlay = false;
    gPendingSeekUs = -1;
    if (!data || size == 0 || size > (size_t)UINT32_MAX) {
        free(data);
        return false;
    }
    const char* ext = MoAudioSniffExt(data, size);
    if (!ext) {
        logf("MoAudio: '%s' is not MP3, MP4, Ogg or WAV audio\n", name ? name : "");
        free(data);
        return false;
    }
    if (!gEngine && !CreateEngine()) {
        free(data);
        return false;
    }
    InterlockedExchange(&gNotify->metadataLoaded, 0);
    InterlockedExchange(&gNotify->ended, 0);
    InterlockedExchange(&gNotify->error, 0);

    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!mem) {
        free(data);
        return false;
    }
    void* dst = GlobalLock(mem);
    memcpy(dst, data, size);
    GlobalUnlock(mem);
    free(data);

    IStream* stream = nullptr;
    HRESULT hr = CreateStreamOnHGlobal(mem, TRUE, &stream);
    if (FAILED(hr)) {
        GlobalFree(mem);
        return false;
    }
    IMFByteStream* bs = nullptr;
    hr = MFCreateMFByteStreamOnStream(stream, &bs);
    stream->Release();
    if (FAILED(hr)) {
        return false;
    }
    bs->SetLength((QWORD)size);

    // the URL is only a format hint for the source resolver; nothing is opened by name
    TempStr url = str::FormatTemp("narration%s", ext);
    BSTR burl = SysAllocString(ToWStrTemp(url));
    hr = gEngineEx->SetSourceFromByteStream(bs, burl);
    SysFreeString(burl);
    bs->Release();
    if (FAILED(hr)) {
        logf("MoAudio: SetSourceFromByteStream failed 0x%x (%s)\n", (unsigned)hr, name ? name : "");
        return false;
    }
    MoAudioApplyRateToEngine();
    return true;
}

void MoAudioClose() {
    gWantPlay = false;
    gPendingSeekUs = -1;
    ReleaseEngine();
}

bool MoAudioIsReady() {
    return gEngine && gNotify && InterlockedCompareExchange(&gNotify->metadataLoaded, 0, 0) != 0;
}

bool MoAudioHasError() {
    return gNotify && InterlockedCompareExchange(&gNotify->error, 0, 0) != 0;
}

bool MoAudioEnded() {
    if (!gEngine) {
        return false;
    }
    return (gNotify && InterlockedCompareExchange(&gNotify->ended, 0, 0) != 0) || gEngine->IsEnded();
}

void MoAudioTick() {
    if (!MoAudioIsReady()) {
        return;
    }
    if (gPendingSeekUs >= 0) {
        gEngine->SetCurrentTime((double)gPendingSeekUs / 1e6);
        gPendingSeekUs = -1;
    }
    if (gWantPlay && gEngine->IsPaused()) {
        gEngine->Play();
    }
    // Media Foundation often resets the rate when metadata arrives or Play() starts
    double cur = gEngine->GetPlaybackRate();
    if (cur < gRate - 0.01 || cur > gRate + 0.01) {
        MoAudioApplyRateToEngine();
    }
}

void MoAudioPlay() {
    gWantPlay = true;
    if (gNotify) {
        InterlockedExchange(&gNotify->ended, 0);
    }
    MoAudioTick();
}

void MoAudioPause() {
    gWantPlay = false;
    if (gEngine) {
        gEngine->Pause();
    }
}

bool MoAudioIsPlaying() {
    return gWantPlay && gEngine && !MoAudioHasError();
}

void MoAudioSeekUs(i64 us) {
    if (us < 0) {
        us = 0;
    }
    if (gNotify) {
        InterlockedExchange(&gNotify->ended, 0);
    }
    if (!MoAudioIsReady()) {
        gPendingSeekUs = us;
        return;
    }
    gPendingSeekUs = -1;
    gEngine->SetCurrentTime((double)us / 1e6);
}

i64 MoAudioPositionUs() {
    if (gPendingSeekUs >= 0) {
        return gPendingSeekUs;
    }
    if (!MoAudioIsReady()) {
        return 0;
    }
    return (i64)(gEngine->GetCurrentTime() * 1e6);
}

i64 MoAudioDurationUs() {
    if (!MoAudioIsReady()) {
        return -1;
    }
    double d = gEngine->GetDuration();
    if (!(d > 0) || d > 1e9) {
        return -1;
    }
    return (i64)(d * 1e6);
}

void MoAudioSetRate(double rate) {
    if (rate < 0.25) {
        rate = 0.25;
    }
    if (rate > 4.0) {
        rate = 4.0;
    }
    gRate = rate;
    MoAudioApplyRateToEngine();
}
