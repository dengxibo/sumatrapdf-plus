// this file is compiled as part of mupdf library and ends up
// in libmupdf.dll, to avoid issues related to crossing .dll boundaries
// It implements loading of Fonts included in windows
#include "mupdf/fitz.h"
#include "mupdf/ucdn.h"
#include "mupdf/pdf.h"

#ifdef _WIN32

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <assert.h>

typedef uint32_t u32;

// TODO: Use more of FreeType for TTF parsing (for performance reasons,
//       the fonts can't be parsed completely, though)
#include <ft2build.h>
#include FT_TRUETYPE_IDS_H
#include FT_TRUETYPE_TAGS_H

#define TTC_VERSION1 0x00010000
#define TTC_VERSION2 0x00020000

#define MAX_FACENAME 256

#define GROW_BY 128

typedef struct {
    const char* file_path;
    void* data;
    size_t size;
} font_file;

typedef struct {
    const char* fontface;
    u32 index;
    u32 file_idx;
} win_font_info;

typedef struct {
    win_font_info* fontmap;
    int len;
    int cap;
} win_fonts;

typedef struct {
    font_file* files;
    int len;
    int cap;
} font_files;

font_files g_font_files;

typedef struct {
    ULONG uVersion;
    USHORT uNumOfTables;
    USHORT uSearchRange;
    USHORT uEntrySelector;
    USHORT uRangeShift;
} TT_OFFSET_TABLE;

typedef struct {
    ULONG uTag;      // table name
    ULONG uCheckSum; // Check sum
    ULONG uOffset;   // Offset from beginning of file
    ULONG uLength;   // length of the table in bytes
} TT_TABLE_DIRECTORY;

typedef struct {
    USHORT uFSelector;     // format selector. Always 0
    USHORT uNRCount;       // Name Records count
    USHORT uStorageOffset; // Offset for strings storage, from start of the table
} TT_NAME_TABLE_HEADER;

typedef struct {
    USHORT uPlatformID;
    USHORT uEncodingID;
    USHORT uLanguageID;
    USHORT uNameID;
    USHORT uStringLength;
    USHORT uStringOffset; // from start of storage area
} TT_NAME_RECORD;

typedef struct {
    ULONG Tag;
    ULONG Version;
    ULONG NumFonts;
} FONT_COLLECTION;

static struct {
    const char* name;
    const char* pattern;
} baseSubstitutes[] = {
    {"Courier", "CourierNewPSMT"},
    {"Courier-Bold", "CourierNewPS-BoldMT"},
    {"Courier-Oblique", "CourierNewPS-ItalicMT"},
    {"Courier-BoldOblique", "CourierNewPS-BoldItalicMT"},
    {"Helvetica", "ArialMT"},
    {"Helvetica-Bold", "Arial-BoldMT"},
    {"Helvetica-Oblique", "Arial-ItalicMT"},
    {"Helvetica-BoldOblique", "Arial-BoldItalicMT"},
    {"Times-Roman", "TimesNewRomanPSMT"},
    {"Times-Bold", "TimesNewRomanPS-BoldMT"},
    {"Times-Italic", "TimesNewRomanPS-ItalicMT"},
    {"Times-BoldItalic", "TimesNewRomanPS-BoldItalicMT"},
    {"Symbol", "SymbolMT"},
    // CSS generic font families used in EPUB files
    {"serif", "TimesNewRomanPSMT"},
    {"sans-serif", "ArialMT"},
    {"sans", "ArialMT"},
    {"monospace", "CourierNewPSMT"},
    {"cursive", "ComicSansMS"},
    {"fantasy", "Impact"},
    // fallbacks for fonts commonly used in EPUBs that may not be installed
    {"DroidSansMono", "Consolas"},
    {"SourceSansPro", "SegoeUI"},
    {"SourceSansPro-Bold", "SegoeUI-Bold"},
    {"SourceSansPro-Italic", "SegoeUI-Italic"},
    {"SourceSansPro-BoldItalic", "SegoeUI-BoldItalic"},
    {"SourceSansPro-Light", "SegoeUI-Light"},
    {"SourceSansPro-Semibold", "SegoeUI-Semibold"},
    {"HelveticaNeue", "ArialMT"},
    {"HelveticaNeue-Bold", "Arial-BoldMT"},
    {"HelveticaNeue-Italic", "Arial-ItalicMT"},
    {"HelveticaNeue-BoldItalic", "Arial-BoldItalicMT"},
    {"HelveticaNeue-Light", "ArialMT"},
    {"HelveticaNeueLight", "ArialMT"},
    {"LucidaSans", "SegoeUI"},
    {"LucidaGrande", "SegoeUI"},
    {"LucidaConsole", "Consolas"},
    {"LucidaBright", "Georgia"},
    {"Handwriting", "SegoeScript"},
    {"Console", "Consolas"},
    {"CourierNew", "CourierNewPSMT"},
    {"SegoeUI", "SegoeUI"},
    {"FuturaStd-Bold", "SegoeUI-Bold"},
    {"DINPro", "SegoeUI"},
    {"ACaslonPro-Regular", "Georgia"},
    {"ACaslonPro-Italic", "Georgia-Italic"},
    {"Literata", "Georgia"},
    {"Literata-Regular", "Georgia"},
    {"Literata-Italic", "Georgia-Italic"},
    {"Literata-Bold", "Georgia-Bold"},
    {"Literata-BoldItalic", "Georgia-BoldItalic"},
    // Liberation Sans (Linux metrically compatible with Arial) fallbacks
    {"LiberationSans", "ArialMT"},
    {"LiberationSans-Bold", "Arial-BoldMT"},
    {"LiberationSans-Italic", "Arial-ItalicMT"},
    {"LiberationSans-BoldItalic", "Arial-BoldItalicMT"},
    // Safari placeholder font
    {"SafariFakeFont", "ArialMT"},
    // PingFang SC (macOS/iOS Simplified Chinese) fallbacks
    {"PingFangSC-Regular", "Microsoft YaHei"},
    {"PingFangSC-Medium", "Microsoft YaHei"},
    {"PingFangSC-Semibold", "Microsoft YaHei Bold"},
    {"PingFangSC-Bold", "Microsoft YaHei Bold"},
    {"PingFangSC-Light", "Microsoft YaHei Light"},
    {"PingFangSC-Ultralight", "Microsoft YaHei Light"},
    {"PingFangSC-Thin", "Microsoft YaHei Light"},
    {"PingFang SC", "Microsoft YaHei"},
    // Founder FangSong font fallback
    {"FZFangSong-Z02", "FangSong"},
    {"FZFangSong-Z02S", "FangSong"},
    // Chinese Kai (regular script) font fallbacks — map publisher/device names to Windows KaiTi
    {"MKai PRC", "KaiTi"},
    {"MKaiPRC-Regular", "KaiTi"},
    {"MKaiPRC", "KaiTi"},
    {"STKaiti", "KaiTi"},
    {"STKaiti-Regular", "KaiTi"},
    {"STKai", "KaiTi"},
    {"STKai-Regular", "KaiTi"},
    {"Kai", "KaiTi"},
    {"Kaiti_GB2312", "KaiTi"},
    {"Kaiti SC", "KaiTi"},
    {"Kaiti TC", "KaiTi"},
    {"KaiTi", "KaiTi"},
    {"楷体", "KaiTi"},
};

static win_fonts g_win_fonts = {0};

static char g_sumatra_cjk_family[128] = "Source Han Serif SC";
static char g_sumatra_cjk_file[MAX_PATH] = "";

static void clear_font_load_failures(void);

void sumatra_set_ebook_font_config(const char* cjk_family, const char* cjk_file) {
    if (cjk_family && cjk_family[0]) {
        fz_strlcpy(g_sumatra_cjk_family, cjk_family, sizeof g_sumatra_cjk_family);
    } else {
        fz_strlcpy(g_sumatra_cjk_family, "Source Han Serif SC", sizeof g_sumatra_cjk_family);
    }
    g_sumatra_cjk_file[0] = '\0';
    if (cjk_file && cjk_file[0]) {
        int has_sep = strchr(cjk_file, '/') != NULL || strchr(cjk_file, '\\') != NULL;
        if (!has_sep && strstr(cjk_file, "..") == NULL) {
            fz_strlcpy(g_sumatra_cjk_file, cjk_file, sizeof g_sumatra_cjk_file);
        }
    }
    clear_font_load_failures();
}

static int did_init = 0;
static CRITICAL_SECTION cs_fonts;

// cache of font names that failed to load, to avoid repeated lookups
// names are stored 0-separated in gFontsFailedToLoad
static char gFontsFailedToLoad[256];
static int gFontsFailedToLoadLen = 0;

static int is_font_failed(const char* name) {
    int nameLen = (int)strlen(name);
    int pos = 0;
    while (pos < gFontsFailedToLoadLen) {
        const char* entry = gFontsFailedToLoad + pos;
        int entryLen = (int)strlen(entry);
        if (entryLen == nameLen && memcmp(entry, name, nameLen) == 0) {
            return 1;
        }
        pos += entryLen + 1;
    }
    return 0;
}

