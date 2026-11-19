#pragma once
#include <winrt/Windows.Foundation.h>
#include <windows.h>
#include <roapi.h>
#include <activation.h>
#include <winrt/base.h>
#include <string>
#include <memory>
#include "HdPcmTap.h"

// Experimental ABI recovered from the matching Microsoft public symbols.
// Only locally verified extension versions are accepted; unknown versions
// fail explicitly instead of calling an unverified vtable layout.
class HdSpeechRuntime {
    struct DirectoryScope {
        using Add = void*(WINAPI*)(const WCHAR*);
        using Remove = BOOL(WINAPI*)(void*);
        void* root = nullptr;
        void* sdk = nullptr;
        Remove remove = nullptr;
        void Open(const std::wstring& path) {
            HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
            auto add = reinterpret_cast<Add>(GetProcAddress(kernel, "AddDllDirectory"));
            remove = reinterpret_cast<Remove>(GetProcAddress(kernel, "RemoveDllDirectory"));
            if (!add || !remove) winrt::throw_hresult(E_NOTIMPL);
            root = add(path.c_str());
            sdk = add((path + L"\\SpeechSynthesizer").c_str());
            if (!root || !sdk) winrt::throw_last_error();
        }
        ~DirectoryScope() {
            if (root) remove(root);
            if (sdk) remove(sdk);
        }
    } directories;
    winrt::com_ptr<IInspectable> voice;
    winrt::com_ptr<IInspectable> voice2;
    std::unique_ptr<HdPcmTap> pcm;
    static auto Table(IInspectable* object) { return *reinterpret_cast<void***>(object); }
    static GUID Id(const wchar_t* text) {
        GUID value{};
        winrt::check_hresult(IIDFromString(text, &value));
        return value;
    }

