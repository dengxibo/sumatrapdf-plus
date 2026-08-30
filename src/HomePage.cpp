/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/ScopedWin.h"
#include "utils/Dpi.h"
#include "utils/FileUtil.h"
#include "utils/WinUtil.h"
#include "utils/SquareTreeParser.h"

#include "wingui/UIModels.h"
#include "wingui/Layout.h"
#include "wingui/WinGui.h"
#include "wingui/VirtWnd.h"

#include "Settings.h"
#include "DocController.h"
#include "EngineBase.h"
#include "SumatraConfig.h"
#include "FileHistory.h"
#include "GlobalPrefs.h"
#include "Annotation.h"
#include "SumatraPDF.h"
#include "MainWindow.h"
#include "resource.h"
#include "Commands.h"
#include "Accelerators.h"
#include "CommandPalette.h"
#include "FileThumbnails.h"
#include "HomePage.h"
#include "Translations.h"
#include "WindowTab.h"
#include "Version.h"
#include "Theme.h"
#include "AppSettings.h"
#include "OverlayScrollbar.h"
#include "DarkModeSubclass.h"
#include "SvgIcons.h"
#include "Toolbar.h"
#include "utils/GdiPlusUtil.h"
#include "utils/Log.h"

#ifndef ABOUT_USE_LESS_COLORS
#define ABOUT_LINE_OUTER_SIZE 2
#else
#define ABOUT_LINE_OUTER_SIZE 1
#endif
#define ABOUT_LINE_SEP_SIZE 1

static const char* const gSumatraTipKeys[] = {
    _TRN("You can [customize scrollbar](CmdChangeScrollbar)."),
    _TRN("You can [customize keyboard shortcuts](Help/Customizing-keyboard-shortcuts)."),
    _TRN("You can [customize toolbar](Help/Customize-toolbar)."),
    _TRN("Press (Key/CmdCommandPalette) to open [command palette](CmdCommandPalette)."),
    _TRN("To open file from history open [command palette](CmdCommandPalette) with (Key/CmdCommandPalette) and type "
         "`#`."),
    _TRN("You can [extract text from PDF file](Help/Tool-x-extract-text-from-pdf)."),
    _TRN("You can [toggle menu bar](CmdToggleMenuBar) with (Key/CmdToggleMenuBar)."),
    _TRN("You can [toggle toolbar](CmdToggleToolbar) with (Key/CmdToggleToolbar)."),
    _TRN("You can [edit PDF annotations](Help/Editing-annotations)."),
};

constexpr const char* sumatraPromos = "";

// TODO: leaks if set
const char* promoFromServer = nullptr;

// a word in a parsed tip; can be part of a link
struct TipWord {
    char* text = nullptr; // owned
    int dx = 0;
    int dy = 0;
    int x = 0;
    int y = 0;
    bool isLink = false;
    int linkIdx = -1; // index into ParsedTip::links
};

struct TipLink {
    char* cmd = nullptr; // owned, the link_command
    int firstWord = 0;
    int lastWord = 0; // inclusive
};

struct ParsedTip {
    Vec<TipWord> words;
    Vec<TipLink> links;
    int totalDy = 0; // computed by layout

    ~ParsedTip() {
        for (auto& w : words) {
            str::Free(w.text);
        }
        for (auto& l : links) {
            str::Free(l.cmd);
        }
    }
};

// resolve (Key/CmdXxx) to keyboard shortcut string
static TempStr ResolveKeyShortcutTemp(const char* cmdName) {
    int cmdId = GetCommandIdByName(cmdName);
    if (cmdId <= 0) {
        return str::DupTemp(cmdName);
    }
    TempStr accel = AppendAccelKeyToMenuStringTemp((TempStr) "", cmdId);
    if (!accel || !*accel) {
        return str::DupTemp(cmdName);
    }
    // AppendAccelKeyToMenuStringTemp prepends \t, skip it
    if (accel[0] == '\t') {
        accel++;
    }
    return accel;
}

// resolve link command to a URL for StaticLink target
static TempStr ResolveLinkCmdTemp(const char* cmd) {
    if (str::StartsWith(cmd, "https://") || str::StartsWith(cmd, "http://")) {
        return str::DupTemp(cmd);
    }
    if (str::StartsWith(cmd, "Help/")) {
        return str::FormatTemp("https://www.sumatrapdfreader.org/docs/%s", cmd + 5);
    }
    // Cmd* - use as-is, will be resolved to command ID on click
    return str::DupTemp(cmd);
}

static void ParseTip(ParsedTip& tip, const char* s) {
    StrBuilder expanded;
    // first pass: expand (Key/CmdXxx) to shortcut strings
    while (*s) {
        if (*s == '(' && str::StartsWith(s + 1, "Key/")) {
            const char* end = str::FindChar(s, ')');
            if (end) {
                // extract command name between "Key/" and ")"
                const char* cmdStart = s + 5; // skip "(Key/"
                TempStr cmdName = str::DupTemp(cmdStart, (int)(end - cmdStart));
                TempStr shortcut = ResolveKeyShortcutTemp(cmdName);
                expanded.Append(shortcut);
                s = end + 1;
                continue;
            }
        }
        expanded.AppendChar(*s);
        s++;
    }

    // second pass: split into words, detecting [text](link) markdown links
    const char* p = expanded.Get();
    while (*p) {
        // skip spaces
        while (*p == ' ') {
            p++;
        }
        if (!*p) {
            break;
        }

        if (*p == '[') {
            // parse markdown link: [text](cmd)
            const char* textStart = p + 1;
            const char* textEnd = str::FindChar(textStart, ']');
            if (textEnd && textEnd[1] == '(') {
                const char* cmdStart = textEnd + 2;
                const char* cmdEnd = str::FindChar(cmdStart, ')');
                if (cmdEnd) {
                    TempStr linkCmd = str::DupTemp(cmdStart, (int)(cmdEnd - cmdStart));
                    TempStr linkText = str::DupTemp(textStart, (int)(textEnd - textStart));

                    TipLink link;
                    link.cmd = str::Dup(ResolveLinkCmdTemp(linkCmd));
                    link.firstWord = tip.words.Size();

                    // split link text into words
                    const char* lt = linkText;
                    while (*lt) {
                        while (*lt == ' ') {
                            lt++;
                        }
                        if (!*lt) {
                            break;
                        }
                        const char* wordStart = lt;
                        while (*lt && *lt != ' ') {
                            lt++;
                        }
                        TipWord w;
                        w.text = str::Dup(wordStart, (int)(lt - wordStart));
                        w.isLink = true;
                        w.linkIdx = tip.links.Size();
                        tip.words.Append(w);
                    }

                    link.lastWord = tip.words.Size() - 1;
                    tip.links.Append(link);
                    p = cmdEnd + 1;
                    continue;
                }
            }
        }

        // regular word
        const char* wordStart = p;
        while (*p && *p != ' ' && *p != '[') {
            p++;
        }
        if (p > wordStart) {
            TipWord w;
            w.text = str::Dup(wordStart, (int)(p - wordStart));
            tip.words.Append(w);
        }
    }
}

static void MeasureTipWords(ParsedTip& tip, HDC hdc, HFONT font) {
    uint fmt = DT_LEFT | DT_NOCLIP;
    for (auto& w : tip.words) {
        Size sz = HdcMeasureText(hdc, w.text, fmt, font);
        w.dx = sz.dx;
        w.dy = sz.dy;
    }
}

static void LayoutTip(ParsedTip& tip, int areaWidth, int startX, int startY) {
    int x = startX;
    int y = startY;
    int lineHeight = 0;
    int spaceWidth = 4; // approximate space between words
    for (auto& w : tip.words) {
        if (x > startX && x + w.dx > startX + areaWidth) {
            // wrap to next line
            x = startX;
            y += lineHeight + 2;
            lineHeight = 0;
        }
        w.x = x;
        w.y = y;
        x += w.dx + spaceWidth;
        if (w.dy > lineHeight) {
            lineHeight = w.dy;
        }
    }
    tip.totalDy = (y - startY) + lineHeight;
}

static ParsedTip* gParsedTips = nullptr;
static int gParsedTipCount = 0;
static ParsedTip* gParsedPromos = nullptr;
static int gParsedPromoCount = 0;
static bool gSelectedIsPromo = false;
static int gSelectedTipIdx = -1;

static int ParseTipsFromString(const char* src, const char* prefix, ParsedTip*& outTips) {
    StrVec lines;
    Split(&lines, src, "\n");
    int n = 0;
    for (int i = 0; i < lines.Size(); i++) {
        const char* line = lines.At(i);
        if (!str::IsEmptyOrWhiteSpace(line)) {
            n++;
        }
    }
    if (n == 0) {
        return 0;
    }
    outTips = new ParsedTip[n];
    int count = 0;
    for (int i = 0; i < lines.Size(); i++) {
        const char* line = lines.At(i);
        if (str::IsEmptyOrWhiteSpace(line)) {
            continue;
        }
        if (prefix) {
            TempStr prefixed = str::FormatTemp("%s%s", prefix, line);
            ParseTip(outTips[count], prefixed);
        } else {
            ParseTip(outTips[count], line);
        }
        count++;
    }
    return count;
}

static void PickRandomTipOrPromo() {
    bool pickPromo = (gParsedPromoCount > 0) && (rand() % 100 < 30);
    if (pickPromo) {
        gSelectedIsPromo = true;
        gSelectedTipIdx = rand() % gParsedPromoCount;
    } else if (gParsedTipCount > 0) {
        gSelectedIsPromo = false;
        gSelectedTipIdx = rand() % gParsedTipCount;
    }
}

static void ResetTipsParsed() {
    if (gParsedTips) {
        delete[] gParsedTips;
        gParsedTips = nullptr;
    }
    gParsedTipCount = 0;
    if (gParsedPromos) {
        delete[] gParsedPromos;
        gParsedPromos = nullptr;
    }
    gParsedPromoCount = 0;
    gSelectedTipIdx = -1;
}

static void EnsureTipsParsed() {
    if (gParsedTips || gParsedPromos) {
        return;
    }
    const char* tipPrefix = _TRA("Tip: ");
    int n = dimofi(gSumatraTipKeys);
    gParsedTips = new ParsedTip[n];
    for (int i = 0; i < n; i++) {
        const char* translated = _TRA(gSumatraTipKeys[i]);
        TempStr prefixed = str::FormatTemp("%s%s", tipPrefix, translated);
        ParseTip(gParsedTips[i], prefixed);
    }
    gParsedTipCount = n;
    gParsedPromoCount = ParseTipsFromString(sumatraPromos, nullptr, gParsedPromos);
    PickRandomTipOrPromo();
}

static void PickAnotherRandomTip() {
    bool prevIsPromo = gSelectedIsPromo;
    int prev = gSelectedTipIdx;
    // keep picking until we get a different one
    int maxIter = 100;
    while (maxIter-- > 0) {
        PickRandomTipOrPromo();
        if (gSelectedIsPromo != prevIsPromo || gSelectedTipIdx != prev) {
            return;
        }
    }
}

constexpr COLORREF kAboutBorderCol = RGB(0, 0, 0);

constexpr int kAboutLeftRightSpaceDx = 8;
constexpr int kAboutMarginDx = 10;
constexpr int kAboutBoxMarginDy = 6;
constexpr int kAboutTxtDy = 6;
constexpr int kAboutRectPadding = 8;

constexpr int kInnerPadding = 8;

constexpr const char* kSumatraTxtFont = "Arial Black";
constexpr int kSumatraTxtFontSize = 24;

constexpr const char* kVersionTxtFont = "Arial Black";
constexpr int kVersionTxtFontSize = 12;

#define LAYOUT_LTR 0

static ATOM gAtomAbout;
static HWND gHwndAbout;
static Tooltip* gAboutTooltip = nullptr;
static const char* gClickedURL = nullptr;

struct AboutLayoutInfoEl {
    /* static data, must be provided */
    const char* leftTxt;
    const char* rightTxt;
    const char* url;

    /* data calculated by the layout */
    Rect leftPos;
    Rect rightPos;
};

static AboutLayoutInfoEl gAboutLayoutInfo[] = {
    {_TRN("Plus source"), _TRN("Sumatra PDF Plus on GitHub"), kPlusRepoURL},
    {_TRN("Plus issues"), _TRN("Report bugs (this fork only)"), kPlusIssuesURL},
    {_TRN("Plus guide"), _TRN("User guide (readme.txt)"), kPlusReadmeURL},
    {_TRN("official site"), _TRN("SumatraPDF website (upstream)"), kWebsiteURL},
    {_TRN("official manual"), _TRN("SumatraPDF manual (upstream)"), kManualURL},
    {_TRN("official forums"), _TRN("SumatraPDF forums (upstream)"),
     "https://github.com/sumatrapdfreader/sumatrapdf/discussions"},
    {_TRN("programming"), _TRN("The Programmers"),
     "https://github.com/sumatrapdfreader/sumatrapdf/blob/master/AUTHORS"},
    {_TRN("licenses"), _TRN("Various Open Source"),
     "https://github.com/sumatrapdfreader/sumatrapdf/blob/master/AUTHORS"},
#if defined(GIT_COMMIT_ID_STR)
    {_TRN("last change"), _TRN("git commit"), kPlusRepoURL "/commit/" GIT_COMMIT_ID_STR},
#endif
#if defined(PRE_RELEASE_VER)
    {_TRN("a note"), _TRN("Pre-release version, for testing only!"), nullptr},
#endif
#ifdef DEBUG
    {_TRN("a note"), _TRN("Debug version, for testing only!"), nullptr},
#endif
    {nullptr, nullptr, nullptr}};

static TempStr AboutLeftTxtTemp(const AboutLayoutInfoEl* el) {
    return str::DupTemp(_TRA(el->leftTxt));
}

static TempStr AboutRightTxtTemp(const AboutLayoutInfoEl* el) {
    if (str::Eq(el->rightTxt, "git commit")) {
#if defined(GIT_COMMIT_ID_STR)
        return str::JoinTemp(_TRA("git commit"), " ", GIT_COMMIT_ID_STR);
#else
        return str::DupTemp(_TRA("git commit"));
#endif
    }
    return str::DupTemp(_TRA(el->rightTxt));
}

static Vec<StaticLink*> gStaticLinks;

void SetPromoString(const char* s) {
    if (!s) return;
    str::ReplaceWithCopy(&promoFromServer, s);
}

static TempStr GetAppVersionTemp() {
    TempStr s = str::DupTemp("v" CURR_VERSION_STRA);
    if (IsProcess64()) {
        s = str::JoinTemp(s, " ", _TRA("64-bit"));
    } else {
        s = str::JoinTemp(s, " ", _TRA("32-bit"));
    }
    if (gIsDebugBuild) {
        s = str::JoinTemp(s, " ", _TRA("(dbg)"));
    }
    return s;
}

constexpr COLORREF kCol1 = RGB(196, 64, 50);
constexpr COLORREF kCol2 = RGB(227, 107, 35);
constexpr COLORREF kCol3 = RGB(93, 160, 40);
constexpr COLORREF kCol4 = RGB(69, 132, 190);
constexpr COLORREF kCol5 = RGB(112, 115, 207);

static void DrawSumatraVersion(HDC hdc, Rect rect) {
    uint fmt = DT_LEFT | DT_NOCLIP;
    HFONT fontSumatraTxt = CreateSimpleFont(hdc, kSumatraTxtFont, kSumatraTxtFontSize);
    HFONT fontVersionTxt = CreateSimpleFont(hdc, kVersionTxtFont, kVersionTxtFontSize);

    SetBkMode(hdc, TRANSPARENT);

    const char* txt = kAppName;
    Size txtSize = HdcMeasureText(hdc, txt, fmt, fontSumatraTxt);
    Rect mainRect(rect.x + (rect.dx - txtSize.dx) / 2, rect.y + (rect.dy - txtSize.dy) / 2, txtSize.dx, txtSize.dy);

    // draw SumatraPDF in colorful way
    Point pt = mainRect.TL();
    // colorful version
    static COLORREF cols[] = {kCol1, kCol2, kCol3, kCol4, kCol5, kCol5, kCol4, kCol3, kCol2, kCol1};
    char buf[2] = {};
    for (int i = 0; i < str::Leni(kAppName); i++) {
        SetTextColor(hdc, cols[i % dimofi(cols)]);
        buf[0] = kAppName[i];
        HdcDrawText(hdc, buf, pt, fmt, fontSumatraTxt);
        txtSize = HdcMeasureText(hdc, buf, fmt, fontSumatraTxt);
        pt.x += txtSize.dx;
    }

    SetTextColor(hdc, ThemeWindowTextColor());
    int x = mainRect.x + mainRect.dx + DpiScale(hdc, kInnerPadding);
    int y = mainRect.y;

    TempStr ver = GetAppVersionTemp();
    Point p = {x, y};
    HdcDrawText(hdc, ver, p, fmt, fontVersionTxt);
    p.y += DpiScale(hdc, 13);
    if (gIsPreReleaseBuild) {
        HdcDrawText(hdc, _TRA("Pre-release"), p, fmt);
    }
}

