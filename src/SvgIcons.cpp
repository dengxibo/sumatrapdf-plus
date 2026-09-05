/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#include "utils/BaseUtil.h"
#include "SvgIcons.h"

// https://github.com/tabler/tabler-icons/blob/master/icons/folder.svg
static const char* gIconFileOpen =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="1" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <rect x="0" y="0" width="24" height="24" stroke="none"></rect>
  <path d="M5 4h4l3 3h7a2 2 0 0 1 2 2v8a2 2 0 0 1 -2 2h-14a2 2 0 0 1 -2 -2v-11a2 2 0 0 1 2 -2" />
</svg>)";

// https://github.com/tabler/tabler-icons/blob/master/icons/printer.svg
static const char* gIconPrint =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="1" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <rect x="0" y="0" width="24" height="24" stroke="none"></rect>
  <path d="M17 17h2a2 2 0 0 0 2 -2v-4a2 2 0 0 0 -2 -2h-14a2 2 0 0 0 -2 2v4a2 2 0 0 0 2 2h2" />
  <path d="M17 9v-4a2 2 0 0 0 -2 -2h-6a2 2 0 0 0 -2 2v4" />
  <rect x="7" y="13" width="10" height="8" rx="2" />
</svg>)";

// https://github.com/tabler/tabler-icons/blob/master/icons/arrow-left.svg
static const char* gIconPagePrev =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="1" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <rect x="0" y="0" width="24" height="24" stroke="none"></rect>
  <line x1="5" y1="12" x2="19" y2="12" />
  <line x1="5" y1="12" x2="11" y2="18" />
  <line x1="5" y1="12" x2="11" y2="6" />
</svg>)";

// https://github.com/tabler/tabler-icons/blob/master/icons/arrow-right.svg
static const char* gIconPageNext =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="1" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <rect x="0" y="0" width="24" height="24" stroke="none"></rect>
  <line x1="5" y1="12" x2="19" y2="12" />
  <line x1="13" y1="18" x2="19" y2="12" />
  <line x1="13" y1="6" x2="19" y2="12" />
</svg>)";

// https://github.com/tabler/tabler-icons/blob/master/icons/layout-rows.svg
static const char* gIconLayoutContinuous =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="1" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <rect x="0" y="0" width="24" height="24" stroke="none"></rect>
  <rect x="3" y="3" width="18" height="18" rx="2" />
  <line x1="3" y1="12" x2="21" y2="12" />
</svg>)";

// Portrait rounded rect: Fit Single Page vs AnnotSquare (square).
static const char* gIconLayoutSinglePage =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="1" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <rect x="0" y="0" width="24" height="24" stroke="none"></rect>
  <rect x="5" y="3" width="14" height="18" rx="2" />
</svg>)";

// https://github.com/tabler/tabler-icons/blob/master/icons/chevron-left.svg
static const char* gIconSearchPrev =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="1" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <rect x="0" y="0" width="24" height="24" stroke="none"></rect>
  <polyline points="15 6 9 12 15 18" />
</svg>)";

// https://github.com/tabler/tabler-icons/blob/master/icons/chevron-right.svg
static const char* gIconSearchNext =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="1" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <rect x="0" y="0" width="24" height="24" stroke="none"></rect>
  <polyline points="9 6 15 12 9 18" />
</svg>)";

// https://github.com/tabler/tabler-icons/blob/master/icons/letter-case.svg
static const char* gIconMatchCase =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="1" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <path stroke="none" d="M0 0h24v24H0z"/>
  <circle cx="18" cy="16" r="3" />
  <line x1="21" y1="13" x2="21" y2="19" />
  <path d="M3 19l5 -13l5 13" />
  <line x1="5" y1="14" x2="11" y2="14" />
</svg>)";

// "match whole word": lowercase "ab" over an underline bracketed at both ends,
// suggesting a complete word delimited by word boundaries (like VS Code's
// whole-word toggle). Custom icon drawn in the tabler stroke style.
static const char* gIconMatchWholeWord =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="1" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <path stroke="none" d="M0 0h24v24H0z"/>
  <circle cx="7" cy="11" r="2.5" />
  <line x1="9.5" y1="8.5" x2="9.5" y2="13.5" />
  <line x1="14.5" y1="6" x2="14.5" y2="13.5" />
  <circle cx="17" cy="11" r="2.5" />
  <path d="M3 16v3h18v-3" />
