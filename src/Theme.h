/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
License: GPLv3 */

void SetTheme(const char* name);
void SetCurrentThemeFromSettings();
void SelectNextTheme();
void ToggleLightDarkTheme();
void CreateThemeCommands();
void UpdateThemeCommandLabels();

COLORREF ThemeDocumentColors(COLORREF&);
COLORREF ThemePageRenderColors(COLORREF&, bool respectPdfDocColorMode = true);
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
// slightly recessed background for find/search edit fields
COLORREF ThemeFindEditBackgroundColor();
// recessed background for annotation note edit fields
COLORREF ThemeAnnotationContentsEditBackgroundColor();
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
bool ThemeUsesEyeCareChrome();
bool IsCurrentThemeDefault();
void UpdateWindowCaptionTheme(HWND hwnd);
COLORREF AccentColor(COLORREF col, int light, int dark = 0);
void FreeThemes();
bool UseDarkModeLib();

extern int gFirstSetThemeCmdId;
extern int gLastSetThemeCmdId;
extern int gCurrSetThemeCmdId;
