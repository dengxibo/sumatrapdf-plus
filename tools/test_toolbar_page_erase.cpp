/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Regression for #117 using the application's actual static-control subclass.
// Compare native visible painting with buffered painting, and verify text renders.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include "../src/ToolbarPageText.h"

static COLORREF background;
static COLORREF foreground;
static HBRUSH brush;
static DWORD lastPaintDcType;
static LRESULT CALLBACK HostProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_CTLCOLORSTATIC) {
        lastPaintDcType = GetObjectType((HDC)wp);
        SetBkColor((HDC)wp, background);
        SetTextColor((HDC)wp, foreground);
        return (LRESULT)brush;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static int PaintTest() {
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    WNDCLASSW cls{};
    cls.lpfnWndProc = HostProc;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = L"PageEraseRegression";
    RegisterClassW(&cls);
    HWND host = CreateWindowW(cls.lpszClassName, L"", WS_OVERLAPPED | WS_VISIBLE, 0, 0, 200, 100, nullptr, nullptr,
                              cls.hInstance, nullptr);
    HWND field = CreateWindowW(L"Static", L" / 514", WS_CHILD | WS_VISIBLE, 0, 0, 100, 30, host, nullptr, cls.hInstance,
                               nullptr);
    HDC dc = CreateCompatibleDC(nullptr);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = 100;
    info.bmiHeader.biHeight = -30;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    HGDIOBJ old = SelectObject(dc, bitmap);
    COLORREF palettes[] = {RGB(255, 255, 255), RGB(38, 40, 52), RGB(0, 0, 0), RGB(246, 239, 220)};
    bool passed = host && field && bitmap;
    RECT rc{0, 0, 100, 30};
    for (COLORREF color : palettes) {
        background = color;
        foreground = color == palettes[1] || color == palettes[2] ? RGB(240, 240, 240) : RGB(20, 20, 20);
        brush = CreateSolidBrush(background);
        RemoveWindowSubclass(field, ToolbarPageTextSubclassProc, 0);
        lastPaintDcType = 0;
        InvalidateRect(field, nullptr, TRUE);
        SendMessageW(field, WM_PAINT, 0, 0);
        bool nativeDirect = lastPaintDcType == OBJ_DC;
        SetWindowSubclass(field, ToolbarPageTextSubclassProc, 0, 0);
        lastPaintDcType = 0;
        InvalidateRect(field, nullptr, TRUE);
        SendMessageW(field, WM_PAINT, 0, 0);
        bool buffered = lastPaintDcType == OBJ_MEMDC;
        SetPixel(dc, 0, 0, RGB(251, 1, 253));
        SendMessageW(field, WM_ERASEBKGND, (WPARAM)dc, 0);
        bool preserved = GetPixel(dc, 0, 0) == RGB(251, 1, 253);
        // The no-erase rule must not suppress native offscreen text rendering.
        FillRect(dc, &rc, brush);
        SendMessageW(field, WM_PRINTCLIENT, (WPARAM)dc, PRF_CLIENT | PRF_ERASEBKGND);
        bool hasText = false;
        for (int y = 0; y < 30; y++)
            for (int x = 0; x < 100; x++) hasText |= GetPixel(dc, x, y) != background;
        bool correctBackground = GetPixel(dc, 99, 29) == background;
        bool ok = nativeDirect && buffered && preserved && hasText && correctBackground;
        wprintf(L"background=%06lx native-direct=%d buffered=%d preserved=%d text=%d background-ok=%d %s\n", color,
                nativeDirect, buffered, preserved, hasText, correctBackground, ok ? L"PASS" : L"FAIL");
        passed &= ok;
        DeleteObject(brush);
    }
    SelectObject(dc, old);
    DeleteObject(bitmap);
    DeleteDC(dc);
    DestroyWindow(host);
    return passed ? 0 : 1;
}

struct DpiChild {
    DWORD pid;
    HWND frame = nullptr;
    HWND pageEdit = nullptr;
    HWND fields[8]{};
    int count = 0;
    RECT monitors[8]{};
    int monitorCount = 0;
};

static BOOL CALLBACK FindChildFrame(HWND hwnd, LPARAM arg) {
    auto* child = (DpiChild*)arg;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    WCHAR cls[64]{};
    GetClassNameW(hwnd, cls, ARRAYSIZE(cls));
    if (pid == child->pid && wcscmp(cls, L"SUMATRA_PDF_FRAME") == 0) child->frame = hwnd;
    return TRUE;
}

static BOOL CALLBACK FindPageFields(HWND hwnd, LPARAM arg) {
    auto* child = (DpiChild*)arg;
    WCHAR cls[64]{}, parent[64]{};
    GetClassNameW(hwnd, cls, ARRAYSIZE(cls));
    GetClassNameW(GetParent(hwnd), parent, ARRAYSIZE(parent));
    bool toolbar = wcscmp(cls, L"ToolbarWindow32") == 0 && wcscmp(parent, L"ReBarWindow32") == 0;
    bool pageField = wcscmp(parent, L"ToolbarWindow32") == 0 &&
                     (wcscmp(cls, L"Static") == 0 ||
                      (wcscmp(cls, L"Edit") == 0 && (GetWindowLongPtrW(hwnd, GWL_STYLE) & ES_NUMBER)));
    if ((toolbar || pageField) && child->count < 8) child->fields[child->count++] = hwnd;
    if (pageField && wcscmp(cls, L"Edit") == 0) child->pageEdit = hwnd;
    return TRUE;
}

static BOOL CALLBACK FindMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM arg) {
    auto* child = (DpiChild*)arg;
    MONITORINFO info{sizeof(info)};
    if (GetMonitorInfoW(monitor, &info) && child->monitorCount < 8) {
        child->monitors[child->monitorCount++] = info.rcWork;
    }
    return TRUE;
}