static void add_font_failed(const char* name) {
    int n = (int)strlen(name) + 1;
    if (gFontsFailedToLoadLen + n > (int)sizeof(gFontsFailedToLoad)) {
        return; // buffer full, just skip
    }
    memcpy(gFontsFailedToLoad + gFontsFailedToLoadLen, name, n);
    gFontsFailedToLoadLen += n;
}

static void clear_font_load_failures(void) {
    gFontsFailedToLoadLen = 0;
    gFontsFailedToLoad[0] = '\0';
}

static int streq(const char* s1, const char* s2) {
    if (strcmp(s1, s2) == 0) {
        return 1;
    }
    return 0;
}

static int streqi(const char* s1, const char* s2) {
    if (_stricmp(s1, s2) == 0) {
        return 1;
    }
    return 0;
}

static void normalize_font_name_key(const char* src, char* dst, size_t dstcap);
static int is_source_han_serif_sc_request(const char* fontname);
static int is_reader_cjk_font_request(const char* fontname);
static int is_publisher_cjk_font_request(const char* fontname);
static fz_font* load_bundled_source_han_serif(fz_context* ctx, const char* display_name, int ordering);
static fz_font* load_reader_cjk_serif(fz_context* ctx, const char* display_name, int ordering);

static inline USHORT BEtoHs(USHORT x) {
    BYTE* data = (BYTE*)&x;
    return (data[0] << 8) | data[1];
}

static inline ULONG BEtoHl(ULONG x) {
    BYTE* data = (BYTE*)&x;
    return (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3];
}

/* A little bit more sophisticated name matching so that e.g. "EurostileExtended"
    matches "EurostileExtended-Roman" or "Tahoma-Bold,Bold" matches "Tahoma-Bold" */
static int cmp_font_name(const char* name1, const char* name2) {
    int len1 = strlen(name1);
    int len2 = strlen(name2);

    if (len1 != len2) {
        const char* rest = len1 > len2 ? name1 + len2 : name2 + len1;
        if (',' == *rest || streqi(rest, "-roman")) return _strnicmp(name1, name2, fz_mini(len1, len2));
    }

    return _stricmp(name1, name2);
}

static int font_name_eq(const char* name1, const char* name2) {
    return cmp_font_name(name1, name2) == 0;
}

static int cmp_win_font_info(const void* el1, const void* el2) {
    win_font_info* i1 = (win_font_info*)el1;
    win_font_info* i2 = (win_font_info*)el2;
    if (!i1->fontface) {
        return i2->fontface ? -1 : 0;
    }
    if (!i2->fontface) {
        return 1;
    }
    return cmp_font_name(i1->fontface, i2->fontface);
}

static win_font_info* pdf_find_windows_font_path(const char* fontname) {
    win_font_info* map = &(g_win_fonts.fontmap[0]);
    size_t n = (size_t)g_win_fonts.len;
    size_t elSize = sizeof(win_font_info);
    win_font_info el;
    el.fontface = fontname;
    el.index = 0;
    win_font_info* res = (win_font_info*)bsearch(&el, map, n, elSize, cmp_win_font_info);
    return res;
}

/* source and dest can be same */
static void decode_unicode_BE(fz_context* ctx, char* source, int sourcelen, char* dest, int destlen) {
    WCHAR* tmp;
    int converted, i;

    if (sourcelen % 2 != 0) fz_throw(ctx, FZ_ERROR_GENERIC, "fonterror : invalid unicode string");

    tmp = fz_malloc_array(ctx, sourcelen / 2 + 1, WCHAR);
    for (i = 0; i < sourcelen / 2; i++) tmp[i] = BEtoHs(((WCHAR*)source)[i]);
    tmp[sourcelen / 2] = '\0';

    converted = WideCharToMultiByte(CP_UTF8, 0, tmp, -1, dest, destlen, NULL, NULL);
    fz_free(ctx, tmp);
    if (!converted) fz_throw(ctx, FZ_ERROR_GENERIC, "fonterror : invalid unicode string");
}

static void decode_platform_string(fz_context* ctx, int platform, int enctype, char* source, int sourcelen, char* dest,
                                   int destlen) {
    switch (platform) {
        case TT_PLATFORM_APPLE_UNICODE:
            switch (enctype) {
                case TT_APPLE_ID_DEFAULT:
                case TT_APPLE_ID_UNICODE_2_0:
                    decode_unicode_BE(ctx, source, sourcelen, dest, destlen);
                    return;
            }
            fz_throw(ctx, FZ_ERROR_GENERIC, "fonterror : unsupported encoding (%d/%d)", platform, enctype);
        case TT_PLATFORM_MACINTOSH:
            switch (enctype) {
                case TT_MAC_ID_ROMAN:
                    if (sourcelen + 1 > destlen)
                        fz_throw(ctx, FZ_ERROR_GENERIC, "fonterror : overlong fontname: %s", source);
                    // TODO: Convert to UTF-8 from what encoding?
                    memcpy(dest, source, sourcelen);
                    dest[sourcelen] = 0;
                    return;
            }
            fz_throw(ctx, FZ_ERROR_GENERIC, "fonterror : unsupported encoding (%d/%d)", platform, enctype);
        case TT_PLATFORM_MICROSOFT:
            switch (enctype) {
                case TT_MS_ID_SYMBOL_CS:
                case TT_MS_ID_UNICODE_CS:
                case TT_MS_ID_UCS_4:
                    decode_unicode_BE(ctx, source, sourcelen, dest, destlen);
                    return;
            }
            fz_throw(ctx, FZ_ERROR_GENERIC, "fonterror : unsupported encoding (%d/%d)", platform, enctype);
        default:
            fz_throw(ctx, FZ_ERROR_GENERIC, "fonterror : unsupported encoding (%d/%d)", platform, enctype);
    }
}

// on my machine it's ~21k for facename and path
static int g_font_allocated = 0;

static int get_font_file(const char* file_path) {
    int i;
    font_file* ff;
    for (i = 0; i < g_font_files.len; i++) {
        ff = &(g_font_files.files[i]);
        if (streq(file_path, ff->file_path)) {
            return i;
        }
    }
    return -1;
}
static int get_or_append_font_file(const char* file_path) {
    font_file* ff;
    int i = get_font_file(file_path);
    if (i >= 0) {
        return i;
    }
    if (g_font_files.len >= g_font_files.cap) {
        int newCap = g_font_files.cap + GROW_BY;
        font_file* newFiles = (font_file*)realloc(g_font_files.files, newCap * sizeof(font_file));
        if (!newFiles) {
            return -1;
        }
        memset(newFiles + g_font_files.cap, 0, GROW_BY * sizeof(font_file));
        g_font_files.files = newFiles;
        g_font_files.cap = newCap;
    }
    i = g_font_files.len;
    ff = &g_font_files.files[i];
    g_font_allocated += strlen(file_path) + 1;
    ff->file_path = strdup(file_path);
    ff->data = NULL;
    ff->size = 0;
    g_font_files.len++;
    return i;
}

static void append_mapping(fz_context* ctx, const char* facename, const char* path, int index) {
    win_fonts* fl = &g_win_fonts;
    int file_idx = get_or_append_font_file(path);
    if (file_idx < 0) {
        return;
    }
    if (fl->len >= fl->cap) {
        int newCap = fl->cap + GROW_BY;
        win_font_info* newMap = (win_font_info*)realloc(fl->fontmap, newCap * sizeof(win_font_info));
        if (!newMap) {
            return;
        }
        memset(newMap + fl->cap, 0, GROW_BY * sizeof(win_font_info));
        fl->fontmap = newMap;
        fl->cap = newCap;
    }

    win_font_info* i = &fl->fontmap[fl->len];
    g_font_allocated += strlen(facename) + 1;
    // TODO: allocate facename and path from a pool allocator
    i->fontface = strdup(facename);
    if (!i->fontface) {
        return;
    }
    i->file_idx = (u32)file_idx;
    i->index = (u32)index;
    fl->len++;
}

static void safe_read(fz_context* ctx, fz_stream* file, int offset, char* buf, int size) {
    int n;
    fz_seek(ctx, file, offset, SEEK_SET);
    n = fz_read(ctx, file, (unsigned char*)buf, size);
    if (n != size) fz_throw(ctx, FZ_ERROR_GENERIC, "safe_read: read %d, expected %d", n, size);
}

static void read_ttf_string(fz_context* ctx, fz_stream* file, int offset, TT_NAME_RECORD* ttRecordBE, char* buf,
                            int size) {
    char szTemp[MAX_FACENAME * 2];
    // ignore empty and overlong strings
    int stringLength = BEtoHs(ttRecordBE->uStringLength);
    if (stringLength == 0 || stringLength >= sizeof(szTemp)) return;

    safe_read(ctx, file, offset + BEtoHs(ttRecordBE->uStringOffset), szTemp, stringLength);
    decode_platform_string(ctx, BEtoHs(ttRecordBE->uPlatformID), BEtoHs(ttRecordBE->uEncodingID), szTemp, stringLength,
                           buf, size);
}

