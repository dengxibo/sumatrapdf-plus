/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "wingui/DialogSizer.h"
#include "utils/WinUtil.h"
#include "utils/Dpi.h"
#include "utils/ThreadUtil.h"
#include "utils/UITask.h"
#include "utils/JsonParser.h"
#include "utils/WinDynCalls.h"

#include "Settings.h"
#include "AppSettings.h"

#include "GlobalPrefs.h"
#include "EbookInstalledFonts.h"

#include "Annotation.h"
#include "SumatraPDF.h"
#include "resource.h"
#include "Commands.h"
#include "AppTools.h"
#include "SumatraDialogs.h"
#include "Translations.h"
#include "Theme.h"
#include "AppDialogTheme.h"
#include "DarkModeSubclass.h"
#include "AiTocApi.h"
#include "InlineTranslateLang.h"
#include "InlineTranslate.h"

// Modeless Enter/Esc routing (defined in wingui/Wnd.cpp).
HWND GetCurrentModelessDialog();
void SetCurrentModelessDialog(HWND);

// http://msdn.microsoft.com/en-us/library/ms645398(v=VS.85).aspx
#pragma pack(push, 1)
struct DLGTEMPLATEEX {
    WORD dlgVer;    // 0x0001
    WORD signature; // 0xFFFF
    DWORD helpID;
    DWORD exStyle;
    DWORD style;
    WORD cDlgItems;
    short x, y, cx, cy;
    /*
    sz_Or_Ord menu;
    sz_Or_Ord windowClass;
    WCHAR     title[titleLen];
    WORD      pointsize;
    WORD      weight;
    BYTE      italic;
    BYTE      charset;
    WCHAR     typeface[stringLen];
    */
};
#pragma pack(pop)

DLGTEMPLATE* DupTemplate(int dlgId) {
    HRSRC dialogRC = FindResourceW(nullptr, MAKEINTRESOURCE(dlgId), RT_DIALOG);
    ReportIf(!dialogRC);
    HGLOBAL dlgTemplate = LoadResource(nullptr, dialogRC);
    ReportIf(!dlgTemplate);
    void* orig = LockResource(dlgTemplate);
    size_t size = SizeofResource(nullptr, dialogRC);
    ReportIf(size == 0);
    DLGTEMPLATE* ret = (DLGTEMPLATE*)memdup(orig, size);
    UnlockResource(orig);
    return ret;
}

/*
Type: sz_Or_Ord

A variable-length array of 16-bit elements that identifies a menu resource for the dialog box. If the first element of
this array is 0x0000, the dialog box has no menu and the array has no other elements. If the first element is 0xFFFF,
the array has one additional element that specifies the ordinal value of a menu resource in an executable file. If the
first element has any other value, the system treats the array as a null-terminated Unicode string that specifies the
name of a menu resource in an executable file.
*/
static u8* SkipSzOrOrd(u8* d) {
    WORD* pw = (WORD*)d;
    WORD w = *pw++;
    if (w == 0x0000) {
        // no menu
    } else if (w == 0xffff) {
        // menu id followed by another WORD item
        pw++;
    } else {
        // anything else: zero-terminated WCHAR*
        WCHAR* s = (WCHAR*)pw;
        while (*s) {
            s++;
        }
        s++;
        pw = (WORD*)s;
    }
    return (u8*)pw;
}

static u8* SkipSz(u8* d) {
    WCHAR* s = (WCHAR*)d;
    while (*s) {
        s++;
    }
    s++;
    return (u8*)s;
}

static bool IsDlgTemplateEx(DLGTEMPLATE* tpl) {
    return tpl->style == MAKELONG(0x0001, 0xFFFF);
}

static bool HasDlgTemplateExFont(DLGTEMPLATEEX* tpl) {
    DWORD style = tpl->style & (DS_SETFONT | DS_FIXEDSYS);
    return style != 0;
}

// gets a dialog template from the resources and sets the RTL flag
// cf. http://www.ureader.com/msg/1484387.aspx
static void SetDlgTemplateRtl(DLGTEMPLATE* tpl) {
    if (IsDlgTemplateEx(tpl)) {
        ((DLGTEMPLATEEX*)tpl)->exStyle |= WS_EX_LAYOUTRTL;
    } else {
        tpl->dwExtendedStyle |= WS_EX_LAYOUTRTL;
    }
}

static int ToFontPointSize(int fontSize) {
    int res = (fontSize * 72) / 96;
    return res;
}

// https://stackoverflow.com/questions/14370238/can-i-dynamically-change-the-font-size-of-a-dialog-window-created-with-c-in-vi
// TODO: if changing font name would have do more complicated dance of replacing
// variable string in the middle of the struct
static void SetDlgTemplateExFont(DLGTEMPLATE* tmp, bool isRtl, int fontSize) {
    ReportIf(!IsDlgTemplateEx(tmp));
    if (isRtl) {
        SetDlgTemplateRtl(tmp);
    }
    DLGTEMPLATEEX* tpl = (DLGTEMPLATEEX*)tmp;
    ReportIf(!HasDlgTemplateExFont(tpl));
    u8* d = (u8*)tpl;
    d += sizeof(DLGTEMPLATEEX);
    // sz_Or_Ord menu
    d = SkipSzOrOrd(d);
    // sz_Or_Ord windowClass;
    d = SkipSzOrOrd(d);
    // WCHAR[] title
    d = SkipSz(d);
    // WCHAR pointSize;
    WORD* wd = (WORD*)d;
    fontSize = ToFontPointSize(fontSize);
    *wd = fontSize;
}

DLGTEMPLATE* GetRtLDlgTemplate(int dlgId) {
    DLGTEMPLATE* tpl = DupTemplate(dlgId);
    SetDlgTemplateRtl(tpl);
    return tpl;
}

struct AppDialogInit {
    DLGPROC proc;
    LPARAM data;
};

static INT_PTR CALLBACK AppDialogDispatch(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    constexpr const wchar_t* prop = L"SumatraAppDialogProc";
    DLGPROC proc = (DLGPROC)GetPropW(hwnd, prop);
    if (msg == WM_INITDIALOG) {
        auto* init = (AppDialogInit*)lp;
        proc = init->proc;
        SetPropW(hwnd, prop, (HANDLE)proc);
        INT_PTR result = proc(hwnd, msg, wp, init->data);
        if (IsWindow(hwnd)) {
            // Run after dialog-specific initialization/themes. Include editable
            // combos and child pages, without changing each dialog's behavior.
            AppDialogUseStandardControls(hwnd);
            AppDialogApplyChrome(hwnd);
        }
        return result;
    }
    INT_PTR result = proc ? proc(hwnd, msg, wp, lp) : FALSE;
    if (msg == WM_NCDESTROY) {
        RemovePropW(hwnd, prop);
    }
    return result;
}

// creates a dialog box that dynamically gets a right-to-left layout if needed
INT_PTR CreateAppDialogBox(int dlgId, HWND parent, DLGPROC DlgProc, LPARAM data) {
    AppDialogInit init{DlgProc, data};
    bool isRtl = IsUIRtl();
    bool isDefaultFont = IsAppFontSizeDefault();
    if (!isRtl && isDefaultFont) {
        return DialogBoxParam(nullptr, MAKEINTRESOURCE(dlgId), parent, AppDialogDispatch, (LPARAM)&init);
    }

    DLGTEMPLATE* tpl = DupTemplate(dlgId);
    int fntSize = GetAppFontSize();
    if (isDefaultFont) {
        SetDlgTemplateRtl(tpl);
    } else {
        SetDlgTemplateExFont(tpl, isRtl, fntSize);
    }

    INT_PTR res = DialogBoxIndirectParamW(nullptr, tpl, parent, AppDialogDispatch, (LPARAM)&init);
    free(tpl);
    return res;
}

HWND CreateAppDialogModeless(int dlgId, HWND parent, DLGPROC DlgProc, LPARAM data) {
    AppDialogInit init{DlgProc, data};
    bool isRtl = IsUIRtl();
    bool isDefaultFont = IsAppFontSizeDefault();
    if (!isRtl && isDefaultFont) {
        return CreateDialogParamW(nullptr, MAKEINTRESOURCE(dlgId), parent, AppDialogDispatch, (LPARAM)&init);
    }

    DLGTEMPLATE* tpl = DupTemplate(dlgId);
    int fntSize = GetAppFontSize();
    if (isDefaultFont) {
        SetDlgTemplateRtl(tpl);
    } else {
        SetDlgTemplateExFont(tpl, isRtl, fntSize);
    }

    HWND hwnd = CreateDialogIndirectParamW(nullptr, tpl, parent, AppDialogDispatch, (LPARAM)&init);
    free(tpl);
    return hwnd;
}

/* For passing data to/from GetPassword dialog */
struct Dialog_GetPassword_Data {
    const char* fileName; /* name of the file for which we need the password */
    char* pwdOut;         /* password entered by the user */
    bool* remember;       /* remember the password (encrypted) or ask again? */
    bool* showPassword;   /* keep the "show password" state across retries */
};

static INT_PTR CALLBACK Dialog_GetPassword_Proc(HWND hDlg, UINT msg, WPARAM wp, LPARAM lp) {
    Dialog_GetPassword_Data* data;

    //[ ACCESSKEY_GROUP Password Dialog
    if (WM_INITDIALOG == msg) {
        data = (Dialog_GetPassword_Data*)lp;
        HwndSetText(hDlg, _TRA("Enter password"));
        SetWindowLongPtr(hDlg, GWLP_USERDATA, (LONG_PTR)data);
        if (UseDarkModeLib()) {
            DarkMode::setDarkWndSafe(hDlg);
        }
        UpdateWindowCaptionTheme(hDlg);
        EnableWindow(GetDlgItem(hDlg, IDC_REMEMBER_PASSWORD), data->remember != nullptr);

        TempStr txt = str::FormatTemp(_TRA("Enter password for %s"), data->fileName);
        HwndSetDlgItemText(hDlg, IDC_GET_PASSWORD_LABEL, txt);
        HwndSetDlgItemText(hDlg, IDC_GET_PASSWORD_EDIT, "");
        HwndSetDlgItemText(hDlg, IDC_STATIC, _TRA("&Password:"));
        HwndSetDlgItemText(hDlg, IDC_SHOW_PASSWORD, _TRA("&Show password"));
        HwndSetDlgItemText(hDlg, IDC_REMEMBER_PASSWORD, _TRA("&Remember the password for this document"));
        HwndSetDlgItemText(hDlg, IDOK, _TRA("OK"));
        HwndSetDlgItemText(hDlg, IDCANCEL, _TRA("Cancel"));
        if (data->showPassword && *data->showPassword) {
            CheckDlgButton(hDlg, IDC_SHOW_PASSWORD, BST_CHECKED);
            HWND hwndEdit = GetDlgItem(hDlg, IDC_GET_PASSWORD_EDIT);
            SendMessageW(hwndEdit, EM_SETPASSWORDCHAR, 0, 0);
            InvalidateRect(hwndEdit, nullptr, TRUE);
        }

        CenterDialog(hDlg);
        HwndSetFocus(GetDlgItem(hDlg, IDC_GET_PASSWORD_EDIT));
        BringWindowToTop(hDlg);
        return FALSE;
    }
    //] ACCESSKEY_GROUP Password Dialog

    char* tmp;
    switch (msg) {
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDOK:
                    data = (Dialog_GetPassword_Data*)GetWindowLongPtr(hDlg, GWLP_USERDATA);
                    tmp = HwndGetTextTemp(GetDlgItem(hDlg, IDC_GET_PASSWORD_EDIT));
                    data->pwdOut = str::Dup(tmp);
                    if (data->remember) {
                        *data->remember = BST_CHECKED == IsDlgButtonChecked(hDlg, IDC_REMEMBER_PASSWORD);
                    }
                    EndDialog(hDlg, IDOK);
                    return TRUE;

                case IDCANCEL:
                    EndDialog(hDlg, IDCANCEL);
                    return TRUE;

                case IDC_SHOW_PASSWORD: {
                    HWND hwndEdit = GetDlgItem(hDlg, IDC_GET_PASSWORD_EDIT);
                    bool show = BST_CHECKED == IsDlgButtonChecked(hDlg, IDC_SHOW_PASSWORD);
                    data = (Dialog_GetPassword_Data*)GetWindowLongPtr(hDlg, GWLP_USERDATA);
                    if (data && data->showPassword) {
                        *data->showPassword = show;
                    }
                    SendMessageW(hwndEdit, EM_SETPASSWORDCHAR, show ? 0 : (WPARAM)L'\x25CF', 0);
                    InvalidateRect(hwndEdit, nullptr, TRUE);
                    return TRUE;
                }
            }
            break;
    }
    return FALSE;
}

/* Shows a 'get password' dialog for a given file.
   Returns a password entered by user as a newly allocated string or
   nullptr if user cancelled the dialog or there was an error.
   Caller needs to free() the result.
*/
char* Dialog_GetPassword(HWND hwndParent, const char* fileName, bool* rememberPassword, bool* showPassword) {
    Dialog_GetPassword_Data data = {nullptr};
    data.fileName = fileName;
    data.remember = rememberPassword;
    data.showPassword = showPassword;

    INT_PTR res = CreateAppDialogBox(IDD_DIALOG_GET_PASSWORD, hwndParent, Dialog_GetPassword_Proc, (LPARAM)&data);
    if (IDOK != res) {
        free(data.pwdOut);
        return nullptr;
    }
    return data.pwdOut;
}

/* For passing data to/from GoToPage dialog */
struct Dialog_GoToPage_Data {
    char* currPageLabel = nullptr; // currently shown page label
    int pageCount = 0;             // total number of pages
    bool onlyNumeric = false;      // whether the page label must be numeric
    char* newPageLabel = nullptr;  // page number entered by user

    ~Dialog_GoToPage_Data() {
        str::Free(currPageLabel);
        str::Free(newPageLabel);
    }
};

static INT_PTR CALLBACK Dialog_GoToPage_Proc(HWND hDlg, UINT msg, WPARAM wp, LPARAM lp) {
    HWND editPageNo;
    Dialog_GoToPage_Data* data;

    //[ ACCESSKEY_GROUP GoTo Page Dialog
    if (WM_INITDIALOG == msg) {
        data = (Dialog_GoToPage_Data*)lp;
        SetWindowLongPtr(hDlg, GWLP_USERDATA, (LONG_PTR)data);
        if (UseDarkModeLib()) {
            DarkMode::setDarkWndSafe(hDlg);
        }
        UpdateWindowCaptionTheme(hDlg);
        HwndSetText(hDlg, _TRA("Go to page"));

        editPageNo = GetDlgItem(hDlg, IDC_GOTO_PAGE_EDIT);
        if (!data->onlyNumeric) {
            SetWindowLong(editPageNo, GWL_STYLE, GetWindowLong(editPageNo, GWL_STYLE) & ~ES_NUMBER);
        }
        ReportIf(!data->currPageLabel);
        HwndSetDlgItemText(hDlg, IDC_GOTO_PAGE_EDIT, data->currPageLabel);
        TempStr totalCount = str::FormatTemp(_TRA("(of %d)"), data->pageCount);
        HwndSetDlgItemText(hDlg, IDC_GOTO_PAGE_LABEL_OF, totalCount);

        EditSelectAll(editPageNo);
        HwndSetDlgItemText(hDlg, IDC_STATIC, _TRA("&Go to page:"));
        HwndSetDlgItemText(hDlg, IDOK, _TRA("Go to page"));
        HwndSetDlgItemText(hDlg, IDCANCEL, _TRA("Cancel"));

        CenterDialog(hDlg);
        HwndSetFocus(editPageNo);
        return FALSE;
    }
    //] ACCESSKEY_GROUP GoTo Page Dialog

    char* tmp;
    switch (msg) {
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDOK:
                    data = (Dialog_GoToPage_Data*)GetWindowLongPtr(hDlg, GWLP_USERDATA);
                    editPageNo = GetDlgItem(hDlg, IDC_GOTO_PAGE_EDIT);
                    tmp = HwndGetTextTemp(editPageNo);
                    str::ReplaceWithCopy(&data->newPageLabel, tmp);
                    EndDialog(hDlg, IDOK);
                    return TRUE;

                case IDCANCEL:
                    EndDialog(hDlg, IDCANCEL);
                    return TRUE;
            }
            break;
    }
    return FALSE;
}

/* Shows a 'go to page' dialog and returns the page label entered by the user
   or nullptr if user clicked the "cancel" button or there was an error.
   The caller must free() the result. */
char* Dialog_GoToPage(HWND hwnd, const char* currentPageLabel, int pageCount, bool onlyNumeric) {
    Dialog_GoToPage_Data data;
    data.currPageLabel = str::Dup(currentPageLabel);
    data.pageCount = pageCount;
    data.onlyNumeric = onlyNumeric;
    data.newPageLabel = nullptr;

    CreateAppDialogBox(IDD_DIALOG_GOTO_PAGE, hwnd, Dialog_GoToPage_Proc, (LPARAM)&data);
    return str::Dup(data.newPageLabel);
}

/* For passing data to/from Find dialog */
struct Dialog_Find_Data {
    char* searchTerm;
    bool matchCase;
    WNDPROC editWndProc;
};

static LRESULT CALLBACK Dialog_Find_Edit_Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    ExtendedEditWndProc(hwnd, msg, wp, lp);

    Dialog_Find_Data* data = (Dialog_Find_Data*)GetWindowLongPtr(GetParent(hwnd), GWLP_USERDATA);
    return CallWindowProc(data->editWndProc, hwnd, msg, wp, lp);
}

static INT_PTR CALLBACK Dialog_Find_Proc(HWND hDlg, UINT msg, WPARAM wp, LPARAM lp) {
    Dialog_Find_Data* data;

    switch (msg) {
        case WM_INITDIALOG: {
            //[ ACCESSKEY_GROUP Find Dialog
            data = (Dialog_Find_Data*)lp;
            SetWindowLongPtr(hDlg, GWLP_USERDATA, (LONG_PTR)data);
            if (UseDarkModeLib()) {
                DarkMode::setDarkWndSafe(hDlg);
            }
            UpdateWindowCaptionTheme(hDlg);
            HwndSetText(hDlg, _TRA("Find"));
            HwndSetDlgItemText(hDlg, IDC_STATIC, _TRA("&Find what:"));
            HwndSetDlgItemText(hDlg, IDC_MATCH_CASE, _TRA("&Match case"));
            HwndSetDlgItemText(hDlg, IDC_FIND_NEXT_HINT, _TRA("Hint: Use the F3 key for finding again"));
            HwndSetDlgItemText(hDlg, IDOK, _TRA("Find"));
            HwndSetDlgItemText(hDlg, IDCANCEL, _TRA("Cancel"));
            if (data->searchTerm) {
                HwndSetDlgItemText(hDlg, IDC_FIND_EDIT, data->searchTerm);
            }
            data->searchTerm = nullptr;
            CheckDlgButton(hDlg, IDC_MATCH_CASE, data->matchCase ? BST_CHECKED : BST_UNCHECKED);
            data->editWndProc = (WNDPROC)SetWindowLongPtr(GetDlgItem(hDlg, IDC_FIND_EDIT), GWLP_WNDPROC,
                                                          (LONG_PTR)Dialog_Find_Edit_Proc);
            EditSelectAll(GetDlgItem(hDlg, IDC_FIND_EDIT));

            CenterDialog(hDlg);
            HwndSetFocus(GetDlgItem(hDlg, IDC_FIND_EDIT));
            return FALSE;
            //] ACCESSKEY_GROUP Find Dialog
        }

        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDOK: {
                    data = (Dialog_Find_Data*)GetWindowLongPtr(hDlg, GWLP_USERDATA);
                    TempStr tmp = HwndGetTextTemp(GetDlgItem(hDlg, IDC_FIND_EDIT));
                    data->searchTerm = str::Dup(tmp);
                    data->matchCase = BST_CHECKED == IsDlgButtonChecked(hDlg, IDC_MATCH_CASE);
                    EndDialog(hDlg, IDOK);
                    return TRUE;
                }

                case IDCANCEL:
                    EndDialog(hDlg, IDCANCEL);
                    return TRUE;
            }
            break;
    }
    return FALSE;
}

/* Shows a 'Find' dialog and returns the new search term entered by the user
   or nullptr if the search was canceled. previousSearch is the search term to
   be displayed as default. */
char* Dialog_Find(HWND hwnd, const char* previousSearch, bool* matchCase) {
    Dialog_Find_Data data;
    data.searchTerm = str::DupTemp(previousSearch);
    data.matchCase = matchCase ? *matchCase : false;
    INT_PTR res = CreateAppDialogBox(IDD_DIALOG_FIND, hwnd, Dialog_Find_Proc, (LPARAM)&data);
    if (res != IDOK) {
        return nullptr;
    }

    if (matchCase) {
        *matchCase = data.matchCase;
    }
    return data.searchTerm;
}

/* For passing data to/from ChangeLanguage dialog */
struct Dialog_ChangeLanguage_Data {
    const char* langCode;
};

// maps listbox index to lang index when filtered
static Vec<int>* gLangListMap = nullptr;

