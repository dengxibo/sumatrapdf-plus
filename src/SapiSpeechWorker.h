#pragma once

#include <windows.h>
#include <sapi.h>
#include <stdlib.h>
#include <wchar.h>
#include "HdPrepare.h"

// Playback COM objects never leave this thread. UI notifications are wakeups;
// identity is captured here, before dispatch, in generation + logical chunk.
class SapiSpeechWorker {
  public:
    struct Event {
        Event* next = nullptr;
        ULONGLONG generation = 0;
        ULONG chunk = 0;
        WORD kind = 0;
        ULONG position = 0;
        ULONG length = 0;
        HRESULT error = S_OK;
        bool prepared = false;
        HRESULT bufferStatus = S_OK;
    };

  private:
    struct Command {
        Command* next = nullptr;
        ULONGLONG generation = 0;
        ULONG chunk = 0;
        WCHAR* text = nullptr;
        WCHAR* voice = nullptr;
        LONG rate = 0;
        bool following = false;
        bool xml = false;
    };
    struct Segment {
        ULONGLONG generation = 0;
        ULONG chunk = 0;
        ULONG stream = 0;
        WCHAR* text = nullptr;
        bool started = false;
        bool ended = false;
        ULONGLONG ticket = 0;
        IHdPreparedAudio* audio = nullptr;
    };
    CRITICAL_SECTION lock;
    HANDLE wake = nullptr;
    HANDLE thread = nullptr;
    bool quitting = false;
    ULONGLONG requestedGeneration = 1;
    Command* commands = nullptr;
    Command* commandTail = nullptr;
    Event* events = nullptr;
    Event* eventTail = nullptr;
    HWND hwnd = nullptr;
    UINT message = 0;
    WPARAM wp = 0;
    LPARAM lp = 0;
    // Everything below is worker-only.
    HRESULT bufferStatus = S_OK;
    CLSID preparerClass;
    ISpVoice* voice = nullptr;
    IHdSpeechPreparer* preparer = nullptr;
    ULONGLONG generation = 0;
    Segment current;
    Segment following;