// draw on the bottom right
static Rect DrawHideFrequentlyReadLink(HWND hwnd, HDC hdc, const char* txt) {
    HFONT fontLeftTxt = CreateSimpleFont(hdc, "MS Shell Dlg", 16);

    VirtWndText w(hwnd, txt, fontLeftTxt);
    w.isRtl = IsUIRtl();
    w.withUnderline = true;
    Size txtSize = w.GetIdealSize(true);

    auto col = ThemeWindowLinkColor();
    ScopedSelectObject pen(hdc, CreatePen(PS_SOLID, 1, col), true);

    SetTextColor(hdc, col);
    SetBkMode(hdc, TRANSPARENT);
    Rect rc = ClientRect(hwnd);

    int innerPadding = DpiScale(hwnd, kInnerPadding);
    Rect r = {0, 0, txtSize.dx, txtSize.dy};
    PositionRB(rc, r);
    MoveXY(r, -innerPadding, -innerPadding);
    w.SetBounds(r);
    w.Paint(hdc);

    // make the click target larger
    r.Inflate(innerPadding, innerPadding);
    return r;
}

static Size CalcSumatraVersionSize(HDC hdc) {
    HFONT fontSumatraTxt = CreateSimpleFont(hdc, kSumatraTxtFont, kSumatraTxtFontSize);
    HFONT fontVersionTxt = CreateSimpleFont(hdc, kVersionTxtFont, kVersionTxtFontSize);

    /* calculate minimal top box size */
    Size sz = HdcMeasureText(hdc, kAppName, fontSumatraTxt);
    sz.dy = sz.dy + DpiScale(hdc, kAboutBoxMarginDy * 2);

    /* consider version and version-sub strings */
    TempStr ver = GetAppVersionTemp();
    Size txtSize = HdcMeasureText(hdc, ver, fontVersionTxt);
    int minWidth = txtSize.dx + DpiScale(hdc, 8);
    int dx = std::max(txtSize.dx, minWidth);
    sz.dx += 2 * (dx + DpiScale(hdc, kInnerPadding));
    return sz;
}

static TempStr TrimGitTemp(const char* s) {
    if (gitCommidId && str::EndsWith(s, gitCommidId)) {
        int len = (int)(str::Len(s) - str::Len(gitCommidId));
        while (len > 0 && s[len - 1] == ' ') {
            len--;
        }
        return str::DupTemp(s, len);
    }
    return (TempStr)s;
}

/* Draws the about screen and remembers some state for hyperlinking.
   It transcribes the design I did in graphics software - hopeless
   to understand without seeing the design. */
static void DrawAbout(HWND hwnd, HDC hdc, Rect rect, Vec<StaticLink*>& staticLinks) {
    auto col = ThemeWindowTextColor();
    AutoDeletePen penBorder(CreatePen(PS_SOLID, ABOUT_LINE_OUTER_SIZE, col));
    AutoDeletePen penDivideLine(CreatePen(PS_SOLID, ABOUT_LINE_SEP_SIZE, col));
    col = ThemeWindowLinkColor();
    AutoDeletePen penLinkLine(CreatePen(PS_SOLID, ABOUT_LINE_SEP_SIZE, col));

    HFONT fontLeftTxt = CreateSimpleFont(hdc, kLeftTextFont, kLeftTextFontSize);
    HFONT fontRightTxt = CreateSimpleFont(hdc, kRightTextFont, kRightTextFontSize);

    ScopedSelectObject font(hdc, fontLeftTxt); /* Just to remember the orig font */

    Rect rc = ClientRect(hwnd);
    col = ThemeMainWindowBackgroundColor();
    AutoDeleteBrush brushAboutBg = CreateSolidBrush(col);
    FillRect(hdc, rc, brushAboutBg);

    /* render title */
    Rect titleRect(rect.TL(), CalcSumatraVersionSize(hdc));

    ScopedSelectObject brush(hdc, CreateSolidBrush(col), true);
    ScopedSelectObject pen(hdc, penBorder);
#ifndef ABOUT_USE_LESS_COLORS
    Rectangle(hdc, rect.x, rect.y + ABOUT_LINE_OUTER_SIZE, rect.x + rect.dx,
              rect.y + titleRect.dy + ABOUT_LINE_OUTER_SIZE);
#else
    Rect titleBgBand(0, rect.y, rc.dx, titleRect.dy);
    RECT rcLogoBg = titleBgBand.ToRECT();
    FillRect(hdc, &rcLogoBg, bgBrush);
    DrawLine(hdc, Rect(0, rect.y, rc.dx, 0));
    DrawLine(hdc, Rect(0, rect.y + titleRect.dy, rc.dx, 0));
#endif

    titleRect.Offset((rect.dx - titleRect.dx) / 2, 0);
    DrawSumatraVersion(hdc, titleRect);

    /* render attribution box */
    col = ThemeWindowTextColor();
    SetTextColor(hdc, col);
    SetBkMode(hdc, TRANSPARENT);

#ifndef ABOUT_USE_LESS_COLORS
    Rectangle(hdc, rect.x, rect.y + titleRect.dy, rect.x + rect.dx, rect.y + rect.dy);
#endif

    /* render text on the left*/
    SelectObject(hdc, fontLeftTxt);
    uint fmt = DT_LEFT | DT_NOCLIP;
    for (AboutLayoutInfoEl* el = gAboutLayoutInfo; el->leftTxt; el++) {
        TempStr leftTxt = AboutLeftTxtTemp(el);
        auto& pos = el->leftPos;
        HdcDrawText(hdc, leftTxt, pos, fmt);
    }

    /* render text on the right */
    SelectObject(hdc, fontRightTxt);
    SelectObject(hdc, penLinkLine);
    DeleteVecMembers(staticLinks);
    for (AboutLayoutInfoEl* el = gAboutLayoutInfo; el->leftTxt; el++) {
        bool hasUrl = CanAccessDisk() && el->url;
        if (hasUrl) {
            col = ThemeWindowLinkColor();
        } else {
            col = ThemeWindowTextColor();
        }
        SetTextColor(hdc, col);
        TempStr s = AboutRightTxtTemp(el);
        s = TrimGitTemp(s);
        auto& pos = el->rightPos;
        HdcDrawText(hdc, s, pos, fmt);

        if (hasUrl) {
            int underlineY = pos.y + pos.dy - 3;
            DrawLine(hdc, Rect(pos.x, underlineY, pos.dx, 0));
            auto sl = new StaticLink(pos, el->url, el->url);
            staticLinks.Append(sl);
        }
    }

    SelectObject(hdc, penDivideLine);
    Rect divideLine(gAboutLayoutInfo[0].rightPos.x - DpiScale(hwnd, kAboutLeftRightSpaceDx), rect.y + titleRect.dy + 4,
                    0, rect.y + rect.dy - 4 - gAboutLayoutInfo[0].rightPos.y);
    DrawLine(hdc, divideLine);
}

static void UpdateAboutLayoutInfo(HWND hwnd, HDC hdc, Rect* rect) {
    HFONT fontLeftTxt = CreateSimpleFont(hdc, kLeftTextFont, kLeftTextFontSize);
    HFONT fontRightTxt = CreateSimpleFont(hdc, kRightTextFont, kRightTextFontSize);

    /* calculate minimal top box size */
    Size headerSize = CalcSumatraVersionSize(hdc);

    /* calculate left text dimensions */
    int leftLargestDx = 0;
    int leftDy = 0;
    uint fmt = DT_LEFT;
    for (AboutLayoutInfoEl* el = gAboutLayoutInfo; el->leftTxt; el++) {
        TempStr leftTxt = AboutLeftTxtTemp(el);
        Size txtSize = HdcMeasureText(hdc, leftTxt, fmt, fontLeftTxt);
        el->leftPos.dx = txtSize.dx;
        el->leftPos.dy = txtSize.dy;

        if (el == &gAboutLayoutInfo[0]) {
            leftDy = el->leftPos.dy;
        } else {
            ReportIf(leftDy != el->leftPos.dy);
        }
        if (leftLargestDx < el->leftPos.dx) {
            leftLargestDx = el->leftPos.dx;
        }
    }

    /* calculate right text dimensions */
    int rightLargestDx = 0;
    int rightDy = 0;
    for (AboutLayoutInfoEl* el = gAboutLayoutInfo; el->leftTxt; el++) {
        TempStr s = AboutRightTxtTemp(el);
        s = TrimGitTemp(s);
        Size txtSize = HdcMeasureText(hdc, s, fmt, fontRightTxt);
        el->rightPos.dx = txtSize.dx;
        el->rightPos.dy = txtSize.dy;

        if (el == &gAboutLayoutInfo[0]) {
            rightDy = el->rightPos.dy;
        } else {
            ReportIf(rightDy != el->rightPos.dy);
        }
        if (rightLargestDx < el->rightPos.dx) {
            rightLargestDx = el->rightPos.dx;
        }
    }

    int leftRightSpaceDx = DpiScale(hwnd, kAboutLeftRightSpaceDx);
    int marginDx = DpiScale(hwnd, kAboutMarginDx);
    int aboutTxtDy = DpiScale(hwnd, kAboutTxtDy);
    /* calculate total dimension and position */
    Rect minRect;
    minRect.dx = leftRightSpaceDx + leftLargestDx + ABOUT_LINE_SEP_SIZE + rightLargestDx + leftRightSpaceDx;
    if (minRect.dx < headerSize.dx) {
        minRect.dx = headerSize.dx;
    }
    minRect.dx += 2 * ABOUT_LINE_OUTER_SIZE + 2 * marginDx;

    minRect.dy = headerSize.dy;
    for (AboutLayoutInfoEl* el = gAboutLayoutInfo; el->leftTxt; el++) {
        minRect.dy += rightDy + aboutTxtDy;
    }
    minRect.dy += 2 * ABOUT_LINE_OUTER_SIZE + 4;

    Rect rc = ClientRect(hwnd);
    minRect.x = (rc.dx - minRect.dx) / 2;
    minRect.y = (rc.dy - minRect.dy) / 2;

    if (rect) {
        *rect = minRect;
    }

    /* calculate text positions */
    int linePosX = ABOUT_LINE_OUTER_SIZE + marginDx + leftLargestDx + leftRightSpaceDx;
    int currY = minRect.y + headerSize.dy + 4;
    for (AboutLayoutInfoEl* el = gAboutLayoutInfo; el->leftTxt; el++) {
        el->leftPos.x = minRect.x + linePosX - leftRightSpaceDx - el->leftPos.dx;
        el->leftPos.y = currY + (rightDy - leftDy) / 2;
        el->rightPos.x = minRect.x + linePosX + leftRightSpaceDx;
        el->rightPos.y = currY;
        currY += rightDy + aboutTxtDy;
    }
}

static void OnPaintAbout(HWND hwnd) {
    PAINTSTRUCT ps;
    Rect rc;
    HDC hdc = BeginPaint(hwnd, &ps);
    SetLayout(hdc, LAYOUT_LTR);
    UpdateAboutLayoutInfo(hwnd, hdc, &rc);
    DrawAbout(hwnd, hdc, rc, gStaticLinks);
    EndPaint(hwnd, &ps);
}

static void OnSizeAbout(HWND hwnd) {
    // TODO: do I need anything here?
}

static void CopyAboutInfoToClipboard() {
    StrBuilder info(512);
    TempStr ver = GetAppVersionTemp();
    info.AppendFmt("%s %s\r\n", kAppName, ver);
    for (int i = info.Size() - 2; i > 0; i--) {
        info.AppendChar('-');
    }
    info.Append("\r\n");
    // concatenate all the information into a single string
    // (cf. CopyPropertiesToClipboard in SumatraProperties.cpp)
    int maxLen = 0;
    for (AboutLayoutInfoEl* el = gAboutLayoutInfo; el->leftTxt; el++) {
        maxLen = std::max(maxLen, str::Leni(el->leftTxt));
    }
    for (AboutLayoutInfoEl* el = gAboutLayoutInfo; el->leftTxt; el++) {
        for (int i = maxLen - str::Leni(el->leftTxt); i > 0; i--) {
            info.AppendChar(' ');
        }
        info.AppendFmt("%s: %s\r\n", el->leftTxt, el->url ? el->url : el->rightTxt);
    }
    CopyTextToClipboard(info.LendData());
}

TempStr GetStaticLinkAtTemp(Vec<StaticLink*>& staticLinks, int x, int y, StaticLink** linkOut) {
    if (!CanAccessDisk()) {
        return nullptr;
    }

    Point pt(x, y);
    for (int i = 0; i < staticLinks.Size(); i++) {
        if (staticLinks.at(i)->rect.Contains(pt)) {
            auto link = staticLinks.At(i);
            if (linkOut) {
                *linkOut = link;
            }
            return str::DupTemp(link->target);
        }
    }

    return nullptr;
}

static void CreateInfotipForLink(StaticLink* linkInfo) {
    if (gAboutTooltip != nullptr) {
        return;
    }

    Tooltip::CreateArgs args;
    args.parent = gHwndAbout;
    args.font = GetAppFont();
    args.isRtl = IsUIRtl();

    gAboutTooltip = new Tooltip();
    gAboutTooltip->Create(args);
    gAboutTooltip->SetSingle(linkInfo->tooltip, linkInfo->rect, false);
}

static void DeleteInfotip() {
    if (gAboutTooltip == nullptr) {
        return;
    }
    // gAboutTooltip->Hide();
    delete gAboutTooltip;
    gAboutTooltip = nullptr;
}

LRESULT CALLBACK WndProcAbout(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    const char* url;
    Point pt;

    int x = GET_X_LPARAM(lp);
    int y = GET_Y_LPARAM(lp);
    switch (msg) {
        case WM_CREATE:
            ReportIf(gHwndAbout);
            UpdateWindowCaptionTheme(hwnd);
            break;

        case WM_ERASEBKGND:
            // do nothing, helps to avoid flicker
            return TRUE;

        case WM_SIZE:
            OnSizeAbout(hwnd);
            break;

        case WM_PAINT:
            OnPaintAbout(hwnd);
            break;

        case WM_SETCURSOR:
            pt = HwndGetCursorPos(hwnd);
            if (!pt.IsEmpty()) {
                StaticLink* linkInfo;
                if (GetStaticLinkAtTemp(gStaticLinks, pt.x, pt.y, &linkInfo)) {
                    CreateInfotipForLink(linkInfo);
                    SetCursorCached(IDC_HAND);
                    return TRUE;
                }
            }
            DeleteInfotip();
            return DefWindowProc(hwnd, msg, wp, lp);

        case WM_LBUTTONDOWN: {
            url = GetStaticLinkAtTemp(gStaticLinks, x, y, nullptr);
            str::ReplaceWithCopy(&gClickedURL, url);
        } break;

        case WM_LBUTTONUP:
            url = GetStaticLinkAtTemp(gStaticLinks, x, y, nullptr);
            if (url && str::Eq(url, gClickedURL)) {
                SumatraLaunchBrowser(url);
            }
            break;

        case WM_CHAR:
            if (VK_ESCAPE == wp) {
                DestroyWindow(hwnd);
            }
            break;

        case WM_COMMAND:
            if (CmdCopySelection == LOWORD(wp)) {
                CopyAboutInfoToClipboard();
            }
            break;

        case WM_DESTROY:
            DeleteInfotip();
            ReportIf(!gHwndAbout);
            gHwndAbout = nullptr;
            break;

        default:
            return DefWindowProc(hwnd, msg, wp, lp);
    }
    return 0;
}

constexpr const WCHAR* kAboutClassName = L"SUMATRA_PDF_ABOUT";

void CloseAboutWindow() {
    if (gHwndAbout) {
        DestroyWindow(gHwndAbout);
    }
}