</svg>)";

// https://github.com/tabler/tabler-icons/blob/master/icons/zoom-in.svg
static const char* gIconZoomIn =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="1" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <rect x="0" y="0" width="24" height="24" stroke="none"></rect>
  <circle cx="10" cy="10" r="7" />
  <line x1="7" y1="10" x2="13" y2="10" />
  <line x1="10" y1="7" x2="10" y2="13" />
  <line x1="21" y1="21" x2="15" y2="15" />
</svg>)";

// https://github.com/tabler/tabler-icons/blob/master/icons/zoom-out.svg
static const char* gIconZoomOut =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="1" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <rect x="0" y="0" width="24" height="24" stroke="none"></rect>
  <circle cx="10" cy="10" r="7" />
  <line x1="7" y1="10" x2="13" y2="10" />
  <line x1="21" y1="21" x2="15" y2="15" />
</svg>)";

// https://github.com/tabler/tabler-icons/blob/master/icons/floppy-disk.svg
static const char* gIconSave =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="1" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <path stroke="none" d="M0 0h24v24H0z"/>
  <path d="M18 20h-12a2 2 0 0 1 -2 -2v-12a2 2 0 0 1 2 -2h9l5 5v9a2 2 0 0 1 -2 2" />
  <circle cx="12" cy="13" r="2" />
  <polyline points="4 8 10 8 10 4" />
</svg>)";

// sidebar toggle icon - custom
static const char* gIconBookmark =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="1.25" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <path stroke="none" d="M0 0h24v24H0z" fill="none"/>
  <rect x="3" y="4" width="18" height="16" rx="2" />
  <path d="M8 4v16" />
</svg>)";

// https://github.com/tabler/tabler-icons/blob/master/icons/rotate-2.svg - modified
static const char* gIconRotateLeft =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="1" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <path stroke="none" d="M0 0h24v24H0z" fill="none"/>
  <path d="M15 4.55a8 8 0 0 0 -6 14.9m0 -5.45v6h-6"/>
  <circle cx="18.37" cy="7.16" r="0.15"/>
  <circle cx="13" cy="19.94" r="0.15"/>
  <circle cx="16.84" cy="18.37" r="0.15"/>
  <circle cx="19.37" cy="15.1" r="0.15"/>
  <circle cx="19.94" cy="11" r="0.15"/>
</svg>)";

// https://github.com/tabler/tabler-icons/blob/master/icons/rotate-clockwise-2.svg - modified
static const char* gIconRotateRight =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="1" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <path stroke="none" d="M0 0h24v24H0z" fill="none"/>
  <path d="M9 4.55a8 8 0 0 1 6 14.9m0 -5.45v6h6"/>
  <circle cx="5.63" cy="7.16" r="0.15"/>
  <circle cx="4.06" cy="11" r="0.15"/>
  <circle cx="4.63" cy="15.1" r="0.15"/>
  <circle cx="7.16" cy="18.37" r="0.15"/>
  <circle cx="11" cy="19.94" r="0.15"/>
</svg>)";

static const char* gIconSpeak =
    R"SPEAK(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="0.95" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <path stroke="none" d="M0 0h24v24H0z" fill="none"/>
  <g transform="translate(12 12) scale(0.96) translate(-12 -12)">
    <path d="M15 8a5 5 0 0 1 0 8" />
    <path d="M17.7 5a9 9 0 0 1 0 14" />
    <path d="M6 15h-2a1 1 0 0 1 -1 -1v-4a1 1 0 0 1 1 -1h2l3.5 -4.5a.8 .8 0 0 1 1.5 .5v14a.8 .8 0 0 1 -1.5 .5l-3.5 -4.5" />
  </g>
</svg>)SPEAK";

static const char* gIconPauseSpeaking =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="1" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <path stroke="none" d="M0 0h24v24H0z" fill="none"/>
  <path d="M6 5v14" />
  <path d="M18 5v14" />
</svg>)";

