/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

int DpiGetForHwnd(HWND);
int DpiGetForMonitorOfHwnd(HWND hwnd);
int DpiGet(HWND);
void DbgLogDpi(const char* hypothesisId, const char* location, const char* message, HWND hwnd, int frameDpi,
               int explicitDpi, int fontSize, int earlyReturn);
void DbgLogFontMetrics(const char* hypothesisId, const char* location, const char* message, HWND hwnd);
void DbgLogFontHandle(const char* hypothesisId, const char* location, const char* message, HFONT font, int wndDpi);
int DpiScale(HWND, int);
void DpiScale(HWND, int&, int&);

int DpiScale(HDC, int x);