void ShowAboutWindow(MainWindow* win) {
    if (gHwndAbout) {
        SetActiveWindow(gHwndAbout);
        return;
    }

    if (!gAtomAbout) {
        WNDCLASSEX wcex;
        FillWndClassEx(wcex, kAboutClassName, WndProcAbout);
        HMODULE h = GetModuleHandleW(nullptr);
        wcex.hIcon = LoadIcon(h, MAKEINTRESOURCE(GetAppIconID()));
        gAtomAbout = RegisterClassEx(&wcex);
        ReportIf(!gAtomAbout);
    }

    TempWStr title = ToWStrTemp(_TRA("About Sumatra PDF Plus"));
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    int x = CW_USEDEFAULT;
    int y = CW_USEDEFAULT;
    int dx = CW_USEDEFAULT;
    int dy = CW_USEDEFAULT;
    HINSTANCE h = GetModuleHandleW(nullptr);
    gHwndAbout = CreateWindowExW(0, kAboutClassName, title, style, x, y, dx, dy, nullptr, nullptr, h, nullptr);
    if (!gHwndAbout) {
        return;
    }

    HwndSetRtl(gHwndAbout, IsUIRtl());

    // get the dimensions required for the about box's content
    Rect rc;
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(gHwndAbout, &ps);
    SetLayout(hdc, LAYOUT_LTR);
    UpdateAboutLayoutInfo(gHwndAbout, hdc, &rc);
    EndPaint(gHwndAbout, &ps);
    int rectPadding = DpiScale(gHwndAbout, kAboutRectPadding);
    rc.Inflate(rectPadding, rectPadding);

    // resize the new window to just match these dimensions
    Rect wRc = WindowRect(gHwndAbout);
    Rect cRc = ClientRect(gHwndAbout);
    wRc.dx += rc.dx - cRc.dx;
    wRc.dy += rc.dy - cRc.dy;
    MoveWindow(gHwndAbout, wRc.x, wRc.y, wRc.dx, wRc.dy, FALSE);

    HwndPositionInCenterOf(gHwndAbout, win->hwndFrame);
    ShowWindow(gHwndAbout, SW_SHOW);
}

void DrawAboutPage(MainWindow* win, HDC hdc) {
    Rect rc = ClientRect(win->hwndCanvas);
    UpdateAboutLayoutInfo(win->hwndCanvas, hdc, &rc);
    DrawAbout(win->hwndCanvas, hdc, rc, win->staticLinks);
    if (HasPermission(Perm::SavePreferences | Perm::DiskAccess) && SettingsRememberOpenedFiles()) {
        Rect rect = DrawHideFrequentlyReadLink(win->hwndCanvas, hdc, _TRA("Show frequently read"));
        auto sl = new StaticLink(rect, kLinkShowList);
        win->staticLinks.Append(sl);
    }
}

/* alternate static page to display when no document is loaded */

constexpr int kThumbsSeparatorDy = 2;
constexpr int kThumbsBorderDx = 1;
// Use window DPI (not the paint/buffer HDC). CreateCompatibleDC memory DCs often
// report 96 DPI under PerMonitorV2, which made layout row pitch disagree with
// scroll redraw via HomePageItemRect and crushed rows after the first gap.
#define kThumbsMarginLeft DpiScale(dpiHwnd, 40)
#define kThumbsMarginRight DpiScale(dpiHwnd, 40)
#define kThumbsMarginTop DpiScale(dpiHwnd, 50)
#define kThumbsMarginBottom DpiScale(dpiHwnd, 40)
#define kThumbsSpaceBetweenX DpiScale(dpiHwnd, 38)
#define kThumbsSpaceBetweenY DpiScale(dpiHwnd, 58)
#define kThumbsBottomBoxDy DpiScale(dpiHwnd, 50)

struct HomeUiFontKey {
    int dpi = 0;
    int px = 0;
    int weight = 0;
    HFONT font = nullptr;
};
static Vec<HomeUiFontKey> gHomeUiFonts;

// System UI face (Segoe UI / Microsoft YaHei UI) + ClearType. MS Shell Dlg 24pt
// falls back to SimSun for CJK and looks jagged at display sizes.
static HFONT HomePageUiFont(HWND hwnd, int deltaPx96, int weight) {
    HFONT app = GetAppFontForHwnd(hwnd);
    LOGFONTW lf{};
    GetObjectW(app, sizeof(lf), &lf);
    int dpi = DpiGet(hwnd);
    int px = std::abs(lf.lfHeight) + DpiScale(hwnd, deltaPx96);
    if (px < DpiScale(hwnd, 11)) {
        px = DpiScale(hwnd, 11);
    }
    for (HomeUiFontKey& e : gHomeUiFonts) {
        if (e.dpi == dpi && e.px == px && e.weight == weight) {
            return e.font;
        }
    }
    lf.lfHeight = -px;
    lf.lfWidth = 0;
    lf.lfWeight = weight;
    lf.lfItalic = FALSE;
    lf.lfUnderline = FALSE;
    lf.lfStrikeOut = FALSE;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lf.lfOutPrecision = OUT_TT_ONLY_PRECIS;
    HFONT font = CreateFontIndirectW(&lf);
    gHomeUiFonts.Append({dpi, px, weight, font});
    return font;
}

static HFONT HomePageFileNameFont(HWND hwnd) {
    return HomePageUiFont(hwnd, 2, FW_SEMIBOLD);
}

// Soften theme ink so home-page type isn't max-contrast black on beige.
static COLORREF HomePageInkColor() {
    return AccentColor(ThemeWindowTextColor(), 42);
}

static COLORREF HomePageListNameColor() {
    return HomePageInkColor();
}

static HFONT HomePageSearchFont(HWND hwnd) {
    // Same size band as filenames. Menu/app font sits too high in the chrome.
    return HomePageUiFont(hwnd, 3, FW_NORMAL);
}

static HFONT HomePageMetaFont(HWND hwnd) {
    return HomePageUiFont(hwnd, 0, FW_NORMAL);
}

static HFONT HomePageThumbLabelFont(HWND hwnd) {
    return HomePageUiFont(hwnd, 2, FW_MEDIUM);
}

static int HomePageFontLineDy(HWND hwnd, HFONT font) {
    HDC hdc = GetDC(hwnd);
    TEXTMETRIC tm{};
    HFONT old = (HFONT)SelectObject(hdc, font);
    GetTextMetrics(hdc, &tm);
    SelectObject(hdc, old);
    ReleaseDC(hwnd, hdc);
    return tm.tmHeight + tm.tmExternalLeading;
}

static int HomePageListThumbDx(HWND hwnd) {
    return DpiScale(hwnd, 40);
}

static int HomePageListThumbDy(HWND hwnd) {
    return DpiScale(hwnd, 48);
}

static int HomePageListGapDx(HWND hwnd) {
    return DpiScale(hwnd, 10);
}

static int HomePageListRowSpacing(HWND hwnd) {
    return DpiScale(hwnd, 2);
}

static int HomePageListRowDy(HWND hwnd) {
    int pad = DpiScale(hwnd, 8);
    int thumb = HomePageListThumbDy(hwnd);
    int text = HomePageFontLineDy(hwnd, HomePageFileNameFont(hwnd)) + DpiScale(hwnd, 2) +
               HomePageFontLineDy(hwnd, HomePageMetaFont(hwnd));
    return std::max(thumb, text) + pad * 2;
}

static int HomePageThumbLabelDy(HWND hwnd, HDC hdc, HFONT font) {
    TEXTMETRIC tm{};
    HFONT old = (HFONT)SelectObject(hdc, font);
    GetTextMetrics(hdc, &tm);
    SelectObject(hdc, old);
    int fromFont = tm.tmHeight + tm.tmExternalLeading;
    int minDy = DpiScale(hwnd, 20);
    return std::max(minDy, fromFont);
}

// Gap between thumbnail bottom and filename (keep tight; do not vertically
// center the label in the full row gap — that left ~18px empty under the thumb).
static int HomePageThumbLabelGapY(HWND hwnd) {
    return DpiScale(hwnd, 6);
}

static void GetFileStateIcon(FileState* fs);

static int HomePageThumbDisplayDx(MainWindow* win) {
    if (win && win->homePageThumbDx > 0) {
        return win->homePageThumbDx;
    }
    return kThumbnailDx;
}

static int HomePageThumbDisplayDy(MainWindow* win) {
    if (win && win->homePageThumbDy > 0) {
        return win->homePageThumbDy;
    }
    return kThumbnailDy;
}

static int HomePagePreferredThumbDx() {
    int dx = kThumbnailDx;
    if (gGlobalPrefs && gGlobalPrefs->homePageThumbnailDx > 0) {
        dx = gGlobalPrefs->homePageThumbnailDx;
    }
    if (dx < 160) {
        dx = 160;
    }
    if (dx > 280) {
        dx = 280;
    }
    return dx;
}

// Grow/shrink thumbnail size around the preferred width so the grid fills the
// canvas. Cap at ~±12–20% so the 212px cache does not look too blurry.
static void ComputeHomeThumbGrid(HWND dpiHwnd, int canvasDx, int& colsOut, int& thumbDxOut, int& thumbDyOut,
                                 int& startXOut, int& gapXOut) {
    int preferred = HomePagePreferredThumbDx();
    int gapX = kThumbsSpaceBetweenX;
    int marginL = kThumbsMarginLeft;
    int marginR = kThumbsMarginRight;
    int avail = canvasDx - marginL - marginR;
    if (avail < 80) {
        avail = 80;
    }
    int minDx = (preferred * 88 + 50) / 100;
    int maxDx = (preferred * 120 + 50) / 100;
    if (minDx < 120) {
        minDx = 120;
    }

    auto fillDx = [&](int nCols) -> int {
        if (nCols < 1) {
            nCols = 1;
        }
        return (avail - (nCols - 1) * gapX) / nCols;
    };

    int cols = (avail + gapX) / (preferred + gapX);
    if (cols < 1) {
        cols = 1;
    }
    int dx = fillDx(cols);
    if (dx > maxDx) {
        int dxMoreCols = fillDx(cols + 1);
        if (dxMoreCols >= minDx) {
            cols = cols + 1;
            dx = dxMoreCols;
        } else {
            dx = maxDx;
        }
    } else if (dx < minDx && cols > 1) {
        cols--;
        dx = fillDx(cols);
        if (dx > maxDx) {
            dx = maxDx;
        }
    }
    if (dx < 120) {
        dx = 120;
    }

    int thumbDy = (dx * kThumbnailDy + kThumbnailDx / 2) / kThumbnailDx;
    int content = cols * dx + (cols - 1) * gapX;
    int startX = marginL + std::max(0, (avail - content) / 2);
    int minStart = DpiScale(dpiHwnd, kInnerPadding);
    if (startX < minStart) {
        startX = minStart;
    }
    colsOut = cols;
    thumbDxOut = dx;
    thumbDyOut = thumbDy;
    startXOut = startX;
    gapXOut = gapX;
}

static int HomePageThumbSpaceBetweenY(MainWindow* win) {
    int thumbDy = HomePageThumbDisplayDy(win);
    int spaceY = win && win->homePageRowDy > thumbDy ? win->homePageRowDy - thumbDy : 0;
    if (spaceY <= 0 && win) {
        spaceY = DpiScale(win->hwndCanvas, 58);
    }
    return spaceY;
}

// Full-width label band under the thumbnail. Icon+filename are horizontally
// centered inside this band when drawing.
static Rect HomePageThumbTextRect(HWND hwnd, const Rect& rcPage, int labelDy, int spaceBetweenY, bool) {
    int padY = HomePageThumbLabelGapY(hwnd);
    if (spaceBetweenY > 0 && padY + labelDy > spaceBetweenY) {
        padY = std::max(0, spaceBetweenY - labelDy);
    }
    return Rect(rcPage.x, rcPage.y + rcPage.dy + padY, rcPage.dx, labelDy);
}

struct HomePageThumbLabelLayout {
    Rect rcIcon;
    Rect rcText;
    int blockX = 0;
    int blockDx = 0;
    int padLeft = 0;
    int padRight = 0;
};

static HomePageThumbLabelLayout LayoutHomePageThumbLabel(HWND hwnd, HDC hdc, const Rect& page, const Rect& rcBand,
                                                         FileState* fs, const char* fileName, HFONT font, bool isRtl) {
    HomePageThumbLabelLayout out{};
    GetFileStateIcon(fs);
    int iconDx = 0, iconDy = 0;
    if (fs->himl) {
        ImageList_GetIconSize(fs->himl, &iconDx, &iconDy);
    }
    int gap = DpiScale(hwnd, 4);
    HFONT old = (HFONT)SelectObject(hdc, font);
    Size textSz = HdcMeasureText(hdc, fileName, DT_SINGLELINE | DT_NOPREFIX, font);
    SelectObject(hdc, old);

    int maxTextDx = std::max(0, page.dx - iconDx - gap);
    int textDx = std::min(textSz.dx, maxTextDx);
    out.blockDx = iconDx + (iconDx > 0 ? gap : 0) + textDx;
    out.blockX = page.x + (page.dx - out.blockDx) / 2;
    out.padLeft = out.blockX - page.x;
    out.padRight = (page.x + page.dx) - (out.blockX + out.blockDx);

    int iconY = rcBand.y + (rcBand.dy - iconDy) / 2;
    if (isRtl) {
        out.rcText = {out.blockX, rcBand.y, textDx, rcBand.dy};
        out.rcIcon = {out.blockX + textDx + (iconDx > 0 ? gap : 0), iconY, iconDx, iconDy};
    } else {
        out.rcIcon = {out.blockX, iconY, iconDx, iconDy};
        out.rcText = {out.blockX + iconDx + (iconDx > 0 ? gap : 0), rcBand.y, textDx, rcBand.dy};
    }
    return out;
}

static void DrawHomePageThumbLabel(HWND hwnd, HDC hdc, const Rect& page, const Rect& rcBand, FileState* fs,
                                   const char* fileName, HFONT font, StrVec& filterWords, Vec<u8>& highlighted,
                                   bool isRtl, COLORREF backgroundColor) {
    HomePageThumbLabelLayout lay = LayoutHomePageThumbLabel(hwnd, hdc, page, rcBand, fs, fileName, font, isRtl);
    SelectObject(hdc, font);
    UINT fmt = DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX | (isRtl ? DT_RIGHT : DT_LEFT);
    RECT rcTextWin = {lay.rcText.x, lay.rcText.y, lay.rcText.x + lay.rcText.dx, lay.rcText.y + lay.rcText.dy};
    DrawMaybeHighlightedTextArgs hlArgs(filterWords, highlighted);
    hlArgs.hdc = hdc;
    hlArgs.rc = rcTextWin;
    hlArgs.text = fileName;
    hlArgs.colBg = backgroundColor;
    hlArgs.isRtl = isRtl;
    hlArgs.drawFmt = fmt;
    DrawMaybeHighlightedText(hlArgs);

    if (fs->himl) {
        ImageList_Draw(fs->himl, fs->iconIdx, hdc, lay.rcIcon.x, lay.rcIcon.y, ILD_TRANSPARENT);
    }
}

static bool HomePageUsesListView() {
    return gGlobalPrefs && str::EqI(gGlobalPrefs->homePageViewMode, "list");
}

struct ThumbnailLayout {
    Rect rcPage;
    Size szThumb;
    Rect rcText;
    Rect rcListRow;
    Rect rcListThumb;
    Rect rcListFileName;
    Rect rcListPath;
    Rect rcListSize;
    Rect rcListRemove;
    Rect rcListPin;
    FileState* fs = nullptr; // info needed to draw the thumbnail
    StaticLink* sl = nullptr;
};

struct HomePageLayout {
    // args in
    HWND hwnd = nullptr;
    HDC hdc = nullptr;
    Rect rc;
    MainWindow* win = nullptr;

    Rect rcAppWithVer; // SumatraPDF colorful text + version
    Rect rcLine;       // line under bApp
    Rect rcIconView;
    Rect rcIconSort;

    HIMAGELIST himlOpen = nullptr;
    VirtWndText* hideShowFreqRead = nullptr;
    Vec<ThumbnailLayout> thumbnails; // info for each thumbnail
    Vec<FileState*> fileStates;      // filtered list, not owned
    int totalContentDy = 0;          // total height of all thumbnail rows
    int thumbsVisibleDy = 0;         // visible height for thumbnails area
    int thumbsStartX = 0;
    int thumbsTopY = 0; // y of row 0 before scroll offset
    int thumbsCols = 0;
    int thumbDx = kThumbnailDx;
    int thumbDy = kThumbnailDy;
    int thumbGapX = 0;
    Rect rcThumbsArea; // clip rect for thumbnails