static int DpiTest(WCHAR** argv, bool focusPage) {
    // Only move the child process created here. No global input injection,
    // user windows, or persisted user settings are involved.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WCHAR command[8192]{};
    swprintf_s(command, L"\"%s\" -appdata \"%s\" -new-window \"%s\"", argv[1], argv[2], argv[3]);
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_SHOWNOACTIVATE;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(argv[1], command, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) return 3;
    CloseHandle(process.hThread);
    DpiChild child{process.dwProcessId};
    for (int i = 0; i < 100 && !child.frame; i++) {
        EnumWindows(FindChildFrame, (LPARAM)&child);
        if (WaitForSingleObject(process.hProcess, 50) != WAIT_TIMEOUT) break;
    }
    Sleep(1200);
    if (child.frame) EnumChildWindows(child.frame, FindPageFields, (LPARAM)&child);
    EnumDisplayMonitors(nullptr, nullptr, FindMonitor, (LPARAM)&child);
    bool passed = child.frame && child.count == 5 && child.monitorCount > 1;
    if (passed && focusPage) {
        MSG msg{};
        PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE);
        DWORD thread = GetWindowThreadProcessId(child.frame, nullptr);
        SetForegroundWindow(child.frame);
        bool attached = AttachThreadInput(GetCurrentThreadId(), thread, TRUE) != FALSE;
        if (attached) {
            SetFocus(child.pageEdit);
            AttachThreadInput(GetCurrentThreadId(), thread, FALSE);
        }
        GUITHREADINFO gui{sizeof(gui)};
        passed &= attached && GetGUIThreadInfo(thread, &gui) && gui.hwndFocus == child.pageEdit;
        SendMessageW(child.pageEdit, WM_SETTEXT, 0, (LPARAM)L"7");
        SendMessageW(child.pageEdit, EM_SETSEL, 0, 1);
    }
    int changes = 0;
    UINT previousDpi = child.frame ? GetDpiForWindow(child.frame) : 0;
    SendMessageW(child.frame, WM_ENTERSIZEMOVE, 0, 0);
    for (int i = 0; i < 24 && passed; i++) {
        RECT rc = child.monitors[i % child.monitorCount];
        SetWindowPos(child.frame, nullptr, rc.left + 80, rc.top + 80, 900, 640, SWP_NOZORDER | SWP_NOACTIVATE);
        Sleep(50);
        UINT dpi = GetDpiForWindow(child.frame);
        if (dpi != previousDpi) changes++;
        previousDpi = dpi;
        for (int j = 0; j < child.count; j++) {
            HWND field = child.fields[j];
            passed &= IsWindow(field) != FALSE;
            WCHAR parentClass[64]{};
            HWND parent = GetParent(field);
            GetClassNameW(parent, parentClass, ARRAYSIZE(parentClass));
            if (wcscmp(parentClass, L"ToolbarWindow32") != 0) continue;
            RECT bounds{}, fieldRect{};
            GetWindowRect(parent, &bounds);
            GetWindowRect(field, &fieldRect);
            bool fits = fieldRect.right > fieldRect.left && fieldRect.bottom > fieldRect.top &&
                        fieldRect.left >= bounds.left - 2 && fieldRect.right <= bounds.right + 2 &&
                        fieldRect.top >= bounds.top - 2 && fieldRect.bottom <= bounds.bottom + 2;
            if (!fits) wprintf(L"page control outside toolbar at DPI %u\n", dpi);
            passed &= fits;
        }
        DWORD code = 0;
        if (focusPage) {
            WCHAR input[32]{};
            SendMessageW(child.pageEdit, WM_GETTEXT, ARRAYSIZE(input), (LPARAM)input);
            LRESULT selection = SendMessageW(child.pageEdit, EM_GETSEL, 0, 0);
            bool retained = wcscmp(input, L"7") == 0 && LOWORD(selection) == 0 && HIWORD(selection) == 1;
            if (!retained) {
                GUITHREADINFO gui{sizeof(gui)};
                GetGUIThreadInfo(GetWindowThreadProcessId(child.frame, nullptr), &gui);
                wprintf(L"page input/selection changed at DPI %u: [%s] selection=%ld,%ld focused=%d\n", dpi, input,
                        (long)LOWORD(selection), (long)HIWORD(selection), gui.hwndFocus == child.pageEdit);
            }
            passed &= retained;
        }
        GetExitCodeProcess(process.hProcess, &code);
        passed &= code == STILL_ACTIVE;
    }
    SendMessageW(child.frame, WM_EXITSIZEMOVE, 0, 0);
    Sleep(250);
    for (int j = 0; j < child.count; j++) passed &= IsWindow(child.fields[j]) != FALSE;
    if (focusPage) {
        WCHAR input[32]{};
        SendMessageW(child.pageEdit, WM_GETTEXT, ARRAYSIZE(input), (LPARAM)input);
        LRESULT selection = SendMessageW(child.pageEdit, EM_GETSEL, 0, 0);
        passed &= wcscmp(input, L"7") == 0 && LOWORD(selection) == 0 && HIWORD(selection) == 1;
    }
    passed &= changes > 0;
    wprintf(L"monitors=%d actual-DPI-changes=%d retained-controls=%d focus-test=%d result=%s\n", child.monitorCount,
            changes, child.count, focusPage, passed ? L"PASS" : L"FAIL");
    PostMessageW(child.frame, WM_CLOSE, 0, 0);
    if (WaitForSingleObject(process.hProcess, 5000) != WAIT_OBJECT_0) TerminateProcess(process.hProcess, 1);
    CloseHandle(process.hProcess);
    return passed ? 0 : 1;
}

int wmain(int argc, WCHAR** argv) {
    if (argc == 4 || argc == 5) return DpiTest(argv, argc == 5);
    return PaintTest();
}
