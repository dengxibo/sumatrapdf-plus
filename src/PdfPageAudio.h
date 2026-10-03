/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Page-level playback of embedded PDF audio (Sound / RichMedia / Screen).
// Word highlight follows the recording: silence in the clip does not advance the
// highlight, and the page's text (OCR when the page has none) is spread across
// the speech.

class EngineBase;
struct MainWindow;
struct WindowTab;
struct Annotation;

struct ReadAloudBarSource* PdfPageAudioBarSource();

bool PdfPageAudioIsThisClip(u64 token);
bool PdfPageAudioIsPlayingInTab(WindowTab* tab);
bool PdfPageAudioCanContinueInTab(WindowTab* tab);
// Current page already has a loaded media annotation. Does not open the page.
bool PdfPageAudioVisiblePageHas(WindowTab* tab);
// True when the toolbar speaker should drive embedded audio instead of TTS.
bool PdfPageAudioOwnsToolbarSpeaker(WindowTab* tab);
// Toolbar speaker and the bar's play button. Plays this page's clip, or pauses
// the clip already running. False when this page has no embedded audio.
bool PdfPageAudioHandleReadAloud(WindowTab* tab);
// Word lookup pauses the clip and resumes it when the popup closes.
bool PdfPageAudioPauseForLookup(WindowTab* tab);
bool PdfPageAudioResumeAfterLookup(WindowTab* tab);
void PdfPageAudioStop();
// Drop our session without touching the shared media engine (EPUB narration is taking it).
void PdfPageAudioAbandon();
// Takes ownership of data (free). Starts playback and shows the bar.
bool PdfPageAudioPlayClip(Annotation* annot, u8* data, size_t size);

void PdfPageAudioOnCanvasPaint(MainWindow* win, HDC hdc);
void PdfPageAudioOnOcrPageReady(EngineBase* engine, int pageNo);
void PdfPageAudioOnUserViewChanged(MainWindow* win);
void PdfPageAudioOnTabGone(WindowTab* tab);
void PdfPageAudioOnWindowClosing(MainWindow* win);
// Pause when this clip's tab is no longer the one on screen. Same as TTS and EPUB narration.
void PdfPageAudioPauseIfNotCurrent();