    // search filter
    StrVec filterWords;
    Vec<u8> highlighted;
    Rect rcSearchBorder; // border rect drawn around the edit control

    // tip layout
    Rect rcTip;               // background rect for tip area
    ParsedTip* tip = nullptr; // points to gParsedTips or gParsedPromos, not owned

    ~HomePageLayout();
};

HomePageLayout::~HomePageLayout() {}

constexpr int kThumbsMiddleMargin = 32;
constexpr int kSearchChromeDy = 44;
constexpr int kHeaderSearchGapY = 10;
constexpr int kSearchThumbnailsGapY = 16;

static WNDPROC DefWndProcHomeSearch = nullptr;

static LRESULT CALLBACK WndProcHomeSearch(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) {
        HwndSetText(hwnd, "");
        MainWindow* win = FindMainWindowByHwnd(GetParent(hwnd));
        if (win) {
            HomePageApplySearchFilter(win);
            HwndSetFocus(win->hwndCanvas);
        }
        return 0;
    }
    if (msg == WM_MOUSEWHEEL) {
        HWND parent = GetParent(hwnd);
        return SendMessageW(parent, msg, wp, lp);
    }
    return CallWindowProcW(DefWndProcHomeSearch, hwnd, msg, wp, lp);
}

static void EnsureHomeSearchCreated(MainWindow* win) {
    if (win->hwndHomeSearch) {
        return;
    }
    HMODULE hmod = GetModuleHandleW(nullptr);
    DWORD style = WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL;
    DWORD exStyle = 0;
    win->hwndHomeSearch = CreateWindowExW(exStyle, WC_EDITW, L"", style, 0, 0, 100, DpiScale(win->hwndCanvas, 24),
                                          win->hwndCanvas, nullptr, hmod, nullptr);
    SetWindowFont(win->hwndHomeSearch, HomePageSearchFont(win->hwndCanvas), TRUE);
    if (!DefWndProcHomeSearch) {
        DefWndProcHomeSearch = (WNDPROC)GetWindowLongPtr(win->hwndHomeSearch, GWLP_WNDPROC);
    }
    SetWindowLongPtr(win->hwndHomeSearch, GWLP_WNDPROC, (LONG_PTR)WndProcHomeSearch);
    TempWStr searchCue = ToWStrTemp(_TRA("search files (Ctrl + F)"));
    Edit_SetCueBannerText(win->hwndHomeSearch, searchCue);
    // add left/right padding so text doesn't overlap the border
    int margin = DpiScale(win->hwndCanvas, 6);
    SendMessage(win->hwndHomeSearch, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(margin, margin));
}

bool HomePageApplySearchFont(MainWindow* win) {
    if (!win || !win->hwndHomeSearch) {
        return false;
    }
    HWND hwnd = win->hwndCanvas ? win->hwndCanvas : win->hwndFrame;
    HFONT font = HomePageSearchFont(hwnd);
    if (HwndGetFont(win->hwndHomeSearch) == font) {
        return false;
    }
    HwndSetFont(win->hwndHomeSearch, font);
    return true;
}

COLORREF HomePageSearchTextColor() {
    return HomePageInkColor();
}

void HomePageDestroySearch(MainWindow* win) {
    if (win->hwndHomeSearch) {
        DestroyWindow(win->hwndHomeSearch);
        win->hwndHomeSearch = nullptr;
    }
}

void HomePageOnLanguageChangedAll() {
    ResetTipsParsed();
    if (gHwndAbout) {
        HwndSetText(gHwndAbout, _TRA("About Sumatra PDF Plus"));
        InvalidateRect(gHwndAbout, nullptr, TRUE);
    }
    for (MainWindow* win : gWindows) {
        if (win->hwndHomeSearch) {
            TempWStr cue = ToWStrTemp(_TRA("search files (Ctrl + F)"));
            Edit_SetCueBannerText(win->hwndHomeSearch, cue);
        }
        HomePageInvalidateScrollCache(win);
        WindowTab* tab = win->CurrentTab();
        if (tab && tab->IsAboutTab()) {
            win->RedrawAll(true);
        }
    }
}

void HomePageFocusSearch(MainWindow* win) {
    EnsureHomeSearchCreated(win);
    ShowWindow(win->hwndHomeSearch, SW_SHOW);
    HwndSetFocus(win->hwndHomeSearch);
}

constexpr UINT kHomeSearchDebounceMs = 120;

void HomePageScheduleSearchFilter(MainWindow* win) {
    if (!win->hwndFrame) {
        return;
    }
    KillTimer(win->hwndFrame, kHomeSearchDebounceTimerId);
    SetTimer(win->hwndFrame, kHomeSearchDebounceTimerId, kHomeSearchDebounceMs, nullptr);
}

void HomePageApplySearchFilter(MainWindow* win) {
    win->homePageScrollY = 0;
    win->homePageScrollTargetY = 0;
    HomePageInvalidateScrollCache(win);
    InvalidateRect(win->hwndCanvas, nullptr, FALSE);
}

void HomePageRemoveMissingFiles(MainWindow* win) {
    Vec<char*> doomed;
    size_t i = 0;
    FileState* fs;
    while ((fs = gFileHistory.Get(i)) != nullptr) {
        i++;
        if (!fs->filePath) {
            continue;
        }
        if (!path::IsOnFixedDrive(fs->filePath)) {
            continue;
        }
        if (DocumentPathExists(fs->filePath)) {
            continue;
        }
        doomed.Append(str::Dup(fs->filePath));
    }
    if (doomed.Size() == 0) {
        return;
    }
    for (char* path : doomed) {
        fs = gFileHistory.FindByPath(path);
        if (fs) {
            fs->isPinned = false;
            if (!fs->favorites->IsEmpty()) {
                gFileHistory.MarkFileInexistent(fs->filePath, true);
            } else {
                gFileHistory.Remove(fs);
                DeleteFileState(fs);
            }
        }
        DeleteThumbnailForFile(path);
        str::Free(path);
    }
    SaveSettings();
    for (MainWindow* w : gWindows) {
        if (!w || !w->IsCurrentTabAbout()) {
            continue;
        }
        w->DeleteToolTip();
        HomePageInvalidateScrollCache(w);
        w->RedrawAll(true);
    }
}

void PickAnotherRandomPromotion() {
    PickAnotherRandomTip();
}

static TempStr FileSizeForHomeListTemp(const char* path) {
    if (!path || !path[0] || (path[0] == '\\' && path[1] == '\\')) {
        return str::DupTemp("");
    }
    WCHAR* ws = ToWStrTemp(path);
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExW(ws, GetFileExInfoStandard, &info)) {
        return str::DupTemp("");
    }
    ULARGE_INTEGER ul{};
    ul.LowPart = info.nFileSizeLow;
    ul.HighPart = info.nFileSizeHigh;
    return str::FormatSizeShortTemp((i64)ul.QuadPart, nullptr);
}

static int HomePageThumbActionDx(HWND hwnd) {
    return DpiScale(hwnd, 18);
}

static int HomePageThumbActionInset(HWND hwnd) {
    return DpiScale(hwnd, 3);
}

static int HomePageThumbActionGap(HWND hwnd) {
    return DpiScale(hwnd, 1);
}

static int HomePageListActionPad(HWND hwnd) {
    return DpiScale(hwnd, 6);
}

static int HomePageListActionPillDx(HWND hwnd) {
    return HomePageThumbActionInset(hwnd) * 2 + HomePageThumbActionDx(hwnd) * 2 + HomePageThumbActionGap(hwnd);
}

static void LayoutHomeListActions(HWND hwnd, const Rect& row, bool isRtl, Rect& rcRemove, Rect& rcPin) {
    int slot = HomePageThumbActionDx(hwnd);
    int inset = HomePageThumbActionInset(hwnd);
    int gap = HomePageThumbActionGap(hwnd);
    int pad = HomePageListActionPad(hwnd);
    int pillDx = HomePageListActionPillDx(hwnd);
    int pillDy = inset * 2 + slot;
    int pillY = row.y + (row.dy - pillDy) / 2;
    int pillX = isRtl ? row.x + pad : row.x + row.dx - pad - pillDx;
    int slotY = pillY + inset;
    if (isRtl) {
        rcPin = {pillX + inset, slotY, slot, slot};
        rcRemove = {rcPin.x + slot + gap, slotY, slot, slot};
    } else {
        rcRemove = {pillX + inset, slotY, slot, slot};
        rcPin = {rcRemove.x + slot + gap, slotY, slot, slot};
    }
}

static void LayoutHomeListItem(HWND hwnd, HDC hdc, const Rect& row, FileState* fs, bool isRtl, ThumbnailLayout& item) {
    int gapDx = HomePageListGapDx(hwnd);
    int thumbDx = HomePageListThumbDx(hwnd);
    int thumbDy = HomePageListThumbDy(hwnd);
    HFONT fontName = HomePageFileNameFont(hwnd);
    HFONT fontMeta = HomePageMetaFont(hwnd);
    TempStr fileName = path::GetBaseNameTemp(fs->filePath);
    TempStr fileSize = FileSizeForHomeListTemp(fs->filePath);
    int nameDy = HdcMeasureText(hdc, fileName, fontName).dy;
    int sizeDx = HdcMeasureText(hdc, fileSize, fontMeta).dx;
    int metaDy = HomePageFontLineDy(hwnd, fontMeta);
    if (nameDy <= 0) {
        nameDy = HomePageFontLineDy(hwnd, fontName);
    }

    Rect rcRemove;
    Rect rcPin;
    LayoutHomeListActions(hwnd, row, isRtl, rcRemove, rcPin);
    int pad = HomePageListActionPad(hwnd);
    int pillDx = HomePageListActionPillDx(hwnd);
    int pillX = isRtl ? row.x + pad : row.x + row.dx - pad - pillDx;

    int sizeGap = DpiScale(hwnd, 10);
    Rect rcSize;
    if (sizeDx > 0) {
        if (isRtl) {
            rcSize = {pillX + pillDx + sizeGap, row.y, sizeDx, row.dy};
        } else {
            rcSize = {pillX - sizeGap - sizeDx, row.y, sizeDx, row.dy};
        }
    }

    Rect rcThumb(row.x, row.y + (row.dy - thumbDy) / 2, thumbDx, thumbDy);
    int textX = rcThumb.x + rcThumb.dx + gapDx;
    int textRight = sizeDx > 0 ? rcSize.x : pillX;
    int textDx = textRight - gapDx - textX;
    if (isRtl) {
        rcThumb.x = row.x + row.dx - rcThumb.dx;
        textX = (sizeDx > 0 ? rcSize.x + rcSize.dx : pillX + pillDx) + gapDx;
        textDx = rcThumb.x - gapDx - textX;
    }
    if (textDx < 0) {
        textDx = 0;
    }

    int lineGap = DpiScale(hwnd, 2);
    int textBlockDy = nameDy + lineGap + metaDy;
    int textY = row.y + (row.dy - textBlockDy) / 2;
    Rect rcFileName(textX, textY, textDx, nameDy);
    Rect rcPath(textX, textY + nameDy + lineGap, textDx, metaDy);

    item.fs = fs;
    item.rcPage = row;
    item.rcListRow = row;
    item.rcListThumb = rcThumb;
    item.rcListFileName = rcFileName;
    item.rcListPath = rcPath;
    item.rcListSize = rcSize;
    item.rcListRemove = rcRemove;
    item.rcListPin = rcPin;
}

static void LayoutHomeThumbActions(HWND hwnd, const Rect& rcPage, bool isRtl, Rect& rcRemove, Rect& rcPin) {
    rcRemove = {};
    rcPin = {};
    if (rcPage.IsEmpty()) {
        return;
    }
    int slot = HomePageThumbActionDx(hwnd);
    int inset = HomePageThumbActionInset(hwnd);
    int gap = HomePageThumbActionGap(hwnd);
    int pad = DpiScale(hwnd, 5);
    int pillDx = inset * 2 + slot * 2 + gap;
    int pillX = isRtl ? rcPage.x + pad : rcPage.x + rcPage.dx - pad - pillDx;
    int pillY = rcPage.y + pad;
    int slotY = pillY + inset;
    if (isRtl) {
        rcPin = {pillX + inset, slotY, slot, slot};
        rcRemove = {rcPin.x + slot + gap, slotY, slot, slot};
    } else {
        rcRemove = {pillX + inset, slotY, slot, slot};
        rcPin = {rcRemove.x + slot + gap, slotY, slot, slot};
    }
}

static const char* HomePageFilePathFromLink(const char* target) {
    if (!target || !*target) {
        return nullptr;
    }
    if (str::StartsWith(target, kLinkHomePageRemoveFile)) {
        return target + str::Len(kLinkHomePageRemoveFile);
    }
    if (str::StartsWith(target, kLinkHomePagePinFile)) {
        return target + str::Len(kLinkHomePagePinFile);
    }
    if (*target == '<' || str::StartsWith(target, "http://") || str::StartsWith(target, "https://")) {
        return nullptr;
    }
    return target;
}

static bool HomePageThumbIsHovered(MainWindow* win, FileState* fs) {
    return win && fs && win->homePageHoverPath && str::Eq(win->homePageHoverPath, fs->filePath);
}

static Gdiplus::Color HomeThumbGdip(COLORREF col, BYTE alpha = 255) {
    return Gdiplus::Color(alpha, GetRValue(col), GetGValue(col), GetBValue(col));
}

// Idle X / pin: mid gray. Pinned pin: near-black (near-white in dark chrome).
constexpr BYTE kHomeThumbActionIconAlpha = 178;

static COLORREF HomeThumbActionIconColor(bool dark) {
    return dark ? RGB(214, 214, 220) : RGB(72, 72, 78);
}

static COLORREF HomeThumbPinColor(bool dark, bool pinned) {
    if (!pinned) {
        return HomeThumbActionIconColor(dark);
    }
    return dark ? RGB(248, 248, 250) : RGB(28, 28, 32);
}

static void HomeThumbRoundRectPath(Gdiplus::GraphicsPath& path, Gdiplus::REAL x, Gdiplus::REAL y, Gdiplus::REAL w,
                                   Gdiplus::REAL h, Gdiplus::REAL r) {
    if (r * 2.f > w) {
        r = w * 0.5f;
    }
    if (r * 2.f > h) {
        r = h * 0.5f;
    }
    Gdiplus::REAL d = r * 2.f;
    path.AddArc(x, y, d, d, 180.f, 90.f);
    path.AddArc(x + w - d, y, d, d, 270.f, 90.f);
    path.AddArc(x + w - d, y + h - d, d, d, 0.f, 90.f);
    path.AddArc(x, y + h - d, d, d, 90.f, 90.f);
    path.CloseFigure();
}

static void DrawHomeThumbFrosted(Gdiplus::Graphics& g, const Gdiplus::GraphicsPath& path, bool dark, bool onPage) {
    Gdiplus::GraphicsPath* shadow = path.Clone();
    if (shadow) {
        Gdiplus::Matrix shift;
        shift.Translate(0.f, 1.f);
        shadow->Transform(&shift);
        Gdiplus::SolidBrush sh(HomeThumbGdip(RGB(0, 0, 0), dark ? 48 : (onPage ? 14 : 28)));
        g.FillPath(&sh, shadow);
        delete shadow;
    }

    COLORREF fill;
    BYTE alpha;
    if (onPage) {
        fill = AccentColor(ThemeMainWindowBackgroundColor(), dark ? -12 : 10);
        alpha = 255;
    } else if (dark) {
        fill = RGB(32, 32, 36);
        alpha = 214;
    } else {
        fill = RGB(255, 255, 255);
        alpha = 232;
    }
    Gdiplus::SolidBrush br(HomeThumbGdip(fill, alpha));
    g.FillPath(&br, &path);

    Gdiplus::Pen ring(HomeThumbGdip(dark ? RGB(255, 255, 255) : RGB(20, 20, 24), dark ? 36 : (onPage ? 16 : 24)), 1.0f);
    g.DrawPath(&ring, &path);
}

