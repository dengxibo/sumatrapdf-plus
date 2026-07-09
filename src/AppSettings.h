/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

/* enum from windowState */
enum {
    WIN_STATE_NORMAL = 1, /* use remembered position and size */
    WIN_STATE_MAXIMIZED,  /* ignore position and size, maximize the window */
    WIN_STATE_FULLSCREEN,
    WIN_STATE_MINIMIZED,
};

extern bool gDontSaveSettings;
extern HANDLE gInstanceMutex;

extern Vec<SessionData*>* gInitialSessionData;

TempStr GetSettingsPathTemp();
TempStr GetSettingsFileNameTemp();

bool LoadSettings();
bool SaveSettings();
void CleanUpSettings();
void RegisterSettingsForFileChanges();
void UnregisterSettingsForFileChanges();
int GetAppFontSize();
HFONT GetAppFont();
HFONT GetAppFontForHwnd(HWND hwnd);
HFONT GetAppFontForDpi(int dpi);
int GetAppMenuFontSize();
int GetAppMenuFontSizeForHwnd(HWND hwnd);
bool IsAppFontSizeDefault();
HFONT GetAppMenuFont();
HFONT GetAppMenuFontForHwnd(HWND hwnd);
HFONT GetAppSidebarLabelFontForHwnd(HWND hwnd);
HFONT GetAppBiggerFont();
HFONT GetAppBiggerFontForHwnd(HWND hwnd);
HFONT GetAppTreeFont();
HFONT GetAppTreeFontForHwnd(HWND hwnd);
bool IsMenuFontSizeDefault();
void InvalidateUiFonts();
