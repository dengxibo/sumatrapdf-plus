/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#pragma once

#include "utils/BaseUtil.h"

struct WindowTab;

void CreateEbookFontMenuCommands();
void AppendEbookLatinFontsToMenu(HMENU menu);
void AppendEbookCjkFontsToMenu(HMENU menu);
void UpdateEbookFontMenuRadioState(HMENU menu);

bool IsReflowableEbookTabForFontMenu(WindowTab* tab);

void UpdateAfterEbookFontChange();

extern int gFirstEbookLatinFontCmdId;
extern int gLastEbookLatinFontCmdId;
extern int gFirstEbookCjkFontCmdId;
extern int gLastEbookCjkFontCmdId;