static void DrawHomeThumbCloseIcon(Gdiplus::Graphics& g, const Rect& rc, COLORREF col) {
    Gdiplus::REAL d = (Gdiplus::REAL)std::min(rc.dx, rc.dy);
    Gdiplus::REAL x = (Gdiplus::REAL)rc.x;
    Gdiplus::REAL y = (Gdiplus::REAL)rc.y;
    Gdiplus::REAL inset = d * 0.32f;
    Gdiplus::Pen pen(HomeThumbGdip(col, 240), std::max(1.25f, d * 0.075f));
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    g.DrawLine(&pen, x + inset, y + inset, x + d - inset, y + d - inset);
    g.DrawLine(&pen, x + d - inset, y + inset, x + inset, y + d - inset);
}

// Filled pushpin (viewBox 0 0 1024 1024), even-odd hole in the head.
// clang-format off
static const Gdiplus::PointF kHomePinOuter[] = {
    {807.42f, 370.24f}, {811.00f, 374.15f}, {814.10f, 378.26f}, {816.72f, 382.56f},
    {818.86f, 387.05f}, {820.52f, 391.74f}, {821.71f, 396.62f}, {822.42f, 401.70f},
    {822.66f, 406.98f}, {822.42f, 412.25f}, {821.70f, 417.33f}, {820.50f, 422.21f},
    {818.83f, 426.90f}, {816.69f, 431.38f}, {814.07f, 435.67f}, {810.98f, 439.76f},
    {807.42f, 443.65f}, {688.00f, 563.20f}, {674.37f, 576.77f}, {676.48f, 595.90f},
    {676.00f, 665.00f}, {659.20f, 732.03f}, {654.49f, 742.12f}, {647.77f, 750.99f},
    {639.33f, 758.25f}, {629.55f, 763.57f}, {618.88f, 766.72f}, {607.82f, 768.29f},
    {596.67f, 767.48f}, {585.95f, 764.33f}, {576.14f, 758.98f}, {567.68f, 751.68f},
    {437.95f, 621.95f}, {237.25f, 822.66f}, {192.00f, 777.34f}, {392.70f, 576.64f},
    {263.04f, 446.85f}, {255.74f, 438.39f}, {250.39f, 428.58f}, {247.24f, 417.86f},
    {246.43f, 406.71f}, {248.00f, 395.65f}, {251.15f, 384.97f}, {256.47f, 375.20f},
    {263.73f, 366.76f}, {272.60f, 360.04f}, {282.69f, 355.33f}, {291.52f, 352.13f},
    {354.41f, 337.97f}, {418.88f, 338.11f}, {438.02f, 340.22f}, {570.88f, 207.23f},
    {578.70f, 200.65f}, {587.70f, 195.80f}, {597.49f, 192.87f}, {607.68f, 192.00f},
    {612.96f, 192.24f}, {618.04f, 192.96f}, {622.92f, 194.15f}, {627.60f, 195.82f},
    {632.08f, 197.97f}, {636.37f, 200.59f}, {640.46f, 203.67f}, {644.35f, 207.23f},
};
static const Gdiplus::PointF kHomePinHole[] = {
    {607.68f, 260.99f}, {461.44f, 407.23f}, {411.84f, 401.73f}, {364.21f, 401.33f},
    {317.57f, 410.94f}, {603.58f, 697.09f}, {605.63f, 690.18f}, {608.39f, 679.49f},
    {610.61f, 668.73f}, {612.30f, 657.91f}, {613.46f, 647.03f}, {614.09f, 636.09f},
    {614.19f, 625.08f}, {613.76f, 614.01f}, {612.80f, 602.88f}, {607.30f, 553.28f},
    {642.69f, 517.95f}, {753.66f, 406.98f},
};
// clang-format on

static void DrawHomeThumbPinIcon(Gdiplus::Graphics& g, const Rect& rc, COLORREF col, bool pinned) {
    Gdiplus::REAL d = (Gdiplus::REAL)std::min(rc.dx, rc.dy);
    if (d < 4.f) {
        return;
    }
    Gdiplus::REAL pad = d * 0.10f;
    Gdiplus::REAL s = (d - pad * 2.f) / 1024.f;
    Gdiplus::Matrix xf;
    xf.Translate((Gdiplus::REAL)rc.x + pad, (Gdiplus::REAL)rc.y + pad);
    xf.Scale(s, s);

    Gdiplus::GraphicsPath path(Gdiplus::FillModeAlternate);
    path.AddPolygon(kHomePinOuter, dimof(kHomePinOuter));
    path.AddPolygon(kHomePinHole, dimof(kHomePinHole));
    path.Transform(&xf);

    BYTE alpha = pinned ? (BYTE)255 : kHomeThumbActionIconAlpha;
    Gdiplus::SolidBrush br(HomeThumbGdip(col, alpha));
    g.FillPath(&br, &path);
}

static void DrawHomeThumbActions(HDC hdc, HWND hwnd, FileState* fs, const Rect& rcRemove, const Rect& rcPin,
                                 bool hovered, bool onPage) {
    if (!fs) {
        return;
    }
    bool showRemove = hovered;
    bool showPin = hovered || fs->isPinned;
    if (!showRemove && !showPin) {
        return;
    }

    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

    bool dark = ThemeUsesDarkChrome();
    COLORREF iconCol = HomeThumbActionIconColor(dark);
    COLORREF pinCol = HomeThumbPinColor(dark, fs->isPinned);
    int inset = HomePageThumbActionInset(hwnd);

    if (hovered) {
        Rect pill = rcRemove.Union(rcPin);
        pill.Inflate(inset, inset);
        Gdiplus::GraphicsPath path;
        HomeThumbRoundRectPath(path, (Gdiplus::REAL)pill.x + 0.5f, (Gdiplus::REAL)pill.y + 0.5f,
                               (Gdiplus::REAL)pill.dx - 1.f, (Gdiplus::REAL)pill.dy - 1.f,
                               (Gdiplus::REAL)pill.dy * 0.5f);
        DrawHomeThumbFrosted(g, path, dark, onPage);

        Gdiplus::REAL leftEdge = (Gdiplus::REAL)std::min(rcRemove.x + rcRemove.dx, rcPin.x + rcPin.dx);
        Gdiplus::REAL rightEdge = (Gdiplus::REAL)std::max(rcRemove.x, rcPin.x);
        Gdiplus::REAL midX = (leftEdge + rightEdge) * 0.5f;
        Gdiplus::REAL y1 = (Gdiplus::REAL)pill.y + inset + 3.f;
        Gdiplus::REAL y2 = (Gdiplus::REAL)(pill.y + pill.dy) - inset - 3.f;
        Gdiplus::Pen div(HomeThumbGdip(dark ? RGB(255, 255, 255) : RGB(20, 20, 24), dark ? 32 : 22), 1.0f);
        g.DrawLine(&div, midX, y1, midX, y2);

        DrawHomeThumbCloseIcon(g, rcRemove, iconCol);
        DrawHomeThumbPinIcon(g, rcPin, pinCol, fs->isPinned);
        return;
    }

    Gdiplus::GraphicsPath path;
    HomeThumbRoundRectPath(path, (Gdiplus::REAL)rcPin.x + 0.5f, (Gdiplus::REAL)rcPin.y + 0.5f,
                           (Gdiplus::REAL)rcPin.dx - 1.f, (Gdiplus::REAL)rcPin.dy - 1.f,
                           (Gdiplus::REAL)rcPin.dy * 0.5f);
    DrawHomeThumbFrosted(g, path, dark, onPage);
    DrawHomeThumbPinIcon(g, rcPin, pinCol, true);
}

static void AppendHomeThumbActionLinks(MainWindow* win, FileState* fs, const Rect& rcRemove, const Rect& rcPin,
                                       const Rect& clip) {
    if (!fs || !fs->filePath) {
        return;
    }
    Rect removeRect = rcRemove.Intersect(clip);
    if (!removeRect.IsEmpty()) {
        TempStr target = str::JoinTemp(kLinkHomePageRemoveFile, fs->filePath);
        win->staticLinks.Append(new StaticLink(removeRect, target, _TRA("&Remove From History")));
    }
    Rect pinRect = rcPin.Intersect(clip);
    if (!pinRect.IsEmpty()) {
        TempStr target = str::JoinTemp(kLinkHomePagePinFile, fs->filePath);
        const char* tooltip = fs->isPinned ? "Unpin" : _TRA("&Pin Document");
        win->staticLinks.Append(new StaticLink(pinRect, target, tooltip));
    }
}

static void AppendHomeListItemLinks(MainWindow* win, ThumbnailLayout& item, const Rect& clip) {
    Rect removeRect = item.rcListRemove.Intersect(clip);
    if (!removeRect.IsEmpty()) {
        TempStr target = str::JoinTemp(kLinkHomePageRemoveFile, item.fs->filePath);
        win->staticLinks.Append(new StaticLink(removeRect, target, _TRA("&Remove From History")));
    }
    Rect pinRect = item.rcListPin.Intersect(clip);
    if (!pinRect.IsEmpty()) {
        TempStr target = str::JoinTemp(kLinkHomePagePinFile, item.fs->filePath);
        const char* tooltip = item.fs->isPinned ? "Unpin" : _TRA("&Pin Document");
        win->staticLinks.Append(new StaticLink(pinRect, target, tooltip));
    }
    Rect rowRect = item.rcListRow.Intersect(clip);
    if (!rowRect.IsEmpty()) {
        item.sl = new StaticLink(rowRect, item.fs->filePath, item.fs->filePath);
        win->staticLinks.Append(item.sl);
    }
}

// Only touch files that can appear on screen (plus one row of overscan).
// Painting used to LoadThumbnail/GetSize every history entry (up to 1000).
static void HomePageVisibleIndexRange(int nFiles, int scrollY, int visibleDy, int rowDy, int cols, int* firstOut,
                                      int* lastOut) {
    *firstOut = 0;
    *lastOut = nFiles - 1;
    if (nFiles < 1) {
        *lastOut = -1;
        return;
    }
    if (rowDy < 1 || visibleDy < 1) {
        *lastOut = -1;
        return;
    }
    if (cols < 1) {
        cols = 1;
    }
    int firstRow = scrollY / rowDy - 1;
    if (firstRow < 0) {
        firstRow = 0;
    }
    int lastRow = (scrollY + visibleDy) / rowDy + 1;
    int first = firstRow * cols;
    int last = (lastRow + 1) * cols - 1;
    if (first >= nFiles) {
        *firstOut = 0;
        *lastOut = -1;
        return;
    }
    if (last >= nFiles) {
        last = nFiles - 1;
    }
    *firstOut = first;
    *lastOut = last;
}

