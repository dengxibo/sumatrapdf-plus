/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#pragma once

#include "utils/BaseUtil.h"

struct MainWindow;
struct WindowTab;
struct Annotation;

enum class CommandVisibility {
    Show,
    Disable,
    Hide,
};

enum class CommandSurface {
    Menu,
    Palette,
    Toolbar,
};

struct AppCommandCtx {
    MainWindow* win = nullptr;
    WindowTab* tab = nullptr;

    bool isDocLoaded = false;
    const char* filePath = nullptr;
    Kind engineKind = nullptr;
    int pageCount = 0;
    bool isPdf = false;
    bool isPdfEncrypted = false;
    bool canEditPdfToc = false;
    bool isChm = false;
    bool isCbx = false;
    bool isImageCollection = false;
    bool isReflowableEbook = false;
    bool isSinglePage = false;
    bool hasToc = false;

    Point cursorPos = {};
    bool hasSelection = false;
    bool isCursorOnPage = false;
    Annotation* annotationUnderCursor = nullptr;
    bool cursorOnLinkTarget = false;
    bool cursorOnComment = false;
    bool cursorOnImage = false;

    bool supportsAnnots = false;
    bool hasUnsavedAnnotations = false;

    int nTabs = 0;
    bool hasDocTabs = false;
    bool canCloseOtherTabs = false;
    bool canCloseTabsToRight = false;
    bool canCloseTabsToLeft = false;

    bool canSendEmail = false;
    bool allowToggleMenuBar = true;
    bool isSpeaking = false;
    bool canContinueReadAloud = false;
};

AppCommandCtx NewAppCommandCtx(MainWindow* win, Point cursorPos = {});

CommandVisibility GetCommandVisibility(int cmdId, const AppCommandCtx& ctx, CommandSurface surface);

bool CmdWorksWithoutDocument(int cmdId);

inline bool CommandShouldRemove(CommandVisibility v) {
    return v == CommandVisibility::Hide;
}

inline bool CommandShouldDisable(CommandVisibility v) {
    return v == CommandVisibility::Disable;
}

inline bool CommandShouldShow(CommandVisibility v) {
    return v != CommandVisibility::Hide;
}