static void FilterLangList(HWND hDlg, const char* filter, const char* currLangCode) {
    HWND langList = GetDlgItem(hDlg, IDC_CHANGE_LANG_LANG_LIST);
    ListBox_ResetContent(langList);

    delete gLangListMap;
    gLangListMap = new Vec<int>();

    int itemToSelect = 0;
    for (int i = 0; i < trans::GetLangsCount(); i++) {
        const char* name = trans::GetLangNameByIdx(i);
        if (filter && *filter && !str::ContainsI(name, filter)) {
            continue;
        }
        auto langName = ToWStrTemp(name);
        ListBox_AppendString_NoSort(langList, langName);
        const char* langCode = trans::GetLangCodeByIdx(i);
        if (str::Eq(langCode, currLangCode)) {
            itemToSelect = gLangListMap->Size();
        }
        gLangListMap->Append(i);
    }
    if (gLangListMap->Size() > 0) {
        ListBox_SetCurSel(langList, itemToSelect);
    }
}

static INT_PTR CALLBACK Dialog_ChangeLanguage_Proc(HWND hDlg, UINT msg, WPARAM wp, LPARAM lp) {
    Dialog_ChangeLanguage_Data* data;
    HWND langList;

    if (WM_INITDIALOG == msg) {
        DIALOG_SIZER_START(sz)
        DIALOG_SIZER_ENTRY(IDOK, DS_MoveX | DS_MoveY)
        DIALOG_SIZER_ENTRY(IDCANCEL, DS_MoveX | DS_MoveY)
        DIALOG_SIZER_ENTRY(IDC_CHANGE_LANG_SEARCH, DS_SizeX)
        DIALOG_SIZER_ENTRY(IDC_CHANGE_LANG_LANG_LIST, DS_SizeY | DS_SizeX)
        DIALOG_SIZER_END()
        DialogSizer_Set(hDlg, sz, TRUE);

        data = (Dialog_ChangeLanguage_Data*)lp;
        SetWindowLongPtr(hDlg, GWLP_USERDATA, (LONG_PTR)data);
        if (UseDarkModeLib()) {
            DarkMode::setDarkWndSafe(hDlg);
        }
        UpdateWindowCaptionTheme(hDlg);
        // for non-latin languages this depends on the correct fonts being installed,
        // otherwise all the user will see are squares
        HwndSetText(hDlg, _TRA("Change Language"));

        FilterLangList(hDlg, nullptr, data->langCode);

        langList = GetDlgItem(hDlg, IDC_CHANGE_LANG_LANG_LIST);
        // the language list is meant to be laid out left-to-right
        SetWindowExStyle(langList, WS_EX_LAYOUTRTL, false);
        HwndSetDlgItemText(hDlg, IDOK, _TRA("OK"));
        HwndSetDlgItemText(hDlg, IDCANCEL, _TRA("Cancel"));

        CenterDialog(hDlg);
        HwndSetFocus(GetDlgItem(hDlg, IDC_CHANGE_LANG_SEARCH));
        return FALSE;
    }

    switch (msg) {
        case WM_COMMAND:
            data = (Dialog_ChangeLanguage_Data*)GetWindowLongPtr(hDlg, GWLP_USERDATA);
            if (LOWORD(wp) == IDC_CHANGE_LANG_SEARCH && HIWORD(wp) == EN_CHANGE) {
                char* filter = HwndGetTextTemp(GetDlgItem(hDlg, IDC_CHANGE_LANG_SEARCH));
                FilterLangList(hDlg, filter, data->langCode);
                return TRUE;
            }
            if (HIWORD(wp) == LBN_DBLCLK) {
                ReportIf(IDC_CHANGE_LANG_LANG_LIST != LOWORD(wp));
                langList = GetDlgItem(hDlg, IDC_CHANGE_LANG_LANG_LIST);
                ReportIf(langList != (HWND)lp);
                int idx = (int)ListBox_GetCurSel(langList);
                if (gLangListMap && idx >= 0 && idx < gLangListMap->Size()) {
                    int langIdx = gLangListMap->At(idx);
                    data->langCode = trans::GetLangCodeByIdx(langIdx);
                    EndDialog(hDlg, IDOK);
                }
                return FALSE;
            }
            switch (LOWORD(wp)) {
                case IDOK: {
                    langList = GetDlgItem(hDlg, IDC_CHANGE_LANG_LANG_LIST);
                    int idx = ListBox_GetCurSel(langList);
                    if (gLangListMap && idx >= 0 && idx < gLangListMap->Size()) {
                        int langIdx = gLangListMap->At(idx);
                        data->langCode = trans::GetLangCodeByIdx(langIdx);
                    }
                    EndDialog(hDlg, IDOK);
                }
                    return TRUE;

                case IDCANCEL:
                    EndDialog(hDlg, IDCANCEL);
                    return TRUE;
            }
            break;
    }

    return FALSE;
}

/* Returns nullptr  -1 if user choses 'cancel' */
const char* Dialog_ChangeLanguge(HWND hwnd, const char* currLangCode) {
    Dialog_ChangeLanguage_Data data;
    data.langCode = currLangCode;

    INT_PTR res = CreateAppDialogBox(IDD_DIALOG_CHANGE_LANGUAGE, hwnd, Dialog_ChangeLanguage_Proc, (LPARAM)&data);
    delete gLangListMap;
    gLangListMap = nullptr;
    if (IDCANCEL == res) {
        return nullptr;
    }
    return data.langCode;
}

TempStr ZoomLevelStr(float zoom) {
    if (zoom == kZoomFitPage) {
        return (TempStr)_TRA("Fit Page");
    }
    if (zoom == kZoomFitWidth) {
        return (TempStr)_TRA("Fit Width");
    }
    if (zoom == kZoomFitContent) {
        return (TempStr)_TRA("Fit Content");
    }
    if (zoom == kZoomShrinkToFit) {
        return (TempStr)_TRA("Shrink To Fit");
    }
    if (zoom == 0) {
        return (TempStr) "-";
    }
    TempStr res = str::FormatTemp("%.f%%", zoom);
    return res;
}

// clang-format off
static float gZoomLevels[] = {
    kZoomFitPage,
    kZoomFitWidth,
    kZoomFitContent,
    kZoomShrinkToFit,
    0,
    6400.0,
    3200.0,
    1600.0,
    800.0,
    400.0,
    200.0,
    150.0,
    125.0,
    100.0,
    50.0,
    25.0,
    12.5,
    8.33f
};
static float gZoomLevelsChm[] = {
    800.0,
    400.0,
    200.0,
    150.0,
    125.0,
    100.0,
    50.0,
    25.0,
};
// clang-format on

static Vec<float>* gCurrZoomLevels = nullptr;

static void AddZoomLevel(float zoomLevel, HWND hwnd, Vec<float>* levels) {
    TempStr s = ZoomLevelStr(zoomLevel);
    CbAddString(hwnd, s);
    levels->Append(zoomLevel);
}

static void SetupZoomComboBox(HWND hDlg, UINT idComboBox, bool forChm, float currZoom) {
    HWND hwnd = GetDlgItem(hDlg, idComboBox);

    auto prefs = gGlobalPrefs;
    auto customZoomLevels = prefs->zoomLevels;
    auto currZoomLevels = new Vec<float>();
    int n = customZoomLevels->Size();
    if (n > 0) {
        if (!forChm) {
            float* zoomLevels = gZoomLevels;
            for (int i = 0; i < 4; i++) {
                AddZoomLevel(zoomLevels[i], hwnd, currZoomLevels);
            }
        }
        float maxZoom = forChm ? 800 : kZoomMax;
        float minZoom = forChm ? 16 : kZoomMin;
        for (int i = 0; i < n; i++) {
            float zl = customZoomLevels->At(n - i - 1); // largest first
            if (zl >= minZoom && zl <= maxZoom) {
                AddZoomLevel(zl, hwnd, currZoomLevels);
            }
        }
    } else {
        float* zoomLevels = forChm ? gZoomLevelsChm : gZoomLevels;
        n = forChm ? dimofi(gZoomLevelsChm) : dimofi(gZoomLevels);
        for (int i = 0; i < n; i++) {
            AddZoomLevel(zoomLevels[i], hwnd, currZoomLevels);
        }
    }

    n = currZoomLevels->Size();
    for (int i = 0; i < n; i++) {
        float zl = currZoomLevels->At(i);
        if (zl == currZoom) {
            CbSetCurrentSelection(hwnd, i);
        }
    }

    if (SendDlgItemMessage(hDlg, idComboBox, CB_GETCURSEL, 0, 0) == -1) {
        TempStr customZoom = str::FormatTemp("%.0f%%", currZoom);
        SetDlgItemTextW(hDlg, idComboBox, ToWStrTemp(customZoom));
    }
    delete gCurrZoomLevels;
    gCurrZoomLevels = currZoomLevels;
}

static float GetZoomComboBoxValue(HWND hDlg, UINT idComboBox, float defaultZoom) {
    float newZoom = defaultZoom;
    int idx = ComboBox_GetCurSel(GetDlgItem(hDlg, idComboBox));
    if (idx == -1) {
        char* customZoom = HwndGetTextTemp(GetDlgItem(hDlg, idComboBox));
        float zoom = (float)atof(customZoom);
        newZoom = limitValue(zoom, kZoomMin, kZoomMax);
        return newZoom;
    }
    newZoom = gCurrZoomLevels->At(idx);
    if (newZoom == 0) {
        newZoom = defaultZoom;
    }
    return newZoom;
}

struct Dialog_CustomZoom_Data {
    float zoomArg = 0;
    float zoomResult = 0;
    bool forChm = false;
};

static INT_PTR CALLBACK Dialog_CustomZoom_Proc(HWND hDlg, UINT msg, WPARAM wp, LPARAM lp) {
    Dialog_CustomZoom_Data* data;

    switch (msg) {
        case WM_INITDIALOG:
            //[ ACCESSKEY_GROUP Zoom Dialog
            data = (Dialog_CustomZoom_Data*)lp;
            SetWindowLongPtr(hDlg, GWLP_USERDATA, (LONG_PTR)data);
            if (UseDarkModeLib()) {
                DarkMode::setDarkWndSafe(hDlg);
            }
            UpdateWindowCaptionTheme(hDlg);
            SetupZoomComboBox(hDlg, IDC_DEFAULT_ZOOM, data->forChm, data->zoomArg);

            HwndSetText(hDlg, _TRA("Zoom factor"));
            HwndSetDlgItemText(hDlg, IDC_STATIC, _TRA("&Magnification:"));
            HwndSetDlgItemText(hDlg, IDOK, _TRA("Zoom"));
            HwndSetDlgItemText(hDlg, IDCANCEL, _TRA("Cancel"));

            CenterDialog(hDlg);
            HwndSetFocus(GetDlgItem(hDlg, IDC_DEFAULT_ZOOM));
            return FALSE;
            //] ACCESSKEY_GROUP Zoom Dialog

        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDOK:
                    data = (Dialog_CustomZoom_Data*)GetWindowLongPtr(hDlg, GWLP_USERDATA);
                    data->zoomResult = GetZoomComboBoxValue(hDlg, IDC_DEFAULT_ZOOM, data->zoomArg);
                    EndDialog(hDlg, IDOK);
                    return TRUE;

                case IDCANCEL:
                    EndDialog(hDlg, IDCANCEL);
                    return TRUE;
            }
            break;
    }
    return FALSE;
}

bool Dialog_CustomZoom(HWND hwnd, bool forChm, float* currZoomInOut) {
    Dialog_CustomZoom_Data data;
    data.forChm = forChm;
    data.zoomArg = *currZoomInOut;
    INT_PTR res = CreateAppDialogBox(IDD_DIALOG_CUSTOM_ZOOM, hwnd, Dialog_CustomZoom_Proc, (LPARAM)&data);
    if (res == IDCANCEL) {
        return false;
    }

    *currZoomInOut = data.zoomResult;
    return true;
}

static INT_PTR CALLBACK Dialog_ChangeScrollbar_Proc(HWND hDlg, UINT msg, WPARAM wp, LPARAM) {
    switch (msg) {
        case WM_INITDIALOG: {
            if (UseDarkModeLib()) {
                DarkMode::setDarkWndSafe(hDlg);
            }
            UpdateWindowCaptionTheme(hDlg);
            const char* s = gGlobalPrefs->scrollbars;
            int checkId = IDC_SCROLLBAR_WINDOWS;
            if (str::EqI(s, "smart")) {
                checkId = IDC_SCROLLBAR_SMART;
            } else if (str::EqI(s, "overlay")) {
                checkId = IDC_SCROLLBAR_OVERLAY;
            } else if (str::EqI(s, "hidden")) {
                checkId = IDC_SCROLLBAR_HIDDEN;
            }
            CheckRadioButton(hDlg, IDC_SCROLLBAR_WINDOWS, IDC_SCROLLBAR_HIDDEN, checkId);
            HwndSetText(hDlg, _TRA("Change Scrollbar"));
            HwndSetDlgItemText(hDlg, IDOK, _TRA("OK"));
            HwndSetDlgItemText(hDlg, IDCANCEL, _TRA("Cancel"));
            CenterDialog(hDlg);
            return TRUE;
        }
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDOK: {
                    const char* val = "windows";
                    if (IsDlgButtonChecked(hDlg, IDC_SCROLLBAR_SMART) == BST_CHECKED) {
                        val = "smart";
                    } else if (IsDlgButtonChecked(hDlg, IDC_SCROLLBAR_OVERLAY) == BST_CHECKED) {
                        val = "overlay";
                    } else if (IsDlgButtonChecked(hDlg, IDC_SCROLLBAR_HIDDEN) == BST_CHECKED) {
                        val = "hidden";
                    }
                    str::ReplaceWithCopy(&gGlobalPrefs->scrollbars, val);
                    EndDialog(hDlg, IDOK);
                    return TRUE;
                }
                case IDCANCEL:
                    EndDialog(hDlg, IDCANCEL);
                    return TRUE;
            }
            break;
    }
    return FALSE;
}

bool Dialog_ChangeScrollbar(HWND hwnd) {
    INT_PTR res = CreateAppDialogBox(IDD_DIALOG_CHANGE_SCROLLBAR, hwnd, Dialog_ChangeScrollbar_Proc, 0);
    return res == IDOK;
}

static const int gSettingsGeneralControls[] = {
    IDC_SETTINGS_PAGE_GENERAL, IDC_GROUP_UPDATE,          IDC_CHECK_FOR_UPDATES,
    IDC_GROUP_SESSION,         IDC_REMEMBER_OPENED_FILES, IDC_REMEMBER_STATE_PER_DOCUMENT,
    IDC_RESTORE_SESSION,       IDC_LAZY_LOADING,          IDC_REUSE_INSTANCE,
    IDC_GROUP_FILE_CHANGES,    IDC_RELOAD_MODIFIED};
static const int gSettingsInterfaceControls[] = {IDC_SETTINGS_PAGE_INTERFACE,
                                                 IDC_GROUP_APPEARANCE,
                                                 IDC_THEME_LABEL,
                                                 IDC_THEME,
                                                 IDC_DOCUMENT_COLOR_LABEL,
                                                 IDC_DOCUMENT_COLOR,
                                                 IDC_GROUP_TOOLBAR,
                                                 IDC_USE_TABS,
                                                 IDC_NO_HOME_TAB,
                                                 IDC_SHOW_MENUBAR_WITH_TABS,
                                                 IDC_SHOW_TOOLBAR,
                                                 IDC_SHOW_ANNOT_TOOLBAR_BUTTONS,
                                                 IDC_TABS_MRU,
                                                 IDC_SEARCH_UI_FLOATING,
                                                 IDC_TAB_FONT_SIZE_LABEL,
                                                 IDC_TAB_FONT_SIZE,
                                                 IDC_TAB_BAR_HEIGHT_LABEL,
                                                 IDC_TAB_BAR_HEIGHT,
                                                 IDC_GROUP_SCROLLBARS,
                                                 IDC_SCROLLBARS_LABEL,
                                                 IDC_SCROLLBARS,
                                                 IDC_GROUP_SIDEBAR,
                                                 IDC_TREE_FONT_LABEL,
                                                 IDC_TREE_FONT_NAME,
                                                 IDC_TREE_FONT_SIZE_LABEL,
                                                 IDC_TREE_FONT_SIZE,
                                                 IDC_TREE_WRAP_LABELS};
static const int gSettingsReadingControls[] = {IDC_SETTINGS_PAGE_READING,
                                               IDC_GROUP_DEFAULT_VIEW,
                                               IDC_DEFAULT_LAYOUT_LABEL,
                                               IDC_DEFAULT_LAYOUT,
                                               IDC_DEFAULT_ZOOM_LABEL,
                                               IDC_DEFAULT_ZOOM,
                                               IDC_DEFAULT_SHOW_TOC,
                                               IDC_GROUP_SCROLLING,
                                               IDC_SMOOTH_SCROLL,
                                               IDC_SCROLLBAR_SINGLE_PAGE,
                                               IDC_FAST_SCROLL_OVER_SCROLLBAR,
                                               IDC_GROUP_DISPLAY_QUALITY,
                                               IDC_ENGINEERING_ENHANCE_LABEL,
                                               IDC_ENGINEERING_ENHANCE,
                                               IDC_ENABLE_ANTIALIAS,
                                               IDC_GROUP_DICTIONARY,
                                               IDC_ENABLE_WORD_LOOKUP,
                                               IDC_DICTIONARY_PATH_LABEL,
                                               IDC_DICTIONARY_PATH,
                                               IDC_DICTIONARY_BROWSE,
                                               IDC_GROUP_FULLSCREEN,
                                               IDC_PREVENT_SLEEP_FULLSCREEN,
                                               IDC_GROUP_ANNOT_AUTHOR,
                                               IDC_DEFAULT_AUTHOR_LABEL,
                                               IDC_DEFAULT_AUTHOR};
static const int gSettingsReadAloudControls[] = {IDC_SETTINGS_PAGE_READ_ALOUD,
                                                 IDC_GROUP_RA_VOICE,
                                                 IDC_RA_VOICE_MODE_LABEL,
                                                 IDC_RA_VOICE_MODE,
                                                 IDC_RA_VOICE_ZH_LABEL,
                                                 IDC_RA_VOICE_ZH,
                                                 IDC_RA_VOICE_EN_LABEL,
                                                 IDC_RA_VOICE_EN,
                                                 IDC_RA_VOICE_MULTI_LABEL,
                                                 IDC_RA_VOICE_MULTI,
                                                 IDC_RA_PREVIEW,
                                                 IDC_GROUP_RA_HIGHLIGHT,
                                                 IDC_RA_HIGHLIGHT_COLOR_LABEL,
                                                 IDC_RA_HIGHLIGHT_COLOR,
                                                 IDC_RA_HIGHLIGHT_RESET,
                                                 IDC_RA_AUTO_FOLLOW,
                                                 IDC_GROUP_RA_NARRATION,
                                                 IDC_RA_NARRATION_USE_AUDIO,
                                                 IDC_RA_NARRATION_HINT,
                                                 IDC_RA_NARRATION_USE_COLOR};
static const int gSettingsOcrControls[] = {
    IDC_SETTINGS_PAGE_OCR_AI, IDC_GROUP_OCR,       IDC_AUTO_OCR,       IDC_OCR_DESCRIPTION, IDC_OCR_AUTO_SAVE,
    IDC_OCR_SAVE_WARNING,     IDC_GROUP_SMART_TOC, IDC_TOC_MODE_LABEL, IDC_TOC_MODE,        IDC_TOC_MODE_DESCRIPTION};
static const int gSettingsAiControls[] = {IDC_SETTINGS_PAGE_AI,
                                          IDC_GROUP_ASK_AI,
                                          IDC_ENABLE_ASK_AI,
                                          IDC_AI_PROVIDER_LABEL,
                                          IDC_AI_PROVIDER,
                                          IDC_GROUP_INLINE_TRANSLATE,
                                          IDC_ENABLE_INLINE_TRANSLATE,
                                          IDC_TRANSLATE_TARGET_LABEL,
                                          IDC_TRANSLATE_TARGET,
                                          IDC_TRANSLATE_VOLC_AK_LABEL,
                                          IDC_TRANSLATE_VOLC_AK,
                                          IDC_TRANSLATE_VOLC_SK_LABEL,
                                          IDC_TRANSLATE_VOLC_SK,
                                          IDC_TRANSLATE_VOLC_TEST,
                                          IDC_TRANSLATE_HINT,
                                          IDC_GROUP_AITOC_API,
                                          IDC_AITOC_API_BASEURL_LABEL,
                                          IDC_AITOC_API_BASEURL,
                                          IDC_AITOC_API_KEY_LABEL,
                                          IDC_AITOC_API_KEY,
                                          IDC_AITOC_API_MODEL_LABEL,
                                          IDC_AITOC_API_MODEL,
                                          IDC_AITOC_API_TEST,
                                          IDC_AITOC_API_PROFILE_LABEL,
                                          IDC_AITOC_API_PROFILE,
                                          IDC_AITOC_API_ADD,
                                          IDC_AITOC_API_REMOVE,
                                          IDC_AITOC_API_NAME_LABEL,
                                          IDC_AITOC_API_NAME,
                                          IDC_AITOC_API_FETCH_MODELS,
                                          IDC_AITOC_API_CONCURRENCY_LABEL,
                                          IDC_AITOC_API_CONCURRENCY};