void LayoutHomePage(HomePageLayout& l) {
    EnsureTipsParsed();

    Vec<FileState*> allFileStates;
    if (gGlobalPrefs->homePageSortByFrequentlyRead) {
        gFileHistory.GetFrequencyOrder(allFileStates);
    } else {
        gFileHistory.GetRecentlyOpenedOrder(allFileStates);
    }
    auto hwnd = l.hwnd;
    auto hdc = l.hdc;
    auto rc = l.rc;
    auto win = l.win;
    HWND dpiHwnd = win->hwndCanvas ? win->hwndCanvas : hwnd;

    // filter by search query if present
    TempStr searchQuery = nullptr;
    if (win->hwndHomeSearch) {
        searchQuery = HwndGetTextTemp(win->hwndHomeSearch);
    }
    bool hasFilter = searchQuery && searchQuery[0];
    if (hasFilter) {
        SplitFilterToWords(searchQuery, l.filterWords);
    }
    bool listView = HomePageUsesListView();
    Vec<FileState*> fileStates;
    for (int i = 0; i < allFileStates.Size(); i++) {
        FileState* fs = allFileStates.at(i);
        if (hasFilter) {
            TempStr baseName = path::GetBaseNameTemp(fs->filePath);
            if (!FilterMatches(baseName, l.filterWords) && !(listView && FilterMatches(fs->filePath, l.filterWords))) {
                continue;
            }
        }
        fileStates.Append(fs);
    }
    l.fileStates = fileStates;

    bool isRtl = IsUIRtl();

    Size sz = CalcSumatraVersionSize(hdc);
    {
        Rect& r = l.rcAppWithVer;
        r.x = rc.dx - sz.dx - 3;
        r.y = 0;
        r.SetSize(sz);
    }

    l.rcLine = {0, sz.dy, rc.dx, 0};

    // Header (icons + search) always follows the thumbnail grid, so switching
    // list/thumbnails does not move the buttons.
    int headerCols = 1;
    int headerThumbDx = kThumbnailDx;
    int headerThumbDy = kThumbnailDy;
    int headerGapX = kThumbsSpaceBetweenX;
    int headerStartX = kThumbsMarginLeft;
    ComputeHomeThumbGrid(dpiHwnd, rc.dx, headerCols, headerThumbDx, headerThumbDy, headerStartX, headerGapX);
    int headerContentWidth = headerCols * headerThumbDx + (headerCols - 1) * headerGapX;

    // File list / thumbnail cards may use a different start X; do not feed that
    // back into the header.
    int nFilesForLayout = allFileStates.Size();
    int thumbsColsForLayout = 1;
    int thumbDx = kThumbnailDx;
    int thumbDy = kThumbnailDy;
    int thumbGapX = kThumbsSpaceBetweenX;
    int thumbsStartX = kThumbsMarginLeft;
    if (listView) {
        int colsForLayout = (rc.dx - kThumbsMarginLeft - kThumbsMarginRight + kThumbsSpaceBetweenX) /
                            (kThumbnailDx + kThumbsSpaceBetweenX);
        thumbsColsForLayout = std::max(colsForLayout, 1);
        thumbsStartX = rc.x + kThumbsMarginLeft +
                       (rc.dx - thumbsColsForLayout * kThumbnailDx - (thumbsColsForLayout - 1) * kThumbsSpaceBetweenX -
                        kThumbsMarginLeft - kThumbsMarginRight) /
                           2;
        if (thumbsStartX < DpiScale(dpiHwnd, kInnerPadding)) {
            thumbsStartX = DpiScale(dpiHwnd, kInnerPadding);
        } else if (nFilesForLayout == 0) {
            thumbsStartX = kThumbsMarginLeft;
        }
    } else {
        thumbsColsForLayout = headerCols;
        thumbDx = headerThumbDx;
        thumbDy = headerThumbDy;
        thumbGapX = headerGapX;
        thumbsStartX = headerStartX;
        if (nFilesForLayout == 0) {
            thumbsStartX = kThumbsMarginLeft;
        }
    }
    l.thumbDx = thumbDx;
    l.thumbDy = thumbDy;
    l.thumbGapX = thumbGapX;
    win->homePageThumbDx = thumbDx;
    win->homePageThumbDy = thumbDy;

    // --- Step 1: two icon toggles (view + sort) ---
    l.himlOpen = (HIMAGELIST)SendMessageW(win->hwndToolbar, TB_GETIMAGELIST, 0, 0);
    Rect rcIconSz(0, 0, 0, 0);
    ImageList_GetIconSize(l.himlOpen, &rcIconSz.dx, &rcIconSz.dy);

    int hdrY = DpiScale(dpiHwnd, 14);
    int iconPad = DpiScale(dpiHwnd, 2);
    int cellDx = rcIconSz.dx + iconPad * 2;
    int cellDy = rcIconSz.dy + iconPad * 2;
    int headerRowDy = cellDy;
    int clusterGap = DpiScale(dpiHwnd, 4);

    l.rcIconView = {headerStartX, hdrY, cellDx, headerRowDy};
    l.rcIconSort = {l.rcIconView.x + cellDx + clusterGap, hdrY, cellDx, headerRowDy};
    if (isRtl) {
        l.rcIconView.x = rc.dx - headerStartX - cellDx;
        l.rcIconSort.x = l.rcIconView.x - clusterGap - cellDx;
    }
    bool frequent = gGlobalPrefs && gGlobalPrefs->homePageSortByFrequentlyRead;
    win->staticLinks.Append(
        new StaticLink(l.rcIconView, kLinkHomePageToggleView, listView ? _TRA("List view") : _TRA("Thumbnail view")));
    win->staticLinks.Append(new StaticLink(l.rcIconSort, kLinkHomePageToggleSort,
                                           frequent ? _TRA("Frequently Read") : _TRA("Recently Opened")));

    int headerBottomY = hdrY + headerRowDy;

    // --- Position search edit below header ---
    EnsureHomeSearchCreated(win);
    HomePageApplySearchFont(win);
    int chromeDy = DpiScale(dpiHwnd, kSearchChromeDy);
    int headerSearchGap = DpiScale(dpiHwnd, kHeaderSearchGapY);
    int searchThumbsGap = DpiScale(dpiHwnd, kSearchThumbnailsGapY);
    int searchPadX = DpiScale(dpiHwnd, 12);
    int searchEditDy = HomePageFontLineDy(win->hwndCanvas, HomePageSearchFont(win->hwndCanvas));
    int extra = chromeDy - searchEditDy;
    int minExtra = DpiScale(dpiHwnd, 10);
    if (extra < minExtra) {
        extra = minExtra;
        chromeDy = searchEditDy + extra;
    }
    int searchPadY = extra / 2;
    {
        int borderDx = headerContentWidth;
        if (borderDx < DpiScale(dpiHwnd, 200)) {
            borderDx = DpiScale(dpiHwnd, 200);
        }
        int borderX = headerStartX;
        int borderY = headerBottomY + headerSearchGap;
        int borderDy = searchEditDy + 2 * searchPadY;
        l.rcSearchBorder = {borderX, borderY, borderDx, borderDy};
        MoveWindow(win->hwndHomeSearch, borderX + searchPadX, borderY + searchPadY, borderDx - 2 * searchPadX,
                   searchEditDy, TRUE);
    }
    int searchAreaDy = headerSearchGap + searchEditDy + 2 * searchPadY + searchThumbsGap;
    headerBottomY += searchAreaDy;

    // --- Step 2: calculate tip area at the bottom (before thumbnails) ---
    int tipHeight = 0;
    HFONT fontTip = GetAppFontForHwnd(dpiHwnd);
    ParsedTip* tip = nullptr;
    if (gGlobalPrefs->showTips && gSelectedTipIdx >= 0) {
        if (gSelectedIsPromo && gSelectedTipIdx < gParsedPromoCount) {
            tip = &gParsedPromos[gSelectedTipIdx];
        } else if (!gSelectedIsPromo && gSelectedTipIdx < gParsedTipCount) {
            tip = &gParsedTips[gSelectedTipIdx];
        }
    }
    if (tip) {
        MeasureTipWords(*tip, hdc, fontTip);
        int tipPadding = DpiScale(dpiHwnd, 8);
        // do a preliminary layout to get the height (use thumbnails content width)
        int tipTextWidth = thumbsColsForLayout * thumbDx + (thumbsColsForLayout - 1) * thumbGapX;
        LayoutTip(*tip, tipTextWidth, 0, 0);
        tipHeight = tip->totalDy + 2 * tipPadding;
    }

    // --- Step 3: middle area for thumbnails ---
    // thumbnails start directly after headerBottomY (which includes kSearchThumbnailsGapY)
    int thumbsTopY = headerBottomY;
    l.thumbsStartX = thumbsStartX;
    l.thumbsTopY = thumbsTopY;
    l.thumbsCols = thumbsColsForLayout;
    int thumbsBottomY = rc.dy - tipHeight - kThumbsMiddleMargin;
    int thumbsVisibleDy = std::max(0, thumbsBottomY - thumbsTopY);

    l.rcThumbsArea = {0, thumbsTopY, rc.dx, thumbsVisibleDy};

    int nFiles = fileStates.Size();
    int contentDy = 0;
    int rowDy = 0;
    int listContentWidth = thumbsColsForLayout * thumbDx + (thumbsColsForLayout - 1) * thumbGapX;
    if (listView) {
        rowDy = HomePageListRowDy(dpiHwnd) + HomePageListRowSpacing(dpiHwnd);
        contentDy = nFiles > 0 ? nFiles * rowDy - HomePageListRowSpacing(dpiHwnd) : 0;
        l.thumbsCols = 1;

        int scrollY = win->homePageScrollY;
        int maxScrollY = std::max(0, contentDy - thumbsVisibleDy);
        win->homePageMaxScrollY = maxScrollY;
        win->homePageThumbsVisibleDy = thumbsVisibleDy;
        if (scrollY > maxScrollY) {
            scrollY = maxScrollY;
            win->homePageScrollY = scrollY;
        }
        l.totalContentDy = contentDy;
        l.thumbsVisibleDy = thumbsVisibleDy;

        int first = 0;
        int last = -1;
        HomePageVisibleIndexRange(nFiles, scrollY, thumbsVisibleDy, rowDy, 1, &first, &last);
        for (int idx = first; idx <= last; idx++) {
            ThumbnailLayout& item = *l.thumbnails.AppendBlanks(1);
            FileState* fs = fileStates.at(idx);

            int y = thumbsTopY - scrollY + idx * rowDy;
            Rect rcRow(thumbsStartX, y, listContentWidth, HomePageListRowDy(dpiHwnd));
            if (isRtl) {
                rcRow.x = rc.dx - thumbsStartX - rcRow.dx;
            }
            LayoutHomeListItem(dpiHwnd, hdc, rcRow, fs, isRtl, item);
            AppendHomeListItemLinks(win, item, l.rcThumbsArea);
        }
    } else {
        int thumbsCols = thumbsColsForLayout;
        int thumbsRows = (nFiles + thumbsCols - 1) / thumbsCols;
        HFONT labelFont = HomePageThumbLabelFont(win->hwndCanvas);
        int labelDy = HomePageThumbLabelDy(dpiHwnd, hdc, labelFont);
        int labelGapY = HomePageThumbLabelGapY(dpiHwnd);
        int spaceBetweenY = std::max(kThumbsSpaceBetweenY, labelGapY + labelDy + DpiScale(dpiHwnd, 12));
        rowDy = thumbDy + spaceBetweenY;
        // include last row's filename band in scrollable content height
        contentDy = thumbsRows > 0 ? (thumbsRows - 1) * rowDy + thumbDy + labelGapY + labelDy : 0;

        int scrollY = win->homePageScrollY;
        int maxScrollY = std::max(0, contentDy - thumbsVisibleDy);
        win->homePageMaxScrollY = maxScrollY;
        win->homePageThumbsVisibleDy = thumbsVisibleDy;
        if (scrollY > maxScrollY) {
            scrollY = maxScrollY;
            win->homePageScrollY = scrollY;
        }
        l.totalContentDy = contentDy;
        l.thumbsVisibleDy = thumbsVisibleDy;

        Point ptOff(thumbsStartX, thumbsTopY - scrollY);
        int first = 0;
        int last = -1;
        HomePageVisibleIndexRange(nFiles, scrollY, thumbsVisibleDy, rowDy, thumbsCols, &first, &last);
        for (int idx = first; idx <= last; idx++) {
            ThumbnailLayout& thumb = *l.thumbnails.AppendBlanks(1);
            FileState* fs = fileStates.at(idx);
            thumb.fs = fs;
            int row = idx / thumbsCols;
            int col = idx % thumbsCols;

            Rect rcPage(ptOff.x + col * (thumbDx + thumbGapX), ptOff.y + row * rowDy, thumbDx, thumbDy);
            if (isRtl) {
                rcPage.x = rc.dx - rcPage.x - rcPage.dx;
            }
            if (fs->thumbnail) {
                thumb.szThumb = fs->thumbnail->GetSize();
            }
            thumb.rcPage = rcPage;
            thumb.rcText = HomePageThumbTextRect(dpiHwnd, rcPage, labelDy, spaceBetweenY, isRtl);
            LayoutHomeThumbActions(hwnd, rcPage, isRtl, thumb.rcListRemove, thumb.rcListPin);
            AppendHomeThumbActionLinks(win, fs, thumb.rcListRemove, thumb.rcListPin, l.rcThumbsArea);
            char* path = fs->filePath;
            Rect slRect = thumb.rcText.Union(rcPage).Intersect(l.rcThumbsArea);
            if (!slRect.IsEmpty()) {
                thumb.sl = new StaticLink(slRect, path, path);
                win->staticLinks.Append(thumb.sl);
            }
        }
    }
    win->homePageListView = listView;
    win->homePageRowDy = rowDy;
    win->homePageColDx = listView ? 0 : (thumbDx + thumbGapX);

    // layout tip at the bottom
    if (tip) {
        Rect rcClient = ClientRect(win->hwndCanvas);
        int tipPadding = DpiScale(dpiHwnd, 8);

        int tipY = rcClient.dy - tipHeight;
        // background spans full window width
        l.rcTip = {0, tipY, rcClient.dx, tipHeight};
        l.tip = tip;

        // text area aligned with thumbnails
        int tipTextWidth = thumbsColsForLayout * thumbDx + (thumbsColsForLayout - 1) * thumbGapX;
        int tipStartX = thumbsStartX;
        int tipStartY = tipY + tipPadding;
        LayoutTip(*tip, tipTextWidth, tipStartX, tipStartY);

        // register tip links; per-link rects first so they take priority in hit testing
        for (auto& link : tip->links) {
            // compute bounding rect of all words in this link
            Rect linkRect;
            for (int i = link.firstWord; i <= link.lastWord; i++) {
                auto& w = tip->words[i];
                Rect wr = {w.x, w.y, w.dx, w.dy};
                if (i == link.firstWord) {
                    linkRect = wr;
                } else {
                    linkRect = linkRect.Union(wr);
                }
            }
            auto slTip = new StaticLink(linkRect, link.cmd, link.cmd);
            win->staticLinks.Append(slTip);
        }
        // tip background: clicking outside of links picks another tip
        auto slBg = new StaticLink(l.rcTip, kLinkNextTip);
        win->staticLinks.Append(slBg);
    }
}

static void GetFileStateIcon(FileState* fs) {
    if (fs->himl) {
        return;
    }
    SHFILEINFO sfi{};
    sfi.iIcon = -1;
    uint flags = SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES;
    WCHAR* filePathW = ToWStrTemp(fs->filePath);
    fs->himl = (HIMAGELIST)SHGetFileInfoW(filePathW, 0, &sfi, sizeof(sfi), flags);
    fs->iconIdx = sfi.iIcon;
}

constexpr int kThumbCornerRadius = 10;

static void FillRoundedRect(HDC hdc, const Rect& rc, int radius, COLORREF col) {
    HRGN rgn = CreateRoundRectRgn(rc.x, rc.y, rc.x + rc.dx, rc.y + rc.dy, radius, radius);
    HBRUSH br = CreateSolidBrush(col);
    FillRgn(hdc, rgn, br);
    DeleteObject(br);
    DeleteObject(rgn);
}

static void DrawRoundedRectBorder(HDC hdc, const Rect& rc, int radius, HPEN pen) {
    SelectObject(hdc, pen);
    SelectObject(hdc, GetStockBrush(NULL_BRUSH));
    RoundRect(hdc, rc.x, rc.y, rc.x + rc.dx, rc.y + rc.dy, radius, radius);
}

static void DrawThumbnailCardShadow(HDC hdc, const Rect& page, int radius) {
    COLORREF bg = ThemeMainWindowBackgroundColor();
    int offX = DpiScale(hdc, 2);
    int offY = DpiScale(hdc, 3);
    Rect sh = page;
    sh.Offset(offX, offY);
    FillRoundedRect(hdc, sh, radius, AccentColor(bg, 22));
    sh = page;
    sh.Offset(offX / 2, offY / 2 + DpiScale(hdc, 1));
    FillRoundedRect(hdc, sh, radius, AccentColor(bg, 12));
}

// thumbnails that failed to render often come back as a uniform near-black bitmap
static bool IsThumbnailMostlyBlank(RenderedBitmap* bmp) {
    if (!bmp || !bmp->IsValid()) {
        return true;
    }
    BitmapPixels* pixels = GetBitmapPixels(bmp->GetBitmap());
    if (!pixels || !pixels->pixels) {
        if (pixels) {
            free(pixels);
        }
        return false;
    }

    Size sz = pixels->size;
    int step = 12;
    float maxLightness = 0;
    for (int y = 0; y < sz.dy; y += step) {
        for (int x = 0; x < sz.dx; x += step) {
            COLORREF c = GetPixel(pixels, x, y);
            float lightness = GetLightness(c);
            if (lightness > maxLightness) {
                maxLightness = lightness;
            }
        }
    }
    if (!pixels->hdc) {
        free(pixels);
    } else {
        FinalizeBitmapPixels(pixels);
    }
    return maxLightness < 0.08f;
}

static void DrawThumbnailPlaceholder(HDC hdc, FileState* fs, const Rect& page) {
    SHFILEINFO sfi{};
    WCHAR* pathW = ToWStrTemp(fs->filePath);
    DWORD flags = SHGFI_ICON | SHGFI_LARGEICON;
    HIMAGELIST himl = (HIMAGELIST)SHGetFileInfoW(pathW, 0, &sfi, sizeof(sfi), flags);
    if (!himl || !sfi.hIcon) {
        return;
    }
    int drawDx = DpiScale(hdc, 48);
    int drawDy = DpiScale(hdc, 48);
    int x = page.x + (page.dx - drawDx) / 2;
    int y = page.y + (page.dy - drawDy) / 2;
    DrawIconEx(hdc, x, y, sfi.hIcon, drawDx, drawDy, 0, nullptr, DI_NORMAL);
    DestroyIcon(sfi.hIcon);
}

static void DrawThumbnailCard(HDC hdc, const Rect& page, FileState* fs, RenderedBitmap* thumbImg, HPEN borderPen,
                              bool fastDraw = false) {
    if (!fastDraw) {
        DrawThumbnailCardShadow(hdc, page, kThumbCornerRadius);
    }

    bool showPlaceholder = !thumbImg;
    if (thumbImg) {
        if (!fs->thumbnailBlankKnown) {
            fs->thumbnailIsBlank = IsThumbnailMostlyBlank(thumbImg);
            fs->thumbnailBlankKnown = true;
        }
        showPlaceholder = fs->thumbnailIsBlank;
    }
    if (showPlaceholder) {
        FillRoundedRect(hdc, page, kThumbCornerRadius, ThemeThumbnailBackgroundColor());
    }

    {
        int savedDC = SaveDC(hdc);
        HRGN clip = CreateRoundRectRgn(page.x, page.y, page.x + page.dx, page.y + page.dy, kThumbCornerRadius,
                                       kThumbCornerRadius);
        ExtSelectClipRgn(hdc, clip, RGN_AND);
        if (showPlaceholder) {
            DrawThumbnailPlaceholder(hdc, fs, page);
        } else {
            thumbImg->Blit(hdc, page);
        }
        RestoreDC(hdc, savedDC);
        DeleteObject(clip);
    }

    DrawRoundedRectBorder(hdc, page, kThumbCornerRadius, borderPen);
}

// Scale to fill dest and crop overflow (CSS background-size: cover).
static Rect CoverRectInRect(Size src, Rect dst) {
    if (src.dx <= 0 || src.dy <= 0 || dst.dx <= 0 || dst.dy <= 0) {
        return dst;
    }
    double scale = std::max((double)dst.dx / src.dx, (double)dst.dy / src.dy);
    int dx = (int)(src.dx * scale + 0.999);
    int dy = (int)(src.dy * scale + 0.999);
    if (dx < dst.dx) {
        dx = dst.dx;
    }
    if (dy < dst.dy) {
        dy = dst.dy;
    }
    return {dst.x + (dst.dx - dx) / 2, dst.y + (dst.dy - dy) / 2, dx, dy};
}

