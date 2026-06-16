/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/ScopedWin.h"
#include "utils/Log.h"

#include "LookupAudio.h"

#include <objbase.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mfreadwrite.h>
#include <Audioclient.h>
#include <mmdeviceapi.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

#include <algorithm>

struct LookupPcm {
    WAVEFORMATEX* format = nullptr;
    DWORD formatSize = 0;
    u8* data = nullptr;
    size_t dataSize = 0;
};

struct LookupAudioPlayCtx {
    u8* data = nullptr;
    size_t size = 0;
    char* ext = nullptr;
};

static CRITICAL_SECTION gLookupAudioCs;
static volatile LONG gLookupAudioCsReady = 0;
static volatile LONG gLookupAudioMfStarted = 0;
static HANDLE gLookupAudioThread = nullptr;
static IAudioClient* gLookupAudioClient = nullptr;
static volatile LONG gLookupAudioPlaying = 0;
static volatile LONG gLookupAudioStop = 0;

static CRITICAL_SECTION* LookupAudioCs() {
    if (InterlockedCompareExchange(&gLookupAudioCsReady, 1, 0) == 0) {
        InitializeCriticalSection(&gLookupAudioCs);
        InterlockedExchange(&gLookupAudioCsReady, 2);
        return &gLookupAudioCs;
    }
    while (InterlockedCompareExchange(&gLookupAudioCsReady, 2, 2) != 2) {
        YieldProcessor();
    }
    return &gLookupAudioCs;
}

static bool EnsureLookupAudioMf() {
    if (InterlockedCompareExchange(&gLookupAudioMfStarted, 1, 0) == 0) {
        HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
        if (FAILED(hr)) {
            logfa("LookupAudio: MFStartup failed: 0x%x\n", (unsigned)hr);
            return false;
        }
    }
    return true;
}

static bool LookupPcmAppend(LookupPcm* pcm, const u8* src, size_t n) {
    if (!src || n == 0) {
        return true;
    }
    size_t newSize = pcm->dataSize + n;
    u8* grown = (u8*)realloc(pcm->data, newSize);
    if (!grown) {
        return false;
    }
    memcpy(grown + pcm->dataSize, src, n);
    pcm->data = grown;
    pcm->dataSize = newSize;
    return true;
}

static void LookupFreePcm(LookupPcm* pcm) {
    if (!pcm) {
        return;
    }
    free(pcm->data);
    CoTaskMemFree(pcm->format);
    pcm->data = nullptr;
    pcm->dataSize = 0;
    pcm->format = nullptr;
    pcm->formatSize = 0;
}

static bool LookupGetDefaultMixFormat(WAVEFORMATEX** fmtOut) {
    if (!fmtOut) {
        return false;
    }
    *fmtOut = nullptr;

    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) {
        return false;
    }

    IMMDevice* device = nullptr;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    enumerator->Release();
    if (FAILED(hr)) {
        return false;
    }

    IAudioClient* client = nullptr;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client);
    device->Release();
    if (FAILED(hr)) {
        return false;
    }

    hr = client->GetMixFormat(fmtOut);
    client->Release();
    return SUCCEEDED(hr) && *fmtOut;
}