static const int gSettingsAdvancedControls[] = {IDC_SETTINGS_PAGE_ADVANCED,
                                                IDC_GROUP_WINDOW,
                                                IDC_ESC_TO_EXIT,
                                                IDC_FULL_PATH_IN_TITLE,
                                                IDC_GROUP_DISPLAY,
                                                IDC_CUSTOM_DPI_LABEL,
                                                IDC_CUSTOM_DPI,
                                                IDC_GROUP_PDF,
                                                IDC_SHOW_LINKS,
                                                IDC_SECTION_INVERSESEARCH,
                                                IDC_CMDLINE_LABEL,
                                                IDC_CMDLINE,
                                                IDC_MORE_EXPERT_SETTINGS,
                                                IDC_OPEN_ADVANCED_OPTIONS};

static int gSettingsInitialPage = 0;

// long translations (German, Russian) need wider labels: move the controls
// right of the labels over, keeping the right edge of the group
// Grow label column and shift fields so translated (often Chinese) labels fit.
static void FitSettingsLabelColumn(HWND hDlg, const int* labelIds, int labelCount, const int* fieldIds, int fieldCount,
                                   int refFieldId, int gapDlg = 6) {
    HWND firstLabel = GetDlgItem(hDlg, labelIds[0]);
    HWND refField = GetDlgItem(hDlg, refFieldId);
    HDC hdc = GetDC(hDlg);
    if (!firstLabel || !refField || !hdc) {
        return;
    }
    HFONT font = (HFONT)SendMessageW(hDlg, WM_GETFONT, 0, 0);
    HFONT oldFont = font ? (HFONT)SelectObject(hdc, font) : nullptr;
    int need = 0;
    for (int i = 0; i < labelCount; i++) {
        TempWStr s = HwndGetTextWTemp(GetDlgItem(hDlg, labelIds[i]));
        RECT rc{};
        DrawTextW(hdc, s, -1, &rc, DT_CALCRECT | DT_SINGLELINE);
        need = std::max(need, (int)(rc.right - rc.left));
    }
    if (oldFont) {
        SelectObject(hdc, oldFont);
    }
    ReleaseDC(hDlg, hdc);

    Rect label = MapRectToWindow(WindowRect(firstLabel), HWND_DESKTOP, hDlg);
    Rect field = MapRectToWindow(WindowRect(refField), HWND_DESKTOP, hDlg);
    int gap = DpiScale(hDlg, gapDlg);
    int shift = label.x + need + gap - field.x;
    if (shift <= 0) {
        return;
    }
    int right = field.Right();
    for (int i = 0; i < labelCount; i++) {
        HWND h = GetDlgItem(hDlg, labelIds[i]);
        Rect r = MapRectToWindow(WindowRect(h), HWND_DESKTOP, hDlg);
        SetWindowPos(h, nullptr, r.x, r.y, r.dx + shift, r.dy, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    for (int i = 0; i < fieldCount; i++) {
        HWND h = GetDlgItem(hDlg, fieldIds[i]);
        Rect r = MapRectToWindow(WindowRect(h), HWND_DESKTOP, hDlg);
        int dx = r.dx;
        // full-width controls keep the right edge; short ones just move
        if (r.Right() >= right - 1) {
            dx -= shift;
        } else if (r.Right() + shift > right) {
            dx = std::max(right - (r.x + shift), DpiScale(hDlg, 40));
        }
        // a combo's window height is its dropped-down height
        WCHAR cls[32]{};
        GetClassNameW(h, cls, dimof(cls));
        int dy = str::EqI(cls, L"ComboBox") ? DpiScale(hDlg, 300) : r.dy;
        SetWindowPos(h, nullptr, r.x + shift, r.y, dx, dy, SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

static void FitSettingsInterfaceLabels(HWND hDlg) {
    static const int labelIds[] = {IDC_THEME_LABEL, IDC_DOCUMENT_COLOR_LABEL, IDC_SCROLLBARS_LABEL};
    static const int fieldIds[] = {IDC_THEME, IDC_DOCUMENT_COLOR, IDC_SCROLLBARS};
    FitSettingsLabelColumn(hDlg, labelIds, dimof(labelIds), fieldIds, dimof(fieldIds), IDC_THEME);
}

static void FitSettingsReadingLabels(HWND hDlg) {
    static const int labelIds[] = {IDC_DEFAULT_LAYOUT_LABEL, IDC_DEFAULT_ZOOM_LABEL, IDC_DICTIONARY_PATH_LABEL,
                                   IDC_DEFAULT_AUTHOR_LABEL};
    static const int fieldIds[] = {IDC_DEFAULT_LAYOUT, IDC_DEFAULT_ZOOM, IDC_DICTIONARY_PATH, IDC_DICTIONARY_BROWSE,
                                   IDC_DEFAULT_AUTHOR};
    FitSettingsLabelColumn(hDlg, labelIds, dimof(labelIds), fieldIds, dimof(fieldIds), IDC_DEFAULT_LAYOUT);
}

static void FitSettingsOcrLabels(HWND hDlg) {
    static const int labelIds[] = {IDC_TOC_MODE_LABEL};
    static const int fieldIds[] = {IDC_TOC_MODE};
    FitSettingsLabelColumn(hDlg, labelIds, dimof(labelIds), fieldIds, dimof(fieldIds), IDC_TOC_MODE);
}

static void FitSettingsReadAloudLabels(HWND hDlg) {
    static const int labelIds[] = {IDC_RA_VOICE_MODE_LABEL, IDC_RA_VOICE_ZH_LABEL, IDC_RA_VOICE_EN_LABEL,
                                   IDC_RA_VOICE_MULTI_LABEL, IDC_RA_HIGHLIGHT_COLOR_LABEL};
    static const int fieldIds[] = {IDC_RA_VOICE_MODE, IDC_RA_VOICE_ZH,        IDC_RA_VOICE_EN,       IDC_RA_VOICE_MULTI,
                                   IDC_RA_PREVIEW,    IDC_RA_HIGHLIGHT_COLOR, IDC_RA_HIGHLIGHT_RESET};
    FitSettingsLabelColumn(hDlg, labelIds, dimof(labelIds), fieldIds, dimof(fieldIds), IDC_RA_VOICE_MODE);
}

// Align AI page label/field columns after translation (Chinese labels are wider).
static void FitSettingsAiLabels(HWND hDlg) {
    static const int labelIds[] = {
        IDC_AI_PROVIDER_LABEL,           IDC_TRANSLATE_TARGET_LABEL,  IDC_TRANSLATE_VOLC_AK_LABEL,
        IDC_TRANSLATE_VOLC_SK_LABEL,     IDC_AITOC_API_PROFILE_LABEL, IDC_AITOC_API_NAME_LABEL,
        IDC_AITOC_API_BASEURL_LABEL,     IDC_AITOC_API_KEY_LABEL,     IDC_AITOC_API_MODEL_LABEL,
        IDC_AITOC_API_CONCURRENCY_LABEL,
    };
    // Fields that share the main value column (right edge of AI provider combo).
    static const int fullFieldIds[] = {
        IDC_AI_PROVIDER,    IDC_TRANSLATE_TARGET,  IDC_TRANSLATE_VOLC_AK,
        IDC_AITOC_API_NAME, IDC_AITOC_API_BASEURL, IDC_AITOC_API_KEY,
    };
    // Fields that leave room for a right-side button.
    static const int midFieldIds[] = {IDC_TRANSLATE_VOLC_SK, IDC_AITOC_API_PROFILE, IDC_AITOC_API_MODEL,
                                      IDC_AITOC_API_CONCURRENCY};

    HWND firstLabel = GetDlgItem(hDlg, labelIds[0]);
    HWND firstField = GetDlgItem(hDlg, IDC_AI_PROVIDER);
    if (!firstLabel || !firstField) {
        return;
    }
    HDC hdc = GetDC(hDlg);
    if (!hdc) {
        return;
    }
    HFONT font = (HFONT)SendMessageW(hDlg, WM_GETFONT, 0, 0);
    HFONT oldFont = font ? (HFONT)SelectObject(hdc, font) : nullptr;
    int need = 0;
    for (int id : labelIds) {
        TempWStr s = HwndGetTextWTemp(GetDlgItem(hDlg, id));
        RECT rc{};
        DrawTextW(hdc, s, -1, &rc, DT_CALCRECT | DT_SINGLELINE);
        need = std::max(need, (int)(rc.right - rc.left));
    }
    if (oldFont) {
        SelectObject(hdc, oldFont);
    }
    ReleaseDC(hDlg, hdc);

    Rect label = MapRectToWindow(WindowRect(firstLabel), HWND_DESKTOP, hDlg);
    Rect field = MapRectToWindow(WindowRect(firstField), HWND_DESKTOP, hDlg);
    int gap = DpiScale(hDlg, 8);
    int shift = label.x + need + gap - field.x;
    if (shift == 0) {
        return;
    }
    // Growing labels: push fields right. Shrinking: leave RC spacing.
    if (shift < 0) {
        return;
    }
    int fullRight = field.Right();
    for (int id : labelIds) {
        HWND h = GetDlgItem(hDlg, id);
        Rect r = MapRectToWindow(WindowRect(h), HWND_DESKTOP, hDlg);
        SetWindowPos(h, nullptr, r.x, r.y, r.dx + shift, r.dy, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    for (int id : fullFieldIds) {
        HWND h = GetDlgItem(hDlg, id);
        Rect r = MapRectToWindow(WindowRect(h), HWND_DESKTOP, hDlg);
        int dx = std::max(fullRight - (r.x + shift), DpiScale(hDlg, 80));
        WCHAR cls[32]{};
        GetClassNameW(h, cls, dimof(cls));
        int dy = str::EqI(cls, L"ComboBox") ? DpiScale(hDlg, 300) : r.dy;
        SetWindowPos(h, nullptr, r.x + shift, r.y, dx, dy, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    for (int id : midFieldIds) {
        HWND h = GetDlgItem(hDlg, id);
        Rect r = MapRectToWindow(WindowRect(h), HWND_DESKTOP, hDlg);
        WCHAR cls[32]{};
        GetClassNameW(h, cls, dimof(cls));
        int dy = str::EqI(cls, L"ComboBox") ? DpiScale(hDlg, 300) : r.dy;
        // Keep the right edge so the field never runs under its button.
        int dx = std::max(r.dx - shift, DpiScale(hDlg, 60));
        SetWindowPos(h, nullptr, r.x + shift, r.y, dx, dy, SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

static void ShowSettingsPage(HWND hDlg, int page) {
    struct PageControls {
        const int* ids;
        int count;
    } pages[] = {{gSettingsGeneralControls, dimof(gSettingsGeneralControls)},
                 {gSettingsInterfaceControls, dimof(gSettingsInterfaceControls)},
                 {gSettingsReadingControls, dimof(gSettingsReadingControls)},
                 {gSettingsReadAloudControls, dimof(gSettingsReadAloudControls)},
                 {gSettingsOcrControls, dimof(gSettingsOcrControls)},
                 {gSettingsAiControls, dimof(gSettingsAiControls)},
                 {gSettingsAdvancedControls, dimof(gSettingsAdvancedControls)}};
    page = limitValue(page, 0, dimof(pages) - 1);
    auto* prefs = (GlobalPrefs*)GetWindowLongPtr(hDlg, GWLP_USERDATA);
    bool showInverseSearch = prefs && prefs->enableTeXEnhancements && CanAccessDisk();
    for (int i = 0; i < dimof(pages); i++) {
        for (int j = 0; j < pages[i].count; j++) {
            int id = pages[i].ids[j];
            bool isInverseSearch = id == IDC_SECTION_INVERSESEARCH || id == IDC_CMDLINE_LABEL || id == IDC_CMDLINE;
            bool show = i == page && (!isInverseSearch || showInverseSearch);
            ShowWindow(GetDlgItem(hDlg, id), show ? SW_SHOW : SW_HIDE);
        }
    }
}

static constexpr const WCHAR* kAiTocSettingsStateProp = L"AiTocSettingsState";
static LONG gAiTocSettingsToken = 0;

struct AiTocUiProfile {
    char* name = nullptr;
    char* baseUrl = nullptr;
    char* key = nullptr;
    char* model = nullptr;
    int concurrency = 4;

    void Free() {
        str::Free(name);
        str::Free(baseUrl);
        str::Free(key);
        str::Free(model);
    }
};

struct AiTocSettingsState {
    Vec<AiTocUiProfile> profiles;
    Vec<char*> models;
    int current = 0;
    unsigned fetchGeneration = 0;
    LONG token = 0;
    bool loading = false;
    HWND fetchTip = nullptr;
    WCHAR* fetchTipText = nullptr;

    ~AiTocSettingsState() {
        if (fetchTip) {
            DestroyWindow(fetchTip);
        }
        str::Free(fetchTipText);
        for (auto& profile : profiles) {
            profile.Free();
        }
        for (auto* model : models) {
            str::Free(model);
        }
        profiles.Reset();
        models.Reset();
    }
};

static AiTocSettingsState* GetAiTocSettingsState(HWND hDlg) {
    return (AiTocSettingsState*)GetPropW(hDlg, kAiTocSettingsStateProp);
}

struct AiTocProfilesVisitor : json::ValueVisitor {
    AiTocSettingsState* state;
    int active = 0;

    explicit AiTocProfilesVisitor(AiTocSettingsState* s) : state(s) {}

    bool Visit(const char* path, const char* value, json::Type type) override {
        if (str::Eq(path, "/active") && type == json::Type::Number) {
            active = atoi(value);
            return true;
        }
        if (!str::StartsWith(path, "/profiles[")) {
            return true;
        }
        const char* p = path + str::Len("/profiles[");
        char* end = nullptr;
        long idx = strtol(p, &end, 10);
        if (end == p || idx < 0 || idx >= 100 || !end || *end++ != ']' || *end++ != '/') {
            return true;
        }
        while (state->profiles.Size() <= idx) {
            state->profiles.Append(AiTocUiProfile{});
        }
        auto& profile = state->profiles[idx];
        if (type == json::Type::String) {
            if (str::Eq(end, "name")) {
                str::ReplaceWithCopy(&profile.name, value);
            } else if (str::Eq(end, "base_url")) {
                str::ReplaceWithCopy(&profile.baseUrl, value);
            } else if (str::Eq(end, "api_key")) {
                str::ReplaceWithCopy(&profile.key, value);
            } else if (str::Eq(end, "model")) {
                str::ReplaceWithCopy(&profile.model, value);
            }
        } else if (type == json::Type::Number && str::Eq(end, "concurrency")) {
            profile.concurrency = limitValue(atoi(value), 1, 8);
        }
        return true;
    }
};

static void AiTocJsonString(StrBuilder& out, const char* s) {
    out.AppendChar('"');
    if (s) {
        for (const u8* p = (const u8*)s; *p; p++) {
            u8 c = *p;
            if (c == '"') {
                out.Append("\\\"");
            } else if (c == '\\') {
                out.Append("\\\\");
            } else if (c < 0x20) {
                out.AppendFmt("\\u%04x", (int)c);
            } else {
                out.AppendChar((char)c);
            }
        }
    }
    out.AppendChar('"');
}

static void AiTocClearModels(AiTocSettingsState* state) {
    for (auto* model : state->models) {
        str::Free(model);
    }
    state->models.Reset();
}

static void AiTocAddTip(HWND tip, HWND owner, HWND ctrl, WCHAR* text) {
    if (!tip || !ctrl || !text) {
        return;
    }
    TOOLINFOW ti{};
    ti.cbSize = sizeof(ti);
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = owner;
    ti.uId = (UINT_PTR)ctrl;
    ti.lpszText = text;
    SendMessageW(tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
}

static void AiTocAddFetchModelsTip(HWND hDlg) {
    auto* state = GetAiTocSettingsState(hDlg);
    if (!state || state->fetchTip) {
        return;
    }
    HWND tip =
        CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, CW_USEDEFAULT,
                        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, hDlg, nullptr, GetModuleHandle(nullptr), nullptr);
    if (!tip) {
        return;
    }
    SetWindowPos(tip, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SendMessageW(tip, TTM_SETMAXTIPWIDTH, 0, 320);
    state->fetchTipText = str::Dup(
        ToWStrTemp(_TRA("Ask the API for model names and put the one you pick in the box. You can also type a name.")));
    state->fetchTip = tip;
    AiTocAddTip(tip, hDlg, GetDlgItem(hDlg, IDC_AITOC_API_FETCH_MODELS), state->fetchTipText);
}

static void AiTocSaveVisibleProfile(HWND hDlg) {
    auto* state = GetAiTocSettingsState(hDlg);
    if (!state || state->loading || state->current < 0 || state->current >= state->profiles.Size()) {
        return;
    }
    auto& p = state->profiles[state->current];
    str::ReplaceWithCopy(&p.name, HwndGetTextTemp(GetDlgItem(hDlg, IDC_AITOC_API_NAME)));
    str::ReplaceWithCopy(&p.baseUrl, HwndGetTextTemp(GetDlgItem(hDlg, IDC_AITOC_API_BASEURL)));
    str::ReplaceWithCopy(&p.key, HwndGetTextTemp(GetDlgItem(hDlg, IDC_AITOC_API_KEY)));
    str::ReplaceWithCopy(&p.model, HwndGetTextTemp(GetDlgItem(hDlg, IDC_AITOC_API_MODEL)));
    BOOL valid = FALSE;
    UINT n = GetDlgItemInt(hDlg, IDC_AITOC_API_CONCURRENCY, &valid, FALSE);
    p.concurrency = valid ? limitValue((int)n, 1, 8) : 4;
    if (str::IsEmptyOrWhiteSpace(p.name)) {
        str::ReplaceWithCopy(&p.name, str::FormatTemp("Platform %d", state->current + 1));
    }
}

static void AiTocRefreshProfileSelector(HWND hDlg) {
    auto* state = GetAiTocSettingsState(hDlg);
    HWND combo = GetDlgItem(hDlg, IDC_AITOC_API_PROFILE);
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    for (int i = 0; i < state->profiles.Size(); i++) {
        const char* name = state->profiles[i].name;
        CbAddString(combo, name && *name ? name : str::FormatTemp("Platform %d", i + 1));
    }
    CbSetCurrentSelection(combo, state->current);
    EnableWindow(GetDlgItem(hDlg, IDC_AITOC_API_REMOVE), state->profiles.Size() > 1);
}

static void AiTocShowProfile(HWND hDlg) {
    auto* state = GetAiTocSettingsState(hDlg);
    if (!state || state->current < 0 || state->current >= state->profiles.Size()) {
        return;
    }
    auto& p = state->profiles[state->current];
    state->loading = true;
    HwndSetDlgItemText(hDlg, IDC_AITOC_API_NAME, p.name ? p.name : "");
    HwndSetDlgItemText(hDlg, IDC_AITOC_API_BASEURL, p.baseUrl ? p.baseUrl : "");
    HwndSetDlgItemText(hDlg, IDC_AITOC_API_KEY, p.key ? p.key : "");
    HwndSetDlgItemText(hDlg, IDC_AITOC_API_MODEL, p.model ? p.model : "");
    SetDlgItemInt(hDlg, IDC_AITOC_API_CONCURRENCY, p.concurrency, FALSE);
    state->loading = false;
    AiTocClearModels(state);
    state->fetchGeneration++;
    AiTocRefreshProfileSelector(hDlg);
    EnableWindow(GetDlgItem(hDlg, IDC_AITOC_API_FETCH_MODELS), TRUE);
}

static void AiTocInitProfiles(HWND hDlg, GlobalPrefs* prefs) {
    auto* state = new AiTocSettingsState();
    state->token = InterlockedIncrement(&gAiTocSettingsToken);
    if (!str::IsEmptyOrWhiteSpace(prefs->aiTocApiProfiles)) {
        AiTocProfilesVisitor visitor(state);
        if (json::Parse(prefs->aiTocApiProfiles, &visitor)) {
            state->current = visitor.active;
        } else {
            for (auto& profile : state->profiles) {
                profile.Free();
            }
            state->profiles.Reset();
        }
    }
    if (state->profiles.IsEmpty()) {
        AiTocUiProfile p;
        p.name = str::Dup("Default");
        p.baseUrl = str::Dup(prefs->aiTocApiBaseUrl);
        p.key = str::Dup(prefs->aiTocApiKey);
        p.model = str::Dup(prefs->aiTocApiModel);
        p.concurrency = limitValue(prefs->aiTocApiConcurrency, 1, 8);
        state->profiles.Append(p);
    }
    state->current = limitValue(state->current, 0, state->profiles.Size() - 1);
    SetPropW(hDlg, kAiTocSettingsStateProp, state);
    AiTocShowProfile(hDlg);
    AiTocAddFetchModelsTip(hDlg);
}

static void AiTocCommitProfiles(HWND hDlg, GlobalPrefs* prefs) {
    auto* state = GetAiTocSettingsState(hDlg);
    if (!state) {
        return;
    }
    AiTocSaveVisibleProfile(hDlg);
    auto& active = state->profiles[state->current];
    str::ReplaceWithCopy(&prefs->aiTocApiBaseUrl, active.baseUrl ? active.baseUrl : "");
    str::ReplaceWithCopy(&prefs->aiTocApiKey, active.key ? active.key : "");
    str::ReplaceWithCopy(&prefs->aiTocApiModel, active.model ? active.model : "");
    prefs->aiTocApiConcurrency = active.concurrency;
    StrBuilder out;
    out.AppendFmt("{\"active\":%d,\"profiles\":[", state->current);
    for (int i = 0; i < state->profiles.Size(); i++) {
        auto& p = state->profiles[i];
        if (i) {
            out.AppendChar(',');
        }
        out.Append("{\"name\":");
        AiTocJsonString(out, p.name);
        out.Append(",\"base_url\":");
        AiTocJsonString(out, p.baseUrl);
        out.Append(",\"api_key\":");
        AiTocJsonString(out, p.key);
        out.Append(",\"model\":");
        AiTocJsonString(out, p.model);
        out.AppendFmt(",\"concurrency\":%d}", p.concurrency);
    }
    out.Append("]}");
    str::ReplaceWithCopy(&prefs->aiTocApiProfiles, out.Get());
}

struct AiTocFetchModelsReq {
    HWND hwnd = nullptr;
    LONG token = 0;
    unsigned generation = 0;
    char* baseUrl = nullptr;
    char* key = nullptr;
    Vec<char*> models;
    char* error = nullptr;

    ~AiTocFetchModelsReq() {
        str::Free(baseUrl);
        str::Free(key);
        str::Free(error);
        for (auto* model : models) {
            str::Free(model);
        }
        models.Reset();
    }
};

struct AiModelPickData {
    Vec<char*>* models = nullptr;
    const char* current = nullptr;
    char* chosen = nullptr;
};

static void AiModelPickFill(HWND hDlg, const char* filter) {
    auto* data = (AiModelPickData*)GetWindowLongPtr(hDlg, GWLP_USERDATA);
    HWND list = GetDlgItem(hDlg, IDC_AI_MODEL_LIST);
    int keepSrc = -1;
    int sel = (int)SendMessageW(list, LB_GETCURSEL, 0, 0);
    if (sel >= 0) {
        LRESULT src = SendMessageW(list, LB_GETITEMDATA, sel, 0);
        if (src != LB_ERR) {
            keepSrc = (int)src;
        }
    }
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    int selectShown = -1;
    if (data && data->models) {
        for (int i = 0; i < data->models->Size(); i++) {
            const char* name = data->models->At(i);
            if (filter && *filter && !str::ContainsI(name, filter)) {
                continue;
            }
            int idx = (int)SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)ToWStrTemp(name));
            if (idx < 0) {
                continue;
            }
            SendMessageW(list, LB_SETITEMDATA, idx, (LPARAM)i);
            bool keep = keepSrc == i;
            bool current = keepSrc < 0 && data->current && str::EqI(name, data->current);
            if (keep || (selectShown < 0 && current)) {
                selectShown = idx;
            }
        }
    }
    if (selectShown >= 0) {
        SendMessageW(list, LB_SETCURSEL, selectShown, 0);
    }
    EnableWindow(GetDlgItem(hDlg, IDOK), selectShown >= 0);
}

static void AiModelPickApplySelection(HWND hDlg) {
    auto* data = (AiModelPickData*)GetWindowLongPtr(hDlg, GWLP_USERDATA);
    HWND list = GetDlgItem(hDlg, IDC_AI_MODEL_LIST);
    int sel = (int)SendMessageW(list, LB_GETCURSEL, 0, 0);
    if (!data || !data->models || sel < 0) {
        return;
    }
    LRESULT src = SendMessageW(list, LB_GETITEMDATA, sel, 0);
    if (src == LB_ERR || src < 0 || src >= data->models->Size()) {
        return;
    }
    str::ReplaceWithCopy(&data->chosen, data->models->At((int)src));
    EndDialog(hDlg, IDOK);
}

static INT_PTR CALLBACK Dialog_AiModelPick_Proc(HWND hDlg, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_INITDIALOG) {
        auto* data = (AiModelPickData*)lp;
        SetWindowLongPtr(hDlg, GWLP_USERDATA, (LONG_PTR)data);
        if (UseDarkModeLib()) {
            DarkMode::setDarkWndSafe(hDlg);
        }
        UpdateWindowCaptionTheme(hDlg);
        HwndSetText(hDlg, _TRA("Choose a model"));
        HwndSetDlgItemText(hDlg, IDOK, _TRA("OK"));
        HwndSetDlgItemText(hDlg, IDCANCEL, _TRA("Cancel"));
        HWND search = GetDlgItem(hDlg, IDC_AI_MODEL_SEARCH);
        SendMessageW(search, EM_SETCUEBANNER, TRUE, (LPARAM)ToWStrTemp(_TRA("Search models")));
        AiModelPickFill(hDlg, nullptr);
        CenterDialog(hDlg);
        HwndSetFocus(search);
        return FALSE;
    }
    switch (msg) {
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDC_AI_MODEL_SEARCH:
                    if (HIWORD(wp) == EN_CHANGE) {
                        char* filter = str::Dup(HwndGetTextTemp(GetDlgItem(hDlg, IDC_AI_MODEL_SEARCH)));
                        if (filter) {
                            str::TrimWSInPlace(filter, str::TrimOpt::Both);
                        }
                        AiModelPickFill(hDlg, filter);
                        str::Free(filter);
                    }
                    return TRUE;
                case IDC_AI_MODEL_LIST:
                    if (HIWORD(wp) == LBN_SELCHANGE) {
                        int sel = (int)SendMessageW(GetDlgItem(hDlg, IDC_AI_MODEL_LIST), LB_GETCURSEL, 0, 0);
                        EnableWindow(GetDlgItem(hDlg, IDOK), sel >= 0);
                    } else if (HIWORD(wp) == LBN_DBLCLK) {
                        AiModelPickApplySelection(hDlg);
                    }
                    return TRUE;
                case IDOK:
                    AiModelPickApplySelection(hDlg);
                    return TRUE;
                case IDCANCEL:
                    EndDialog(hDlg, IDCANCEL);
                    return TRUE;
            }
            break;
    }
    return FALSE;
}

// Speech, image-generation, and embedding models are not chat completions.
// Match a whole name segment so "asr" does not hide an unrelated model.
static bool AiModelNameHasSegment(const char* name, const char* segment) {
    if (!name || !segment || !*segment) {
        return false;
    }
    size_t n = str::Len(segment);
    const char* p = name;
    while (*p) {
        const char* start = p;
        while (*p && *p != '-' && *p != '_' && *p != '/') {
            p++;
        }
        if ((size_t)(p - start) == n && str::EqNI(start, segment, n)) {
            return true;
        }
        if (*p) {
            p++;
        }
    }
    return false;
}

static bool AiModelNameUnsupported(const char* name) {
    static const char* segments[] = {"tts",        "asr",    "realtime",   "livetranslate", "image",
                                     "embedding",  "rerank", "paraformer", "cosyvoice",     "sambert",
                                     "sensevoice", "wanx",   "mt"};
    for (const char* segment : segments) {
        if (AiModelNameHasSegment(name, segment)) {
            return true;
        }
    }
    return false;
}

static int CmpFetchedModelName(const void* a, const void* b) {
    const char* sa = *(char* const*)a;
    const char* sb = *(char* const*)b;
    if (!sa || !sb) {
        return (sa ? 1 : 0) - (sb ? 1 : 0);
    }
    return str::CmpNatural(sa, sb);
}

static void AiTocFetchModelsFinished(AiTocFetchModelsReq* req) {
    if (IsWindow(req->hwnd)) {
        auto* state = GetAiTocSettingsState(req->hwnd);
        if (state && state->token == req->token && state->fetchGeneration == req->generation) {
            EnableWindow(GetDlgItem(req->hwnd, IDC_AITOC_API_FETCH_MODELS), TRUE);
            if (req->error) {
                MessageBoxW(req->hwnd, ToWStrTemp(req->error), ToWStrTemp(_TRA("Choose a model")), MB_ICONWARNING);
            } else {
                AiTocClearModels(state);
                for (auto* model : req->models) {
                    if (AiModelNameUnsupported(model)) {
                        continue;
                    }
                    state->models.Append(str::Dup(model));
                }
                if (state->models.IsEmpty()) {
                    MessageBoxW(req->hwnd,
                                ToWStrTemp(_TRA("None of the models from the API can be used here. Type a chat model "
                                                "name instead.")),
                                ToWStrTemp(_TRA("Choose a model")), MB_ICONINFORMATION);
                } else {
                    state->models.Sort(CmpFetchedModelName);
                    AiModelPickData pick;
                    pick.models = &state->models;
                    char* current = str::Dup(HwndGetTextTemp(GetDlgItem(req->hwnd, IDC_AITOC_API_MODEL)));
                    pick.current = current;
                    if (CreateAppDialogBox(IDD_DIALOG_AI_MODEL_PICK, req->hwnd, Dialog_AiModelPick_Proc,
                                           (LPARAM)&pick) == IDOK &&
                        pick.chosen) {
                        HwndSetDlgItemText(req->hwnd, IDC_AITOC_API_MODEL, pick.chosen);
                    }
                    str::Free(current);
                    str::Free(pick.chosen);
                }
            }
        }
    }
    delete req;
}

static void AiTocFetchModelsWorker(AiTocFetchModelsReq* req) {
    AiTocApiFetchModels(req->baseUrl, req->key, req->models, &req->error);
    uitask::Post(MkFunc0<AiTocFetchModelsReq>(AiTocFetchModelsFinished, req), "AiTocFetchModels");
}

struct AiTocApiTestReq {
    HWND hwnd = nullptr;
    LONG token = 0;
    char* baseUrl = nullptr;
    char* key = nullptr;
    char* model = nullptr;
    char* msg = nullptr;
    bool ok = false;
};

static void AiTocApiTestFinished(AiTocApiTestReq* req) {
    auto* state = IsWindow(req->hwnd) ? GetAiTocSettingsState(req->hwnd) : nullptr;
    HWND owner = state && state->token == req->token ? req->hwnd : nullptr;
    const WCHAR* caption = ToWStrTemp(_TRA("AI table of contents"));
    if (req->msg && owner) {
        MessageBoxW(owner, ToWStrTemp(req->msg), caption, req->ok ? MB_ICONINFORMATION : MB_ICONWARNING);
    }
    str::Free(req->msg);
    str::Free(req->baseUrl);
    str::Free(req->key);
    str::Free(req->model);
    delete req;
}

static void AiTocApiTestWorker(AiTocApiTestReq* req) {
    char* msg = nullptr;
    req->ok = AiTocApiTestConnection(req->baseUrl, req->key, req->model, &msg);
    req->msg = msg;
    uitask::Post(MkFunc0<AiTocApiTestReq>(AiTocApiTestFinished, req), "AiTocApiTest");
}

struct VolcTranslateTestReq {
    HWND hwnd = nullptr;
    LONG token = 0;
    char* ak = nullptr;
    char* sk = nullptr;
    char* result = nullptr;
    char* err = nullptr;
    bool ok = false;
};

static void VolcTranslateTestFinished(VolcTranslateTestReq* req) {
    auto* state = IsWindow(req->hwnd) ? GetAiTocSettingsState(req->hwnd) : nullptr;
    HWND owner = state && state->token == req->token ? req->hwnd : nullptr;
    if (owner) {
        EnableWindow(GetDlgItem(owner, IDC_TRANSLATE_VOLC_TEST), TRUE);
        const WCHAR* caption = ToWStrTemp(_TRA("Volcengine Translate"));
        TempStr msg = nullptr;
        if (req->ok) {
            msg = str::FormatTemp("%s\n\nHello, world. \xE2\x86\x92 %s", _TRA("Volcengine Translate works."),
                                  req->result ? req->result : "");
        } else {
            msg = str::FormatTemp("%s\n\n%s", _TRA("Volcengine Translate failed:"), req->err ? req->err : "");
        }
        MessageBoxW(owner, ToWStrTemp(msg), caption, req->ok ? MB_ICONINFORMATION : MB_ICONWARNING);
    }
    str::Free(req->ak);
    str::Free(req->sk);
    str::Free(req->result);
    str::Free(req->err);
    delete req;
}

static void VolcTranslateTestWorker(VolcTranslateTestReq* req) {
    req->ok = InlineTranslateVolcTest(req->ak, req->sk, &req->result, &req->err);
    uitask::Post(MkFunc0<VolcTranslateTestReq>(VolcTranslateTestFinished, req), "VolcTranslateTest");
}

static void UpdateSettingsDependencies(HWND hDlg) {
    bool rememberFiles = IsDlgButtonChecked(hDlg, IDC_REMEMBER_OPENED_FILES) == BST_CHECKED;
    EnableWindow(GetDlgItem(hDlg, IDC_REMEMBER_STATE_PER_DOCUMENT), rememberFiles);

    bool useTabs = IsDlgButtonChecked(hDlg, IDC_USE_TABS) == BST_CHECKED;
    EnableWindow(GetDlgItem(hDlg, IDC_NO_HOME_TAB), useTabs);
    EnableWindow(GetDlgItem(hDlg, IDC_SHOW_MENUBAR_WITH_TABS), useTabs);

    bool showToolbar = IsDlgButtonChecked(hDlg, IDC_SHOW_TOOLBAR) == BST_CHECKED;
    EnableWindow(GetDlgItem(hDlg, IDC_SHOW_ANNOT_TOOLBAR_BUTTONS), showToolbar);

    bool restoreSession = IsDlgButtonChecked(hDlg, IDC_RESTORE_SESSION) == BST_CHECKED;
    EnableWindow(GetDlgItem(hDlg, IDC_LAZY_LOADING), restoreSession);

    bool wordLookup = IsDlgButtonChecked(hDlg, IDC_ENABLE_WORD_LOOKUP) == BST_CHECKED;
    EnableWindow(GetDlgItem(hDlg, IDC_DICTIONARY_PATH_LABEL), wordLookup);
    EnableWindow(GetDlgItem(hDlg, IDC_DICTIONARY_PATH), wordLookup);
    EnableWindow(GetDlgItem(hDlg, IDC_DICTIONARY_BROWSE), wordLookup);

    bool askAi = IsDlgButtonChecked(hDlg, IDC_ENABLE_ASK_AI) == BST_CHECKED;
    EnableWindow(GetDlgItem(hDlg, IDC_AI_PROVIDER_LABEL), askAi);
    EnableWindow(GetDlgItem(hDlg, IDC_AI_PROVIDER), askAi);

    bool inlineTr = IsDlgButtonChecked(hDlg, IDC_ENABLE_INLINE_TRANSLATE) == BST_CHECKED;
    EnableWindow(GetDlgItem(hDlg, IDC_TRANSLATE_TARGET_LABEL), inlineTr);
    EnableWindow(GetDlgItem(hDlg, IDC_TRANSLATE_TARGET), inlineTr);
    EnableWindow(GetDlgItem(hDlg, IDC_TRANSLATE_VOLC_AK_LABEL), inlineTr);
    EnableWindow(GetDlgItem(hDlg, IDC_TRANSLATE_VOLC_AK), inlineTr);
    EnableWindow(GetDlgItem(hDlg, IDC_TRANSLATE_VOLC_SK_LABEL), inlineTr);
    EnableWindow(GetDlgItem(hDlg, IDC_TRANSLATE_VOLC_SK), inlineTr);
    EnableWindow(GetDlgItem(hDlg, IDC_TRANSLATE_VOLC_TEST), inlineTr);
    EnableWindow(GetDlgItem(hDlg, IDC_TRANSLATE_HINT), inlineTr);
}

static void BrowseForDictionaryFolder(HWND hDlg) {
    BROWSEINFOW bi{};
    bi.hwndOwner = hDlg;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    bi.lpszTitle = ToWStrTemp(_TRA("Choose the offline dictionary folder"));
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl) {
        return;
    }
    WCHAR path[MAX_PATH]{};
    if (SHGetPathFromIDListW(pidl, path)) {
        HwndSetDlgItemText(hDlg, IDC_DICTIONARY_PATH, ToUtf8Temp(path));
    }
    CoTaskMemFree(pidl);
}

static AppDialogBrushes gSettingsDialogBrushes;
static constexpr UINT_PTR kSettingsCategorySubclassId = 1;
static int gSettingsCategoryHover = -1;

// Extra pixels around the label so rows are not packed against each other.
static int SettingsCategoryRowExtra(HWND hwnd) {
    return DpiScale(hwnd, 8);
}

static int SettingsCategoryItemHeight(HWND hwnd, HFONT font) {
    HDC dc = GetDC(hwnd);
    HFONT old = nullptr;
    if (dc && font) {
        old = (HFONT)SelectObject(dc, font);
    }
    TEXTMETRICW tm{};
    if (dc) {
        GetTextMetricsW(dc, &tm);
    }
    if (dc && old) {
        SelectObject(dc, old);
    }
    if (dc) {
        ReleaseDC(hwnd, dc);
    }
    int h = tm.tmHeight + SettingsCategoryRowExtra(hwnd);
    if (h < 1) {
        h = DpiScale(hwnd, 22);
    }
    return h;
}

static void SettingsCategoryListSetItemHeight(HWND category) {
    if (!category) {
        return;
    }
    HFONT font = (HFONT)SendMessageW(category, WM_GETFONT, 0, 0);
    SendMessageW(category, LB_SETITEMHEIGHT, 0, SettingsCategoryItemHeight(category, font));
}

static void SettingsCategoryInvalidateItem(HWND category, int idx) {
    if (!category || idx < 0) {
        return;
    }
    RECT rc{};
    if (ListBox_GetItemRect(category, idx, &rc) != LB_ERR) {
        InvalidateRect(category, &rc, FALSE);
    }
}

static int SettingsCategoryItemFromPoint(HWND category, POINTS pts) {
    DWORD hit = (DWORD)SendMessageW(category, LB_ITEMFROMPOINT, 0, MAKELPARAM(pts.x, pts.y));
    if (HIWORD(hit)) {
        return -1;
    }
    int idx = (int)LOWORD(hit);
    int n = (int)ListBox_GetCount(category);
    if (idx < 0 || idx >= n) {
        return -1;
    }
    return idx;
}

static LRESULT CALLBACK SettingsCategorySubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR subclassId,
                                                     DWORD_PTR) {
    switch (msg) {
        case WM_MOUSEMOVE: {
            POINTS pts = MAKEPOINTS(lp);
            int idx = SettingsCategoryItemFromPoint(hwnd, pts);
            if (idx != gSettingsCategoryHover) {
                int old = gSettingsCategoryHover;
                gSettingsCategoryHover = idx;
                SettingsCategoryInvalidateItem(hwnd, old);
                SettingsCategoryInvalidateItem(hwnd, idx);
            }
            TRACKMOUSEEVENT tme{};
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            TrackMouseEvent(&tme);
            break;
        }
        case WM_MOUSELEAVE:
            if (gSettingsCategoryHover >= 0) {
                int old = gSettingsCategoryHover;
                gSettingsCategoryHover = -1;
                SettingsCategoryInvalidateItem(hwnd, old);
            }
            break;
        case WM_ERASEBKGND: {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            COLORREF bg{};
            COLORREF text{};
            ThemeSidebarColors(bg, text);
            HBRUSH br = CreateSolidBrush(bg);
            FillRect((HDC)wp, &rc, br);
            DeleteObject(br);
            return TRUE;
        }
        case WM_NCDESTROY:
            gSettingsCategoryHover = -1;
            RemoveWindowSubclass(hwnd, SettingsCategorySubclassProc, subclassId);
            break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static void SettingsCategoryListInstallHover(HWND category) {
    if (!category) {
        return;
    }
    gSettingsCategoryHover = -1;
    RemoveWindowSubclass(category, SettingsCategorySubclassProc, kSettingsCategorySubclassId);
    SetWindowSubclass(category, SettingsCategorySubclassProc, kSettingsCategorySubclassId, 0);
}

// TOC-matched colors: sidebar bg/text, AccentColor selection/hover (no system blue invert).
static void SettingsCategoryDrawItem(HWND category, DRAWITEMSTRUCT* dis) {
    if (!dis || dis->itemID == (UINT)-1) {
        return;
    }
    HDC hdc = dis->hDC;
    RECT rc = dis->rcItem;
    bool selected = (dis->itemState & ODS_SELECTED) != 0;
    bool hovered = ((int)dis->itemID == gSettingsCategoryHover) && !selected;

    COLORREF rowBg{};
    COLORREF text{};
    ThemeSidebarColors(rowBg, text);
    HBRUSH rowBr = CreateSolidBrush(rowBg);
    FillRect(hdc, &rc, rowBr);
    DeleteObject(rowBr);

    // Leave a gap between rows. The highlight sits inside that gap so it does not touch the next label.
    int gap = DpiScale(category, 2);
    RECT box = rc;
    box.top += gap;
    box.bottom -= gap;
    if (box.bottom <= box.top) {
        box = rc;
    }
    if (selected || hovered) {
        COLORREF boxBg = AccentColor(ThemeWindowControlBackgroundColor(), selected ? 25 : 12);
        HBRUSH boxBr = CreateSolidBrush(boxBg);
        FillRect(hdc, &box, boxBr);
        DeleteObject(boxBr);
    }
    if (ThemeUsesDarkChrome() && selected) {
        COLORREF frame = AccentColor(ThemeWindowLinkColor(), -20);
        HPEN pen = CreatePen(PS_SOLID, 1, frame);
        HPEN oldPen = (HPEN)SelectObject(hdc, pen);
        HBRUSH oldBr = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, box.left, box.top, box.right, box.bottom);
        SelectObject(hdc, oldBr);
        SelectObject(hdc, oldPen);
        DeleteObject(pen);
    }

    WCHAR buf[256]{};
    int n = (int)SendMessageW(category, LB_GETTEXT, dis->itemID, (LPARAM)buf);
    if (n < 0) {
        n = 0;
    }

    HFONT font = (HFONT)SendMessageW(category, WM_GETFONT, 0, 0);
    HFONT oldFont = font ? (HFONT)SelectObject(hdc, font) : nullptr;
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, text);
    TEXTMETRICW tm{};
    GetTextMetricsW(hdc, &tm);
    RECT textRc = box;
    int padX = DpiScale(category, 8);
    textRc.left += padX;
    textRc.right -= padX;
    // DT_VCENTER centers the em box. Internal leading sits above the glyphs, so the
    // letters look low in the highlight. Shift up by half of that leading.
    int nudge = tm.tmInternalLeading / 2;
    if (nudge > 0) {
        textRc.top -= nudge;
        textRc.bottom -= nudge;
    }
    DrawTextW(hdc, buf, n, &textRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    if (oldFont) {
        SelectObject(hdc, oldFont);
    }
}

static BOOL CALLBACK SoftenSettingsInputTheme(HWND hwnd, LPARAM) {
    WCHAR cls[32]{};
    GetClassNameW(hwnd, cls, dimof(cls));
    // Untheme Edit/Combo so Warm CTLCOLOR cream shows instead of pure white.
    if (str::EqI(cls, L"Edit") || str::EqI(cls, L"ComboBox")) {
        if (DynSetWindowTheme) {
            DynSetWindowTheme(hwnd, L"", L"");
        }
    }
    return TRUE;
}

static INT_PTR CALLBACK Dialog_Settings_Proc(HWND hDlg, UINT msg, WPARAM wp, LPARAM lp) {
    GlobalPrefs* prefs;

    switch (msg) {
        //[ ACCESSKEY_GROUP Settings Dialog
        case WM_INITDIALOG: {
            prefs = (GlobalPrefs*)lp;
            SetWindowLongPtr(hDlg, GWLP_USERDATA, (LONG_PTR)prefs);
            gSettingsDialogBrushes.Create();
            AppDialogApplyChrome(hDlg);
            EnumChildWindows(hDlg, SoftenSettingsInputTheme, 0);
            {
                HWND hwndCb = GetDlgItem(hDlg, IDC_DEFAULT_LAYOUT);
                // Fill the page layouts into the select box
                CbAddString(hwndCb, _TRA("Automatic"));
                CbAddString(hwndCb, _TRA("Single Page"));
                CbAddString(hwndCb, _TRA("Facing"));
                CbAddString(hwndCb, _TRA("Book View"));
                CbAddString(hwndCb, _TRA("Continuous"));
                CbAddString(hwndCb, _TRA("Continuous Facing"));
                CbAddString(hwndCb, _TRA("Continuous Book View"));
                int selIdx = (int)prefs->defaultDisplayModeEnum - (int)DisplayMode::Automatic;
                CbSetCurrentSelection(hwndCb, selIdx);
            }

            SetupZoomComboBox(hDlg, IDC_DEFAULT_ZOOM, false, prefs->defaultZoomFloat);

            CheckDlgButton(hDlg, IDC_DEFAULT_SHOW_TOC, prefs->showToc ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hDlg, IDC_REMEMBER_STATE_PER_DOCUMENT,
                           prefs->rememberStatePerDocument ? BST_CHECKED : BST_UNCHECKED);
            EnableWindow(GetDlgItem(hDlg, IDC_REMEMBER_STATE_PER_DOCUMENT), prefs->rememberOpenedFiles);
            CheckDlgButton(hDlg, IDC_USE_TABS, prefs->useTabs ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hDlg, IDC_CHECK_FOR_UPDATES, prefs->checkForUpdates ? BST_CHECKED : BST_UNCHECKED);
            EnableWindow(GetDlgItem(hDlg, IDC_CHECK_FOR_UPDATES), HasPermission(Perm::InternetAccess));
            CheckDlgButton(hDlg, IDC_REMEMBER_OPENED_FILES, prefs->rememberOpenedFiles ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hDlg, IDC_RESTORE_SESSION, prefs->restoreSession ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hDlg, IDC_LAZY_LOADING, prefs->lazyLoading ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hDlg, IDC_REUSE_INSTANCE, prefs->reuseInstance ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hDlg, IDC_NO_HOME_TAB, !prefs->noHomeTab ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hDlg, IDC_SHOW_MENUBAR_WITH_TABS, prefs->showMenubarWithTabs ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hDlg, IDC_SHOW_TOOLBAR, prefs->showToolbar ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hDlg, IDC_SHOW_ANNOT_TOOLBAR_BUTTONS,
                           prefs->showAnnotToolbarButtons ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hDlg, IDC_TABS_MRU, prefs->tabsMru ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hDlg, IDC_SEARCH_UI_FLOATING, prefs->searchUIFloating ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hDlg, IDC_SMOOTH_SCROLL, prefs->smoothScroll ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hDlg, IDC_SCROLLBAR_SINGLE_PAGE, prefs->scrollbarInSinglePage ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hDlg, IDC_RELOAD_MODIFIED, prefs->reloadModifiedDocuments ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hDlg, IDC_PREVENT_SLEEP_FULLSCREEN,
                           prefs->preventSleepInFullscreen ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hDlg, IDC_TREE_WRAP_LABELS, prefs->treeWrapLabels ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hDlg, IDC_FAST_SCROLL_OVER_SCROLLBAR,
                           prefs->fastScrollOverScrollbar ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hDlg, IDC_ENABLE_ANTIALIAS, !prefs->disableAntiAlias ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hDlg, IDC_ENABLE_WORD_LOOKUP,
                           prefs->enableDoubleClickWordLookup ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hDlg, IDC_AUTO_OCR, prefs->autoOcrScanPages ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hDlg, IDC_OCR_AUTO_SAVE, prefs->ocrAutoSave ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hDlg, IDC_ENABLE_ASK_AI, prefs->enableAskAI ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hDlg, IDC_ENABLE_INLINE_TRANSLATE,
                           prefs->enableInlineTranslate ? BST_CHECKED : BST_UNCHECKED);
            HwndSetDlgItemText(hDlg, IDC_TRANSLATE_VOLC_AK, prefs->translateVolcAccessKey);
            HwndSetDlgItemText(hDlg, IDC_TRANSLATE_VOLC_SK, prefs->translateVolcSecretKey);
            CheckDlgButton(hDlg, IDC_ESC_TO_EXIT, prefs->escToExit ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hDlg, IDC_FULL_PATH_IN_TITLE, prefs->fullPathInTitle ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hDlg, IDC_SHOW_LINKS, prefs->showLinks ? BST_CHECKED : BST_UNCHECKED);
            SetDlgItemInt(hDlg, IDC_TREE_FONT_SIZE, prefs->treeFontSize, FALSE);
            SetDlgItemInt(hDlg, IDC_TAB_FONT_SIZE, prefs->tabFontSize, FALSE);
            SetDlgItemInt(hDlg, IDC_TAB_BAR_HEIGHT, prefs->tabBarHeight, FALSE);
            SetDlgItemInt(hDlg, IDC_CUSTOM_DPI, prefs->customScreenDPI, FALSE);
            HwndSetDlgItemText(hDlg, IDC_DICTIONARY_PATH, prefs->offlineDictionaryPath);
            HwndSetDlgItemText(hDlg, IDC_DEFAULT_AUTHOR, prefs->annotations.defaultAuthor);
            SendMessageW(GetDlgItem(hDlg, IDC_DEFAULT_AUTHOR), EM_SETCUEBANNER, TRUE,
                         (LPARAM)ToWStrTemp(_TRA("Windows user name")));
            AiTocInitProfiles(hDlg, prefs);

            HWND category = GetDlgItem(hDlg, IDC_SETTINGS_CATEGORY);
            SettingsCategoryListSetItemHeight(category);
            SettingsCategoryListInstallHover(category);
            SendMessageW(category, LB_ADDSTRING, 0, (LPARAM)(WCHAR*)ToWStrTemp(_TRA("General")));
            SendMessageW(category, LB_ADDSTRING, 0, (LPARAM)(WCHAR*)ToWStrTemp(_TRA("Interface")));
            SendMessageW(category, LB_ADDSTRING, 0, (LPARAM)(WCHAR*)ToWStrTemp(_TRA("Reading")));
            SendMessageW(category, LB_ADDSTRING, 0, (LPARAM)(WCHAR*)ToWStrTemp(_TRA("Read Aloud")));
            SendMessageW(category, LB_ADDSTRING, 0, (LPARAM)(WCHAR*)ToWStrTemp(_TRA("OCR")));
            SendMessageW(category, LB_ADDSTRING, 0, (LPARAM)(WCHAR*)ToWStrTemp(_TRA("AI")));
            SendMessageW(category, LB_ADDSTRING, 0, (LPARAM)(WCHAR*)ToWStrTemp(_TRA("Advanced")));
            ListBox_SetCurSel(category, gSettingsInitialPage);

            HWND scrollbars = GetDlgItem(hDlg, IDC_SCROLLBARS);
            CbAddString(scrollbars, _TRA("Windows scrollbars"));
            CbAddString(scrollbars, _TRA("Smart auto-hide scrollbars"));
            CbAddString(scrollbars, _TRA("Always-visible overlay scrollbars"));
            CbAddString(scrollbars, _TRA("No scrollbars"));
            int scrollbarIdx = seqstrings::StrToIdxIS(gScrollbarModeNames, prefs->scrollbars);
            CbSetCurrentSelection(scrollbars, std::max(0, scrollbarIdx));

            HWND themes = GetDlgItem(hDlg, IDC_THEME);
            int themeSelection = 0;
            for (int i = 0; i < GetThemeCount(); i++) {
                const char* name = GetThemeName(i);
                CbAddString(themes, name);
                if (str::EqI(name, prefs->theme)) {
                    themeSelection = i;
                }
            }
            CbSetCurrentSelection(themes, themeSelection);

            HWND documentColors = GetDlgItem(hDlg, IDC_DOCUMENT_COLOR);
            CbAddString(documentColors, _TRA("Keep original document colors"));
            CbAddString(documentColors, _TRA("Match the current theme"));
            CbSetCurrentSelection(documentColors, str::EqI(prefs->documentColorMode, "original") ? 0 : 1);

            HWND treeFont = GetDlgItem(hDlg, IDC_TREE_FONT_NAME);
            CbAddString(treeFont, _TRA("Automatic"));
            Vec<char*> fontFamilies;
            CollectInstalledLatinFontFamilies(&fontFamilies);
            Vec<char*> cjkFontFamilies;
            CollectInstalledCjkFontFamilies(&cjkFontFamilies);
            for (char* family : cjkFontFamilies) {
                bool exists = false;
                for (char* existing : fontFamilies) {
                    if (str::EqI(existing, family)) {
                        exists = true;
                        break;
                    }
                }
                if (!exists) {
                    fontFamilies.Append(str::Dup(family));
                }
            }
            int treeFontSelection = 0;
            int treeFontIndex = 1;
            for (char* family : fontFamilies) {
                CbAddString(treeFont, family);
                if (prefs->treeFontName && str::EqI(family, prefs->treeFontName)) {
                    treeFontSelection = treeFontIndex;
                }
                treeFontIndex++;
            }
            if (prefs->treeFontName &&
                (str::EqI(prefs->treeFontName, "automatic") || str::Eq(prefs->treeFontName, _TRA("Automatic")))) {
                treeFontSelection = 0;
            }
            CbSetCurrentSelection(treeFont, treeFontSelection);
            DeleteVecMembers(fontFamilies);
            DeleteVecMembers(cjkFontFamilies);

            HWND engineering = GetDlgItem(hDlg, IDC_ENGINEERING_ENHANCE);
            CbAddString(engineering, _TRA("Off"));
            CbAddString(engineering, _TRA("Automatic"));
            CbAddString(engineering, _TRA("On"));
            int engineeringSelection = str::EqI(prefs->engineeringDrawingEnhance, "on")    ? 2
                                       : str::EqI(prefs->engineeringDrawingEnhance, "off") ? 0
                                                                                           : 1;
            CbSetCurrentSelection(engineering, engineeringSelection);

            HWND tocMode = GetDlgItem(hDlg, IDC_TOC_MODE);
            CbAddString(tocMode, _TRA("Conservative"));
            CbAddString(tocMode, _TRA("Standard (recommended)"));
            CbAddString(tocMode, _TRA("Detailed"));
            int tocSelection = str::EqI(prefs->extractPdfTocMode, "conservative") ? 0
                               : str::EqI(prefs->extractPdfTocMode, "detailed")   ? 2
                                                                                  : 1;
            CbSetCurrentSelection(tocMode, tocSelection);

            HWND aiProvider = GetDlgItem(hDlg, IDC_AI_PROVIDER);
            CbAddString(aiProvider, _TRA("Doubao"));
            CbAddString(aiProvider, _TRA("DeepSeek"));
            CbAddString(aiProvider, _TRA("ChatGPT"));
            int providerSelection = str::EqI(prefs->aiChatProvider, "deepseek")  ? 1
                                    : str::EqI(prefs->aiChatProvider, "chatgpt") ? 2
                                                                                 : 0;
            CbSetCurrentSelection(aiProvider, providerSelection);

            HWND translateTarget = GetDlgItem(hDlg, IDC_TRANSLATE_TARGET);
            int nTargets = 0;
            const TranslateTargetOption* targets = GetTranslateTargetOptions(&nTargets);
            for (int i = 0; i < nTargets; i++) {
                CbAddString(translateTarget, _TRA(targets[i].label));
            }
            CbSetCurrentSelection(translateTarget, FindTranslateTargetOptionIndex(prefs->translateTargetMode));

            HwndSetText(hDlg, _TRA("SumatraPDF Options"));
            HwndSetDlgItemText(hDlg, IDC_SETTINGS_PAGE_GENERAL, _TRA("General"));
            HwndSetDlgItemText(hDlg, IDC_SETTINGS_PAGE_INTERFACE, _TRA("Interface"));
            HwndSetDlgItemText(hDlg, IDC_SETTINGS_PAGE_READING, _TRA("Reading"));
            HwndSetDlgItemText(hDlg, IDC_SETTINGS_PAGE_READ_ALOUD, _TRA("Read Aloud"));
            HwndSetDlgItemText(hDlg, IDC_SETTINGS_PAGE_OCR_AI, _TRA("OCR"));
            HwndSetDlgItemText(hDlg, IDC_SETTINGS_PAGE_AI, _TRA("AI"));
            HwndSetDlgItemText(hDlg, IDC_GROUP_RA_VOICE, _TRA("Voice"));
            HwndSetDlgItemText(hDlg, IDC_RA_VOICE_MODE_LABEL, _TRA("&Voice:"));
            HwndSetDlgItemText(hDlg, IDC_RA_VOICE_ZH_LABEL, _TRA("&Chinese voice:"));
            HwndSetDlgItemText(hDlg, IDC_RA_VOICE_EN_LABEL, _TRA("&English voice:"));
            HwndSetDlgItemText(hDlg, IDC_RA_VOICE_MULTI_LABEL, _TRA("&Multilingual voice:"));
            HwndSetDlgItemText(hDlg, IDC_RA_PREVIEW, _TRA("&Preview"));
            HwndSetDlgItemText(hDlg, IDC_GROUP_RA_HIGHLIGHT, _TRA("Highlight and follow"));
            HwndSetDlgItemText(hDlg, IDC_RA_HIGHLIGHT_COLOR_LABEL, _TRA("Highlight co&lor:"));
            HwndSetDlgItemText(hDlg, IDC_RA_HIGHLIGHT_RESET, _TRA("&Reset"));
            HwndSetDlgItemText(hDlg, IDC_RA_AUTO_FOLLOW, _TRA("&Follow the text being read"));
            HwndSetDlgItemText(hDlg, IDC_GROUP_RA_NARRATION, _TRA("Narrated books (EPUB 3)"));
            HwndSetDlgItemText(hDlg, IDC_RA_NARRATION_USE_AUDIO,
                               _TRA("Play the book's recorded narration when &available"));
            HwndSetDlgItemText(hDlg, IDC_RA_NARRATION_HINT,
                               _TRA("When off, narrated books are read with the voice above."));
            HwndSetDlgItemText(hDlg, IDC_RA_NARRATION_USE_COLOR, _TRA("Use the book's highlight c&olor"));
            HwndSetDlgItemText(hDlg, IDC_SETTINGS_PAGE_ADVANCED, _TRA("Advanced"));
            HwndSetDlgItemText(hDlg, IDC_GROUP_UPDATE, _TRA("Updates"));
            HwndSetDlgItemText(hDlg, IDC_GROUP_SESSION, _TRA("Startup and session"));
            HwndSetDlgItemText(hDlg, IDC_GROUP_FILE_CHANGES, _TRA("File changes"));
            HwndSetDlgItemText(hDlg, IDC_LAZY_LOADING, _TRA("&Lazy-load inactive tabs"));
            HwndSetDlgItemText(hDlg, IDC_GROUP_APPEARANCE, _TRA("Appearance"));
            HwndSetDlgItemText(hDlg, IDC_THEME_LABEL, _TRA("&Theme:"));
            HwndSetDlgItemText(hDlg, IDC_DOCUMENT_COLOR_LABEL, _TRA("Default document &colors:"));
            HwndSetDlgItemText(hDlg, IDC_GROUP_TOOLBAR, _TRA("Tabs and toolbar"));
            HwndSetDlgItemText(hDlg, IDC_TAB_FONT_SIZE_LABEL, _TRA("Tab fo&nt size (0 = auto, 6-72):"));
            HwndSetDlgItemText(hDlg, IDC_TAB_BAR_HEIGHT_LABEL, _TRA("Tab bar &height (0 = auto, 16-128):"));
            HwndSetDlgItemText(hDlg, IDC_GROUP_SCROLLBARS, _TRA("Scrollbars"));
            HwndSetDlgItemText(hDlg, IDC_GROUP_SIDEBAR, _TRA("Contents / favorites sidebar"));
            HwndSetDlgItemText(hDlg, IDC_TREE_FONT_LABEL, _TRA("Tree &font:"));
            HwndSetDlgItemText(hDlg, IDC_TREE_FONT_SIZE_LABEL, _TRA("Si&ze (0 = auto, 6-72):"));
            HwndSetDlgItemText(hDlg, IDC_TREE_WRAP_LABELS, _TRA("&Wrap long titles"));
            HwndSetDlgItemText(hDlg, IDC_GROUP_DEFAULT_VIEW, _TRA("Default view"));
            HwndSetDlgItemText(hDlg, IDC_GROUP_SCROLLING, _TRA("Scrolling"));
            HwndSetDlgItemText(hDlg, IDC_FAST_SCROLL_OVER_SCROLLBAR, _TRA("Fast page scrolling over the scroll&bar"));
            HwndSetDlgItemText(hDlg, IDC_GROUP_DISPLAY_QUALITY, _TRA("Display quality"));
            HwndSetDlgItemText(hDlg, IDC_ENGINEERING_ENHANCE_LABEL, _TRA("Engineering drawing &enhancement:"));
            HwndSetDlgItemText(hDlg, IDC_ENABLE_ANTIALIAS, _TRA("Enable PDF &anti-aliasing"));
            HwndSetDlgItemText(hDlg, IDC_GROUP_DICTIONARY, _TRA("Dictionary"));
            HwndSetDlgItemText(hDlg, IDC_ENABLE_WORD_LOOKUP, _TRA("Look up words on &double-click"));
            HwndSetDlgItemText(hDlg, IDC_DICTIONARY_PATH_LABEL, _TRA("Offline dictionary:"));
            HwndSetDlgItemText(hDlg, IDC_DICTIONARY_BROWSE, _TRA("&Browse..."));
            HwndSetDlgItemText(hDlg, IDC_GROUP_FULLSCREEN, _TRA("Fullscreen"));
            HwndSetDlgItemText(hDlg, IDC_GROUP_ANNOT_AUTHOR, _TRA("Annotations"));
            HwndSetDlgItemText(hDlg, IDC_DEFAULT_AUTHOR_LABEL, _TRA("Default &author:"));
            HwndSetDlgItemText(hDlg, IDC_GROUP_OCR, _TRA("OCR"));
            HwndSetDlgItemText(hDlg, IDC_AUTO_OCR, _TRA("Automatically OCR scanned pages"));
            HwndSetDlgItemText(hDlg, IDC_OCR_DESCRIPTION,
                               _TRA("Recognized text can be selected, copied, searched, and read aloud."));
            HwndSetDlgItemText(hDlg, IDC_OCR_AUTO_SAVE, _TRA("Automatically &save PDF after OCR or TOC processing"));
            HwndSetDlgItemText(hDlg, IDC_OCR_SAVE_WARNING,
                               _TRA("Processing results may overwrite the current PDF file."));
            HwndSetDlgItemText(hDlg, IDC_GROUP_SMART_TOC, _TRA("Smart contents"));
            HwndSetDlgItemText(hDlg, IDC_TOC_MODE_LABEL, _TRA("Extraction &detail:"));
            HwndSetDlgItemText(hDlg, IDC_TOC_MODE_DESCRIPTION,
                               _TRA("Balanced accuracy and completeness is recommended."));
            HwndSetDlgItemText(hDlg, IDC_GROUP_ASK_AI, _TRA("Web AI"));
            HwndSetDlgItemText(hDlg, IDC_ENABLE_ASK_AI, _TRA("&Enable Web AI"));
            HwndSetDlgItemText(hDlg, IDC_AI_PROVIDER_LABEL, _TRA("AI &service:"));
            HwndSetDlgItemText(hDlg, IDC_GROUP_INLINE_TRANSLATE, _TRA("Translate"));
            HwndSetDlgItemText(hDlg, IDC_ENABLE_INLINE_TRANSLATE, _TRA("Enable &inline translate"));
            HwndSetDlgItemText(hDlg, IDC_TRANSLATE_TARGET_LABEL, _TRA("Target &language:"));
            HwndSetDlgItemText(hDlg, IDC_TRANSLATE_VOLC_AK_LABEL, _TRA("Volc &Access Key:"));
            HwndSetDlgItemText(hDlg, IDC_TRANSLATE_VOLC_SK_LABEL, _TRA("Volc &Secret Key:"));
            HwndSetDlgItemText(hDlg, IDC_TRANSLATE_HINT,
                               _TRA("Without Volc keys, translation uses the AI API below when configured."));
            HwndSetDlgItemText(hDlg, IDC_GROUP_AITOC_API, _TRA("AI API (Q&A, translate fallback, TOC)"));
            HwndSetDlgItemText(hDlg, IDC_AITOC_API_PROFILE_LABEL, _TRA("&Platform:"));
            HwndSetDlgItemText(hDlg, IDC_AITOC_API_ADD, _TRA("&Add"));
            HwndSetDlgItemText(hDlg, IDC_AITOC_API_REMOVE, _TRA("&Remove"));
            HwndSetDlgItemText(hDlg, IDC_AITOC_API_NAME_LABEL, _TRA("&Name:"));
            HwndSetDlgItemText(hDlg, IDC_AITOC_API_BASEURL_LABEL, _TRA("API &base URL:"));
            HwndSetDlgItemText(hDlg, IDC_AITOC_API_KEY_LABEL, _TRA("API &key:"));
            HwndSetDlgItemText(hDlg, IDC_AITOC_API_MODEL_LABEL, _TRA("&Model:"));
            HwndSetDlgItemText(hDlg, IDC_AITOC_API_FETCH_MODELS, _TRA("&Choose..."));
            HwndSetDlgItemText(hDlg, IDC_AITOC_API_CONCURRENCY_LABEL, _TRA("&Parallel (1-8):"));
            HwndSetDlgItemText(hDlg, IDC_AITOC_API_TEST, _TRA("&Test"));
            HwndSetDlgItemText(hDlg, IDC_TRANSLATE_VOLC_TEST, _TRA("&Test"));
            HwndSetDlgItemText(hDlg, IDC_GROUP_WINDOW, _TRA("Window"));
            HwndSetDlgItemText(hDlg, IDC_ESC_TO_EXIT, _TRA("E&xit the application with Esc"));
            HwndSetDlgItemText(hDlg, IDC_FULL_PATH_IN_TITLE, _TRA("Show the full file &path in the title bar"));
            HwndSetDlgItemText(hDlg, IDC_GROUP_DISPLAY, _TRA("Display"));
            HwndSetDlgItemText(hDlg, IDC_CUSTOM_DPI_LABEL, _TRA("Custom &DPI (0 = automatic):"));
            HwndSetDlgItemText(hDlg, IDC_GROUP_PDF, _TRA("PDF"));
            HwndSetDlgItemText(hDlg, IDC_SHOW_LINKS, _TRA("Show &link borders"));
            HwndSetDlgItemText(hDlg, IDC_MORE_EXPERT_SETTINGS, _TRA("More expert settings"));
            HwndSetDlgItemText(hDlg, IDC_DEFAULT_LAYOUT_LABEL, _TRA("Default &Layout:"));
            HwndSetDlgItemText(hDlg, IDC_DEFAULT_ZOOM_LABEL, _TRA("Default &Zoom:"));
            HwndSetDlgItemText(hDlg, IDC_DEFAULT_SHOW_TOC, _TRA("Show the &bookmarks sidebar when available"));
            HwndSetDlgItemText(hDlg, IDC_REMEMBER_STATE_PER_DOCUMENT, _TRA("&Remember settings for each document"));
            HwndSetDlgItemText(hDlg, IDC_USE_TABS, _TRA("Use &tabs (requires restart)"));
            HwndSetDlgItemText(hDlg, IDC_CHECK_FOR_UPDATES, _TRA("Automatically check for &updates"));
            HwndSetDlgItemText(hDlg, IDC_REMEMBER_OPENED_FILES, _TRA("Remember &opened files"));
            HwndSetDlgItemText(hDlg, IDC_RESTORE_SESSION, _TRA("Restore the last &session at startup"));
            HwndSetDlgItemText(hDlg, IDC_REUSE_INSTANCE, _TRA("Open new files in the existing &instance"));
            HwndSetDlgItemText(hDlg, IDC_NO_HOME_TAB, _TRA("Keep a &Home tab (requires restart)"));
            HwndSetDlgItemText(hDlg, IDC_SHOW_MENUBAR_WITH_TABS, _TRA("Show the &menu bar with tabs"));
            HwndSetDlgItemText(hDlg, IDC_SHOW_TOOLBAR, _TRA("Show the tool&bar"));
            HwndSetDlgItemText(hDlg, IDC_SHOW_ANNOT_TOOLBAR_BUTTONS, _TRA("Show quick &annotation buttons"));
            HwndSetDlgItemText(hDlg, IDC_TABS_MRU, _TRA("Ctrl+Tab uses most recently used &order"));
            HwndSetDlgItemText(hDlg, IDC_SEARCH_UI_FLOATING, _TRA("Use the &floating search window"));
            HwndSetDlgItemText(hDlg, IDC_SCROLLBARS_LABEL, _TRA("&Scrollbars:"));
            HwndSetDlgItemText(hDlg, IDC_SMOOTH_SCROLL, _TRA("Use s&mooth scrolling"));
            HwndSetDlgItemText(hDlg, IDC_SCROLLBAR_SINGLE_PAGE, _TRA("Show a scrollbar in single-&page mode"));
            HwndSetDlgItemText(hDlg, IDC_RELOAD_MODIFIED, _TRA("Automatically &reload changed documents"));
            HwndSetDlgItemText(hDlg, IDC_PREVENT_SLEEP_FULLSCREEN,
                               _TRA("Prevent sleep in &fullscreen or presentation mode"));
            HwndSetDlgItemText(hDlg, IDC_SECTION_INVERSESEARCH, _TRA("Set inverse search command-line"));
            HwndSetDlgItemText(hDlg, IDC_CMDLINE_LABEL, _TRA("Command invoked when you double-click a PDF document:"));
            HwndSetDlgItemText(hDlg, IDC_OPEN_ADVANCED_OPTIONS, _TRA("Open &Advanced Options File..."));
            HwndSetDlgItemText(hDlg, IDOK, _TRA("OK"));
            HwndSetDlgItemText(hDlg, IDCANCEL, _TRA("Cancel"));

            if (prefs->enableTeXEnhancements && CanAccessDisk()) {
                // Fill the combo with the list of possible inverse search commands
                // Try to select a correct default when first showing this dialog
                const char* cmdLine = prefs->inverseSearchCmdLine;
                HWND hwndComboBox = GetDlgItem(hDlg, IDC_CMDLINE);
                Vec<TextEditor*> textEditors;
                DetectTextEditors(textEditors);
                StrVec detected;
                for (auto e : textEditors) {
                    const char* open = e->openFileCmd;
                    AppendIfNotExists(&detected, open);
                }
                if (cmdLine) {
                    AppendIfNotExists(&detected, cmdLine);
                } else {
                    if (detected.Size() > 0) {
                        cmdLine = detected[0];
                    }
                }
                for (char* s : detected) {
                    // if no existing command was selected then set the user custom command in the combo
                    CbAddString(hwndComboBox, s);
                }

                // Find the index of the active command line
                TempWStr cmdLineW = ToWStrTemp(cmdLine);
                LRESULT ind = SendMessageW(hwndComboBox, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)cmdLineW);
                if (CB_ERR == ind) {
                    HwndSetDlgItemText(hDlg, IDC_CMDLINE, cmdLine);
                } else {
                    // select the active command
                    CbSetCurrentSelection(hwndComboBox, ind);
                }
            } else {
                ShowWindow(GetDlgItem(hDlg, IDC_SECTION_INVERSESEARCH), SW_HIDE);
                ShowWindow(GetDlgItem(hDlg, IDC_CMDLINE_LABEL), SW_HIDE);
                ShowWindow(GetDlgItem(hDlg, IDC_CMDLINE), SW_HIDE);
            }

            ReadAloudSettingsPageInit(hDlg);
            FitSettingsInterfaceLabels(hDlg);
            FitSettingsReadingLabels(hDlg);
            FitSettingsReadAloudLabels(hDlg);
            FitSettingsOcrLabels(hDlg);
            FitSettingsAiLabels(hDlg);
            ShowSettingsPage(hDlg, gSettingsInitialPage);
            UpdateSettingsDependencies(hDlg);
            CenterDialog(hDlg);
            HwndSetFocus(category);
            return FALSE;
        }
            //] ACCESSKEY_GROUP Settings Dialog

        case WM_MEASUREITEM: {
            MEASUREITEMSTRUCT* mis = (MEASUREITEMSTRUCT*)lp;
            if (mis && mis->CtlID == IDC_SETTINGS_CATEGORY) {
                HFONT font = (HFONT)SendMessageW(hDlg, WM_GETFONT, 0, 0);
                mis->itemHeight = (UINT)SettingsCategoryItemHeight(hDlg, font);
                return TRUE;
            }
            break;
        }

        case WM_DRAWITEM: {
            DRAWITEMSTRUCT* dis = (DRAWITEMSTRUCT*)lp;
            if (dis && dis->CtlID == IDC_SETTINGS_CATEGORY) {
                SettingsCategoryDrawItem(dis->hwndItem, dis);
                return TRUE;
            }
            if (ReadAloudSettingsPageDrawItem(dis)) {
                return TRUE;
            }
            break;
        }

        case WM_CTLCOLORDLG:
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX: {
            HBRUSH br =
                AppDialogCtlColorBrush(msg, wp, lp, gSettingsDialogBrushes.background, gSettingsDialogBrushes.control);
            if (br) {
                return (INT_PTR)br;
            }
            break;
        }

        case WM_DESTROY:
            ReadAloudSettingsPageDestroy();
            delete GetAiTocSettingsState(hDlg);
            RemovePropW(hDlg, kAiTocSettingsStateProp);
            gSettingsCategoryHover = -1;
            gSettingsDialogBrushes.Destroy();
            break;

        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDOK: {
                    prefs = (GlobalPrefs*)GetWindowLongPtr(hDlg, GWLP_USERDATA);
                    BOOL treeSizeOk = FALSE;
                    int treeFontSize = (int)GetDlgItemInt(hDlg, IDC_TREE_FONT_SIZE, &treeSizeOk, FALSE);
                    BOOL tabFontSizeOk = FALSE;
                    int tabFontSize = (int)GetDlgItemInt(hDlg, IDC_TAB_FONT_SIZE, &tabFontSizeOk, FALSE);
                    BOOL tabBarHeightOk = FALSE;
                    int tabBarHeight = (int)GetDlgItemInt(hDlg, IDC_TAB_BAR_HEIGHT, &tabBarHeightOk, FALSE);
                    BOOL dpiOk = FALSE;
                    int customDpi = (int)GetDlgItemInt(hDlg, IDC_CUSTOM_DPI, &dpiOk, FALSE);
                    if (!treeSizeOk || (treeFontSize != 0 && (treeFontSize < 6 || treeFontSize > 72))) {
                        MessageBoxWarning(hDlg, _TRA("Tree font size must be 0 (automatic) or between 6 and 72."),
                                          _TRA("Invalid value"));
                        HwndSetFocus(GetDlgItem(hDlg, IDC_TREE_FONT_SIZE));
                        return TRUE;
                    }
                    if (!tabFontSizeOk || (tabFontSize != 0 && (tabFontSize < 6 || tabFontSize > 72))) {
                        MessageBoxWarning(hDlg, _TRA("Tab font size must be 0 (automatic) or between 6 and 72."),
                                          _TRA("Invalid value"));
                        HwndSetFocus(GetDlgItem(hDlg, IDC_TAB_FONT_SIZE));
                        return TRUE;
                    }
                    if (!tabBarHeightOk || (tabBarHeight != 0 && (tabBarHeight < 16 || tabBarHeight > 128))) {
                        MessageBoxWarning(hDlg, _TRA("Tab bar height must be 0 (automatic) or between 16 and 128."),
                                          _TRA("Invalid value"));
                        HwndSetFocus(GetDlgItem(hDlg, IDC_TAB_BAR_HEIGHT));
                        return TRUE;
                    }
                    if (!dpiOk || (customDpi != 0 && (customDpi < 72 || customDpi > 600))) {
                        MessageBoxWarning(hDlg, _TRA("Custom DPI must be 0 (automatic) or between 72 and 600."),
                                          _TRA("Invalid value"));
                        HwndSetFocus(GetDlgItem(hDlg, IDC_CUSTOM_DPI));
                        return TRUE;
                    }
                    if (int badId = ReadAloudSettingsPageInvalidControl(hDlg)) {
                        ListBox_SetCurSel(GetDlgItem(hDlg, IDC_SETTINGS_CATEGORY), kSettingsPageReadAloud);
                        ShowSettingsPage(hDlg, kSettingsPageReadAloud);
                        MessageBoxWarning(hDlg, _TRA("Enter a speed from 0.25x to 2.00x."), _TRA("Invalid value"));
                        HwndSetFocus(GetDlgItem(hDlg, badId));
                        return TRUE;
                    }
                    ReadAloudSettingsPageApply(hDlg);
                    prefs->defaultDisplayModeEnum =
                        (DisplayMode)(SendDlgItemMessage(hDlg, IDC_DEFAULT_LAYOUT, CB_GETCURSEL, 0, 0) +
                                      (int)DisplayMode::Automatic);
                    prefs->defaultZoomFloat = GetZoomComboBoxValue(hDlg, IDC_DEFAULT_ZOOM, prefs->defaultZoomFloat);

                    prefs->showToc = (BST_CHECKED == IsDlgButtonChecked(hDlg, IDC_DEFAULT_SHOW_TOC));
                    prefs->rememberStatePerDocument =
                        (BST_CHECKED == IsDlgButtonChecked(hDlg, IDC_REMEMBER_STATE_PER_DOCUMENT));
                    prefs->useTabs = (BST_CHECKED == IsDlgButtonChecked(hDlg, IDC_USE_TABS));
                    prefs->checkForUpdates = (BST_CHECKED == IsDlgButtonChecked(hDlg, IDC_CHECK_FOR_UPDATES));
                    prefs->rememberOpenedFiles = (BST_CHECKED == IsDlgButtonChecked(hDlg, IDC_REMEMBER_OPENED_FILES));
                    prefs->restoreSession = (BST_CHECKED == IsDlgButtonChecked(hDlg, IDC_RESTORE_SESSION));
                    prefs->lazyLoading = (BST_CHECKED == IsDlgButtonChecked(hDlg, IDC_LAZY_LOADING));
                    prefs->reuseInstance = (BST_CHECKED == IsDlgButtonChecked(hDlg, IDC_REUSE_INSTANCE));
                    prefs->noHomeTab = (BST_CHECKED != IsDlgButtonChecked(hDlg, IDC_NO_HOME_TAB));
                    prefs->showMenubarWithTabs = (BST_CHECKED == IsDlgButtonChecked(hDlg, IDC_SHOW_MENUBAR_WITH_TABS));
                    prefs->showToolbar = (BST_CHECKED == IsDlgButtonChecked(hDlg, IDC_SHOW_TOOLBAR));
                    prefs->showAnnotToolbarButtons =
                        (BST_CHECKED == IsDlgButtonChecked(hDlg, IDC_SHOW_ANNOT_TOOLBAR_BUTTONS));
                    prefs->tabsMru = (BST_CHECKED == IsDlgButtonChecked(hDlg, IDC_TABS_MRU));
                    prefs->searchUIFloating = (BST_CHECKED == IsDlgButtonChecked(hDlg, IDC_SEARCH_UI_FLOATING));
                    prefs->smoothScroll = (BST_CHECKED == IsDlgButtonChecked(hDlg, IDC_SMOOTH_SCROLL));
                    prefs->scrollbarInSinglePage = (BST_CHECKED == IsDlgButtonChecked(hDlg, IDC_SCROLLBAR_SINGLE_PAGE));
                    prefs->reloadModifiedDocuments = (BST_CHECKED == IsDlgButtonChecked(hDlg, IDC_RELOAD_MODIFIED));
                    prefs->preventSleepInFullscreen =
                        (BST_CHECKED == IsDlgButtonChecked(hDlg, IDC_PREVENT_SLEEP_FULLSCREEN));
                    prefs->treeWrapLabels = (BST_CHECKED == IsDlgButtonChecked(hDlg, IDC_TREE_WRAP_LABELS));
                    prefs->fastScrollOverScrollbar =
                        (BST_CHECKED == IsDlgButtonChecked(hDlg, IDC_FAST_SCROLL_OVER_SCROLLBAR));
                    prefs->disableAntiAlias = (BST_CHECKED != IsDlgButtonChecked(hDlg, IDC_ENABLE_ANTIALIAS));
                    prefs->enableDoubleClickWordLookup =
                        (BST_CHECKED == IsDlgButtonChecked(hDlg, IDC_ENABLE_WORD_LOOKUP));
                    prefs->autoOcrScanPages = (BST_CHECKED == IsDlgButtonChecked(hDlg, IDC_AUTO_OCR));
                    prefs->ocrAutoSave = (BST_CHECKED == IsDlgButtonChecked(hDlg, IDC_OCR_AUTO_SAVE));
                    prefs->enableAskAI = (BST_CHECKED == IsDlgButtonChecked(hDlg, IDC_ENABLE_ASK_AI));
                    prefs->enableInlineTranslate =
                        (BST_CHECKED == IsDlgButtonChecked(hDlg, IDC_ENABLE_INLINE_TRANSLATE));
                    {
                        char* ak = HwndGetTextTemp(GetDlgItem(hDlg, IDC_TRANSLATE_VOLC_AK));
                        char* sk = HwndGetTextTemp(GetDlgItem(hDlg, IDC_TRANSLATE_VOLC_SK));
                        str::ReplaceWithCopy(&prefs->translateVolcAccessKey, ak);
                        str::ReplaceWithCopy(&prefs->translateVolcSecretKey, sk);
                        int targetIdx = (int)SendDlgItemMessage(hDlg, IDC_TRANSLATE_TARGET, CB_GETCURSEL, 0, 0);
                        int nTargets = 0;
                        const TranslateTargetOption* targets = GetTranslateTargetOptions(&nTargets);
                        if (targetIdx < 0 || targetIdx >= nTargets) {
                            targetIdx = 0;
                        }
                        str::ReplaceWithCopy(&prefs->translateTargetMode, targets[targetIdx].mode);
                    }
                    prefs->escToExit = (BST_CHECKED == IsDlgButtonChecked(hDlg, IDC_ESC_TO_EXIT));
                    prefs->fullPathInTitle = (BST_CHECKED == IsDlgButtonChecked(hDlg, IDC_FULL_PATH_IN_TITLE));
                    prefs->showLinks = (BST_CHECKED == IsDlgButtonChecked(hDlg, IDC_SHOW_LINKS));
                    prefs->treeFontSize = treeFontSize;
                    prefs->tabFontSize = tabFontSize;
                    prefs->tabBarHeight = tabBarHeight;
                    prefs->customScreenDPI = customDpi;

                    int themeIdx = (int)SendDlgItemMessage(hDlg, IDC_THEME, CB_GETCURSEL, 0, 0);
                    const char* themeName = GetThemeName(themeIdx);
                    if (themeName) {
                        str::ReplaceWithCopy(&prefs->theme, themeName);
                    }
                    int documentColorIdx = (int)SendDlgItemMessage(hDlg, IDC_DOCUMENT_COLOR, CB_GETCURSEL, 0, 0);
                    str::ReplaceWithCopy(&prefs->documentColorMode, documentColorIdx == 0 ? "original" : "theme");
                    int engineeringIdx = (int)SendDlgItemMessage(hDlg, IDC_ENGINEERING_ENHANCE, CB_GETCURSEL, 0, 0);
                    const char* engineeringMode = engineeringIdx == 0 ? "off" : engineeringIdx == 2 ? "on" : "auto";
                    str::ReplaceWithCopy(&prefs->engineeringDrawingEnhance, engineeringMode);
                    int tocModeIdx = (int)SendDlgItemMessage(hDlg, IDC_TOC_MODE, CB_GETCURSEL, 0, 0);
                    const char* tocMode = tocModeIdx == 0 ? "conservative" : tocModeIdx == 2 ? "detailed" : "standard";
                    str::ReplaceWithCopy(&prefs->extractPdfTocMode, tocMode);
                    int providerIdx = (int)SendDlgItemMessage(hDlg, IDC_AI_PROVIDER, CB_GETCURSEL, 0, 0);
                    const char* provider = providerIdx == 1 ? "deepseek" : providerIdx == 2 ? "chatgpt" : "doubao";
                    str::ReplaceWithCopy(&prefs->aiChatProvider, provider);
                    int treeFontIdx = (int)SendDlgItemMessage(hDlg, IDC_TREE_FONT_NAME, CB_GETCURSEL, 0, 0);
                    if (treeFontIdx <= 0) {
                        str::ReplaceWithCopy(&prefs->treeFontName, "automatic");
                    } else {
                        char* treeFontName = HwndGetTextTemp(GetDlgItem(hDlg, IDC_TREE_FONT_NAME));
                        str::ReplaceWithCopy(&prefs->treeFontName,
                                             str::IsEmptyOrWhiteSpace(treeFontName) ? "automatic" : treeFontName);
                    }
                    char* dictionaryPath = HwndGetTextTemp(GetDlgItem(hDlg, IDC_DICTIONARY_PATH));
                    str::ReplaceWithCopy(&prefs->offlineDictionaryPath, dictionaryPath);
                    char* defaultAuthor = HwndGetTextTemp(GetDlgItem(hDlg, IDC_DEFAULT_AUTHOR));
                    str::TrimWSInPlace(defaultAuthor, str::TrimOpt::Both);
                    str::ReplaceWithCopy(&prefs->annotations.defaultAuthor, defaultAuthor);
                    AiTocCommitProfiles(hDlg, prefs);
                    int scrollbarIdx = (int)SendDlgItemMessage(hDlg, IDC_SCROLLBARS, CB_GETCURSEL, 0, 0);
                    const char* scrollbarMode = seqstrings::IdxToStr(gScrollbarModeNames, scrollbarIdx);
                    if (scrollbarMode) {
                        str::ReplaceWithCopy(&prefs->scrollbars, scrollbarMode);
                    }
                    if (prefs->enableTeXEnhancements && CanAccessDisk()) {
                        char* tmp = HwndGetTextTemp(GetDlgItem(hDlg, IDC_CMDLINE));
                        char* cmdLine = str::Dup(tmp);
                        str::ReplacePtr(&prefs->inverseSearchCmdLine, cmdLine);
                    }
                    EndDialog(hDlg, IDOK);
                    return TRUE;
                }

                case IDCANCEL:
                    EndDialog(hDlg, IDCANCEL);
                    return TRUE;

                case IDC_REMEMBER_OPENED_FILES: {
                    UpdateSettingsDependencies(hDlg);
                }
                    return TRUE;

                case IDC_USE_TABS:
                case IDC_SHOW_TOOLBAR:
                case IDC_RESTORE_SESSION:
                case IDC_ENABLE_WORD_LOOKUP:
                case IDC_ENABLE_ASK_AI:
                case IDC_ENABLE_INLINE_TRANSLATE:
                    UpdateSettingsDependencies(hDlg);
                    return TRUE;

                case IDC_DICTIONARY_BROWSE:
                    BrowseForDictionaryFolder(hDlg);
                    return TRUE;

                case IDC_AITOC_API_PROFILE:
                    if (HIWORD(wp) == CBN_SELCHANGE) {
                        auto* state = GetAiTocSettingsState(hDlg);
                        int next = ComboBox_GetCurSel(GetDlgItem(hDlg, IDC_AITOC_API_PROFILE));
                        if (state && next >= 0 && next < state->profiles.Size() && next != state->current) {
                            AiTocSaveVisibleProfile(hDlg);
                            state->current = next;
                            AiTocShowProfile(hDlg);
                        }
                    }
                    return TRUE;

                case IDC_AITOC_API_ADD: {
                    auto* state = GetAiTocSettingsState(hDlg);
                    if (!state) {
                        return TRUE;
                    }
                    AiTocSaveVisibleProfile(hDlg);
                    AiTocUiProfile p;
                    p.name = str::Format("Platform %d", state->profiles.Size() + 1);
                    state->profiles.Append(p);
                    state->current = state->profiles.Size() - 1;
                    AiTocShowProfile(hDlg);
                    SetFocus(GetDlgItem(hDlg, IDC_AITOC_API_NAME));
                    return TRUE;
                }

                case IDC_AITOC_API_REMOVE: {
                    auto* state = GetAiTocSettingsState(hDlg);
                    if (!state || state->profiles.Size() <= 1) {
                        return TRUE;
                    }
                    state->profiles[state->current].Free();
                    state->profiles.RemoveAt(state->current);
                    state->current = std::min(state->current, state->profiles.Size() - 1);
                    AiTocShowProfile(hDlg);
                    return TRUE;
                }

                case IDC_AITOC_API_NAME:
                    if (HIWORD(wp) == EN_KILLFOCUS) {
                        AiTocSaveVisibleProfile(hDlg);
                        AiTocRefreshProfileSelector(hDlg);
                    }
                    return TRUE;

                case IDC_AITOC_API_BASEURL:
                case IDC_AITOC_API_KEY:
                    if (HIWORD(wp) == EN_CHANGE) {
                        auto* state = GetAiTocSettingsState(hDlg);
                        if (state && !state->loading) {
                            state->fetchGeneration++;
                            AiTocClearModels(state);
                            EnableWindow(GetDlgItem(hDlg, IDC_AITOC_API_FETCH_MODELS), TRUE);
                        }
                    }
                    return TRUE;

                case IDC_AITOC_API_FETCH_MODELS: {
                    auto* state = GetAiTocSettingsState(hDlg);
                    if (!state) {
                        return TRUE;
                    }
                    auto* req = new AiTocFetchModelsReq();
                    req->hwnd = hDlg;
                    req->token = state->token;
                    req->generation = ++state->fetchGeneration;
                    req->baseUrl = str::Dup(HwndGetTextTemp(GetDlgItem(hDlg, IDC_AITOC_API_BASEURL)));
                    req->key = str::Dup(HwndGetTextTemp(GetDlgItem(hDlg, IDC_AITOC_API_KEY)));
                    EnableWindow(GetDlgItem(hDlg, IDC_AITOC_API_FETCH_MODELS), FALSE);
                    RunAsync(MkFunc0<AiTocFetchModelsReq>(AiTocFetchModelsWorker, req), "AiTocFetchModels");
                    return TRUE;
                }

                case IDC_TRANSLATE_VOLC_TEST: {
                    char* ak = HwndGetTextTemp(GetDlgItem(hDlg, IDC_TRANSLATE_VOLC_AK));
                    char* sk = HwndGetTextTemp(GetDlgItem(hDlg, IDC_TRANSLATE_VOLC_SK));
                    str::TrimWSInPlace(ak, str::TrimOpt::Both);
                    str::TrimWSInPlace(sk, str::TrimOpt::Both);
                    if (str::IsEmpty(ak) || str::IsEmpty(sk)) {
                        MessageBoxW(hDlg, ToWStrTemp(_TRA("Enter both the Access Key and Secret Key first.")),
                                    ToWStrTemp(_TRA("Volcengine Translate")), MB_ICONWARNING);
                        HwndSetFocus(
                            GetDlgItem(hDlg, str::IsEmpty(ak) ? IDC_TRANSLATE_VOLC_AK : IDC_TRANSLATE_VOLC_SK));
                        return TRUE;
                    }
                    auto* req = new VolcTranslateTestReq();
                    req->hwnd = hDlg;
                    auto* state = GetAiTocSettingsState(hDlg);
                    req->token = state ? state->token : 0;
                    req->ak = str::Dup(ak);
                    req->sk = str::Dup(sk);
                    EnableWindow(GetDlgItem(hDlg, IDC_TRANSLATE_VOLC_TEST), FALSE);
                    RunAsync(MkFunc0<VolcTranslateTestReq>(VolcTranslateTestWorker, req), "VolcTranslateTest");
                    return TRUE;
                }

                case IDC_AITOC_API_TEST: {
                    auto* req = new AiTocApiTestReq();
                    req->hwnd = hDlg;
                    auto* state = GetAiTocSettingsState(hDlg);
                    req->token = state ? state->token : 0;
                    req->baseUrl = str::Dup(HwndGetTextTemp(GetDlgItem(hDlg, IDC_AITOC_API_BASEURL)));
                    req->key = str::Dup(HwndGetTextTemp(GetDlgItem(hDlg, IDC_AITOC_API_KEY)));
                    req->model = str::Dup(HwndGetTextTemp(GetDlgItem(hDlg, IDC_AITOC_API_MODEL)));
                    RunAsync(MkFunc0<AiTocApiTestReq>(AiTocApiTestWorker, req), "AiTocApiTest");
                    return TRUE;
                }

                case IDC_SETTINGS_CATEGORY:
                    if (HIWORD(wp) == LBN_SELCHANGE) {
                        int page = ListBox_GetCurSel(GetDlgItem(hDlg, IDC_SETTINGS_CATEGORY));
                        ShowSettingsPage(hDlg, page);
                    }
                    return TRUE;

                case IDC_OPEN_ADVANCED_OPTIONS:
                    // Close first: saving the file can reload and replace gGlobalPrefs,
                    // invalidating the pointer held by this modal dialog.
                    EndDialog(hDlg, IDC_OPEN_ADVANCED_OPTIONS);
                    return TRUE;

                case IDC_DEFAULT_SHOW_TOC:
                case IDC_REMEMBER_STATE_PER_DOCUMENT:
                case IDC_CHECK_FOR_UPDATES:
                    return TRUE;

                case IDC_RA_VOICE_MODE:
                case IDC_RA_VOICE_ZH:
                case IDC_RA_VOICE_EN:
                case IDC_RA_PREVIEW:
                case IDC_RA_HIGHLIGHT_COLOR:
                case IDC_RA_HIGHLIGHT_RESET:
                    ReadAloudSettingsPageOnCommand(hDlg, LOWORD(wp), HIWORD(wp));
                    return TRUE;
            }
            break;
    }
    return FALSE;
}

INT_PTR Dialog_Settings(HWND hwnd, GlobalPrefs* prefs, int initialPage) {
    gSettingsInitialPage = initialPage;
    INT_PTR res = CreateAppDialogBox(IDD_DIALOG_SETTINGS, hwnd, Dialog_Settings_Proc, (LPARAM)prefs);
    gSettingsInitialPage = 0;
    return res;
}

#ifndef ID_APPLY_NOW
#define ID_APPLY_NOW 0x3021
#endif

static INT_PTR CALLBACK Sheet_Print_Advanced_Proc(HWND hDlg, UINT msg, WPARAM wp, LPARAM lp) {
    Print_Advanced_Data* data;

    switch (msg) {
        //[ ACCESSKEY_GROUP Advanced Print Tab
        case WM_INITDIALOG:
            data = (Print_Advanced_Data*)((PROPSHEETPAGE*)lp)->lParam;
            SetWindowLongPtr(hDlg, GWLP_USERDATA, (LONG_PTR)data);
            if (UseDarkModeLib()) {
                DarkMode::setDarkWndSafe(hDlg);
            }
            UpdateWindowCaptionTheme(hDlg);
            HwndSetDlgItemText(hDlg, IDC_SECTION_PRINT_RANGE, _TRA("Print range"));
            HwndSetDlgItemText(hDlg, IDC_PRINT_RANGE_ALL, _TRA("&All selected pages"));
            HwndSetDlgItemText(hDlg, IDC_PRINT_RANGE_EVEN, _TRA("&Even pages only"));
            HwndSetDlgItemText(hDlg, IDC_PRINT_RANGE_ODD, _TRA("&Odd pages only"));
            HwndSetDlgItemText(hDlg, IDC_SECTION_PRINT_SCALE, _TRA("Page scaling"));
            HwndSetDlgItemText(hDlg, IDC_PRINT_SCALE_SHRINK, _TRA("&Shrink pages to printable area (if necessary)"));
            HwndSetDlgItemText(hDlg, IDC_PRINT_SCALE_FIT, _TRA("&Fit pages to printable area"));
            HwndSetDlgItemText(hDlg, IDC_PRINT_SCALE_NONE, _TRA("&Use original page sizes"));
            HwndSetDlgItemText(hDlg, IDC_SECTION_PRINT_COMPATIBILITY, _TRA("Compatibility"));

            CheckRadioButton(hDlg, IDC_PRINT_RANGE_ALL, IDC_PRINT_RANGE_ODD,
                             data->range == PrintRangeAdv::Even  ? IDC_PRINT_RANGE_EVEN
                             : data->range == PrintRangeAdv::Odd ? IDC_PRINT_RANGE_ODD
                                                                 : IDC_PRINT_RANGE_ALL);
            CheckRadioButton(hDlg, IDC_PRINT_SCALE_SHRINK, IDC_PRINT_SCALE_NONE,
                             data->scale == PrintScaleAdv::Fit      ? IDC_PRINT_SCALE_FIT
                             : data->scale == PrintScaleAdv::Shrink ? IDC_PRINT_SCALE_SHRINK
                                                                    : IDC_PRINT_SCALE_NONE);

            return FALSE;
            //] ACCESSKEY_GROUP Advanced Print Tab

        case WM_NOTIFY:
            if (((LPNMHDR)lp)->code == PSN_APPLY) {
                data = (Print_Advanced_Data*)GetWindowLongPtr(hDlg, GWLP_USERDATA);
                if (IsDlgButtonChecked(hDlg, IDC_PRINT_RANGE_EVEN)) {
                    data->range = PrintRangeAdv::Even;
                } else if (IsDlgButtonChecked(hDlg, IDC_PRINT_RANGE_ODD)) {
                    data->range = PrintRangeAdv::Odd;
                } else {
                    data->range = PrintRangeAdv::All;
                }
                if (IsDlgButtonChecked(hDlg, IDC_PRINT_SCALE_FIT)) {
                    data->scale = PrintScaleAdv::Fit;
                } else if (IsDlgButtonChecked(hDlg, IDC_PRINT_SCALE_SHRINK)) {
                    data->scale = PrintScaleAdv::Shrink;
                } else {
                    data->scale = PrintScaleAdv::None;
                }
                return TRUE;
            }
            break;

        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDC_PRINT_RANGE_ALL:
                case IDC_PRINT_RANGE_EVEN:
                case IDC_PRINT_RANGE_ODD:
                case IDC_PRINT_SCALE_SHRINK:
                case IDC_PRINT_SCALE_FIT:
                case IDC_PRINT_SCALE_NONE: {
                    HWND hApplyButton = GetDlgItem(GetParent(hDlg), ID_APPLY_NOW);
                    EnableWindow(hApplyButton, TRUE);
                } break;
            }
    }
    return FALSE;
}

HPROPSHEETPAGE CreatePrintAdvancedPropSheet(Print_Advanced_Data* data, ScopedMem<DLGTEMPLATE>& dlgTemplate) {
    PROPSHEETPAGE psp{};

    psp.dwSize = sizeof(PROPSHEETPAGE);
    psp.dwFlags = PSP_USETITLE | PSP_PREMATURE;
    psp.pszTemplate = MAKEINTRESOURCE(IDD_PROPSHEET_PRINT_ADVANCED);
    psp.pfnDlgProc = Sheet_Print_Advanced_Proc;
    psp.lParam = (LPARAM)data;
    auto s = _TRA("Advanced");
    psp.pszTitle = ToWStrTemp(s);

    if (IsUIRtl()) {
        dlgTemplate.Set(GetRtLDlgTemplate(IDD_PROPSHEET_PRINT_ADVANCED));
        psp.pResource = dlgTemplate.Get();
        psp.dwFlags |= PSP_DLGINDIRECT;
    }

    return CreatePropertySheetPage(&psp);
}

struct Dialog_AddFav_Data {
    char* pageNo = nullptr;
    char* favName = nullptr;
    const char* dialogTitle = nullptr;
    const char* prompt = nullptr;
    bool showSetCurrentView = false;
    bool setCurrentView = false;
    ~Dialog_AddFav_Data() {
        str::Free(pageNo);
        str::Free(favName);
    }
};

static INT_PTR CALLBACK Dialog_AddFav_Proc(HWND hDlg, UINT msg, WPARAM wp, LPARAM lp) {
    if (WM_INITDIALOG == msg) {
        Dialog_AddFav_Data* data = (Dialog_AddFav_Data*)lp;
        SetWindowLongPtr(hDlg, GWLP_USERDATA, (LONG_PTR)data);
        if (UseDarkModeLib()) {
            DarkMode::setDarkWndSafe(hDlg);
        }
        UpdateWindowCaptionTheme(hDlg);
        HwndSetText(hDlg, data->dialogTitle ? data->dialogTitle : _TRA("Add Favorite"));
        TempStr s;
        const char* prompt = data->prompt;
        if (!prompt) {
            s = str::FormatTemp(_TRA("Add page %s to favorites with (optional) name:"), data->pageNo);
            prompt = s;
        }
        HwndSetDlgItemText(hDlg, IDC_ADD_PAGE_STATIC, prompt);
        HwndSetDlgItemText(hDlg, IDOK, _TRA("OK"));
        HwndSetDlgItemText(hDlg, IDCANCEL, _TRA("Cancel"));
        AppDialogUseStandardControls(hDlg);
        AppDialogApplyChrome(hDlg);
        // The application font is taller than the resource's 8-DLU label.
        // Measure it after applying the shared font and keep the edit below it.
        HWND label = GetDlgItem(hDlg, IDC_ADD_PAGE_STATIC);
        HWND edit = GetDlgItem(hDlg, IDC_FAV_NAME_EDIT);
        HDC dc = GetDC(label);
        HFONT font = (HFONT)SendMessageW(label, WM_GETFONT, 0, 0);
        HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
        TEXTMETRICW metrics{};
        GetTextMetricsW(dc, &metrics);
        if (oldFont) SelectObject(dc, oldFont);
        ReleaseDC(label, dc);
        RECT labelRect{}, editRect{};
        GetWindowRect(label, &labelRect);
        GetWindowRect(edit, &editRect);
        MapWindowPoints(nullptr, hDlg, (POINT*)&labelRect, 2);
        MapWindowPoints(nullptr, hDlg, (POINT*)&editRect, 2);
        int labelHeight = std::max((int)(labelRect.bottom - labelRect.top),
                                   (int)(metrics.tmHeight + metrics.tmExternalLeading) + DpiScale(hDlg, 2));
        MoveWindow(label, labelRect.left, labelRect.top, labelRect.right - labelRect.left, labelHeight, TRUE);
        int editTop = std::max((int)editRect.top, (int)labelRect.top + labelHeight + DpiScale(hDlg, 6));
        MoveWindow(edit, editRect.left, editTop, editRect.right - editRect.left, editRect.bottom - editRect.top, TRUE);
        // Use the same native action metrics as the annotation inspector.
        // Keep the existing dialog width and right-aligned OK/Cancel order.
        HWND ok = GetDlgItem(hDlg, IDOK);
        HWND cancel = GetDlgItem(hDlg, IDCANCEL);
        Size okSize = ButtonGetIdealSize(ok);
        Size cancelSize = ButtonGetIdealSize(cancel);
        int buttonWidth = std::max(okSize.dx, cancelSize.dx);
        int buttonHeight = std::max(okSize.dy, cancelSize.dy);
        RECT client{};
        GetClientRect(hDlg, &client);
        int pad = DpiScale(hDlg, 12), gap = DpiScale(hDlg, 8);
        int y = std::max(0, (int)client.bottom - pad - buttonHeight);
        MoveWindow(cancel, client.right - pad - buttonWidth, y, buttonWidth, buttonHeight, TRUE);
        MoveWindow(ok, client.right - pad - buttonWidth * 2 - gap, y, buttonWidth, buttonHeight, TRUE);
        HWND setCurrent = GetDlgItem(hDlg, IDC_PDF_TOC_SET_CURRENT);
        if (data->showSetCurrentView) {
            HwndSetText(setCurrent, _TRA("Set target to current view"));
            Button_SetCheck(setCurrent, data->setCurrentView ? BST_CHECKED : BST_UNCHECKED);
        } else {
            ShowWindow(setCurrent, SW_HIDE);
        }
        if (data->favName) {
            HwndSetDlgItemText(hDlg, IDC_FAV_NAME_EDIT, data->favName);
            EditSelectAll(GetDlgItem(hDlg, IDC_FAV_NAME_EDIT));
        }
        CenterDialog(hDlg);
        HwndSetFocus(GetDlgItem(hDlg, IDC_FAV_NAME_EDIT));
        return FALSE;
    }

    if (WM_COMMAND == msg) {
        Dialog_AddFav_Data* data = (Dialog_AddFav_Data*)GetWindowLongPtr(hDlg, GWLP_USERDATA);
        WORD cmd = LOWORD(wp);
        if (IDOK == cmd) {
            char* name = HwndGetTextTemp(GetDlgItem(hDlg, IDC_FAV_NAME_EDIT));
            str::TrimWSInPlace(name, str::TrimOpt::Both);
            if (!str::IsEmpty(name)) {
                str::ReplaceWithCopy(&data->favName, name);
            } else {
                str::FreePtr(&data->favName);
            }
            if (data->showSetCurrentView) {
                data->setCurrentView = Button_GetCheck(GetDlgItem(hDlg, IDC_PDF_TOC_SET_CURRENT)) == BST_CHECKED;
            }
            EndDialog(hDlg, IDOK);
            return TRUE;
        } else if (IDCANCEL == cmd) {
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }
    }

    return FALSE;
}

// pageNo is the page we're adding to favorites
// returns true if the user wants to add a favorite.
// favName is the name the user wants the favorite to have
// (passing in a non-nullptr favName will use it as default name)
// --- Change Background Color dialog ---

static const int kMaxCustomColors = 13;

struct BgColorDlgData {
    COLORREF currentColor; // current selected color
    bool isCheckered;      // true if "checkered" is selected
    bool applyToAll;       // radio: all files like this
    COLORREF customColors[kMaxCustomColors];
    bool customColorSet[kMaxCustomColors]; // true if slot has a color
    bool customColorsChanged;
    int selectedCustomIdx;     // -1 = no custom button selected
    bool previewSelected;      // true if preview button is selected
    const char* title;         // dialog title (nullptr = default)
    bool showRadioButtons;     // show "this file" / "all files" radio buttons
    const char* allFilesLabel; // label for "all files" radio button (nullptr = default)
};

// fixed preset colors: checkered, black, white
static const COLORREF kBgPresetColors[] = {
    kColorUnset,        // checkered
    RGB(0, 0, 0),       // black
    RGB(255, 255, 255), // white
};
static const int kNumPresets = 3;

static void ParseCustomColors(BgColorDlgData* data) {
    for (int i = 0; i < kMaxCustomColors; i++) {
        data->customColorSet[i] = false;
        data->customColors[i] = 0;
    }
    data->customColorsChanged = false;
    char* s = gGlobalPrefs->customColors;
    if (!s || !*s) {
        return;
    }
    int idx = 0;
    while (*s && idx < kMaxCustomColors) {
        while (*s == ' ') {
            s++;
        }
        if (!*s) {
            break;
        }
        ParsedColor parsed;
        ParseColor(parsed, s);
        if (parsed.parsedOk) {
            data->customColors[idx] = parsed.col;
            data->customColorSet[idx] = true;
            idx++;
        }
        // skip to next space or end
        while (*s && *s != ' ') {
            s++;
        }
    }
}

static void SaveCustomColors(BgColorDlgData* data) {
    StrBuilder buf;
    for (int i = 0; i < kMaxCustomColors; i++) {
        if (!data->customColorSet[i]) {
            continue;
        }
        if (buf.Size() > 0) {
            buf.AppendChar(' ');
        }
        TempStr cs = SerializeColorTemp(data->customColors[i]);
        buf.Append(cs);
    }
    str::ReplaceWithCopy(&gGlobalPrefs->customColors, buf.LendData());
    SaveSettings();
}

static void HsvToRgb(float h, float s, float v, u8& r, u8& g, u8& b) {
    float c = v * s;
    float x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
    float m = v - c;
    float rf, gf, bf;
    if (h < 60) {
        rf = c;
        gf = x;
        bf = 0;
    } else if (h < 120) {
        rf = x;
        gf = c;
        bf = 0;
    } else if (h < 180) {
        rf = 0;
        gf = c;
        bf = x;
    } else if (h < 240) {
        rf = 0;
        gf = x;
        bf = c;
    } else if (h < 300) {
        rf = x;
        gf = 0;
        bf = c;
    } else {
        rf = c;
        gf = 0;
        bf = x;
    }
    r = (u8)((rf + m) * 255.0f);
    g = (u8)((gf + m) * 255.0f);
    b = (u8)((bf + m) * 255.0f);
}

static void PaintColorArea(HDC hdc, RECT* rc) {
    int w = rc->right - rc->left;
    int h = rc->bottom - rc->top;
    if (w <= 0 || h <= 0) {
        return;
    }
    // rows must be DWORD-aligned; each pixel is 3 bytes (BGR)
    int stride = (w * 3 + 3) & ~3;
    u8* bits = (u8*)malloc(stride * h);
    if (!bits) {
        return;
    }
    for (int y = 0; y < h; y++) {
        float val = 1.0f - (float)y / (float)h;
        u8* row = bits + y * stride;
        for (int x = 0; x < w; x++) {
            float hue = (float)x / (float)w * 360.0f;
            u8 r, g, b;
            HsvToRgb(hue, 1.0f, val, r, g, b);
            row[x * 3] = b;
            row[x * 3 + 1] = g;
            row[x * 3 + 2] = r;
        }
    }
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 24;
    bmi.bmiHeader.biCompression = BI_RGB;
    SetDIBitsToDevice(hdc, rc->left, rc->top, w, h, 0, 0, 0, h, bits, &bmi, DIB_RGB_COLORS);
    free(bits);
}

static void SelectPreviewButton(HWND hDlg, BgColorDlgData* data) {
    int prevCustom = data->selectedCustomIdx;
    bool wasPreview = data->previewSelected;
    data->selectedCustomIdx = -1;
    data->previewSelected = true;
    if (prevCustom >= 0) {
        InvalidateRect(GetDlgItem(hDlg, IDC_BGCOL_CUSTOM_FIRST + prevCustom), nullptr, TRUE);
    }
    if (!wasPreview) {
        InvalidateRect(GetDlgItem(hDlg, IDC_BGCOL_PREVIEW), nullptr, TRUE);
    }
}

static void SelectCustomButton(HWND hDlg, BgColorDlgData* data, int idx) {
    int prevCustom = data->selectedCustomIdx;
    bool wasPreview = data->previewSelected;
    data->selectedCustomIdx = idx;
    data->previewSelected = false;
    if (prevCustom >= 0 && prevCustom != idx) {
        InvalidateRect(GetDlgItem(hDlg, IDC_BGCOL_CUSTOM_FIRST + prevCustom), nullptr, TRUE);
    }
    if (wasPreview) {
        InvalidateRect(GetDlgItem(hDlg, IDC_BGCOL_PREVIEW), nullptr, TRUE);
    }
    InvalidateRect(GetDlgItem(hDlg, IDC_BGCOL_CUSTOM_FIRST + idx), nullptr, TRUE);
}

static void InvalidatePreview(HWND hDlg, BgColorDlgData* data) {
    if (data->selectedCustomIdx >= 0) {
        InvalidateRect(GetDlgItem(hDlg, IDC_BGCOL_CUSTOM_FIRST + data->selectedCustomIdx), nullptr, TRUE);
    }
    if (data->previewSelected) {
        InvalidateRect(GetDlgItem(hDlg, IDC_BGCOL_PREVIEW), nullptr, TRUE);
    }
}

static void UpdateBgColorEditFromColor(HWND hDlg, BgColorDlgData* data) {
    if (data->isCheckered) {
        HwndSetDlgItemText(hDlg, IDC_BGCOL_EDIT, data->showRadioButtons ? "checkered" : "unset");
    } else {
        TempStr s = SerializeColorTemp(data->currentColor);
        HwndSetDlgItemText(hDlg, IDC_BGCOL_EDIT, s);
    }
    // update selected custom button color and refresh preview
    if (data->selectedCustomIdx >= 0 && !data->isCheckered) {
        data->customColors[data->selectedCustomIdx] = data->currentColor;
        data->customColorSet[data->selectedCustomIdx] = true;
        data->customColorsChanged = true;
    }
    InvalidatePreview(hDlg, data);
}

static bool TryParseBgColorEdit(HWND hDlg, BgColorDlgData* data) {
    TempStr text = HwndGetTextTemp(GetDlgItem(hDlg, IDC_BGCOL_EDIT));
    if (!text || !*text) {
        return false;
    }
    ParsedColor parsed;
    ParseColor(parsed, text);
    if (!parsed.parsedOk) {
        return false;
    }
    if (parsed.col == kColorUnset) {
        data->isCheckered = true;
    } else {
        data->isCheckered = false;
        data->currentColor = parsed.col;
    }
    return true;
}

static void PickColorFromArea(HWND hwndCA, BgColorDlgData* data, HWND hDlg) {
    POINT pt;
    GetCursorPos(&pt);
    ScreenToClient(hwndCA, &pt);
    HDC hdcCA = GetDC(hwndCA);
    COLORREF picked = GetPixel(hdcCA, pt.x, pt.y);
    ReleaseDC(hwndCA, hdcCA);
    if (picked != CLR_INVALID) {
        data->isCheckered = false;
        data->currentColor = picked;
        UpdateBgColorEditFromColor(hDlg, data);
    }
}

static WNDPROC gOrigColorAreaProc = nullptr;

static LRESULT CALLBACK ColorAreaSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    HWND hDlg = GetParent(hwnd);
    BgColorDlgData* data = (BgColorDlgData*)GetWindowLongPtrW(hDlg, GWLP_USERDATA);

    switch (msg) {
        case WM_LBUTTONDOWN:
            SetCapture(hwnd);
            PickColorFromArea(hwnd, data, hDlg);
            return 0;
        case WM_MOUSEMOVE:
            if (wp & MK_LBUTTON) {
                PickColorFromArea(hwnd, data, hDlg);
            }
            return 0;
        case WM_LBUTTONUP:
            ReleaseCapture();
            return 0;
    }
    return CallWindowProcW(gOrigColorAreaProc, hwnd, msg, wp, lp);
}

static INT_PTR CALLBACK Dialog_ChangeBgColor_Proc(HWND hDlg, UINT msg, WPARAM wp, LPARAM lp) {
    BgColorDlgData* data;
    if (msg == WM_INITDIALOG) {
        data = (BgColorDlgData*)lp;
        SetWindowLongPtrW(hDlg, GWLP_USERDATA, (LONG_PTR)data);
    } else {
        data = (BgColorDlgData*)GetWindowLongPtrW(hDlg, GWLP_USERDATA);
    }

    switch (msg) {
        case WM_INITDIALOG: {
            if (UseDarkModeLib()) {
                DarkMode::setDarkWndSafe(hDlg);
            }
            UpdateWindowCaptionTheme(hDlg);
            HwndSetText(hDlg, data->title ? data->title : _TRA("Change Background Color"));
            HwndSetDlgItemText(hDlg, IDOK, _TRA("OK"));
            HwndSetDlgItemText(hDlg, IDCANCEL, _TRA("Cancel"));
            if (data->showRadioButtons) {
                if (data->allFilesLabel) {
                    HwndSetDlgItemText(hDlg, IDC_BGCOL_ALL_FILES, data->allFilesLabel);
                }
                CheckRadioButton(hDlg, IDC_BGCOL_THIS_FILE, IDC_BGCOL_ALL_FILES,
                                 data->applyToAll ? IDC_BGCOL_ALL_FILES : IDC_BGCOL_THIS_FILE);
            } else {
                ShowWindow(GetDlgItem(hDlg, IDC_BGCOL_THIS_FILE), SW_HIDE);
                ShowWindow(GetDlgItem(hDlg, IDC_BGCOL_ALL_FILES), SW_HIDE);
            }
            ParseCustomColors(data);
            UpdateBgColorEditFromColor(hDlg, data);
            // subclass color area for mouse drag tracking
            HWND hwndCA = GetDlgItem(hDlg, IDC_BGCOL_COLORAREA);
            gOrigColorAreaProc = (WNDPROC)SetWindowLongPtrW(hwndCA, GWLP_WNDPROC, (LONG_PTR)ColorAreaSubclassProc);
            CenterDialog(hDlg);
            return TRUE;
        }

        case WM_DRAWITEM: {
            DRAWITEMSTRUCT* dis = (DRAWITEMSTRUCT*)lp;
            int ctlId = (int)dis->CtlID;
            if (ctlId == IDC_BGCOL_COLORAREA) {
                PaintColorArea(dis->hDC, &dis->rcItem);
                return TRUE;
            }
            // preview button shows the currently selected color
            if (ctlId == IDC_BGCOL_PREVIEW) {
                RECT rc = dis->rcItem;
                if (data->previewSelected) {
                    FillRect(dis->hDC, &rc, (HBRUSH)(COLOR_HIGHLIGHT + 1));
                    InflateRect(&rc, -3, -3);
                }
                if (data->isCheckered) {
                    PaintCheckerboard(dis->hDC, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top);
                } else {
                    HBRUSH br = CreateSolidBrush(data->currentColor);
                    FillRect(dis->hDC, &rc, br);
                    DeleteObject(br);
                }
                return TRUE;
            }
            // preset color buttons
            if (ctlId >= IDC_BGCOL_PRESET_FIRST && ctlId < IDC_BGCOL_PRESET_FIRST + kNumPresets) {
                int idx = ctlId - IDC_BGCOL_PRESET_FIRST;
                COLORREF col = kBgPresetColors[idx];
                if (col == kColorUnset) {
                    PaintCheckerboard(dis->hDC, dis->rcItem.left, dis->rcItem.top, dis->rcItem.right - dis->rcItem.left,
                                      dis->rcItem.bottom - dis->rcItem.top);
                } else {
                    HBRUSH br = CreateSolidBrush(col);
                    FillRect(dis->hDC, &dis->rcItem, br);
                    DeleteObject(br);
                }
                // draw focus rect if focused
                if (dis->itemState & ODS_FOCUS) {
                    DrawFocusRect(dis->hDC, &dis->rcItem);
                }
                return TRUE;
            }
            // custom color buttons
            if (ctlId >= IDC_BGCOL_CUSTOM_FIRST && ctlId < IDC_BGCOL_CUSTOM_FIRST + kMaxCustomColors) {
                int idx = ctlId - IDC_BGCOL_CUSTOM_FIRST;
                RECT rc = dis->rcItem;
                bool isSelected = (idx == data->selectedCustomIdx);
                if (isSelected) {
                    // draw selection outline: fill background, then inset for 2px gap
                    FillRect(dis->hDC, &rc, (HBRUSH)(COLOR_HIGHLIGHT + 1));
                    InflateRect(&rc, -3, -3);
                }
                if (data->customColorSet[idx]) {
                    HBRUSH br = CreateSolidBrush(data->customColors[idx]);
                    FillRect(dis->hDC, &rc, br);
                    DeleteObject(br);
                } else {
                    // empty slot: window background with accent border and diagonal X
                    FillRect(dis->hDC, &rc, (HBRUSH)(COLOR_WINDOW + 1));
                    HPEN pen = CreatePen(PS_SOLID, 1, GetSysColor(COLOR_BTNSHADOW));
                    HPEN oldPen = (HPEN)SelectObject(dis->hDC, pen);
                    // border
                    MoveToEx(dis->hDC, rc.left, rc.top, nullptr);
                    LineTo(dis->hDC, rc.right - 1, rc.top);
                    LineTo(dis->hDC, rc.right - 1, rc.bottom - 1);
                    LineTo(dis->hDC, rc.left, rc.bottom - 1);
                    LineTo(dis->hDC, rc.left, rc.top);
                    // diagonal lines
                    MoveToEx(dis->hDC, rc.left, rc.top, nullptr);
                    LineTo(dis->hDC, rc.right - 1, rc.bottom - 1);
                    MoveToEx(dis->hDC, rc.right - 1, rc.top, nullptr);
                    LineTo(dis->hDC, rc.left, rc.bottom - 1);
                    SelectObject(dis->hDC, oldPen);
                    DeleteObject(pen);
                }
                return TRUE;
            }
            break;
        }

        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDOK:
                    TryParseBgColorEdit(hDlg, data);
                    data->applyToAll = IsDlgButtonChecked(hDlg, IDC_BGCOL_ALL_FILES) == BST_CHECKED;
                    if (data->customColorsChanged) {
                        SaveCustomColors(data);
                    }
                    EndDialog(hDlg, IDOK);
                    return TRUE;
                case IDCANCEL:
                    if (data->customColorsChanged) {
                        SaveCustomColors(data);
                    }
                    EndDialog(hDlg, IDCANCEL);
                    return TRUE;
                case IDC_BGCOL_EDIT:
                    if (HIWORD(wp) == EN_CHANGE) {
                        if (TryParseBgColorEdit(hDlg, data)) {
                            // update selected button color
                            if (data->selectedCustomIdx >= 0 && !data->isCheckered) {
                                data->customColors[data->selectedCustomIdx] = data->currentColor;
                                data->customColorSet[data->selectedCustomIdx] = true;
                                data->customColorsChanged = true;
                            }
                            InvalidatePreview(hDlg, data);
                        }
                    }
                    break;
                case IDC_BGCOL_PREVIEW:
                    SelectPreviewButton(hDlg, data);
                    break;
                default: {
                    int id = LOWORD(wp);
                    // preset buttons: select preview button
                    if (id >= IDC_BGCOL_PRESET_FIRST && id < IDC_BGCOL_PRESET_FIRST + kNumPresets) {
                        int idx = id - IDC_BGCOL_PRESET_FIRST;
                        COLORREF col = kBgPresetColors[idx];
                        if (col == kColorUnset) {
                            data->isCheckered = true;
                        } else {
                            data->isCheckered = false;
                            data->currentColor = col;
                        }
                        SelectPreviewButton(hDlg, data);
                        UpdateBgColorEditFromColor(hDlg, data);
                    }
                    // custom color buttons: select this button
                    if (id >= IDC_BGCOL_CUSTOM_FIRST && id < IDC_BGCOL_CUSTOM_FIRST + kMaxCustomColors) {
                        int idx = id - IDC_BGCOL_CUSTOM_FIRST;
                        if (data->selectedCustomIdx == idx) {
                            // clicking selected button deselects it
                            SelectPreviewButton(hDlg, data);
                        } else {
                            SelectCustomButton(hDlg, data, idx);
                            // load the button's color as current selection
                            if (data->customColorSet[idx]) {
                                data->isCheckered = false;
                                data->currentColor = data->customColors[idx];
                                UpdateBgColorEditFromColor(hDlg, data);
                            }
                        }
                    }
                } break;
            }
            break;

        case WM_CONTEXTMENU: {
            HWND hwndClicked = (HWND)wp;
            int ctlId = GetDlgCtrlID(hwndClicked);
            if (ctlId >= IDC_BGCOL_CUSTOM_FIRST && ctlId < IDC_BGCOL_CUSTOM_FIRST + kMaxCustomColors) {
                int idx = ctlId - IDC_BGCOL_CUSTOM_FIRST;
                if (data->customColorSet[idx]) {
                    data->customColorSet[idx] = false;
                    data->customColorsChanged = true;
                    if (data->selectedCustomIdx == idx) {
                        SelectPreviewButton(hDlg, data);
                    }
                    InvalidateRect(hwndClicked, nullptr, TRUE);
                }
                return TRUE;
            }
            break;
        }
    }
    return FALSE;
}

bool Dialog_ChangeBackgroundColor(HWND hwnd, COLORREF currentColor, bool isCheckered, const char* allFilesLabel,
                                  BgColorResult& result) {
    BgColorDlgData data;
    data.currentColor = currentColor;
    data.isCheckered = isCheckered;
    data.applyToAll = false;
    data.selectedCustomIdx = -1;
    data.previewSelected = true;
    data.title = nullptr;
    data.showRadioButtons = true;
    data.allFilesLabel = allFilesLabel;

    INT_PTR res = CreateAppDialogBox(IDD_DIALOG_CHANGE_BG_COLOR, hwnd, Dialog_ChangeBgColor_Proc, (LPARAM)&data);
    if (res != IDOK) {
        return false;
    }

    result.color = data.currentColor;
    result.isCheckered = data.isCheckered;
    result.applyToAllFiles = data.applyToAll;
    return true;
}

bool Dialog_SetTabColor(HWND hwnd, COLORREF currentColor, bool isUnset, COLORREF& resultColor, bool& resultIsUnset) {
    BgColorDlgData data;
    data.currentColor = currentColor;
    data.isCheckered = isUnset;
    data.applyToAll = false;
    data.selectedCustomIdx = -1;
    data.previewSelected = true;
    data.title = _TRA("Set Tab Color");
    data.showRadioButtons = false;

    INT_PTR res = CreateAppDialogBox(IDD_DIALOG_CHANGE_BG_COLOR, hwnd, Dialog_ChangeBgColor_Proc, (LPARAM)&data);
    if (res != IDOK) {
        return false;
    }

    resultColor = data.currentColor;
    resultIsUnset = data.isCheckered;
    return true;
}

bool Dialog_AddFavorite(HWND hwnd, const char* pageNo, AutoFreeStr& favName) {
    Dialog_AddFav_Data data;
    data.pageNo = str::Dup(pageNo);
    data.favName = str::Dup(favName);

    INT_PTR res = CreateAppDialogBox(IDD_DIALOG_FAV_ADD, hwnd, Dialog_AddFav_Proc, (LPARAM)&data);
    if (IDCANCEL == res) {
        return false;
    }

    favName.SetCopy(data.favName);
    return true;
}

bool Dialog_PdfTocTitle(HWND hwnd, const char* dialogTitle, const char* prompt, AutoFreeStr& title,
                        bool* setTargetToCurrentView) {
    Dialog_AddFav_Data data;
    data.favName = str::Dup(title);
    data.dialogTitle = dialogTitle;
    data.prompt = prompt;
    data.showSetCurrentView = setTargetToCurrentView != nullptr;
    data.setCurrentView = setTargetToCurrentView && *setTargetToCurrentView;
    INT_PTR res = CreateAppDialogBox(IDD_DIALOG_FAV_ADD, hwnd, Dialog_AddFav_Proc, (LPARAM)&data);
    if (res != IDOK || str::IsEmpty(data.favName)) {
        return false;
    }
    title.SetCopy(data.favName);
    if (setTargetToCurrentView) {
        *setTargetToCurrentView = data.setCurrentView;
    }
    return true;
}
