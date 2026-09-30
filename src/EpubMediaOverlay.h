/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// EPUB 3 package + Media Overlays (SMIL) parsing. No UI, no MuPDF.
// https://www.w3.org/TR/epub-33/#sec-media-overlays

constexpr i64 kEpubMoClipToEnd = -1;

enum EpubMoTypeFlags : u32 {
    kEpubMoSkippable = 1 << 0, // footnote, endnote, pagebreak, ... (EPUB 3.3 9.4.1)
    kEpubMoEscapable = 1 << 1, // table, list, figure, aside, ... (EPUB 3.3 9.4.2)
};

// SMIL clock value (full clock, partial clock or timecount) to microseconds.
bool EpubParseClockValue(const char* s, i64* usOut);
bool EpubParseClockValueN(const char* s, size_t n, i64* usOut);

// Resolves href against baseDir (an archive directory without trailing '/', "" for the root).
// Percent-decodes, removes dot segments and splits off the fragment. Returns nullptr for
// anything that is not a path inside the container: URL schemes (http:, file:, data:, ...),
// drive letters, UNC paths, and ".." escaping the root. Strings are allocated in arena, or
// with str::Dup when arena is nullptr.
char* EpubResolvePath(const char* baseDir, const char* href, char** fragmentOut, Arena* arena = nullptr);
TempStr EpubDirOfTemp(const char* path);

u32 EpubMoFlagsForEpubType(const char* epubType);

struct EpubManifestItem {
    const char* id = nullptr;
    const char* path = nullptr; // resolved archive path
    const char* mediaType = nullptr;
    const char* properties = nullptr;
    const char* mediaOverlay = nullptr; // idref of the SMIL item
    i64 durationUs = -1;                // media:duration refines="#id"
};

struct EpubSpineItem {
    const char* idref = nullptr;
    int manifestIdx = -1;
    bool linear = true;
    bool prePaginated = false;
    int overlayIdx = -1; // index into EpubPackage::overlays, -1 when the item has no overlay
};

struct EpubMoPar {
    const char* id = nullptr;
    const char* textPath = nullptr;
    const char* textFragment = nullptr; // nullptr: whole document
    const char* audioPath = nullptr;    // nullptr: text-only par
    i64 clipBeginUs = 0;
    i64 clipEndUs = kEpubMoClipToEnd;
    const char* epubType = nullptr; // own epub:type
    u32 flags = 0;                  // own + inherited from enclosing seq
    int seqIdx = -1;                // innermost enclosing seq, -1 when directly under body
};

struct EpubMoSeq {
    const char* id = nullptr;
    const char* textPath = nullptr;
    const char* textFragment = nullptr;
    const char* epubType = nullptr;
    u32 flags = 0;
    int parentSeq = -1;
    int firstPar = 0;
    int endPar = 0; // exclusive
};

struct EpubMoDocument {
    int manifestIdx = -1;
    const char* smilPath = nullptr;
    Vec<EpubMoPar> pars;
    Vec<EpubMoSeq> seqs;
    bool parsed = false;
    bool failed = false;
};

struct EpubPackage {
    Arena* arena = nullptr;
    const char* opfPath = nullptr;
    const char* opfDir = nullptr;
    const char* version = nullptr;
    const char* navPath = nullptr;
    const char* activeClass = nullptr;
    const char* playbackActiveClass = nullptr;
    const char* narrator = nullptr;
    const char* pageProgressionDirection = nullptr;
    bool prePaginated = false;
    i64 totalDurationUs = -1;
    Vec<EpubManifestItem> manifest;
    Vec<EpubSpineItem> spine;
    Vec<EpubMoDocument*> overlays;
    StrVec diagnostics;

    EpubPackage();
    ~EpubPackage();

    bool HasMediaOverlays() const;
    int FindManifestById(const char* id) const;
    int FindManifestByPath(const char* path) const;
    int FindSpineByPath(const char* path) const;
    const char* SpinePath(int spineIdx) const;
    EpubMoDocument* OverlayForSpine(int spineIdx) const;
    void AddDiag(const char* fmt, ...);
};

// META-INF/container.xml: full-path of the first rootfile (allocated with str::Dup).
char* EpubParseContainerXml(const char* xml, size_t len);
bool EpubParsePackage(EpubPackage* pkg, const char* opfPath, const char* xml, size_t len);
bool EpubParseSmil(EpubPackage* pkg, EpubMoDocument* doc, const char* xml, size_t len);

// Finds a rule whose selector uses .className and returns its background colour (or, failing
// that, its text colour) as 0x00RRGGBB. Supports #rgb, #rrggbb, rgb(), rgba() and basic names.
bool EpubCssFindClassColor(const char* css, size_t len, const char* className, u32* rgbOut, bool* isBackgroundOut);

// One line per par in reading order, then diagnostics. Only includes parsed overlays.
void EpubMoDumpTimeline(EpubPackage* pkg, StrBuilder& out);
