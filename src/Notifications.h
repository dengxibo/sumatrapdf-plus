/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

struct NotificationWnd;

extern Kind kNotifCursorPos;
extern Kind kNotifActionResponse;
extern Kind kNotifPageInfo;
extern Kind kNotifAdHoc;
extern Kind kNotifThemeRelayout;
extern Kind kNotifEbookFontLayout;

using NotificationWndRemoved = Func1<NotificationWnd*>;

constexpr const int kNotifDefaultTimeOut = 1000 * 3; // 3 seconds
constexpr const int kNotif5SecsTimeOut = 1000 * 5;
constexpr const int kNotifNoTimeout = 0;

struct NotificationCreateArgs {
    HWND hwndParent = nullptr;
    HFONT font = nullptr;
    Kind groupId = kNotifActionResponse;
    bool warning = false;
    bool noClose = false; // if true, no close button; must have timeoutMs > 0
    int timeoutMs = 0;    // if 0 => persists until closed manually
    int delayInMs = 0;    // if > 0 => create hidden, show after delay
    float shrinkLimit = 1.0f;
    const char* msg = nullptr;
    NotificationWndRemoved onRemoved;
};

void NotificationUpdateMessage(NotificationWnd* wnd, const char* msg, int timeoutInMS = 0, bool highlight = false);
void RemoveNotification(NotificationWnd*);
bool RemoveNotificationsForGroup(HWND, Kind);
NotificationWnd* GetNotificationForGroup(HWND, Kind);
bool UpdateNotificationProgress(NotificationWnd*, const char* msg, int perc);
void RelayoutNotifications(HWND hwnd);

NotificationWnd* ShowNotification(const NotificationCreateArgs& args);
NotificationWnd* ShowTemporaryNotification(HWND hwnd, const char* msg, int timeoutMs = kNotifDefaultTimeOut);
NotificationWnd* ShowWarningNotification(HWND hwndParent, const char* msg, int timeoutMs);

void MaybeDelayedWarningNotification(const char* fmt, ...);
void ShowMaybeDelayedNotifications(HWND hwndParent);

int CalcPerc(int current, int total);