// https://github.com/tabler/tabler-icons/blob/master/icons/arrow-back-up.svg
static const char* gIconNavigateBack =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="1" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <rect x="0" y="0" width="24" height="24" stroke="none"></rect>
  <path d="M9 14l-4 -4l4 -4" />
  <path d="M5 10h11a4 4 0 1 1 0 8h-1" />
</svg>)";

// https://github.com/tabler/tabler-icons/blob/master/icons/arrow-forward-up.svg
static const char* gIconNavigateForward =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="1" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <rect x="0" y="0" width="24" height="24" stroke="none"></rect>
  <path d="M15 14l4 -4l-4 -4" />
  <path d="M19 10h-11a4 4 0 1 0 0 8h1" />
</svg>)";

static const char* gIconSearch =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="1" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <rect x="0" y="0" width="24" height="24" stroke="none"></rect>
  <circle cx="10" cy="10" r="7" />
  <line x1="21" y1="21" x2="15" y2="15" />
</svg>)";

static const char* gIconChevronUp =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="1" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <rect x="0" y="0" width="24" height="24" stroke="none"></rect>
  <polyline points="6 15 12 9 18 15" />
</svg>)";

static const char* gIconChevronDown =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="1" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <rect x="0" y="0" width="24" height="24" stroke="none"></rect>
  <polyline points="6 9 12 15 18 9" />
</svg>)";

// original PDF colors: portrait page with text lines (matches layout icon style)
static const char* gIconDocColorOriginal =
    R"DOCORIG(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="1" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <rect x="0" y="0" width="24" height="24" stroke="none"></rect>
  <g transform="translate(12 12) scale(0.92) translate(-12 -12)">
    <rect x="5" y="3" width="14" height="18" rx="2" />
    <line x1="8" y1="8" x2="16" y2="8" />
    <line x1="8" y1="12" x2="16" y2="12" />
    <line x1="8" y1="16" x2="13" y2="16" />
  </g>
</svg>)DOCORIG";

// https://github.com/tabler/tabler-icons/blob/master/icons/moon.svg - scaled down to match sun icon
static const char* gIconThemeMoon =
    R"MOON(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="1" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <rect x="0" y="0" width="24" height="24" stroke="none"></rect>
  <g transform="translate(12 12) scale(0.92) translate(-12 -12)">
    <path d="M12 3c.132 0 .263 0 .393 0a7.5 7.5 0 0 0 7.92 12.446a9 9 0 1 1 -8.313 -12.454z" />
  </g>
</svg>)MOON";

// magic wand: smart document color mode
static const char* gIconDocColorAuto =
    R"DOCAUTO(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="1" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <rect x="0" y="0" width="24" height="24" stroke="none"></rect>
  <path d="M4.25 19.75L15.6 8.4" />
  <path d="M13.9 6.7L17.3 10.1" />
  <path d="M17.5 2.8V5.2" />
  <path d="M17.5 8V10.4" />
  <path d="M13.7 6.6H16.1" />
  <path d="M18.9 6.6H21.3" />
  <path d="M8.1 3.5V5.1" />
  <path d="M7.3 4.3H8.9" />
  <path d="M20 14.2V15.8" />
  <path d="M19.2 15H20.8" />
</svg>)DOCAUTO";

// T-shirt (theme): match document colors to the current theme; stroke style after iconfont garment glyph
static const char* gIconDocColorFollowTheme =
    R"DOCFOLLOW(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="1" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <rect x="0" y="0" width="24" height="24" stroke="none"></rect>
  <g transform="translate(12 12) scale(0.92) translate(-12 -12)">
    <path d="M15 4l6 2v5h-3v8a1 1 0 0 1 -1 1h-10a1 1 0 0 1 -1 -1v-8h-3v-5l6 -2" />
    <path d="M10.5 6.5c.6 -.6 1.1 -.9 1.5 -.9s.9 .3 1.5 .9" />
  </g>
</svg>)DOCFOLLOW";

