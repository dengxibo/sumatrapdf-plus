/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/WinUtil.h"

#include "Settings.h"
#include "AppSettings.h"
#include "Commands.h"
#include "DisplayMode.h"
#include "Theme.h"
#include "GlobalPrefs.h"
#include "Translations.h"
#include "Toolbar.h"
#include "DarkModeSubclass.h"

#include "utils/Log.h"

// allow only x64 and arm64 for compatibility for older OS
#if !defined(_DARKMODELIB_NOT_USED) && \
    (defined(__x86_64__) || defined(_M_X64) || defined(__arm64__) || defined(__arm64) || defined(_M_ARM64))
bool gUseDarkModeLib = true;
#else
bool gUseDarkModeLib = false;
#endif

bool UseDarkModeLib() {
    return gUseDarkModeLib;
}

/*
preserve those translations:
_TRN("System")
_TRN("Dark")
_TRN("Darker")
_TRN("Light")
*/

constexpr COLORREF kColBlack = 0x000000;
constexpr COLORREF kColWhite = 0xFFFFFF;
constexpr COLORREF kRedColor = RgbToCOLORREF(0xff0000);
// warm eye-care palette for light mode (paper-like, less glare than pure white)
constexpr COLORREF kColEyeCareWindowBg = RgbToCOLORREF(0xebe6da);
constexpr COLORREF kColEyeCareControlBg = RgbToCOLORREF(0xf5f1e8);
constexpr COLORREF kColEyeCarePageBg = RgbToCOLORREF(0xf7f3e8);
constexpr COLORREF kColEyeCareText = RgbToCOLORREF(0x333333);

static const char* themesTxt = R"(Themes [
    [
        Name = Light
        TextColor = #333333
        BackgroundColor = #ebe6da
        ControlBackgroundColor = #f5f1e8
        LinkColor = #0020a0
        ColorizeControls = false
    ]
    [
        Name = System
        TextColor = #333333
        BackgroundColor = #ebe6da
        ControlBackgroundColor = #f5f1e8
        LinkColor = #0020a0
        ColorizeControls = false
    ]
    [
        Name = Dark
        TextColor = #F9FAFB
        BackgroundColor = #000000
        ControlBackgroundColor = #000000
        LinkColor = #6B7280
        ColorizeControls = true
    ]
]
)";

extern void UpdateAfterThemeChange();

int gFirstSetThemeCmdId;
int gLastSetThemeCmdId;
int gCurrSetThemeCmdId;

static Vec<Theme*>* gThemes = nullptr;
static int gThemeCount;
static int gCurrThemeIndex = 0;
static Theme* gCurrentTheme = nullptr;
static Theme* gThemeLight = nullptr;
static Theme* gThemeDark = nullptr;
static Themes* gParsedThemes = nullptr;
static bool gLastSystemDark = false;

bool IsCurrentThemeDefault() {
    return gCurrThemeIndex == 0;
}

static bool IsSystemTheme() {
    return gCurrThemeIndex == 1;
}

static Theme* GetResolvedTheme() {
    if (IsSystemTheme()) {
        if (gThemeLight && gThemeDark) {
            return DarkMode::isDarkModeReg() ? gThemeDark : gThemeLight;
        }
    }
    return gCurrentTheme;
}

bool ThemeUsesDarkChrome() {
    return gThemeDark && (GetResolvedTheme() == gThemeDark);
}

bool IsDarkThemeSelected() {
    return ThemeUsesDarkChrome();
}

void FreeThemes() {
    delete gThemes; // no need to free members, they are owned by gParsedThemes
    gThemes = nullptr;
    FreeParsedThemes(gParsedThemes);
    gParsedThemes = nullptr;
}

