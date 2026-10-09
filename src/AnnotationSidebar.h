#pragma once

#include "SvgIcons.h"

// After the inspector lays out, sit the reset button on the dropdown chevron.
inline void PlaceAnnotResetButton(HWND button) {
    if (!button || !IsWindow(button) || !IsWindowVisible(button)) {
        return;
    }
    HWND parent = GetParent(button);
    if (!parent) {
        return;
    }
    HWND combo = nullptr;
    int bestY = 0x7fffffff;
    for (HWND child = GetWindow(parent, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        if (child == button || !IsWindowVisible(child)) {
            continue;
        }
        WCHAR cls[32]{};
        if (GetClassNameW(child, cls, 32) <= 0 || wcscmp(cls, L"ComboBox") != 0) {
            continue;
        }
        RECT rc{};
        GetWindowRect(child, &rc);
        MapWindowPoints(nullptr, parent, (LPPOINT)&rc, 2);
        if (rc.top < bestY) {
            bestY = rc.top;
            combo = child;
        }
    }
    if (!combo) {
        return;
    }
    COMBOBOXINFO info{};
    info.cbSize = sizeof(info);
    if (!GetComboBoxInfo(combo, &info)) {
        return;
    }
    POINT center{(info.rcButton.left + info.rcButton.right) / 2, 0};
    MapWindowPoints(combo, parent, &center, 1);
    RECT wr{};
    GetWindowRect(button, &wr);
    MapWindowPoints(nullptr, parent, (LPPOINT)&wr, 2);
    int bw = wr.right - wr.left;
    int bh = wr.bottom - wr.top;
    int x = center.x - bw / 2 - 7;
    int y = wr.top + 11;
    if (x != wr.left || y != wr.top) {
        // The layout pass just parked the button. Move it without copying bits
        // or painting; the inspector redraws once at the final position.
        SetWindowPos(button, nullptr, x, y, bw, bh,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW | SWP_NOCOPYBITS);
    }
}

// Child visibility, in z-order. Used to skip a full inspector relayout when
// selecting another annotation that shows the same controls.
inline void CaptureInspectorChildVis(HWND parent, Vec<HWND>& hwnds, Vec<u8>& vis) {
    hwnds.Reset();
    vis.Reset();
    if (!parent) {
        return;
    }
    for (HWND child = GetWindow(parent, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        hwnds.Append(child);
        Wnd* wnd = WndListFindByHwnd(child);
        vis.Append(wnd ? (u8)wnd->IsVisible() : (u8)(IsWindowVisible(child) ? 1 : 0));
    }
}

inline bool InspectorChildVisUnchanged(HWND parent, const Vec<HWND>& hwnds, const Vec<u8>& was) {
    if (!parent || hwnds.Size() != was.Size()) {
        return false;
    }
    int i = 0;
    for (HWND child = GetWindow(parent, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT), i++) {
        if (i >= hwnds.Size() || child != hwnds.at(i)) {
            return false;
        }
        Wnd* wnd = WndListFindByHwnd(child);
        u8 now = wnd ? (u8)wnd->IsVisible() : (u8)(IsWindowVisible(child) ? 1 : 0);
        if (now != was.at(i)) {
            return false;
        }
    }
    return i == hwnds.Size();
}

// Hide/show/relayout of the inspector paints each control as it changes.
// Lock the sidebar until the final arrangement, then present it once.
struct InspectorUpdateLock {
    HWND hwnd = nullptr;
    HWND pane = nullptr;
    bool locked = false;
    bool erase = false;
    explicit InspectorUpdateLock(HWND window, HWND inspector) : hwnd(window), pane(inspector) {
        locked = hwnd && LockWindowUpdate(hwnd);
    }
    ~InspectorUpdateLock() {
        if (locked) {
            LockWindowUpdate(nullptr);
        }
        if (!hwnd) {
            return;
        }
        if (erase && pane) {
            RedrawWindow(pane, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
        }
        RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    }
};

// Native annotation sidebar controls shared by PDF and EPUB.
static LRESULT CALLBACK AnnotationSecondaryButtonProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id,
                                                      DWORD_PTR refData) {
    bool combo = false;
    if (msg == WM_ERASEBKGND) {
        return 1;
    }
    if (msg == WM_MOUSEMOVE || msg == WM_MOUSELEAVE || msg == WM_SETFOCUS || msg == WM_KILLFOCUS || msg == WM_ENABLE ||
        msg == CB_SETCURSEL) {
        if (msg == WM_MOUSEMOVE) {
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&track);
        }
        LRESULT result = DefSubclassProc(hwnd, msg, wp, lp);
        InvalidateRect(hwnd, nullptr, FALSE);
        return result;
    }
    if (msg == WM_PAINT || msg == WM_PRINTCLIENT) {
        PAINTSTRUCT ps{};
        HDC dc = msg == WM_PAINT ? BeginPaint(hwnd, &ps) : (HDC)wp;
        RECT rc{};
        GetClientRect(hwnd, &rc);
        POINT cursor{};
        GetCursorPos(&cursor);
        ScreenToClient(hwnd, &cursor);
        bool hot = PtInRect(&rc, cursor) != FALSE;
        bool enabled = IsWindowEnabled(hwnd) != FALSE;
        bool focused = HwndIsFocused(hwnd);
        COLORREF panel = ThemeWindowControlBackgroundColor();
        COLORREF fill = hot && enabled ? ThemeInspectorHoverBackgroundColor() : panel;
        ScopedGdiObj<HBRUSH> brush(CreateSolidBrush(fill));
        FillRect(dc, &rc, brush);
        {
            COLORREF border = ThemeInspectorSeparatorColor();
            if ((hot || focused) && enabled) {
                border = ThemeUsesDarkChrome() ? ThemeWindowLinkColor() : ThemeInspectorSecondaryTextColor();
            }
            ScopedGdiObj<HBRUSH> rim(CreateSolidBrush(border));
            FrameRect(dc, &rc, rim);
        }
        HFONT font = GetWindowFont(hwnd);
        HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, (enabled || refData != 0) ? ThemeWindowTextColor() : ThemeWindowTextDisabledColor());
        RECT text = rc;
        int pad = DpiScale(hwnd, 6);
        text.left += pad;
        text.right -= pad;
        TempWStr value = HwndGetTextWTemp(hwnd);
        DrawTextW(dc, value, -1, &text,
                  DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX |
                      ((combo || GetPropW(hwnd, L"AnnotLeftAligned")) ? DT_LEFT : DT_CENTER));
        if (oldFont) {
            SelectObject(dc, oldFont);
        }
        if (msg == WM_PAINT) {
            EndPaint(hwnd, &ps);
        }
        return 0;
    }
    if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, AnnotationSecondaryButtonProc, id);
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

struct AnnotHeadingLine : Wnd {
    HFONT titleFont = nullptr;
    HFONT metaFont = nullptr;
    char* title = nullptr;
    char* meta = nullptr;
    int titleColW = 0;
    Func0 onClick;

    ~AnnotHeadingLine() override {
        str::Free(title);
        str::Free(meta);
    }

    void SetParts(const char* titleText, const char* metaText) {
        if (str::Eq(title, titleText) && str::Eq(meta, metaText)) return;
        str::ReplaceWithCopy(&title, titleText);
        str::ReplaceWithCopy(&meta, metaText);
        if (hwnd) {
            InvalidateRect(hwnd, nullptr, FALSE);
        }
    }