// Translate-style word lookup: corner swap arrows + Latin a + hiragana (Tabler language-hiragana, scaled)
static const char* gIconDictionary =
    R"DICT(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="1" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <rect x="0" y="0" width="24" height="24" stroke="none"></rect>
  <path d="M9 4.5a4.5 4.5 0 0 0 -4.5 4.5" />
  <path d="M4.5 9l1.25 0" />
  <path d="M4.5 9l0 -1.25" />
  <path d="M15 19.5a4.5 4.5 0 0 0 4.5 -4.5" />
  <path d="M19.5 15l-1.25 0" />
  <path d="M19.5 15l0 1.25" />
  <path d="M4 19v-5a2 2 0 1 1 4 0v5" />
  <path d="M4 16h4" />
  <g transform="translate(9.5 0) scale(0.88)">
    <path d="M4 5h7" />
    <path d="M7 4c0 4.846 0 7 .5 8" />
    <path d="M10 8.5c0 2.286 -2 4.5 -3.5 4.5s-2.5 -1.135 -2.5 -2c0 -2 1 -3 3 -3s5 .57 5 2.857c0 1.524 -.667 2.571 -2 3.143" />
  </g>
</svg>)DICT";

// https://github.com/tabler/tabler-icons/blob/master/icons/list.svg
// Same 1px stroke as the toolbar; scale the glyph only (stroke stays 1).
static const char* gIconHomeList =
    R"LIST(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="0.85" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <rect x="0" y="0" width="24" height="24" stroke="none"></rect>
  <g transform="translate(12 12) scale(1.18) translate(-12 -12)">
    <line x1="9" y1="6" x2="20" y2="6" />
    <line x1="9" y1="12" x2="20" y2="12" />
    <line x1="9" y1="18" x2="20" y2="18" />
    <circle cx="5" cy="6" r="1" fill="currentColor" stroke="none" />
    <circle cx="5" cy="12" r="1" fill="currentColor" stroke="none" />
    <circle cx="5" cy="18" r="1" fill="currentColor" stroke="none" />
  </g>
</svg>)LIST";

// https://github.com/tabler/tabler-icons/blob/master/icons/layout-grid.svg
static const char* gIconHomeThumbnails =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="1" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <rect x="0" y="0" width="24" height="24" stroke="none"></rect>
  <rect x="4" y="4" width="6" height="6" rx="1" />
  <rect x="14" y="4" width="6" height="6" rx="1" />
  <rect x="4" y="14" width="6" height="6" rx="1" />
  <rect x="14" y="14" width="6" height="6" rx="1" />
</svg>)";

// Recently opened: official Microsoft Fluent UI System Icons "History 20
// Regular", geometry used verbatim:
// https://github.com/microsoft/fluentui-system-icons/blob/main/assets/History/SVG/ic_fluent_history_20_regular.svg
// Only fill="#212121" is changed to fill="currentColor" so the icon follows
// the theme like the other toolbar icons (DrawSvgIcon swaps currentColor for
// the theme text color). The toolbar renders home icons at ~20px, matching
// this icon's native 20px grid. The background rect (like in the Tabler
// icons) is required by DrawSvgIcon's transparency keying: BlitPixmap marks
// pixels equal to the theme background color as transparent; without it the
// MuPDF pixmap's white base would show as an opaque white square.
// fill-opacity compensates visual weight: the Tabler toolbar icons render a
// ~0.83px stroke (stroke-width 1 on a 24px grid scaled to 20px) whose
// antialiased coverage integrates to pi*0.83/4 ~= 0.65, while this icon's
// native 1:1 fill strokes are ~1.5px solid. fill-opacity is a paint
// property; the official path geometry is untouched.
static const char* gIconHomeHistory =
    R"HIST(<svg width="20" height="20" viewBox="0 0 20 20" fill="none" xmlns="http://www.w3.org/2000/svg">
