/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#pragma once

struct MainWindow;
struct WindowTab;
struct GlobalPrefs;

bool InlineTranslateVolcIsConfigured();
bool InlineTranslateAiApiIsConfigured();

// Selection toolbar / menu: open the floating panel and translate.
void TranslateSelectionInTab(MainWindow* win, WindowTab* tab);

// Ask AI with configured AiToc API: open the floating panel for Q&A on the selection.
// initialPrompt is the full first user message (already classified/prompted).
void AskAiSelectionInTab(MainWindow* win, WindowTab* tab, const char* selection, const char* initialPrompt);

// clearSelection drops the document highlight when the user dismisses the popup.
void CloseInlineTranslatePopup(bool clearSelection = false);
bool IsInlineTranslatePopupVisible();
void RefreshInlineTranslatePopupTheme();
// Paints the popup prepared by RefreshInlineTranslatePopupTheme. Call once the
// main window is ready, so the popup and the frame switch together.
void FinishInlineTranslatePopupTheme();

// Main message loop hook: Esc closes the popup; keys typed into the popup
// bypass app accelerators. Returns true if the message was handled.
bool InlineTranslatePopupPreTranslate(MSG& msg);

// Blocking; call from a worker thread. Translates a fixed English sample to Chinese.
bool InlineTranslateVolcTest(const char* ak, const char* sk, char** resultOut, char** errOut);

// DPAPI protect/unprotect for Volc keys (called from AppSettings save/load).
void InlineTranslateUnprotectPrefsSecrets(GlobalPrefs* prefs);
void InlineTranslateProtectPrefsSecretsForSave(GlobalPrefs* prefs, char** plainAkOut, char** plainSkOut);
void InlineTranslateRestorePrefsSecretsAfterSave(GlobalPrefs* prefs, char* plainAk, char* plainSk);