void CreateThemeCommands() {
    FreeThemes();

    gThemes = new Vec<Theme*>();
    gParsedThemes = ParseThemes(themesTxt);
    for (Theme* theme : *gParsedThemes->themes) {
        gThemes->Append(theme);
    }

    gThemeCount = gThemes->Size();
    if (gCurrThemeIndex >= gThemeCount) {
        gCurrThemeIndex = 0;
    }
    gCurrentTheme = gThemes->At(gCurrThemeIndex);
    gThemeLight = gThemes->At(0);
    gThemeDark = gThemes->At(2);

    CustomCommand* cmd;
    for (int i = 0; i < gThemeCount; i++) {
        Theme* theme = gThemes->At(i);
        const char* themeName = theme->name;
        auto args = NewStringArg(kCmdArgTheme, themeName);
        cmd = CreateCustomCommand(themeName, CmdSetTheme, args);
        cmd->name = str::Format(_TRA("Set theme '%s'"), themeName);
        if (i == 0) {
            gFirstSetThemeCmdId = cmd->id;
        } else if (i == gThemeCount - 1) {
            gLastSetThemeCmdId = cmd->id;
        }
    }
    gCurrSetThemeCmdId = gFirstSetThemeCmdId + gCurrThemeIndex;
    gLastSystemDark = DarkMode::isDarkModeReg();
}

void SetThemeByIndex(int themeIdx) {
    ReportIf((themeIdx < 0) || (themeIdx >= gThemeCount));
    if (themeIdx >= gThemeCount) {
        themeIdx = 0;
    }
    bool selectingSystem = (themeIdx == 1);
    bool systemDark = selectingSystem ? DarkMode::isDarkModeReg() : false;
    bool themeChanged = (gCurrThemeIndex != themeIdx) || (selectingSystem && (gLastSystemDark != systemDark));
    gCurrThemeIndex = themeIdx;
    gCurrSetThemeCmdId = gFirstSetThemeCmdId + themeIdx;
    gCurrentTheme = gThemes->At(gCurrThemeIndex);
    gLastSystemDark = systemDark;
    str::ReplaceWithCopy(&gGlobalPrefs->theme, gCurrentTheme->name);
    if (UseDarkModeLib()) {
        if (IsSystemTheme()) {
            DarkMode::setDarkModeConfig();
        } else {
            const UINT mode = static_cast<UINT>(ThemeUsesDarkChrome() ? DarkMode::DarkModeType::dark
                                                                      : DarkMode::DarkModeType::classic);
            DarkMode::setDarkModeConfigEx(mode);
        }
        DarkMode::setDefaultColors(false);

        DarkMode::setBackgroundColor(ThemeWindowBackgroundColor());
        DarkMode::setCtrlBackgroundColor(ThemeWindowControlBackgroundColor());
        COLORREF ctrlBg = ThemeWindowControlBackgroundColor();
        COLORREF hotBg = AccentColor(ctrlBg, 20);
        COLORREF edgeCol = AccentColor(ctrlBg, 40);
        DarkMode::setHotBackgroundColor(hotBg);
        DarkMode::setTextColor(ThemeWindowTextColor());
        DarkMode::setDisabledTextColor(ThemeWindowTextDisabledColor());
        DarkMode::setDlgBackgroundColor(ctrlBg);
        DarkMode::setLinkTextColor(ThemeWindowLinkColor());
        DarkMode::setEdgeColor(edgeCol);
        DarkMode::updateThemeBrushesAndPens();

        DarkMode::setViewTextColor(ThemeWindowTextColor());
        DarkMode::setViewBackgroundColor(ThemeWindowControlBackgroundColor());
        DarkMode::calculateTreeViewStyle();

        if (themeChanged) {
            UpdateAfterThemeChange();
        }

        DarkMode::setPrevTreeViewStyle();
    } else {
        if (themeChanged) {
            UpdateAfterThemeChange();
        }
    }
};

void SelectNextTheme() {
    int newIdx = (gCurrThemeIndex + 1) % 3;
    SetThemeByIndex(newIdx);
}

void ToggleLightDarkTheme() {
    int newIdx;
    if (IsSystemTheme()) {
        newIdx = ThemeUsesDarkChrome() ? 0 : 2;
    } else if (ThemeUsesDarkChrome()) {
        newIdx = 0;
    } else {
        newIdx = 2;
    }
    SetThemeByIndex(newIdx);
}

// not case sensitive
static int GetThemeByName(const char* name) {
    if (!name) {
        return -1;
    }
    if (str::EqI(name, "Light")) {
        return 0;
    }
    if (str::EqI(name, "System")) {
        return 1;
    }
    if (str::EqI(name, "Dark")) {
        return 2;
    }
    if (str::FindI(name, "system")) {
        return 1;
    }
    if (str::FindI(name, "light")) {
        return 0;
    }
    if (str::FindI(name, "dark")) {
        return 2;
    }
    if (str::IsEmpty(name)) {
        return -1;
    }
    return 2;
}

