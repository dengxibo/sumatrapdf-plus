/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

void UpdateDeltaPerLine();

inline int ValidWheelScrollLines(int lines) {
    return lines >= 1 && lines <= 100 ? lines : 0;
}

// Preserve subpixel input rather than waiting for a complete scroll line.
inline int WheelScrollPixels(int delta, int lineHeight, int deltaPerLine, int& remainder, int lines = 1) {
    long long scaled = remainder + (long long)delta * lineHeight * lines;
    remainder = (int)(scaled % deltaPerLine);
    return (int)-(scaled / deltaPerLine);
}

LRESULT CALLBACK WndProcCanvas(HWND, UINT, WPARAM, LPARAM);
LRESULT WndProcCanvasAbout(MainWindow*, HWND, UINT, WPARAM, LPARAM);
bool IsDragDistance(int x1, int x2, int y1, int y2);
void CancelDrag(MainWindow*);

extern Kind kNotifAnnotation;

void RegisterCanvasDropTarget(HWND hwndCanvas);
void RevokeCanvasDropTarget(HWND hwndCanvas);

// Timer for mouse wheel smooth scrolling
constexpr UINT_PTR kSmoothScrollTimerID = 6;
// Ctrl+wheel zoom: apply after a short idle so a flick is one jump
constexpr UINT_PTR kWheelZoomTimerID = 10;

void CancelPendingWheelZoom(MainWindow* win);
