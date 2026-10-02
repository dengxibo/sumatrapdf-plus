/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

int DpiGetForHwnd(HWND);
int DpiGetForMonitor(HMONITOR monitor);
int DpiGetForMonitorOfHwnd(HWND hwnd);
int DpiGet(HWND);
int DpiScale(HWND, int);
void DpiScale(HWND, int&, int&);

int DpiScale(HDC, int x);

// Move every child by newDpi/oldDpi. Used when a window keeps pixel positions
// from an earlier layout and then crosses monitors.
void DpiResizeChildren(HWND parent, int oldDpi, int newDpi);