  public:
    HdSpeechRuntime(const std::wstring& root, const std::wstring& model, const std::wstring& name,
                    HdPcmTap::Consumer audio = {}, HdPcmTap::EventConsumer events = {}) {
        const auto file = root + L"\\SpeechSynthesizerExtension.dll";
        DWORD unused = 0;
        DWORD length = GetFileVersionInfoSizeW(file.c_str(), &unused);
        std::unique_ptr<BYTE[]> bytes(new BYTE[length ? length : 1]);
        if (!length || !GetFileVersionInfoW(file.c_str(), 0, length, bytes.get()))
            winrt::throw_hresult(HRESULT_FROM_WIN32(GetLastError()));
        VS_FIXEDFILEINFO* version = nullptr;
        UINT size = 0;
        if (!VerQueryValueW(bytes.get(), L"\\", reinterpret_cast<void**>(&version), &size) ||
            size < sizeof(VS_FIXEDFILEINFO) ||
            (version->dwFileVersionMS != MAKELONG(11000, 2608) && version->dwFileVersionMS != MAKELONG(28000, 2609)) ||
            version->dwFileVersionLS != 0)
            throw winrt::hresult_error(E_NOTIMPL, L"尚未验证此系统语音扩展版本，请更新适配代码。");

        // Never bind the extension to an older SDK already loaded by another component.
        const auto sdkPath = root + L"\\SpeechSynthesizer\\Microsoft.CognitiveServices.Speech.core.dll";
        HMODULE sdk = GetModuleHandleW(L"Microsoft.CognitiveServices.Speech.core.dll");
        if (sdk) {
            WCHAR loaded[MAX_PATH];
            DWORD count = GetModuleFileNameW(sdk, loaded, MAX_PATH);
            if (!count || count >= MAX_PATH || _wcsicmp(loaded, sdkPath.c_str()) != 0)
                throw winrt::hresult_error(E_NOTIMPL, L"已加载不兼容的系统语音引擎，请重新启动阅读器。");
        }
        directories.Open(root);
        HMODULE module = GetModuleHandleW(file.c_str());
        if (!module)
            module = LoadLibraryExW(
                file.c_str(), nullptr,
                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS);
        HRESULT loadHr = module ? S_OK : HRESULT_FROM_WIN32(GetLastError());
        winrt::check_hresult(loadHr);
        if (audio) {
            if (!sdk)
                sdk = LoadLibraryExW(sdkPath.c_str(), nullptr,
                                     LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS |
                                         LOAD_LIBRARY_SEARCH_USER_DIRS);
            if (!sdk) winrt::throw_last_error();
            {
                const wchar_t* plugins[] = {L"Microsoft.CognitiveServices.Speech.extension.onnxruntime.dll",
                                            L"Microsoft.CognitiveServices.Speech.extension.embedded.tts.dll"};
                for (const auto* plugin : plugins) {
                    auto path = root + L"\\SpeechSynthesizer\\" + plugin;
                    if (!GetModuleHandleW(path.c_str()) &&
                        !LoadLibraryExW(path.c_str(), nullptr,
                                        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS |
                                            LOAD_LIBRARY_SEARCH_USER_DIRS))
                        winrt::throw_last_error();
                }
            }
            pcm = std::make_unique<HdPcmTap>(module, sdk, std::move(audio), std::move(events));
        }
        // System runtime modules stay loaded until process exit, including async
        // event cleanup after the last projected interface is released.
        auto getFactory = reinterpret_cast<HRESULT(WINAPI*)(HSTRING, IActivationFactory**)>(
            GetProcAddress(module, "DllGetActivationFactory"));
        if (!getFactory) winrt::throw_hresult(E_NOINTERFACE);
        winrt::hstring className(L"SpeechSynthesizerExtension.SpeechSynthesizerExtensionImpl");
        winrt::com_ptr<IActivationFactory> factory;
        winrt::check_hresult(getFactory(static_cast<HSTRING>(winrt::get_abi(className)), factory.put()));
        winrt::com_ptr<IInspectable> statics;
        winrt::check_hresult(
            factory->QueryInterface(Id(L"{B0EE6BF6-E804-5DD8-A5F1-8FA934DC7B06}"), statics.put_void()));
        winrt::hstring modelPath(model), device;
        HSTRING path = static_cast<HSTRING>(winrt::get_abi(modelPath));
        winrt::com_ptr<IInspectable> instance;
        using Create = HRESULT(WINAPI*)(IInspectable*, UINT32, HSTRING*, HSTRING, IInspectable**);
        winrt::check_hresult(reinterpret_cast<Create>(Table(statics.get())[6])(
            statics.get(), 1, &path, static_cast<HSTRING>(winrt::get_abi(device)), instance.put()));
        winrt::check_hresult(instance->QueryInterface(Id(L"{9BCE424E-5D0E-5A50-B770-F246CF3F5640}"), voice.put_void()));
        winrt::check_hresult(
            instance->QueryInterface(Id(L"{18B03E9F-B9E8-5E71-AE6F-5E652F6914D0}"), voice2.put_void()));
        winrt::hstring voiceName(name);
        using Select = HRESULT(WINAPI*)(IInspectable*, HSTRING);
        winrt::check_hresult(reinterpret_cast<Select>(Table(voice.get())[6])(
            voice.get(), static_cast<HSTRING>(winrt::get_abi(voiceName))));
        if (pcm) pcm->Connect();
    }
    INT32 Speak(const std::wstring& text, bool& needsUpdate, bool ssml = false) {
        if (pcm) pcm->Begin();
        winrt::hstring input(text);
        winrt::com_ptr<IInspectable> result;
        using SpeakMethod = HRESULT(WINAPI*)(IInspectable*, HSTRING, IInspectable**);
        winrt::check_hresult(reinterpret_cast<SpeakMethod>(Table(voice2.get())[ssml ? 6 : 8])(
            voice2.get(), static_cast<HSTRING>(winrt::get_abi(input)), result.put()));
        if (!result) winrt::throw_hresult(E_FAIL);
        winrt::com_ptr<IInspectable> typed;
        winrt::check_hresult(result->QueryInterface(Id(L"{76BD7B79-5AF5-57E0-9312-24ACA89C534C}"), typed.put_void()));
        INT32 status = -1;
        BYTE update = 0;
        using GetStatus = HRESULT(WINAPI*)(IInspectable*, INT32*);
        using GetUpdate = HRESULT(WINAPI*)(IInspectable*, BYTE*);
        winrt::check_hresult(reinterpret_cast<GetStatus>(Table(typed.get())[6])(typed.get(), &status));
        winrt::check_hresult(reinterpret_cast<GetUpdate>(Table(typed.get())[7])(typed.get(), &update));
        needsUpdate = update != 0;
        // The result can become ready before the SDK event dispatcher has
        // drained word/viseme callbacks. Do not reuse the request context early.
        if (pcm && (status == 0 || status == 1)) pcm->WaitForEvents();
        if (pcm && pcm->Failed()) winrt::throw_hresult(E_FAIL);
        return status;
    }
    void Stop() {
        winrt::com_ptr<IInspectable> action;
        using StopMethod = HRESULT(WINAPI*)(IInspectable*, IInspectable**);
        winrt::check_hresult(reinterpret_cast<StopMethod>(Table(voice.get())[11])(voice.get(), action.put()));
        if (action) action.as<winrt::Windows::Foundation::IAsyncAction>().get();
    }
};
