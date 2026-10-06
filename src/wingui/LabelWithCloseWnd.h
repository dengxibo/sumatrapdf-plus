/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#include "SvgIcons.h"

struct Tooltip;

struct LabelViewButton {
    TbIcon icon = TbIcon::None;
    const char* tooltip = nullptr; // _TRN key
    bool selected = false;
    bool enabled = true;
    // False: same ink as the header chevrons, tooltip only, no hover well.
    bool chromeWell = true;
    Func0 onClick;
};

struct LabelWithCloseWnd : Wnd {
    struct CreateArgs {
        HWND parent = nullptr;
        HFONT font = nullptr;
        int cmdId = 0;
        bool isRtl = false;
    };

    LabelWithCloseWnd() = default;
    ~LabelWithCloseWnd() override;

    HWND Create(const CreateArgs&);

    void OnPaint(HDC hdc, PAINTSTRUCT* ps) override;
    LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) override;

    void SetLabel(const char*);
    void SetFont(HFONT);
    void SetPaddingXY(int x, int y);
    void SetHeaderActions(const Func0& firstAction, const char* firstTooltip, const Func0& secondAction,
                          const char* secondTooltip);
    void SetThirdHeaderAction(const Func0& action, const char* tooltip);
    void ClearThirdHeaderAction();
    void UpdateActionsTooltipTheme();
    void UpdateHeaderActionTooltips();
    void SetViewButtons(const LabelViewButton* buttons, int count);
    void SetRightButtons(const LabelViewButton* buttons, int count);
    void SetHeaderActionsVisible(bool visible);
    void Layout();

    Size GetIdealSize();

    int cmdId = 0;

    Rect closeBtnPos{};
    Rect firstActionPos{};
    Rect secondActionPos{};
    Rect thirdActionPos{};
    Rect viewBtnPos[5]{};
    LabelViewButton viewBtns[5]{};
    int nViewButtons = 0;
    int viewBtnTooltipId[5]{-1, -1, -1, -1, -1};
    Rect rightBtnPos[3]{};
    LabelViewButton rightBtns[3]{};
    int nRightButtons = 0;
    int rightBtnTooltipId[3]{-1, -1, -1};
    bool headerActionsVisible = true;

    // in points
    int padX = 0;
    int padY = 0;

    Func0 firstAction;
    Func0 secondAction;
    Func0 thirdAction;
    Tooltip* actionsTooltip = nullptr;
    HFONT actionsTooltipFont = nullptr;
    int firstActionTooltipId = -1;
    int secondActionTooltipId = -1;
    int thirdActionTooltipId = -1;
    const char* firstActionTooltip = nullptr; // _TRN key, translated on demand
    const char* secondActionTooltip = nullptr;
    const char* thirdActionTooltip = nullptr;
    int pressedAction = 0;
};
