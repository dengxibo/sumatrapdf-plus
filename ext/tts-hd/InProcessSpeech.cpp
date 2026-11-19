#ifndef _HAS_EXCEPTIONS
#define _HAS_EXCEPTIONS 0
#endif
#include "InProcessSpeech.h"
#include "HdSpeechRuntime.h"
#include <mutex>
#include <atomic>

#pragma comment(lib, "runtimeobject.lib")
#pragma comment(lib, "delayimp.lib")

namespace {
struct Engine {
    HANDLE wake = nullptr, ready = nullptr, done = nullptr, thread = nullptr;
    std::wstring root, model, name, locale, input;
    std::unique_ptr<HdSpeechRuntime> runtime;
    std::mutex callbackLock, stateLock;
    HdInProcessCallbacks callbacks{};
    HANDLE cancel = nullptr;
    bool active = false, speaking = false;
    std::atomic<bool> quit{false};
    HRESULT result = E_PENDING;

    Engine(const WCHAR* root, const WCHAR* model, const WCHAR* name, const WCHAR* locale)
        : root(root), model(model), name(name), locale(locale) {
        wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!wake || !ready || !done) {
            DWORD error = GetLastError();
            if (wake) CloseHandle(wake);
            if (ready) CloseHandle(ready);
            if (done) CloseHandle(done);
            winrt::throw_hresult(HRESULT_FROM_WIN32(error));
        }
        thread = CreateThread(nullptr, 0, Run, this, 0, nullptr);
        if (!thread) {
            DWORD error = GetLastError();
            CloseHandle(wake);
            CloseHandle(ready);
            CloseHandle(done);
            winrt::throw_hresult(HRESULT_FROM_WIN32(error));
        }
    }
    ~Engine() {
        quit = true;
        if (wake) SetEvent(wake);
        if (thread) {
            WaitForSingleObject(thread, INFINITE);
            CloseHandle(thread);
        }
        if (wake) CloseHandle(wake);
        if (ready) CloseHandle(ready);
        if (done) CloseHandle(done);
    }
    static DWORD WINAPI Run(void* context) {
        auto& self = *static_cast<Engine*>(context);
        HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        try {
            winrt::check_hresult(init);
            self.runtime = std::make_unique<HdSpeechRuntime>(
                self.root, self.model, self.name,
                [&](const BYTE* data, size_t size) {
                    std::lock_guard lock(self.callbackLock);
                    if (self.active && self.callbacks.audio && !self.Cancelled())
                        self.callbacks.audio(self.callbacks.context, data, size);
                },
                [&](HdSpeech::Type type, const HdSpeech::Event& event, const std::string& text) {
                    std::lock_guard lock(self.callbackLock);
                    if (self.active && self.callbacks.word && type == HdSpeech::Type::Word && !self.Cancelled())
                        self.callbacks.word(self.callbacks.context, event.ticks, text.c_str());
                });
            bool update = false;
            const wchar_t* sample = L"Hello, this is a test.";
            if (self.locale.starts_with(L"zh"))
                sample = L"你好。Hello, this is a test.";
            else if (self.locale.starts_with(L"ja"))
                sample = L"こんにちは。";
            else if (self.locale.starts_with(L"ko"))
                sample = L"안녕하세요.";
            else if (self.locale.starts_with(L"fr"))
                sample = L"Bonjour.";
            else if (self.locale.starts_with(L"de"))
                sample = L"Guten Tag.";
            else if (self.locale.starts_with(L"es"))
                sample = L"Hola.";
            else if (self.locale.starts_with(L"pt"))
                sample = L"Olá.";
            // Prime the same SSML/prosody path used for real requests, including
            // both languages for Chinese voices before any audio is published.
            auto warmup = L"<speak version=\"1.0\" xmlns=\"http://www.w3.org/2001/10/synthesis\" xml:lang=\"" +
                          self.locale + L"\"><voice name=\"" + self.name + L"\"><prosody rate=\"+0%\">" + sample +
                          L"</prosody></voice></speak>";
            int status = self.runtime->Speak(warmup, update, true);
            self.result = status == 0 && !update ? S_OK : E_FAIL;
        } catch (...) {
            self.result = winrt::to_hresult();
        }
        SetEvent(self.ready);
        if (SUCCEEDED(self.result)) {
            while (WaitForSingleObject(self.wake, INFINITE) == WAIT_OBJECT_0 && !self.quit) {
                try {
                    {
                        std::lock_guard lock(self.stateLock);
                        self.speaking = !self.Cancelled();
                    }
                    bool update = false;
                    int status = self.Cancelled() ? 1 : self.runtime->Speak(self.input, update, true);
                    self.result = self.Cancelled() ? HRESULT_FROM_WIN32(ERROR_CANCELLED)
                                                   : (status == 0 && !update ? S_OK : E_FAIL);
                } catch (...) {
                    self.result = winrt::to_hresult();
                }
                {
                    std::lock_guard lock(self.stateLock);
                    self.speaking = false;
                }
                {
                    std::lock_guard lock(self.callbackLock);
                    self.active = false;
                }
                SetEvent(self.done);
            }
        }
        self.runtime.reset();
        if (SUCCEEDED(init)) CoUninitialize();
        return 0;
    }
    bool Cancelled() const { return cancel && WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0; }
    static void CALLBACK Stop(void* context, BOOLEAN) noexcept {
        auto& self = *static_cast<Engine*>(context);
        HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        try {
            if (SUCCEEDED(init)) {
                for (;;) {
                    {
                        std::lock_guard lock(self.stateLock);
                        if (!self.speaking || !self.Cancelled()) break;
                        self.runtime->Stop();
                    }
                    // Cancellation can arrive immediately before Speak enters the SDK.
                    // Keep stopping until the request has actually drained.
                    if (WaitForSingleObject(self.done, 20) == WAIT_OBJECT_0) break;
                }
            }
        } catch (...) {
        }
        if (SUCCEEDED(init)) CoUninitialize();
    }
};
} // namespace

