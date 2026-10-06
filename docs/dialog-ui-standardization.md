# Dialog UI standardization

## Standard

Use the current annotation sidebar as the visual reference, not its sidebar layout:

- `GetAppFontForDpi` defines the UI font face and size; semibold is derived from it.
- `ButtonGetIdealSize` defines native button metrics (including logical padding).
- The panel background uses application control background in White/Warm and window background in Darcula/Dark. Text and control colors use existing theme tokens.
- Existing dialog actions, layout direction and persistence behavior remain unchanged.
- Windows-owned open/save/print/color-picker dialogs are not replaced or forcibly repainted.

## Entry-point inventory and current coverage

| Group | Implementation | Current change |
| --- | --- | --- |
| Options/settings and model picker | SumatraDialogs.cpp | Shared resource-dialog font/panel theme layer |
| Jump to page, find, password, language, zoom | SumatraDialogs.cpp | Shared resource-dialog font/panel theme layer |
| Favorite/bookmark, background color, scrollbar settings | SumatraDialogs.cpp | Shared resource-dialog font/panel theme layer |
| Western/CJK font picker | EbookFontMenu.cpp | Sidebar font/panel color; native button size measurement |
| TOC method selection and network-send confirmation | TocExtraction.cpp | Shared resource-dialog font/panel theme layer |
| Local/online AI TOC workflow and notices | AiToc.cpp | Sidebar font/panel color; native footer-button height |
| Unsaved PDF changes | SumatraPDF.cpp | Dedicated themed native dialog with application font and measured buttons |

`AppDialogFonts` now copies the sidebar application font rather than selecting MS Shell Dlg independently. `AppDialogUseStandardControls` covers resource dialogs routed through `AppDialogDispatch`, preserving semibold controls and dialog-specific event handlers. Existing theme-refresh callbacks are retained.

## Outstanding audit / verification

This is not a completed all-dialog visual acceptance pass.

- Remaining TaskDialog/MessageBox prompts: PDF destructive confirmations, TOC warnings, startup/recovery, update/download and error prompts.
- Independent windows: screenshot/signature, tab-group manager and property dialogs need individual inspection for layout/button parity.
- Resource-dialog button layout needs per-dialog measurement; applying the shared font/theme does not by itself normalize every fixed resource button dimension.
- All changed dialogs need White/Warm/Darcula/Dark visual checks, long localized labels and DPI regression.
- Font-family preview art must continue displaying the sampled font, not the application font.

Compilation reached linking without compiler errors, but the running SumatraPDF-Plus.exe blocked replacement (LNK1168). No new executable or visual acceptance is claimed for this pass.