static bool LookupDecodeAudioToPcm(const u8* data, size_t size, const char* ext, LookupPcm* pcm,
                                   const WAVEFORMATEX* targetFmt) {
    if (!data || size == 0 || !pcm) {
        return false;
    }
    if (!EnsureLookupAudioMf()) {
        return false;
    }

    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!mem) {
        return false;
    }
    void* memPtr = GlobalLock(mem);
    if (!memPtr) {
        GlobalFree(mem);
        return false;
    }
    memcpy(memPtr, data, size);
    GlobalUnlock(mem);

    IStream* stream = nullptr;
    HRESULT hr = CreateStreamOnHGlobal(mem, TRUE, &stream);
    if (FAILED(hr)) {
        logfa("LookupAudio: CreateStreamOnHGlobal failed: 0x%x\n", (unsigned)hr);
        return false;
    }

    IMFByteStream* byteStream = nullptr;
    hr = MFCreateMFByteStreamOnStream(stream, &byteStream);
    stream->Release();
    if (FAILED(hr)) {
        logfa("LookupAudio: MFCreateMFByteStreamOnStream failed: 0x%x\n", (unsigned)hr);
        return false;
    }
    byteStream->SetLength((ULONG)size);

    IMFSourceReader* reader = nullptr;
    IMFSourceResolver* resolver = nullptr;
    hr = MFCreateSourceResolver(&resolver);
    if (SUCCEEDED(hr)) {
        TempStr url = str::IsEmpty(ext) ? str::DupTemp("audio.bin") : str::FormatTemp("audio.%s", ext);
        TempWStr urlW = ToWStrTemp(url);
        MF_OBJECT_TYPE objType = MF_OBJECT_INVALID;
        IUnknown* sourceUnk = nullptr;
        hr = resolver->CreateObjectFromByteStream(byteStream, urlW, MF_RESOLUTION_MEDIASOURCE, nullptr, &objType,
                                                 &sourceUnk);
        resolver->Release();
        if (SUCCEEDED(hr) && sourceUnk) {
            IMFMediaSource* mediaSource = nullptr;
            hr = sourceUnk->QueryInterface(IID_PPV_ARGS(&mediaSource));
            sourceUnk->Release();
            if (SUCCEEDED(hr) && mediaSource) {
                IMFAttributes* readerAttrs = nullptr;
                MFCreateAttributes(&readerAttrs, 1);
                hr = MFCreateSourceReaderFromMediaSource(mediaSource, readerAttrs, &reader);
                if (readerAttrs) {
                    readerAttrs->Release();
                }
                mediaSource->Release();
            }
        }
    }
    if (!reader) {
        IMFAttributes* readerAttrs = nullptr;
        MFCreateAttributes(&readerAttrs, 1);
        hr = MFCreateSourceReaderFromByteStream(byteStream, readerAttrs, &reader);
        if (readerAttrs) {
            readerAttrs->Release();
        }
    }
    byteStream->Release();
    if (FAILED(hr) || !reader) {
        logfa("LookupAudio: create source reader failed: 0x%x ext=%s\n", (unsigned)hr, ext ? ext : "");
        return false;
    }

    hr = reader->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    if (SUCCEEDED(hr)) {
        hr = reader->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
    }

    IMFMediaType* outType = nullptr;
    if (SUCCEEDED(hr)) {
        hr = MFCreateMediaType(&outType);
    }
    if (SUCCEEDED(hr)) {
        hr = outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    }
    if (SUCCEEDED(hr)) {
        hr = outType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    }
    if (SUCCEEDED(hr)) {
        hr = outType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    }
    if (SUCCEEDED(hr)) {
        hr = outType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 0);
    }
    if (SUCCEEDED(hr) && targetFmt && targetFmt->nSamplesPerSec > 0 && targetFmt->nChannels > 0) {
        hr = outType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, targetFmt->nSamplesPerSec);
    }
    if (SUCCEEDED(hr) && targetFmt && targetFmt->nChannels > 0) {
        hr = outType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, targetFmt->nChannels);
    }
    if (SUCCEEDED(hr)) {
        hr = reader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, outType);
    }
    if (outType) {
        outType->Release();
    }
    if (FAILED(hr)) {
        reader->Release();
        logfa("LookupAudio: SetCurrentMediaType PCM failed: 0x%x\n", (unsigned)hr);
        return false;
    }

    IMFMediaType* curType = nullptr;
    hr = reader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, &curType);
    if (FAILED(hr)) {
        reader->Release();
        logfa("LookupAudio: GetCurrentMediaType failed: 0x%x\n", (unsigned)hr);
        return false;
    }
    UINT32 fmtSize = 0;
    WAVEFORMATEX* fmt = nullptr;
    hr = MFCreateWaveFormatExFromMFMediaType(curType, &fmt, &fmtSize);
    curType->Release();
    if (FAILED(hr) || !fmt || fmtSize < sizeof(WAVEFORMATEX)) {
        reader->Release();
        CoTaskMemFree(fmt);
        logfa("LookupAudio: MFCreateWaveFormatExFromMFMediaType failed: 0x%x\n", (unsigned)hr);
        return false;
    }
    pcm->format = (WAVEFORMATEX*)CoTaskMemAlloc(fmtSize);
    if (!pcm->format) {
        reader->Release();
        CoTaskMemFree(fmt);
        return false;
    }
    memcpy(pcm->format, fmt, fmtSize);
    pcm->formatSize = fmtSize;
    CoTaskMemFree(fmt);

    for (;;) {
        if (InterlockedCompareExchange(&gLookupAudioStop, 0, 0) != 0) {
            reader->Release();
            return false;
        }
        DWORD streamIndex = 0;
        DWORD flags = 0;
        LONGLONG timestamp = 0;
        IMFSample* sample = nullptr;
        hr = reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &streamIndex, &flags, &timestamp,
                                &sample);
        if (FAILED(hr)) {
            reader->Release();
            logfa("LookupAudio: ReadSample failed: 0x%x\n", (unsigned)hr);
            return false;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            break;
        }
        if (!sample) {
            continue;
        }

        IMFMediaBuffer* buffer = nullptr;
        hr = sample->ConvertToContiguousBuffer(&buffer);
        sample->Release();
        if (FAILED(hr) || !buffer) {
            reader->Release();
            return false;
        }

        BYTE* bufData = nullptr;
        DWORD bufLen = 0;
        DWORD maxLen = 0;
        hr = buffer->Lock(&bufData, &maxLen, &bufLen);
        if (SUCCEEDED(hr)) {
            if (!LookupPcmAppend(pcm, bufData, bufLen)) {
                buffer->Unlock();
                buffer->Release();
                reader->Release();
                return false;
            }
            buffer->Unlock();
        }
        buffer->Release();
        if (FAILED(hr)) {
            reader->Release();
            return false;
        }
    }

    reader->Release();
    if (pcm->dataSize == 0) {
        logfa("LookupAudio: decoded zero PCM bytes\n");
        return false;
    }
    return true;
}

