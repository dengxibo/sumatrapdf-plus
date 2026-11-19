#pragma once
#include <windows.h>
#include <delayimp.h>
#include <functional>
#include <vector>
#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <string>
#include "HdSpeechEvents.h"

// Instruments the extension's import data while its one synthesizer is constructed.
// The original import is restored immediately.
// No system file, other process, model key, or license is changed or extracted.
class HdPcmTap {
  public:
    using Consumer = std::function<void(const BYTE*, size_t)>;
    using EventConsumer = std::function<void(HdSpeech::Type, const HdSpeech::Event&, const std::string&)>;

  private:
    using Code = uintptr_t;
    using Make = Code(WINAPI*)(void**, void*, void*);
    using SetFormat = Code(WINAPI*)(void*, int);
    using Callback = void(__cdecl*)(void*, void*, void*);
    using SetCallback = Code(WINAPI*)(void*, Callback, void*);
    using GetResult = Code(WINAPI*)(void*, void**);
    using GetLength = Code(WINAPI*)(void*, uint32_t*, uint64_t*);
    using GetData = Code(WINAPI*)(void*, BYTE*, uint32_t, uint32_t*);
    using Release = Code(WINAPI*)(void*);
    using GetBag = Code(WINAPI*)(void*, void**);
    using SetProperty = Code(WINAPI*)(void*, int, const char*, const char*);
    using BoundaryValues = Code(WINAPI*)(void*, uint64_t*, uint64_t*, uint32_t*, uint32_t*, int*);
    using VisemeValues = Code(WINAPI*)(void*, uint64_t*, uint32_t*);
    using BookmarkValues = Code(WINAPI*)(void*, uint64_t*);
    using EventText = const char*(WINAPI*)(void*);
    inline static HdPcmTap* constructing = nullptr;
    Make original = nullptr;
    SetFormat setFormat = nullptr;
    SetCallback setCallback = nullptr;
    GetResult getResult = nullptr;
    GetLength getLength = nullptr;
    GetData getData = nullptr;
    Release releaseResult = nullptr, releaseEvent = nullptr;
    GetBag getBag = nullptr;
    SetProperty setProperty = nullptr;
    Release releaseBag = nullptr;
    SetCallback setWord = nullptr, setViseme = nullptr, setBookmark = nullptr;
    SetCallback setCompleted = nullptr, setCanceled = nullptr;
    HANDLE eventsDone = nullptr;
    BoundaryValues boundaryValues = nullptr;
    VisemeValues visemeValues = nullptr;
    BookmarkValues bookmarkValues = nullptr;
    EventText eventText = nullptr;
    void** importSlot = nullptr;
    void* savedImport = nullptr;
    void* synth = nullptr;
    Consumer consume;
    EventConsumer eventConsumer;
    std::atomic<bool> failed{false};
    bool connected = false;
    inline static SRWLOCK lifetimeLock = SRWLOCK_INIT;
    inline static HdPcmTap* liveTaps = nullptr;
    inline static Release retiredEventRelease = nullptr;
    inline static uintptr_t nextContext = 0;
    uintptr_t contextId = 0;
    HdPcmTap* nextLive = nullptr;

    // SDK event queues may already hold a callback when a voice is disconnected.
    // Validate the context without dereferencing it and drain active callbacks
    // before destruction, including callbacks arriving after unregistration.
    struct CallbackScope {
        bool live = false;
        HdPcmTap* owner = nullptr;
        CallbackScope(void* context, void* event) {
            AcquireSRWLockShared(&lifetimeLock);
            for (auto* tap = liveTaps; tap; tap = tap->nextLive) {
                if (reinterpret_cast<void*>(tap->contextId) == context) {
                    live = true;
                    owner = tap;
                    return;
                }
            }
            if (retiredEventRelease) retiredEventRelease(event);
            ReleaseSRWLockShared(&lifetimeLock);
        }
        ~CallbackScope() {
            if (live) ReleaseSRWLockShared(&lifetimeLock);
        }
    };