    static WCHAR* Copy(const WCHAR* s) { return s ? _wcsdup(s) : nullptr; }
    static void FreeCommand(Command* c) {
        free(c->text);
        free(c->voice);
        delete c;
    }
    void ClearSegment(Segment& s) {
        if (s.ticket && preparer) preparer->Cancel(s.ticket);
        if (s.audio) s.audio->Release();
        free(s.text);
        s = {};
    }
    bool IsCurrent(ULONGLONG g) {
        EnterCriticalSection(&lock);
        bool result = !quitting && requestedGeneration == g;
        LeaveCriticalSection(&lock);
        return result;
    }
    void Publish(const Segment& s, WORD kind, ULONG pos = 0, ULONG length = 0, HRESULT error = S_OK) {
        EnterCriticalSection(&lock);
        if (!quitting && s.generation == requestedGeneration) {
            // Keep event storage bounded even when the UI is busy extracting a
            // document: only the latest adjacent WORD is needed for highlighting.
            if (kind == SPEI_WORD_BOUNDARY && eventTail && eventTail->kind == kind && eventTail->chunk == s.chunk) {
                eventTail->position = pos;
                eventTail->length = length;
            } else {
                Event* e = new Event;
                e->generation = s.generation;
                e->chunk = s.chunk;
                e->kind = kind;
                e->position = pos;
                e->length = length;
                e->error = error;
                e->prepared = s.audio != nullptr;
                e->bufferStatus = bufferStatus;
                if (eventTail)
                    eventTail->next = e;
                else
                    events = e;
                eventTail = e;
            }
            if (hwnd && message) PostMessageW(hwnd, message, wp, lp);
        }
        LeaveCriticalSection(&lock);
    }
    HRESULT OpenVoice() {
        if (voice) return S_OK;
        HRESULT hr = CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_INPROC_SERVER, IID_ISpVoice, (void**)&voice);
        if (FAILED(hr)) return hr;
        ULONGLONG interest = SPFEI(SPEI_START_INPUT_STREAM) | SPFEI(SPEI_WORD_BOUNDARY) | SPFEI(SPEI_END_INPUT_STREAM);
        hr = voice->SetNotifyWin32Event();
        if (SUCCEEDED(hr)) hr = voice->SetInterest(interest, interest);
        if (FAILED(hr)) {
            voice->Release();
            voice = nullptr;
            return hr;
        }
        // Unsupported outputs retain their normal buffer and ordinary playback.
        bufferStatus = HdConfigureSapiQueue(voice, 5000);
        return S_OK;
    }
    void Reset(ULONGLONG g) {
        if (voice) {
            HRESULT hr = HdPurgeAndDrain(voice);
            if (FAILED(hr)) {
                // Never reuse an instance whose abort/drain was not confirmed.
                voice->Release();
                voice = nullptr;
            }
        }
        ClearSegment(current);
        ClearSegment(following);
        generation = g;
        if (preparer && FAILED(preparer->AdvanceGeneration(g))) {
            preparer->Release();
            preparer = nullptr;
        }
    }
    HRESULT Select(const Command& c) {
        HRESULT hr = OpenVoice();
        if (FAILED(hr)) return hr;
        ISpObjectToken* token = nullptr;
        if (c.voice && *c.voice) {
            hr = CoCreateInstance(CLSID_SpObjectToken, nullptr, CLSCTX_INPROC_SERVER, IID_ISpObjectToken,
                                  (void**)&token);
            if (SUCCEEDED(hr)) hr = token->SetId(nullptr, c.voice, FALSE);
        }
        if (SUCCEEDED(hr)) hr = voice->SetVoice(token);
        if (token) token->Release();
        if (SUCCEEDED(hr)) hr = voice->SetRate(c.rate);
        return hr;
    }
    void Fail(Segment& s, HRESULT hr) {
        Publish(s, SPEI_END_INPUT_STREAM, 0, 0, hr);
        s.ended = true;
    }
    void QueueFollowing(bool deadline) {
        if (!following.chunk || following.stream || following.ended || !IsCurrent(generation)) return;
        HRESULT hr = E_PENDING;
        if (following.ticket && preparer) {
            HD_PREPARE_STATUS status;
            hr = preparer->GetStatus(following.ticket, &status);
            if (SUCCEEDED(hr) && status.state == HD_READY && status.generation == generation) {
                hr = preparer->Acquire(following.ticket, &following.audio);
                if (!IsCurrent(generation)) return;
                if (SUCCEEDED(hr))
                    hr = HdQueuePrepared(voice, following.audio, following.text, generation, SPF_ASYNC,
                                         &following.stream);
                if (SUCCEEDED(hr)) {
                    // Cancel invalidates reads from acquired audio too. Retain
                    // the ticket and lease until END or a completed purge.
                    return;
                }
                if (following.audio) {
                    following.audio->Release();
                    following.audio = nullptr;
                }
            } else if (SUCCEEDED(hr) && (status.state == HD_QUEUED || status.state == HD_RUNNING) && !deadline) {
                return;
            }
            // Cancel before fallback so a late READY can never enqueue it again.
            preparer->Cancel(following.ticket);
            following.ticket = 0;
        }
        if (!IsCurrent(generation)) return;
        hr = voice->Speak(following.text, SPF_ASYNC | SPF_IS_NOT_XML, &following.stream);
        if (FAILED(hr)) Fail(following, hr);
    }
    void Execute(Command& c) {
        if (!IsCurrent(c.generation)) return;
        if (generation != c.generation) Reset(c.generation);
        if (!IsCurrent(c.generation)) return;
        if (!c.text) return; // Stop barrier.
        if (!c.following) {
            HRESULT hr = Select(c);
            if (!IsCurrent(c.generation)) return;
            ClearSegment(current);
            current.generation = generation;
            current.chunk = c.chunk;
            current.text = Copy(c.text);
            if (SUCCEEDED(hr))
                hr = voice->Speak(c.text, SPF_ASYNC | (c.xml ? SPF_IS_XML : SPF_IS_NOT_XML), &current.stream);
            if (FAILED(hr)) Fail(current, hr);
            return;
        }
        // Logical chunk IDs stay unique across SAPI's stream-number reuse.
        if (following.chunk && following.chunk != c.chunk) {
            ClearSegment(current);
            current = following;
            following = {};
        }
        following.generation = generation;
        following.chunk = c.chunk;
        following.text = Copy(c.text);
        if (!voice || !current.chunk) {
            Fail(following, E_UNEXPECTED);
            return;
        }
        // This optional service is for the installed HD adapter. Other SAPI
        // voices retain the ordinary queue without a duplicate synthesizer.
        if (c.voice && wcsstr(c.voice, L"TtsApplicationHD-")) {
            if (!preparer) {
                HRESULT hr = CoCreateInstance(preparerClass, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IHdSpeechPreparer),
                                              (void**)&preparer);
                if (SUCCEEDED(hr)) {
                    HD_PREPARE_CAPS caps;
                    hr = preparer->GetCapabilities(&caps);
                    ULONG required =
                        HD_CAP_PLAIN_TEXT | HD_CAP_ASYNC | HD_CAP_SAPI_AUDIO_EVENTS | HD_CAP_PLAYBACK_VOLUME;
                    if (SUCCEEDED(hr) && (caps.version != 1 || (caps.flags & required) != required)) hr = E_NOINTERFACE;
                    HD_PREPARE_LIMITS limits;
                    if (SUCCEEDED(hr)) hr = preparer->SetLimits(&limits);
                    if (SUCCEEDED(hr)) hr = preparer->AdvanceGeneration(generation);
                    if (FAILED(hr)) {
                        preparer->Release();
                        preparer = nullptr;
                    }
                }
            }
            if (preparer) {
                ISpObjectToken* token = nullptr;
                WCHAR* id = nullptr;
                HRESULT hr = voice->GetVoice(&token);
                if (SUCCEEDED(hr)) {
                    hr = token->GetId(&id);
                    token->Release();
                }
                if (SUCCEEDED(hr)) preparer->Prepare(c.text, id, c.rate, generation, &following.ticket);
                CoTaskMemFree(id);
            }
        }
        QueueFollowing(current.ended);
    }
    void Pump() {
        if (!voice || !IsCurrent(generation)) return;
        SPEVENT e{};
        ULONG count = 0;
        while (voice->GetEvents(1, &e, &count) == S_OK && count) {
            Segment* s = e.ulStreamNum == current.stream     ? &current
                         : e.ulStreamNum == following.stream ? &following
                                                             : nullptr;
            if (s && !s->ended) {
                if (e.eEventId == SPEI_START_INPUT_STREAM && !s->started) {
                    s->started = true;
                    Publish(*s, e.eEventId);
                } else if (s->started && e.eEventId == SPEI_WORD_BOUNDARY) {
                    ULONG pos = (ULONG)e.lParam;
                    ULONG len = (ULONG)e.wParam;
                    size_t textLen = wcslen(s->text);
                    if (pos <= textLen && len <= textLen - pos) Publish(*s, e.eEventId, pos, len);
                } else if (s->started && e.eEventId == SPEI_END_INPUT_STREAM) {
                    s->ended = true;
                    Publish(*s, e.eEventId);
                }
            }
            if (e.elParamType == SPET_LPARAM_IS_TOKEN || e.elParamType == SPET_LPARAM_IS_OBJECT) {
                if (e.lParam) ((IUnknown*)e.lParam)->Release();
            } else if (e.elParamType == SPET_LPARAM_IS_POINTER || e.elParamType == SPET_LPARAM_IS_STRING) {
                CoTaskMemFree((void*)e.lParam);
            }
            e = {};
        }
        QueueFollowing(current.ended);
    }
    static DWORD WINAPI ThreadProc(void* p) {
        SapiSpeechWorker* self = (SapiSpeechWorker*)p;
        HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        for (;;) {
            WaitForSingleObject(self->wake, 10);
            EnterCriticalSection(&self->lock);
            bool quit = self->quitting;
            Command* list = self->commands;
            self->commands = self->commandTail = nullptr;
            LeaveCriticalSection(&self->lock);
            while (list) {
                Command* next = list->next;
                if (!quit) {
                    if (SUCCEEDED(initialized))
                        self->Execute(*list);
                    else {
                        Segment failed;
                        failed.generation = list->generation;
                        failed.chunk = list->chunk;
                        if (failed.chunk) self->Fail(failed, initialized);
                    }
                }
                FreeCommand(list);
                list = next;
            }
            if (quit) break;
            if (SUCCEEDED(initialized)) self->Pump();
        }
        self->Reset(self->generation + 1);
        if (self->preparer) {
            self->preparer->Release();
            self->preparer = nullptr;
        }
        if (self->voice) {
            self->voice->Release();
            self->voice = nullptr;
        }
        if (SUCCEEDED(initialized)) CoUninitialize();
        return 0;
    }

  public:
    explicit SapiSpeechWorker(const CLSID& serviceClass = CLSID_HdSpeechPreparer) : preparerClass(serviceClass) {
        InitializeCriticalSection(&lock);
    }
    ~SapiSpeechWorker() {
        Shutdown();
        DeleteCriticalSection(&lock);
    }
    bool IsRunning() const { return thread != nullptr; }
    bool Start() {
        if (thread) return true;
        wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!wake) return false;
        quitting = false;
        thread = CreateThread(nullptr, 0, ThreadProc, this, 0, nullptr);
        if (!thread) {
            CloseHandle(wake);
            wake = nullptr;
        }
        return thread != nullptr;
    }
    void Notify(HWND window, UINT msg, WPARAM w, LPARAM l) {
        EnterCriticalSection(&lock);
        hwnd = window;
        message = msg;
        wp = w;
        lp = l;
        LeaveCriticalSection(&lock);
    }
    ULONGLONG Advance() {
        EnterCriticalSection(&lock);
        ULONGLONG g = ++requestedGeneration;
        while (events) {
            Event* next = events->next;
            delete events;
            events = next;
        }
        eventTail = nullptr;
        // Drop commands made obsolete before the worker has picked them up.
        while (commands) {
            Command* next = commands->next;
            FreeCommand(commands);
            commands = next;
        }
        commandTail = nullptr;
        LeaveCriticalSection(&lock);
        return g;
    }
    bool Submit(ULONGLONG g, ULONG chunk, const WCHAR* text, const WCHAR* token, LONG rate, bool next,
                bool xml = false) {
        if (!Start()) return false;
        Command* c = new Command;
        c->generation = g;
        c->chunk = chunk;
        c->text = Copy(text);
        c->voice = Copy(token);
        c->rate = rate;
        c->following = next;
        c->xml = xml;
        EnterCriticalSection(&lock);
        if (commandTail)
            commandTail->next = c;
        else
            commands = c;
        commandTail = c;
        SetEvent(wake);
        LeaveCriticalSection(&lock);
        return true;
    }
    bool Pop(ULONGLONG g, ULONG chunk, Event& result) {
        EnterCriticalSection(&lock);
        bool found = events && events->generation == g && events->chunk == chunk;
        if (found) {
            Event* e = events;
            events = e->next;
            if (!events) eventTail = nullptr;
            result = *e;
            result.next = nullptr;
            delete e;
        }
        LeaveCriticalSection(&lock);
        return found;
    }
    void Shutdown() {
        if (!thread) return;
        EnterCriticalSection(&lock);
        quitting = true;
        SetEvent(wake);
        LeaveCriticalSection(&lock);
        WaitForSingleObject(thread, INFINITE);
        CloseHandle(thread);
        thread = nullptr;
        CloseHandle(wake);
        wake = nullptr;
        Advance();
    }
};
