/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

/* styling for About/Properties windows */

struct MainWindow;

constexpr const char* kLeftTextFont = "Arial";
constexpr int kLeftTextFontSize = 14;
constexpr const char* kRightTextFont = "Arial Black";
constexpr int kRightTextFontSize = 14;

void ShowAboutWindow(MainWindow*);
void CloseAboutWindow();

void DrawAboutPage(MainWindow* win, HDC hdc);

TempStr GetStaticLinkAtTemp(Vec<StaticLink*>& linkInfo, int x, int y, StaticLink** info);

constexpr const char* kLinkOpenFile = "<File,Open>";
constexpr const char* kLinkShowList = "<View,ShowList>";
constexpr const char* kLinkHideList = "<View,HideList>";
constexpr const char* kLinkHomePageToggleView = "<View,HomePageToggleView>";
constexpr const char* kLinkHomePageToggleSort = "<View,HomePageToggleSort>";
constexpr const char* kLinkHomePageRemoveFile = "<View,HomePageRemove>";
constexpr const char* kLinkHomePagePinFile = "<View,HomePagePin>";
constexpr const char* kLinkNextTip = "<NextTip>";

void SetPromoString(const char*);

void DrawHomePage(MainWindow* win, HDC hdc);
void HomePageUpdateScrollbar(MainWindow* win, bool forHomePage);
void PickAnotherRandomPromotion();
void HomePageOnVScroll(MainWindow* win, WPARAM wp);
void HomePageOnMouseWheel(MainWindow* win, int delta);
void HomePageOnScrollTimer(MainWindow* win);
void HomePageInvalidateScrollCache(MainWindow* win);
void HomePageFocusSearch(MainWindow* win);
void HomePageDestroySearch(MainWindow* win);
bool HomePageApplySearchFont(MainWindow* win);
COLORREF HomePageSearchTextColor();
void HomePageScheduleSearchFilter(MainWindow* win);
void HomePageApplySearchFilter(MainWindow* win);
void HomePageRemoveMissingFiles(MainWindow* win);
void HomePageUpdateHover(MainWindow* win, int x, int y);

#define kHomeSearchDebounceTimerId 0x103
void HomePageOnLanguageChangedAll();