static void remove_spaces(char* srcDest) {
    char* dest;
    for (dest = srcDest; *srcDest; srcDest++) {
        if (*srcDest != ' ') {
            *dest++ = *srcDest;
        }
    }
    *dest = '\0';
}

static void makeFakePSName(char szName[MAX_FACENAME], const char* szStyle) {
    // append the font's subfamily, unless it's a Regular font
    if (*szStyle && !streqi(szStyle, "Regular")) {
        fz_strlcat(szName, "-", MAX_FACENAME);
        fz_strlcat(szName, szStyle, MAX_FACENAME);
    }
    remove_spaces(szName);
}

static void parseTTF(fz_context* ctx, fz_stream* file, int offset, int index, const char* path) {
    TT_OFFSET_TABLE ttOffsetTableBE;
    TT_TABLE_DIRECTORY tblDirBE;
    TT_NAME_TABLE_HEADER ttNTHeaderBE;
    TT_NAME_RECORD ttRecordBE;

    char szPSName[MAX_FACENAME] = {0};
    char szTTName[MAX_FACENAME] = {0};
    char szStyle[MAX_FACENAME] = {0};
    char szCJKName[MAX_FACENAME] = {0};
    int i, count, tblOffset;

    safe_read(ctx, file, offset, (char*)&ttOffsetTableBE, sizeof(TT_OFFSET_TABLE));

    // check if this is a TrueType font of version 1.0 or an OpenType font
    if (BEtoHl(ttOffsetTableBE.uVersion) != TTC_VERSION1 && BEtoHl(ttOffsetTableBE.uVersion) != TTAG_OTTO) {
        fz_throw(ctx, FZ_ERROR_GENERIC, "fonterror : invalid font '%s', invalid version %x", path,
                 BEtoHl(ttOffsetTableBE.uVersion));
    }

    // determine the name table's offset by iterating through the offset table
    count = BEtoHs(ttOffsetTableBE.uNumOfTables);
    for (i = 0; i < count; i++) {
        int entryOffset = offset + sizeof(TT_OFFSET_TABLE) + i * sizeof(TT_TABLE_DIRECTORY);
        safe_read(ctx, file, entryOffset, (char*)&tblDirBE, sizeof(TT_TABLE_DIRECTORY));
        if (!BEtoHl(tblDirBE.uTag) || BEtoHl(tblDirBE.uTag) == TTAG_name) {
            break;
        }
    }
    if (count == i || !BEtoHl(tblDirBE.uTag)) {
        fz_throw(ctx, FZ_ERROR_GENERIC, "fonterror : nameless font");
    }
    tblOffset = BEtoHl(tblDirBE.uOffset);

    // read the 'name' table for record count and offsets
    safe_read(ctx, file, tblOffset, (char*)&ttNTHeaderBE, sizeof(TT_NAME_TABLE_HEADER));
    offset = tblOffset + sizeof(TT_NAME_TABLE_HEADER);
    tblOffset += BEtoHs(ttNTHeaderBE.uStorageOffset);

    // read through the strings for PostScript name and font family
    count = BEtoHs(ttNTHeaderBE.uNRCount);
    for (i = 0; i < count; i++) {
        short langId, nameId;
        BOOL isCJKName;

        safe_read(ctx, file, offset + i * sizeof(TT_NAME_RECORD), (char*)&ttRecordBE, sizeof(TT_NAME_RECORD));

        langId = BEtoHs(ttRecordBE.uLanguageID);
        nameId = BEtoHs(ttRecordBE.uNameID);
        isCJKName = TT_NAME_ID_FONT_FAMILY == nameId && LANG_CHINESE == PRIMARYLANGID(langId);

        // ignore non-English strings (except for Chinese font names)
        if (langId && langId != TT_MS_LANGID_ENGLISH_UNITED_STATES && !isCJKName) {
            continue;
        }
        // ignore names other than font (sub)family and PostScript name
        fz_try(ctx) {
            if (isCJKName) {
                read_ttf_string(ctx, file, tblOffset, &ttRecordBE, szCJKName, sizeof(szCJKName));
            } else if (TT_NAME_ID_FONT_FAMILY == nameId) {
                read_ttf_string(ctx, file, tblOffset, &ttRecordBE, szTTName, sizeof(szTTName));
            } else if (TT_NAME_ID_FONT_SUBFAMILY == nameId) {
                read_ttf_string(ctx, file, tblOffset, &ttRecordBE, szStyle, sizeof(szStyle));
            } else if (TT_NAME_ID_PS_NAME == nameId) {
                read_ttf_string(ctx, file, tblOffset, &ttRecordBE, szPSName, sizeof(szPSName));
            }
        }
        fz_catch(ctx) {
            fz_report_error(ctx);
            fz_warn(ctx, "ignoring face name decoding fonterror");
        }
    }

    // try to prevent non-Arial fonts from accidentally substituting Arial
    if (streq(szPSName, "ArialMT")) {
        // cf. https://code.google.com/p/sumatrapdf/issues/detail?id=2471
        if (!streq(szTTName, "Arial")) {
            szPSName[0] = '\0';
        } else if (strstr(path, "caps") || strstr(path, "Caps")) {
            // TODO: is there a better way to distinguish Arial Caps from Arial proper?
            // cf. https://code.google.com/p/sumatrapdf/issues/detail?id=1290
            fz_throw(ctx, FZ_ERROR_GENERIC, "ignore %s, as it can't be distinguished from Arial,Regular", path);
        }
    }

    if (szPSName[0]) {
        append_mapping(ctx, szPSName, path, index);
    }
    if (szTTName[0]) {
        // derive a PostScript-like name and add it, if it's different from the font's
        // included PostScript name; cf. https://code.google.com/p/sumatrapdf/issues/detail?id=376
        // compare the two names before adding this one
        if (!font_name_eq(szTTName, szPSName)) {
            append_mapping(ctx, szTTName, path, index);
        }
    }
    if (szCJKName[0]) {
        makeFakePSName(szCJKName, szStyle);
        if (!font_name_eq(szCJKName, szPSName) && !font_name_eq(szCJKName, szTTName)) {
            append_mapping(ctx, szCJKName, path, index);
        }
    }
}

static void parseTTFs(fz_context* ctx, const char* path) {
    fz_stream* file = 0;
    fz_try(ctx) {
        file = fz_open_file(ctx, path);
        parseTTF(ctx, file, 0, 0, path);
    }
    fz_always(ctx) {
        fz_drop_stream(ctx, file);
    }
    fz_catch(ctx) {
        fz_rethrow(ctx);
    }
}

static void parseTTCs(fz_context* ctx, const char* path) {
    FONT_COLLECTION fontcollectionBE;
    ULONG i, numFonts, *offsettableBE = NULL;

    fz_stream* file = fz_open_file(ctx, path);

    fz_var(offsettableBE);

    fz_try(ctx) {
        safe_read(ctx, file, 0, (char*)&fontcollectionBE, sizeof(FONT_COLLECTION));
        if (BEtoHl(fontcollectionBE.Tag) != TTAG_ttcf) {
            fz_throw(ctx, FZ_ERROR_GENERIC, "fonterror : wrong format %x", BEtoHl(fontcollectionBE.Tag));
        }
        if (BEtoHl(fontcollectionBE.Version) != TTC_VERSION1 && BEtoHl(fontcollectionBE.Version) != TTC_VERSION2) {
            fz_throw(ctx, FZ_ERROR_GENERIC, "fonterror : invalid version %x", BEtoHl(fontcollectionBE.Version));
        }

        numFonts = BEtoHl(fontcollectionBE.NumFonts);
        offsettableBE = fz_malloc_array(ctx, numFonts, ULONG);

        int offset = (int)sizeof(FONT_COLLECTION);
        safe_read(ctx, file, offset, (char*)offsettableBE, numFonts * sizeof(ULONG));
        for (i = 0; i < numFonts; i++) {
            parseTTF(ctx, file, BEtoHl(offsettableBE[i]), i, path);
        }
    }
    fz_always(ctx) {
        fz_free(ctx, offsettableBE);
        fz_drop_stream(ctx, file);
    }
    fz_catch(ctx) {
        fz_rethrow(ctx);
    }
}

static int get_exe_dir_w(WCHAR* dir, size_t dircap);

