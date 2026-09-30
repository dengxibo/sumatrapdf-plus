/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/WinUtil.h"

#include "wingui/UIModels.h"

#include "Settings.h"
#include "GlobalPrefs.h"
#include "DocController.h"
#include "EngineBase.h"
#include "DisplayModel.h"
#include "DisplayMode.h"
#include "WindowTab.h"
#include "MainWindow.h"
#include "Canvas.h"
#include "ReadAloudFollow.h"

bool ReadAloudFollowEnabled() {
    return !gGlobalPrefs || gGlobalPrefs->readAloudAutoFollow;
}

bool ReadAloudFollowScreenPos(DisplayModel* dm, int pageNo, const RectF& pageRect, float* topOut, float* bottomOut) {
    if (!dm || pageNo <= 0 || !dm->ValidPageNo(pageNo) || !dm->PageVisible(pageNo)) {
        return false;
    }
    Rect vp = dm->GetViewPort();
    if (vp.dy <= 0) {
        return false;
    }
    if (pageRect.IsEmpty()) {
        *topOut = 0.5f;
        *bottomOut = 0.5f;
        return true;
    }
    Rect sr = dm->CvtToScreen(pageNo, pageRect);
    *topOut = (float)sr.y / (float)vp.dy;
    *bottomOut = (float)(sr.y + sr.dy) / (float)vp.dy;
    return *bottomOut > 0.f && *topOut < 1.f;
}

bool ReadAloudFollowInSafeZone(DisplayModel* dm, int pageNo, const RectF& pageRect) {
    float top = 0;
    float bottom = 0;
    if (!ReadAloudFollowScreenPos(dm, pageNo, pageRect, &top, &bottom)) {
        return false;
    }
    return top >= kReadAloudFollowSafeTop && bottom <= kReadAloudFollowSafeBottom;
}

bool ReadAloudFollowFullyVisible(DisplayModel* dm, int pageNo, const RectF& pageRect) {
    float top = 0;
    float bottom = 0;
    if (!ReadAloudFollowScreenPos(dm, pageNo, pageRect, &top, &bottom)) {
        return false;
    }
    return top >= 0.f && bottom <= kReadAloudFollowSafeBottom;
}

static void ReadAloudFollowGoToPage(MainWindow* win, DisplayModel* dm, int pageNo, int scrollY) {
    win->readAloudScrollFromCode = true;
    dm->GoToPage(pageNo, scrollY, false);
    win->readAloudScrollFromCode = false;
}

void ReadAloudFollowScrollTo(MainWindow* win, DisplayModel* dm, int pageNo, const RectF& pageRect) {
    if (!win || !dm || pageNo <= 0 || !dm->ValidPageNo(pageNo)) {
        return;
    }
    Rect vp = dm->GetViewPort();
    if (vp.dy <= 0) {
        return;
    }
    int targetOff = (int)(vp.dy * kReadAloudFollowTarget);
    if (pageRect.IsEmpty()) {
        if (!dm->PageVisible(pageNo)) {
            ReadAloudFollowGoToPage(win, dm, pageNo, 0);
        }
        return;
    }
    if (IsContinuous(dm->GetDisplayMode()) && dm->PageVisible(pageNo)) {
        Rect sr = dm->CvtToScreen(pageNo, pageRect);
        int maxY = std::max(0, dm->canvasSize.dy - vp.dy);
        int target = dm->yOffset() + sr.y - targetOff;
        // text near the top of a page: show the page from its top instead of pulling the end of
        // the previous page (in reflowed books usually the previous chapter) into view
        if (PageInfo* pi = dm->GetPageInfo(pageNo)) {
            target = std::max(target, pi->pos.y - dm->windowMargin.top);
        }
        target = std::clamp(target, 0, maxY);
        int current = dm->yOffset();
        if (target == current) {
            return;
        }
        constexpr int kMinAnimateDeltaPx = 12;
        if (std::abs(target - current) < kMinAnimateDeltaPx) {
            win->readAloudScrollFromCode = true;
            dm->ScrollYTo(target);
            win->readAloudScrollFromCode = false;
            return;
        }
        // the smooth-scroll timer clears readAloudScrollFromCode when it arrives or gets stuck
        win->scrollTargetY = target;
        win->readAloudScrollFromCode = true;
        SetTimer(win->hwndCanvas, kSmoothScrollTimerID, USER_TIMER_MINIMUM, nullptr);
        return;
    }
    dm->EnsurePagesInfoForPage(pageNo);
    float zoom = dm->GetZoomSafe(pageNo);
    RectF tr = dm->GetEngine()->Transform(pageRect, pageNo, zoom, dm->GetRotation());
    ReadAloudFollowGoToPage(win, dm, pageNo, std::max(0, (int)tr.y - targetOff));
}