    int MeasureTitlePx() {
        if (!hwnd || str::IsEmpty(title)) {
            return 0;
        }
        HDC hdc = GetDC(hwnd);
        if (!hdc) {
            return 0;
        }
        HFONT tf = titleFont ? titleFont : GetFont();
        HGDIOBJ old = SelectObject(hdc, tf);
        WCHAR* s = ToWStrTemp(title);
        SIZE sz{};
        GetTextExtentPoint32W(hdc, s, (int)wcslen(s), &sz);
        SelectObject(hdc, old);
        ReleaseDC(hwnd, hdc);
        return sz.cx;
    }

    Size GetIdealSize() override {
        if (!hwnd) {
            return {8, 16};
        }
        HDC hdc = GetDC(hwnd);
        if (!hdc) {
            return {8, DpiScale(hwnd, 16)};
        }
        HFONT tf = titleFont ? titleFont : GetFont();
        HFONT mf = metaFont ? metaFont : tf;
        int h = 0;
        int w = 0;
        HGDIOBJ old = SelectObject(hdc, tf);
        TEXTMETRIC tm{};
        GetTextMetrics(hdc, &tm);
        h = tm.tmHeight;
        SIZE titleSz{};
        if (!str::IsEmpty(title)) {
            WCHAR* s = ToWStrTemp(title);
            GetTextExtentPoint32W(hdc, s, (int)wcslen(s), &titleSz);
        }
        int titleW = titleColW > 0 ? titleColW : titleSz.cx;
        w = titleW;
        SelectObject(hdc, mf);
        GetTextMetrics(hdc, &tm);
        if (tm.tmHeight > h) {
            h = tm.tmHeight;
        }
        if (!str::IsEmpty(meta)) {
            WCHAR* s = ToWStrTemp(meta);
            SIZE sz{};
            GetTextExtentPoint32W(hdc, s, (int)wcslen(s), &sz);
            w += DpiScale(hwnd, 8) + sz.cx;
        }
        SelectObject(hdc, old);
        ReleaseDC(hwnd, hdc);
        if (h < 1) {
            h = DpiScale(hwnd, 16);
        }
        if (w < 8) {
            w = 8;
        }
        return {w, h};
    }

    LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) override {
        if (msg == WM_ERASEBKGND) return 1;
        return Wnd::WndProc(hwnd, msg, wp, lp);
    }

    void OnPaint(HDC target, PAINTSTRUCT*) override {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        if (rc.right <= 0 || rc.bottom <= 0) return;
        HDC memory = CreateCompatibleDC(target);
        HBITMAP bitmap = CreateCompatibleBitmap(target, rc.right, rc.bottom);
        if (!memory || !bitmap) {
            if (memory) DeleteDC(memory);
            if (bitmap) DeleteObject(bitmap);
            PaintHeading(target);
            return;
        }
        HGDIOBJ old = SelectObject(memory, bitmap);
        PaintHeading(memory);
        BitBlt(target, 0, 0, rc.right, rc.bottom, memory, 0, 0, SRCCOPY);
        SelectObject(memory, old);
        DeleteObject(bitmap);
        DeleteDC(memory);
    }

    void PaintHeading(HDC hdc) {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        COLORREF bg = bgColor != kColorUnset ? bgColor : ThemeWindowControlBackgroundColor();
        COLORREF fg = textColor != kColorUnset ? textColor : ThemeWindowTextColor();
        ScopedGdiObj<HBRUSH> br(CreateSolidBrush(bg));
        FillRect(hdc, &rc, br);
        SetBkMode(hdc, TRANSPARENT);
        HFONT tf = titleFont ? titleFont : GetFont();
        HFONT mf = metaFont ? metaFont : tf;
        HGDIOBJ old = SelectObject(hdc, tf);
        UINT titleFmt = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX;
        RECT tr = rc;
        SIZE titleSz{};
        WCHAR* wt = ToWStrTemp(title ? title : "");
        GetTextExtentPoint32W(hdc, wt, (int)wcslen(wt), &titleSz);
        bool hasMeta = !str::IsEmpty(meta);
        int gap = DpiScale(hwnd, 8);
        int titleW = titleColW > 0 ? titleColW : titleSz.cx;
        if (hasMeta) {
            int minMeta = DpiScale(hwnd, 48);
            if (rc.left + titleW + gap + minMeta > rc.right) {
                titleW = std::max(0, (int)rc.right - (int)rc.left - minMeta - gap);
                titleFmt |= DT_END_ELLIPSIS;
            }
            tr.right = rc.left + titleW;
        } else {
            titleFmt |= DT_END_ELLIPSIS;
        }
        SetTextColor(hdc, fg);
        if (!str::IsEmpty(title)) {
            DrawTextW(hdc, wt, -1, &tr, titleFmt);
        }
        if (hasMeta) {
            RECT mr = rc;
            mr.left = rc.left + titleW + gap;
            if (mr.left < rc.right) {
                SelectObject(hdc, mf);
                SetTextColor(hdc, ThemeInspectorSecondaryTextColor());
                DrawTextW(hdc, ToWStrTemp(meta), -1, &mr,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            }
        }
        SelectObject(hdc, old);
    }

    LRESULT OnMouseEvent(UINT msg, WPARAM wparam, LPARAM lparam) override {
        if (msg == WM_LBUTTONUP && onClick.IsValid()) {
            onClick.Call();
            return 0;
        }
        return Wnd::OnMouseEvent(msg, wparam, lparam);
    }
};

struct AnnotSidebarList : ListBox {
    Func1<int> onDelete;
    int pressedAction = -1;
    int hotAction = -1;
    Tooltip deleteTooltip;
    bool paintingBuffered = false;

    void InvalidateAction(int idx) {
        if (idx < 0) return;
        RECT row{};
        if (SendMessageW(hwnd, LB_GETITEMRECT, idx, (LPARAM)&row) == LB_ERR) return;
        RECT action = ActionRect(hwnd, row);
        InvalidateRect(hwnd, &action, FALSE);
    }

    LRESULT OnMessageReflect(UINT msg, WPARAM wp, LPARAM lp) override {
        if (msg != WM_DRAWITEM || paintingBuffered) return ListBox::OnMessageReflect(msg, wp, lp);
        auto item = (DRAWITEMSTRUCT*)lp;
        InvalidateRect(hwnd, &item->rcItem, FALSE);
        return TRUE;
    }

    static RECT ActionRect(HWND hwnd, RECT row) {
        // The native client width already excludes a visible scrollbar. Keep
        // the same gutter when it is hidden so the action column never moves.
        if (!(GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_VSCROLL)) {
            row.right = std::max(row.left, row.right - GetSystemMetrics(SM_CXVSCROLL));
        }
        row.left = std::max(row.left, row.right - DpiScale(hwnd, 26));
        return row;
    }

    int ActionAt(LPARAM lp) {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        LRESULT hit = SendMessageW(hwnd, LB_ITEMFROMPOINT, 0, lp);
        if (HIWORD(hit) || LOWORD(hit) >= GetCount()) return -1;
        RECT row{};
        if (SendMessageW(hwnd, LB_GETITEMRECT, LOWORD(hit), (LPARAM)&row) == LB_ERR) return -1;
        row = ActionRect(hwnd, row);
        return PtInRect(&row, pt) ? LOWORD(hit) : -1;
    }

    void DeleteItem(int idx) {
        if (idx >= 0 && idx < GetCount() && onDelete.IsValid()) onDelete.Call(idx);
    }

    LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) override {
        if (msg == WM_ERASEBKGND) return 1;
        if (msg == WM_PAINT) {
            RECT client{};
            GetClientRect(hwnd, &client);
            if (client.right > 0 && client.bottom > 0) {
                HDC target = GetDC(hwnd);
                HDC memory = CreateCompatibleDC(target);
                HBITMAP bitmap = CreateCompatibleBitmap(target, client.right, client.bottom);
                ReleaseDC(hwnd, target);
                if (memory && bitmap) {
                    PAINTSTRUCT paint{};
                    target = BeginPaint(hwnd, &paint);
                    HGDIOBJ old = SelectObject(memory, bitmap);
                    COLORREF color =
                        ThemeUsesDarkChrome() ? ThemeWindowBackgroundColor() : ThemeWindowControlBackgroundColor();
                    ScopedGdiObj<HBRUSH> brush(CreateSolidBrush(color));
                    FillRect(memory, &client, brush);
                    paintingBuffered = true;
                    int selection = GetCurrentSelection();
                    int count = GetCount();
                    for (int i = (int)SendMessageW(hwnd, LB_GETTOPINDEX, 0, 0); i >= 0 && i < count; i++) {
                        DRAWITEMSTRUCT item{};
                        item.CtlType = ODT_LISTBOX;
                        item.hwndItem = hwnd;
                        item.hDC = memory;
                        item.itemID = i;
                        item.itemAction = ODA_DRAWENTIRE;
                        if (SendMessageW(hwnd, LB_GETITEMRECT, i, (LPARAM)&item.rcItem) == LB_ERR ||
                            item.rcItem.top >= client.bottom)
                            break;
                        if (i == selection) {
                            item.itemState = ODS_SELECTED;
                            if (HwndIsFocused(hwnd)) item.itemState |= ODS_FOCUS;
                        }
                        ListBox::OnMessageReflect(WM_DRAWITEM, 0, (LPARAM)&item);
                    }
                    paintingBuffered = false;
                    RECT& r = paint.rcPaint;
                    BitBlt(target, r.left, r.top, r.right - r.left, r.bottom - r.top, memory, r.left, r.top, SRCCOPY);
                    SelectObject(memory, old);
                    DeleteObject(bitmap);
                    DeleteDC(memory);
                    EndPaint(hwnd, &paint);
                    return 0;
                }
                if (bitmap) DeleteObject(bitmap);
                if (memory) DeleteDC(memory);
            }
        }
        if (msg == WM_MOUSEMOVE || msg == WM_MOUSELEAVE) {
            int action = msg == WM_MOUSEMOVE ? ActionAt(lp) : -1;
            if (action != hotAction) {
                int previous = hotAction;
                hotAction = action;
                InvalidateAction(previous);
                InvalidateAction(action);
                if (!deleteTooltip.hwnd) {
                    Tooltip::CreateArgs args;
                    args.parent = hwnd;
                    deleteTooltip.Create(args);
                }
                deleteTooltip.Delete();
                if (action >= 0) {
                    RECT row{};
                    SendMessageW(hwnd, LB_GETITEMRECT, action, (LPARAM)&row);
                    row = ActionRect(hwnd, row);
                    deleteTooltip.SetSingle(_TRA("Delete Annotation"),
                                            Rect(row.left, row.top, row.right - row.left, row.bottom - row.top), false);
                }
            }
        }
        if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK) {
            int action = ActionAt(lp);
            if (action >= 0) {
                pressedAction = action;
                SetCapture(hwnd);
                return 0;
            }
        }
        if (msg == WM_LBUTTONUP && pressedAction >= 0) {
            int action = pressedAction;
            pressedAction = -1;
            ReleaseCapture();
            if (ActionAt(lp) == action) DeleteItem(action);
            return 0;
        }
        if (msg == WM_CAPTURECHANGED || msg == WM_CANCELMODE) pressedAction = -1;
        if (msg == WM_KEYDOWN && wp == VK_DELETE) {
            DeleteItem(GetCurrentSelection());
            return 0;
        }
        if (msg == WM_CONTEXTMENU) {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            int idx = GetCurrentSelection();
            if (pt.x != -1 || pt.y != -1) {
                POINT client = pt;
                ScreenToClient(hwnd, &client);
                LRESULT hit = SendMessageW(hwnd, LB_ITEMFROMPOINT, 0, MAKELPARAM(client.x, client.y));
                idx = HIWORD(hit) ? -1 : LOWORD(hit);
            } else {
                RECT row{};
                SendMessageW(hwnd, LB_GETITEMRECT, idx, (LPARAM)&row);
                pt = {row.left, row.bottom};
                ClientToScreen(hwnd, &pt);
            }
            if (idx >= 0 && idx < GetCount()) {
                HMENU menu = CreatePopupMenu();
                AppendMenuW(menu, MF_STRING, 1, ToWStrTemp(_TRA("Delete Annotation")));
                UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, pt.x, pt.y, 0,
                                          GetParent(hwnd), nullptr);
                DestroyMenu(menu);
                if (cmd == 1) DeleteItem(idx);
            }
            return 0;
        }
        if (msg == WM_SETFOCUS || msg == WM_KILLFOCUS) InvalidateRect(hwnd, nullptr, FALSE);
        return ListBox::WndProc(hwnd, msg, wp, lp);
    }

    void SetBounds(Rect bounds) override {
        // Like LayoutTocContainer's tree/bookmarks, the sidebar-level list
        // ends at the container client edge, not the inspector's right padding.
        // Win32 reserves the scrollbar gutter from the resulting client width.
        Rect client = ClientRect(GetParent(hwnd));
        bounds.dx = std::max(0, client.dx - bounds.x);
        ListBox::SetBounds(bounds);
    }
};

struct AnnotNoteEdit : Edit {
    int preferredHeight = 140;
    // Light theme: our own sunken edge. The themed client edge puts the dark
    // line on the bottom, so the note reads as raised.
    bool sunkenEdge = false;

    Size GetIdealSize() override {
        Size size = Edit::GetIdealSize();
        size.dy = DpiScale(hwnd, preferredHeight);
        return size;
    }

    void HideNoteScrollbar() {
        if (!hwnd) {
            return;
        }
        LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
        if ((style & ES_AUTOVSCROLL) == 0) {
            SetWindowLongPtrW(hwnd, GWL_STYLE, style | ES_AUTOVSCROLL);
        }
        ShowScrollBarIfChanged(hwnd, SB_VERT, FALSE);
    }

    void OnSize(UINT msg, UINT type, SIZE size) override {
        Edit::OnSize(msg, type, size);
        HideNoteScrollbar();
    }

    LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) override {
        bool lightEdge = sunkenEdge && !ThemeUsesDarkChrome();
        if (msg == WM_NCCALCSIZE) {
            // Sunken edge on the top, left and bottom. No scrollbar column.
            RECT* rc = wp ? &((NCCALCSIZE_PARAMS*)lp)->rgrc[0] : (RECT*)lp;
            if (lightEdge) {
                rc->left += 2;
                rc->top += 2;
                rc->bottom -= 2;
            }
            int windowRight = rc->right;
            LRESULT res = Edit::WndProc(hwnd, msg, wp, lp);
            rc->right = windowRight;
            return res;
        }
        if (msg == WM_NCPAINT && lightEdge) {
            LRESULT res = Edit::WndProc(hwnd, msg, wp, lp);
            PaintSunkenNoteEdge();
            return res;
        }
        // Wheel still moves the text when there is no bar.
        if (msg == WM_MOUSEWHEEL) {
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            UINT lines = 3;
            SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
            if (lines == WHEEL_PAGESCROLL) {
                lines = 8;
            }
            int steps = delta / WHEEL_DELTA;
            if (steps == 0) {
                steps = delta > 0 ? 1 : -1;
            }
            SendMessageW(hwnd, EM_LINESCROLL, 0, (LPARAM)(-steps * (int)lines));
            return 0;
        }
        LRESULT res = Edit::WndProc(hwnd, msg, wp, lp);
        if (msg == WM_PAINT || msg == WM_VSCROLL) {
            HideNoteScrollbar();
        }
        return res;
    }

    void PaintSunkenNoteEdge() {
        RECT wr{};
        if (!GetWindowRect(hwnd, &wr)) {
            return;
        }
        int w = wr.right - wr.left;
        int h = wr.bottom - wr.top;
        if (w < 4 || h < 4) {
            return;
        }
        HDC hdc = GetWindowDC(hwnd);
        if (!hdc) {
            return;
        }
        // Light from the top-left: shadow on the top and left, highlight on the bottom.
        COLORREF shadow = RGB(168, 168, 168);
        COLORREF innerShadow = RGB(214, 214, 214);
        COLORREF highlight = RGB(255, 255, 255);
        COLORREF innerHi = RGB(250, 250, 250);
        auto fill = [&](int x, int y, int dx, int dy, COLORREF c) {
            if (dx <= 0 || dy <= 0) {
                return;
            }
            RECT rc{x, y, x + dx, y + dy};
            ScopedGdiObj<HBRUSH> br(CreateSolidBrush(c));
            FillRect(hdc, &rc, br);
        };
        fill(0, h - 1, w, 1, highlight);
        fill(1, h - 2, w - 2, 1, innerHi);
        fill(0, 0, w, 1, shadow);
        fill(0, 0, 1, h, shadow);
        fill(w - 1, 1, 1, h - 1, highlight);
        fill(1, 1, w - 3, 1, innerShadow);
        fill(1, 1, 1, h - 2, innerShadow);
        fill(w - 2, 1, 1, h - 2, innerHi);
        ReleaseDC(hwnd, hdc);
    }
};

struct AnnotInspectorPane : Wnd {
    AnnotNoteEdit* note = nullptr;
    ILayout* inner = nullptr;

    LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) override {
        if (msg == WM_ERASEBKGND) {
            // The default window class erases with white before WM_PAINT.
            // Use the panel color even while startup document loading delays painting.
            COLORREF bg = bgColor != kColorUnset ? bgColor
                                                 : (ThemeUsesDarkChrome() ? ThemeWindowBackgroundColor()
                                                                          : ThemeWindowControlBackgroundColor());
            ScopedGdiObj<HBRUSH> brush(CreateSolidBrush(bg));
            RECT rc{};
            GetClientRect(hwnd, &rc);
            FillRect((HDC)wp, &rc, brush);
            return 1;
        }
        return Wnd::WndProc(hwnd, msg, wp, lp);
    }

    Size GetIdealSize() override { return {8, 0}; }

    void SetBounds(Rect bounds) override {
        // Full sidebar width. Controls, including the note, keep the inspector padding.
        Rect client = ClientRect(GetParent(hwnd));
        bounds.dx = std::max(0, client.dx - bounds.x);
        Wnd::SetBounds(bounds);
    }

    void RelayoutInner();
    void OnSize(UINT, UINT, SIZE) override {
        if (!gLayoutSuspendPaint) RelayoutInner();
    }
};

