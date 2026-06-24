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
#include "PdfDarkMode.h"
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
_TRN("Set theme 'Light-Warm'")
_TRN("Set theme 'Light-White'")
_TRN("Set theme 'System'")
_TRN("Set theme 'Dark-Dracula'")
_TRN("Set theme 'Dark-Black'")
*/

static const char* SetThemeMenuLabel(int themeIdx) {
    switch (themeIdx) {
        case 0:
            return _TRA("Set theme 'Light-Warm'");
        case 1:
            return _TRA("Set theme 'Light-White'");
        case 2:
            return _TRA("Set theme 'System'");
        case 3:
            return _TRA("Set theme 'Dark-Dracula'");
        case 4:
            return _TRA("Set theme 'Dark-Black'");
        default:
            return _TRA("Set theme 'Light-Warm'");
    }
}

constexpr const char* kThemeLightWarm = "Light-Warm";
constexpr const char* kThemeLightWhite = "Light-White";
constexpr const char* kThemeDarkDracula = "Dark-Dracula";
constexpr const char* kThemeDarkBlack = "Dark-Black";
static constexpr int kThemeIdxLightWarm = 0;
static constexpr int kThemeIdxLightWhite = 1;
static constexpr int kThemeIdxSystem = 2;
static constexpr int kThemeIdxDarkDracula = 3;
static constexpr int kThemeIdxDarkBlack = 4;

constexpr COLORREF kColBlack = 0x000000;
constexpr COLORREF kColWhite = 0xFFFFFF;
constexpr COLORREF kRedColor = RgbToCOLORREF(0xff0000);
// warm eye-care palette for light mode (paper-like, less glare than pure white)
constexpr COLORREF kColEyeCareWindowBg = RgbToCOLORREF(0xebe6da);
constexpr COLORREF kColEyeCareControlBg = RgbToCOLORREF(0xf5f1e8);
constexpr COLORREF kColEyeCarePageBg = RgbToCOLORREF(0xf7f3e8);
constexpr COLORREF kColEyeCareText = RgbToCOLORREF(0x333333);
constexpr COLORREF kColReadingText = RgbToCOLORREF(0xe6e1d8);

