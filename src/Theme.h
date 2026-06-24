/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
License: GPLv3 */

void SetTheme(const char* name);
void SetCurrentThemeFromSettings();
void SelectNextTheme();
void ToggleLightDarkTheme();
void CreateThemeCommands();
void UpdateThemeCommandLabels();

COLORREF ThemeDocumentColors(COLORREF&);
COLORREF ThemePageRenderColors(COLORREF&);
void ThemeSidebarColors(COLORREF& bg, COLORREF& text);
COLORREF ThemeSidebarBackgroundColor();
COLORREF ThemeMainWindowBackgroundColor();
COLORREF ThemeControlBackgroundColor();
COLORREF ThemeChromeBackgroundColor();
COLORREF ThemeThumbnailBackgroundColor();
COLORREF ThemeThumbnailBorderColor();
COLORREF ThemeWindowBackgroundColor();
COLORREF ThemeWindowTextColor();
COLORREF ThemeReadingTextColor();
COLORREF ThemeWindowTextDisabledColor();
COLORREF ThemeReadingTextDisabledColor();
COLORREF ThemeWindowControlBackgroundColor();
COLORREF ThemeWindowLinkColor();
COLORREF ThemeNotificationsBackgroundColor();
COLORREF ThemeNotificationsTextColor();
COLORREF ThemeNotificationsHighlightColor();
COLORREF ThemeNotificationsHighlightTextColor();
COLORREF ThemeNotificationsProgressColor();
bool ThemeColorizeControls();
bool ThemeUsesDarkChrome();
bool ThemeUsesBlackChrome();
bool ThemeUsesOriginalPageColors();
bool IsCurrentThemeDefault();
COLORREF AccentColor(COLORREF col, int light, int dark = 0);
void FreeThemes();
bool UseDarkModeLib();

extern int gFirstSetThemeCmdId;
extern int gLastSetThemeCmdId;
extern int gCurrSetThemeCmdId;