inline void AnnotInspectorPane::RelayoutInner() {
    auto* pane = this;
    if (!pane || !pane->hwnd || !pane->inner) {
        return;
    }
    RECT rc{};
    GetClientRect(pane->hwnd, &rc);
    int w = rc.right;
    int h = rc.bottom;
    if (w <= 0 || h <= 0) {
        return;
    }
    AnnotNoteEdit* note = pane->note;
    constexpr int kNotePref = 140;
    constexpr int kNoteMin = 48;
    if (!gLayoutSuspendPaint && note && note->GetVisibility() != Visibility::Collapse) {
        note->preferredHeight = kNotePref;
    }
    Constraints c;
    c.min = {w, 0};
    c.max = {w, Inf};
    Size sz = pane->inner->Layout(c);
    // Steal note height before anything else overflows the footer.
    if (sz.dy > h && !gLayoutSuspendPaint && note && note->GetVisibility() != Visibility::Collapse) {
        int overflow = sz.dy - h;
        int curPx = DpiScale(pane->hwnd, note->preferredHeight);
        int minPx = DpiScale(pane->hwnd, kNoteMin);
        int shrink = std::min(overflow, std::max(0, curPx - minPx));
        int nextPx = curPx - shrink;
        int dpi = DpiGet(pane->hwnd);
        if (dpi < 1) {
            dpi = 96;
        }
        note->preferredHeight = std::max(kNoteMin, MulDiv(nextPx, 96, dpi));
        sz = pane->inner->Layout(c);
    }
    bool suspended = gLayoutSuspendPaint;
    gLayoutSuspendPaint = true;
    pane->inner->SetBounds(Rect{0, 0, w, std::max(h, sz.dy)});
    gLayoutSuspendPaint = suspended;
    for (HWND child = GetWindow(pane->hwnd, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        if (GetPropW(child, L"AnnotResetButton")) {
            PlaceAnnotResetButton(child);
        }
    }
}

inline void LayoutAnnotationSidebarControls(LayoutBase* layout, AnnotInspectorPane* pane, Size size) {
    // Place the outer controls without painting or reentering the inspector
    // layout through WM_SIZE. Place its children once, then repaint together.
    bool suspended = gLayoutSuspendPaint;
    gLayoutSuspendPaint = true;
    LayoutToSize(layout, size);
    gLayoutSuspendPaint = suspended;
    if (pane) pane->RelayoutInner();
}
struct AnnotSizedButton : Button {
    int fixedDx = 0;
    Wnd* widthPeer = nullptr;

    Size GetIdealSize() override {
        Size s = Button::GetIdealSize();
        if (fixedDx > 0) {
            s.dx = fixedDx;
        }
        if (widthPeer) {
            s.dx = widthPeer->GetIdealSize().dx;
        }
        return s;
    }
};

struct AnnotColorDropDown : DropDown {
    int fixedDx = 0;

    HWND Create(const CreateArgs& args) {
        CreateControlArgs control;
        control.parent = args.parent;
        control.font = args.font;
        control.isRtl = args.isRtl;
        control.className = WC_COMBOBOX;
        control.style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS;
        Wnd::CreateControl(control);
        if (hwnd) {
            int height = HwndMeasureText(hwnd, "Ag", args.font).dy + DpiScale(hwnd, 6);
            SendMessageW(hwnd, CB_SETITEMHEIGHT, 0, height);
            SendMessageW(hwnd, CB_SETITEMHEIGHT, (WPARAM)-1, height);
            SetCurrentSelection(-1);
            SizeToIdealSize(this);
        }
        return hwnd;
    }

    LRESULT OnMessageReflect(UINT msg, WPARAM wp, LPARAM lp) override {
        if (msg != WM_DRAWITEM) {
            return DropDown::OnMessageReflect(msg, wp, lp);
        }
        auto* item = (DRAWITEMSTRUCT*)lp;
        if (item->itemID == (UINT)-1 || item->itemID >= (UINT)items.Size()) {
            return TRUE;
        }
        HDC dc = item->hDC;
        RECT rc = item->rcItem;
        bool selected = (item->itemState & ODS_SELECTED) != 0;
        COLORREF idleBg = ThemeWindowControlBackgroundColor();
        COLORREF bg = selected ? ThemeInspectorSelectedBackgroundColor() : idleBg;
        ScopedGdiObj<HBRUSH> background(CreateSolidBrush(bg));
        FillRect(dc, &rc, background);
        const char* value = items.At(item->itemID);
        int dot = DpiScale(hwnd, 10);
        int pad = DpiScale(hwnd, 6);
        int y = rc.top + (rc.bottom - rc.top - dot) / 2;
        if (!str::Eq(value, "Transparent")) {
            ScopedGdiObj<HBRUSH> color(CreateSolidBrush(GetAnnotationColorFromDropDown(value)));
            ScopedSelectObject selBrush(dc, color);
            ScopedSelectObject selPen(dc, GetStockObject(NULL_PEN));
            Ellipse(dc, rc.left + pad, y, rc.left + pad + dot, y + dot);
        } else {
            ScopedSelectObject selBrush(dc, GetStockObject(NULL_BRUSH));
            ScopedGdiObj<HPEN> pen(CreatePen(PS_SOLID, 1, ThemeInspectorSecondaryTextColor()));
            ScopedSelectObject selPen(dc, pen);
            Ellipse(dc, rc.left + pad, y, rc.left + pad + dot, y + dot);
        }
        ScopedSelectObject selFont(dc, GetWindowFont(hwnd));
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, selected ? ThemeInspectorSelectedTextColor() : ThemeWindowTextColor());
        rc.left += dot + pad * 2;
        rc.right -= pad;
        DrawTextW(dc, ToWStrTemp(trans::GetTranslation(value)), -1, &rc,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        return TRUE;
    }

    Size GetIdealSize() override {
        Size size = DropDown::GetIdealSize();
        // Owner-drawn face needs room for the swatch, both text margins and
        // the themed arrow area; native text-only combo sizing is too narrow.
        HFONT font = GetWindowFont(hwnd);
        int textWidth = HwndMeasureText(hwnd, "Transparent", font).dx;
        for (int i = 0; i < items.Size(); i++) {
            textWidth = std::max(textWidth, HwndMeasureText(hwnd, trans::GetTranslation(items.At(i)), font).dx);
        }
        size.dx = std::max(size.dx, textWidth + DpiScale(hwnd, 10 + 18 + 32));
        if (fixedDx > 0) {
            size.dx = std::max(size.dx, fixedDx);
        }
        int faceHeight = (int)SendMessageW(hwnd, CB_GETITEMHEIGHT, (WPARAM)-1, 0);
        if (faceHeight > 0) {
            size.dy = std::max(size.dy, faceHeight + DpiScale(hwnd, 4));
        }
        return size;
    }
};

struct AnnotationIconPreviewRequest {
    const char* name;
    int size;
    int lineEnding;
    bool lineStart;
    HBITMAP bitmap = nullptr;
};

struct AnnotIconDropDown : AnnotColorDropDown {
    bool lineEndPreview = false;
    bool lineStartPreview = false;
    Vec<HBITMAP> previews;
    AnnotationType previewType = AnnotationType::Unknown;
    COLORREF previewColor = 0;
    Func1<AnnotationIconPreviewRequest*> renderPreview;
    int previewSize = 0;

    ~AnnotIconDropDown() override { ClearPreviews(); }

    void ClearPreviews() {
        for (HBITMAP bitmap : previews) {
            if (bitmap) {
                DeleteObject(bitmap);
            }
        }
        previews.Reset();
    }

    void PreparePreviews(AnnotationType type, COLORREF color) {
        int size = DpiScale(hwnd, lineEndPreview ? 28 : 18);
        if (previewType == type && previewColor == color && previewSize == size && previews.Size() == items.Size()) {
            return;
        }
        ClearPreviews();
        previewType = type;
        previewColor = color;
        previewSize = size;
        for (int i = 0; i < items.Size(); i++) {
            AnnotationIconPreviewRequest request{items.At(i), size, lineEndPreview ? i : -1, lineStartPreview};
            renderPreview.Call(&request);
            previews.Append(request.bitmap);
        }
        int height = std::max(HwndMeasureText(hwnd, "Ag", GetWindowFont(hwnd)).dy, size) + DpiScale(hwnd, 6);
        SendMessageW(hwnd, CB_SETITEMHEIGHT, 0, height);
        SendMessageW(hwnd, CB_SETITEMHEIGHT, (WPARAM)-1, height);
        InvalidateRect(hwnd, nullptr, FALSE);
    }

    LRESULT OnMessageReflect(UINT msg, WPARAM wp, LPARAM lp) override {
        if (msg != WM_DRAWITEM) {
            return DropDown::OnMessageReflect(msg, wp, lp);
        }
        auto* item = (DRAWITEMSTRUCT*)lp;
        if (item->itemID == (UINT)-1 || item->itemID >= (UINT)items.Size()) {
            return TRUE;
        }
        HDC dc = item->hDC;
        RECT rc = item->rcItem;
        bool selected = (item->itemState & ODS_SELECTED) != 0;
        COLORREF idleBg = ThemeWindowControlBackgroundColor();
        ScopedGdiObj<HBRUSH> background(CreateSolidBrush(selected ? ThemeInspectorSelectedBackgroundColor() : idleBg));
        FillRect(dc, &rc, background);
        int pad = DpiScale(hwnd, 6);
        int size = DpiScale(hwnd, lineEndPreview ? 28 : 18);
        if (item->itemID < (UINT)previews.Size() && previews.At(item->itemID)) {
            HBITMAP bitmap = previews.At(item->itemID);
            BITMAP bm{};
            if (GetObject(bitmap, sizeof(bm), &bm) && bm.bmWidth > 0 && bm.bmHeight > 0) {
                HDC mem = CreateCompatibleDC(dc);
                if (mem) {
                    HGDIOBJ old = SelectObject(mem, bitmap);
                    int width = MulDiv(bm.bmWidth, size, std::max(bm.bmWidth, bm.bmHeight));
                    int height = MulDiv(bm.bmHeight, size, std::max(bm.bmWidth, bm.bmHeight));
                    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
                    AlphaBlend(dc, rc.left + pad + (size - width) / 2, rc.top + (rc.bottom - rc.top - height) / 2,
                               width, height, mem, 0, 0, bm.bmWidth, bm.bmHeight, blend);
                    SelectObject(mem, old);
                    DeleteDC(mem);
                }
            }
        }
        ScopedSelectObject font(dc, GetWindowFont(hwnd));
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, selected ? ThemeInspectorSelectedTextColor() : ThemeWindowTextColor());
        rc.left += size + pad * 2;
        rc.right -= pad;
        DrawTextW(dc, ToWStrTemp(items.At(item->itemID)), -1, &rc,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        return TRUE;
    }

    Size GetIdealSize() override {
        Size size = AnnotColorDropDown::GetIdealSize();
        size.dx += DpiScale(hwnd, 14);
        return size;
    }
};
struct AnnotationColorProperty {
    Static* label = nullptr;
    AnnotColorDropDown* value = nullptr;
};

inline AnnotationColorProperty AddAnnotationColorProperty(VBox* box, HWND parent, HFONT font, bool rtl,
                                                          const char* labelText, const char* items, Func0 changed) {
    auto row = new HBox();
    row->alignMain = MainAxisAlign::MainStart;
    row->alignCross = CrossAxisAlign::CrossCenter;
    auto label = new Static();
    Static::CreateArgs labelArgs;
    labelArgs.parent = parent;
    labelArgs.font = font;
    labelArgs.isRtl = rtl;
    labelArgs.text = labelText;
    label->Create(labelArgs);
    label->SetInsetsPt(12, 0, 0, 0);
    row->AddChild(label);
    row->AddChild(new Spacer(0, 0), 1);
    auto value = new AnnotColorDropDown();
    value->fixedDx =
        std::max(HwndMeasureText(parent, "Export Notes", font).dx, HwndMeasureText(parent, "导出笔记", font).dx) +
        DpiScale(parent, 16);
    value->SetInsetsPt(12, 0, 0, 0);
    DropDown::CreateArgs args{parent, font, rtl};
    value->Create(args);
    value->SetItemsSeqStrings(items);
    value->onSelectionChanged = changed;
    row->AddChild(value);
    box->AddChild(row);
    return {label, value};
}

inline void UpdateAnnotationContentsEditChrome(Edit* edit, AnnotNoteEdit* note = nullptr) {
    if (!edit || !edit->hwnd) {
        return;
    }
    if (note) {
        note->sunkenEdge = !ThemeUsesDarkChrome();
    }
    if (ThemeUsesDarkChrome()) {
        // Options-dialog edit chrome: DarkModeLib border + control fill.
        SetWindowExStyle(edit->hwnd, WS_EX_CLIENTEDGE, false);
        SetWindowPos(edit->hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        if (UseDarkModeLib()) {
            DarkMode::setCustomBorderForListBoxOrEditCtrlSubclass(edit->hwnd);
        }
        return;
    }
    // Themed WS_EX_CLIENTEDGE draws its dark pixel along the bottom. The note
    // paints that edge itself, with the shadow on the top and left.
    bool sunkenNote = note && note->sunkenEdge;
    SetWindowExStyle(edit->hwnd, WS_EX_CLIENTEDGE, !sunkenNote);
    SetWindowPos(edit->hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    if (UseDarkModeLib()) {
        DarkMode::removeCustomBorderForListBoxOrEditCtrlSubclass(edit->hwnd);
    }
}

struct AnnotCommandButton : Button {
    Tooltip tooltip;
    const char* tooltipText = nullptr;
    bool reserveSaveWidth = false;

    Size GetIdealSize() override {
        Size size = Button::GetIdealSize();
        if (reserveSaveWidth) {
            HFONT font = GetWindowFont(hwnd);
            int textWidth =
                std::max(HwndMeasureText(hwnd, _TRA("Save"), font).dx, HwndMeasureText(hwnd, _TRA("Saved"), font).dx);
            size.dx = std::max(size.dx, textWidth + DpiScale(hwnd, 16));
        }
        return size;
    }

    void SetBounds(Rect bounds) override {
        Button::SetBounds(bounds);
        if (tooltipText) {
            if (!tooltip.hwnd) {
                Tooltip::CreateArgs args;
                args.parent = hwnd;
                tooltip.Create(args);
            }
            tooltip.SetSingle(tooltipText, ClientRect(hwnd), false);
        }
    }
};

// Resets appearance (font, size, color, opacity, border, background, alignment),
// not the note. It sits on the trailing edge of the gap under the note, so it
// reads as the action for the controls below and not as a way to clear the note.
struct AnnotResetButton : Wnd {
    Func0 onClick;
    bool hot = false;
    bool pressed = false;
    HWND tip = nullptr;
    WCHAR tipText[160]{};

    ~AnnotResetButton() override {
        if (tip) {
            DestroyWindow(tip);
            tip = nullptr;
        }
    }

    HWND Create(HWND parent) {
        CreateCustomArgs args;
        args.parent = parent;
        args.style = WS_CHILD | WS_VISIBLE | WS_TABSTOP;
        args.pos = {0, 0, 18, 18};
        HWND created = CreateCustom(args);
        if (!created) {
            return nullptr;
        }
        SetPropW(created, L"AnnotResetButton", (HANDLE)1);
        tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASS, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                              CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, created, nullptr,
                              GetModuleHandle(nullptr), nullptr);
        if (tip) {
            TOOLINFOW info{};
            info.cbSize = sizeof(info);
            info.uFlags = TTF_IDISHWND | TTF_SUBCLASS | TTF_TRANSPARENT;
            info.hwnd = parent;
            info.uId = (UINT_PTR)created;
            wcsncpy_s(tipText, _TRW("Restore appearance"), _TRUNCATE);
            info.lpszText = tipText;
            SendMessageW(tip, TTM_ADDTOOLW, 0, (LPARAM)&info);
            SendMessageW(tip, TTM_SETMAXTIPWIDTH, 0, 240);
        }
        return created;
    }

    Size GetIdealSize() override {
        int side = hwnd ? MulDiv(DpiScale(hwnd, 22), 4, 5) : 18;
        return {side, side};
    }

    void OnPaint(HDC hdc, PAINTSTRUCT*) override {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        COLORREF bg = ThemeWindowControlBackgroundColor();
        if ((hot || pressed) && IsWindowEnabled(hwnd)) {
            bg = ThemeInspectorHoverBackgroundColor();
        }
        ScopedGdiObj<HBRUSH> brush(CreateSolidBrush(bg));
        FillRect(hdc, &rc, brush);
        COLORREF ink = !IsWindowEnabled(hwnd) ? ThemeWindowTextDisabledColor()
                       : hot                  ? ThemeWindowLinkColor()
                                              : ThemeInspectorSecondaryTextColor();
        int glyph = MulDiv(DpiScale(hwnd, 16), 4, 5);
        int side = (int)std::min(rc.right - rc.left, rc.bottom - rc.top);
        glyph = std::min(glyph, side);
        Rect dest((rc.right - rc.left - glyph) / 2, (rc.bottom - rc.top - glyph) / 2, glyph, glyph);
        DrawSvgIcon(hdc, dest, TbIcon::ResetAppearance, ink, bg);
    }

    LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) override {
        if (msg == WM_ERASEBKGND) {
            return 1;
        }
        if (msg == WM_MOUSEMOVE) {
            if (!hot) {
                hot = true;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&track);
        } else if (msg == WM_MOUSELEAVE) {
            hot = false;
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (msg == WM_SETCURSOR) {
            SetCursor(LoadCursor(nullptr, IDC_HAND));
            return TRUE;
        } else if (msg == WM_LBUTTONDOWN && IsWindowEnabled(hwnd)) {
            pressed = true;
            HwndSetFocus(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            SetCapture(hwnd);
            return 0;
        } else if (msg == WM_LBUTTONUP) {
            bool click = pressed;
            pressed = false;
            InvalidateRect(hwnd, nullptr, FALSE);
            if (GetCapture() == hwnd) {
                ReleaseCapture();
            }
            POINT pt{(int)(short)LOWORD(lp), (int)(short)HIWORD(lp)};
            RECT rc{};
            GetClientRect(hwnd, &rc);
            if (click && IsWindowEnabled(hwnd) && PtInRect(&rc, pt) && onClick.IsValid()) {
                onClick.Call();
            }
            return 0;
        } else if (msg == WM_KEYDOWN && (wp == VK_SPACE || wp == VK_RETURN)) {
            if (IsWindowEnabled(hwnd) && onClick.IsValid()) {
                onClick.Call();
            }
            return 0;
        }
        if (msg == WM_CAPTURECHANGED || msg == WM_CANCELMODE || msg == WM_ENABLE) {
            pressed = false;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return Wnd::WndProc(hwnd, msg, wp, lp);
    }
};

struct AnnotCommandBar : HBox {
    Button* exportButton = nullptr;
    Button* saveButton = nullptr;
    Button* copyButton = nullptr;

    int MinIntrinsicWidth(int) override { return 0; }
    static void SetLabel(Button* button, const char* text) {
        if (!str::Eq(HwndGetTextTemp(button->hwnd), text)) HwndSetText(button->hwnd, text);
    }
    Size Layout(Constraints bc) override {
        if (exportButton && copyButton) {
            SetLabel(exportButton, _TRA("Export Notes"));
            SetLabel(copyButton, _TRA("Save as…"));
            int gap = DpiScale(exportButton->hwnd, 8);
            int width = exportButton->GetIdealSize().dx + saveButton->GetIdealSize().dx +
                        copyButton->GetIdealSize().dx + gap * 2;
            if (width > bc.max.dx) SetLabel(exportButton, _TRA("Export"));
            width = exportButton->GetIdealSize().dx + saveButton->GetIdealSize().dx + copyButton->GetIdealSize().dx +
                    gap * 2;
            if (width > bc.max.dx) SetLabel(copyButton, "…");
        }
        return HBox::Layout(bc);
    }
};

inline AnnotCommandBar* AddAnnotationCommandBar(VBox* footer) {
    auto row = new AnnotCommandBar();
    row->alignMain = MainAxisAlign::MainStart;
    row->alignCross = CrossAxisAlign::CrossCenter;
    footer->AddChild(row);
    return row;
}

inline Button* AddAnnotationFooterAction(HBox* footer, HWND parent, HFONT font, bool rtl, const char* text,
                                         bool enabled, Func0 onClick) {
    Button::CreateArgs args;
    args.parent = parent;
    args.font = font;
    args.isRtl = rtl;
    args.text = text;
    auto button = new AnnotCommandButton();
    if (footer->ChildrenCount()) {
        // Export stays at the leading edge; the two save actions form a
        // trailing group. Only the space between these groups stretches.
        int flex = footer->ChildrenCount() == 1 ? 1 : 0;
        footer->AddChild(new Spacer(DpiScale(parent, 8), 0), flex);
    }
    HWND hwnd = button->Create(args);
    ReportIf(!hwnd);
    button->SetIsEnabled(enabled);
    button->onClick = onClick;
    footer->AddChild(button);
    return button;
}

struct AnnotationSidebarShell {
    VBox* root = nullptr;
    VBox* inspector = nullptr;
    VBox* footer = nullptr;
    AnnotSidebarList* list = nullptr;
    AnnotInspectorPane* pane = nullptr;
};

inline AnnotationSidebarShell CreateAnnotationSidebarShell(HWND parent, HFONT font, HFONT headingFont, bool rtl,
                                                           Func1<ListBox::DrawItemEvent*> draw, Func0 select,
                                                           const char* title) {
    AnnotationSidebarShell shell;
    COLORREF bg = ThemeUsesDarkChrome() ? ThemeWindowBackgroundColor() : ThemeWindowControlBackgroundColor();
    COLORREF text = ThemeWindowTextColor();
    auto root = new VBox();
    root->alignMain = MainAxisAlign::MainStart;
    root->alignCross = CrossAxisAlign::Stretch;
    shell.root = root;
    auto header = new Static();
    Static::CreateArgs headerArgs;
    headerArgs.parent = parent;
    headerArgs.font = headingFont;
    headerArgs.isRtl = rtl;
    headerArgs.text = title;
    header->Create(headerArgs);
    header->SetColors(text, bg);
    header->SetInsetsPt(0, 0, 6, 0);
    root->AddChild(header);

    ListBox::CreateArgs listArgs;
    listArgs.parent = parent;
    listArgs.font = font;
    listArgs.isRtl = rtl;
    listArgs.idealSizeLines = 7;
    listArgs.itemHeightExtra = 10;
    auto list = new AnnotSidebarList();
    list->onDrawItem = draw;
    list->Create(listArgs);
    list->SetModel(new ListBoxModelStrings());
    list->onSelectionChanged = select;
    list->SetColors(text, bg);
    // Soft gap under the list — no drawn hairline (bookmarks/favorites use space only).
    list->SetInsetsPt(0, 0, 8, 0);
    root->AddChild(list);
    shell.list = list;

    auto pane = new AnnotInspectorPane();
    CreateCustomArgs paneArgs;
    paneArgs.parent = parent;
    paneArgs.style = WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN;
    paneArgs.bgColor = bg;
    paneArgs.pos = {0, 0, 10, 10};
    pane->CreateCustom(paneArgs);
    // Present the inspector's native labels, combos and trackbars together.
    // Per-control buffering cannot hide intermediate erase/resize frames from
    // sibling controls while the sidebar splitter changes their widths.
    SetWindowLongPtrW(pane->hwnd, GWL_EXSTYLE, GetWindowLongPtrW(pane->hwnd, GWL_EXSTYLE) | WS_EX_COMPOSITED);
    auto inspector = new VBox();
    inspector->alignMain = MainAxisAlign::MainStart;
    inspector->alignCross = CrossAxisAlign::Stretch;
    pane->inner = new Padding(inspector, DpiScaledInsets(pane->hwnd, 0, 12, 0, 0));
    root->AddChild(pane, 1);
    shell.pane = pane;
    shell.inspector = inspector;
    auto footer = new VBox();
    footer->alignMain = MainAxisAlign::MainStart;
    footer->alignCross = CrossAxisAlign::Stretch;
    // Top padding instead of a separator rule above Save / Save As.
    root->AddChild(new Padding(footer, DpiScaledInsets(parent, 12, 0, 0, 0)));
    shell.footer = footer;
    return shell;
}
inline void LayoutAnnotationSidebar(HWND sidebar, LayoutBase* layout, ListBox* list, AnnotInspectorPane* pane,
                                    ILayout* footerLayout, int dx, int dy, bool liveDrag) {
    if (!layout || dx <= 0 || dy <= 0) return;
    // The list stays a short scannable block. Footer height is reserved first
    // so Save / Save As never leave the sidebar. Width-only live drags must
    // not recompute row counts or the inspector jumps vertically.
    if (list && list->hwnd && !liveDrag) {
        int row = list->GetItemHeight(0);
        if (row < 1) {
            row = DpiScale(sidebar, 28);
        }
        int footerH = footerLayout ? footerLayout->MinIntrinsicHeight(dx) : DpiScale(sidebar, 88);
        int chrome = DpiScale(sidebar, 16) + DpiScale(sidebar, 28) + DpiScale(sidebar, 13) + footerH;
        int inspectorMin = DpiScale(sidebar, 80);
        int budget = dy - chrome;
        // Fixed viewport. Extra annotations scroll inside the list, so the
        // property controls below stay put when the count changes.
        int listPref = DpiScale(sidebar, 200);
        int listMax = DpiScale(sidebar, 220);
        int listMin = row * 3;
        int listPx = listPref;
        if (budget - listPx < inspectorMin) {
            listPx = budget - inspectorMin;
        }
        listPx = limitValue(listPx, listMin, listMax);
        if (budget < listMin + inspectorMin) {
            int leftover = budget - inspectorMin;
            if (leftover < row * 2) {
                leftover = row * 2;
            }
            listPx = leftover;
        }
        int lines = listPx / row;
        if (lines < 2) {
            lines = 2;
        }
        if (lines > 8) {
            lines = 8;
        }
        list->idealSizeLines = lines;
    }
    if (false && layout->lastBounds.EqSize(dx, dy)) {
        // avoid un-necessary layout
        return;
    }
    if (liveDrag) {
        gLayoutSuspendPaint = true;
    }
    LayoutAnnotationSidebarControls(layout, pane, {dx, dy});
    if (liveDrag) {
        gLayoutSuspendPaint = false;
        // RelayoutFrame's coalesced UPDATENOW paints this column once.
    } else {
        RedrawWindow(sidebar, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    }
}
inline void DrawAnnotationSidebarRow(HWND sidebar, HWND list, ListBox::DrawItemEvent* ev, bool selected, bool hasColor,
                                     COLORREF c, const char* location, const char* typeName, const char* excerpt,
                                     int locationWidth) {
    RECT rc = ev->itemRect;
    RECT action = AnnotSidebarList::ActionRect(list, rc);
    COLORREF bg = ThemeUsesDarkChrome() ? ThemeWindowBackgroundColor() : ThemeWindowControlBackgroundColor();
    COLORREF text = ThemeWindowTextColor();
    if (selected) {
        bg = ThemeInspectorSelectedBackgroundColor();
        text = ThemeInspectorSelectedTextColor();
    } else if (ev->hot) {
        bg = ThemeInspectorHoverBackgroundColor();
    }
    ScopedGdiObj<HBRUSH> br(CreateSolidBrush(bg));
    FillRect(ev->hdc, &rc, br);

    int barW = std::max(2, DpiScale(sidebar, 2));
    if (hasColor) {
        HFONT rowFont = GetWindowFont(list);
        // Font line height includes space above/below the visible glyphs.
        int textHeight = std::max(1, HwndMeasureText(list, "Ag", rowFont).dy - DpiScale(list, 4));
        int barHeight = std::min(textHeight, (int)(rc.bottom - rc.top));
        RECT bar = rc;
        bar.right = bar.left + barW;
        bar.top += (rc.bottom - rc.top - barHeight) / 2;
        bar.bottom = bar.top + barHeight;
        if (bar.bottom > bar.top) {
            u8 r, g, b;
            UnpackColor(c, r, g, b);
            Gdiplus::Graphics gs(ev->hdc);
            gs.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            Gdiplus::SolidBrush barBr(Gdiplus::Color(255, r, g, b));
            float x = (float)bar.left;
            float y = (float)bar.top;
            float w = (float)barW;
            float h = (float)(bar.bottom - bar.top);
            float d = w;
            if (h < d) {
                d = h;
            }
            Gdiplus::GraphicsPath pill;
            pill.AddArc(x, y, d, d, 180.f, 90.f);
            pill.AddArc(x + w - d, y, d, d, 270.f, 90.f);
            pill.AddArc(x + w - d, y + h - d, d, d, 0.f, 90.f);
            pill.AddArc(x, y + h - d, d, d, 90.f, 90.f);
            pill.CloseFigure();
            gs.FillPath(&barBr, &pill);
        }
    }

    HFONT font = (HFONT)SendMessageW(list, WM_GETFONT, 0, 0);
    HGDIOBJ oldFont = font ? SelectObject(ev->hdc, font) : nullptr;
    SetBkMode(ev->hdc, TRANSPARENT);
    SetTextColor(ev->hdc, text);

    int pageW = locationWidth;
    RECT pageRc = rc;
    pageRc.left += barW + DpiScale(sidebar, 6);
    pageRc.right = pageRc.left + pageW;
    DrawTextW(ev->hdc, ToWStrTemp(location), -1, &pageRc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    RECT typeRc = rc;
    typeRc.left = pageRc.right + DpiScale(sidebar, 8);
    typeRc.right = action.left - DpiScale(sidebar, 4);
    RECT excerptRc = typeRc;
    typeRc.right = std::min(typeRc.right, typeRc.left + HwndMeasureText(list, typeName, font).dx);
    excerptRc.left = typeRc.right + DpiScale(sidebar, 6);
    DrawTextW(ev->hdc, ToWStrTemp(typeName), -1, &typeRc,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    if (excerptRc.right > excerptRc.left && !str::IsEmpty(excerpt)) {
        SetTextColor(ev->hdc, selected ? text : ThemeInspectorSecondaryTextColor());
        DrawTextW(ev->hdc, ToWStrTemp(excerpt), -1, &excerptRc,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    if (ev->focused && HwndIsFocused(list)) {
        RECT focus = typeRc;
        focus.right = std::min(focus.right, focus.left + HwndMeasureText(list, typeName, font).dx);
        focus.bottom -= DpiScale(sidebar, 3);
        focus.top = focus.bottom - 1;
        ScopedGdiObj<HBRUSH> cue(CreateSolidBrush(ThemeInspectorSeparatorColor()));
        FillRect(ev->hdc, &focus, cue);
    }
    {
        POINT cursor{};
        GetCursorPos(&cursor);
        ScreenToClient(list, &cursor);
        bool hot = PtInRect(&action, cursor) != FALSE;
        COLORREF icon = hot ? ThemeWindowLinkColor() : ThemeInspectorSecondaryTextColor();
        int size = DpiScale(list, 16);
        Rect dest(action.left + (action.right - action.left - size) / 2,
                  action.top + (action.bottom - action.top - size) / 2, size, size);
        // Same outline as the sidebar trash SVG, without a raster background
        // that can differ from the selected/hovered row in Warm and Dracula.
        u8 red, green, blue;
        UnpackColor(icon, red, green, blue);
        Gdiplus::Graphics graphics(ev->hdc);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        graphics.TranslateTransform((float)dest.x, (float)dest.y);
        graphics.ScaleTransform((float)dest.dx / 24.f, (float)dest.dy / 24.f);
        Gdiplus::Pen pen(Gdiplus::Color(255, red, green, blue), 1.f);
        pen.SetStartCap(Gdiplus::LineCapRound);
        pen.SetEndCap(Gdiplus::LineCapRound);
        pen.SetLineJoin(Gdiplus::LineJoinRound);
        Gdiplus::GraphicsPath path;
        path.AddLine(4.f, 7.f, 20.f, 7.f);
        path.StartFigure();
        path.AddLine(9.f, 7.f, 9.f, 4.f);
        path.AddLine(9.f, 4.f, 15.f, 4.f);
        path.AddLine(15.f, 4.f, 15.f, 7.f);
        path.StartFigure();
        path.AddLine(6.f, 7.f, 7.f, 20.f);
        path.AddLine(7.f, 20.f, 17.f, 20.f);
        path.AddLine(17.f, 20.f, 18.f, 7.f);
        path.StartFigure();
        path.AddLine(10.f, 10.f, 10.f, 17.f);
        path.StartFigure();
        path.AddLine(14.f, 10.f, 14.f, 17.f);
        graphics.DrawPath(&pen, &path);
    }
    if (oldFont) {
        SelectObject(ev->hdc, oldFont);
    }
}
