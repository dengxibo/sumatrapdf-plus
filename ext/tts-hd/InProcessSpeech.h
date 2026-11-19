#pragma once
#include <windows.h>

struct HdInProcessCallbacks {
    void* context;
    void (*audio)(void*, const BYTE*, size_t);
    void (*word)(void*, ULONGLONG, const char*);
};

// Exceptions and C++/WinRT objects never cross this interface.
void* HdInProcessCreate(const WCHAR* root, const WCHAR* model, const WCHAR* name, const WCHAR* locale,
                        HRESULT* error) noexcept;
HRESULT HdInProcessSpeak(void* engine, const WCHAR* ssml, HANDLE cancel,
                         const HdInProcessCallbacks* callbacks) noexcept;
void HdInProcessDestroy(void* engine) noexcept;