static bool LookupPlayPcmWasapi(const LookupPcm& pcm) {
    if (!pcm.data || pcm.dataSize == 0 || !pcm.format || pcm.format->nChannels == 0 ||
        pcm.format->nSamplesPerSec == 0) {
        return false;
    }

    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) {
        logfa("LookupAudio: MMDeviceEnumerator failed: 0x%x\n", (unsigned)hr);
        return false;
    }

    IMMDevice* device = nullptr;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    enumerator->Release();
    if (FAILED(hr)) {
        logfa("LookupAudio: GetDefaultAudioEndpoint failed: 0x%x\n", (unsigned)hr);
        return false;
    }

    IAudioClient* client = nullptr;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client);
    device->Release();
    if (FAILED(hr)) {
        logfa("LookupAudio: IAudioClient Activate failed: 0x%x\n", (unsigned)hr);
        return false;
    }

    WAVEFORMATEX* closest = nullptr;
    hr = client->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED, pcm.format, &closest);
    CoTaskMemFree(closest);
    bool needsAutoConvert = (hr == S_FALSE);
    if (FAILED(hr)) {
        logfa("LookupAudio: WASAPI IsFormatSupported failed: 0x%x tag=%u ch=%u rate=%u bits=%u align=%u\n",
              (unsigned)hr, (unsigned)pcm.format->wFormatTag, (unsigned)pcm.format->nChannels,
              (unsigned)pcm.format->nSamplesPerSec, (unsigned)pcm.format->wBitsPerSample,
              (unsigned)pcm.format->nBlockAlign);
        client->Release();
        return false;
    }

    DWORD streamFlags = 0;
    if (needsAutoConvert) {
        streamFlags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    }

    REFERENCE_TIME bufferDuration = 10000000; // 1 second
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, streamFlags, bufferDuration, 0, pcm.format, nullptr);
    if (FAILED(hr)) {
        logfa("LookupAudio: IAudioClient Initialize failed: 0x%x\n", (unsigned)hr);
        client->Release();
        return false;
    }

    IAudioRenderClient* render = nullptr;
    hr = client->GetService(__uuidof(IAudioRenderClient), (void**)&render);
    if (FAILED(hr)) {
        client->Release();
        return false;
    }

    UINT32 frameSize = (UINT32)pcm.format->nBlockAlign;
    if (frameSize == 0) {
        render->Release();
        client->Release();
        return false;
    }
    UINT32 totalFrames = (UINT32)(pcm.dataSize / frameSize);
    if (totalFrames == 0) {
        render->Release();
        client->Release();
        return false;
    }

    hr = client->Start();
    if (FAILED(hr)) {
        render->Release();
        client->Release();
        return false;
    }

    {
        ScopedCritSec lock(LookupAudioCs());
        if (gLookupAudioClient) {
            gLookupAudioClient->Release();
        }
        gLookupAudioClient = client;
        client->AddRef();
    }

    UINT32 bufferFrameCount = 0;
    client->GetBufferSize(&bufferFrameCount);
    UINT32 framesWritten = 0;
    while (framesWritten < totalFrames) {
        if (InterlockedCompareExchange(&gLookupAudioStop, 0, 0) != 0) {
            break;
        }
        UINT32 padding = 0;
        client->GetCurrentPadding(&padding);
        UINT32 framesAvailable = bufferFrameCount > padding ? bufferFrameCount - padding : 0;
        if (framesAvailable == 0) {
            Sleep(1);
            continue;
        }
        UINT32 framesToWrite = std::min(framesAvailable, totalFrames - framesWritten);
        BYTE* dst = nullptr;
        hr = render->GetBuffer(framesToWrite, &dst);
        if (FAILED(hr)) {
            break;
        }
        memcpy(dst, pcm.data + (size_t)framesWritten * frameSize, (size_t)framesToWrite * frameSize);
        render->ReleaseBuffer(framesToWrite, 0);
        framesWritten += framesToWrite;
    }

    if (InterlockedCompareExchange(&gLookupAudioStop, 0, 0) == 0) {
        for (int i = 0; i < 500; i++) {
            UINT32 padding = 0;
            client->GetCurrentPadding(&padding);
            if (padding == 0) {
                break;
            }
            if (InterlockedCompareExchange(&gLookupAudioStop, 0, 0) != 0) {
                break;
            }
            Sleep(10);
        }
    }

    client->Stop();
    render->Release();

    {
        ScopedCritSec lock(LookupAudioCs());
        if (gLookupAudioClient) {
            gLookupAudioClient->Release();
            gLookupAudioClient = nullptr;
        }
    }
    client->Release();

    if (framesWritten == 0) {
        logfa("LookupAudio: wrote zero frames to WASAPI\n");
        return false;
    }
    return true;
}