static void DrawListItemRow(HWND hwnd, HDC hdc, const ThumbnailLayout& item, StrVec& filterWords, Vec<u8>& highlighted,
                            bool isRtl, COLORREF backgroundColor, bool hovered) {
    FileState* fs = item.fs;
    if (!fs) {
        return;
    }

    HFONT fontName = HomePageFileNameFont(hwnd);
    HFONT fontMeta = HomePageMetaFont(hwnd);
    char* path = fs->filePath;
    TempStr fileName = path::GetBaseNameTemp(path);
    TempStr dirPath = path::GetDirTemp(path);
    TempStr fileSize = FileSizeForHomeListTemp(path);

    const Rect& row = item.rcListRow;
    if (hovered) {
        int radius = DpiScale(hwnd, 8);
        COLORREF hoverCol = AccentColor(backgroundColor, ThemeUsesDarkChrome() ? -14 : 14);
        FillRoundedRect(hdc, row, radius, hoverCol);
        backgroundColor = hoverCol;
    }

    const Rect& thumbBox = item.rcListThumb;
    int thumbRadius = DpiScale(hwnd, 5);
    FillRoundedRect(hdc, thumbBox, thumbRadius, ThemeThumbnailBackgroundColor());
    RenderedBitmap* thumbImg = LoadThumbnail(fs);
    if (thumbImg) {
        int savedDC = SaveDC(hdc);
        HRGN clip = CreateRoundRectRgn(thumbBox.x, thumbBox.y, thumbBox.x + thumbBox.dx, thumbBox.y + thumbBox.dy,
                                       thumbRadius, thumbRadius);
        ExtSelectClipRgn(hdc, clip, RGN_AND);
        thumbImg->Blit(hdc, CoverRectInRect(thumbImg->GetSize(), thumbBox));
        RestoreDC(hdc, savedDC);
        DeleteObject(clip);
    }
    AutoDeletePen thumbPen(CreatePen(PS_SOLID, 1, ThemeThumbnailBorderColor()));
    DrawRoundedRectBorder(hdc, thumbBox, thumbRadius, thumbPen);

    UINT nameFmt = DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX | (isRtl ? DT_RIGHT : DT_LEFT);

    SelectObject(hdc, fontName);
    SetTextColor(hdc, HomePageListNameColor());
    {
        const Rect& rect = item.rcListFileName;
        RECT rcText = {rect.x, rect.y, rect.x + rect.dx, rect.y + rect.dy};
        DrawMaybeHighlightedTextArgs hlArgs(filterWords, highlighted);
        hlArgs.hdc = hdc;
        hlArgs.rc = rcText;
        hlArgs.text = fileName;
        hlArgs.colBg = backgroundColor;
        hlArgs.isRtl = isRtl;
        hlArgs.drawFmt = nameFmt;
        DrawMaybeHighlightedText(hlArgs);
    }

    SetTextColor(hdc, ThemeWindowTextDisabledColor());
    if (!item.rcListPath.IsEmpty()) {
        const Rect& rect = item.rcListPath;
        RECT rcPathWin = {rect.x, rect.y, rect.x + rect.dx, rect.y + rect.dy};
        UINT pathFmt = DT_SINGLELINE | DT_VCENTER | DT_PATH_ELLIPSIS | DT_NOPREFIX | (isRtl ? DT_RIGHT : DT_LEFT);
        HdcDrawText(hdc, dirPath, &rcPathWin, pathFmt, fontMeta);
    }

    if (!item.rcListSize.IsEmpty()) {
        const Rect& sizeRect = item.rcListSize;
        RECT rcSize = {sizeRect.x, sizeRect.y, sizeRect.x + sizeRect.dx, sizeRect.y + sizeRect.dy};
        UINT sizeFmt = DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX | (isRtl ? DT_LEFT : DT_RIGHT);
        HdcDrawText(hdc, fileSize, &rcSize, sizeFmt, fontMeta);
    }

    DrawHomeThumbActions(hdc, hwnd, fs, item.rcListRemove, item.rcListPin, true, true);
}

static void DrawHomeIconBtn(HDC hdc, const Rect& rc, TbIcon icon) {
    if (rc.IsEmpty()) {
        return;
    }
    int pad = DpiScale(hdc, 2);
    Rect dest = rc;
    dest.Inflate(-pad, -pad);
    if (dest.dx < 8 || dest.dy < 8) {
        dest = rc;
    }
    int side = std::min(dest.dx, dest.dy);
    dest = {dest.x + (dest.dx - side) / 2, dest.y + (dest.dy - side) / 2, side, side};
    DrawSvgIcon(hdc, dest, icon, ThemeWindowTextColor(), ThemeMainWindowBackgroundColor());
}

static void DrawHomePageLayout(HomePageLayout& l) {
    bool isRtl = IsUIRtl();
    auto hdc = l.hdc;
    auto win = l.win;
    auto textColor = ThemeWindowTextColor();
    auto backgroundColor = ThemeMainWindowBackgroundColor();

    {
        Rect rc = ClientRect(win->hwndCanvas);
        auto color = ThemeMainWindowBackgroundColor();
        FillRect(hdc, rc, color);
    }

    // draw search field chrome (rounded) around the edit control
    {
        const Rect& sb = l.rcSearchBorder;
        COLORREF bgCol = ThemeControlBackgroundColor();
        int radius = DpiScale(hdc, 8);
        FillRoundedRect(hdc, sb, radius, bgCol);
        COLORREF borderCol = AccentColor(bgCol, ThemeUsesDarkChrome() ? -18 : 32);
        AutoDeletePen penSearch(CreatePen(PS_SOLID, 1, borderCol));
        DrawRoundedRectBorder(hdc, sb, radius, penSearch);
    }

    if (false) {
        const Rect& r = l.rcAppWithVer;
        DrawSumatraVersion(hdc, r);
    }

    auto color = ThemeWindowTextColor();
    if (false) {
        ScopedSelectObject pen(hdc, CreatePen(PS_SOLID, 1, color), true);
        DrawLine(hdc, l.rcLine);
    }
    HFONT fontText = HomePageThumbLabelFont(win->hwndCanvas);

    AutoDeletePen penThumbBorder(CreatePen(PS_SOLID, kThumbsBorderDx, ThemeThumbnailBorderColor()));
    color = ThemeWindowLinkColor();
    AutoDeletePen penLinkLine(CreatePen(PS_SOLID, 1, color));

    SelectObject(hdc, penThumbBorder);
    SetBkMode(hdc, TRANSPARENT);
    color = ThemeWindowTextColor();
    SetTextColor(hdc, color);

    bool frequent = gGlobalPrefs && gGlobalPrefs->homePageSortByFrequentlyRead;
    DrawHomeIconBtn(hdc, l.rcIconView, HomePageUsesListView() ? TbIcon::HomeList : TbIcon::HomeThumbnails);
    DrawHomeIconBtn(hdc, l.rcIconSort, frequent ? TbIcon::HomeFrequent : TbIcon::HomeHistory);

    // clip file list to the middle area
    {
        const Rect& ta = l.rcThumbsArea;
        HRGN thumbsClip = CreateRectRgn(ta.x, ta.y, ta.x + ta.dx, ta.y + ta.dy);
        SelectClipRgn(hdc, thumbsClip);
        DeleteObject(thumbsClip);
    }

    bool listView = HomePageUsesListView();
    if (listView) {
        for (const ThumbnailLayout& item : l.thumbnails) {
            DrawListItemRow(l.win->hwndCanvas, hdc, item, l.filterWords, l.highlighted, isRtl, backgroundColor,
                            HomePageThumbIsHovered(win, item.fs));
        }
    } else {
        for (const ThumbnailLayout& thumb : l.thumbnails) {
            FileState* fs = thumb.fs;
            const Rect& page = thumb.rcPage;

            RenderedBitmap* thumbImg = LoadThumbnail(fs);
            DrawThumbnailCard(hdc, page, fs, thumbImg, penThumbBorder);

            const Rect& rect = thumb.rcText;
            char* path = fs->filePath;
            TempStr fileName = path::GetBaseNameTemp(path);

            DrawHomePageThumbLabel(win->hwndCanvas, hdc, page, rect, fs, fileName, fontText, l.filterWords,
                                   l.highlighted, isRtl, backgroundColor);
            DrawHomeThumbActions(hdc, win->hwndCanvas, fs, thumb.rcListRemove, thumb.rcListPin,
                                 HomePageThumbIsHovered(win, fs), false);
        }
    }

    // restore full clip region
    SelectClipRgn(hdc, nullptr);

    SetTextColor(hdc, ThemeWindowTextColor());

    if (false) {
        Rect rcFreqRead = DrawHideFrequentlyReadLink(win->hwndCanvas, hdc, _TRA("Hide frequently read"));
        auto sl = new StaticLink(rcFreqRead, kLinkHideList);
        win->staticLinks.Append(sl);
    }

    // draw tip at the bottom
    if (l.tip) {
        COLORREF tipBgCol = ThemeControlBackgroundColor();
        FillRect(hdc, l.rcTip, tipBgCol);

        HFONT fontTip = GetAppFontForHwnd(win->hwndCanvas);
        uint fmt = DT_LEFT | DT_NOCLIP;
        COLORREF textCol = ThemeWindowTextColor();
        COLORREF linkCol = ThemeWindowLinkColor();

        for (auto& w : l.tip->words) {
            Point pt = {w.x, w.y};
            if (w.isLink) {
                SetTextColor(hdc, linkCol);
                HdcDrawText(hdc, w.text, pt, fmt, fontTip);
            } else {
                SetTextColor(hdc, textCol);
                HdcDrawText(hdc, w.text, pt, fmt, fontTip);
            }
        }
        // draw underlines spanning each link
        SelectObject(hdc, penLinkLine);
        for (auto& link : l.tip->links) {
            auto& first = l.tip->words[link.firstWord];
            auto& last = l.tip->words[link.lastWord];
            int underlineY = first.y + first.dy - 3;
            int x1 = first.x;
            int x2 = last.x + last.dx;
            DrawLine(hdc, Rect(x1, underlineY, x2 - x1, 0));
        }
    }
}

void HomePageInvalidateScrollCache(MainWindow* win) {
    if (win->homePageScrollTimer) {
        KillTimer(win->hwndCanvas, HOME_SCROLL_TIMER_ID);
        win->homePageScrollTimer = 0;
    }
    win->homePageBlitScrollReady = false;
    win->homePageScrollTargetY = win->homePageScrollY;
    win->homePageHoverPath.Set(nullptr);
}

static bool HomePageIsThumbFileLink(MainWindow* win, const char* target) {
    if (!target) {
        return false;
    }
    if (str::StartsWith(target, kLinkHomePageRemoveFile) || str::StartsWith(target, kLinkHomePagePinFile)) {
        return true;
    }
    for (FileState* fs : win->homePageFileStates) {
        if (str::Eq(target, fs->filePath)) {
            return true;
        }
    }
    return false;
}

static void HomePageUpdateScrollPos(MainWindow* win, int pos) {
    if (!win) {
        return;
    }
    if (ScrollbarsUseOverlay()) {
        if (win->overlayScrollV && win->homePageMaxScrollY > 0) {
            SCROLLINFO si{};
            si.cbSize = sizeof(si);
            si.fMask = SIF_POS;
            si.nPos = pos;
            OverlayScrollbarSetInfo(win->overlayScrollV, &si, TRUE);
        }
        return;
    }
    if (ScrollbarsAreHidden()) {
        return;
    }
    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_POS;
    si.nPos = pos;
    SetScrollInfo(win->hwndCanvas, SB_VERT, &si, TRUE);
}

static void HomePageOffsetThumbLinks(MainWindow* win, int dy) {
    const Rect& ta = win->homePageThumbsArea;
    for (int i = win->staticLinks.Size() - 1; i >= 0; i--) {
        StaticLink* sl = win->staticLinks[i];
        if (!HomePageIsThumbFileLink(win, sl->target)) {
            continue;
        }
        sl->rect.Offset(0, dy);
        if (sl->rect.Intersect(ta).IsEmpty()) {
            delete sl;
            win->staticLinks.RemoveAt(i);
        }
    }
}

static void HomePageUpdateScrollCache(MainWindow* win, HomePageLayout& l) {
    win->homePageThumbsArea = l.rcThumbsArea;
    win->homePageThumbsStartX = l.thumbsStartX;
    win->homePageThumbsTopY = l.thumbsTopY;
    win->homePageThumbsCols = l.thumbsCols;
    win->homePageThumbDx = l.thumbDx;
    win->homePageThumbDy = l.thumbDy;
    win->homePagePaintScrollY = win->homePageScrollY;
    win->homePageScrollTargetY = win->homePageScrollY;
    win->homePageBlitScrollReady = true;

    win->homePageFileStates.Reset();
    for (FileState* fs : l.fileStates) {
        win->homePageFileStates.Append(fs);
    }
    win->homePageFilterWords.Reset();
    for (int i = 0; i < l.filterWords.Size(); i++) {
        win->homePageFilterWords.Append(l.filterWords.At(i));
    }
    win->homePageHighlighted.Reset();
}

static void HomePageRemoveThumbLinks(MainWindow* win) {
    for (int i = win->staticLinks.Size() - 1; i >= 0; i--) {
        StaticLink* sl = win->staticLinks[i];
        if (HomePageIsThumbFileLink(win, sl->target)) {
            delete sl;
            win->staticLinks.RemoveAt(i);
        }
    }
}

static Rect HomePageItemRect(MainWindow* win, int idx, int scrollY) {
    if (win->homePageListView) {
        int rowDy = win->homePageRowDy;
        if (rowDy <= 0) {
            rowDy = HomePageListRowDy(win->hwndCanvas) + HomePageListRowSpacing(win->hwndCanvas);
        }
        int y = win->homePageThumbsTopY - scrollY + idx * rowDy;
        int listContentWidth = win->canvasRc.dx - 2 * win->homePageThumbsStartX;
        Rect rcRow(win->homePageThumbsStartX, y, listContentWidth, HomePageListRowDy(win->hwndCanvas));
        if (IsUIRtl()) {
            rcRow.x = win->canvasRc.dx - win->homePageThumbsStartX - rcRow.dx;
        }
        return rcRow;
    }

    int cols = win->homePageThumbsCols;
    if (cols <= 0) {
        cols = 1;
    }
    int row = idx / cols;
    int col = idx % cols;
    int rowDy = win->homePageRowDy;
    int colDx = win->homePageColDx;
    int thumbDx = HomePageThumbDisplayDx(win);
    int thumbDy = HomePageThumbDisplayDy(win);
    if (rowDy <= 0) {
        rowDy = thumbDy + DpiScale(win->hwndCanvas, 58);
    }
    if (colDx <= 0) {
        colDx = thumbDx + DpiScale(win->hwndCanvas, 38);
    }
    Rect rcPage(win->homePageThumbsStartX + col * colDx, win->homePageThumbsTopY - scrollY + row * rowDy, thumbDx,
                thumbDy);
    if (IsUIRtl()) {
        rcPage.x = win->canvasRc.dx - rcPage.x - rcPage.dx;
    }
    return rcPage;
}

static void HomePageRebuildItemLinks(MainWindow* win, int scrollY) {
    HomePageRemoveThumbLinks(win);
    const Rect& ta = win->homePageThumbsArea;
    HDC hdc = GetDC(win->hwndCanvas);
    for (int idx = 0; idx < win->homePageFileStates.Size(); idx++) {
        FileState* fs = win->homePageFileStates[idx];
        Rect rcItem = HomePageItemRect(win, idx, scrollY);
        if (rcItem.Intersect(ta).IsEmpty()) {
            continue;
        }
        if (win->homePageListView) {
            ThumbnailLayout item;
            LayoutHomeListItem(win->hwndCanvas, hdc, rcItem, fs, IsUIRtl(), item);
            AppendHomeListItemLinks(win, item, ta);
        } else {
            HFONT labelFont = HomePageThumbLabelFont(win->hwndCanvas);
            int labelDy = HomePageThumbLabelDy(win->hwndCanvas, hdc, labelFont);
            Rect rcText =
                HomePageThumbTextRect(win->hwndCanvas, rcItem, labelDy, HomePageThumbSpaceBetweenY(win), IsUIRtl());
            Rect rcRemove;
            Rect rcPin;
            LayoutHomeThumbActions(win->hwndCanvas, rcItem, IsUIRtl(), rcRemove, rcPin);
            AppendHomeThumbActionLinks(win, fs, rcRemove, rcPin, ta);
            Rect slRect = rcText.Union(rcItem).Intersect(ta);
            if (!slRect.IsEmpty()) {
                auto sl = new StaticLink(slRect, fs->filePath, fs->filePath);
                win->staticLinks.Append(sl);
            }
        }
    }
    ReleaseDC(win->hwndCanvas, hdc);
    Point pt = HwndGetCursorPos(win->hwndCanvas);
    HomePageUpdateHover(win, pt.IsEmpty() ? -1 : pt.x, pt.IsEmpty() ? -1 : pt.y);
}

static void HomePageDrawListItemAt(MainWindow* win, HDC hdc, FileState* fs, int idx, int scrollY) {
    ThumbnailLayout item;
    LayoutHomeListItem(win->hwndCanvas, hdc, HomePageItemRect(win, idx, scrollY), fs, IsUIRtl(), item);
    DrawListItemRow(win->hwndCanvas, hdc, item, win->homePageFilterWords, win->homePageHighlighted, IsUIRtl(),
                    ThemeMainWindowBackgroundColor(), HomePageThumbIsHovered(win, fs));
}

