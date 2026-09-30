/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Keeps the text being read aloud in view, shared by text-to-speech and EPUB 3 narration.
// The highlight may move freely inside a safe zone of the viewport; once it leaves it, the view
// scrolls so the highlight sits at the follow target. Positions are fractions of the viewport
// height.

struct DisplayModel;
struct MainWindow;

constexpr float kReadAloudFollowSafeTop = 0.10f;
constexpr float kReadAloudFollowSafeBottom = 0.82f;
constexpr float kReadAloudFollowTarget = 0.30f;

// ReadAloudAutoFollow: when off, the view only moves on an explicit Follow request
bool ReadAloudFollowEnabled();

// top / bottom of (pageNo, pageRect) as viewport fractions; false when it is not on screen. An
// empty pageRect stands for the whole page.
bool ReadAloudFollowScreenPos(DisplayModel* dm, int pageNo, const RectF& pageRect, float* topOut, float* bottomOut);
bool ReadAloudFollowInSafeZone(DisplayModel* dm, int pageNo, const RectF& pageRect);
// entirely on screen and above the read-aloud bar: the reader brought it back into view
bool ReadAloudFollowFullyVisible(DisplayModel* dm, int pageNo, const RectF& pageRect);

// scrolls (smoothly when the page is shown in a continuous layout) so the rect sits at the follow
// target, never above the top of its page
void ReadAloudFollowScrollTo(MainWindow* win, DisplayModel* dm, int pageNo, const RectF& pageRect);
