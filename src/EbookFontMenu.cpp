/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/GdiPlusUtil.h"

#include "wingui/UIModels.h"

#include "Settings.h"
#include "AppSettings.h"
#include "DisplayMode.h"
#include "DocController.h"
#include "EngineBase.h"
#include "EngineAll.h"
#include "GlobalPrefs.h"
#include "SumatraPDF.h"
#include "WindowTab.h"

#include "Commands.h"
#include "EbookFontConfig.h"
#include "EbookFontMenu.h"
#include "EbookInstalledFonts.h"

int gFirstEbookLatinFontCmdId = 0;
int gLastEbookLatinFontCmdId = 0;
int gFirstEbookCjkFontCmdId = 0;
int gLastEbookCjkFontCmdId = 0;

static int gEbookLatinBundledMenuCount = 0;
static int gEbookCjkBundledMenuCount = 0;

bool IsReflowableEbookTabForFontMenu(WindowTab* tab) {
    if (!tab || !tab->IsDocLoaded() || tab->IsAboutTab()) {
        return false;
    }
    EngineBase* engine = tab->GetEngine();
    if (!engine) {
        return false;
    }
    if (engine->kind == kindEngineMupdf) {
        return !str::EqI(engine->defaultExt, ".pdf");
    }
    return engine->kind == kindEngineMobi || engine->kind == kindEngineEpub || engine->kind == kindEngineFb2 ||
           engine->kind == kindEnginePdb || engine->kind == kindEngineHtml || engine->kind == kindEngineTxt;
}

static void SortFamilyNames(Vec<char*>* families) {
    if (!families || families->size() < 2) {
        return;
    }
    for (size_t i = 0; i + 1 < families->size(); i++) {
        for (size_t j = i + 1; j < families->size(); j++) {
            if (_stricmp(families->at(i), families->at(j)) > 0) {
                char* tmp = families->at(i);
                families->at(i) = families->at(j);
                families->at(j) = tmp;
            }
        }
    }
}

static bool FamilyListContainsCanonical(Vec<char*>* families, const char* family, bool cjk) {
    if (!families || !family) {
        return false;
    }
    for (char* existing : *families) {
        bool same =
            cjk ? EbookCjkFontFamiliesEquivalent(existing, family) : EbookLatinFontFamiliesEquivalent(existing, family);
        if (same) {
            return true;
        }
    }
    return false;
}

static void AppendUniqueFamily(Vec<char*>* families, char* family, bool cjk) {
    if (!families || !family || !family[0]) {
        str::Free(family);
        return;
    }
    if (FamilyListContainsCanonical(families, family, cjk)) {
        str::Free(family);
        return;
    }
    families->Append(family);
}

static void CollectBundledFontFamilies(Vec<char*>* latinFamilies, Vec<char*>* cjkFamilies) {
    if (!latinFamilies || !cjkFamilies) {
        return;
    }
    Vec<char*> all;
    CollectBundledFontFamilyNames(&all);
    for (char* family : all) {
        if (!family || !family[0]) {
            str::Free(family);
            continue;
        }
        if (IsBundledLatinFontFamily(family)) {
            const char* canonical = NormalizeEbookLatinFontFamily(family);
            AppendUniqueFamily(latinFamilies, str::Dup(canonical), false);
        } else {
            const char* canonical = NormalizeEbookCjkFontFamily(family);
            AppendUniqueFamily(cjkFamilies, str::Dup(canonical), true);
        }
        str::Free(family);
    }
    SortFamilyNames(latinFamilies);
    SortFamilyNames(cjkFamilies);
}

static void MergeInstalledFontFamilies(Vec<char*>* bundledFamilies, Vec<char*>* installedFamilies, bool cjk) {
    if (!bundledFamilies || !installedFamilies) {
        return;
    }
    SortFamilyNames(installedFamilies);
    for (char* family : *installedFamilies) {
        AppendUniqueFamily(bundledFamilies, family, cjk);
    }
    installedFamilies->Clear();
}

static int FindFontMenuCmdId(int origCmdId, const char* family) {
    if (!family || !family[0]) {
        return 0;
    }
    bool cjk = origCmdId == CmdSetEbookCjkFont;
    Vec<CustomCommand*> cmds;
    GetCommandsWithOrigId(cmds, origCmdId);
    for (CustomCommand* cmd : cmds) {
        const char* cmdFamily = GetCommandStringArg(cmd, kCmdArgFontFamily, nullptr);
        if (!cmdFamily) {
            continue;
        }
        bool same = cjk ? EbookCjkFontFamiliesEquivalent(cmdFamily, family)
                        : EbookLatinFontFamiliesEquivalent(cmdFamily, family);
        if (same) {
            return cmd->id;
        }
    }
    return 0;
}

