/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

struct MainWindow;
struct WindowTab;

// One sidebar column shows one of these. Favorites used to sit in a second pane.
enum class SidebarView {
    Bookmarks = 0,
    Thumbnails = 1,
    Favorites = 2,
    Ai = 3,
    Annotations = 4,
};

SidebarView SidebarViewFromStr(const char* s);
const char* SidebarViewToStr(SidebarView view);

SidebarView CurrentSidebarView(MainWindow* win);
bool SidebarViewAvailable(MainWindow* win, SidebarView view);
SidebarView FallbackSidebarView(MainWindow* win, SidebarView want);
void LoadTabSidebarView(WindowTab* tab);
void RememberTabSidebarView(WindowTab* tab);

// Show this view, or hide the column if it is already showing.
void ShowOrToggleSidebarView(MainWindow* win, SidebarView view);
// Show this view. Does not hide the column when it is already showing.
void ShowSidebarPage(MainWindow* win, SidebarView view);
void ApplySidebarViewLayout(MainWindow* win);
void UpdateSidebarViewButtons(MainWindow* win);

void CreateSidebarThumbs(MainWindow* win);
void DestroySidebarThumbs(MainWindow* win);
void SidebarThumbsOnThemeChanged(MainWindow* win);
void SidebarThumbsSyncCurrentPage(MainWindow* win);
void SidebarThumbsApplyFind(MainWindow* win, const char* query);
void BindSidebarFindEdit(MainWindow* win, SidebarView view);
void ApplySidebarFindEdit(MainWindow* win);