static void extend_system_font_list(fz_context* ctx, const WCHAR* path) {
    WCHAR szPath[MAX_PATH], *lpFileName;
    WIN32_FIND_DATA FileData;
    HANDLE hList;

    GetFullPathNameW(path, nelem(szPath), szPath, &lpFileName);

    hList = FindFirstFile(szPath, &FileData);
    if (hList == INVALID_HANDLE_VALUE) {
        // Don't complain about missing directories
        if (GetLastError() == ERROR_FILE_NOT_FOUND) {
            return;
        }
        fz_throw(ctx, FZ_ERROR_GENERIC, "extend_system_font_list: unknown error %d", GetLastError());
    }
    do {
        if (!(FileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            if (wcsstr(FileData.cFileName, L"VariableFont")) {
                continue;
            }
            char szPathUtf8[MAX_PATH], *fileExt;
            int res;
            lstrcpynW(lpFileName, FileData.cFileName, szPath + MAX_PATH - lpFileName);
            res = WideCharToMultiByte(CP_UTF8, 0, szPath, -1, szPathUtf8, sizeof(szPathUtf8), NULL, NULL);
            if (!res) {
                fz_warn(ctx, "WideCharToMultiByte failed");
                continue;
            }
            fileExt = szPathUtf8 + strlen(szPathUtf8) - 4;
            fz_try(ctx) {
                if (streqi(fileExt, ".ttc")) {
                    parseTTCs(ctx, szPathUtf8);
                } else if (streqi(fileExt, ".ttf") || streqi(fileExt, ".otf")) {
                    parseTTFs(ctx, szPathUtf8);
                }
            }
            fz_catch(ctx) {
                fz_report_error(ctx);
                // ignore errors occurring while parsing a given font file
            }
        }
    } while (FindNextFile(hList, &FileData));
    FindClose(hList);
}

// cf. https://blogs.msdn.com/b/oldnewthing/archive/2004/10/25/247180.aspx
EXTERN_C IMAGE_DOS_HEADER __ImageBase;
#define CURRENT_HMODULE ((HMODULE) & __ImageBase)

// clang-cl notices the mismatch in function parameters with qsort
// as _stricmp is int _stricmp(const char *string1, const char *string2);
// and qsort expects int (*compar)(const void*,const void*)).
static int stricmp_wrapper(const void* ptr1, const void* ptr2) {
    const char* string1 = (const char*)ptr1;
    const char* string2 = (const char*)ptr2;
    return _stricmp(string1, string2);
}

static int font_face_already_mapped(const char* facename) {
    int i;
    if (!facename || !facename[0]) {
        return 0;
    }
    for (i = 0; i < g_win_fonts.len; i++) {
        if (font_name_eq(g_win_fonts.fontmap[i].fontface, facename)) {
            return 1;
        }
    }
    return 0;
}

static void append_mapping_if_new(fz_context* ctx, const char* facename, const char* path, int index) {
    if (!font_face_already_mapped(facename)) {
        append_mapping(ctx, facename, path, index);
    }
}

static void strip_registry_font_suffix(WCHAR* name) {
    WCHAR* p;
    if (!name) {
        return;
    }
    p = wcsstr(name, L" (");
    if (p) {
        *p = L'\0';
    }
}

static int resolve_registry_font_file_w(const WCHAR* regFile, WCHAR* out, size_t outcap) {
    if (!regFile || !regFile[0] || !out || outcap < 2) {
        return 0;
    }
    if ((regFile[1] == L':' && regFile[2] == L'\\') || (regFile[0] == L'\\' && regFile[1] == L'\\')) {
        if (wcsncpy_s(out, outcap, regFile, _TRUNCATE) != 0) {
            return 0;
        }
    } else {
        WCHAR winDir[MAX_PATH];
        if (!GetWindowsDirectoryW(winDir, (UINT)nelem(winDir))) {
            return 0;
        }
        if (swprintf(out, outcap, L"%s\\Fonts\\%s", winDir, regFile) < 0) {
            return 0;
        }
    }
    return GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES;
}

static void parse_font_file_if_needed(fz_context* ctx, const char* pathUtf8) {
    size_t len;
    char* fileExt;

    if (!pathUtf8 || !pathUtf8[0]) {
        return;
    }
    if (get_font_file(pathUtf8) >= 0) {
        return;
    }
    len = strlen(pathUtf8);
    if (len < 4) {
        return;
    }
    fileExt = (char*)pathUtf8 + len - 4;
    fz_try(ctx) {
        if (streqi(fileExt, ".ttc")) {
            parseTTCs(ctx, pathUtf8);
        } else if (streqi(fileExt, ".ttf") || streqi(fileExt, ".otf")) {
            parseTTFs(ctx, pathUtf8);
        }
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
    }
}

static void extend_fonts_from_registry_hive(fz_context* ctx, HKEY hive, const WCHAR* subkey) {
    HKEY hKey;
    DWORD i;

    if (RegOpenKeyExW(hive, subkey, 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return;
    }

    for (i = 0;; i++) {
        WCHAR valueName[512];
        WCHAR valueData[MAX_PATH * 2];
        WCHAR pathW[MAX_PATH * 2];
        char pathUtf8[MAX_PATH * 3];
        char displayUtf8[MAX_FACENAME];
        DWORD nameLen = (DWORD)nelem(valueName);
        DWORD dataLen = sizeof(valueData);
        DWORD type;
        LONG rc;

        rc = RegEnumValueW(hKey, i, valueName, &nameLen, NULL, &type, (LPBYTE)valueData, &dataLen);
        if (rc == ERROR_NO_MORE_ITEMS) {
            break;
        }
        if (rc != ERROR_SUCCESS || type != REG_SZ) {
            continue;
        }
        if (!resolve_registry_font_file_w(valueData, pathW, nelem(pathW))) {
            continue;
        }
        if (!WideCharToMultiByte(CP_UTF8, 0, pathW, -1, pathUtf8, (int)sizeof pathUtf8, NULL, NULL)) {
            continue;
        }
        parse_font_file_if_needed(ctx, pathUtf8);

        strip_registry_font_suffix(valueName);
        if (!WideCharToMultiByte(CP_UTF8, 0, valueName, -1, displayUtf8, (int)sizeof displayUtf8, NULL, NULL)) {
            continue;
        }
        remove_spaces(displayUtf8);
        append_mapping_if_new(ctx, displayUtf8, pathUtf8, 0);
    }

    RegCloseKey(hKey);
}

/* GDI lists fonts installed outside %WINDIR%\\Fonts (e.g. Kingsoft/WPS muifont paths). */
static void extend_fonts_from_registry(fz_context* ctx) {
    static const WCHAR* subkey = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Fonts";
    extend_fonts_from_registry_hive(ctx, HKEY_LOCAL_MACHINE, subkey);
    extend_fonts_from_registry_hive(ctx, HKEY_CURRENT_USER, subkey);
}

static void create_system_font_list(fz_context* ctx) {
    WCHAR szFontDir[MAX_PATH];
    UINT cch;

    cch = GetWindowsDirectory(szFontDir, nelem(szFontDir) - 12);
    if (0 < cch && cch < nelem(szFontDir) - 12) {
        wcscat_s(szFontDir, MAX_PATH, L"\\Fonts\\*.?t?");
        extend_system_font_list(ctx, szFontDir);
    }

    if (g_win_fonts.len == 0) {
        fz_warn(ctx, "couldn't find any usable system fonts");
    }

#ifdef NOCJKFONT
    {
        // If no CJK fallback font is builtin but one has been shipped separately (in the same
        // directory as the main executable), add it to the list of loadable system fonts
        WCHAR szFile[MAX_PATH], *lpFileName;
        szFile[0] = '\0';
        GetModuleFileName(CURRENT_HMODULE, szFontDir, MAX_PATH);
        szFontDir[nelem(szFontDir) - 1] = '\0';
        GetFullPathNameW(szFontDir, MAX_PATH, szFile, &lpFileName);
        lstrcpyn(lpFileName, L"DroidSansFallback.ttf", szFile + MAX_PATH - lpFileName);
        extend_system_font_list(ctx, szFile);
    }
#endif

    {
        WCHAR exeDir[MAX_PATH];
        WCHAR fontPattern[MAX_PATH];
        if (get_exe_dir_w(exeDir, nelem(exeDir)) &&
            swprintf(fontPattern, nelem(fontPattern), L"%s\\fonts\\*.?t?", exeDir) > 0) {
            fz_try(ctx) {
                extend_system_font_list(ctx, fontPattern);
            }
            fz_catch(ctx) {
                fz_report_error(ctx);
            }
        }
    }

    fz_try(ctx) {
        extend_fonts_from_registry(ctx);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
    }

    // sort the font list, so that it can be searched binarily
    void* map = (void*)&(g_win_fonts.fontmap[0]);
    size_t n = (size_t)g_win_fonts.len;
    size_t elSize = sizeof(win_font_info);
    qsort(map, n, elSize, cmp_win_font_info);

#ifdef DEBUG
    // allow to overwrite system fonts for debugging purposes
    // (either pass a full path or a search pattern such as "fonts\*.ttf")
    cch = GetEnvironmentVariable(L"MUPDF_FONTS_PATTERN", szFontDir, nelem(szFontDir));
    if (0 < cch && cch < nelem(szFontDir)) {
        int i, prev_len = g_win_fonts.len;
        extend_system_font_list(ctx, szFontDir);
        for (i = prev_len; i < g_win_fonts.len; i++) {
            win_font_info* entry = bsearch(g_win_fonts.fontmap[i].fontface, g_win_fonts.fontmap, prev_len,
                                           sizeof(win_font_info), cmp_win_font_info);
            if (entry) {
                *entry = g_win_fonts.fontmap[i];
            }
        }
        void* map = (void*)&(g_win_fonts.fontmap[0]);
        size_t n = (size_t)g_win_fonts.len;
        size_t elSize = sizeof(win_font_info);
        qsort(map, n, elSize, cmp_win_font_info);
    }
#endif
}

// TODO(port): replace the caller
static void* fz_resize_array(fz_context* ctx, void* p, unsigned int count, unsigned int size) {
    void* np = fz_realloc(ctx, p, count * size);
    if (!np) fz_throw(ctx, FZ_ERROR_GENERIC, "resize array (%d x %d bytes) failed", count, size);
    return np;
}

static fz_buffer* load_and_cache_font(fz_context* ctx, win_font_info* fi, const char* font_name) {
    fz_buffer* buffer = NULL;
    int file_idx = (int)fi->file_idx;
    font_file* ff;

    EnterCriticalSection(&cs_fonts);
    ff = &g_font_files.files[file_idx];
    if (ff->data) {
        buffer = fz_new_buffer_from_shared_data(ctx, ff->data, ff->size);
        // fz_warn(ctx, "found cached font '%s' from '%s'", font_name, ff->file_path);
    }
    LeaveCriticalSection(&cs_fonts);
    if (buffer) {
        return buffer;
    }

    // can fz_throw so load outside of cs
    buffer = fz_read_file(ctx, ff->file_path);
    if (!buffer) {
        return NULL;
    }

    EnterCriticalSection(&cs_fonts);
    // ff->data is freed in destroy_system_font_list()
    ff->size = fz_buffer_extract(ctx, buffer, (unsigned char**)&ff->data);
    fz_drop_buffer(ctx, buffer);
    buffer = fz_new_buffer_from_shared_data(ctx, ff->data, ff->size);
    LeaveCriticalSection(&cs_fonts);
    // fz_warn(ctx, "loaded font '%s' from '%s'", font_name, ff->file_path);
    return buffer;
}

static int str_ends_with(const char* str, const char* end) {
    size_t len1 = strlen(str);
    size_t len2 = strlen(end);
    return len1 >= len2 && streq(str + len1 - len2, end);
}

static fz_font* load_windows_font_by_name_impl(fz_context* ctx, const char* orig_name, int redirect_cjk) {
    win_font_info* found = NULL;
    char *comma, *fontname;
    fz_font* font = NULL;
    fz_buffer* buffer;

    if (is_font_failed(orig_name)) {
        return NULL;
    }

    if (redirect_cjk && is_reader_cjk_font_request(orig_name)) {
        fz_font* bundled = load_reader_cjk_serif(ctx, orig_name, FZ_ADOBE_GB);
        if (bundled) {
            return bundled;
        }
        /* fall through to system SimSun / 宋体 if bundled font is unavailable */
    }

    if (redirect_cjk && is_bundled_reader_cjk_setting() && is_publisher_cjk_font_request(orig_name)) {
        fz_font* user = load_reader_cjk_serif(ctx, orig_name, FZ_ADOBE_GB);
        if (user) {
            return user;
        }
    }

    EnterCriticalSection(&cs_fonts);
    if (g_win_fonts.len == 0) {
        fz_try(ctx) {
            create_system_font_list(ctx);
        }
        fz_catch(ctx) {
            fz_report_error(ctx);
        }
    }
    LeaveCriticalSection(&cs_fonts);

    if (g_win_fonts.len == 0) {
        fz_throw(ctx, FZ_ERROR_GENERIC, "fonterror: couldn't find any fonts");
    }

    // work on a normalized copy of the font name
    fontname = fz_strdup(ctx, orig_name);
    remove_spaces(fontname);

    // first, try to find the exact font name (including appended style information)
    comma = strchr(fontname, ',');
    if (comma) {
        *comma = '-';
        found = pdf_find_windows_font_path(fontname);
        if (found) {
            goto Exit;
        }
        *comma = ',';
    } else {
        // second, substitute the font name with a known PostScript name
        int i;
        for (i = 0; i < nelem(baseSubstitutes) && !found; i++)
            if (streq(fontname, baseSubstitutes[i].name)) {
                found = pdf_find_windows_font_path(baseSubstitutes[i].pattern);
                if (found) {
                    goto Exit;
                }
            }
    }
    // third, search for the font name without additional style information
    found = pdf_find_windows_font_path(fontname);
    if (found) {
        goto Exit;
    }
    // fourth, try to separate style from basename for prestyled fonts (e.g. "ArialBold")
    if (!comma && (str_ends_with(fontname, "Bold") || str_ends_with(fontname, "Italic"))) {
        int styleLen = str_ends_with(fontname, "Bold") ? 4 : str_ends_with(fontname, "BoldItalic") ? 10 : 6;
        fontname = (char*)fz_resize_array(ctx, fontname, strlen(fontname) + 2, sizeof(char));
        comma = fontname + strlen(fontname) - styleLen;
        memmove(comma + 1, comma, styleLen + 1);
        *comma = '-';
        found = pdf_find_windows_font_path(fontname);
        if (found) {
            goto Exit;
        }
        *comma = ',';
        found = pdf_find_windows_font_path(fontname);
        if (found) {
            goto Exit;
        }
    }
    // fifth, try to convert the font name from the common Chinese codepage 936
    if (fontname[0] < 0) {
        WCHAR cjkNameW[MAX_FACENAME];
        char cjkName[MAX_FACENAME];
        if (MultiByteToWideChar(936, MB_ERR_INVALID_CHARS, fontname, -1, cjkNameW, nelem(cjkNameW)) &&
            WideCharToMultiByte(CP_UTF8, 0, cjkNameW, -1, cjkName, nelem(cjkName), NULL, NULL)) {
            comma = strchr(cjkName, ',');
            if (comma) {
                *comma = '-';
                found = pdf_find_windows_font_path(cjkName);
                if (found) {
                    goto Exit;
                }
                *comma = ',';
            }
            found = pdf_find_windows_font_path(cjkName);
            if (found) {
                goto Exit;
            }
        }
    }
Exit:
    fz_free(ctx, fontname);
    if (!found) {
        fz_warn(ctx, "couldn't find system font '%s'", orig_name);
        add_font_failed(orig_name);
        return NULL;
    }
    buffer = load_and_cache_font(ctx, found, orig_name);
    int use_glyph_bbox = !streq(found->fontface, "DroidSansFallback");
    fz_try(ctx) {
        font = fz_new_font_from_buffer(ctx, orig_name, buffer, found->index, use_glyph_bbox);
        font->flags.ft_substitute = 1;
    }
    fz_always(ctx) {
        fz_drop_buffer(ctx, buffer);
    }
    fz_catch(ctx) {
        fz_rethrow(ctx);
    }
    return font;
}

static fz_font* load_windows_font_by_name(fz_context* ctx, const char* orig_name) {
    return load_windows_font_by_name_impl(ctx, orig_name, 1);
}

static int is_css_generic_family(const char* fontname) {
    return !strcmp(fontname, "serif") || !strcmp(fontname, "sans-serif") || !strcmp(fontname, "monospace");
}

static int fontname_has_style_suffix(const char* fontname) {
    return str_ends_with(fontname, "Regular") || str_ends_with(fontname, "Bold") || str_ends_with(fontname, "Italic") ||
           str_ends_with(fontname, "BoldItalic");
}

static fz_font* load_windows_font_by_name_and_style(fz_context* ctx, const char* fontname, int bold, int italic) {
    char styled[MAX_FACENAME * 2];

    if (is_css_generic_family(fontname) || fontname_has_style_suffix(fontname)) {
        return NULL;
    }

    fz_strlcpy(styled, fontname, sizeof(styled));
    if (!bold && !italic) {
        fz_strlcat(styled, "Regular", sizeof(styled));
    } else if (bold && italic) {
        fz_strlcat(styled, "BoldItalic", sizeof(styled));
    } else if (bold) {
        fz_strlcat(styled, "Bold", sizeof(styled));
    } else {
        fz_strlcat(styled, "Italic", sizeof(styled));
    }
    remove_spaces(styled);
    return load_windows_font_by_name(ctx, styled);
}

static int get_exe_dir_w(WCHAR* dir, size_t dircap);
static int try_font_path_w(fz_context* ctx, const char* display_name, const WCHAR* base, const WCHAR* suffix, int index,
                           int use_glyph_bbox, fz_font** out);

static fz_font* load_literata_font_file(fz_context* ctx, const char* fontname, int bold, int italic) {
    WCHAR exeDir[MAX_PATH];
    WCHAR pathW[MAX_PATH];
    char pathUtf8[MAX_PATH * 3];
    const WCHAR* fileName;
    const WCHAR* exeFileName;

    if (_stricmp(fontname, "Literata") != 0) {
        return NULL;
    }

    if (bold && italic) {
        fileName = L"\\Fonts\\Literata-BoldItalic.ttf";
        exeFileName = L"\\fonts\\Literata-BoldItalic.ttf";
    } else if (bold) {
        fileName = L"\\Fonts\\Literata-Bold.ttf";
        exeFileName = L"\\fonts\\Literata-Bold.ttf";
    } else if (italic) {
        fileName = L"\\Fonts\\Literata-Italic.ttf";
        exeFileName = L"\\fonts\\Literata-Italic.ttf";
    } else {
        fileName = L"\\Fonts\\Literata-Regular.ttf";
        exeFileName = L"\\fonts\\Literata-Regular.ttf";
    }

    if (get_exe_dir_w(exeDir, nelem(exeDir))) {
        fz_font* font = NULL;
        if (try_font_path_w(ctx, fontname, exeDir, exeFileName, 0, 1, &font)) {
            return font;
        }
    }

    UINT cch = GetWindowsDirectoryW(pathW, nelem(pathW) - (UINT)wcslen(fileName) - 1);
    if (cch == 0 || cch >= nelem(pathW) - wcslen(fileName) - 1) {
        return NULL;
    }
    wcscat_s(pathW, nelem(pathW), fileName);
    if (GetFileAttributesW(pathW) == INVALID_FILE_ATTRIBUTES) {
        return NULL;
    }
    if (!WideCharToMultiByte(CP_UTF8, 0, pathW, -1, pathUtf8, sizeof(pathUtf8), NULL, NULL)) {
        return NULL;
    }
    return fz_new_font_from_file(ctx, fontname, pathUtf8, 0, 1);
}

static void normalize_font_name_key(const char* src, char* dst, size_t dstcap) {
    size_t j = 0;
    if (!src || !dst || dstcap < 2) {
        return;
    }
    dst[0] = '\0';
    for (size_t i = 0; src[i] && j + 1 < dstcap; i++) {
        char c = src[i];
        if (c == ' ' || c == '-') {
            continue;
        }
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c + 32);
        }
        dst[j++] = c;
    }
    dst[j] = '\0';
}