// this is the default aggressive yellow that we suppress
constexpr COLORREF kMainWinBgColDefault = (RGB(0xff, 0xf2, 0) - 0x80000000);

static bool IsDefaultMainWinColor(ParsedColor* col) {
    return col->parsedOk && col->col == kMainWinBgColDefault;
}

void SetTheme(const char* name) {
    int idx = GetThemeByName(name);
    if (idx < 0) {
        // invalid or empty name, reset to light theme
        str::ReplaceWithCopy(&gGlobalPrefs->theme, gThemeLight->name);
        idx = 0;
    }
    SetThemeByIndex(idx);
}

// call after loading settings
void SetCurrentThemeFromSettings() {
    SetTheme(gGlobalPrefs->theme);
    ParsedColor* bgParsed = GetPrefsColor(gGlobalPrefs->mainWindowBackground);
    bool isDefault = IsDefaultMainWinColor(bgParsed);
    if (isDefault) {
        gThemeLight->colorizeControls = false;
        gThemeLight->controlBackgroundColorParsed.col = kColEyeCareControlBg;
    } else {
        gThemeLight->colorizeControls = true;
        gThemeLight->controlBackgroundColorParsed.col = bgParsed->col;
    }
}

COLORREF AccentColor(COLORREF col, int light, int dark) {
    if (dark == 0) {
        dark = light;
    }
    if (IsLightColor(col)) {
        return AdjustLightness2(col, -light);
    }
    return AdjustLightness2(col, dark);
}

#define GetThemeCol(name, def) GetParsedCOLORREF(name, name##Parsed, def)

// canvas/window background color around the document pages
// not affected by FixedPageUI.TextColor/BackgroundColor (those affect page rendering)
COLORREF ThemeDocumentColors(COLORREF& bg) {
    bg = ThemeMainWindowBackgroundColor();

    if (!gGlobalPrefs->fixedPageUI.invertColors) {
        return ThemeWindowTextColor();
    }

    COLORREF text = ThemeWindowTextColor();
    bg = ThemeMainWindowBackgroundColor();

    if (gCurrThemeIndex < 3) {
        bg = AccentColor(bg, 8);
    }
    return text;
}

// colors for page bitmap recoloring (render cache)
// TextColor substitutes black, BackgroundColor substitutes white in rendered pages
COLORREF ThemePageRenderColors(COLORREF& bg) {
    COLORREF text = kColBlack;
    bg = kColWhite;
    bool invertColors = gGlobalPrefs->fixedPageUI.invertColors || ThemeUsesDarkChrome();

    ParsedColor* parsedCol;
    parsedCol = GetPrefsColor(gGlobalPrefs->fixedPageUI.textColor);
    if (parsedCol->parsedOk) {
        text = parsedCol->col;
    }

    parsedCol = GetPrefsColor(gGlobalPrefs->fixedPageUI.backgroundColor);
    if (parsedCol->parsedOk) {
        bg = parsedCol->col;
    }

    if (!invertColors) {
        if (!ThemeUsesDarkChrome() && text == kColBlack && bg == kColWhite) {
            text = kColEyeCareText;
            bg = kColEyeCarePageBg;
        }
        return text;
    }

    // if user did change those colors in advanced settings, respect them
    bool userDidChange = text != kColBlack || bg != kColWhite;
    if (userDidChange) {
        std::swap(text, bg);
        return text;
    }

    // default colors
    if (!ThemeUsesDarkChrome()) {
        std::swap(text, bg);
        return text;
    }

    // if we're inverting in non-default themes, the colors
    // should match the colors of the window
    text = ThemeWindowTextColor();
    bg = ThemeMainWindowBackgroundColor();

    if (gCurrThemeIndex < 3) {
        bg = AccentColor(bg, 8);
    }
    return text;
}

COLORREF ThemeControlBackgroundColor() {
    // note: we can change it in ThemeUpdateAfterLoadSettings()
    Theme* theme = GetResolvedTheme();
    auto col = GetThemeCol(theme->controlBackgroundColor, kRedColor);
    return col;
}

