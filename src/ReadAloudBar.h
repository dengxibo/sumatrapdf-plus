/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Floating bottom bar with read-aloud controls, shared by EPUB 3 narration and text-to-speech.
// Each window has at most one bar. TTS that is speaking or paused wins over idle book
// audio so ±10 s does not appear and disappear between play and pause. While a source
// is playing it always wins (play icon and speed match what the reader hears).

struct MainWindow;

struct ReadAloudBarSource {
    virtual ~ReadAloudBarSource() = default;
    // "mo" / "tts", for the trace log
    virtual const char* Name() = 0;
    virtual bool Wanted(MainWindow* win) = 0;
    virtual bool IsPlaying(MainWindow* win) = 0;
    // offers the -10 s / +10 s buttons
    virtual bool CanSeek(MainWindow* win) = 0;
    // offers the elapsed / total time label
    virtual bool HasTime(MainWindow* win) = 0;
    virtual TempStr TimeLabelTemp(MainWindow* win) = 0;
    virtual double Rate(MainWindow* win) = 0;
    // following the text being read; when false the bar offers Follow
    virtual bool IsFollowing(MainWindow* win) = 0;
    virtual const char* PrevTooltip() = 0;
    virtual const char* NextTooltip() = 0;
    virtual const char* PlayTooltip() = 0;
    virtual const char* CloseTooltip() = 0;

    virtual void Prev(MainWindow* win) = 0;
    virtual void Next(MainWindow* win) = 0;
    virtual void TogglePlay(MainWindow* win) = 0;
    virtual void Skip(MainWindow* win, int dir) = 0;
    virtual void SetRate(MainWindow* win, double rate) = 0;
    virtual void Follow(MainWindow* win) = 0;
    virtual void Close(MainWindow* win) = 0;
};

// sources in priority order; defined next to the players
ReadAloudBarSource* MediaOverlayBarSource();
ReadAloudBarSource* TextToSpeechBarSource();

// shows, hides and relabels the bar of a window; cheap when nothing changed
void ReadAloudBarUpdate(MainWindow* win);
void ReadAloudBarOnWindowClosing(MainWindow* win);