static int is_source_han_serif_sc_request(const char* fontname) {
    if (!fontname) {
        return 0;
    }
    if (streq(fontname, "\xe6\x80\x9d\xe6\xba\x90\xe5\xae\x8b\xe4\xbd\x93")) {
        return 1; // 思源宋体
    }
    if (streq(fontname, "\xe5\xae\x8b\xe4\xbd\x93")) {
        return 1; // 宋体
    }
    char norm[128];
    norm[0] = '\0';
    normalize_font_name_key(fontname, norm, sizeof norm);
    static const char* kNames[] = {
        "sourcehanserifsc",
        "sourcehanserifscregular",
        "sourcehanserifcn",
        "sourcehanserifcnregular",
        "sourcehanserif",
        "notoserifcjksc",
        "notoserifcjkscregular",
        /* Windows / publisher songti names -> bundled Source Han Serif SC (matches MOBI/GDI+ path) */
        "simsun",
        "nsimsun",
        "stsong",
        "stsongti",
        "stsonglight",
        "songti",
        "songtisc",
        "songtitc",
        NULL,
    };
    for (int i = 0; kNames[i]; i++) {
        if (streq(norm, kNames[i])) {
            return 1;
        }
    }
    return 0;
}

static int han_ordering_for_language(int language) {
    if (language == FZ_LANG_ja) {
        return FZ_ADOBE_JAPAN;
    }
    if (language == FZ_LANG_ko) {
        return FZ_ADOBE_KOREA;
    }
    if (language == FZ_LANG_zh_Hant) {
        return FZ_ADOBE_CNS;
    }
    return FZ_ADOBE_GB;
}

