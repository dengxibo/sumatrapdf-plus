/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#pragma once

struct MainWindow;

void HandwrittenSignatureOpen(MainWindow* win);
bool HandwrittenSignatureIsPlacing(MainWindow* win);
// true when the click was used (place, or ignored because place mode is active)
bool HandwrittenSignatureOnMouseDown(MainWindow* win, int x, int y);
bool HandwrittenSignatureOnMouseMove(MainWindow* win, int x, int y);
bool HandwrittenSignatureOnMouseUp(MainWindow* win, int x, int y);
void HandwrittenSignaturePaintPreview(HDC hdc, MainWindow* win);
void HandwrittenSignatureCancelPlace(MainWindow* win);