<rect x="-1" y="-1" width="22" height="22" fill="none"/>
<path d="M10 4C13.3137 4 16 6.68629 16 10C16 13.3137 13.3137 16 10 16C6.68629 16 4 13.3137 4 10C4 9.84443 4.00591 9.69034 4.0175 9.53793C4.03845 9.26258 3.83222 9.02239 3.55687 9.00144C3.28152 8.98049 3.04133 9.18673 3.02038 9.46207C3.00687 9.6397 3 9.8191 3 10C3 13.866 6.13401 17 10 17C13.866 17 17 13.866 17 10C17 6.13401 13.866 3 10 3C8.04094 3 6.27012 3.80499 5 5.10109V3.5C5 3.22386 4.77614 3 4.5 3C4.22386 3 4 3.22386 4 3.5V6.5C4 6.77614 4.22386 7 4.5 7H7.5C7.77614 7 8 6.77614 8 6.5C8 6.22386 7.77614 6 7.5 6H5.52772C6.62683 4.77191 8.2234 4 10 4ZM10 6.5C10 6.22386 9.77614 6 9.5 6C9.22386 6 9 6.22386 9 6.5V10.5C9 10.7761 9.22386 11 9.5 11H12.5C12.7761 11 13 10.7761 13 10.5C13 10.2239 12.7761 10 12.5 10H10V6.5Z" fill="currentColor" fill-opacity="0.65"/>
</svg>)HIST";

// https://github.com/tabler/tabler-icons/blob/master/icons/outline/flame.svg
// Same 1px stroke as the toolbar after scale.
static const char* gIconHomeFrequent =
    R"FLAME(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="0.83" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <rect x="0" y="0" width="24" height="24" stroke="none"></rect>
  <g transform="translate(12 10.8) scale(1.2) translate(-12 -12)">
    <path d="M12 12c2 -2.96 0 -7 -1 -8c0 3.038 -1.773 4.741 -3 6c-1.226 1.26 -2 3.24 -2 5a6 6 0 1 0 12 0c0 -1.532 -1.056 -3.94 -2 -5c-1.786 3 -2.791 3 -4 2z" />
  </g>
</svg>)FLAME";

// https://github.com/tabler/tabler-icons/blob/master/icons/x.svg
static const char* gIconClose =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="1" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <rect x="0" y="0" width="24" height="24" stroke="none"></rect>
  <line x1="18" y1="6" x2="6" y2="18" />
  <line x1="6" y1="6" x2="18" y2="18" />
</svg>)";

// https://github.com/tabler/tabler-icons/blob/master/icons/pin.svg
static const char* gIconPin =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="1" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <rect x="0" y="0" width="24" height="24" stroke="none"></rect>
  <path d="M15 4.5l4.5 4.5" />
  <path d="M14.5 9.5l-5 5" />
  <path d="M16 3l5 5l-4 1l-4 4l-1 4l-5 -5l4 -1l4 -4z" />
  <path d="M9 15l-4.5 4.5" />
  <path d="M14 18l-3 3" />
</svg>)";

// tabler arrows-diagonal: expand the compact find bar into a floating window
static const char* gIconArrowsDiagonal =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="1" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <rect x="0" y="0" width="24" height="24" stroke="none"></rect>
  <path d="M16 4l4 0l0 4" />
  <path d="M14 10l6 -6" />
  <path d="M8 20l-4 0l0 -4" />
  <path d="M4 20l6 -6" />
</svg>)";

// tabler arrows-diagonal-minimize-2: dock the floating window back to the bar
static const char* gIconArrowsDiagonalMinimize =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="1" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <rect x="0" y="0" width="24" height="24" stroke="none"></rect>
  <path d="M16 20l0 -4l4 0" />
  <path d="M14 14l6 6" />
  <path d="M8 4l0 4l-4 0" />
  <path d="M4 4l6 6" />
</svg>)";

// Custom "font size" icons: A with superscript - / + (tabler stroke style).
static const char* gIconEbookFontSizeDecrease =
    R"FONTDEC(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="1" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <rect x="0" y="0" width="24" height="24" stroke="none"></rect>
  <g transform="translate(12 12) scale(0.85) translate(-12 -12)">
    <line x1="4.5" y1="19.5" x2="10.5" y2="4.5"/>
    <line x1="10.5" y1="4.5" x2="16.5" y2="19.5"/>
    <line x1="6.8" y1="14" x2="14.2" y2="14"/>
    <line x1="18" y1="6.5" x2="22" y2="6.5"/>
  </g>
</svg>)FONTDEC";

