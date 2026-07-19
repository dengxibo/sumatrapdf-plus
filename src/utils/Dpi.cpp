/* Copyright 2024 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#include "utils/BaseUtil.h"
#include "utils/Dpi.h"
#include "utils/WinDynCalls.h"
#include "utils/ScopedWin.h"
#include "utils/WinUtil.h"

/* Info from https://code.msdn.microsoft.com/DPI-Tutorial-sample-64134744

DPI Unaware: virtualized to 96 DPI and scaled by the system for the DPI of the monitor where shown

System DPI Aware:
 These apps render themselves according to the DPI of the display where they
 are launched, and they expect that scaling to remain constant for all displays on the system.
 These apps are scaled up or down when moved to a display with a different DPI from the system DPI.

Per-Monitor DPI Aware:
 These apps render themselves for any DPI, and re-render when the DPI changes
 (as indicated by the WM_DPICHANGED window message).
*/

#include <shellscalingapi.h>

typedef HRESULT(WINAPI* Sig_GetDpiForMonitor)(HMONITOR, MONITOR_DPI_TYPE, UINT*, UINT*);

static Sig_GetDpiForMonitor GetDpiForMonitorFn() {
    static Sig_GetDpiForMonitor fn = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        HMODULE h = LoadLibraryW(L"shcore.dll");
        if (h) {
            fn = (Sig_GetDpiForMonitor)GetProcAddress(h, "GetDpiForMonitor");
        }
    }
    return fn;
}

static int DpiGetForMonitorAtWindowCenter(HWND hwnd) {
    RECT rc{};
    if (!GetWindowRect(hwnd, &rc)) {
        return 0;
    }

    POINT pt = {(rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2};
    HMONITOR h = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    if (!h) {
        return 0;
    }

    return DpiGetForMonitor(h);
}

int DpiGetForMonitor(HMONITOR monitor) {
    auto getDpiForMonitor = GetDpiForMonitorFn();
    if (!getDpiForMonitor || !monitor) {
        return 0;
    }
    UINT dpiX = 96, dpiY = 96;
    HRESULT hr = getDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
    if (hr != S_OK || dpiX == 0) {
        return 0;
    }
    ReportIf(dpiX < 72);
    return (int)dpiX;
}

int DpiGetForMonitorOfHwnd(HWND hwnd) {
    if (!hwnd) {
        return 0;
    }
    HMONITOR h = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (!h) {
        return 0;
    }
    return DpiGetForMonitor(h);
}

// get uncached dpi
int DpiGetForHwnd(HWND hwnd) {
    // GetDpiForWindow() returns defult 96 DPI for desktop window
    // (most likely desktop has DPI_AWARENESS set to UNAWARE)
    if (!hwnd || (hwnd == HWND_DESKTOP) || (hwnd == GetDesktopWindow())) {
        ScopedGetDC dc(hwnd);
        return GetDeviceCaps(dc, LOGPIXELSX);
    }

    int monDpi = DpiGetForMonitorAtWindowCenter(hwnd);

    int wndDpi = 0;
    if (DynGetDpiForWindow) {
        uint dpi = DynGetDpiForWindow(hwnd);
        if (dpi > 0) {
            ReportIf(dpi < 72);
            wndDpi = (int)dpi;
        }
    }

    // After the window is shown, GetDpiForWindow() is authoritative. The monitor
    // under the window-center can lag during cross-monitor moves (see debug logs:
    // wndDpi=120 but monDpi=96 while dragging back to a 120-DPI display).
    if (IsWindowVisible(hwnd) && wndDpi > 0) {
        return wndDpi;
    }

    // Before first show, GetDpiForWindow() can report the primary monitor DPI.
    if (wndDpi > 0 && monDpi > 0 && wndDpi != monDpi) {
        return monDpi;
    }
    if (wndDpi > 0) {
        return wndDpi;
    }
    if (monDpi > 0) {
        return monDpi;
    }

    ScopedGetDC dc(hwnd);
    return GetDeviceCaps(dc, LOGPIXELSX);
}

int DpiGet(HWND hwnd) {
    int dpi = DpiGetForHwnd(hwnd);
    dpi = RoundUp(dpi, 4);
    return dpi;
}

int DpiScale(HWND hwnd, int x) {
    int dpi = DpiGet(hwnd);
    int res = MulDiv(x, dpi, 96);
    return res;
}

void DpiScale(HWND hwnd, int& x1, int& x2) {
    int dpi = DpiGet(hwnd);
    int nx1 = MulDiv(x1, dpi, 96);
    int nx2 = MulDiv(x2, dpi, 96);
    x1 = nx1;
    x2 = nx2;
}

int DpiScale(HDC hdc, int x) {
    int dpi = GetDeviceCaps(hdc, LOGPIXELSX);
    int res = MulDiv(x, dpi, 96);
    return res;
}