static DWORD WINAPI LookupAudioThreadProc(void* param) {
    ScopedCom com;
    LookupAudioPlayCtx* ctx = (LookupAudioPlayCtx*)param;
    LookupPcm pcm;
    WAVEFORMATEX* mixFmt = nullptr;
    LookupGetDefaultMixFormat(&mixFmt);
    bool decodeOk = LookupDecodeAudioToPcm(ctx->data, ctx->size, ctx->ext, &pcm, mixFmt);
    CoTaskMemFree(mixFmt);
    bool playOk = false;
    if (decodeOk) {
        playOk = LookupPlayPcmWasapi(pcm);
    } else {
        logfa("LookupAudio: decode failed for %u byte input ext=%s\n", (unsigned)ctx->size, ctx->ext ? ctx->ext : "");
    }
    LookupFreePcm(&pcm);
    free(ctx->data);
    str::Free(ctx->ext);
    free(ctx);
    InterlockedExchange(&gLookupAudioPlaying, 0);
    return playOk ? 0 : 1;
}

void LookupAudioStop() {
    InterlockedExchange(&gLookupAudioStop, 1);
    {
        ScopedCritSec lock(LookupAudioCs());
        if (gLookupAudioClient) {
            gLookupAudioClient->Stop();
        }
    }
    HANDLE thread = nullptr;
    {
        ScopedCritSec lock(LookupAudioCs());
        thread = gLookupAudioThread;
        gLookupAudioThread = nullptr;
    }
    if (thread) {
        WaitForSingleObject(thread, 2000);
        CloseHandle(thread);
    }
    InterlockedExchange(&gLookupAudioPlaying, 0);
    InterlockedExchange(&gLookupAudioStop, 0);
}

bool LookupAudioIsPlaying() {
    return InterlockedCompareExchange(&gLookupAudioPlaying, 0, 0) != 0;
}

bool LookupAudioPlayOwned(u8* data, size_t size, const char* ext) {
    if (!data || size == 0) {
        free(data);
        return false;
    }
    LookupAudioStop();

    auto* ctx = AllocStruct<LookupAudioPlayCtx>();
    ctx->data = data;
    ctx->size = size;
    ctx->ext = str::Dup(ext);

    InterlockedExchange(&gLookupAudioStop, 0);
    InterlockedExchange(&gLookupAudioPlaying, 1);

    HANDLE thread = CreateThread(nullptr, 0, LookupAudioThreadProc, ctx, 0, nullptr);
    if (!thread) {
        free(ctx->data);
        str::Free(ctx->ext);
        free(ctx);
        InterlockedExchange(&gLookupAudioPlaying, 0);
        return false;
    }
    {
        ScopedCritSec lock(LookupAudioCs());
        gLookupAudioThread = thread;
    }
    return true;
}