static void CreateFontFamilyCommands(int origCmdId, Vec<char*>* families, int* firstCmdId, int* lastCmdId) {
    *firstCmdId = 0;
    *lastCmdId = 0;
    bool cjk = origCmdId == CmdSetEbookCjkFont;
    for (char* family : *families) {
        const char* label = cjk ? GetEbookCjkFontMenuLabel(family) : GetEbookLatinFontMenuLabel(family);
        auto args = NewStringArg(kCmdArgFontFamily, family);
        CustomCommand* cmd = CreateCustomCommand(label, origCmdId, args);
        cmd->name = str::Dup(label);
        if (*firstCmdId == 0) {
            *firstCmdId = cmd->id;
        }
        *lastCmdId = cmd->id;
    }
}

void CreateEbookFontMenuCommands() {
    gFirstEbookLatinFontCmdId = 0;
    gLastEbookLatinFontCmdId = 0;
    gFirstEbookCjkFontCmdId = 0;
    gLastEbookCjkFontCmdId = 0;
    gEbookLatinBundledMenuCount = 0;
    gEbookCjkBundledMenuCount = 0;

    Vec<char*> latinFamilies;
    Vec<char*> cjkFamilies;
    CollectBundledFontFamilies(&latinFamilies, &cjkFamilies);
    gEbookLatinBundledMenuCount = (int)latinFamilies.size();
    gEbookCjkBundledMenuCount = (int)cjkFamilies.size();

    Vec<char*> latinInstalled;
    Vec<char*> cjkInstalled;
    CollectInstalledLatinFontFamilies(&latinInstalled);
    CollectInstalledCjkFontFamilies(&cjkInstalled);
    MergeInstalledFontFamilies(&latinFamilies, &latinInstalled, false);
    MergeInstalledFontFamilies(&cjkFamilies, &cjkInstalled, true);

    CreateFontFamilyCommands(CmdSetEbookLatinFont, &latinFamilies, &gFirstEbookLatinFontCmdId,
                             &gLastEbookLatinFontCmdId);
    CreateFontFamilyCommands(CmdSetEbookCjkFont, &cjkFamilies, &gFirstEbookCjkFontCmdId, &gLastEbookCjkFontCmdId);
    for (char* family : latinFamilies) {
        str::Free(family);
    }
    for (char* family : cjkFamilies) {
        str::Free(family);
    }
}

static void CheckFontMenuRadio(HMENU menu, int origCmdId, const char* currentFamily, int firstCmdId, int lastCmdId) {
    if (!menu || firstCmdId <= 0 || lastCmdId < firstCmdId) {
        return;
    }
    int currCmdId = FindFontMenuCmdId(origCmdId, currentFamily);
    if (currCmdId >= firstCmdId && currCmdId <= lastCmdId) {
        CheckMenuRadioItem(menu, firstCmdId, lastCmdId, currCmdId, MF_BYCOMMAND);
    }
}

static void AppendFontCommandsToMenu(HMENU menu, int origCmdId, int bundledCount, int firstCmdId, int lastCmdId,
                                     const char* currentFamily) {
    Vec<CustomCommand*> cmds;
    GetCommandsWithOrigId(cmds, origCmdId);
    int idx = 0;
    for (CustomCommand* cmd : cmds) {
        if (!cmd->name) {
            continue;
        }
        if (bundledCount > 0 && idx == bundledCount) {
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        }
        TempWStr ws = ToWStrTemp(cmd->name);
        AppendMenuW(menu, MF_STRING, (UINT_PTR)cmd->id, ws);
        idx++;
    }
    CheckFontMenuRadio(menu, origCmdId, currentFamily, firstCmdId, lastCmdId);
}

void AppendEbookLatinFontsToMenu(HMENU menu) {
    AppendFontCommandsToMenu(menu, CmdSetEbookLatinFont, gEbookLatinBundledMenuCount, gFirstEbookLatinFontCmdId,
                             gLastEbookLatinFontCmdId, GetEbookLatinFontFamily());
}

void AppendEbookCjkFontsToMenu(HMENU menu) {
    AppendFontCommandsToMenu(menu, CmdSetEbookCjkFont, gEbookCjkBundledMenuCount, gFirstEbookCjkFontCmdId,
                             gLastEbookCjkFontCmdId, GetEbookCjkFontFamily());
}

void UpdateEbookFontMenuRadioState(HMENU menu) {
    CheckFontMenuRadio(menu, CmdSetEbookLatinFont, GetEbookLatinFontFamily(), gFirstEbookLatinFontCmdId,
                       gLastEbookLatinFontCmdId);
    CheckFontMenuRadio(menu, CmdSetEbookCjkFont, GetEbookCjkFontFamily(), gFirstEbookCjkFontCmdId,
                       gLastEbookCjkFontCmdId);
}