static const char* gIconEbookFontSizeIncrease =
    R"FONTINC(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="1" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <rect x="0" y="0" width="24" height="24" stroke="none"></rect>
  <g transform="translate(12 12) scale(0.85) translate(-12 -12)">
    <line x1="4.5" y1="19.5" x2="10.5" y2="4.5"/>
    <line x1="10.5" y1="4.5" x2="16.5" y2="19.5"/>
    <line x1="6.8" y1="14" x2="14.2" y2="14"/>
    <line x1="20" y1="5.5" x2="20" y2="9.5"/>
    <line x1="18" y1="7.5" x2="22" y2="7.5"/>
  </g>
</svg>)FONTINC";

static const char* gIconAnnotText =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="1" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <rect x="0" y="0" width="24" height="24" stroke="none"></rect>
  <path d="M5 4h14a2 2 0 0 1 2 2v9a2 2 0 0 1 -2 2h-5l-5 3v-3h-4a2 2 0 0 1 -2 -2v-9a2 2 0 0 1 2 -2" />
</svg>)";

static const char* gIconAnnotSquare =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="1" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <rect x="0" y="0" width="24" height="24" stroke="none"></rect>
  <rect x="4" y="4" width="16" height="16" rx="2" />
</svg>)";

static const char* gIconAnnotCircle =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="1" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <rect x="0" y="0" width="24" height="24" stroke="none"></rect>
  <circle cx="12" cy="12" r="8" />
</svg>)";

static const char* gIconAnnotLine =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="1" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <rect x="0" y="0" width="24" height="24" stroke="none"></rect>
  <line x1="5" y1="19" x2="19" y2="5" />
</svg>)";

static const char* gIconAnnotInk =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="1" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <rect x="0" y="0" width="24" height="24" stroke="none"></rect>
  <path d="M4 18 L8 14 L14 11 L20 5" />
</svg>)";

// tabler rubber-stamp, without the ground line (same stroke as other toolbar icons)
static const char* gIconAnnotStamp =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="1" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <rect x="0" y="0" width="24" height="24" stroke="none"></rect>
  <path d="M21 17.85h-18c0 -4.05 1.421 -4.05 3.79 -4.05c5.21 0 1.21 -4.59 1.21 -6.8a4 4 0 1 1 8 0c0 2.21 -4 6.8 1.21 6.8c2.369 0 3.79 0 3.79 4.05z" />
</svg>)";

// Compact four-corner expand icon. Reduce the filled path's opacity so its
// visual weight matches the neighboring 1px toolbar strokes.
static const char* gIconFullscreen =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 1024 1024">
  <rect x="0" y="0" width="1024" height="1024" stroke="none" fill="none"></rect>
  <path d="M285.866667 810.666667H384v42.666666H213.333333v-170.666666h42.666667v98.133333l128-128 29.866667 29.866667-128 128z m494.933333 0l-128-128 29.866667-29.866667 128 128V682.666667h42.666666v170.666666h-170.666666v-42.666666h98.133333zM285.866667 256l128 128-29.866667 29.866667-128-128V384H213.333333V213.333333h170.666667v42.666667H285.866667z m494.933333 0H682.666667V213.333333h170.666666v170.666667h-42.666666V285.866667l-128 128-29.866667-29.866667 128-128z" fill="currentColor" fill-opacity="0.66"></path>
</svg>)";

// Matching 4-corner inward when already fullscreen (tabler arrows-minimize).
static const char* gIconFullscreenExit =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="1" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <rect x="0" y="0" width="24" height="24" stroke="none"></rect>
  <path d="M5 9l4 0l0 -4" />
  <path d="M3 3l6 6" />
  <path d="M5 15l4 0l0 4" />
  <path d="M3 21l6 -6" />
  <path d="M19 9l-4 0l0 -4" />
  <path d="M15 9l6 -6" />
  <path d="M19 15l-4 0l0 4" />
  <path d="M15 15l6 6" />
</svg>)";

// https://github.com/tabler/tabler-icons/blob/master/icons/outline/scan.svg
static const char* gIconOcr =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="1" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <rect x="0" y="0" width="24" height="24" stroke="none"></rect>
  <path d="M4 7v-1a2 2 0 0 1 2 -2h2" />
  <path d="M4 17v1a2 2 0 0 0 2 2h2" />
  <path d="M16 4h2a2 2 0 0 1 2 2v1" />
  <path d="M16 20h2a2 2 0 0 0 2 -2v-1" />
  <path d="M5 12l14 0" />