static int get_exe_dir_w(WCHAR* dir, size_t dircap) {
    if (!dir || dircap < 2) {
        return 0;
    }
    if (!GetModuleFileNameW(NULL, dir, (DWORD)dircap)) {
        return 0;
    }
    WCHAR* slash = wcsrchr(dir, L'\\');
    if (!slash) {
        return 0;
    }
    *slash = L'\0';
    return 1;
}

static int wide_to_utf8(const WCHAR* w, char* dst, size_t dstcap) {
    if (!w || !dst || dstcap < 2) {
        return 0;
    }
    return WideCharToMultiByte(CP_UTF8, 0, w, -1, dst, (int)dstcap, NULL, NULL) > 0;
}

static int source_han_serif_ttc_index(int ordering) {
    switch (ordering) {
        case FZ_ADOBE_JAPAN:
            return 0;
        case FZ_ADOBE_KOREA:
            return 1;
        case FZ_ADOBE_CNS:
            return 3;
        case FZ_ADOBE_GB:
        default:
            return 2;
    }
}

static fz_font* try_source_han_serif_file(fz_context* ctx, const char* display_name, const char* path, int ttc_index,
                                          int ordering) {
    fz_font* font = NULL;

    fz_var(font);
    fz_try(ctx) {
        font = fz_new_font_from_file(ctx, display_name, path, ttc_index, 0);
        if (font) {
            font->flags.cjk = 1;
            font->flags.cjk_lang = ordering;
        }
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        font = NULL;
    }
    return font;
}

static fz_font* try_font_file(fz_context* ctx, const char* display_name, const char* path, int index,
                              int use_glyph_bbox) {
    fz_font* font = NULL;

    fz_var(font);
    fz_try(ctx) {
        font = fz_new_font_from_file(ctx, display_name, path, index, use_glyph_bbox);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        font = NULL;
    }
    return font;
}

static int try_font_path_w(fz_context* ctx, const char* display_name, const WCHAR* base, const WCHAR* suffix, int index,
                           int use_glyph_bbox, fz_font** out) {
    WCHAR pathW[MAX_PATH];
    char pathUtf8[MAX_PATH * 3];

    if (!base || !suffix || !out) {
        return 0;
    }
    if (lstrlenW(base) + lstrlenW(suffix) >= MAX_PATH) {
        return 0;
    }
    wcscpy_s(pathW, nelem(pathW), base);
    wcscat_s(pathW, nelem(pathW), suffix);
    if (GetFileAttributesW(pathW) == INVALID_FILE_ATTRIBUTES) {
        return 0;
    }
    if (!wide_to_utf8(pathW, pathUtf8, sizeof pathUtf8)) {
        return 0;
    }
    *out = try_font_file(ctx, display_name, pathUtf8, index, use_glyph_bbox);
    return *out != NULL;
}

static int try_source_han_serif_path_w(fz_context* ctx, const char* display_name, const WCHAR* base,
                                       const WCHAR* suffix, int ttc_index, int ordering, fz_font** out) {
    WCHAR pathW[MAX_PATH];
    char pathUtf8[MAX_PATH * 3];

    if (!base || !suffix || !out) {
        return 0;
    }
    if (lstrlenW(base) + lstrlenW(suffix) >= MAX_PATH) {
        return 0;
    }
    wcscpy_s(pathW, nelem(pathW), base);
    wcscat_s(pathW, nelem(pathW), suffix);
    if (GetFileAttributesW(pathW) == INVALID_FILE_ATTRIBUTES) {
        return 0;
    }
    if (!wide_to_utf8(pathW, pathUtf8, sizeof pathUtf8)) {
        return 0;
    }
    *out = try_source_han_serif_file(ctx, display_name, pathUtf8, ttc_index, ordering);
    return *out != NULL;
}

static fz_font* load_bundled_source_han_serif(fz_context* ctx, const char* display_name, int ordering) {
    WCHAR exeDir[MAX_PATH];
    WCHAR winDir[MAX_PATH];
    fz_font* font = NULL;
    int ttc_index = source_han_serif_ttc_index(ordering);
    static const WCHAR* kExeFontFiles[] = {
        L"\\fonts\\SourceHanSerifSC-Regular.otf",
        L"\\fonts\\SOURCEHANSERIFSC-REGULAR.OTF",
        L"\\fonts\\SourceHanSerif-Regular.ttc",
        NULL,
    };
    static const int kExeFontIndices[] = {0, 0, -1};
    static const WCHAR* kDevTtcPaths[] = {
        L"\\..\\mupdf\\resources\\fonts\\han\\SourceHanSerif-Regular.ttc",
        L"\\..\\..\\mupdf\\resources\\fonts\\han\\SourceHanSerif-Regular.ttc",
        NULL,
    };
    static const WCHAR* kWinOtfNames[] = {
        L"\\Fonts\\SourceHanSerifSC-Regular.otf",
        L"\\Fonts\\SOURCEHANSERIFSC-REGULAR.OTF",
        NULL,
    };

    if (get_exe_dir_w(exeDir, nelem(exeDir))) {
        for (int i = 0; kExeFontFiles[i]; i++) {
            int index = kExeFontIndices[i] < 0 ? ttc_index : kExeFontIndices[i];
            if (try_source_han_serif_path_w(ctx, display_name, exeDir, kExeFontFiles[i], index, ordering, &font)) {
                return font;
            }
        }
        for (int i = 0; kDevTtcPaths[i]; i++) {
            if (try_source_han_serif_path_w(ctx, display_name, exeDir, kDevTtcPaths[i], ttc_index, ordering, &font)) {
                return font;
            }
        }
    }

    if (GetWindowsDirectoryW(winDir, nelem(winDir))) {
        for (int i = 0; kWinOtfNames[i]; i++) {
            if (try_source_han_serif_path_w(ctx, display_name, winDir, kWinOtfNames[i], 0, ordering, &font)) {
                return font;
            }
        }
    }

    return NULL;
}

