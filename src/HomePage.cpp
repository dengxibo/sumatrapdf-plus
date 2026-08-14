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

static int HomePageListRowDy(HWND hwnd) {
    return DpiScale(hwnd, 58);
}

static int HomePageListRowSpacing(HWND hwnd) {
    return 0;
}

static int HomePageListThumbDx(HWND hwnd) {
    return DpiScale(hwnd, 38);
}

static int HomePageListThumbDy(HWND hwnd) {
    return DpiScale(hwnd, 50);
}

static int HomePageListGapDx(HWND hwnd) {
    return DpiScale(hwnd, 8);
}

// Keep the historic "MS Shell Dlg" 14pt look (CreateSimpleFont uses the paint
// HDC DPI). Row label height must be measured from that same font — hwnd-DPI
// DpiScale(20) was shorter than tmHeight when hdcDpi > hwndDpi and clipped text.
static HFONT HomePageThumbLabelFont(HDC hdc) {
    return CreateSimpleFont(hdc, "MS Shell Dlg", 14);
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

static int HomePageThumbSpaceBetweenY(MainWindow* win) {
    int spaceY = win && win->homePageRowDy > kThumbnailDy ? win->homePageRowDy - kThumbnailDy : 0;
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
    Rect rcIconOpen;
    Rect rcIconListView;
    Rect rcIconThumbnailView;

    HIMAGELIST himlOpen = nullptr;
    VirtWndText* freqRead = nullptr;
    VirtWndText* openDoc = nullptr;
    VirtWndText* hideShowFreqRead = nullptr;
    Vec<ThumbnailLayout> thumbnails; // info for each thumbnail
    Vec<FileState*> fileStates;      // filtered list, not owned
    int totalContentDy = 0;          // total height of all thumbnail rows
    int thumbsVisibleDy = 0;         // visible height for thumbnails area
    int thumbsStartX = 0;
    int thumbsTopY = 0; // y of row 0 before scroll offset
    int thumbsCols = 0;
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

HomePageLayout::~HomePageLayout() {
    delete freqRead;
    delete openDoc;
}

constexpr int kOpenDocumentYShift = 7;
constexpr int kThumbsMiddleMargin = 32;
constexpr int kSearchEditDy = 28;
constexpr int kHeaderSearchGapY = 12;
constexpr int kSearchThumbnailsGapY = 12;

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
    win->hwndHomeSearch = CreateWindowExW(exStyle, WC_EDITW, L"", style, 0, 0, 100, kSearchEditDy, win->hwndCanvas,
                                          nullptr, hmod, nullptr);
    HDC hdc = GetDC(win->hwndCanvas);
    HFONT font = CreateSimpleFont(hdc, "MS Shell Dlg", 14);
    ReleaseDC(win->hwndCanvas, hdc);
    SetWindowFont(win->hwndHomeSearch, font, TRUE);
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

void PickAnotherRandomPromotion() {
    PickAnotherRandomTip();
}

static TempStr FileSizeForHomeListTemp(const char* path) {
    i64 size = file::GetSize(path);
    if (size < 0) {
        return str::DupTemp("");
    }
    return str::FormatSizeShortTemp(size, nullptr);
}

static HFONT CreateHomeListFileNameFont(HDC hdc) {
    HFONT baseFont = CreateSimpleFont(hdc, "MS Shell Dlg", 14);
    LOGFONTW lf{};
    GetObjectW(baseFont, sizeof(lf), &lf);
    lf.lfWeight = FW_SEMIBOLD;
    return CreateFontIndirectW(&lf);
}

static void LayoutHomeListItem(HWND hwnd, HDC hdc, const Rect& row, FileState* fs, int actionIconDx, bool isRtl,
                               ThumbnailLayout& item) {
    int gapDx = HomePageListGapDx(hwnd);
    int thumbDx = HomePageListThumbDx(hwnd);
    int thumbDy = HomePageListThumbDy(hwnd);
    HFONT fontText = CreateSimpleFont(hdc, "MS Shell Dlg", 14);
    HFONT fontFileName = CreateHomeListFileNameFont(hdc);
    TempStr fileName = path::GetBaseNameTemp(fs->filePath);
    TempStr fileSize = FileSizeForHomeListTemp(fs->filePath);
    int nameDx = HdcMeasureText(hdc, fileName, fontFileName).dx;
    int sizeDx = HdcMeasureText(hdc, fileSize, fontText).dx;
    DeleteObject(fontFileName);

    Rect rcPin(row.x + row.dx - actionIconDx, row.y + (row.dy - actionIconDx) / 2, actionIconDx, actionIconDx);
    Rect rcRemove(rcPin.x - gapDx - actionIconDx, rcPin.y, actionIconDx, actionIconDx);
    Rect rcSize(rcRemove.x - gapDx - sizeDx, row.y, sizeDx, row.dy);
    Rect rcThumb(row.x, row.y + (row.dy - thumbDy) / 2, thumbDx, thumbDy);
    Rect rcFileName(rcThumb.x + rcThumb.dx + gapDx, row.y, rcSize.x - (rcThumb.x + rcThumb.dx + gapDx) - gapDx, row.dy);
    if (isRtl) {
        rcPin.x = row.x;
        rcRemove.x = rcPin.x + rcPin.dx + gapDx;
        rcSize.x = rcRemove.x + rcRemove.dx + gapDx;
        rcThumb.x = row.x + row.dx - rcThumb.dx;
        rcFileName.x = rcSize.x + rcSize.dx + gapDx;
        rcFileName.dx = rcThumb.x - rcFileName.x - gapDx;
    }

    Rect rcPath;
    int minPathDx = DpiScale(hwnd, 100);
    if (nameDx + gapDx + minPathDx <= rcFileName.dx) {
        int pathDx = rcFileName.dx - nameDx - gapDx;
        if (isRtl) {
            rcPath = {rcFileName.x, rcFileName.y, pathDx, rcFileName.dy};
            rcFileName.x = rcPath.x + rcPath.dx + gapDx;
            rcFileName.dx = nameDx;
        } else {
            rcPath = {rcFileName.x + nameDx + gapDx, rcFileName.y, pathDx, rcFileName.dy};
            rcFileName.dx = nameDx;
        }
    }

    item.fs = fs;
    item.rcPage = row;
    item.rcListRow = row;
    item.rcListThumb = rcThumb;
    item.rcListFileName = rcFileName;
    item.rcListPath = rcPath;
    item.rcListSize = rcSize;
    item.rcListRemove = rcRemove;
    item.rcListPin = rcPin;
    RenderedBitmap* thumbImg = LoadThumbnail(fs);
    if (thumbImg) {
        item.szThumb = thumbImg->GetSize();
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
    HFONT fontText = CreateSimpleFont(hdc, "MS Shell Dlg", 14);
    HFONT hdrFont = CreateSimpleFont(hdc, "MS Shell Dlg", 24);

    Size sz = CalcSumatraVersionSize(hdc);
    {
        Rect& r = l.rcAppWithVer;
        r.x = rc.dx - sz.dx - 3;
        r.y = 0;
        r.SetSize(sz);
    }

    l.rcLine = {0, sz.dy, rc.dx, 0};

    // --- Pre-compute thumbnail grid x offset so header can align with it ---
    // use unfiltered count so layout stays stable when search filters results
    int nFilesForLayout = allFileStates.Size();
    int colsForLayout =
        (rc.dx - kThumbsMarginLeft - kThumbsMarginRight + kThumbsSpaceBetweenX) / (kThumbnailDx + kThumbsSpaceBetweenX);
    int thumbsColsForLayout = std::max(colsForLayout, 1);
    int thumbsStartX = rc.x + kThumbsMarginLeft +
                       (rc.dx - thumbsColsForLayout * kThumbnailDx - (thumbsColsForLayout - 1) * kThumbsSpaceBetweenX -
                        kThumbsMarginLeft - kThumbsMarginRight) /
                           2;
    if (thumbsStartX < DpiScale(dpiHwnd, kInnerPadding)) {
        thumbsStartX = DpiScale(dpiHwnd, kInnerPadding);
    } else if (nFilesForLayout == 0) {
        thumbsStartX = kThumbsMarginLeft;
    }

    int thumbsContentWidth = thumbsColsForLayout * kThumbnailDx + (thumbsColsForLayout - 1) * kThumbsSpaceBetweenX;

    // --- Step 1: layout header at the top ---
    l.himlOpen = (HIMAGELIST)SendMessageW(win->hwndToolbar, TB_GETIMAGELIST, 0, 0);
    Rect rcIconView(0, 0, 0, 0);
    ImageList_GetIconSize(l.himlOpen, &rcIconView.dx, &rcIconView.dy);

    const char* txt = _TRA("Recently Opened");
    if (gGlobalPrefs->homePageSortByFrequentlyRead) {
        txt = _TRA("Frequently Read");
    }
    TempStr hdrText = str::JoinTemp(txt, "  \xE2\x96\xBE");
    VirtWndText* hdr = new VirtWndText(hwnd, hdrText, hdrFont);
    l.freqRead = hdr;
    hdr->isRtl = isRtl;
    Size txtSize = hdr->GetIdealSize(true);

    int hdrY = DpiScale(dpiHwnd, 8);
    int iconGap = DpiScale(dpiHwnd, 4);
    int titleGap = DpiScale(dpiHwnd, 8);
    int viewIconsDx = 2 * rcIconView.dx + iconGap;
    Rect rcHdr(thumbsStartX + viewIconsDx + titleGap, hdrY, txtSize.dx, txtSize.dy);
    l.rcIconThumbnailView = {thumbsStartX, rcHdr.y + (rcHdr.dy - rcIconView.dy) / 2, rcIconView.dx, rcIconView.dy};
    l.rcIconListView = {l.rcIconThumbnailView.x + rcIconView.dx + iconGap, l.rcIconThumbnailView.y, rcIconView.dx,
                        rcIconView.dy};
    if (isRtl) {
        rcHdr.x = rc.dx - thumbsStartX - rcHdr.dx;
        l.rcIconThumbnailView.x = rc.dx - thumbsStartX - rcIconView.dx;
        l.rcIconListView.x = l.rcIconThumbnailView.x - iconGap - rcIconView.dx;
    }
    hdr->SetBounds(rcHdr);
    win->staticLinks.Append(new StaticLink(l.rcIconThumbnailView, kLinkHomePageThumbView, _TRA("Thumbnail view")));
    win->staticLinks.Append(new StaticLink(l.rcIconListView, kLinkHomePageListView, _TRA("List view")));
    win->staticLinks.Append(new StaticLink(rcHdr, kLinkHomePageSort, txt));

    /* "Open a document" link next to header */
    Rect rcIconOpen(0, 0, 0, 0);
    ImageList_GetIconSize(l.himlOpen, &rcIconOpen.dx, &rcIconOpen.dy);

    txt = _TRA("Open a document...");
    auto openDoc = new VirtWndText(hwnd, txt, fontText);
    openDoc->isRtl = isRtl;
    openDoc->withUnderline = true;
    txtSize = openDoc->GetIdealSize(true);

    int openDocSpacing = DpiScale(dpiHwnd, 16);
    rcIconOpen.x = rcHdr.x + rcHdr.dx + openDocSpacing;
    rcIconOpen.y = rcHdr.y + rcHdr.dy - rcIconOpen.dy - kOpenDocumentYShift + 3;
    if (isRtl) {
        rcIconOpen.x = rcHdr.x - openDocSpacing - rcIconOpen.dx;
    }
    l.rcIconOpen = rcIconOpen;

    Rect rcOpenDoc(rcIconOpen.x + rcIconOpen.dx + 3, rcHdr.y + rcHdr.dy - txtSize.dy - kOpenDocumentYShift, txtSize.dx,
                   txtSize.dy);
    if (isRtl) {
        rcOpenDoc.x = rcIconOpen.x - rcOpenDoc.dx - 3;
    }
    openDoc->SetBounds(rcOpenDoc);

    Rect rcOpenDocLink = rcOpenDoc.Union(rcIconOpen);
    rcOpenDocLink.Inflate(10, 10);
    l.openDoc = openDoc;
    auto sl = new StaticLink(rcOpenDocLink, kLinkOpenFile);
    win->staticLinks.Append(sl);

    int headerBottomY = rcHdr.y + rcHdr.dy;

    // --- Position search edit below header ---
    EnsureHomeSearchCreated(win);
    int searchEditDy = DpiScale(dpiHwnd, kSearchEditDy);
    int headerSearchGap = DpiScale(dpiHwnd, kHeaderSearchGapY);
    int searchThumbsGap = DpiScale(dpiHwnd, kSearchThumbnailsGapY);
    {
        int borderDx = thumbsContentWidth * 3 / 4;
        if (borderDx < DpiScale(dpiHwnd, 200)) {
            borderDx = DpiScale(dpiHwnd, 200);
        }
        int borderX = thumbsStartX + (thumbsContentWidth - borderDx) / 2;
        int borderY = headerBottomY + headerSearchGap;
        int borderDy = searchEditDy + 2; // 1px border on each side
        l.rcSearchBorder = {borderX, borderY, borderDx, borderDy};
        // measure font height so we can vertically center the edit
        HFONT editFont = (HFONT)SendMessage(win->hwndHomeSearch, WM_GETFONT, 0, 0);
        TEXTMETRIC tm;
        HFONT oldFont = (HFONT)SelectObject(hdc, editFont);
        GetTextMetrics(hdc, &tm);
        SelectObject(hdc, oldFont);
        int fontDy = tm.tmHeight + tm.tmExternalLeading + 2; // +2 for caret padding
        int editDy = std::min(fontDy, searchEditDy);
        int editY = borderY + 1 + (searchEditDy - editDy) / 2;
        MoveWindow(win->hwndHomeSearch, borderX + 1, editY, borderDx - 2, editDy, TRUE);
    }
    // border is 1px top + 1px bottom = 2px
    int searchAreaDy = headerSearchGap + searchEditDy + 2 + searchThumbsGap;
    headerBottomY += searchAreaDy;

    // --- Step 2: calculate tip area at the bottom (before thumbnails) ---
    int tipHeight = 0;
    HFONT fontTip = CreateSimpleFont(hdc, "MS Shell Dlg", 16);
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
        int tipTextWidth = thumbsColsForLayout * kThumbnailDx + (thumbsColsForLayout - 1) * kThumbsSpaceBetweenX;
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
    int listContentWidth = thumbsContentWidth;
    if (listView) {
        rowDy = HomePageListRowDy(hwnd) + HomePageListRowSpacing(hwnd);
        contentDy = nFiles > 0 ? nFiles * rowDy - HomePageListRowSpacing(hwnd) : 0;
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

        for (int idx = 0; idx < nFiles; idx++) {
            ThumbnailLayout& item = *l.thumbnails.AppendBlanks(1);
            FileState* fs = fileStates.at(idx);

            int y = thumbsTopY - scrollY + idx * rowDy;
            Rect rcRow(thumbsStartX, y, listContentWidth, HomePageListRowDy(hwnd));
            if (isRtl) {
                rcRow.x = rc.dx - thumbsStartX - rcRow.dx;
            }
            LayoutHomeListItem(hwnd, hdc, rcRow, fs, l.rcIconListView.dx, isRtl, item);
            AppendHomeListItemLinks(win, item, l.rcThumbsArea);
        }
    } else {
        int thumbsCols = thumbsColsForLayout;
        int thumbsRows = (nFiles + thumbsCols - 1) / thumbsCols;
        HFONT labelFont = HomePageThumbLabelFont(hdc);
        int labelDy = HomePageThumbLabelDy(dpiHwnd, hdc, labelFont);
        int labelGapY = HomePageThumbLabelGapY(dpiHwnd);
        int spaceBetweenY = std::max(kThumbsSpaceBetweenY, labelGapY + labelDy + DpiScale(dpiHwnd, 12));
        rowDy = kThumbnailDy + spaceBetweenY;
        // include last row's filename band in scrollable content height
        contentDy = thumbsRows > 0 ? (thumbsRows - 1) * rowDy + kThumbnailDy + labelGapY + labelDy : 0;

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

        for (int row = 0; row < thumbsRows; row++) {
            for (int col = 0; col < thumbsCols; col++) {
                if (row * thumbsCols + col >= nFiles) {
                    thumbsRows = col > 0 ? row + 1 : row;
                    break;
                }
                ThumbnailLayout& thumb = *l.thumbnails.AppendBlanks(1);
                FileState* fs = fileStates.at(row * thumbsCols + col);
                thumb.fs = fs;

                Rect rcPage(ptOff.x + col * (kThumbnailDx + kThumbsSpaceBetweenX), ptOff.y + row * rowDy, kThumbnailDx,
                            kThumbnailDy);
                if (isRtl) {
                    rcPage.x = rc.dx - rcPage.x - rcPage.dx;
                }
                RenderedBitmap* thumbImg = LoadThumbnail(fs);
                if (thumbImg) {
                    thumb.szThumb = thumbImg->GetSize();
                }
                thumb.rcPage = rcPage;
                thumb.rcText = HomePageThumbTextRect(dpiHwnd, rcPage, labelDy, spaceBetweenY, isRtl);
                char* path = fs->filePath;
                Rect slRect = thumb.rcText.Union(rcPage).Intersect(l.rcThumbsArea);
                if (!slRect.IsEmpty()) {
                    thumb.sl = new StaticLink(slRect, path, path);
                    win->staticLinks.Append(thumb.sl);
                }
            }
        }
    }
    win->homePageListView = listView;
    win->homePageRowDy = rowDy;
    win->homePageColDx = listView ? 0 : (kThumbnailDx + kThumbsSpaceBetweenX);

    // layout tip at the bottom
    if (tip) {
        Rect rcClient = ClientRect(win->hwndCanvas);
        int tipPadding = DpiScale(dpiHwnd, 8);

        int tipY = rcClient.dy - tipHeight;
        // background spans full window width
        l.rcTip = {0, tipY, rcClient.dx, tipHeight};
        l.tip = tip;

        // text area aligned with thumbnails
        int tipTextWidth = thumbsColsForLayout * kThumbnailDx + (thumbsColsForLayout - 1) * kThumbsSpaceBetweenX;
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

static Rect FitRectInRect(Size src, Rect dst) {
    if (src.dx <= 0 || src.dy <= 0 || dst.dx <= 0 || dst.dy <= 0) {
        return dst;
    }
    double scale = std::min((double)dst.dx / src.dx, (double)dst.dy / src.dy);
    int dx = (int)(src.dx * scale);
    int dy = (int)(src.dy * scale);
    return {dst.x + (dst.dx - dx) / 2, dst.y + (dst.dy - dy) / 2, dx, dy};
}

static void DrawListItemRow(HWND hwnd, HDC hdc, HIMAGELIST himl, const ThumbnailLayout& item, StrVec& filterWords,
                            Vec<u8>& highlighted, bool isRtl, COLORREF backgroundColor) {
    FileState* fs = item.fs;
    if (!fs) {
        return;
    }

    HFONT fontText = CreateSimpleFont(hdc, "MS Shell Dlg", 14);
    HFONT fontFileName = CreateHomeListFileNameFont(hdc);
    char* path = fs->filePath;
    TempStr fileName = path::GetBaseNameTemp(path);
    TempStr dirPath = path::GetDirTemp(path);
    TempStr fileSize = FileSizeForHomeListTemp(path);

    const Rect& row = item.rcListRow;
    COLORREF lineCol = AccentColor(ThemeMainWindowBackgroundColor(), ThemeUsesDarkChrome() ? -25 : 30);
    {
        ScopedSelectObject pen(hdc, CreatePen(PS_SOLID, 1, lineCol), true);
        DrawLine(hdc, Rect(row.x, row.y + row.dy - 1, row.dx, 0));
    }

    const Rect& thumbBox = item.rcListThumb;
    FillRoundedRect(hdc, thumbBox, DpiScale(hwnd, 3), ThemeThumbnailBackgroundColor());
    RenderedBitmap* thumbImg = LoadThumbnail(fs);
    if (thumbImg) {
        Rect thumbDst = FitRectInRect(thumbImg->GetSize(), thumbBox);
        thumbImg->Blit(hdc, thumbDst);
    }
    AutoDeletePen thumbPen(CreatePen(PS_SOLID, 1, ThemeThumbnailBorderColor()));
    DrawRoundedRectBorder(hdc, thumbBox, DpiScale(hwnd, 3), thumbPen);

    UINT nameFmt = DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX | (isRtl ? DT_RIGHT : DT_LEFT);

    SelectObject(hdc, fontFileName);
    SetTextColor(hdc, ThemeWindowTextColor());
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

    if (!item.rcListPath.IsEmpty()) {
        SetTextColor(hdc, ThemeWindowTextDisabledColor());
        const Rect& rect = item.rcListPath;
        RECT rcPathWin = {rect.x, rect.y, rect.x + rect.dx, rect.y + rect.dy};
        UINT pathFmt = DT_SINGLELINE | DT_VCENTER | DT_PATH_ELLIPSIS | DT_NOPREFIX | (isRtl ? DT_LEFT : DT_RIGHT);
        HdcDrawText(hdc, dirPath, &rcPathWin, pathFmt, fontText);
    }

    SetTextColor(hdc, ThemeWindowTextColor());
    const Rect& sizeRect = item.rcListSize;
    RECT rcSize = {sizeRect.x, sizeRect.y, sizeRect.x + sizeRect.dx, sizeRect.y + sizeRect.dy};
    UINT sizeFmt = DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX | (isRtl ? DT_LEFT : DT_RIGHT);
    HdcDrawText(hdc, fileSize, &rcSize, sizeFmt, fontText);

    ImageList_Draw(himl, (int)TbIcon::Close, hdc, item.rcListRemove.x, item.rcListRemove.y, ILD_NORMAL);
    if (fs->isPinned) {
        FillRect(hdc, item.rcListPin, ThemeControlBackgroundColor());
    }
    ImageList_Draw(himl, (int)TbIcon::Pin, hdc, item.rcListPin.x, item.rcListPin.y, ILD_NORMAL);
    SelectObject(hdc, fontText);
    DeleteObject(fontFileName);
}

static void DrawHomeViewButton(HDC hdc, HIMAGELIST himl, const Rect& rect, TbIcon icon, bool selected) {
    if (selected) {
        FillRect(hdc, rect, ThemeControlBackgroundColor());
        AutoDeletePen pen(CreatePen(PS_SOLID, 1, AccentColor(ThemeControlBackgroundColor(), 55)));
        DrawRoundedRectBorder(hdc, rect, DpiScale(hdc, 2), pen);
    }
    ImageList_Draw(himl, (int)icon, hdc, rect.x, rect.y, ILD_NORMAL);
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

    // draw search edit border and background on the canvas
    {
        COLORREF bgCol = ThemeControlBackgroundColor();
        const Rect& sb = l.rcSearchBorder;
        RECT rcBorder = {sb.x, sb.y, sb.x + sb.dx, sb.y + sb.dy};
        // fill interior with control background so padding matches the edit
        HBRUSH brBg = CreateSolidBrush(bgCol);
        FillRect(hdc, &rcBorder, brBg);
        DeleteObject(brBg);
        // draw border frame
        COLORREF borderCol = AccentColor(bgCol, 40);
        HBRUSH brBorder = CreateSolidBrush(borderCol);
        FrameRect(hdc, &rcBorder, brBorder);
        DeleteObject(brBorder);
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
    HFONT fontText = HomePageThumbLabelFont(hdc);

    AutoDeletePen penThumbBorder(CreatePen(PS_SOLID, kThumbsBorderDx, ThemeThumbnailBorderColor()));
    color = ThemeWindowLinkColor();
    AutoDeletePen penLinkLine(CreatePen(PS_SOLID, 1, color));

    SelectObject(hdc, penThumbBorder);
    SetBkMode(hdc, TRANSPARENT);
    color = ThemeWindowTextColor();
    SetTextColor(hdc, color);

    DrawHomeViewButton(hdc, l.himlOpen, l.rcIconThumbnailView, TbIcon::HomeThumbnails, !HomePageUsesListView());
    DrawHomeViewButton(hdc, l.himlOpen, l.rcIconListView, TbIcon::HomeList, HomePageUsesListView());
    l.freqRead->Paint(hdc);

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
            DrawListItemRow(l.win->hwndCanvas, hdc, l.himlOpen, item, l.filterWords, l.highlighted, isRtl,
                            backgroundColor);
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
        }
    }

    // restore full clip region
    SelectClipRgn(hdc, nullptr);

    color = ThemeWindowLinkColor();
    SetTextColor(hdc, color);
    SelectObject(hdc, penLinkLine);

    int x = l.rcIconOpen.x;
    int y = l.rcIconOpen.y;
    int openIconIdx = 0;
    ImageList_Draw(l.himlOpen, openIconIdx, hdc, x, y, ILD_NORMAL);

    l.openDoc->Paint(hdc);

    if (false) {
        Rect rcFreqRead = DrawHideFrequentlyReadLink(win->hwndCanvas, hdc, _TRA("Hide frequently read"));
        auto sl = new StaticLink(rcFreqRead, kLinkHideList);
        win->staticLinks.Append(sl);
    }

    // draw tip at the bottom
    if (l.tip) {
        COLORREF tipBgCol = ThemeControlBackgroundColor();
        FillRect(hdc, l.rcTip, tipBgCol);

        HFONT fontTip = CreateSimpleFont(hdc, "MS Shell Dlg", 16);
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
    if (rowDy <= 0) {
        rowDy = kThumbnailDy + DpiScale(win->hwndCanvas, 58);
    }
    if (colDx <= 0) {
        colDx = kThumbnailDx + DpiScale(win->hwndCanvas, 38);
    }
    Rect rcPage(win->homePageThumbsStartX + col * colDx, win->homePageThumbsTopY - scrollY + row * rowDy, kThumbnailDx,
                kThumbnailDy);
    if (IsUIRtl()) {
        rcPage.x = win->canvasRc.dx - rcPage.x - rcPage.dx;
    }
    return rcPage;
}

static void HomePageRebuildItemLinks(MainWindow* win, int scrollY) {
    HomePageRemoveThumbLinks(win);
    const Rect& ta = win->homePageThumbsArea;
    HDC hdc = GetDC(win->hwndCanvas);
    HIMAGELIST himl = (HIMAGELIST)SendMessageW(win->hwndToolbar, TB_GETIMAGELIST, 0, 0);
    int actionIconDx = 0;
    int actionIconDy = 0;
    ImageList_GetIconSize(himl, &actionIconDx, &actionIconDy);
    for (int idx = 0; idx < win->homePageFileStates.Size(); idx++) {
        FileState* fs = win->homePageFileStates[idx];
        Rect rcItem = HomePageItemRect(win, idx, scrollY);
        if (win->homePageListView) {
            ThumbnailLayout item;
            LayoutHomeListItem(win->hwndCanvas, hdc, rcItem, fs, actionIconDx, IsUIRtl(), item);
            AppendHomeListItemLinks(win, item, ta);
        } else {
            HFONT labelFont = HomePageThumbLabelFont(hdc);
            int labelDy = HomePageThumbLabelDy(win->hwndCanvas, hdc, labelFont);
            Rect rcText =
                HomePageThumbTextRect(win->hwndCanvas, rcItem, labelDy, HomePageThumbSpaceBetweenY(win), IsUIRtl());
            Rect slRect = rcText.Union(rcItem).Intersect(ta);
            if (!slRect.IsEmpty()) {
                auto sl = new StaticLink(slRect, fs->filePath, fs->filePath);
                win->staticLinks.Append(sl);
            }
        }
    }
    ReleaseDC(win->hwndCanvas, hdc);
}

static void HomePageDrawListItemAt(MainWindow* win, HDC hdc, FileState* fs, int idx, int scrollY) {
    ThumbnailLayout item;
    HIMAGELIST himl = (HIMAGELIST)SendMessageW(win->hwndToolbar, TB_GETIMAGELIST, 0, 0);
    int actionIconDx = 0;
    int actionIconDy = 0;
    ImageList_GetIconSize(himl, &actionIconDx, &actionIconDy);
    LayoutHomeListItem(win->hwndCanvas, hdc, HomePageItemRect(win, idx, scrollY), fs, actionIconDx, IsUIRtl(), item);
    DrawListItemRow(win->hwndCanvas, hdc, himl, item, win->homePageFilterWords, win->homePageHighlighted, IsUIRtl(),
                    ThemeMainWindowBackgroundColor());
}

static void HomePageDrawThumbnailAt(MainWindow* win, HDC hdc, FileState* fs, int idx, int scrollY, HPEN borderPen,
                                    bool fastDraw) {
    Rect rcPage = HomePageItemRect(win, idx, scrollY);
    RenderedBitmap* thumbImg = LoadThumbnail(fs);
    DrawThumbnailCard(hdc, rcPage, fs, thumbImg, borderPen, fastDraw);

    HFONT fontText = HomePageThumbLabelFont(hdc);
    int labelDy = HomePageThumbLabelDy(win->hwndCanvas, hdc, fontText);
    Rect rcText = HomePageThumbTextRect(win->hwndCanvas, rcPage, labelDy, HomePageThumbSpaceBetweenY(win), IsUIRtl());

    char* path = fs->filePath;
    TempStr fileName = path::GetBaseNameTemp(path);
    auto backgroundColor = ThemeMainWindowBackgroundColor();

    DrawHomePageThumbLabel(win->hwndCanvas, hdc, rcPage, rcText, fs, fileName, fontText, win->homePageFilterWords,
                           win->homePageHighlighted, IsUIRtl(), backgroundColor);
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
        HFONT labelFont = HomePageThumbLabelFont(hdc);
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