COLORREF ThemeThumbnailBackgroundColor() {
    if (ThemeUsesDarkChrome()) {
        return AccentColor(ThemeControlBackgroundColor(), 18);
    }
    // white card on the gray page background
    return ThemeControlBackgroundColor();
}

COLORREF ThemeThumbnailBorderColor() {
    if (ThemeUsesDarkChrome()) {
        return AccentColor(ThemeThumbnailBackgroundColor(), 35);
    }
    // border slightly darker than the page background
    return AccentColor(ThemeMainWindowBackgroundColor(), -25);
}

COLORREF ThemeMainWindowBackgroundColor() {
    Theme* theme = GetResolvedTheme();
    COLORREF bgColor = GetThemeCol(theme->backgroundColor, kRedColor);
    if (!ThemeUsesDarkChrome()) {
        // Special behavior for light theme.
        ParsedColor* bgParsed = GetPrefsColor(gGlobalPrefs->mainWindowBackground);
        if (!IsDefaultMainWinColor(bgParsed)) {
            bgColor = bgParsed->col;
        }
    }
    return bgColor;
}

COLORREF ThemeWindowBackgroundColor() {
    Theme* theme = GetResolvedTheme();
    auto col = GetThemeCol(theme->backgroundColor, kRedColor);
    return col;
}

COLORREF ThemeWindowTextColor() {
    Theme* theme = GetResolvedTheme();
    auto col = GetThemeCol(theme->textColor, kRedColor);
    return col;
}

COLORREF ThemeWindowTextDisabledColor() {
    // blend text color halfway toward background so disabled text
    // is visible but clearly muted on both light and dark themes
    COLORREF txt = ThemeWindowTextColor();
    COLORREF bg = ThemeMainWindowBackgroundColor();
    u8 r = (u8)((GetRValue(txt) + GetRValue(bg)) / 2);
    u8 g = (u8)((GetGValue(txt) + GetGValue(bg)) / 2);
    u8 b = (u8)((GetBValue(txt) + GetBValue(bg)) / 2);
    return RGB(r, g, b);
}

COLORREF ThemeWindowControlBackgroundColor() {
    Theme* theme = GetResolvedTheme();
    auto col = GetThemeCol(theme->controlBackgroundColor, kRedColor);
    return col;
}

COLORREF ThemeWindowLinkColor() {
    Theme* theme = GetResolvedTheme();
    auto col = GetThemeCol(theme->linkColor, kRedColor);
    return col;
}

COLORREF ThemeNotificationsBackgroundColor() {
    auto col = ThemeWindowBackgroundColor();
    return AdjustLightness2(col, 10);
}

COLORREF ThemeNotificationsTextColor() {
    return ThemeWindowTextColor();
}

COLORREF ThemeNotificationsHighlightColor() {
    if (GetResolvedTheme()->colorizeControls) {
        auto col = ThemeWindowBackgroundColor();
        return AccentColor(col, 20);
    }
    return RgbToCOLORREF(0xFFEE70); // yellowish
}

COLORREF ThemeNotificationsHighlightTextColor() {
    if (GetResolvedTheme()->colorizeControls) {
        auto col = ThemeWindowTextColor();
        return AccentColor(col, 20);
    }
    return RgbToCOLORREF(0x8d0801); // reddish
}

COLORREF ThemeNotificationsProgressColor() {
    return ThemeWindowLinkColor();
}

bool ThemeColorizeControls() {
    if (GetResolvedTheme()->colorizeControls) {
        return true;
    }
    return !IsMenuFontSizeDefault();
}

#if 0
void dumpThemes() {
    logf("Themes [\n");
    for (ThemeOld* theme : gThemes) {
        auto w = *theme;
        logf("    [\n");
        logf("        Name = %s\n", w.name);
        logf("        TextColor = %s\n", SerializeColorTemp(w.textColor));
        logf("        BackgroundColor = %s\n", SerializeColorTemp(w.backgroundColor));
        logf("        ControlBackgroundColor = %s\n", SerializeColorTemp(w.controlBackgroundColor));
        logf("        LinkColor = %s\n", SerializeColorTemp(w.linkColor));
        logf("        ColorizeControls = %s\n", w.colorizeControls ? "true" : "false");
        logf("    ]\n");
    }
    logf("]\n");
}
#endif