static int is_configured_cjk_family_request(const char* fontname) {
    char norm[128];
    char cfg[128];

    if (!fontname) {
        return 0;
    }
    if (streq(fontname, g_sumatra_cjk_family)) {
        return 1;
    }
    normalize_font_name_key(fontname, norm, sizeof norm);
    normalize_font_name_key(g_sumatra_cjk_family, cfg, sizeof cfg);
    if (cfg[0] && streq(norm, cfg)) {
        return 1;
    }
    if (streq(fontname, "\xe6\x80\x9d\xe6\xba\x90\xe5\xae\x8b\xe4\xbd\x93") &&
        streq(g_sumatra_cjk_family, "Source Han Serif SC")) {
        return 1;
    }
    return 0;
}

static int is_bundled_reader_cjk_setting(void) {
    if (g_sumatra_cjk_file[0]) {
        return 1;
    }
    if (!g_sumatra_cjk_family[0]) {
        return 1;
    }
    if (streq(g_sumatra_cjk_family, "Source Han Serif SC")) {
        return 1;
    }
    if (streq(g_sumatra_cjk_family, "LXGW WenKai")) {
        return 1;
    }
    if (strstr(g_sumatra_cjk_family, "LXGW") != NULL) {
        return 1;
    }
    if (streq(g_sumatra_cjk_family, "\xe6\x80\x9d\xe6\xba\x90\xe5\xae\x8b\xe4\xbd\x93")) {
        return 1;
    }
    return 0;
}

static int is_reader_cjk_font_request(const char* fontname) {
    if (is_configured_cjk_family_request(fontname)) {
        return 1;
    }
    if (is_bundled_reader_cjk_setting() && is_source_han_serif_sc_request(fontname)) {
        return 1;
    }
    return 0;
}

static int is_publisher_cjk_font_request(const char* fontname) {
    static const char* names[] = {"STKai",
                                  "STKaiti",
                                  "STKaiti-Regular",
                                  "STKai-Regular",
                                  "STSong",
                                  "STSongti",
                                  "STSongti-SC-Regular",
                                  "STFangsong",
                                  "STHeiti",
                                  "SimSun",
                                  "NSimSun",
                                  "KaiTi",
                                  "KaiTi_GB2312",
                                  "KaiTi SC",
                                  "Kaiti SC",
                                  "楷体",
                                  "SimHei",
                                  "FangSong",
                                  "Songti SC",
                                  "Songti TC",
                                  "MKaiPRC",
                                  "MKaiPRC-Regular",
                                  "MKai PRC",
                                  "PingFang SC",
                                  "PingFangSC-Regular",
                                  "Microsoft YaHei",
                                  "FZFangSong-Z02",
                                  "FZFangSong-Z02S",
                                  NULL};
    int i;

    if (!fontname) {
        return 0;
    }
    for (i = 0; names[i]; i++) {
        if (streq(fontname, names[i])) {
            return 1;
        }
    }
    return 0;
}

static fz_font* load_bundled_custom_cjk_font(fz_context* ctx) {
    WCHAR exeDir[MAX_PATH];
    WCHAR pathW[MAX_PATH];
    char pathUtf8[MAX_PATH * 3];
    const char* display_name;

    if (!g_sumatra_cjk_file[0]) {
        return NULL;
    }
    if (!get_exe_dir_w(exeDir, nelem(exeDir))) {
        return NULL;
    }
    if (swprintf(pathW, nelem(pathW), L"%s\\fonts\\%S", exeDir, g_sumatra_cjk_file) < 0) {
        return NULL;
    }
    if (GetFileAttributesW(pathW) == INVALID_FILE_ATTRIBUTES) {
        return NULL;
    }
    if (!wide_to_utf8(pathW, pathUtf8, sizeof pathUtf8)) {
        return NULL;
    }
    display_name = g_sumatra_cjk_family[0] ? g_sumatra_cjk_family : "Custom CJK";
    return try_font_file(ctx, display_name, pathUtf8, 0, 1);
}

static fz_font* load_reader_cjk_serif(fz_context* ctx, const char* display_name, int ordering) {
    const char* family = g_sumatra_cjk_family[0] ? g_sumatra_cjk_family : display_name;
    fz_font* font = load_bundled_custom_cjk_font(ctx);
    if (font) {
        font->flags.cjk = 1;
        font->flags.cjk_lang = ordering;
        return font;
    }

    if (!is_bundled_reader_cjk_setting()) {
        font = load_windows_font_by_name_impl(ctx, family, 0);
        if (font) {
            font->flags.cjk = 1;
            font->flags.cjk_lang = ordering;
            return font;
        }
        return NULL;
    }

    /* Always honor EBookUI.CjkFontFamily for reader CJK requests (including publisher
     * names like STSong / SimSun routed through is_reader_cjk_font_request). */
    if (!is_source_han_serif_sc_request(family)) {
        font = load_windows_font_by_name_impl(ctx, family, 0);
        if (font) {
            font->flags.cjk = 1;
            font->flags.cjk_lang = ordering;
            return font;
        }
        return NULL;
    }

    font = load_bundled_source_han_serif(ctx, family, ordering);
    if (font) {
        font->flags.cjk = 1;
        font->flags.cjk_lang = ordering;
    }
    return font;
}

static fz_font* load_windows_font(fz_context* ctx, const char* fontname, int bold, int italic,
                                  int needs_exact_metrics) {
    fz_font* font;
    const char* clean_name = pdf_clean_font_name(fontname);
    int is_base_14 = clean_name != fontname;

    if (!bold && !italic && is_reader_cjk_font_request(fontname)) {
        font = load_reader_cjk_serif(ctx, fontname, FZ_ADOBE_GB);
        if (font) {
            return font;
        }
    }

    if (!bold && !italic && is_bundled_reader_cjk_setting() && is_publisher_cjk_font_request(fontname)) {
        font = load_reader_cjk_serif(ctx, fontname, FZ_ADOBE_GB);
        if (font) {
            return font;
        }
    }

    /* metrics for Times-Roman don't match those of Windows' Times-Roman */
    /* https://code.google.com/p/sumatrapdf/issues/detail?id=2173 */
    /* https://github.com/sumatrapdfreader/sumatrapdf/issues/2108 */
    /* https://github.com/sumatrapdfreader/sumatrapdf/issues/2028 */
    /* TODO: should this always return NULL if is_base_14 is true? */
    if (is_base_14) {
        if (!strncmp(clean_name, "Times", 5)) {
            return NULL;
        }
        if (!strncmp(clean_name, "Helvetica", 9)) {
            return NULL;
        }
        if (!strncmp(clean_name, "Courier", 7)) {
            return NULL;
        }
        if (!strcmp(clean_name, "Symbol") || !strcmp(clean_name, "ZapfDingbats")) {
            return NULL;
        }
    }

    {
        const char* plus = strchr(fontname, '+');
        if (plus) {
            int b14len;
            const char* suffix = pdf_clean_font_name(plus + 1);
            if (fz_lookup_base14_font(ctx, suffix, &b14len)) {
                return NULL;
            }
        }
    }

    if (needs_exact_metrics) {
        int len;
        if (fz_lookup_base14_font(ctx, fontname, &len)) return NULL;

        if (clean_name != fontname && !strncmp(clean_name, "Times-", 6)) return NULL;
    }

    font = load_literata_font_file(ctx, fontname, bold, italic);
    if (font) return font;

    font = load_windows_font_by_name_and_style(ctx, fontname, bold, italic);
    if (font) return font;

    font = load_windows_font_by_name(ctx, fontname);
    if (!font) return NULL;
    /* use the font's own metrics for base 14 fonts */
    if (is_base_14) font->flags.ft_substitute = 0;
    return font;
}