static const char* themesTxt = R"(Themes [
    [
        Name = Light-Warm
        TextColor = #333333
        BackgroundColor = #ebe6da
        ControlBackgroundColor = #f5f1e8
        LinkColor = #0020a0
        ColorizeControls = false
    ]
    [
        Name = Light-White
        TextColor = #000000
        BackgroundColor = #f5f5f5
        ControlBackgroundColor = #ffffff
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
        Name = Dark-Dracula
        TextColor = #F8F8F2
        BackgroundColor = #282A36
        ControlBackgroundColor = #21222C
        LinkColor = #BD93F9
        ColorizeControls = true
    ]
    [
        Name = Dark-Black
        TextColor = #EDEDED
        BackgroundColor = #000000
        ControlBackgroundColor = #050505
        LinkColor = #7AA2F7
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
static Theme* gThemeLightWhite = nullptr;
static Theme* gThemeDark = nullptr;
static Theme* gThemeBlack = nullptr;
static Themes* gParsedThemes = nullptr;
static bool gLastSystemDark = false;

bool IsCurrentThemeDefault() {
    return gCurrThemeIndex == 0;
}

static bool IsSystemTheme() {
    return gCurrThemeIndex == kThemeIdxSystem;
}

static int DarkThemeIndexFromPrefs() {
    if (gGlobalPrefs &&
        (str::EqI(gGlobalPrefs->lastDarkTheme, "Black") || str::EqI(gGlobalPrefs->lastDarkTheme, "dark-black") ||
         str::EqI(gGlobalPrefs->lastDarkTheme, kThemeDarkBlack))) {
        return kThemeIdxDarkBlack;
    }
    return kThemeIdxDarkDracula;
}

static void UpdateLastDarkThemePref(int themeIdx) {
    if (!gGlobalPrefs || (themeIdx != kThemeIdxDarkDracula && themeIdx != kThemeIdxDarkBlack)) {
        return;
    }
    const char* name = themeIdx == kThemeIdxDarkBlack ? kThemeDarkBlack : kThemeDarkDracula;
    if (!str::EqI(gGlobalPrefs->lastDarkTheme, name)) {
        str::ReplaceWithCopy(&gGlobalPrefs->lastDarkTheme, name);
    }
}

static void MigrateThemePrefName(char** pref) {
    if (!pref || !*pref) {
        return;
    }
    if (str::EqI(*pref, "Light")) {
        str::ReplaceWithCopy(pref, kThemeLightWarm);
    } else if (str::EqI(*pref, "Original")) {
        str::ReplaceWithCopy(pref, kThemeLightWhite);
    } else if (str::EqI(*pref, "Dark") || str::EqI(*pref, "dark-dracula")) {
        str::ReplaceWithCopy(pref, kThemeDarkDracula);
    } else if (str::EqI(*pref, "Black") || str::EqI(*pref, "dark-black")) {
        str::ReplaceWithCopy(pref, kThemeDarkBlack);
    }
}

static int GetPreferredDarkThemeIndex() {
    return DarkThemeIndexFromPrefs();
}

static int LightThemeIndexFromPrefs() {
    if (gGlobalPrefs &&
        (str::EqI(gGlobalPrefs->lastLightTheme, kThemeLightWhite) ||
         str::EqI(gGlobalPrefs->lastLightTheme, "Original") || str::EqI(gGlobalPrefs->lastLightTheme, "Light-White"))) {
        return kThemeIdxLightWhite;
    }
    return kThemeIdxLightWarm;
}

static void UpdateLastLightThemePref(int themeIdx) {
    if (!gGlobalPrefs || (themeIdx != kThemeIdxLightWarm && themeIdx != kThemeIdxLightWhite)) {
        return;
    }
    const char* name = themeIdx == kThemeIdxLightWhite ? kThemeLightWhite : kThemeLightWarm;
    if (!str::EqI(gGlobalPrefs->lastLightTheme, name)) {
        str::ReplaceWithCopy(&gGlobalPrefs->lastLightTheme, name);
    }
}

static int GetPreferredLightThemeIndex() {
    return LightThemeIndexFromPrefs();
}

static Theme* GetPreferredDarkTheme() {
    if (gThemes && gThemeCount > kThemeIdxDarkBlack) {
        return gThemes->At(GetPreferredDarkThemeIndex());
    }
    return gThemeDark;
}

static int GetResolvedThemeIndex() {
    if (IsSystemTheme()) {
        return DarkMode::isDarkModeReg() ? GetPreferredDarkThemeIndex() : kThemeIdxLightWarm;
    }
    return gCurrThemeIndex;
}

static Theme* GetResolvedTheme() {
    if (IsSystemTheme()) {
        if (gThemeLight && gThemeDark) {
            return DarkMode::isDarkModeReg() ? GetPreferredDarkTheme() : gThemeLight;
        }
    }
    return gCurrentTheme;
}

bool ThemeUsesDarkChrome() {
    Theme* theme = GetResolvedTheme();
    return theme && (theme == gThemeDark || theme == gThemeBlack);
}

bool ThemeUsesBlackChrome() {
    Theme* theme = GetResolvedTheme();
    return theme && theme == gThemeBlack;
}

bool ThemeUsesOriginalPageColors() {
    Theme* theme = GetResolvedTheme();
    return gThemeLightWhite && theme == gThemeLightWhite;
}

bool ThemeUsesEyeCareChrome() {
    return !ThemeUsesDarkChrome() && !ThemeUsesOriginalPageColors();
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
    gThemeLight = gThemes->At(kThemeIdxLightWarm);
    gThemeLightWhite = gThemes->At(kThemeIdxLightWhite);
    gThemeDark = gThemes->At(kThemeIdxDarkDracula);
    gThemeBlack = gThemes->At(kThemeIdxDarkBlack);

    CustomCommand* cmd;
    for (int i = 0; i < gThemeCount; i++) {
        Theme* theme = gThemes->At(i);
        const char* themeName = theme->name;
        auto args = NewStringArg(kCmdArgTheme, themeName);
        cmd = CreateCustomCommand(themeName, CmdSetTheme, args);
        cmd->name = str::Dup(SetThemeMenuLabel(i));
        if (i == 0) {
            gFirstSetThemeCmdId = cmd->id;
        } else if (i == gThemeCount - 1) {
            gLastSetThemeCmdId = cmd->id;
        }
    }
    gCurrSetThemeCmdId = gFirstSetThemeCmdId + gCurrThemeIndex;
    gLastSystemDark = DarkMode::isDarkModeReg();
}

void UpdateThemeCommandLabels() {
    if (gFirstSetThemeCmdId <= 0 || gLastSetThemeCmdId < gFirstSetThemeCmdId) {
        return;
    }
    for (int cmdId = gFirstSetThemeCmdId; cmdId <= gLastSetThemeCmdId; cmdId++) {
        CustomCommand* cmd = FindCustomCommand(cmdId);
        if (!cmd) {
            continue;
        }
        int themeIdx = cmdId - gFirstSetThemeCmdId;
        str::Free(cmd->name);
        cmd->name = str::Dup(SetThemeMenuLabel(themeIdx));
    }
}

void SetThemeByIndex(int themeIdx) {
    ReportIf((themeIdx < 0) || (themeIdx >= gThemeCount));
    if (themeIdx >= gThemeCount) {
        themeIdx = 0;
    }
    int prevThemeIdx = gCurrThemeIndex;
    int prevResolvedThemeIdx = GetResolvedThemeIndex();
    bool selectingSystem = (themeIdx == kThemeIdxSystem);
    bool systemDark = selectingSystem ? DarkMode::isDarkModeReg() : false;
    gCurrThemeIndex = themeIdx;
    gCurrSetThemeCmdId = gFirstSetThemeCmdId + themeIdx;
    gCurrentTheme = gThemes->At(gCurrThemeIndex);
    UpdateLastDarkThemePref(themeIdx);
    UpdateLastLightThemePref(themeIdx);
    gLastSystemDark = systemDark;
    int resolvedThemeIdx = GetResolvedThemeIndex();
    bool themeChanged = (prevResolvedThemeIdx != resolvedThemeIdx) || (prevThemeIdx != themeIdx);
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
        COLORREF chromeBg = ThemeChromeBackgroundColor();
        DarkMode::setCtrlBackgroundColor(chromeBg);
        COLORREF ctrlBg = chromeBg;
        COLORREF hotBg = AccentColor(ctrlBg, 20);
        COLORREF edgeCol = ThemeUsesDarkChrome() ? AccentColor(ThemeWindowLinkColor(), -20) : AccentColor(ctrlBg, 40);
        DarkMode::setHotBackgroundColor(hotBg);
        DarkMode::setTextColor(ThemeWindowTextColor());
        DarkMode::setDisabledTextColor(ThemeWindowTextDisabledColor());
        DarkMode::setDlgBackgroundColor(ctrlBg);
        DarkMode::setLinkTextColor(ThemeWindowLinkColor());
        DarkMode::setEdgeColor(edgeCol);
        DarkMode::updateThemeBrushesAndPens();

        DarkMode::setViewTextColor(ThemeUsesDarkChrome() ? ThemeReadingTextColor() : ThemeWindowTextColor());
        if (ThemeUsesDarkChrome()) {
            COLORREF viewBg;
            ThemePageRenderColors(viewBg);
            DarkMode::setViewBackgroundColor(viewBg);
        } else {
            DarkMode::setViewBackgroundColor(ThemeWindowControlBackgroundColor());
        }
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
    int newIdx = (gCurrThemeIndex + 1) % gThemeCount;
    SetThemeByIndex(newIdx);
}

void ToggleLightDarkTheme() {
    int newIdx;
    if (IsSystemTheme()) {
        newIdx = ThemeUsesDarkChrome() ? GetPreferredLightThemeIndex() : GetPreferredDarkThemeIndex();
    } else if (ThemeUsesDarkChrome()) {
        newIdx = GetPreferredLightThemeIndex();
    } else {
        UpdateLastLightThemePref(gCurrThemeIndex);
        newIdx = GetPreferredDarkThemeIndex();
    }
    SetThemeByIndex(newIdx);
}

// not case sensitive
static int GetThemeByName(const char* name) {
    if (!name) {
        return -1;
    }
    if (str::EqI(name, kThemeLightWarm) || str::EqI(name, "Light")) {
        return kThemeIdxLightWarm;
    }
    if (str::EqI(name, kThemeLightWhite) || str::EqI(name, "Original")) {
        return kThemeIdxLightWhite;
    }
    if (str::EqI(name, "System")) {
        return kThemeIdxSystem;
    }
    if (str::EqI(name, "Dark") || str::EqI(name, "dark-dracula") || str::EqI(name, kThemeDarkDracula)) {
        return kThemeIdxDarkDracula;
    }
    if (str::EqI(name, "Black") || str::EqI(name, "dark-black") || str::EqI(name, kThemeDarkBlack)) {
        return kThemeIdxDarkBlack;
    }
    if (str::FindI(name, "system")) {
        return kThemeIdxSystem;
    }
    if (str::FindI(name, "light-white") || str::FindI(name, "white")) {
        return kThemeIdxLightWhite;
    }
    if (str::FindI(name, "light-warm") || str::FindI(name, "warm")) {
        return kThemeIdxLightWarm;
    }
    if (str::FindI(name, "dark-black") || str::FindI(name, "black") || str::FindI(name, "oled")) {
        return kThemeIdxDarkBlack;
    }
    if (str::FindI(name, "dracula") || str::FindI(name, "dark-dracula")) {
        return kThemeIdxDarkDracula;
    }
    if (str::FindI(name, "dark")) {
        return kThemeIdxDarkDracula;
    }
    if (str::FindI(name, "light")) {
        return kThemeIdxLightWarm;
    }
    if (str::IsEmpty(name)) {
        return -1;
    }
    return kThemeIdxDarkDracula;
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
    MigrateThemePrefName(&gGlobalPrefs->theme);
    MigrateThemePrefName(&gGlobalPrefs->lastDarkTheme);
    MigrateThemePrefName(&gGlobalPrefs->lastLightTheme);
    SetTheme(gGlobalPrefs->theme);
    if (gCurrThemeIndex == kThemeIdxDarkDracula || gCurrThemeIndex == kThemeIdxDarkBlack) {
        UpdateLastDarkThemePref(gCurrThemeIndex);
    }
    if (gCurrThemeIndex == kThemeIdxLightWarm || gCurrThemeIndex == kThemeIdxLightWhite) {
        UpdateLastLightThemePref(gCurrThemeIndex);
    }
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
    if (ThemeUsesDarkChrome()) {
        if (GetPdfDocumentColorMode() == PdfDocumentColorMode::Light) {
            bg = ThemeMainWindowBackgroundColor();
            if (GetResolvedThemeIndex() != kThemeIdxDarkBlack) {
                bg = AccentColor(bg, 8);
            }
            return ThemeReadingTextColor();
        }
        return ThemePageRenderColors(bg);
    }

    bg = ThemeMainWindowBackgroundColor();

    if (!gGlobalPrefs->fixedPageUI.invertColors) {
        return ThemeWindowTextColor();
    }

    COLORREF text = ThemeWindowTextColor();
    bg = ThemeMainWindowBackgroundColor();

    if (GetResolvedThemeIndex() != kThemeIdxDarkBlack) {
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
    if (ThemeUsesDarkChrome() && GetPdfDocumentColorMode() == PdfDocumentColorMode::Light) {
        invertColors = false;
    }

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
        if (!ThemeUsesDarkChrome() && !ThemeUsesOriginalPageColors() && text == kColBlack && bg == kColWhite) {
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
    text = ThemeReadingTextColor();
    bg = ThemeMainWindowBackgroundColor();

    if (GetResolvedThemeIndex() != kThemeIdxDarkBlack) {
        bg = AccentColor(bg, 8);
    }
    return text;
}

void ThemeSidebarColors(COLORREF& bg, COLORREF& text) {
    if (ThemeUsesDarkChrome()) {
        bg = ThemeMainWindowBackgroundColor();
        if (GetResolvedThemeIndex() != kThemeIdxDarkBlack) {
            bg = AccentColor(bg, 8);
        }
        text = ThemeReadingTextColor();
        return;
    }
    bg = ThemeControlBackgroundColor();
    text = ThemeWindowTextColor();
}

COLORREF ThemeSidebarBackgroundColor() {
    COLORREF bg, text;
    ThemeSidebarColors(bg, text);
    return bg;
}

COLORREF ThemeControlBackgroundColor() {
    // note: we can change it in ThemeUpdateAfterLoadSettings()
    Theme* theme = GetResolvedTheme();
    auto col = GetThemeCol(theme->controlBackgroundColor, kRedColor);
    return col;
}

// toolbar, tabs, custom caption - match reading area in dark themes
COLORREF ThemeChromeBackgroundColor() {
    if (ThemeUsesDarkChrome()) {
        COLORREF bg;
        ThemeDocumentColors(bg);
        return bg;
    }
    return ThemeControlBackgroundColor();
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

COLORREF ThemeReadingTextColor() {
    if (!ThemeUsesDarkChrome()) {
        return ThemeWindowTextColor();
    }
    return kColReadingText;
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

COLORREF ThemeReadingTextDisabledColor() {
    COLORREF txt = ThemeReadingTextColor();
    COLORREF bg;
    ThemeDocumentColors(bg);
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