static void HomePageDrawThumbnailAt(MainWindow* win, HDC hdc, FileState* fs, int idx, int scrollY, HPEN borderPen,
                                    bool fastDraw) {
    Rect rcPage = HomePageItemRect(win, idx, scrollY);
    RenderedBitmap* thumbImg = LoadThumbnail(fs);
    DrawThumbnailCard(hdc, rcPage, fs, thumbImg, borderPen, fastDraw);

    HFONT fontText = HomePageThumbLabelFont(win->hwndCanvas);
    int labelDy = HomePageThumbLabelDy(win->hwndCanvas, hdc, fontText);
    Rect rcText = HomePageThumbTextRect(win->hwndCanvas, rcPage, labelDy, HomePageThumbSpaceBetweenY(win), IsUIRtl());

    char* path = fs->filePath;
    TempStr fileName = path::GetBaseNameTemp(path);
    auto backgroundColor = ThemeMainWindowBackgroundColor();

    DrawHomePageThumbLabel(win->hwndCanvas, hdc, rcPage, rcText, fs, fileName, fontText, win->homePageFilterWords,
                           win->homePageHighlighted, IsUIRtl(), backgroundColor);

    Rect rcRemove;
    Rect rcPin;
    LayoutHomeThumbActions(win->hwndCanvas, rcPage, IsUIRtl(), rcRemove, rcPin);
    DrawHomeThumbActions(hdc, win->hwndCanvas, fs, rcRemove, rcPin, HomePageThumbIsHovered(win, fs), false);
}

static int HomePageIndexForPath(MainWindow* win, const char* path) {
    if (!path) {
        return -1;
    }
    for (int i = 0; i < win->homePageFileStates.Size(); i++) {
        if (str::Eq(win->homePageFileStates[i]->filePath, path)) {
            return i;
        }
    }
    return -1;
}

static void HomePageFlushThumbIdx(MainWindow* win, int idx) {
    if (idx < 0 || idx >= win->homePageFileStates.Size() || !win->buffer) {
        return;
    }
    Rect rcPage = HomePageItemRect(win, idx, win->homePageScrollY);
    int pad = DpiScale(win->hwndCanvas, 6);
    Rect flush = rcPage;
    flush.Inflate(pad, pad);
    flush = flush.Intersect(win->homePageThumbsArea);
    if (flush.IsEmpty()) {
        return;
    }

    HDC hdc = win->buffer->GetDC();
    FillRect(hdc, flush, ThemeMainWindowBackgroundColor());
    if (win->homePageListView) {
        HomePageDrawListItemAt(win, hdc, win->homePageFileStates[idx], idx, win->homePageScrollY);
    } else {
        AutoDeletePen pen(CreatePen(PS_SOLID, kThumbsBorderDx, ThemeThumbnailBorderColor()));
        HomePageDrawThumbnailAt(win, hdc, win->homePageFileStates[idx], idx, win->homePageScrollY, pen, false);
    }

    HDC screen = GetDC(win->hwndCanvas);
    BitBlt(screen, flush.x, flush.y, flush.dx, flush.dy, hdc, flush.x, flush.y, SRCCOPY);
    ReleaseDC(win->hwndCanvas, screen);
}

void HomePageUpdateHover(MainWindow* win, int x, int y) {
    if (!win) {
        return;
    }
    const char* path = nullptr;
    if (x >= 0 && y >= 0) {
        path = HomePageFilePathFromLink(GetStaticLinkAtTemp(win->staticLinks, x, y, nullptr));
    }
    if (str::Eq(win->homePageHoverPath, path)) {
        return;
    }
    AutoFreeStr prev;
    prev.SetCopy(win->homePageHoverPath);
    win->homePageHoverPath.SetCopy(path);

    if (!win->buffer || !win->homePageBlitScrollReady) {
        InvalidateRect(win->hwndCanvas, nullptr, FALSE);
        return;
    }
    HomePageFlushThumbIdx(win, HomePageIndexForPath(win, prev));
    HomePageFlushThumbIdx(win, HomePageIndexForPath(win, path));
}

static void HomePageDrawItemsInRect(MainWindow* win, HDC hdc, const Rect& clipRect, int scrollY, bool fastDraw) {
    if (win->homePageFileStates.Size() == 0 || clipRect.IsEmpty()) {
        return;
    }

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, ThemeWindowTextColor());

    HRGN clip = CreateRectRgn(clipRect.x, clipRect.y, clipRect.x + clipRect.dx, clipRect.y + clipRect.dy);
    SelectClipRgn(hdc, clip);
    DeleteObject(clip);

    if (win->homePageListView) {
        for (int idx = 0; idx < win->homePageFileStates.Size(); idx++) {
            Rect rcItem = HomePageItemRect(win, idx, scrollY);
            if (rcItem.Intersect(clipRect).IsEmpty()) {
                continue;
            }
            HomePageDrawListItemAt(win, hdc, win->homePageFileStates[idx], idx, scrollY);
        }
    } else {
        AutoDeletePen penThumbBorder(CreatePen(PS_SOLID, kThumbsBorderDx, ThemeThumbnailBorderColor()));
        SelectObject(hdc, penThumbBorder);
        HFONT labelFont = HomePageThumbLabelFont(win->hwndCanvas);
        int labelDy = HomePageThumbLabelDy(win->hwndCanvas, hdc, labelFont);
        int spaceBetweenY = HomePageThumbSpaceBetweenY(win);
        for (int idx = 0; idx < win->homePageFileStates.Size(); idx++) {
            Rect rcPage = HomePageItemRect(win, idx, scrollY);
            // Include filename band: expose strips often cover only the label while the
            // thumbnail page sits just above, and testing page-only skipped redraws.
            Rect rcItem =
                HomePageThumbTextRect(win->hwndCanvas, rcPage, labelDy, spaceBetweenY, IsUIRtl()).Union(rcPage);
            if (rcItem.Intersect(clipRect).IsEmpty()) {
                continue;
            }
            HomePageDrawThumbnailAt(win, hdc, win->homePageFileStates[idx], idx, scrollY, penThumbBorder, fastDraw);
        }
    }

    SelectClipRgn(hdc, nullptr);
}

static void HomePageFlushThumbsToScreen(MainWindow* win, HDC bufDC, HDC screenHdc) {
    const Rect& ta = win->homePageThumbsArea;
    if (!ta.IsEmpty()) {
        BitBlt(screenHdc, ta.x, ta.y, ta.dx, ta.dy, bufDC, ta.x, ta.y, SRCCOPY);
    } else {
        win->buffer->Flush(screenHdc);
    }
}

static bool HomePageTryBlitScroll(MainWindow* win, int scrollBy, bool fastDraw) {
    if (!win->buffer || !win->homePageBlitScrollReady || scrollBy == 0) {
        return false;
    }

    const Rect& ta = win->homePageThumbsArea;
    if (ta.IsEmpty() || ta.dy <= 0) {
        return false;
    }

    int prevScrollY = win->homePageScrollY - scrollBy;
    if (win->homePagePaintScrollY != prevScrollY) {
        return false;
    }

    int absScroll = scrollBy < 0 ? -scrollBy : scrollBy;
    if (absScroll >= ta.dy) {
        return false;
    }

    HDC hdc = win->buffer->GetDC();
    COLORREF bgCol = ThemeMainWindowBackgroundColor();
    Rect exposed;

    if (scrollBy > 0) {
        int copyDy = ta.dy - scrollBy;
        BitBlt(hdc, ta.x, ta.y, ta.dx, copyDy, hdc, ta.x, ta.y + scrollBy, SRCCOPY);
        exposed = {ta.x, ta.y + copyDy, ta.dx, scrollBy};
    } else {
        int scrollUp = -scrollBy;
        int copyDy = ta.dy - scrollUp;
        BitBlt(hdc, ta.x, ta.y + scrollUp, ta.dx, copyDy, hdc, ta.x, ta.y, SRCCOPY);
        exposed = {ta.x, ta.y, ta.dx, scrollUp};
    }

    FillRect(hdc, exposed, bgCol);
    HomePageDrawItemsInRect(win, hdc, exposed, win->homePageScrollY, fastDraw);

    HDC screenHdc = GetDC(win->hwndCanvas);
    HomePageFlushThumbsToScreen(win, hdc, screenHdc);
    ReleaseDC(win->hwndCanvas, screenHdc);

    win->homePagePaintScrollY = win->homePageScrollY;
    if (fastDraw) {
        HomePageOffsetThumbLinks(win, -scrollBy);
    } else {
        HomePageRebuildItemLinks(win, win->homePageScrollY);
    }
    return true;
}

static void HomePageEnsureScrollTimer(MainWindow* win) {
    if (win->homePageScrollTimer) {
        return;
    }
    win->homePageScrollTimer = SetTimer(win->hwndCanvas, HOME_SCROLL_TIMER_ID, HOME_SCROLL_TIMER_MS, nullptr);
}

static bool HomePageApplyScrollStep(MainWindow* win, bool fastDraw) {
    int target = win->homePageScrollTargetY;
    int current = win->homePagePaintScrollY;
    int remaining = target - current;
    if (remaining == 0) {
        return false;
    }

    const Rect& ta = win->homePageThumbsArea;
    int maxStep = DpiScale(win->hwndCanvas, 96);
    if (ta.dy > 0) {
        maxStep = std::max(maxStep, ta.dy / 4);
    }

    int scrollBy = remaining;
    if (abs(remaining) > maxStep) {
        scrollBy = remaining > 0 ? maxStep : -maxStep;
    }

    win->homePageScrollY = current + scrollBy;
    if (HomePageTryBlitScroll(win, scrollBy, fastDraw)) {
        HomePageUpdateScrollPos(win, win->homePageScrollY);
        return win->homePageScrollY != target;
    }

    win->homePageScrollY = target;
    HomePageInvalidateScrollCache(win);
    InvalidateRect(win->hwndCanvas, nullptr, FALSE);
    return false;
}

void HomePageOnScrollTimer(MainWindow* win) {
    win->homePageScrollTimer = 0;
    if (HomePageApplyScrollStep(win, true)) {
        HomePageEnsureScrollTimer(win);
        return;
    }
    HomePageRebuildItemLinks(win, win->homePageScrollY);
}

static void HomePageScrollToTarget(MainWindow* win, int targetY) {
    win->homePageScrollTargetY = targetY;
    HomePageUpdateScrollPos(win, targetY);
    if (HomePageApplyScrollStep(win, true)) {
        HomePageEnsureScrollTimer(win);
    } else if (win->homePageScrollY == targetY) {
        HomePageRebuildItemLinks(win, win->homePageScrollY);
    }
}

void DrawHomePage(MainWindow* win, HDC hdc) {
    HWND hwnd = win->hwndFrame;
    DeleteVecMembers(win->staticLinks);

    HomePageLayout l;
    l.rc = ClientRect(win->hwndCanvas);
    l.hdc = hdc;
    l.hwnd = hwnd;
    l.win = win;
    LayoutHomePage(l);

    DrawHomePageLayout(l);

    HomePageUpdateScrollCache(win, l);

    // Vertical scrollbar when the file grid/list overflows the visible area.
    bool needVScroll = l.totalContentDy > l.thumbsVisibleDy && !ScrollbarsAreHidden();
    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_ALL;
    si.nMin = 0;
    si.nMax = needVScroll ? l.totalContentDy - 1 : 0;
    si.nPage = needVScroll ? (UINT)l.thumbsVisibleDy : 1;
    si.nPos = win->homePageScrollY;

    if (ScrollbarsUseOverlay()) {
        if (needVScroll) {
            // Thick (always visible) on the home page so overflow is obvious;
            // document view restores Smart/Overlay mode via UpdateScrollbars.
            if (!win->overlayScrollV) {
                win->overlayScrollV = OverlayScrollbarCreate(win->hwndCanvas, OverlayScrollbar::Type::Vert,
                                                             OverlayScrollbar::Mode::Thick);
            } else {
                OverlayScrollbarSetMode(win->overlayScrollV, OverlayScrollbar::Mode::Thick);
            }
            OverlayScrollbarShow(win->overlayScrollV, true);
            OverlayScrollbarSetInfo(win->overlayScrollV, &si, TRUE);
        } else {
            OverlayScrollbarShow(win->overlayScrollV, false);
        }
        ShowScrollBar(win->hwndCanvas, SB_VERT, FALSE);
    } else if (needVScroll) {
        OverlayScrollbarShow(win->overlayScrollV, false);
        ShowScrollBar(win->hwndCanvas, SB_VERT, TRUE);
        SetScrollInfo(win->hwndCanvas, SB_VERT, &si, TRUE);
    } else {
        OverlayScrollbarShow(win->overlayScrollV, false);
        ShowScrollBar(win->hwndCanvas, SB_VERT, FALSE);
    }
}

void HomePageOnVScroll(MainWindow* win, WPARAM wp) {
    USHORT msg = LOWORD(wp);
    int lineDy = win->homePageRowDy;
    if (lineDy <= 0) {
        lineDy = win->homePageListView ? HomePageListRowDy(win->hwndCanvas) + HomePageListRowSpacing(win->hwndCanvas)
                                       : kThumbnailDy + DpiScale(win->hwndCanvas, 58);
    }
    int pageDy = lineDy * 3;

    int newScrollY = win->homePageScrollY;
    switch (msg) {
        case SB_LINEUP:
            newScrollY -= lineDy;
            break;
        case SB_LINEDOWN:
            newScrollY += lineDy;
            break;
        case SB_PAGEUP:
            newScrollY -= pageDy;
            break;
        case SB_PAGEDOWN:
            newScrollY += pageDy;
            break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: {
            int pos = 0;
            if (ScrollbarsUseOverlay() && win->overlayScrollV) {
                pos = win->overlayScrollV->nTrackPos;
            } else {
                SCROLLINFO trackSi{};
                trackSi.cbSize = sizeof(trackSi);
                trackSi.fMask = SIF_TRACKPOS;
                GetScrollInfo(win->hwndCanvas, SB_VERT, &trackSi);
                pos = trackSi.nTrackPos;
            }
            newScrollY = pos;
            break;
        }
        case SB_TOP:
            newScrollY = 0;
            break;
        case SB_BOTTOM:
            newScrollY = INT_MAX; // will be clamped by layout
            break;
    }
    if (newScrollY < 0) {
        newScrollY = 0;
    }
    if (win->homePageMaxScrollY > 0 && newScrollY > win->homePageMaxScrollY) {
        newScrollY = win->homePageMaxScrollY;
    }
    if (newScrollY == win->homePageScrollTargetY) {
        return;
    }
    HomePageScrollToTarget(win, newScrollY);
}

void HomePageOnMouseWheel(MainWindow* win, int delta) {
    if (delta == 0) {
        return;
    }

    win->wheelAccumDelta += delta;

    ULONG ulScrollLines = 3;
    SystemParametersInfo(SPI_GETWHEELSCROLLLINES, 0, &ulScrollLines, 0);
    if (ulScrollLines == 0) {
        return;
    }

    int scrollBy = 0;
    if (ulScrollLines == (ULONG)-1) {
        int pageDy = win->homePageThumbsVisibleDy;
        if (pageDy <= 0) {
            return;
        }
        scrollBy = -MulDiv(pageDy, win->wheelAccumDelta, WHEEL_DELTA);
        if (scrollBy != 0) {
            win->wheelAccumDelta += MulDiv(WHEEL_DELTA, scrollBy, pageDy);
        }
    } else {
        int linePx = DpiScale(win->hwndCanvas, 16);
        int pxPerNotch = (int)ulScrollLines * linePx;
        scrollBy = -MulDiv(pxPerNotch, win->wheelAccumDelta, WHEEL_DELTA);
        if (scrollBy != 0) {
            win->wheelAccumDelta += MulDiv(WHEEL_DELTA, scrollBy, pxPerNotch);
        }
    }

    if (scrollBy == 0) {
        return;
    }

    int newTarget = win->homePageScrollTargetY + scrollBy;
    if (newTarget < 0) {
        newTarget = 0;
        win->wheelAccumDelta = 0;
    }
    if (win->homePageMaxScrollY > 0 && newTarget > win->homePageMaxScrollY) {
        newTarget = win->homePageMaxScrollY;
        win->wheelAccumDelta = 0;
    }
    if (newTarget == win->homePageScrollTargetY) {
        return;
    }
    HomePageScrollToTarget(win, newTarget);
}