static fz_font* load_windows_cjk_font(fz_context* ctx, const char* fontname, int ros, int serif) {
    fz_font* font = NULL;

    /* try to find a matching system font before falling back to an approximate one */
    font = load_windows_font_by_name(ctx, fontname);
    if (font) return font;

    /* try to fall back to a reasonable system font */
    fz_try(ctx) {
        if (serif) {
            switch (ros) {
                case FZ_ADOBE_CNS:
                    font = load_windows_font_by_name(ctx, "MingLiU");
                    break;
                case FZ_ADOBE_GB:
                    font = load_reader_cjk_serif(ctx, "Source Han Serif", FZ_ADOBE_GB);
                    if (!font) {
                        font = load_windows_font_by_name(ctx, "SimSun");
                    }
                    break;
                case FZ_ADOBE_JAPAN:
                    font = load_windows_font_by_name(ctx, "MS-Mincho");
                    break;
                case FZ_ADOBE_KOREA:
                    font = load_windows_font_by_name(ctx, "Batang");
                    break;
                default:
                    fz_throw(ctx, FZ_ERROR_GENERIC, "invalid serif ros");
            }
        } else {
            switch (ros) {
                case FZ_ADOBE_CNS:
                    font = load_windows_font_by_name(ctx, "DFKaiShu-SB-Estd-BF");
                    break;
                case FZ_ADOBE_GB:
                    font = load_windows_font_by_name(ctx, "KaiTi");
                    if (!font) {
                        font = load_windows_font_by_name(ctx, "KaiTi_GB2312");
                    }
                    break;
                case FZ_ADOBE_JAPAN:
                    font = load_windows_font_by_name(ctx, "MS-Gothic");
                    break;
                case FZ_ADOBE_KOREA:
                    font = load_windows_font_by_name(ctx, "Gulim");
                    break;
                default:
                    fz_throw(ctx, FZ_ERROR_GENERIC, "invalid sans-serif ros");
            }
        }
    }
    fz_catch(ctx) {
#ifdef NOCJKFONT
        /* If no CJK fallback font is builtin, maybe one has been shipped separately */
        font = load_windows_font_by_name(ctx, "DroidSansFallback");
#else
        fz_rethrow(ctx);
#endif
    }

    return font;
}
#endif

/*
Segoe UI Emoji Regular
Cambria Math Regular - math symbols
Segoe UI Symbol Regular - math and other symbols
Charis SIL => Times New Roman or Georgia

https://learn.microsoft.com/en-us/windows/apps/design/globalizing/loc-international-fonts
*/
static fz_font* load_windows_fallback_font(fz_context* ctx, int script, int language, int serif, int bold, int italic) {
    fz_font* font = NULL;
    const char* font_name = NULL;

    // TODO: more scripts
    switch (script) {
        case UCDN_SCRIPT_BENGALI: // bangla
        case UCDN_SCRIPT_GURMUKHI:
        case UCDN_SCRIPT_GUJARATI:
        case UCDN_SCRIPT_KANNADA:
        case UCDN_SCRIPT_MALAYALAM:
        case UCDN_SCRIPT_SINHALA:
        case UCDN_SCRIPT_SORA_SOMPENG:
        case UCDN_SCRIPT_OL_CHIKI:
        case UCDN_SCRIPT_ORIYA: // odia
        case UCDN_SCRIPT_TAMIL:
        case UCDN_SCRIPT_TELUGU:
        case UCDN_SCRIPT_DEVANAGARI: {
            font_name = "NirmalaUI";
            if (bold) {
                font_name = "NirmalaUI-Bold";
            }
        } break;
        case UCDN_SCRIPT_HEBREW: {
            font_name = "SegoeUI";
            if (bold) {
                font_name = "SegoeUI-Bold";
            }
        } break;
        case UCDN_SCRIPT_ARABIC:
        case UCDN_SCRIPT_SYRIAC:
        case UCDN_SCRIPT_THAANA: {
            font_name = "SegoeUI";
            if (bold) {
                font_name = "SegoeUI-Bold";
            }
        } break;
        case UCDN_SCRIPT_THAI:
        case UCDN_SCRIPT_LAO:
        case UCDN_SCRIPT_KHMER:
        case UCDN_SCRIPT_MYANMAR:
        case UCDN_SCRIPT_TIBETAN: {
            font_name = "MicrosoftSansSerif";
        } break;
        case UCDN_SCRIPT_HAN:
        case UCDN_SCRIPT_BOPOMOFO: {
            int ordering = han_ordering_for_language(language);
            font = load_reader_cjk_serif(ctx, "Source Han Serif", ordering);
            if (font) {
                return font;
            }
            static const char* han_fonts[] = {
                "NSimSun",
                "SimSun",
                NULL,
            };
            for (int i = 0; han_fonts[i]; i++) {
                font = load_windows_font_by_name(ctx, han_fonts[i]);
                if (font) {
                    return font;
                }
            }
            return NULL;
        } break;
        case UCDN_SCRIPT_HIRAGANA:
        case UCDN_SCRIPT_KATAKANA: {
            font_name = "YuGothic-Regular";
            if (bold) {
                font_name = "YuGothic-Bold";
            }
        } break;
        case UCDN_SCRIPT_HANGUL: {
            font_name = "MalgunGothic";
            if (bold) {
                font_name = "MalgunGothicBold";
            }
        } break;
        case UCDN_SCRIPT_ETHIOPIC: {
            font_name = "NyalaSemiBold";
        } break;
        case UCDN_SCRIPT_CANADIAN_ABORIGINAL: {
            font_name = "Gadugi";
            if (bold) {
                font_name = "Gadugi-Bold";
            }
        } break;
        case UCDN_SCRIPT_MONGOLIAN: {
            font_name = "MongolianBaiti";
        } break;
        case UCDN_SCRIPT_YI: {
            font_name = "MicrosoftYiBaiti";
        } break;
        case UCDN_SCRIPT_CYRILLIC:
        case UCDN_SCRIPT_GREEK:
        case UCDN_SCRIPT_ARMENIAN:
        case UCDN_SCRIPT_GEORGIAN: {
            font_name = "Sylfaen";
        } break;
        // per chatgpt Times New Roman is closest to Noto Serif
        case UCDN_SCRIPT_LATIN:
        // case UCDN_SCRIPT_GREEK:
        // case UCDN_SCRIPT_CYRILLIC:
        case UCDN_SCRIPT_COMMON:
        case UCDN_SCRIPT_INHERITED:
        case UCDN_SCRIPT_UNKNOWN: {
            if (serif) {
                font_name = "TimesNewRomanPSMT";
                if (bold) {
                    font_name = "TimesNewRomanPS-BoldMT";
                    if (italic) {
                        font_name = "TimesNewRomanPS-BoldItalicMT";
                    }
                } else if (italic) {
                    font_name = "TimesNewRomanPS-ItalicMT";
                }
            } else {
                font_name = "SegoeUI";
                if (bold) {
                    font_name = "SegoeUI-Bold";
                    if (italic) {
                        font_name = "SegoeUI-BoldItalic";
                    }
                } else if (italic) {
                    font_name = "SegoeUI-Italic";
                }
            }
        } break;
    }

    if (!font_name) {
        fz_warn(ctx, "couldn't find windows system font for script %d, language: %d, bold: %d, italic: %d", script,
                language, (int)bold, (int)italic);
        return NULL;
    }

    /* try to find a matching system font before falling back to an approximate one */
    font = load_windows_font_by_name(ctx, font_name);
    return font;
}

void init_system_font_list(void) {
    // this should always happen on main thread
    if (did_init) {
        return;
    }
    InitializeCriticalSection(&cs_fonts);
    g_win_fonts.fontmap = NULL;
    g_win_fonts.len = 0;
    g_win_fonts.cap = 0;
    g_font_files.files = NULL;
    g_font_files.len = 0;
    g_font_files.cap = 0;
    did_init = 1;
}

void destroy_system_font_list(void) {
    int i;
    for (i = 0; i < g_win_fonts.len; i++) {
        free((void*)g_win_fonts.fontmap[i].fontface);
    }
    free(g_win_fonts.fontmap);
    g_win_fonts.fontmap = NULL;
    g_win_fonts.len = 0;
    g_win_fonts.cap = 0;
    for (i = 0; i < g_font_files.len; i++) {
        free((void*)g_font_files.files[i].file_path);
        free(g_font_files.files[i].data);
    }
    free(g_font_files.files);
    g_font_files.files = NULL;
    g_font_files.len = 0;
    g_font_files.cap = 0;
    DeleteCriticalSection(&cs_fonts);
}

void install_load_windows_font_funcs(fz_context* ctx) {
    init_system_font_list();
    fz_install_load_system_font_funcs(ctx, load_windows_font, load_windows_cjk_font, load_windows_fallback_font);
}

// Wrappers around harfbuzz allocators that use standard C malloc/free.
// With HAVE_ATEXIT defined, harfbuzz registers atexit handlers to free
// its singletons. During atexit, mupdf's fz_hb_lock hasn't been called
// so fz_hb_secret (the fz_context) is NULL and fz_hb_free would crash.
// We use plain malloc/free which is safe both during normal operation
// (mupdf's default allocator is malloc/free) and during atexit cleanup.

void* sumatra_hb_malloc(size_t size) {
    return malloc(size);
}
void* sumatra_hb_calloc(size_t n, size_t size) {
    return calloc(n, size);
}
void* sumatra_hb_realloc(void* ptr, size_t size) {
    return realloc(ptr, size);
}
void sumatra_hb_free(void* ptr) {
    free(ptr);
}
