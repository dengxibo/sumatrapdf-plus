/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#pragma once

#include "utils/BaseUtil.h"

struct WindowTab;

void CreateEbookFontMenuCommands();
// Collect bundled and installed ebook body-font families. Caller owns the
// returned strings and must free them.
void CollectEbookFontFamilies(Vec<char*>* latinFamilies, Vec<char*>* cjkFamilies);
// Match closed-state height to a normal Options ComboBox (e.g. Default Layout).
void LayoutEbookFontCombo(HWND combo, HWND heightRef);
void InitEbookFontCombo(HWND combo);
void EbookFontComboCommand(HWND combo, int notification);
const char* EbookFontComboSelection(HWND combo);
bool DrawEbookFontComboItem(DRAWITEMSTRUCT* item);
void AppendEbookLatinFontsToMenu(HMENU menu);
void AppendEbookCjkFontsToMenu(HMENU menu);
void UpdateEbookFontMenuRadioState(HMENU menu);

bool IsReflowableEbookTabForFontMenu(WindowTab* tab);
// True when A+/A- font-size commands apply. Word/Office keep author sizes;
// MuPDF user CSS does not override them.
bool SupportsEbookFontSizeChange(WindowTab* tab);

void UpdateAfterEbookFontChange();
void ShowEbookFontPicker(HWND owner, bool cjk);
// Same western/CJK lists as the reading font pickers. Calls onPick with the
// family name; does not change the ebook body font.
using FontFamilyPickedFn = void (*)(const char* family, void* ctx);
void ShowFreeTextFontPicker(HWND owner, const char* currentFamily, FontFamilyPickedFn onPick, void* ctx);
bool HwndBelongsToFontPicker(HWND hwnd);

extern int gFirstEbookLatinFontCmdId;
extern int gLastEbookLatinFontCmdId;
extern int gFirstEbookCjkFontCmdId;
extern int gLastEbookCjkFontCmdId;