    template <class T>
    static T Export(HMODULE module, const char* name) {
        auto value = GetProcAddress(module, name);
        if (!value) throw std::runtime_error("Required speech export missing");
        return reinterpret_cast<T>(value);
    }
    static void Exchange(void** slot, void* value) {
        DWORD protection = 0;
        if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &protection))
            throw std::runtime_error("Import data is not writable");
        InterlockedExchangePointer(slot, value);
        DWORD ignored = 0;
        VirtualProtect(slot, sizeof(void*), protection, &ignored);
    }
    static Code WINAPI Create(void** result, void* config, void*) noexcept {
        HdPcmTap* tap = constructing;
        if (!tap) return static_cast<Code>(E_UNEXPECTED);
        Code code = tap->setFormat(config, 16); // Raw24Khz16BitMonoPcm.
        if (code) return code;
        if (tap->eventConsumer) {
            void* bag = nullptr;
            code = tap->getBag(config, &bag);
            if (code) return code;
            code = tap->setProperty(bag, 4200, nullptr, "true");
            if (!code) code = tap->setProperty(bag, 4201, nullptr, "false");
            if (!code) code = tap->setProperty(bag, 4202, nullptr, "true");
            tap->releaseBag(bag);
            if (code) return code;
        }
        // Public SDK nullptr-output overload uses SPXHANDLE_INVALID, not zero.
        code = tap->original(result, config, reinterpret_cast<void*>(~uintptr_t(0)));
        if (!code) tap->synth = *result;
        return code;
    }
    static void __cdecl Audio(void*, void* event, void* context) noexcept {
        CallbackScope scope(context, event);
        if (!scope.live) return;
        auto& tap = *scope.owner;
        void* result = nullptr;
        try {
            uint32_t length = 0, filled = 0;
            uint64_t duration = 0;
            if (tap.getResult(event, &result) || tap.getLength(result, &length, &duration) || length > 4 * 1024 * 1024)
                throw std::runtime_error("Invalid audio event");
            if (length) {
                std::vector<BYTE> bytes(length);
                if (tap.getData(result, bytes.data(), length, &filled) || filled > length || (filled % 2))
                    throw std::runtime_error("Invalid PCM data");
                if (filled && !tap.failed.load()) tap.consume(bytes.data(), filled);
            }
        } catch (...) {
            tap.failed.store(true);
        }
        if (result) tap.releaseResult(result);
        tap.releaseEvent(event);
    }
    template <HdSpeech::Type type>
    static void __cdecl Timing(void*, void* event, void* context) noexcept {
        CallbackScope scope(context, event);
        if (!scope.live) return;
        auto& tap = *scope.owner;
        try {
            HdSpeech::Event value;
            HdSpeech::Type kind = type;
            Code result = 0;
            if constexpr (type == HdSpeech::Type::Word) {
                int boundary = 0;
                result = tap.boundaryValues(event, &value.ticks, &value.duration, &value.textOffset, &value.textLength,
                                            &boundary);
                kind = boundary == 2 ? HdSpeech::Type::Sentence : HdSpeech::Type::Word;
                value.value = static_cast<uint32_t>(boundary);
                if (boundary == 1) {
                    tap.releaseEvent(event);
                    return;
                }
            } else if constexpr (type == HdSpeech::Type::Viseme)
                result = tap.visemeValues(event, &value.ticks, &value.value);
            else
                result = tap.bookmarkValues(event, &value.ticks);
            if (result) throw std::runtime_error("Timing event extraction failed");
            const char* text = type == HdSpeech::Type::Viseme ? nullptr : tap.eventText(event);
            if (!tap.failed.load()) tap.eventConsumer(kind, value, text ? text : "");
        } catch (...) {
            tap.failed.store(true);
        }
        tap.releaseEvent(event);
    }
    static void __cdecl Ended(void*, void* event, void* context) noexcept {
        CallbackScope scope(context, event);
        if (!scope.live) return;
        auto& tap = *scope.owner;
        tap.releaseEvent(event);
        SetEvent(tap.eventsDone);
    }

  public:
    HdPcmTap(HMODULE extension, HMODULE sdk, Consumer consumer, EventConsumer events = {})
        : consume(std::move(consumer)), eventConsumer(std::move(events)) {
        original = Export<Make>(sdk, "synthesizer_create_speech_synthesizer_from_config");
        setFormat = Export<SetFormat>(sdk, "speech_config_set_audio_output_format");
        setCallback = Export<SetCallback>(sdk, "synthesizer_synthesizing_set_callback");
        getResult = Export<GetResult>(sdk, "synthesizer_synthesis_event_get_result");
        getLength = Export<GetLength>(sdk, "synth_result_get_audio_length_duration");
        getData = Export<GetData>(sdk, "synth_result_get_audio_data");
        releaseResult = Export<Release>(sdk, "synthesizer_result_handle_release");
        releaseEvent = Export<Release>(sdk, "synthesizer_event_handle_release");
        getBag = Export<GetBag>(sdk, "speech_config_get_property_bag");
        setProperty = Export<SetProperty>(sdk, "property_bag_set_string");
        releaseBag = Export<Release>(sdk, "property_bag_release");
        setWord = Export<SetCallback>(sdk, "synthesizer_word_boundary_set_callback");
        setViseme = Export<SetCallback>(sdk, "synthesizer_viseme_received_set_callback");
        setBookmark = Export<SetCallback>(sdk, "synthesizer_bookmark_reached_set_callback");
        setCompleted = Export<SetCallback>(sdk, "synthesizer_completed_set_callback");
        setCanceled = Export<SetCallback>(sdk, "synthesizer_canceled_set_callback");
        boundaryValues = Export<BoundaryValues>(sdk, "synthesizer_word_boundary_event_get_values");
        visemeValues = Export<VisemeValues>(sdk, "synthesizer_viseme_event_get_values");
        bookmarkValues = Export<BookmarkValues>(sdk, "synthesizer_bookmark_event_get_values");
        eventText = Export<EventText>(sdk, "synthesizer_event_get_text");
        auto base = reinterpret_cast<BYTE*>(extension);
        auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        auto directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
        if (!directory.VirtualAddress) throw std::runtime_error("Speech delay imports missing");
        auto descriptors = reinterpret_cast<ImgDelayDescr*>(base + directory.VirtualAddress);
        for (auto descriptor = descriptors; descriptor->rvaDLLName; ++descriptor) {
            if (!(descriptor->grAttrs & dlattrRva)) throw std::runtime_error("Unsupported import format");
            auto names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->rvaINT);
            auto addresses = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->rvaIAT);
            for (size_t index = 0; names[index].u1.AddressOfData; ++index) {
                if (IMAGE_SNAP_BY_ORDINAL(names[index].u1.Ordinal)) continue;
                auto name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names[index].u1.AddressOfData);
                if (strcmp(reinterpret_cast<char*>(name->Name), "synthesizer_create_speech_synthesizer_from_config") ==
                    0)
                    importSlot = reinterpret_cast<void**>(&addresses[index].u1.Function);
            }
        }
        if (!importSlot || constructing) throw std::runtime_error("Speech output interception unavailable");
        eventsDone = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!eventsDone) throw std::runtime_error("Speech event barrier creation failed");
        savedImport = *importSlot;
        constructing = this;
        try {
            Exchange(importSlot, reinterpret_cast<void*>(&Create));
        } catch (...) {
            constructing = nullptr;
            CloseHandle(eventsDone);
            eventsDone = nullptr;
            throw;
        }
        AcquireSRWLockExclusive(&lifetimeLock);
        retiredEventRelease = releaseEvent;
        contextId = ++nextContext;
        nextLive = liveTaps;
        liveTaps = this;
        ReleaseSRWLockExclusive(&lifetimeLock);
    }
    void Connect() {
        Restore();
        void* context = reinterpret_cast<void*>(contextId);
        if (!synth || setCallback(synth, &Audio, context)) throw std::runtime_error("PCM callback connection failed");
        connected = true;
        if (eventConsumer && (setWord(synth, &Timing<HdSpeech::Type::Word>, context) ||
                              setViseme(synth, &Timing<HdSpeech::Type::Viseme>, context) ||
                              setBookmark(synth, &Timing<HdSpeech::Type::Bookmark>, context)))
            throw std::runtime_error("Timing callback connection failed");
        if (setCompleted(synth, &Ended, context) || setCanceled(synth, &Ended, context))
            throw std::runtime_error("Speech completion callback connection failed");
    }
    void Restore() noexcept {
        if (constructing == this) {
            try {
                Exchange(importSlot, savedImport);
            } catch (...) {
                std::terminate();
            }
            constructing = nullptr;
        }
    }
    bool Failed() const { return failed.load(); }
    void Begin() { ResetEvent(eventsDone); }
    void WaitForEvents() {
        if (WaitForSingleObject(eventsDone, 30000) != WAIT_OBJECT_0)
            throw std::runtime_error("Speech completion event timed out");
    }
    ~HdPcmTap() {
        Restore();
        AcquireSRWLockExclusive(&lifetimeLock);
        for (auto** current = &liveTaps; *current; current = &(*current)->nextLive) {
            if (*current == this) {
                *current = nextLive;
                break;
            }
        }
        ReleaseSRWLockExclusive(&lifetimeLock);
        if (connected) {
            setCallback(synth, nullptr, nullptr);
            if (eventConsumer) {
                setWord(synth, nullptr, nullptr);
                setViseme(synth, nullptr, nullptr);
                setBookmark(synth, nullptr, nullptr);
            }
            setCompleted(synth, nullptr, nullptr);
            setCanceled(synth, nullptr, nullptr);
        }
        if (eventsDone) CloseHandle(eventsDone);
    }
};