void* HdInProcessCreate(const WCHAR* root, const WCHAR* model, const WCHAR* name, const WCHAR* locale,
                        HRESULT* error) noexcept {
    try {
        auto engine = std::make_unique<Engine>(root, model, name, locale);
        WaitForSingleObject(engine->ready, INFINITE);
        *error = engine->result;
        return SUCCEEDED(*error) ? engine.release() : nullptr;
    } catch (...) {
        *error = winrt::to_hresult();
        return nullptr;
    }
}

HRESULT HdInProcessSpeak(void* context, const WCHAR* ssml, HANDLE cancel,
                         const HdInProcessCallbacks* callbacks) noexcept {
    auto& engine = *static_cast<Engine*>(context);
    HANDLE cancelWait = nullptr;
    try {
        engine.input = ssml;
        {
            std::lock_guard lock(engine.callbackLock);
            engine.callbacks = *callbacks;
            engine.cancel = cancel;
            engine.active = true;
        }
        ResetEvent(engine.done);
        if (cancel &&
            !RegisterWaitForSingleObject(&cancelWait, cancel, Engine::Stop, &engine, INFINITE, WT_EXECUTEONLYONCE))
            winrt::throw_last_error();
        SetEvent(engine.wake);
        WaitForSingleObject(engine.done, INFINITE);
        if (cancelWait) UnregisterWaitEx(cancelWait, INVALID_HANDLE_VALUE);
        return engine.result;
    } catch (...) {
        if (cancelWait) UnregisterWaitEx(cancelWait, INVALID_HANDLE_VALUE);
        std::lock_guard lock(engine.callbackLock);
        engine.active = false;
        return winrt::to_hresult();
    }
}

void HdInProcessDestroy(void* engine) noexcept {
    delete static_cast<Engine*>(engine);
}