</svg>)";

// locate: user pin (1024 filled icon → 24 outline, same stroke as link/merge/close)
static const char* gIconMapPin =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="1" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <rect x="0" y="0" width="24" height="24" stroke="none"></rect>
  <circle cx="12" cy="10.1" r="2.3" />
  <path d="M12 21c-3.6-5.4-6.8-8.2-6.8-11a6.8 6.8 0 0 1 13.6 0c0 2.8-3.2 5.6-6.8 11z" />
</svg>)";

// merge next TOC row: rounded square + up/down chevrons (user icon, Tabler stroke)
static const char* gIconMergeUp =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="1" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <rect x="0" y="0" width="24" height="24" stroke="none"></rect>
  <rect x="4" y="4" width="16" height="16" rx="3" />
  <path d="M9 10l3 -3l3 3" />
  <path d="M9 14l3 3l3 -3" />
</svg>)";

// tabler link: associate dest to current view
// list lines mapped to dest boxes (calibrate printed TOC pages)
static const char* gIconCalibrateToc =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="1" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <rect x="0" y="0" width="24" height="24" stroke="none"></rect>
  <line x1="4" y1="8" x2="13" y2="8" />
  <rect x="15" y="6" width="4" height="4" />
  <line x1="4" y1="16" x2="13" y2="16" />
  <rect x="15" y="14" width="4" height="4" />
</svg>)";

static const char* gIconLink =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" stroke-width="1" stroke="currentColor" fill="none" stroke-linecap="round" stroke-linejoin="round">
  <rect x="0" y="0" width="24" height="24" stroke="none"></rect>
  <path d="M9 15l6 -6" />
  <path d="M11 6l.463 -.536a5 5 0 0 1 7.071 7.072l-.534 .464" />
  <path d="M13 18l-.397 .534a5.068 5.068 0 0 1 -7.127 0a4.972 4.972 0 0 1 0 -7.071l.524 -.463" />
</svg>)";

// must match order in enum class TbIcon
// clang-format off
static const char* gIcons[] = {
    gIconFileOpen,
    gIconPrint,
    gIconPagePrev,
    gIconPageNext,
    gIconLayoutContinuous,
    gIconLayoutSinglePage,
    gIconZoomOut,
    gIconZoomIn,
    gIconSearchPrev,
    gIconSearchNext,
    gIconMatchCase,
    gIconMatchCase,  // TODO: remove this, is for compatiblity with bitmap icons
    gIconSave,
    gIconBookmark,
    gIconRotateLeft,
    gIconRotateRight,
    gIconDocColorOriginal,
    gIconThemeMoon,
    gIconDocColorAuto,
    gIconDocColorFollowTheme,
    gIconDictionary,
    gIconSpeak,
    gIconPauseSpeaking,
    gIconNavigateBack,
    gIconNavigateForward,
    gIconSearch,
    gIconChevronUp,
    gIconChevronDown,
    gIconHomeList,
    gIconHomeThumbnails,
    gIconClose,
    gIconPin,
    gIconArrowsDiagonal,
    gIconArrowsDiagonalMinimize,
    gIconMatchWholeWord,
    gIconEbookFontSizeDecrease,
    gIconEbookFontSizeIncrease,
    gIconAnnotText,
    gIconAnnotSquare,
    gIconAnnotCircle,
    gIconAnnotLine,
    gIconAnnotInk,
    gIconOcr,
    gIconMapPin,
    gIconMergeUp,
    gIconLink,
    gIconCalibrateToc,
    gIconAnnotStamp,
    gIconFullscreen,
    gIconFullscreenExit,
    gIconHomeHistory,
    gIconHomeFrequent,
};
// clang-format on

const char* GetSvgIcon(TbIcon idx) {
    int n = (int)idx;
    ReportIf(n < 0 || n >= dimofi(gIcons));
    if (n >= dimofi(gIcons)) {
        return nullptr;
    }
    return gIcons[n];
}
