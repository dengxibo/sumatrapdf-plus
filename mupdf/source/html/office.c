// Copyright (C) 2023-2026 Artifex Software, Inc.
//
// This file is part of MuPDF.
//
// MuPDF is free software: you can redistribute it and/or modify it under the
// terms of the GNU Affero General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option)
// any later version.
//
// MuPDF is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for more
// details.
//
// You should have received a copy of the GNU Affero General Public License
// along with MuPDF. If not, see <https://www.gnu.org/licenses/agpl-3.0.en.html>
//
// Alternative licensing terms are available from the licensor.
// For commercial licensing, see <https://www.artifex.com/> or contact
// Artifex Software, Inc., 39 Mesa Street, Suite 108A, San Francisco,
// CA 94129, USA, for further information.

#include "mupdf/fitz.h"
#include "html-imp.h"

#include <string.h>

#define DEBUG_OFFICE_TO_HTML
#undef DEBUG_OFFICE_TO_HTML

/* Defaults are all 0's. FIXME: Very subject to change. Possibly might be removed entirely. */
typedef struct {
    int output_page_numbers;
    int output_sheet_names;
    int output_cell_markers;
    int output_cell_row_markers;
    int output_cell_names;
    int output_formatting;
    int output_filenames;
    int output_errors;
} fz_office_to_html_opts;

typedef struct {
    fz_office_to_html_opts opts;

    fz_output* out;

    int page;

    /* State for if we are parsing a sheet. */
    /* The last column label we have to send. */
    char* label;
    /* Columns are numbered from 1. */
    /* The column we are at. */
    int col_at;
    /* The column we last signalled. If this is 0, then we haven't
     * even started a row yet. */
    int col_signalled;

    /* If we are currently processing a spreadsheet, store the current
     * sheets name here. */
    const char* sheet_name;

    int shared_string_max;
    int shared_string_len;
    char** shared_strings;

    int footnotes_max;
    char** footnotes;

    char* title;

    /* Word document context for resolving images etc. */
    fz_archive* arch;
    const char* doc_file;
    fz_xml* doc_rels;
    int do_pages;
    /* 0 = none, 1 = <ul>, 2 = <ol> */
    int list_kind;
    /* >0 while emitting a table — skip text-outline inference (cell noise). */
    int in_table;
    /* >0 while emitting a paragraph/heading — do not inject page <div>s mid-flow. */
    int in_paragraph;
    /* Strip leading spaces/tabs while emitting a right-aligned 落款 paragraph. */
    int skip_leading_ws;
    /* Current paragraph is a space-padded 落款 — right-align, no nbsp padding. */
    int signoff_emit;
    /* Extra padding-right (em) so a shorter 落款 line centers under a longer sibling. */
    float signoff_pad_em;

    /* word/styles.xml: styleId -> heading level 1..6 (0 = not a heading). */
    char** style_ids;
    unsigned char* style_levels;
    int style_len;
    int style_cap;
    /* Kept styles.xml root for table border / style lookup (owned). */
    fz_xml* styles_xml;
    /* word/numbering.xml (owned) + per-numId counters for auto markers. */
    fz_xml* numbering_xml;
    int* num_ids;
    int* num_abs_ids;
    int* num_counts; /* length num_n * 9 */
    int num_n;
    int num_cap;

    /* Theme / document default East-Asian + Latin faces (owned). */
    char* theme_minor_ea;
    char* theme_major_ea;
    char* theme_minor_latin;
    char* doc_default_ea;
    /* Default paragraph styleId from styles.xml (owned); used for indent inheritance. */
    char* default_pstyle;
    /* Normal/default body size in pt (from styles.xml); drives body CSS + run fallback. */
    float doc_body_font_pt;
    /* Current paragraph's effective run size when run omits w:sz (style / pPr rPr). */
    float run_default_font_pt;
    /* Paragraph style / pPr eastAsia+latin when a run omits w:rFonts (公文 仿宋/小标宋). */
    char run_default_ea[128];
    char run_default_latin[128];
    /* Printable width (page − margins) for fitting condensed 红头 on one line. */
    float page_content_pt;
    /* Current section paper (pt). A later sectPr may switch to landscape. */
    float sect_pw, sect_ph, sect_mt, sect_mr, sect_mb, sect_ml;
    float next_pw, next_ph, next_mt, next_mr, next_mb, next_ml;
    int next_page_ready;
    /* >0: force run font-size this paragraph (红头 fit after missing true w:w scale). */
    float force_run_font_pt;
    /* Table cell textDirection is vertical — emit one glyph per line. */
    int emit_vertical_breaks;
    /* Next block should start a new page (Word next-page section break). */
    int pending_page_break;
    /* This paragraph has already emitted visible text (second "3." run → break). */
    int para_seen_text;
    /* 0-based row while emitting a table cell; -1 outside tables. */
    int table_row;
} doc_info;

static int ascii_strncasecmp(const char* a, const char* b, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (ca == 0 || cb == 0) return (int)ca - (int)cb;
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb - 'A' + 'a');
        if (ca != cb) return (int)ca - (int)cb;
    }
    return 0;
}

static int ascii_strcasecmp(const char* a, const char* b) {
    while (1) {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb - 'A' + 'a');
        if (ca != cb) return (int)ca - (int)cb;
        if (ca == 0) return 0;
    }
}

static int word_style_name_heading_level(const char* name) {
    int n;
    const char* p;

    if (!name || !name[0]) return 0;
    /* Do not use fz_strcasecmp here: it asserts equal UTF-8 widths, which
     * fails when comparing ASCII "Title"/"heading" against Chinese style names. */
    /* Skip TOC entry styles (toc 1 / toc 2) — those are the printed TOC, not body headings. */
    if (!ascii_strncasecmp(name, "toc ", 4)) return 0;
    if (!strncmp(name, "目录", 6)) /* UTF-8 目录 = 6 bytes */
        return 0;
    if (!ascii_strcasecmp(name, "Title") || !strcmp(name, "标题")) return 1;
    if (!ascii_strncasecmp(name, "heading ", 8)) {
        n = fz_atoi(name + 8);
        if (n >= 1 && n <= 6) return n;
    }
    /* Chinese Word: "标题 1" / "标题1" — UTF-8 标题 = 6 bytes */
    if (!strncmp(name, "标题", 6)) {
        p = name + 6;
        while (*p == ' ' || *p == '\t') p++;
        n = fz_atoi(p);
        if (n >= 1 && n <= 6) return n;
    }
    return 0;
}

static void word_styles_set(fz_context* ctx, doc_info* info, const char* style_id, int level) {
    int i;

    if (!style_id || !style_id[0] || level < 1 || level > 6) return;
    for (i = 0; i < info->style_len; i++) {
        if (!strcmp(info->style_ids[i], style_id)) {
            /* Prefer the stronger (already set) level; keep first. */
            if (info->style_levels[i] == 0) info->style_levels[i] = (unsigned char)level;
            return;
        }
    }
    if (info->style_len == info->style_cap) {
        int newcap = info->style_cap ? info->style_cap * 2 : 64;
        info->style_ids = fz_realloc(ctx, info->style_ids, sizeof(char*) * newcap);
        info->style_levels = fz_realloc(ctx, info->style_levels, newcap);
        memset(info->style_ids + info->style_cap, 0, sizeof(char*) * (newcap - info->style_cap));
        memset(info->style_levels + info->style_cap, 0, newcap - info->style_cap);
        info->style_cap = newcap;
    }
    info->style_ids[info->style_len] = fz_strdup(ctx, style_id);
    info->style_levels[info->style_len] = (unsigned char)level;
    info->style_len++;
}

static int word_style_heading_level(doc_info* info, const char* style_id) {
    int i;

    if (!style_id || !info) return 0;
    for (i = 0; i < info->style_len; i++) {
        if (!strcmp(info->style_ids[i], style_id)) return info->style_levels[i];
    }
    /* Fallback when styles.xml was missing: classic English/Chinese names used as styleId. */
    return word_style_name_heading_level(style_id);
}

/* Collect visible paragraph text for outline inference (cap buf). */
static void word_append_xml_text_nodes(fz_xml* node, char* buf, int bufsize, int* len) {
    fz_xml* n;
    for (n = node; n; n = fz_xml_next(n)) {
        const char* s = fz_xml_text(n);
        if (s) {
            while (*s && *len < bufsize - 1) buf[(*len)++] = *s++;
            continue;
        }
        if (fz_xml_down(n)) word_append_xml_text_nodes(fz_xml_down(n), buf, bufsize, len);
    }
}

static void word_paragraph_plain_text(fz_xml* p, char* buf, int bufsize) {
    int len = 0;

    if (!buf || bufsize < 2) return;
    buf[0] = 0;
    if (!p) return;
    /* Text lives in child text nodes under <w:t>, not on the element itself. */
    word_append_xml_text_nodes(fz_xml_down(p), buf, bufsize, &len);
    buf[len] = 0;
}

static int word_utf8_eq(const char* s, const char* u8) {
    size_t n = strlen(u8);
    return n > 0 && !strncmp(s, u8, n);
}

/* Byte length of one 一..十 numeral, or 0. */
static int word_chinese_numeral_bytes(const char* s) {
    static const char* nums[] = {"一", "二", "三", "四", "五", "六", "七", "八", "九", "十", NULL};
    int i;
    for (i = 0; nums[i]; i++) {
        size_t n = strlen(nums[i]);
        if (!strncmp(s, nums[i], n)) return (int)n;
    }
    return 0;
}

static const char* word_skip_leading_space(const char* p) {
    while (p && *p) {
        if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
            p++;
            continue;
        }
        /* U+3000 ideographic space */
        if (word_utf8_eq(p, "　")) {
            p += 3;
            continue;
        }
        break;
    }
    return p;
}

/* Width of leading U+0020 / U+3000 that authors use instead of w:ind.
 * Half-width space is half a 汉字; four of them match firstLine=640 twips. */
static float word_leading_space_pt(const char* s, float font_pt) {
    float pt = 0;
    if (!s || font_pt < 1.0f) return 0;
    while (*s) {
        if (*s == ' ') {
            pt += font_pt * 0.5f;
            s++;
            continue;
        }
        if (*s == '\t') {
            pt += font_pt * 2.0f;
            s++;
            continue;
        }
        if (word_utf8_eq(s, "　")) {
            pt += font_pt;
            s += 3;
            continue;
        }
        break;
    }
    return pt;
}

/* True if paragraph has no visible text and no drawing/image (Word often inserts
 * an empty <w:p> at a section break — CSS line-height still paints a blank line).
 * Do not use fz_xml_find_dfs: it walks out of the paragraph into later siblings. */
static int word_xml_has_desc_tag(fz_xml* top, const char* tag) {
    fz_xml* n;
    if (!top || !tag) return 0;
    for (n = fz_xml_down(top); n; n = fz_xml_next(n)) {
        if (fz_xml_is_tag(n, tag)) return 1;
        if (word_xml_has_desc_tag(n, tag)) return 1;
    }
    return 0;
}

static int word_paragraph_is_visually_empty(fz_xml* p) {
    char plain[512];
    const char* s;

    if (!p) return 1;
    if (word_xml_has_desc_tag(p, "drawing") || word_xml_has_desc_tag(p, "pict") || word_xml_has_desc_tag(p, "object"))
        return 0;
    word_paragraph_plain_text(p, plain, (int)sizeof plain);
    s = word_skip_leading_space(plain);
    return !s || !s[0];
}

/* OOXML default when w:type is omitted is nextPage. continuous stays on this page. */
static int word_sectpr_starts_new_page(fz_xml* sect) {
    fz_xml* typ;
    const char* v;
    if (!sect) return 0;
    typ = fz_xml_find_down(sect, "type");
    if (!typ) return 1;
    v = fz_xml_att_alt(typ, "w:val", "val");
    if (!v || !v[0] || !strcmp(v, "nextPage") || !strcmp(v, "oddPage") || !strcmp(v, "evenPage")) return 1;
    return 0;
}

static fz_xml* word_paragraph_sectpr(fz_xml* p) {
    fz_xml* ppr;
    if (!p) return NULL;
    ppr = fz_xml_find_down(p, "pPr");
    if (!ppr) return NULL;
    return fz_xml_find_down(ppr, "sectPr");
}

/* Next section's sectPr: a later paragraph's pPr/sectPr, or the body sectPr. */
static fz_xml* word_following_sectpr(fz_xml* p) {
    fz_xml* n;
    if (!p) return NULL;
    for (n = fz_xml_next(p); n; n = fz_xml_next(n)) {
        if (fz_xml_is_tag(n, "sectPr")) return n;
        if (fz_xml_is_tag(n, "p")) {
            fz_xml* s = word_paragraph_sectpr(n);
            if (s) return s;
        }
    }
    return NULL;
}

static int word_pt_differ(float a, float b) {
    float d = a - b;
    if (d < 0) d = -d;
    return d > 2.f;
}

/* Paper size of one sectPr. Missing pgMar keeps the caller's margins. */
static int word_sectpr_page_box(fz_xml* sect, float* pw, float* ph, float* mt, float* mr, float* mb, float* ml) {
    fz_xml* n;
    const char* v;
    if (!sect || !pw || !ph) return 0;
    n = fz_xml_find_down(sect, "pgSz");
    if (!n) return 0;
    v = fz_xml_att_alt(n, "w:w", "w");
    if (!v || !v[0]) return 0;
    *pw = (float)fz_atoi(v) / 20.0f;
    v = fz_xml_att_alt(n, "w:h", "h");
    if (!v || !v[0]) return 0;
    *ph = (float)fz_atoi(v) / 20.0f;
    n = fz_xml_find_down(sect, "pgMar");
    if (n) {
        v = fz_xml_att_alt(n, "w:top", "top");
        if (v && v[0] && mt) *mt = (float)fz_atoi(v) / 20.0f;
        v = fz_xml_att_alt(n, "w:right", "right");
        if (v && v[0] && mr) *mr = (float)fz_atoi(v) / 20.0f;
        v = fz_xml_att_alt(n, "w:bottom", "bottom");
        if (v && v[0] && mb) *mb = (float)fz_atoi(v) / 20.0f;
        v = fz_xml_att_alt(n, "w:left", "left");
        if (v && v[0] && ml) *ml = (float)fz_atoi(v) / 20.0f;
    }
    return *pw > 72.f && *ph > 72.f;
}

/* The sectPr on this paragraph describes the section that ENDS here.
 * The following sectPr is the paper for the content after the break. */
static void word_note_following_page_box(fz_xml* p, doc_info* info) {
    fz_xml* next;
    float pw, ph, mt, mr, mb, ml;
    if (!info) return;
    info->next_page_ready = 0;
    next = word_following_sectpr(p);
    if (!next) return;
    mt = info->sect_mt;
    mr = info->sect_mr;
    mb = info->sect_mb;
    ml = info->sect_ml;
    if (!word_sectpr_page_box(next, &pw, &ph, &mt, &mr, &mb, &ml)) return;
    if (!word_pt_differ(pw, info->sect_pw) && !word_pt_differ(ph, info->sect_ph) &&
        !word_pt_differ(mt, info->sect_mt) && !word_pt_differ(mr, info->sect_mr) &&
        !word_pt_differ(mb, info->sect_mb) && !word_pt_differ(ml, info->sect_ml))
        return;
    info->next_pw = pw;
    info->next_ph = ph;
    info->next_mt = mt;
    info->next_mr = mr;
    info->next_mb = mb;
    info->next_ml = ml;
    info->next_page_ready = 1;
}

/* Emit a zero-height marker MuPDF layout turns into a new page size.
 * Returns 1 if pending_page_break was consumed (do not also set page-break on the next tag). */
static void close_word_list(fz_context* ctx, doc_info* info);
static int word_consume_section_page(fz_context* ctx, doc_info* info) {
    float cw, ch;
    if (!info || !info->pending_page_break || !info->next_page_ready) return 0;
    cw = info->next_pw - info->next_ml - info->next_mr;
    ch = info->next_ph - info->next_mt - info->next_mb;
    if (cw < 72.f) cw = 72.f;
    if (ch < 72.f) ch = 72.f;
    close_word_list(ctx, info);
    fz_write_printf(ctx, info->out,
                    "<div id=\"wpage:%.1f:%.1f:%.1f:%.1f:%.1f:%.1f\" "
                    "style=\"page-break-before:always;margin:0;padding:0\"></div>\n",
                    cw, ch, info->next_mt, info->next_mr, info->next_mb, info->next_ml);
    info->page_content_pt = cw;
    info->sect_pw = info->next_pw;
    info->sect_ph = info->next_ph;
    info->sect_mt = info->next_mt;
    info->sect_mr = info->next_mr;
    info->sect_mb = info->next_mb;
    info->sect_ml = info->next_ml;
    info->pending_page_break = 0;
    info->next_page_ready = 0;
    return 1;
}

static int word_infer_text_heading_level(const char* text);

/* "1. 2. 3. 4." is an empty answer list, not an outline heading. */
static int word_plain_is_only_arabic_markers(const char* p) {
    int saw = 0;
    if (!p) return 0;
    while (*p) {
        int d = 0;
        while (*p == ' ' || *p == '\t') p++;
        if (word_utf8_eq(p, "　")) {
            p += 3;
            continue;
        }
        if (!*p) break;
        if (*p < '1' || *p > '9') return 0;
        while (p[d] >= '0' && p[d] <= '9' && d < 3) d++;
        if (d < 1) return 0;
        p += d;
        if (*p == '.' || *p == ')')
            p++;
        else if (word_utf8_eq(p, "、") || word_utf8_eq(p, "．"))
            p += 3;
        else
            return 0;
        saw = 1;
    }
    return saw;
}

/*
 * Many 公文 Word files never apply Heading 1/标题 1 — outline is plain text:
 * 一、… / （一）… / 1.… / 附件. Map those to h1..h3 for the HTML outline TOC.
 */
static int word_infer_text_heading_level(const char* text) {
    const char* p;
    int nlen;
    int digits;

    p = word_skip_leading_space(text);
    if (!p || !p[0]) return 0;
    if (word_plain_is_only_arabic_markers(p)) return 0;

    /* 附件 / 附件： / 附件N */
    if (word_utf8_eq(p, "附件")) {
        const char* r = p + 6;
        if (!r[0] || word_utf8_eq(r, "：") || word_utf8_eq(r, ":") || (*r >= '0' && *r <= '9') ||
            word_chinese_numeral_bytes(r) > 0)
            return 1;
        /* Bare "附件" alone is still a section break. */
        if (word_skip_leading_space(r)[0] == 0) return 1;
        /* "附件xxxx标题" without punctuation — still an attachment heading. */
        return 1;
    }

    /* （一）… fullwidth or halfwidth parens around a Chinese numeral */
    if (word_utf8_eq(p, "（") || *p == '(') {
        const char* q = p + ((*p == '(') ? 1 : 3);
        nlen = word_chinese_numeral_bytes(q);
        if (nlen > 0) {
            q += nlen;
            if (word_utf8_eq(q, "）") || *q == ')') return 2;
        }
        return 0;
    }

    /* 一、二、… or 一. */
    nlen = word_chinese_numeral_bytes(p);
    if (nlen > 0) {
        const char* q = p + nlen;
        if (word_utf8_eq(q, "、") || word_utf8_eq(q, "．") || *q == '.') {
            q += (*q == '.') ? 1 : 3;
            /* Require some title text (avoid stray "一、"). */
            q = word_skip_leading_space(q);
            if (q[0]) return 1;
        }
        return 0;
    }

    /* 1. / 2、 / 10． numbered task lines */
    if (p[0] >= '1' && p[0] <= '9') {
        digits = 0;
        while (p[digits] >= '0' && p[digits] <= '9' && digits < 3) digits++;
        if (digits > 0) {
            const char* q = p + digits;
            if (*q == '.' || word_utf8_eq(q, "、") || word_utf8_eq(q, "．")) {
                q += (*q == '.') ? 1 : 3;
                q = word_skip_leading_space(q);
                /* Skip pure page-ish "1." with no title. */
                if (q[0]) return 3;
            }
        }
    }
    return 0;
}

static int word_utf8_ends_with(const char* s, const char* suffix) {
    size_t sl, su;
    if (!s || !suffix || !suffix[0]) return 0;
    sl = strlen(s);
    su = strlen(suffix);
    return sl >= su && !strcmp(s + sl - su, suffix);
}

static int word_plain_glyph_count(const char* s) {
    int n = 0;
    if (!s) return 0;
    while (*s) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x80) {
            if (c > 32) n++;
            s++;
        } else if ((c & 0xE0) == 0xC0 && s[1]) {
            n++;
            s += 2;
        } else if ((c & 0xF0) == 0xE0 && s[1] && s[2]) {
            n++;
            s += 3;
        } else if ((c & 0xF8) == 0xF0 && s[1] && s[2] && s[3]) {
            n++;
            s += 4;
        } else
            s++;
    }
    return n;
}

static int word_plain_is_blank(const char* plain) {
    const char* p = word_skip_leading_space(plain);
    return !p || !p[0];
}

/*
 * 公文文头 often has no Heading/标题 style — only center + 方正小标宋.
 * Sidebar TOC is HTML outline (h1–h6), not Word's TOC field; promote these
 * to h1 so 关于…的函 / 通知 / 请示 appear as the document root.
 */
static int word_looks_like_official_doc_title(const char* plain, const char* align) {
    const char* p = word_skip_leading_space(plain);
    int glyphs;

    if (!p || !p[0]) return 0;
    /* Require explicit center (公文红头/文件名常见). */
    if (!align || strcmp(align, "center") != 0) return 0;
    glyphs = word_plain_glyph_count(p);
    if (glyphs < 4 || glyphs > 80) return 0;
    /* Body paragraphs that happen to be centered. */
    if (strstr(p, "。") || strstr(p, "；")) return 0;
    if (!(word_utf8_eq(p, "关于") || word_utf8_eq(p, "中共") || word_utf8_eq(p, "印发") || word_utf8_eq(p, "转发") ||
          word_utf8_eq(p, "江西省") || word_utf8_eq(p, "国务院") || word_utf8_eq(p, "人力资源")))
        return 0;
    return word_utf8_ends_with(p, "的函") || word_utf8_ends_with(p, "通知") || word_utf8_ends_with(p, "请示") ||
           word_utf8_ends_with(p, "通报") || word_utf8_ends_with(p, "批复") || word_utf8_ends_with(p, "意见") ||
           word_utf8_ends_with(p, "决定") || word_utf8_ends_with(p, "报告") || word_utf8_ends_with(p, "回复意见") ||
           word_utf8_ends_with(p, "情况说明");
}

/* Word authors often fake-center 红头/文头 with a huge firstLine indent instead of
 * w:jc=center (tuned to Word's page width). That indent is wrong on our HTML
 * page width and reads as "shifted right". Normal 首行缩进 is ~2 汉字
 * (firstLineChars=200 / ~32pt); fake-center is typically ≥3.5 汉字 / ≥48pt.
 * 文号/副标题 also use a large w:ind left (no firstLine) for the same effect. */
static int word_looks_like_fake_center_indent(const char* plain, float indent_em, float indent_pt, float left_pt) {
    const char* p = word_skip_leading_space(plain);
    int glyphs;
    int large = (indent_em >= 2.5f) || (indent_pt >= 48.0f) || (left_pt >= 48.0f);

    if (!large || !p || !p[0]) return 0;
    /* 2. / 4. / 附件 是真左缩进的条目，不是文号那种假居中。 */
    if (word_infer_text_heading_level(p) > 0) return 0;
    /* 2025年  月  日 sits on the right via left indent, not as a centered title. */
    if (strstr(p, "年") && strstr(p, "月")) return 0;
    glyphs = word_plain_glyph_count(p);
    if (glyphs < 2 || glyphs > 40) return 0;
    /* Body sentences — keep real firstLine / left. */
    if (strstr(p, "。") || strstr(p, "；")) return 0;
    /* Salutation 厅各…： is flush-left body, not a fake-centered title. */
    if ((strstr(p, "：") || strstr(p, ":")) && glyphs <= 12) return 0;
    return 1;
}

/* Half-points in w:sz / w:szCs → pt (defined below; used by run metrics). */
static float word_rpr_font_pt(fz_xml* rpr);

/* Scan paragraph runs: max font pt and min character scale (w:w, percent, 100 = none). */
static void word_paragraph_run_metrics(fz_xml* p, float* max_pt_out, int* min_scale_out) {
    fz_xml* n;
    float max_pt = 0;
    int min_scale = 100;

    if (max_pt_out) *max_pt_out = 0;
    if (min_scale_out) *min_scale_out = 100;
    if (!p) return;
    for (n = fz_xml_down(p); n; n = fz_xml_next(n)) {
        fz_xml* rpr;
        float pt;
        fz_xml* w_n;
        const char* wv;
        int scale;
        if (!fz_xml_is_tag(n, "r")) continue;
        rpr = fz_xml_find_down(n, "rPr");
        if (!rpr) continue;
        pt = word_rpr_font_pt(rpr);
        if (pt > max_pt) max_pt = pt;
        w_n = fz_xml_find_down(rpr, "w");
        wv = w_n ? fz_xml_att_alt(w_n, "w:val", "val") : NULL;
        if (wv && wv[0]) {
            scale = fz_atoi(wv);
            if (scale > 0 && scale < min_scale) min_scale = scale;
        }
    }
    if (max_pt_out) *max_pt_out = max_pt;
    if (min_scale_out) *min_scale_out = min_scale;
}

/* 公文红头: oversized agency line, often jc=right + w:w condensed so one line fills
 * the page. Without character scale we wrap; treat as centered banner and fit size. */
static int word_looks_like_hongtou_banner(const char* plain, const char* align, float max_run_pt, int min_scale) {
    const char* p = word_skip_leading_space(plain);
    int glyphs;
    int right = align && !strcmp(align, "right");
    int condensed = min_scale > 0 && min_scale < 90;

    if (!p || !p[0]) return 0;
    if (max_run_pt < 36.0f) return 0;
    if (!right && !condensed) return 0;
    glyphs = word_plain_glyph_count(p);
    if (glyphs < 4 || glyphs > 24) return 0;
    if (strstr(p, "。") || strstr(p, "；") || strstr(p, "：") || strstr(p, ":")) return 0;
    return word_utf8_ends_with(p, "厅") || word_utf8_ends_with(p, "局") || word_utf8_ends_with(p, "委") ||
           word_utf8_ends_with(p, "办") || word_utf8_ends_with(p, "府") || word_utf8_ends_with(p, "院") ||
           word_utf8_ends_with(p, "部") || strstr(p, "人力资源") != NULL || strstr(p, "人民政府") != NULL;
}

static void drop_word_styles(fz_context* ctx, doc_info* info) {
    int i;

    for (i = 0; i < info->style_len; i++) fz_free(ctx, info->style_ids[i]);
    fz_free(ctx, info->style_ids);
    fz_free(ctx, info->style_levels);
    info->style_ids = NULL;
    info->style_levels = NULL;
    info->style_len = 0;
    info->style_cap = 0;
    fz_drop_xml(ctx, info->styles_xml);
    info->styles_xml = NULL;
    fz_drop_xml(ctx, info->numbering_xml);
    info->numbering_xml = NULL;
    fz_free(ctx, info->num_ids);
    fz_free(ctx, info->num_abs_ids);
    fz_free(ctx, info->num_counts);
    info->num_ids = NULL;
    info->num_abs_ids = NULL;
    info->num_counts = NULL;
    info->num_n = 0;
    info->num_cap = 0;
    fz_free(ctx, info->theme_minor_ea);
    fz_free(ctx, info->theme_major_ea);
    fz_free(ctx, info->theme_minor_latin);
    fz_free(ctx, info->doc_default_ea);
    fz_free(ctx, info->default_pstyle);
    info->theme_minor_ea = NULL;
    info->theme_major_ea = NULL;
    info->theme_minor_latin = NULL;
    info->doc_default_ea = NULL;
    info->default_pstyle = NULL;
    info->doc_body_font_pt = 0;
    info->run_default_font_pt = 0;
    info->run_default_ea[0] = 0;
    info->run_default_latin[0] = 0;
}

static char* make_absolute_path(fz_context* ctx, const char* abs, const char* rel);
static fz_xml* try_parse_xml_archive_entry(fz_context* ctx, fz_archive* arch, const char* filename, int preserve_white);

/* Word sectPr page box (twips→pt). Defaults ≈ A4 with 公文 margins (1"/1.25"). */
static void word_default_page_box(float* mt, float* mr, float* mb, float* ml, float* pw, float* ph) {
    *mt = 72.f;
    *mb = 72.f;
    *ml = 90.f;
    *mr = 90.f;
    *pw = 595.3f;
    *ph = 841.9f;
}

static void word_load_page_box(fz_context* ctx, fz_archive* arch, const char* doc_file, float* mt, float* mr, float* mb,
                               float* ml, float* pw, float* ph) {
    fz_xml* xml = NULL;
    fz_xml* n;
    const char* v;

    word_default_page_box(mt, mr, mb, ml, pw, ph);
    if (!arch || !doc_file || !doc_file[0]) return;

    fz_var(xml);
    fz_try(ctx) {
        xml = try_parse_xml_archive_entry(ctx, arch, doc_file, 0);
        if (xml) {
            n = fz_xml_find_dfs(xml, "pgSz", NULL, NULL);
            if (n) {
                v = fz_xml_att_alt(n, "w:w", "w");
                if (v && v[0]) *pw = (float)fz_atoi(v) / 20.0f;
                v = fz_xml_att_alt(n, "w:h", "h");
                if (v && v[0]) *ph = (float)fz_atoi(v) / 20.0f;
            }
            n = fz_xml_find_dfs(xml, "pgMar", NULL, NULL);
            if (n) {
                v = fz_xml_att_alt(n, "w:top", "top");
                if (v && v[0]) *mt = (float)fz_atoi(v) / 20.0f;
                v = fz_xml_att_alt(n, "w:right", "right");
                if (v && v[0]) *mr = (float)fz_atoi(v) / 20.0f;
                v = fz_xml_att_alt(n, "w:bottom", "bottom");
                if (v && v[0]) *mb = (float)fz_atoi(v) / 20.0f;
                v = fz_xml_att_alt(n, "w:left", "left");
                if (v && v[0]) *ml = (float)fz_atoi(v) / 20.0f;
            }
        }
    }
    fz_always(ctx) {
        fz_drop_xml(ctx, xml);
    }
    fz_catch(ctx) {
        /* Keep defaults — margins are best-effort for wrap fidelity. */
        fz_ignore_error(ctx);
    }
}

/* Language placeholders Word writes when the real face comes from the theme. */
static int word_font_is_lang_placeholder(const char* name) {
    if (!name || !name[0]) return 1;
    if (!strcmp(name, "zh-CN") || !strcmp(name, "zh-TW") || !strcmp(name, "zh-HK")) return 1;
    if (!strcmp(name, "ja-JP") || !strcmp(name, "ko-KR")) return 1;
    return 0;
}

/* Extra Windows face names to try when CSS resolves the Word eastAsia name. */
static const char* word_font_win_alias(const char* name) {
    if (!name) return NULL;
    if (!strcmp(name, "仿宋") || !strcmp(name, "仿宋_GB2312")) return "FangSong";
    if (!strcmp(name, "楷体") || !strcmp(name, "楷体_GB2312")) return "KaiTi";
    if (!strcmp(name, "黑体") || !strcmp(name, "黑体_GB2312")) return "SimHei";
    if (!strcmp(name, "宋体") || !strcmp(name, "宋体_GB2312") || !strcmp(name, "新宋体")) return "SimSun";
    if (!strcmp(name, "华文中宋")) return "STZhongsong";
    if (!strcmp(name, "华文仿宋")) return "STFangsong";
    if (!strcmp(name, "华文楷体")) return "STKaiti";
    if (!strcmp(name, "微软雅黑")) return "Microsoft YaHei";
    if (!strcmp(name, "方正小标宋简体") || !strcmp(name, "方正小标宋") || !strcmp(name, "创艺简标宋"))
        return "FZXiaoBiaoSong-B05S";
    if (!strcmp(name, "方正黑体简体")) return "FZHei-B01S";
    if (!strcmp(name, "方正楷体简体")) return "FZKai-Z03S";
    if (!strcmp(name, "方正仿宋简体")) return "FZFangSong-Z02S";
    return NULL;
}

static void word_write_css_font_name(fz_context* ctx, fz_output* out, const char* name) {
    const char* p;
    if (!name || !name[0]) return;
    fz_write_byte(ctx, out, '"');
    for (p = name; *p; p++) {
        if (*p == '"' || *p == '\\') fz_write_byte(ctx, out, '\\');
        fz_write_byte(ctx, out, (unsigned char)*p);
    }
    fz_write_byte(ctx, out, '"');
}

/* Emit font-family list: primary Word face + Windows alias + latin + serif. */
static int word_emit_font_family_css(fz_context* ctx, doc_info* info, fz_output* out, const char* east,
                                     const char* latin) {
    const char* alias;
    int wrote = 0;

    if (word_font_is_lang_placeholder(east)) east = NULL;
    if (!east && info) east = info->doc_default_ea ? info->doc_default_ea : info->theme_minor_ea;
    if (!latin && info) latin = info->theme_minor_latin;
    if (!east && !latin) return 0;

    fz_write_string(ctx, out, "font-family:");
    if (east) {
        word_write_css_font_name(ctx, out, east);
        wrote = 1;
        alias = word_font_win_alias(east);
        if (alias && strcmp(alias, east) != 0) {
            fz_write_byte(ctx, out, ',');
            word_write_css_font_name(ctx, out, alias);
            /* Founder small-标宋 often installs as …B05 without S. */
            if (!strcmp(alias, "FZXiaoBiaoSong-B05S")) {
                fz_write_byte(ctx, out, ',');
                word_write_css_font_name(ctx, out, "FZXiaoBiaoSong-B05");
                /* Founder 小标宋 is rarely installed; 华文中宋 is the usual Windows stand-in. */
                fz_write_byte(ctx, out, ',');
                word_write_css_font_name(ctx, out, "STZhongsong");
                fz_write_byte(ctx, out, ',');
                word_write_css_font_name(ctx, out, "华文中宋");
            }
            if (!strcmp(alias, "FangSong")) {
                fz_write_byte(ctx, out, ',');
                word_write_css_font_name(ctx, out, "FangSong_GB2312");
                fz_write_byte(ctx, out, ',');
                word_write_css_font_name(ctx, out, "STFangsong");
                fz_write_byte(ctx, out, ',');
                word_write_css_font_name(ctx, out, "华文仿宋");
            }
            if (!strcmp(alias, "KaiTi")) {
                fz_write_byte(ctx, out, ',');
                word_write_css_font_name(ctx, out, "KaiTi_GB2312");
            }
        }
    }
    if (latin && (!east || strcmp(latin, east) != 0)) {
        if (wrote) fz_write_byte(ctx, out, ',');
        word_write_css_font_name(ctx, out, latin);
        wrote = 1;
    }
    if (wrote) fz_write_string(ctx, out, ",serif");
    return wrote;
}

static void word_strdup_att(fz_context* ctx, char** dst, fz_xml* node, const char* a1, const char* a2) {
    const char* v = fz_xml_att_alt(node, a1, a2);
    if (!v || !v[0] || word_font_is_lang_placeholder(v)) return;
    fz_free(ctx, *dst);
    *dst = fz_strdup(ctx, v);
}

static void load_word_theme_fonts(fz_context* ctx, fz_archive* arch, doc_info* info) {
    fz_xml* xml = NULL;
    fz_xml* minor;
    fz_xml* major;
    fz_xml* n;

    fz_var(xml);
    if (!info || info->theme_minor_ea || info->theme_major_ea) return;
    fz_try(ctx) {
        xml = try_parse_xml_archive_entry(ctx, arch, "word/theme/theme1.xml", 0);
        if (!xml) break;

        minor = fz_xml_find_dfs(xml, "minorFont", NULL, NULL);
        if (minor) {
            for (n = fz_xml_down(minor); n; n = fz_xml_next(n)) {
                if (fz_xml_is_tag(n, "ea") && !info->theme_minor_ea)
                    word_strdup_att(ctx, &info->theme_minor_ea, n, "typeface", "typeface");
                else if (fz_xml_is_tag(n, "latin") && !info->theme_minor_latin)
                    word_strdup_att(ctx, &info->theme_minor_latin, n, "typeface", "typeface");
                else if (fz_xml_is_tag(n, "font")) {
                    const char* script = fz_xml_att(n, "script");
                    if (script && !strcmp(script, "Hans") && !info->theme_minor_ea)
                        word_strdup_att(ctx, &info->theme_minor_ea, n, "typeface", "typeface");
                }
            }
        }
        major = fz_xml_find_dfs(xml, "majorFont", NULL, NULL);
        if (major) {
            for (n = fz_xml_down(major); n; n = fz_xml_next(n)) {
                if (fz_xml_is_tag(n, "ea") && !info->theme_major_ea)
                    word_strdup_att(ctx, &info->theme_major_ea, n, "typeface", "typeface");
                else if (fz_xml_is_tag(n, "font")) {
                    const char* script = fz_xml_att(n, "script");
                    if (script && !strcmp(script, "Hans") && !info->theme_major_ea)
                        word_strdup_att(ctx, &info->theme_major_ea, n, "typeface", "typeface");
                }
            }
        }
    }
    fz_always(ctx) fz_drop_xml(ctx, xml);
    fz_catch(ctx) fz_ignore_error(ctx);
}

/* Load word/styles.xml so numeric styleIds (2,3,4…) map to heading levels. */
static void load_word_styles(fz_context* ctx, fz_archive* arch, doc_info* info, const char* doc_file) {
    char* styles_path = NULL;
    fz_xml* xml = NULL;
    fz_xml* style;

    fz_var(styles_path);
    fz_var(xml);

    if (!info || info->styles_xml) return; /* already loaded (e.g. CSS pre-pass) */

    fz_try(ctx) {
        styles_path = make_absolute_path(ctx, doc_file ? doc_file : "word/document.xml", "styles.xml");
        xml = try_parse_xml_archive_entry(ctx, arch, styles_path, 0);
        if (!xml) xml = try_parse_xml_archive_entry(ctx, arch, "word/styles.xml", 0);
        if (!xml) break;

        /* Keep styles.xml for table border resolution (tblStyle → tblBorders). */
        info->styles_xml = xml;
        xml = NULL;

        {
            fz_xml* styles_root = fz_xml_find_dfs(info->styles_xml, "styles", NULL, NULL);
            fz_xml* child = fz_xml_down(styles_root ? styles_root : info->styles_xml);
            for (; child; child = fz_xml_next(child)) {
                const char* sid;
                const char* name = NULL;
                int level = 0;
                fz_xml* name_n;
                fz_xml* ppr;
                fz_xml* ol;

                if (fz_xml_is_tag(child, "docDefaults")) {
                    fz_xml* rpr_def = fz_xml_find_down(child, "rPrDefault");
                    fz_xml* rpr = rpr_def ? fz_xml_find_down(rpr_def, "rPr") : NULL;
                    fz_xml* rfonts = rpr ? fz_xml_find_down(rpr, "rFonts") : NULL;
                    if (rfonts && !info->doc_default_ea) {
                        const char* ea = fz_xml_att_alt(rfonts, "w:eastAsia", "eastAsia");
                        if (word_font_is_lang_placeholder(ea)) ea = NULL;
                        if (!ea) {
                            const char* th = fz_xml_att_alt(rfonts, "w:eastAsiaTheme", "eastAsiaTheme");
                            if (th && !strncmp(th, "major", 5))
                                ea = info->theme_major_ea;
                            else
                                ea = info->theme_minor_ea;
                        }
                        if (ea) info->doc_default_ea = fz_strdup(ctx, ea);
                    }
                    continue;
                }

                if (!fz_xml_is_tag(child, "style")) continue;
                style = child;
                sid = fz_xml_att_alt(style, "w:styleId", "styleId");
                {
                    const char* stype = fz_xml_att_alt(style, "w:type", "type");
                    const char* def = fz_xml_att_alt(style, "w:default", "default");
                    if (stype && !strcmp(stype, "paragraph") && def && !strcmp(def, "1") && sid &&
                        !info->default_pstyle)
                        info->default_pstyle = fz_strdup(ctx, sid);
                }
                name_n = fz_xml_find_down(style, "name");
                if (name_n) name = fz_xml_att_alt(name_n, "w:val", "val");
                level = word_style_name_heading_level(name);
                ppr = fz_xml_find_down(style, "pPr");
                ol = ppr ? fz_xml_find_down(ppr, "outlineLvl") : NULL;
                if (ol) {
                    const char* v = fz_xml_att_alt(ol, "w:val", "val");
                    int lvl = v ? fz_atoi(v) : -1;
                    if (lvl >= 0 && lvl <= 5) {
                        int from_ol = lvl + 1;
                        if (level == 0) level = from_ol;
                    }
                }
                if (level > 0) word_styles_set(ctx, info, sid, level);
            }
        }
        /* Fallback: style named Normal / 正文. */
        if (!info->default_pstyle && info->styles_xml) {
            fz_xml* st = fz_xml_find_dfs(info->styles_xml, "style", NULL, NULL);
            while (st) {
                const char* stype = fz_xml_att_alt(st, "w:type", "type");
                fz_xml* name_n = fz_xml_find_down(st, "name");
                const char* nm = name_n ? fz_xml_att_alt(name_n, "w:val", "val") : NULL;
                const char* sid2 = fz_xml_att_alt(st, "w:styleId", "styleId");
                if (stype && !strcmp(stype, "paragraph") && sid2 && nm &&
                    (!ascii_strcasecmp(nm, "Normal") || !strcmp(nm, "正文"))) {
                    info->default_pstyle = fz_strdup(ctx, sid2);
                    break;
                }
                st = fz_xml_find_next_dfs(st, "style", NULL, NULL);
            }
        }
    }
    fz_always(ctx) {
        fz_drop_xml(ctx, xml);
        fz_free(ctx, styles_path);
    }
    fz_catch(ctx) {
        /* Styles are optional; body still converts without outline map. */
        fz_ignore_error(ctx);
    }
}

#define WORD_NUM_ILVL_MAX 9

static void load_word_numbering(fz_context* ctx, fz_archive* arch, doc_info* info, const char* doc_file) {
    char* path = NULL;
    fz_xml* xml = NULL;

    fz_var(path);
    fz_var(xml);

    if (!info) return;
    if (info->numbering_xml) return;
    fz_try(ctx) {
        path = make_absolute_path(ctx, doc_file ? doc_file : "word/document.xml", "numbering.xml");
        xml = try_parse_xml_archive_entry(ctx, arch, path, 0);
        if (!xml) xml = try_parse_xml_archive_entry(ctx, arch, "word/numbering.xml", 0);
        if (!xml) break;
        info->numbering_xml = xml;
        xml = NULL;
    }
    fz_always(ctx) {
        fz_drop_xml(ctx, xml);
        fz_free(ctx, path);
    }
    fz_catch(ctx) {
        /* numbering.xml is optional. */
        fz_ignore_error(ctx);
    }
}

static int word_num_find_abs_id(doc_info* info, int num_id) {
    fz_xml* n;

    if (!info || !info->numbering_xml || num_id <= 0) return -1;
    n = fz_xml_find_dfs(info->numbering_xml, "num", NULL, NULL);
    while (n) {
        const char* id = fz_xml_att_alt(n, "w:numId", "numId");
        if (id && fz_atoi(id) == num_id) {
            fz_xml* abs = fz_xml_find_down(n, "abstractNumId");
            const char* av = abs ? fz_xml_att_alt(abs, "w:val", "val") : NULL;
            if (av) return fz_atoi(av);
            return -1;
        }
        n = fz_xml_find_next_dfs(n, "num", NULL, NULL);
    }
    return -1;
}

static int word_num_ensure_slot(fz_context* ctx, doc_info* info, int num_id) {
    int i;
    int abs_id;

    if (!info || num_id <= 0) return -1;
    for (i = 0; i < info->num_n; i++) {
        if (info->num_ids[i] == num_id) return i;
    }
    abs_id = word_num_find_abs_id(info, num_id);
    if (abs_id < 0) return -1;
    if (info->num_n >= info->num_cap) {
        int ncap = info->num_cap ? info->num_cap * 2 : 16;
        info->num_ids = fz_realloc_array(ctx, info->num_ids, ncap, int);
        info->num_abs_ids = fz_realloc_array(ctx, info->num_abs_ids, ncap, int);
        info->num_counts = fz_realloc_array(ctx, info->num_counts, ncap * WORD_NUM_ILVL_MAX, int);
        info->num_cap = ncap;
    }
    i = info->num_n++;
    info->num_ids[i] = num_id;
    info->num_abs_ids[i] = abs_id;
    memset(info->num_counts + i * WORD_NUM_ILVL_MAX, 0, WORD_NUM_ILVL_MAX * sizeof(int));
    return i;
}

static fz_xml* word_find_abstract_lvl(doc_info* info, int abs_id, int ilvl) {
    fz_xml* abs;
    fz_xml* n;

    if (!info || !info->numbering_xml || abs_id < 0 || ilvl < 0) return NULL;
    abs = fz_xml_find_dfs(info->numbering_xml, "abstractNum", NULL, NULL);
    while (abs) {
        const char* id = fz_xml_att_alt(abs, "w:abstractNumId", "abstractNumId");
        if (id && fz_atoi(id) == abs_id) {
            for (n = fz_xml_down(abs); n; n = fz_xml_next(n)) {
                if (!fz_xml_is_tag(n, "lvl")) continue;
                {
                    const char* lv = fz_xml_att_alt(n, "w:ilvl", "ilvl");
                    if (lv && fz_atoi(lv) == ilvl) return n;
                }
            }
            return NULL;
        }
        abs = fz_xml_find_next_dfs(abs, "abstractNum", NULL, NULL);
    }
    return NULL;
}

static void word_format_list_value(const char* fmt, int n, char* out, int cap) {
    static const char* cn[] = {"零", "一", "二", "三", "四", "五", "六", "七", "八", "九", "十"};

    if (!out || cap < 2) return;
    out[0] = 0;
    if (n < 0) n = 0;
    if (!fmt || !strcmp(fmt, "decimal") || !strcmp(fmt, "decimalZero")) {
        fz_snprintf(out, cap, "%d", n);
        return;
    }
    if (!strcmp(fmt, "bullet")) {
        fz_snprintf(out, cap, "•");
        return;
    }
    if (!strcmp(fmt, "lowerLetter") || !strcmp(fmt, "upperLetter")) {
        char tmp[16];
        int i = 0;
        int v = n <= 0 ? 1 : n;
        int upper = !strcmp(fmt, "upperLetter");
        while (v > 0 && i < (int)sizeof(tmp) - 1) {
            v--;
            tmp[i++] = (char)((upper ? 'A' : 'a') + (v % 26));
            v /= 26;
        }
        {
            int j;
            for (j = 0; j < i && j < cap - 1; j++) out[j] = tmp[i - 1 - j];
            out[j] = 0;
        }
        return;
    }
    if (!strcmp(fmt, "lowerRoman") || !strcmp(fmt, "upperRoman")) {
        static const int vals[] = {1000, 900, 500, 400, 100, 90, 50, 40, 10, 9, 5, 4, 1};
        static const char* low[] = {"m", "cm", "d", "cd", "c", "xc", "l", "xl", "x", "ix", "v", "iv", "i"};
        static const char* up[] = {"M", "CM", "D", "CD", "C", "XC", "L", "XL", "X", "IX", "V", "IV", "I"};
        const char** sym = !strcmp(fmt, "upperRoman") ? up : low;
        int v = n <= 0 ? 1 : n;
        int pos = 0;
        int i;
        for (i = 0; i < 13 && v > 0 && pos < cap - 1; i++) {
            while (v >= vals[i] && pos < cap - 1) {
                int k;
                for (k = 0; sym[i][k] && pos < cap - 1; k++) out[pos++] = sym[i][k];
                v -= vals[i];
            }
        }
        out[pos] = 0;
        return;
    }
    if (!strcmp(fmt, "chineseCounting") || !strcmp(fmt, "chineseCountingThousand") ||
        !strcmp(fmt, "japaneseCounting")) {
        if (n <= 10) {
            fz_snprintf(out, cap, "%s", cn[n <= 0 ? 0 : n]);
            return;
        }
        if (n < 20) {
            fz_snprintf(out, cap, "十%s", n == 10 ? "" : cn[n - 10]);
            return;
        }
        if (n < 100) {
            if (n % 10)
                fz_snprintf(out, cap, "%s十%s", cn[n / 10], cn[n % 10]);
            else
                fz_snprintf(out, cap, "%s十", cn[n / 10]);
            return;
        }
        fz_snprintf(out, cap, "%d", n);
        return;
    }
    fz_snprintf(out, cap, "%d", n);
}

static void word_expand_lvl_text(const char* lvl_text, const char* values[], int nvals, char* out, int cap) {
    const char* p = lvl_text ? lvl_text : "%1.";
    int pos = 0;

    if (!out || cap < 2) return;
    while (*p && pos < cap - 1) {
        if (*p == '%' && p[1] >= '1' && p[1] <= '9') {
            int idx = p[1] - '1';
            const char* v = (idx >= 0 && idx < nvals && values[idx]) ? values[idx] : "";
            while (*v && pos < cap - 1) out[pos++] = *v++;
            p += 2;
            continue;
        }
        out[pos++] = *p++;
    }
    out[pos] = 0;
}

/* Advance auto-number for numId/ilvl; write marker + list left/hanging/firstLine (pt). */
static int word_num_take(fz_context* ctx, doc_info* info, int num_id, int ilvl, char* marker, int marker_cap,
                         float* left_pt, float* hanging_pt, float* first_pt) {
    int slot;
    int abs_id;
    int* counts;
    int i;
    fz_xml* lvl;
    fz_xml* ppr;
    fz_xml* ind;
    fz_xml* start_n;
    fz_xml* fmt_n;
    fz_xml* text_n;
    const char* fmt;
    const char* lvl_text;
    char value_bufs[WORD_NUM_ILVL_MAX][32];
    const char* values[WORD_NUM_ILVL_MAX];
    const char* fmts[WORD_NUM_ILVL_MAX];

    if (left_pt) *left_pt = 0;
    if (hanging_pt) *hanging_pt = 0;
    if (first_pt) *first_pt = 0;
    if (marker && marker_cap > 0) marker[0] = 0;
    if (!info || !info->numbering_xml || num_id <= 0 || ilvl < 0 || ilvl >= WORD_NUM_ILVL_MAX) return 0;

    slot = word_num_ensure_slot(ctx, info, num_id);
    if (slot < 0) return 0;
    abs_id = info->num_abs_ids[slot];
    counts = info->num_counts + slot * WORD_NUM_ILVL_MAX;

    for (i = 0; i <= ilvl; i++) {
        fz_xml* lv = word_find_abstract_lvl(info, abs_id, i);
        int st = 1;
        fz_xml* sn = lv ? fz_xml_find_down(lv, "start") : NULL;
        const char* sv = sn ? fz_xml_att_alt(sn, "w:val", "val") : NULL;
        if (sv) st = fz_atoi(sv);
        if (st < 1) st = 1;
        fmts[i] = "decimal";
        if (lv) {
            fz_xml* fn = fz_xml_find_down(lv, "numFmt");
            const char* fv = fn ? fz_xml_att_alt(fn, "w:val", "val") : NULL;
            if (fv) fmts[i] = fv;
        }
        if (i < ilvl) {
            if (counts[i] == 0) counts[i] = st;
        } else if (counts[i] == 0)
            counts[i] = st;
        else
            counts[i]++;
    }
    for (i = ilvl + 1; i < WORD_NUM_ILVL_MAX; i++) counts[i] = 0;

    lvl = word_find_abstract_lvl(info, abs_id, ilvl);
    if (!lvl) return 0;
    start_n = fz_xml_find_down(lvl, "start");
    (void)start_n;
    fmt_n = fz_xml_find_down(lvl, "numFmt");
    fmt = fmt_n ? fz_xml_att_alt(fmt_n, "w:val", "val") : "decimal";
    if (!fmt) fmt = "decimal";
    text_n = fz_xml_find_down(lvl, "lvlText");
    lvl_text = text_n ? fz_xml_att_alt(text_n, "w:val", "val") : "%1.";
    if (!lvl_text) lvl_text = "%1.";

    ppr = fz_xml_find_down(lvl, "pPr");
    ind = ppr ? fz_xml_find_down(ppr, "ind") : NULL;
    if (ind) {
        const char* left = fz_xml_att_alt(ind, "w:left", "left");
        const char* start = fz_xml_att_alt(ind, "w:start", "start");
        const char* hang = fz_xml_att_alt(ind, "w:hanging", "hanging");
        const char* fl = fz_xml_att_alt(ind, "w:firstLine", "firstLine");
        int tw;
        if (!left) left = start;
        if (left && left_pt) {
            tw = fz_atoi(left);
            if (tw > 0) *left_pt = (float)tw / 20.0f;
        }
        if (hang && hanging_pt) {
            tw = fz_atoi(hang);
            if (tw > 0) *hanging_pt = (float)tw / 20.0f;
        }
        if (fl && first_pt) {
            tw = fz_atoi(fl);
            if (tw > 0) *first_pt = (float)tw / 20.0f;
        }
    }

    for (i = 0; i <= ilvl; i++) {
        word_format_list_value(fmts[i], counts[i], value_bufs[i], (int)sizeof value_bufs[i]);
        values[i] = value_bufs[i];
    }
    for (; i < WORD_NUM_ILVL_MAX; i++) values[i] = "";

    if (marker && marker_cap > 0) {
        if (fmt && !strcmp(fmt, "bullet"))
            fz_snprintf(marker, marker_cap, "•");
        else
            word_expand_lvl_text(lvl_text, values, ilvl + 1, marker, marker_cap);
        {
            int L = (int)strlen(marker);
            if (L > 0 && L < marker_cap - 2) {
                unsigned char c = (unsigned char)marker[L - 1];
                if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c >= 0x80) {
                    marker[L] = ' ';
                    marker[L + 1] = 0;
                }
            }
        }
    }
    return marker && marker[0] ? 1 : 0;
}

/* First useful bookmark name on a paragraph (prefer _Toc*, then user names). */
static const char* paragraph_bookmark_name(fz_xml* p) {
    fz_xml* n;
    const char* fallback = NULL;

    for (n = fz_xml_down(p); n; n = fz_xml_next(n)) {
        if (!fz_xml_is_tag(n, "bookmarkStart")) continue;
        {
            const char* name = fz_xml_att_alt(n, "w:name", "name");
            if (!name || !name[0]) continue;
            if (!strcmp(name, "_GoBack")) continue;
            if (!strncmp(name, "_Toc", 4)) return name;
            if (name[0] != '_' && !fallback) fallback = name;
        }
    }
    return fallback;
}

static int is_user_word_bookmark_name(const char* name) {
    if (!name || !name[0]) return 0;
    if (name[0] == '_') return 0; /* _Toc*, _Hlk*, _GoBack, … */
    return 1;
}

static void doc_escape_ex(fz_context* ctx, fz_output* output, const char* str_, int preserve_space) {
    const unsigned char* str = (const unsigned char*)str_;
    int c;

    if (!str) return;

    while ((c = *str++) != 0) {
        if (c == '&') {
            fz_write_string(ctx, output, "&amp;");
        } else if (c == '<') {
            fz_write_string(ctx, output, "&lt;");
        } else if (c == '>') {
            fz_write_string(ctx, output, "&gt;");
        } else if (preserve_space && c == ' ') {
            /* Word xml:space="preserve" pads 发文机关/日期 to the right with spaces;
             * HTML otherwise collapses them and the block looks left-aligned. */
            fz_write_string(ctx, output, "&nbsp;");
        } else if (preserve_space && c == '\t') {
            fz_write_string(ctx, output, "&nbsp;&nbsp;&nbsp;&nbsp;");
        } else {
            /* We get utf-8 in, just parrot it out again. */
            fz_write_byte(ctx, output, c);
        }
    }
}

static void doc_escape(fz_context* ctx, fz_output* output, const char* str_) {
    doc_escape_ex(ctx, output, str_, 0);
}

/* Emit UTF-8 text one glyph per line (Word table textDirection vertical). */
static void doc_escape_vertical(fz_context* ctx, fz_output* output, const char* str_) {
    const unsigned char* s = (const unsigned char*)str_;
    int first = 1;

    if (!s) return;
    while (*s) {
        char glyph[8];
        int n = 1;
        unsigned char c = *s;
        if (c < 0x80)
            n = 1;
        else if ((c & 0xE0) == 0xC0)
            n = 2;
        else if ((c & 0xF0) == 0xE0)
            n = 3;
        else if ((c & 0xF8) == 0xF0)
            n = 4;
        if (n > 1) {
            int i;
            for (i = 1; i < n; i++) {
                if ((s[i] & 0xC0) != 0x80) {
                    n = 1;
                    break;
                }
            }
        }
        /* Skip ASCII '(' noise sometimes embedded in vertical form labels. */
        if (n == 1 && (c == '(' || c == ')')) {
            s += n;
            continue;
        }
        if (n == 1 && (c == ' ' || c == '\t')) {
            s += n;
            continue;
        }
        if (!first) fz_write_string(ctx, output, "<br/>");
        first = 0;
        if (n >= (int)sizeof glyph) n = 1;
        memcpy(glyph, s, (size_t)n);
        glyph[n] = 0;
        doc_escape_ex(ctx, output, glyph, 0);
        s += n;
    }
}

static void show_text(fz_context* ctx, fz_xml* top, doc_info* info) {
    fz_xml* pos = top;
    fz_xml* next;
    int preserve = 0;
    const char* sp;

    /* xml:space may be stored as "space" after namespace strip. */
    sp = fz_xml_att_alt(top, "xml:space", "space");
    if (sp && !strcmp(sp, "preserve")) preserve = 1;
    /* 落款: strip padding spaces and right-align instead of nbsp-indent. */
    if (info && info->signoff_emit) preserve = 0;

    while (pos) {
        const char* text = fz_xml_text(pos);
        if (text && info && info->skip_leading_ws) {
            while (*text == ' ' || *text == '\t') text++;
            while (word_utf8_eq(text, "　")) text += 3;
            if (*text) info->skip_leading_ws = 0;
        }
        if (text && info && info->emit_vertical_breaks)
            doc_escape_vertical(ctx, info->out, text);
        else
            doc_escape_ex(ctx, info->out, text, preserve);

        if (fz_xml_is_tag(pos, "lineBreak")) {
            fz_write_string(ctx, info->out, "\n");
        } else if (fz_xml_is_tag(pos, "tab")) {
            if (!(info && info->skip_leading_ws)) {
                if (preserve)
                    fz_write_string(ctx, info->out, "&nbsp;&nbsp;&nbsp;&nbsp;");
                else if (!(info && info->signoff_emit))
                    fz_write_string(ctx, info->out, "\t");
            }
        } else if (fz_xml_is_tag(pos, "lastRenderedPageBreak")) {
            info->page++;
        }

        /* Always try to move down. */
        next = fz_xml_down(pos);
        if (next) {
            pos = next;
            continue;
        }

        if (pos == top) break;

        next = fz_xml_next(pos);
        if (next) {
            pos = next;
            continue;
        }

        while (1) {
            pos = fz_xml_up(pos);
            if (pos == top) pos = NULL;
            if (pos == NULL) break;
            if (fz_xml_is_tag(pos, "p")) {
                fz_write_string(ctx, info->out, "\n");
            }
            next = fz_xml_next(pos);
            if (next) {
                pos = next;
                break;
            }
        }
    }
}

static void show_footnote(fz_context* ctx, fz_xml* v, doc_info* info) {
    int n = fz_atoi(fz_xml_att(v, "w:id"));

    if (n < 0 || n >= info->footnotes_max) return;

    if (info->footnotes[n] == NULL || info->footnotes[n][0] == 0) return;

    /* Then send the strings. */
    doc_escape(ctx, info->out, info->footnotes[n]);
}

static char* lookup_rel(fz_context* ctx, fz_xml* rels, const char* id);
static char* make_absolute_path(fz_context* ctx, const char* abs, const char* rel);

/* Word OOXML helpers: namespaces are stripped from tags; attrs often keep prefixes. */

static int rpr_flag_on(fz_xml* rpr, const char* tag) {
    fz_xml* n;
    const char* val;

    if (!rpr) return 0;
    n = fz_xml_find_down(rpr, tag);
    if (!n) return 0;
    val = fz_xml_att_alt(n, "w:val", "val");
    if (val && (!strcmp(val, "0") || !strcmp(val, "false") || !strcmp(val, "off") || !strcmp(val, "none"))) return 0;
    return 1;
}

static const char* jc_to_css_align(const char* jc) {
    if (!jc) return NULL;
    if (!strcmp(jc, "center")) return "center";
    if (!strcmp(jc, "right") || !strcmp(jc, "end")) return "right";
    if (!strcmp(jc, "both") || !strcmp(jc, "distribute")) return "justify";
    if (!strcmp(jc, "left") || !strcmp(jc, "start")) return "left";
    return NULL;
}

/* Word w:spacing → CSS margin-top/bottom + line-height. */
static void word_ppr_spacing(fz_xml* ppr, float* before_pt, float* after_pt, float* line_mult, float* line_pt) {
    fz_xml* sp;
    const char* v;
    const char* rule;

    if (before_pt) *before_pt = 0;
    if (after_pt) *after_pt = 0;
    if (line_mult) *line_mult = 0;
    if (line_pt) *line_pt = 0;
    if (!ppr) return;
    sp = fz_xml_find_down(ppr, "spacing");
    if (!sp) return;
    v = fz_xml_att_alt(sp, "w:before", "before");
    if (v && before_pt) {
        int n = fz_atoi(v);
        if (n > 0) *before_pt = (float)n / 20.0f;
    }
    v = fz_xml_att_alt(sp, "w:after", "after");
    if (v && after_pt) {
        int n = fz_atoi(v);
        if (n > 0) *after_pt = (float)n / 20.0f;
    }
    v = fz_xml_att_alt(sp, "w:line", "line");
    rule = fz_xml_att_alt(sp, "w:lineRule", "lineRule");
    if (v) {
        int n = fz_atoi(v);
        if (n > 0) {
            if (!rule || !strcmp(rule, "auto")) {
                /* 240 = single spacing */
                if (line_mult) *line_mult = (float)n / 240.0f;
            } else if (line_pt) {
                /* exact / atLeast: twips */
                *line_pt = (float)n / 20.0f;
            }
        }
    }
}

/* Word w:ind: firstLine is twips (1/20 pt, layout truth); firstLineChars is
 * hundredths of a char (authoring). Prefer twips so indent stays 2 汉字 wide
 * even when <p> CSS font-size differs from run sz (em would look like ~1.5). */
static void word_ppr_text_indent(fz_xml* ppr, float* em_out, float* pt_out, float* left_pt_out, float* hanging_pt_out) {
    fz_xml* ind;
    const char* flc;
    const char* fl;
    const char* left;
    const char* start;
    const char* hang;
    int n;

    if (em_out) *em_out = 0;
    if (pt_out) *pt_out = 0;
    if (left_pt_out) *left_pt_out = 0;
    if (hanging_pt_out) *hanging_pt_out = 0;
    if (!ppr) return;
    ind = fz_xml_find_down(ppr, "ind");
    if (!ind) return;
    fl = fz_xml_att_alt(ind, "w:firstLine", "firstLine");
    flc = fz_xml_att_alt(ind, "w:firstLineChars", "firstLineChars");
    left = fz_xml_att_alt(ind, "w:left", "left");
    start = fz_xml_att_alt(ind, "w:start", "start");
    hang = fz_xml_att_alt(ind, "w:hanging", "hanging");
    if (!left) left = start;
    if (fl) {
        n = fz_atoi(fl);
        if (n > 0 && pt_out) *pt_out = (float)n / 20.0f;
    }
    if ((!pt_out || *pt_out <= 0) && flc) {
        n = fz_atoi(flc);
        if (n > 0 && em_out) *em_out = (float)n / 100.0f;
    }
    if (left && left_pt_out) {
        n = fz_atoi(left);
        if (n > 0) *left_pt_out = (float)n / 20.0f;
    }
    if (hang && hanging_pt_out) {
        n = fz_atoi(hang);
        if (n > 0) *hanging_pt_out = (float)n / 20.0f;
    }
}

/* Half-points in w:sz / w:szCs → pt. Prefer sz (CJK body) then szCs. */
static float word_rpr_font_pt(fz_xml* rpr) {
    fz_xml* sz_n;
    const char* sv;

    if (!rpr) return 0;
    sz_n = fz_xml_find_down(rpr, "sz");
    if (!sz_n) sz_n = fz_xml_find_down(rpr, "szCs");
    if (!sz_n) return 0;
    sv = fz_xml_att_alt(sz_n, "w:val", "val");
    if (!sv || !sv[0]) return 0;
    return (float)fz_atoi(sv) / 2.0f;
}

/* Resolve run font size from styles.xml rPr (then basedOn). */
static float word_style_resolve_font_pt(doc_info* info, const char* style_id) {
    int depth;
    const char* sid = style_id;

    if (!info || !info->styles_xml || !sid || !sid[0]) return 0;
    for (depth = 0; depth < 8 && sid && sid[0]; depth++) {
        fz_xml* st = fz_xml_find_dfs(info->styles_xml, "style", NULL, NULL);
        fz_xml* found = NULL;
        while (st) {
            const char* id = fz_xml_att_alt(st, "w:styleId", "styleId");
            if (id && !strcmp(id, sid)) {
                found = st;
                break;
            }
            st = fz_xml_find_next_dfs(st, "style", NULL, NULL);
        }
        if (!found) return 0;
        {
            float pt = word_rpr_font_pt(fz_xml_find_down(found, "rPr"));
            if (pt > 0.5f) return pt;
            {
                fz_xml* based = fz_xml_find_down(found, "basedOn");
                sid = based ? fz_xml_att_alt(based, "w:val", "val") : NULL;
            }
        }
    }
    return 0;
}

/* Read eastAsia / ascii(+hAnsi) from an rPr/rFonts node (theme placeholders cleared). */
static void word_rpr_font_names(doc_info* info, fz_xml* rpr, const char** east_out, const char** latin_out) {
    fz_xml* rfonts;
    const char* east = NULL;
    const char* latin = NULL;

    if (east_out) *east_out = NULL;
    if (latin_out) *latin_out = NULL;
    if (!rpr) return;
    rfonts = fz_xml_find_down(rpr, "rFonts");
    if (!rfonts) return;
    east = fz_xml_att_alt(rfonts, "w:eastAsia", "eastAsia");
    latin = fz_xml_att_alt(rfonts, "w:ascii", "ascii");
    if (!latin) latin = fz_xml_att_alt(rfonts, "w:hAnsi", "hAnsi");
    if (word_font_is_lang_placeholder(east)) {
        const char* th = fz_xml_att_alt(rfonts, "w:eastAsiaTheme", "eastAsiaTheme");
        east = NULL;
        if (th && !strncmp(th, "major", 5))
            east = info ? info->theme_major_ea : NULL;
        else
            east = info ? info->theme_minor_ea : NULL;
    } else if (!east) {
        const char* th = fz_xml_att_alt(rfonts, "w:eastAsiaTheme", "eastAsiaTheme");
        if (th && !strncmp(th, "major", 5))
            east = info ? info->theme_major_ea : NULL;
        else if (th)
            east = info ? info->theme_minor_ea : NULL;
    }
    if (word_font_is_lang_placeholder(latin)) latin = NULL;
    if (east_out) *east_out = east;
    if (latin_out) *latin_out = latin;
}

static void word_copy_font_name(char* dst, int dst_sz, const char* src) {
    if (!dst || dst_sz <= 0) return;
    dst[0] = 0;
    if (!src || !src[0]) return;
    fz_strlcpy(dst, src, dst_sz);
}

/* Resolve eastAsia (+ latin) from styles.xml rPr, then basedOn — mirrors font-size resolve. */
static void word_style_resolve_fonts(doc_info* info, const char* style_id, char* ea_buf, int ea_sz, char* lat_buf,
                                     int lat_sz) {
    int depth;
    const char* sid = style_id;

    if (ea_buf && ea_sz > 0) ea_buf[0] = 0;
    if (lat_buf && lat_sz > 0) lat_buf[0] = 0;
    if (!info || !info->styles_xml || !sid || !sid[0]) return;
    for (depth = 0; depth < 8 && sid && sid[0]; depth++) {
        fz_xml* st = fz_xml_find_dfs(info->styles_xml, "style", NULL, NULL);
        fz_xml* found = NULL;
        while (st) {
            const char* id = fz_xml_att_alt(st, "w:styleId", "styleId");
            if (id && !strcmp(id, sid)) {
                found = st;
                break;
            }
            st = fz_xml_find_next_dfs(st, "style", NULL, NULL);
        }
        if (!found) return;
        {
            const char* ea = NULL;
            const char* lat = NULL;
            word_rpr_font_names(info, fz_xml_find_down(found, "rPr"), &ea, &lat);
            if (ea && ea[0] && ea_buf && ea_sz > 0 && !ea_buf[0]) word_copy_font_name(ea_buf, ea_sz, ea);
            if (lat && lat[0] && lat_buf && lat_sz > 0 && !lat_buf[0]) word_copy_font_name(lat_buf, lat_sz, lat);
            if ((ea_buf && ea_buf[0]) || (lat_buf && lat_buf[0])) {
                /* Prefer first style that declares either face; keep walking only if one side empty. */
                if ((!ea_buf || ea_buf[0]) && (!lat_buf || lat_buf[0])) return;
            }
            {
                fz_xml* based = fz_xml_find_down(found, "basedOn");
                sid = based ? fz_xml_att_alt(based, "w:val", "val") : NULL;
            }
        }
    }
}

/* Resolve paragraph indent from styles.xml (direct ind, else basedOn chain). */
static void word_style_resolve_indent(doc_info* info, const char* style_id, float* em_out, float* pt_out,
                                      float* left_pt_out, float* hanging_pt_out) {
    int depth;
    const char* sid = style_id;

    if (em_out) *em_out = 0;
    if (pt_out) *pt_out = 0;
    if (left_pt_out) *left_pt_out = 0;
    if (hanging_pt_out) *hanging_pt_out = 0;
    if (!info || !info->styles_xml || !sid || !sid[0]) return;

    for (depth = 0; depth < 8 && sid && sid[0]; depth++) {
        fz_xml* st = fz_xml_find_dfs(info->styles_xml, "style", NULL, NULL);
        fz_xml* found = NULL;
        while (st) {
            const char* id = fz_xml_att_alt(st, "w:styleId", "styleId");
            if (id && !strcmp(id, sid)) {
                found = st;
                break;
            }
            st = fz_xml_find_next_dfs(st, "style", NULL, NULL);
        }
        if (!found) return;
        {
            fz_xml* ppr = fz_xml_find_down(found, "pPr");
            fz_xml* ind = ppr ? fz_xml_find_down(ppr, "ind") : NULL;
            if (ind) {
                word_ppr_text_indent(ppr, em_out, pt_out, left_pt_out, hanging_pt_out);
                return;
            }
            {
                fz_xml* based = fz_xml_find_down(found, "basedOn");
                const char* next = based ? fz_xml_att_alt(based, "w:val", "val") : NULL;
                sid = next;
            }
        }
    }
}

/* Space-padded short lines (发文机关 / 日期) — Word fakes right align with spaces. */
static int word_looks_like_signoff(const char* plain) {
    const char* s = plain;
    const char* end;
    int lead = 0;
    int rest;

    if (!s) return 0;
    while (*s == ' ' || *s == '\t') {
        lead++;
        s++;
    }
    while (word_utf8_eq(s, "　")) {
        lead++;
        s += 3;
    }
    if (lead < 8) return 0;
    if (!s[0]) return 0;
    rest = (int)strlen(s);
    if (rest > 64) return 0;

    /* 2021年3月  日 */
    if (strstr(s, "年") && strstr(s, "月")) return 1;

    /* …厅 / …局 / …部 / …委 / …办 / …政府 / …办公室 */
    end = s + rest;
    if (rest >= 3) {
        const char* t = end - 3;
        if (word_utf8_eq(t, "厅") || word_utf8_eq(t, "局") || word_utf8_eq(t, "部") || word_utf8_eq(t, "委") ||
            word_utf8_eq(t, "办"))
            return 1;
    }
    if (rest >= 6 && word_utf8_eq(end - 6, "政府")) return 1;
    if (rest >= 9 && word_utf8_eq(end - 9, "办公室")) return 1;
    /* Generic: heavily padded short line */
    if (lead >= 16 && rest <= 40) return 1;
    return 0;
}

/* Approximate advance width in em (CJK ≈ 1em, ASCII ≈ 0.5em). */
static float word_plain_visual_em(const char* plain) {
    const char* s = word_skip_leading_space(plain);
    float w = 0;

    if (!s) return 0;
    while (*s) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x80) {
            if (*s == '\r' || *s == '\n') {
                s++;
                continue;
            }
            w += 0.5f;
            s++;
        } else if ((c & 0xE0) == 0xC0 && s[1]) {
            w += 1.0f;
            s += 2;
        } else if ((c & 0xF0) == 0xE0 && s[1] && s[2]) {
            w += 1.0f;
            s += 3;
        } else if ((c & 0xF8) == 0xF0 && s[1] && s[2] && s[3]) {
            w += 1.0f;
            s += 4;
        } else
            s++;
    }
    return w;
}

/* Same detection path as emit_paragraph's signoff branch (no Heading / no jc). */
static int word_paragraph_is_signoff(fz_xml* p, doc_info* info) {
    fz_xml* ppr;
    fz_xml* pstyle;
    fz_xml* jc;
    const char* style_val = NULL;
    const char* align = NULL;
    int heading_level = 0;
    char plain[512];

    if (!p || (info && info->in_table)) return 0;
    ppr = fz_xml_find_down(p, "pPr");
    if (ppr) {
        pstyle = fz_xml_find_down(ppr, "pStyle");
        if (pstyle) style_val = fz_xml_att_alt(pstyle, "w:val", "val");
        jc = fz_xml_find_down(ppr, "jc");
        if (jc) align = jc_to_css_align(fz_xml_att_alt(jc, "w:val", "val"));
    }
    if (align) return 0;
    if (style_val) heading_level = word_style_heading_level(info, style_val);
    word_paragraph_plain_text(p, plain, (int)sizeof plain);
    if (heading_level == 0) heading_level = word_infer_text_heading_level(plain);
    if (heading_level != 0) return 0;
    return word_looks_like_signoff(plain);
}

static void close_word_list(fz_context* ctx, doc_info* info) {
    if (info->list_kind == 1)
        fz_write_string(ctx, info->out, "</ul>\n");
    else if (info->list_kind == 2)
        fz_write_string(ctx, info->out, "</ol>\n");
    info->list_kind = 0;
}

static void emit_page_break(fz_context* ctx, doc_info* info) {
    if (!info->do_pages) return;
    /* Never split HTML mid-table or mid-paragraph. Word's lastRenderedPageBreak
     * often sits inside a run (even mid-word). Emitting </div><div id=pageN>
     * there orphans open <span style="font-size"> / font-family / <b> tags, so
     * the continuation loses 字号/字体 until stray closers. Tables: same break
     * also orphans rowspan borders. MuPDF reflow paginates the continuous flow. */
    if (info->in_table > 0) return;
    if (info->in_paragraph > 0) return;
    if (info->page) fz_write_string(ctx, info->out, "\n</div>\n");
    info->page++;
    fz_write_printf(ctx, info->out, "<div id=\"page%d\">\n", info->page);
}

static void emit_drawing(fz_context* ctx, fz_xml* drawing, doc_info* info) {
    fz_xml* blip;
    fz_xml* extent;
    const char* embed;
    char* target;
    char* path = NULL;
    int wpx = 0, hpx = 0;

    blip = fz_xml_find_dfs(drawing, "blip", NULL, NULL);
    if (!blip) return;

    embed = fz_xml_att_alt(blip, "r:embed", "embed");
    if (!embed || !info->doc_rels || !info->doc_file) return;

    target = lookup_rel(ctx, info->doc_rels, embed);
    if (!target) return;

    path = make_absolute_path(ctx, info->doc_file, target);

    fz_try(ctx) {
        extent = fz_xml_find_dfs(drawing, "extent", NULL, NULL);
        if (extent) {
            const char* cx_s = fz_xml_att(extent, "cx");
            const char* cy_s = fz_xml_att(extent, "cy");
            /* EMUs: 914400 per inch. Layout must use this box, not the bitmap's
             * pixel size — height:auto blows a scaled screenshot up to a page
             * and pushes 落款 onto the next page. */
            if (cx_s) wpx = (int)((fz_atoi64(cx_s) * 96) / 914400);
            if (cy_s) hpx = (int)((fz_atoi64(cy_s) * 96) / 914400);
        }

        fz_write_string(ctx, info->out, "<img src=\"");
        doc_escape(ctx, info->out, path);
        fz_write_string(ctx, info->out, "\" alt=\"\"");
        if (wpx > 0 && hpx > 0) {
            float wpt = (float)wpx * 72.0f / 96.0f;
            float hpt = (float)hpx * 72.0f / 96.0f;
            if (info && info->page_content_pt > 40.0f && wpt > info->page_content_pt) {
                hpt *= info->page_content_pt / wpt;
                wpt = info->page_content_pt;
            }
            /* No width/height attributes: MuPDF treats those numbers as pt,
             * so a 96dpi px value is 4/3 too tall and the 落款 drops a page.
             * Inline pt also beats img{height:auto}. */
            fz_write_printf(ctx, info->out, " style=\"width:%.1fpt;height:%.1fpt;max-width:100%%;margin:0;\"", wpt,
                            hpt);
        } else
            fz_write_string(ctx, info->out, " style=\"max-width:100%;height:auto;\"");
        fz_write_string(ctx, info->out, "/>");
    }
    fz_always(ctx) fz_free(ctx, path);
    fz_catch(ctx) fz_rethrow(ctx);
}

/* Legacy VML pictures (Word 2003-compat drawings inside OOXML). */
static void emit_pict(fz_context* ctx, fz_xml* pict, doc_info* info) {
    fz_xml* imagedata;
    const char* embed;
    char* target;
    char* path = NULL;

    imagedata = fz_xml_find_dfs(pict, "imagedata", NULL, NULL);
    if (!imagedata) return;

    embed = fz_xml_att_alt(imagedata, "r:id", "id");
    if (!embed) embed = fz_xml_att_alt(imagedata, "r:embed", "embed");
    if (!embed || !info->doc_rels || !info->doc_file) return;

    target = lookup_rel(ctx, info->doc_rels, embed);
    if (!target) return;

    path = make_absolute_path(ctx, info->doc_file, target);
    fz_try(ctx) {
        fz_write_string(ctx, info->out, "<img src=\"");
        doc_escape(ctx, info->out, path);
        fz_write_string(ctx, info->out, "\" alt=\"\" style=\"max-width:100%;height:auto;\"/>");
    }
    fz_always(ctx) fz_free(ctx, path);
    fz_catch(ctx) fz_rethrow(ctx);
}

static void emit_paragraph(fz_context* ctx, fz_xml* p, doc_info* info);
static void emit_table(fz_context* ctx, fz_xml* tbl, doc_info* info);

static void emit_hex_color_span(fz_context* ctx, doc_info* info, const char* hex) {
    /* Word stores "auto" or RRGGBB without '#'. */
    if (!hex || !hex[0] || !strcmp(hex, "auto")) return;
    if (hex[0] == '#')
        fz_write_printf(ctx, info->out, "<span style=\"color:%s\">", hex);
    else
        fz_write_printf(ctx, info->out, "<span style=\"color:#%s\">", hex);
}

/* WPS/Word stores a soft line end as its own space run (xml:space="preserve")
 * between CJK, e.g. 贯彻| 习 and 成| 果. Emitting that as &nbsp; glues the next
 * character to a non-breaking gap, so the line breaks one glyph early and the
 * leftover (省) sits alone. A real Latin space (Word 2010) is not between Han. */
static int word_u_is_han_or_fw(int c) {
    if (c >= 0x4E00 && c <= 0x9FFF) return 1;
    if (c >= 0x3400 && c <= 0x4DBF) return 1;
    if (c >= 0x3000 && c <= 0x303F) return 1;
    if (c >= 0xFF00 && c <= 0xFFEF) return 1;
    return 0;
}

static int word_u_is_cjk_neighbor(int c) {
    if (c >= '0' && c <= '9') return 1;
    return word_u_is_han_or_fw(c);
}

/* First (last=0) or last non-space character in the run. */
static int word_run_boundary_char(fz_xml* r, int last, int* out_c) {
    fz_xml* c;
    int found = 0;
    int ch = 0;

    if (!r || !out_c) return 0;
    for (c = fz_xml_down(r); c; c = fz_xml_next(c)) {
        fz_xml* td;
        const char* tx;
        if (!fz_xml_is_tag(c, "t")) continue;
        td = fz_xml_down(c);
        tx = td ? fz_xml_text(td) : NULL;
        if (!tx) continue;
        while (tx[0]) {
            int u = 0;
            int n = fz_chartorune(&u, tx);
            if (n <= 0) break;
            tx += n;
            if (u == ' ' || u == '\t' || u == '\n' || u == '\r' || u == 0x00A0 || u == 0x3000) continue;
            ch = u;
            found = 1;
            if (!last) {
                *out_c = ch;
                return 1;
            }
        }
    }
    if (!found) return 0;
    *out_c = ch;
    return 1;
}

static int word_run_is_ascii_space_only(fz_xml* r) {
    fz_xml* c;
    int any = 0;

    if (!r) return 0;
    for (c = fz_xml_down(r); c; c = fz_xml_next(c)) {
        fz_xml* td;
        const char* tx;
        if (fz_xml_is_tag(c, "rPr")) continue;
        if (!fz_xml_is_tag(c, "t")) return 0;
        td = fz_xml_down(c);
        tx = td ? fz_xml_text(td) : NULL;
        if (!tx || !tx[0]) continue;
        any = 1;
        while (tx[0] == ' ') tx++;
        if (tx[0]) return 0;
    }
    return any;
}

static int word_run_spacing_twips(fz_xml* r) {
    fz_xml* rpr;
    fz_xml* spn;
    const char* sv;

    rpr = r ? fz_xml_find_down(r, "rPr") : NULL;
    spn = rpr ? fz_xml_find_down(rpr, "spacing") : NULL;
    sv = spn ? fz_xml_att_alt(spn, "w:val", "val") : NULL;
    if (!sv || !sv[0]) return 0;
    return fz_atoi(sv);
}

static fz_xml* word_adjacent_text_run(fz_xml* r, int forward) {
    fz_xml* n;

    n = r ? (forward ? fz_xml_next(r) : fz_xml_prev(r)) : NULL;
    while (n) {
        int ch = 0;
        if (fz_xml_is_tag(n, "r") && word_run_boundary_char(n, forward ? 0 : 1, &ch)) return n;
        n = forward ? fz_xml_next(n) : fz_xml_prev(n);
    }
    return NULL;
}

static int word_run_is_cjk_linebreak_space(fz_xml* r) {
    int sp;
    int left_c = 0;
    int right_c = 0;
    fz_xml* left;
    fz_xml* right;

    if (!word_run_is_ascii_space_only(r)) return 0;
    sp = word_run_spacing_twips(r);
    /* Large negative spacing is a tracking shim (already dropped). A wide
     * positive gap is intentional, not a saved line end. */
    if (sp < -39 || sp > 40) return 0;
    left = word_adjacent_text_run(r, 0);
    right = word_adjacent_text_run(r, 1);
    if (!word_run_boundary_char(left, 1, &left_c)) return 0;
    if (!word_run_boundary_char(right, 0, &right_c)) return 0;
    if (!word_u_is_cjk_neighbor(left_c) || !word_u_is_cjk_neighbor(right_c)) return 0;
    if (!word_u_is_han_or_fw(left_c) && !word_u_is_han_or_fw(right_c)) return 0;
    return 1;
}

static int word_run_is_collapsed_space(fz_xml* r) {
    fz_xml* rpr0;
    fz_xml* spn;
    fz_xml* c;
    const char* sv;
    int only_space = 0;
    int any_t = 0;

    if (!r) return 0;
    rpr0 = fz_xml_find_down(r, "rPr");
    spn = rpr0 ? fz_xml_find_down(rpr0, "spacing") : NULL;
    sv = spn ? fz_xml_att_alt(spn, "w:val", "val") : NULL;
    if (!sv || fz_atoi(sv) > -40) return 0;
    for (c = fz_xml_down(r); c; c = fz_xml_next(c)) {
        fz_xml* td;
        const char* tx;
        if (!fz_xml_is_tag(c, "t")) continue;
        any_t = 1;
        td = fz_xml_down(c);
        tx = td ? fz_xml_text(td) : NULL;
        if (!tx) continue;
        while (*tx == ' ' || *tx == '\t') tx++;
        if (word_utf8_eq(tx, "　")) tx += 3;
        if (*tx) return 0;
        only_space = 1;
    }
    return any_t && only_space;
}

static void emit_run(fz_context* ctx, fz_xml* r, doc_info* info) {
    fz_xml* rpr;
    fz_xml* n;
    fz_xml* sz_n;
    fz_xml* color_n;
    int bold = 0, italic = 0, underline = 0, strike = 0;
    int opened_color = 0;
    int opened_size = 0;
    int opened_font = 0;
    const char* inline_style = NULL;
    const char* color_hex = NULL;
    const char* east = NULL;
    const char* latin = NULL;
    float font_pt = 0;

    /* A space run with large negative w:spacing is a tracking shim (附 件 → 附件).
     * A modest space run between CJK is WPS's saved line end, not a word space. */
    if (word_run_is_collapsed_space(r) || word_run_is_cjk_linebreak_space(r)) return;

    rpr = fz_xml_find_down(r, "rPr");
    if (rpr) {
        fz_xml* rstyle = fz_xml_find_down(rpr, "rStyle");
        bold = rpr_flag_on(rpr, "b") || rpr_flag_on(rpr, "bCs");
        italic = rpr_flag_on(rpr, "i") || rpr_flag_on(rpr, "iCs");
        underline = rpr_flag_on(rpr, "u");
        strike = rpr_flag_on(rpr, "strike") || rpr_flag_on(rpr, "dstrike");
        if (rstyle) {
            const char* val = fz_xml_att_alt(rstyle, "w:val", "val");
            if (val && !strcmp(val, "VerbatimChar")) inline_style = "tt";
        }
        sz_n = fz_xml_find_down(rpr, "sz");
        if (!sz_n) sz_n = fz_xml_find_down(rpr, "szCs");
        if (sz_n) {
            const char* sv = fz_xml_att_alt(sz_n, "w:val", "val");
            /* Half-points → pt */
            if (sv) font_pt = (float)fz_atoi(sv) / 2.0f;
        }
        color_n = fz_xml_find_down(rpr, "color");
        if (color_n) color_hex = fz_xml_att_alt(color_n, "w:val", "val");
        word_rpr_font_names(info, rpr, &east, &latin);
    }

    /* Run omitted w:sz — use paragraph style / pPr size (e.g. 公文 recipient). */
    if (font_pt < 0.5f && info && info->run_default_font_pt > 0.5f) font_pt = info->run_default_font_pt;
    /* Run omitted w:rFonts — inherit paragraph / style eastAsia (仿宋、小标宋). */
    if (info) {
        if (!east && info->run_default_ea[0]) east = info->run_default_ea;
        if (!latin && info->run_default_latin[0]) latin = info->run_default_latin;
    }

    /* Word w:w character scale (%). HTML has no true horizontal glyph scale; for
     * oversized runs approximate by shrinking font-size so 红头 stays one line. */
    if (rpr && font_pt > 24.0f) {
        fz_xml* w_n = fz_xml_find_down(rpr, "w");
        const char* wv = w_n ? fz_xml_att_alt(w_n, "w:val", "val") : NULL;
        if (wv && wv[0]) {
            int scale = fz_atoi(wv);
            if (scale > 0 && scale < 100) font_pt = font_pt * ((float)scale / 100.0f);
        }
    }
    if (info && info->force_run_font_pt > 12.0f) font_pt = info->force_run_font_pt;

    if (east || latin) {
        fz_write_string(ctx, info->out, "<span style='");
        if (!word_emit_font_family_css(ctx, info, info->out, east, latin))
            fz_write_string(ctx, info->out, "font-family:serif");
        fz_write_string(ctx, info->out, "'>");
        opened_font = 1;
    }

    if (color_hex) {
        /* emit_hex_color_span no-ops for "auto"/empty — only mark opened when emitted. */
        if (color_hex[0] && strcmp(color_hex, "auto") != 0) {
            emit_hex_color_span(ctx, info, color_hex);
            opened_color = 1;
        }
    }
    {
        float body_pt = (info && info->doc_body_font_pt > 0.5f) ? info->doc_body_font_pt : 12.0f;
        float d = font_pt - body_pt;
        if (d < 0) d = -d;
        /* Skip spans that match the document body size (already on <body>). */
        if (font_pt > 0.5f && d > 0.6f) {
            fz_write_printf(ctx, info->out, "<span style=\"font-size:%.1fpt\">", font_pt);
            opened_size = 1;
        }
    }
    if (inline_style) fz_write_printf(ctx, info->out, "<%s>", inline_style);
    if (bold) fz_write_string(ctx, info->out, "<b>");
    if (italic) fz_write_string(ctx, info->out, "<i>");
    if (underline) fz_write_string(ctx, info->out, "<u>");
    if (strike) fz_write_string(ctx, info->out, "<s>");

    for (n = fz_xml_down(r); n; n = fz_xml_next(n)) {
        if (fz_xml_is_tag(n, "rPr")) continue;
        if (fz_xml_is_tag(n, "t")) {
            /* Word sometimes packs "2.标题 3.标题" into one paragraph. The second
             * marker is its own run ("3."); break so each item keeps the same left edge. */
            fz_xml* td = fz_xml_down(n);
            const char* tx = td ? fz_xml_text(td) : NULL;
            const char* mk = tx ? word_skip_leading_space(tx) : NULL;
            int d = 0;
            int marker = 0;
            if (mk && mk[0] >= '1' && mk[0] <= '9') {
                while (mk[d] >= '0' && mk[d] <= '9' && d < 3) d++;
                if (d > 0 && mk[d] == '.' && mk[d + 1] == 0) marker = 1;
                if (d > 0 && (word_utf8_eq(mk + d, "、") || word_utf8_eq(mk + d, "．")) && mk[d + 3] == 0) marker = 1;
            }
            if (marker && info && info->para_seen_text) fz_write_string(ctx, info->out, "<br/>\n");
            show_text(ctx, n, info);
            if (info && mk && mk[0]) info->para_seen_text = 1;
        } else if (fz_xml_is_tag(n, "br") || fz_xml_is_tag(n, "lineBreak")) {
            /* Soft wrap only. Page/column breaks are Word pagination — MuPDF reflows. */
            const char* bt = fz_xml_att_alt(n, "w:type", "type");
            if (bt && (!strcmp(bt, "page") || !strcmp(bt, "column")))
                emit_page_break(ctx, info);
            else
                fz_write_string(ctx, info->out, "<br/>\n");
        } else if (fz_xml_is_tag(n, "tab"))
            fz_write_string(ctx, info->out, "&nbsp;&nbsp;&nbsp;&nbsp;");
        else if (fz_xml_is_tag(n, "drawing"))
            emit_drawing(ctx, n, info);
        else if (fz_xml_is_tag(n, "pict") || fz_xml_is_tag(n, "object"))
            emit_pict(ctx, n, info);
        else if (fz_xml_is_tag(n, "footnoteReference"))
            show_footnote(ctx, n, info);
        else if (fz_xml_is_tag(n, "lastRenderedPageBreak"))
            emit_page_break(ctx, info);
    }

    if (strike) fz_write_string(ctx, info->out, "</s>");
    if (underline) fz_write_string(ctx, info->out, "</u>");
    if (italic) fz_write_string(ctx, info->out, "</i>");
    if (bold) fz_write_string(ctx, info->out, "</b>");
    if (inline_style) fz_write_printf(ctx, info->out, "</%s>", inline_style);
    if (opened_size) fz_write_string(ctx, info->out, "</span>");
    if (opened_color) fz_write_string(ctx, info->out, "</span>");
    if (opened_font) fz_write_string(ctx, info->out, "</span>");
}

static void emit_paragraph_children(fz_context* ctx, fz_xml* parent, doc_info* info) {
    fz_xml* n;

    for (n = fz_xml_down(parent); n; n = fz_xml_next(n)) {
        if (fz_xml_is_tag(n, "pPr") || fz_xml_is_tag(n, "bookmarkStart") || fz_xml_is_tag(n, "bookmarkEnd") ||
            fz_xml_is_tag(n, "proofErr"))
            continue;
        if (fz_xml_is_tag(n, "r"))
            emit_run(ctx, n, info);
        else if (fz_xml_is_tag(n, "hyperlink") || fz_xml_is_tag(n, "ins") || fz_xml_is_tag(n, "del") ||
                 fz_xml_is_tag(n, "smartTag") || fz_xml_is_tag(n, "sdt") || fz_xml_is_tag(n, "sdtContent"))
            emit_paragraph_children(ctx, n, info);
        else if (fz_xml_is_tag(n, "drawing"))
            emit_drawing(ctx, n, info);
        else if (fz_xml_is_tag(n, "pict") || fz_xml_is_tag(n, "object"))
            emit_pict(ctx, n, info);
        else if (fz_xml_is_tag(n, "footnoteReference"))
            show_footnote(ctx, n, info);
    }
}

/* 落款日期: short line with 年, 月 and 日, not a sentence or a parenthetical. */
static int word_plain_looks_like_date_line(const char* plain) {
    const char* s = word_skip_leading_space(plain);
    int glyphs;
    if (!s || !s[0]) return 0;
    if (!strstr(s, "年") || !strstr(s, "月") || !strstr(s, "日")) return 0;
    if (strstr(s, "。") || strstr(s, "；") || strstr(s, "，") || strstr(s, "（") || strchr(s, '(')) return 0;
    glyphs = word_plain_glyph_count(s);
    return glyphs >= 4 && glyphs <= 20;
}

/* Blank lines before 单位落款 + 日期 push the date onto the next page.
 * A following 附件 list is not a date, so those blanks stay (they paginate 附件). */
static int word_date_follows_soon(fz_xml* p) {
    fz_xml* n;
    int seen = 0;

    for (n = fz_xml_next(p); n && seen < 4; n = fz_xml_next(n)) {
        char plain[256];
        const char* s;
        if (fz_xml_is_tag(n, "tbl") || fz_xml_is_tag(n, "sectPr")) return 0;
        if (!fz_xml_is_tag(n, "p")) continue;
        if (word_paragraph_is_visually_empty(n)) continue;
        word_paragraph_plain_text(n, plain, (int)sizeof plain);
        if (word_plain_looks_like_date_line(plain)) return 1;
        s = word_skip_leading_space(plain);
        if (word_plain_glyph_count(s) > 36) return 0;
        seen++;
    }
    return 0;
}

static void emit_paragraph(fz_context* ctx, fz_xml* p, doc_info* info) {
    fz_xml* ppr;
    fz_xml* pstyle;
    fz_xml* jc;
    fz_xml* numpr;
    const char* style_val = NULL;
    const char* align = NULL;
    const char* tag = "p";
    const char* cls = NULL;
    const char* bm_name = NULL;
    int list = 0; /* 1=ul, 2=ol — only when numbering.xml cannot synthesize a marker */
    int heading_level = 0;
    int outline_inferred = 0;
    int signoff = 0;
    float indent_em = 0;
    float indent_pt = 0;
    float left_pt = 0;
    float right_pt = 0;
    float hanging_pt = 0;
    float space_before_pt = 0;
    float space_after_pt = 0;
    float line_mult = 0;
    float line_pt = 0;
    int num_id = 0;
    int ilvl = 0;
    char marker[96];
    int have_marker = 0;
    char tag_buf[4];
    char style_css[320];
    int style_n = 0;

    /* A paragraph that only holds sectPr is the section break: omitted w:type
     * means next page, not a blank line. Other empty paragraphs are real
     * vertical space (落款后的空行). Dropping them pulls 附件1 onto the
     * signature page. Table cells already size from trHeight. */
    if (word_paragraph_is_visually_empty(p)) {
        fz_xml* sect = word_paragraph_sectpr(p);
        float before_pt = 0, after_pt = 0, line_mult = 0, line_pt = 0;
        if (sect) {
            if (info && word_sectpr_starts_new_page(sect)) {
                word_note_following_page_box(p, info);
                info->pending_page_break = 1;
            }
            return;
        }
        /* Keep 落款 and 日期 together. Blanks after the date (before 附件) still count. */
        if (word_date_follows_soon(p)) return;
        if (!info || info->in_table) return;
        word_ppr_spacing(fz_xml_find_down(p, "pPr"), &before_pt, &after_pt, &line_mult, &line_pt);
        if (line_pt < 0.5f) {
            float pt = info->doc_body_font_pt > 0.5f ? info->doc_body_font_pt : 16.0f;
            if (line_mult > 0.05f)
                line_pt = pt * line_mult;
            else
                line_pt = pt * 1.5f;
        }
        close_word_list(ctx, info);
        word_consume_section_page(ctx, info);
        fz_write_string(ctx, info->out, "<p style=\"");
        if (info->pending_page_break) {
            fz_write_string(ctx, info->out, "page-break-before:always;");
            info->pending_page_break = 0;
        }
        fz_write_printf(ctx, info->out, "margin:0;line-height:%.1fpt\">&#160;</p>\n", line_pt);
        return;
    }

    marker[0] = 0;
    ppr = fz_xml_find_down(p, "pPr");
    if (ppr) {
        pstyle = fz_xml_find_down(ppr, "pStyle");
        if (pstyle) style_val = fz_xml_att_alt(pstyle, "w:val", "val");
        jc = fz_xml_find_down(ppr, "jc");
        if (jc) align = jc_to_css_align(fz_xml_att_alt(jc, "w:val", "val"));
        word_ppr_text_indent(ppr, &indent_em, &indent_pt, &left_pt, &hanging_pt);
        {
            fz_xml* ind = fz_xml_find_down(ppr, "ind");
            const char* right = ind ? fz_xml_att_alt(ind, "w:right", "right") : NULL;
            int rw;
            if (!right && ind) right = fz_xml_att_alt(ind, "w:end", "end");
            rw = right ? fz_atoi(right) : 0;
            /* Huge right indent forces "1. 2. 3. 4." onto one marker per line. */
            if (rw > 0) right_pt = (float)rw / 20.0f;
        }
        word_ppr_spacing(ppr, &space_before_pt, &space_after_pt, &line_mult, &line_pt);
        numpr = fz_xml_find_down(ppr, "numPr");
        if (numpr) {
            fz_xml* idn = fz_xml_find_down(numpr, "numId");
            fz_xml* iln = fz_xml_find_down(numpr, "ilvl");
            const char* iv = idn ? fz_xml_att_alt(idn, "w:val", "val") : NULL;
            const char* lv = iln ? fz_xml_att_alt(iln, "w:val", "val") : NULL;
            if (iv) num_id = fz_atoi(iv);
            if (lv) ilvl = fz_atoi(lv);
            if (ilvl < 0) ilvl = 0;
            if (ilvl >= WORD_NUM_ILVL_MAX) ilvl = WORD_NUM_ILVL_MAX - 1;
        }
    }

    /* Inherit firstLine/left from paragraph style (e.g. Normal firstLineChars=200). */
    if (indent_em < 0.01f && indent_pt < 0.01f && left_pt < 0.01f && hanging_pt < 0.01f) {
        const char* sid = style_val ? style_val : (info ? info->default_pstyle : NULL);
        /* Centered/right titles should not pick up Normal firstLine. */
        if (!(align && (!strcmp(align, "center") || !strcmp(align, "right")))) {
            float sem = 0, spt = 0, sleft = 0, shang = 0;
            word_style_resolve_indent(info, sid, &sem, &spt, &sleft, &shang);
            indent_em = sem;
            indent_pt = spt;
            left_pt = sleft;
            hanging_pt = shang;
        }
    }

    /* Effective run size when w:r omits w:sz: pPr rPr → pStyle → Normal. */
    if (info) {
        float para_pt = 0;
        const char* pea = NULL;
        const char* plat = NULL;
        info->run_default_ea[0] = 0;
        info->run_default_latin[0] = 0;
        if (ppr) {
            para_pt = word_rpr_font_pt(fz_xml_find_down(ppr, "rPr"));
            word_rpr_font_names(info, fz_xml_find_down(ppr, "rPr"), &pea, &plat);
            if (pea) word_copy_font_name(info->run_default_ea, (int)sizeof info->run_default_ea, pea);
            if (plat) word_copy_font_name(info->run_default_latin, (int)sizeof info->run_default_latin, plat);
        }
        if (para_pt < 0.5f || !info->run_default_ea[0] || !info->run_default_latin[0]) {
            const char* sid = style_val ? style_val : info->default_pstyle;
            char sea[128] = {0};
            char slat[128] = {0};
            if (para_pt < 0.5f) para_pt = word_style_resolve_font_pt(info, sid);
            word_style_resolve_fonts(info, sid, sea, (int)sizeof sea, slat, (int)sizeof slat);
            if (!info->run_default_ea[0] && sea[0])
                word_copy_font_name(info->run_default_ea, (int)sizeof info->run_default_ea, sea);
            if (!info->run_default_latin[0] && slat[0])
                word_copy_font_name(info->run_default_latin, (int)sizeof info->run_default_latin, slat);
        }
        info->run_default_font_pt = para_pt;
    }

    bm_name = paragraph_bookmark_name(p);

    if (style_val) heading_level = word_style_heading_level(info, style_val);

    /* No Heading/标题 style: infer 公文 outline from paragraph text. */
    {
        char plain[512];
        word_paragraph_plain_text(p, plain, (int)sizeof plain);
        if (heading_level == 0 && info && info->in_table == 0) {
            heading_level = word_infer_text_heading_level(plain);
            if (heading_level > 0) outline_inferred = 1;
            if (heading_level == 0 && word_looks_like_official_doc_title(plain, align)) heading_level = 1;
            /* Word pads 落款 with spaces; HTML width ≠ Word, so right-align instead. */
            if (heading_level == 0 && !align && word_looks_like_signoff(plain)) signoff = 1;
            /* Huge firstLine / left indent without jc → fake 红头/文号 center. */
            if (!align && !signoff && word_looks_like_fake_center_indent(plain, indent_em, indent_pt, left_pt)) {
                align = "center";
                indent_em = 0;
                indent_pt = 0;
                left_pt = 0;
            }
            /* jc=right + condensed oversized agency line → true center + fit one line. */
            {
                float max_run_pt = 0;
                int min_scale = 100;
                word_paragraph_run_metrics(p, &max_run_pt, &min_scale);
                if (info && word_looks_like_hongtou_banner(plain, align, max_run_pt, min_scale)) {
                    int glyphs = word_plain_glyph_count(word_skip_leading_space(plain));
                    float fit_pt = max_run_pt;
                    float scale = (min_scale > 0 && min_scale < 100) ? ((float)min_scale / 100.0f) : 1.0f;
                    align = "center";
                    indent_em = 0;
                    indent_pt = 0;
                    left_pt = 0;
                    hanging_pt = 0;
                    /* Approximate Word w:w (no true horizontal glyph scale in HTML). */
                    if (scale < 0.999f) fit_pt = max_run_pt * scale;
                    if (info->page_content_pt > 40.0f && glyphs > 0) {
                        float cap = (info->page_content_pt * 0.98f) / (float)glyphs;
                        if (cap > 12.0f && cap < fit_pt) fit_pt = cap;
                    }
                    if (fit_pt > 12.0f && fit_pt + 0.5f < max_run_pt) info->force_run_font_pt = fit_pt;
                } else if (info && align && !strcmp(align, "center") && info->page_content_pt > 40.0f &&
                           !word_xml_has_desc_tag(p, "br")) {
                    /* Fake-centered form title (整改清单): bold SimSun is wider than 1em,
                     * so the last few glyphs wrap. Fit one line, matching Word. */
                    int glyphs = word_plain_glyph_count(word_skip_leading_space(plain));
                    int any_bold = word_xml_has_desc_tag(p, "b") || word_xml_has_desc_tag(p, "bCs");
                    float adv = any_bold ? 1.12f : 1.0f;
                    if (max_run_pt >= 16.0f && glyphs >= 8 && glyphs <= 48 && !strstr(plain, "。") &&
                        !strstr(plain, "；")) {
                        float need = max_run_pt * (float)glyphs * adv;
                        if (need > info->page_content_pt * 0.92f) {
                            float cap = (info->page_content_pt * 0.90f) / ((float)glyphs * adv);
                            if (cap > 12.0f && cap + 0.5f < max_run_pt) info->force_run_font_pt = cap;
                        }
                    }
                }
            }
        }
        /* Empty Heading-styled lines must not become blank sidebar bookmarks. */
        if (heading_level > 0 && word_plain_is_blank(plain)) {
            heading_level = 0;
            outline_inferred = 0;
        }
    }

    /* Center/right + firstLine shifts the block off true center (Word leftover). */
    if (align && (!strcmp(align, "center") || !strcmp(align, "right"))) {
        indent_em = 0;
        indent_pt = 0;
        hanging_pt = 0;
        /* Fake-centered 文号 used left indent with no jc — drop it once centered. */
        if (!strcmp(align, "center")) left_pt = 0;
    }

    if (signoff) {
        align = "right";
        indent_em = 0;
        indent_pt = 0;
        left_pt = 0;
        hanging_pt = 0;
        if (info) {
            info->signoff_emit = 1;
            info->skip_leading_ws = 1;
        }
    }

    /* Table cells: firstLine/hanging fights the grid. Keep w:ind left —
     * form headers center short labels with left indent, not jc. */
    if (info && info->in_table) {
        indent_em = 0;
        indent_pt = 0;
        hanging_pt = 0;
        /* Header labels are optically centered with w:ind left, but a fixed
         * margin-left does not stay centered once the cell is a percentage of
         * the page. Center the first two rows (表头) unless jc is right
         * (diagonal corner label). */
        if (info->table_row >= 0 && info->table_row < 2 && !(align && !strcmp(align, "right"))) {
            align = "center";
            left_pt = 0;
            right_pt = 0;
        }
    }

    if (heading_level >= 1 && heading_level <= 6) {
        fz_snprintf(tag_buf, sizeof tag_buf, "h%d", heading_level);
        tag = tag_buf;
        /* Strip Word space-padding so TOC titles are not nbsp-indented blanks. */
        if (info) info->skip_leading_ws = 1;
        if (outline_inferred) cls = "Outline";
        /* Keep Word firstLine on 1./（一） body-like headings — 公文 still indents them. */
    } else if (style_val) {
        if (!strcmp(style_val, "Heading1") || !strcmp(style_val, "heading 1") || !strcmp(style_val, "标题1") ||
            !strcmp(style_val, "标题 1"))
            tag = "h1";
        else if (!strcmp(style_val, "Heading2") || !strcmp(style_val, "heading 2") || !strcmp(style_val, "标题2") ||
                 !strcmp(style_val, "标题 2"))
            tag = "h2";
        else if (!strcmp(style_val, "Heading3") || !strcmp(style_val, "heading 3") || !strcmp(style_val, "标题3") ||
                 !strcmp(style_val, "标题 3"))
            tag = "h3";
        else if (!strcmp(style_val, "Heading4") || !strcmp(style_val, "heading 4") || !strcmp(style_val, "标题4") ||
                 !strcmp(style_val, "标题 4"))
            tag = "h4";
        else if (!strcmp(style_val, "Heading5") || !strcmp(style_val, "heading 5") || !strcmp(style_val, "标题5") ||
                 !strcmp(style_val, "标题 5"))
            tag = "h5";
        else if (!strcmp(style_val, "Heading6") || !strcmp(style_val, "heading 6") || !strcmp(style_val, "标题6") ||
                 !strcmp(style_val, "标题 6"))
            tag = "h6";
        else if (!strcmp(style_val, "SourceCode"))
            tag = "pre";
        else if (!strcmp(style_val, "ListBullet") || !strcmp(style_val, "ListParagraph"))
            list = 1;
        else if (!strcmp(style_val, "ListNumber"))
            list = 2;
        else if (!strcmp(style_val, "Title") || !strcmp(style_val, "标题")) {
            char plain[512];
            word_paragraph_plain_text(p, plain, (int)sizeof plain);
            if (!word_plain_is_blank(plain)) {
                tag = "h1"; /* class=Title is not in HTML outline; need a real heading */
                if (info) info->skip_leading_ws = 1;
            }
        } else if (!strcmp(style_val, "Subtitle") || !strcmp(style_val, "副标题")) {
            char plain[512];
            word_paragraph_plain_text(p, plain, (int)sizeof plain);
            if (!word_plain_is_blank(plain)) {
                tag = "h2";
                if (info) info->skip_leading_ws = 1;
            }
        } else if (!strcmp(style_val, "BodyText") || !strcmp(style_val, "Normal") || !strcmp(style_val, "正文"))
            cls = NULL;
        else
            cls = style_val;
    }

    /* Named Word bookmark (Insert → Bookmark) without a heading style: TOC entry. */
    if (bm_name && is_user_word_bookmark_name(bm_name) && !strcmp(tag, "p") && list == 0) {
        fz_write_string(ctx, info->out, "<h6 id=\"");
        doc_escape(ctx, info->out, bm_name);
        fz_write_string(ctx, info->out, "\" class=\"WordBookmark\">");
        doc_escape(ctx, info->out, bm_name);
        fz_write_string(ctx, info->out, "</h6>\n");
        bm_name = NULL; /* avoid duplicate id on the following <p> */
    }

    /* Prefer numbering.xml markers over blind <ul>/<ol> for numPr paragraphs. */
    if (num_id > 0 && info) {
        float num_left = 0, num_hang = 0, num_first = 0;
        have_marker =
            word_num_take(ctx, info, num_id, ilvl, marker, (int)sizeof marker, &num_left, &num_hang, &num_first);
        if (have_marker) {
            list = 0;
            /* Numbering lvl indent wins over paragraph firstLine (avoids double indent). */
            if (!(info->in_table)) {
                if (num_left > 0.01f) left_pt = num_left;
                if (num_hang > 0.01f) {
                    hanging_pt = num_hang;
                    indent_em = 0;
                    indent_pt = 0;
                } else if (num_first > 0.01f) {
                    /* Some list styles use firstLine on the lvl (not hanging). */
                    indent_pt = num_first;
                    indent_em = 0;
                    hanging_pt = 0;
                } else if (hanging_pt < 0.01f && left_pt < 0.01f && (indent_pt > 0.01f || indent_em > 0.01f)) {
                    /* Fallback: turn paragraph firstLine into a hanging list indent. */
                    float fl = indent_pt > 0.01f ? indent_pt : indent_em * 12.0f;
                    hanging_pt = fl > 24.0f ? 24.0f : fl;
                    left_pt = fl;
                    indent_em = 0;
                    indent_pt = 0;
                }
            }
            /* Auto-numbered lines stay body-sized even if text looks like 1.xxx. */
            if (outline_inferred) {
                tag = "p";
                cls = NULL;
                outline_inferred = 0;
                if (info) info->skip_leading_ws = 0;
            }
        } else if (list == 0 && !strcmp(tag, "p")) {
            /* numbering.xml missing/unresolved: keep old list fallback. */
            list = 1;
        }
    }

    if (list && !have_marker) {
        if (info->list_kind != list) {
            close_word_list(ctx, info);
            if (list == 1)
                fz_write_string(ctx, info->out, "<ul>\n");
            else
                fz_write_string(ctx, info->out, "<ol>\n");
            info->list_kind = list;
        }
        fz_write_string(ctx, info->out, "<li>");
        if (bm_name && !is_user_word_bookmark_name(bm_name)) {
            fz_write_string(ctx, info->out, "<a id=\"");
            doc_escape(ctx, info->out, bm_name);
            fz_write_string(ctx, info->out, "\"></a>");
        }
        if (info) {
            info->in_paragraph++;
            info->para_seen_text = 0;
        }
        emit_paragraph_children(ctx, p, info);
        if (info && info->in_paragraph > 0) info->in_paragraph--;
        if (info) {
            info->run_default_font_pt = 0;
            info->force_run_font_pt = 0;
            info->run_default_ea[0] = 0;
            info->run_default_latin[0] = 0;
        }
        fz_write_string(ctx, info->out, "</li>\n");
        return;
    }

    close_word_list(ctx, info);

    /* 一、/二、 are indented with four spaces; 三、 uses w:ind firstLine.
     * Headings strip those spaces (TOC). Put the width back.
     * 附件 "2." is left+hanging, and the spaces fill that hanging gap so
     * "2." lines up with "1.". Stripping the spaces must not close the gap. */
    if (info && info->skip_leading_ws && !signoff &&
        !(align && (!strcmp(align, "center") || !strcmp(align, "right")))) {
        char lead_plain[512];
        float font_pt = info->run_default_font_pt > 0.5f ? info->run_default_font_pt : 16.0f;
        float lead_pt;
        {
            fz_xml* rn;
            for (rn = fz_xml_down(p); rn; rn = fz_xml_next(rn)) {
                float rpt;
                if (!fz_xml_is_tag(rn, "r")) continue;
                rpt = word_rpr_font_pt(fz_xml_find_down(rn, "rPr"));
                if (rpt > 0.5f) {
                    font_pt = rpt;
                    break;
                }
            }
        }
        word_paragraph_plain_text(p, lead_plain, (int)sizeof lead_plain);
        lead_pt = word_leading_space_pt(lead_plain, font_pt);
        if (lead_pt > 0.5f && hanging_pt > 0.01f) {
            float extra = lead_pt - hanging_pt;
            if (extra >= -0.5f) {
                hanging_pt = 0;
                if (extra > 0.5f) indent_pt += extra;
            } else
                hanging_pt = -extra;
        } else if (lead_pt > 0.5f && left_pt < 0.01f && indent_pt < 0.01f && indent_em < 0.01f)
            indent_pt = lead_pt;
    }

    style_css[0] = 0;
    if (cls && !strcmp(cls, "Outline")) /* Use 1em (not font-size:inherit — MuPDF ignores inherit for font-size). */
        style_n += fz_snprintf(style_css + style_n, (int)sizeof style_css - style_n,
                               "%sfont-size:1em;font-weight:normal", style_n ? ";" : "");
    if (align)
        style_n += fz_snprintf(style_css + style_n, (int)sizeof style_css - style_n, "%stext-align:%s",
                               style_n ? ";" : "", align);
    if (signoff && info && info->signoff_pad_em > 0.01f)
        style_n += fz_snprintf(style_css + style_n, (int)sizeof style_css - style_n, "%spadding-right:%.2fem",
                               style_n ? ";" : "", info->signoff_pad_em);
    if (left_pt > 0.01f)
        style_n += fz_snprintf(style_css + style_n, (int)sizeof style_css - style_n, "%smargin-left:%.1fpt",
                               style_n ? ";" : "", left_pt);
    if (right_pt > 0.01f)
        style_n += fz_snprintf(style_css + style_n, (int)sizeof style_css - style_n, "%smargin-right:%.1fpt",
                               style_n ? ";" : "", right_pt);
    if (hanging_pt > 0.01f)
        style_n += fz_snprintf(style_css + style_n, (int)sizeof style_css - style_n, "%stext-indent:-%.1fpt",
                               style_n ? ";" : "", hanging_pt);
    else if (indent_em > 0.01f)
        style_n += fz_snprintf(style_css + style_n, (int)sizeof style_css - style_n, "%stext-indent:%.2fem",
                               style_n ? ";" : "", indent_em);
    else if (indent_pt > 0.01f)
        style_n += fz_snprintf(style_css + style_n, (int)sizeof style_css - style_n, "%stext-indent:%.1fpt",
                               style_n ? ";" : "", indent_pt);
    if (space_before_pt > 0.01f)
        style_n += fz_snprintf(style_css + style_n, (int)sizeof style_css - style_n, "%smargin-top:%.1fpt",
                               style_n ? ";" : "", space_before_pt);
    if (space_after_pt > 0.01f)
        style_n += fz_snprintf(style_css + style_n, (int)sizeof style_css - style_n, "%smargin-bottom:%.1fpt",
                               style_n ? ";" : "", space_after_pt);
    if (line_mult > 0.05f && (line_mult < 0.95f || line_mult > 1.05f)) {
        /* MuPDF treats line-height < 1 as overlapping glyphs; Word's slight
         * tighten (e.g. 217/240) must not crush table form labels. */
        if (line_mult < 1.0f) line_mult = 1.0f;
        style_n += fz_snprintf(style_css + style_n, (int)sizeof style_css - style_n, "%sline-height:%.2f",
                               style_n ? ";" : "", line_mult);
    } else if (line_pt > 0.01f)
        style_n += fz_snprintf(style_css + style_n, (int)sizeof style_css - style_n, "%sline-height:%.1fpt",
                               style_n ? ";" : "", line_pt);

    if (info && info->pending_page_break && !word_consume_section_page(ctx, info)) {
        style_n += fz_snprintf(style_css + style_n, (int)sizeof style_css - style_n, "%spage-break-before:always",
                               style_n ? ";" : "");
        info->pending_page_break = 0;
    }

    fz_write_printf(ctx, info->out, "<%s", tag);
    if (bm_name) {
        fz_write_string(ctx, info->out, " id=\"");
        doc_escape(ctx, info->out, bm_name);
        fz_write_string(ctx, info->out, "\"");
    }
    if (cls) fz_write_printf(ctx, info->out, " class=\"%s\"", cls);
    if (style_css[0]) fz_write_printf(ctx, info->out, " style=\"%s\"", style_css);
    fz_write_string(ctx, info->out, ">");
    if (have_marker) {
        fz_write_string(ctx, info->out, "<span class=\"w-num\">");
        doc_escape(ctx, info->out, marker);
        fz_write_string(ctx, info->out, "</span>");
    }
    if (info) {
        info->in_paragraph++;
        info->para_seen_text = 0;
    }
    emit_paragraph_children(ctx, p, info);
    if (info && info->in_paragraph > 0) info->in_paragraph--;
    if (info) {
        info->run_default_font_pt = 0;
        info->force_run_font_pt = 0;
        info->run_default_ea[0] = 0;
        info->run_default_latin[0] = 0;
    }
    fz_write_printf(ctx, info->out, "</%s>\n", tag);
    if (info && (signoff || (tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6'))) {
        info->signoff_emit = 0;
        info->skip_leading_ws = 0;
    }
}

/* --- Word table borders (OOXML → CSS) ---
 * w:sz is in eighths of a point. Cell edges resolve as:
 *   tcBorders edge → tblBorders outer/inside → tblStyle tblBorders → none.
 * Do not invent borders when OOXML says nil/none. */

typedef struct {
    int set; /* 0 = unset, 1 = draw, -1 = explicitly none/nil */
    float width_pt;
    unsigned char r, g, b;
} word_border_edge;

typedef struct {
    word_border_edge top, left, bottom, right, inside_h, inside_v;
} word_tbl_border_set;

static void word_border_edge_clear(word_border_edge* e) {
    e->set = 0;
    e->width_pt = 0;
    e->r = e->g = e->b = 0;
}

static void word_tbl_border_set_clear(word_tbl_border_set* s) {
    word_border_edge_clear(&s->top);
    word_border_edge_clear(&s->left);
    word_border_edge_clear(&s->bottom);
    word_border_edge_clear(&s->right);
    word_border_edge_clear(&s->inside_h);
    word_border_edge_clear(&s->inside_v);
}

static int word_border_val_is_none(const char* val) {
    if (!val || !val[0]) return 0;
    return !ascii_strcasecmp(val, "nil") || !ascii_strcasecmp(val, "none");
}

static int word_parse_hex6(const char* s, unsigned int* rgb) {
    unsigned int v = 0;
    int i;
    if (!s || !rgb) return 0;
    for (i = 0; i < 6; i++) {
        char c = s[i];
        unsigned int d;
        if (c >= '0' && c <= '9')
            d = (unsigned int)(c - '0');
        else if (c >= 'a' && c <= 'f')
            d = (unsigned int)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            d = (unsigned int)(c - 'A' + 10);
        else
            return 0;
        v = (v << 4) | d;
    }
    *rgb = v;
    return 1;
}

static void word_parse_border_element(fz_xml* el, word_border_edge* out) {
    const char* val;
    const char* sz;
    const char* color;
    int eighths;

    if (!el || !out) return;
    val = fz_xml_att_alt(el, "w:val", "val");
    if (word_border_val_is_none(val)) {
        out->set = -1;
        out->width_pt = 0;
        return;
    }
    sz = fz_xml_att_alt(el, "w:sz", "sz");
    eighths = sz ? fz_atoi(sz) : 0;
    /* Missing sz with a line style → hairline (Word often omits sz on "single"). */
    if (eighths <= 0) eighths = 4;
    out->width_pt = (float)eighths / 8.f;
    out->set = 1;
    out->r = out->g = out->b = 0;
    color = fz_xml_att_alt(el, "w:color", "color");
    if (color && color[0] && ascii_strcasecmp(color, "auto") != 0) {
        unsigned int rgb = 0;
        if (color[0] == '#') color++;
        if (strlen(color) >= 6 && word_parse_hex6(color, &rgb)) {
            out->r = (unsigned char)((rgb >> 16) & 255);
            out->g = (unsigned char)((rgb >> 8) & 255);
            out->b = (unsigned char)(rgb & 255);
        }
    }
}

static void word_parse_tbl_borders_node(fz_xml* borders, word_tbl_border_set* out) {
    fz_xml* n;
    if (!borders || !out) return;
    for (n = fz_xml_down(borders); n; n = fz_xml_next(n)) {
        if (fz_xml_is_tag(n, "top"))
            word_parse_border_element(n, &out->top);
        else if (fz_xml_is_tag(n, "left") || fz_xml_is_tag(n, "start"))
            word_parse_border_element(n, &out->left);
        else if (fz_xml_is_tag(n, "bottom"))
            word_parse_border_element(n, &out->bottom);
        else if (fz_xml_is_tag(n, "right") || fz_xml_is_tag(n, "end"))
            word_parse_border_element(n, &out->right);
        else if (fz_xml_is_tag(n, "insideH"))
            word_parse_border_element(n, &out->inside_h);
        else if (fz_xml_is_tag(n, "insideV"))
            word_parse_border_element(n, &out->inside_v);
    }
}

static void word_border_edge_overlay(word_border_edge* dst, const word_border_edge* src) {
    if (!dst || !src || src->set == 0) return;
    *dst = *src;
}

static void word_tbl_border_set_overlay(word_tbl_border_set* dst, const word_tbl_border_set* src) {
    if (!dst || !src) return;
    word_border_edge_overlay(&dst->top, &src->top);
    word_border_edge_overlay(&dst->left, &src->left);
    word_border_edge_overlay(&dst->bottom, &src->bottom);
    word_border_edge_overlay(&dst->right, &src->right);
    word_border_edge_overlay(&dst->inside_h, &src->inside_h);
    word_border_edge_overlay(&dst->inside_v, &src->inside_v);
}

static fz_xml* word_find_style_by_id(doc_info* info, const char* style_id) {
    fz_xml* styles_root;
    fz_xml* child;
    if (!info || !info->styles_xml || !style_id || !style_id[0]) return NULL;
    styles_root = fz_xml_find_dfs(info->styles_xml, "styles", NULL, NULL);
    child = fz_xml_down(styles_root ? styles_root : info->styles_xml);
    for (; child; child = fz_xml_next(child)) {
        const char* sid;
        if (!fz_xml_is_tag(child, "style")) continue;
        sid = fz_xml_att_alt(child, "w:styleId", "styleId");
        if (sid && !strcmp(sid, style_id)) return child;
    }
    return NULL;
}

static void word_load_tbl_borders_from_style_depth(doc_info* info, const char* style_id, word_tbl_border_set* out,
                                                   int depth) {
    fz_xml* style;
    fz_xml* based;
    fz_xml* tblpr;
    fz_xml* borders;
    const char* based_on;
    if (!out) return;
    word_tbl_border_set_clear(out);
    if (!style_id || !style_id[0] || depth > 16) return;
    style = word_find_style_by_id(info, style_id);
    if (!style) return;
    /* BasedOn first, then this style overlays (Word inheritance). */
    based = fz_xml_find_down(style, "basedOn");
    based_on = based ? fz_xml_att_alt(based, "w:val", "val") : NULL;
    if (based_on && strcmp(based_on, style_id) != 0) {
        word_tbl_border_set parent;
        word_load_tbl_borders_from_style_depth(info, based_on, &parent, depth + 1);
        word_tbl_border_set_overlay(out, &parent);
    }
    tblpr = fz_xml_find_down(style, "tblPr");
    borders = tblpr ? fz_xml_find_down(tblpr, "tblBorders") : NULL;
    if (borders) {
        word_tbl_border_set local;
        word_tbl_border_set_clear(&local);
        word_parse_tbl_borders_node(borders, &local);
        word_tbl_border_set_overlay(out, &local);
    }
}

static void word_load_tbl_borders_from_style(doc_info* info, const char* style_id, word_tbl_border_set* out) {
    word_load_tbl_borders_from_style_depth(info, style_id, out, 0);
}

static void word_resolve_table_borders(fz_xml* tbl, doc_info* info, word_tbl_border_set* out) {
    fz_xml* tblpr;
    fz_xml* style_n;
    fz_xml* borders;
    const char* style_id;
    word_tbl_border_set_clear(out);
    tblpr = fz_xml_find_down(tbl, "tblPr");
    style_n = tblpr ? fz_xml_find_down(tblpr, "tblStyle") : NULL;
    style_id = style_n ? fz_xml_att_alt(style_n, "w:val", "val") : NULL;
    if (style_id) word_load_tbl_borders_from_style(info, style_id, out);
    borders = tblpr ? fz_xml_find_down(tblpr, "tblBorders") : NULL;
    if (borders) {
        word_tbl_border_set local;
        word_tbl_border_set_clear(&local);
        word_parse_tbl_borders_node(borders, &local);
        word_tbl_border_set_overlay(out, &local);
    }
}

static void word_resolve_cell_borders(fz_xml* tc, const word_tbl_border_set* table_b, int row_i, int n_rows, int col_i,
                                      int n_cols, int colspan, int rowspan, word_border_edge* top,
                                      word_border_edge* right, word_border_edge* bottom, word_border_edge* left) {
    fz_xml* tcpr;
    fz_xml* borders;
    fz_xml* n;
    int last_col = col_i + (colspan > 0 ? colspan : 1) - 1;
    int last_row = row_i + (rowspan > 0 ? rowspan : 1) - 1;

    word_border_edge_clear(top);
    word_border_edge_clear(right);
    word_border_edge_clear(bottom);
    word_border_edge_clear(left);

    /* Table-level: outer edges vs insideH/V. */
    if (table_b) {
        *top = (row_i == 0) ? table_b->top : table_b->inside_h;
        *bottom = (last_row >= n_rows - 1) ? table_b->bottom : table_b->inside_h;
        *left = (col_i == 0) ? table_b->left : table_b->inside_v;
        *right = (last_col >= n_cols - 1) ? table_b->right : table_b->inside_v;
    }

    tcpr = fz_xml_find_down(tc, "tcPr");
    borders = tcpr ? fz_xml_find_down(tcpr, "tcBorders") : NULL;
    if (!borders) return;
    for (n = fz_xml_down(borders); n; n = fz_xml_next(n)) {
        if (fz_xml_is_tag(n, "top"))
            word_parse_border_element(n, top);
        else if (fz_xml_is_tag(n, "left") || fz_xml_is_tag(n, "start"))
            word_parse_border_element(n, left);
        else if (fz_xml_is_tag(n, "bottom"))
            word_parse_border_element(n, bottom);
        else if (fz_xml_is_tag(n, "right") || fz_xml_is_tag(n, "end"))
            word_parse_border_element(n, right);
    }
}

static void word_append_css_border_edge(char* buf, size_t buf_n, const char* side, const word_border_edge* e) {
    size_t len;
    if (!buf || !e || e->set != 1 || e->width_pt <= 0) return;
    len = strlen(buf);
    if (len + 64 >= buf_n) return;
    /* Keep fractional pt — MuPDF maps pt numerically; avoid premature px/int. */
    fz_snprintf(buf + len, (int)(buf_n - len), "border-%s:%.3fpt solid #%02x%02x%02x;", side, e->width_pt, e->r, e->g,
                e->b);
}

static int word_tbl_count_rows(fz_xml* tbl) {
    fz_xml* n;
    int rows = 0;
    for (n = fz_xml_down(tbl); n; n = fz_xml_next(n))
        if (fz_xml_is_tag(n, "tr")) rows++;
    return rows;
}

static int word_tc_grid_span(fz_xml* tc) {
    fz_xml* tcpr;
    fz_xml* gs;
    const char* v;
    int n;

    tcpr = fz_xml_find_down(tc, "tcPr");
    if (!tcpr) return 1;
    gs = fz_xml_find_down(tcpr, "gridSpan");
    if (!gs) return 1;
    v = fz_xml_att_alt(gs, "w:val", "val");
    n = v ? fz_atoi(v) : 1;
    return n > 1 ? n : 1;
}

/* 0 = none, 1 = restart (start of vertical merge), 2 = continue. */
static int word_tc_vmerge_kind(fz_xml* tc) {
    fz_xml* tcpr;
    fz_xml* vm;
    const char* v;

    tcpr = fz_xml_find_down(tc, "tcPr");
    if (!tcpr) return 0;
    vm = fz_xml_find_down(tcpr, "vMerge");
    if (!vm) return 0;
    v = fz_xml_att_alt(vm, "w:val", "val");
    if (v && !strcmp(v, "restart")) return 1;
    /* Bare <w:vMerge/> or val="continue" */
    return 2;
}

/* Cell covering grid column want_col in this row (accounts for gridSpan). */
static fz_xml* word_row_cell_at_col(fz_xml* tr, int want_col) {
    fz_xml* n;
    int col = 0;

    if (!tr || want_col < 0) return NULL;
    for (n = fz_xml_down(tr); n; n = fz_xml_next(n)) {
        int span;
        if (!fz_xml_is_tag(n, "tc")) continue;
        span = word_tc_grid_span(n);
        if (want_col >= col && want_col < col + span) return n;
        col += span;
    }
    return NULL;
}

static int word_vmerge_rowspan(fz_xml* start_tr, int col) {
    fz_xml* tr;
    int span = 1;

    for (tr = fz_xml_next(start_tr); tr; tr = fz_xml_next(tr)) {
        fz_xml* tc;
        if (!fz_xml_is_tag(tr, "tr")) continue;
        tc = word_row_cell_at_col(tr, col);
        if (!tc || word_tc_vmerge_kind(tc) != 2) break;
        span++;
    }
    return span;
}

static void emit_table_cell(fz_context* ctx, fz_xml* tc, doc_info* info, int rowspan, int colspan, int col, int row_i,
                            int n_rows, int n_cols, const word_tbl_border_set* table_b, const int* grid_w, int ngrid,
                            int grid_sum, float row_min_height_pt) {
    fz_xml* n;
    fz_xml* tcpr;
    float pct = 0;
    int i;
    int style_n = 0;
    int vertical = 0;
    int saved_vertical = 0;
    int saved_row = -1;
    char style_css[448];
    word_border_edge top, right, bottom, left;

    if (rowspan < 1) rowspan = 1;
    if (colspan < 1) colspan = 1;
    if (grid_w && grid_sum > 0 && col >= 0) {
        for (i = 0; i < colspan && col + i < ngrid; i++) pct += 100.0f * (float)grid_w[col + i] / (float)grid_sum;
    }

    tcpr = fz_xml_find_down(tc, "tcPr");
    if (tcpr) {
        fz_xml* td = fz_xml_find_down(tcpr, "textDirection");
        const char* dv = td ? fz_xml_att_alt(td, "w:val", "val") : NULL;
        /* tbRl / tbRlV / btLr: vertical forms (会议报告单 label column). */
        if (dv && (strstr(dv, "tbRl") || strstr(dv, "btLr") || !strcmp(dv, "tbLrV"))) vertical = 1;
    }

    style_css[0] = 0;
    /* Office cell margins are explicit; default td padding wraps header labels. */
    style_n += fz_snprintf(style_css + style_n, (int)sizeof style_css - style_n, "padding:0;");
    if (pct > 0.01f) style_n += fz_snprintf(style_css + style_n, (int)sizeof style_css - style_n, "width:%.2f%%;", pct);
    /* Word trHeight hRule=atLeast: MuPDF td height floors the cell (grows with content). */
    if (row_min_height_pt > 0.5f)
        style_n +=
            fz_snprintf(style_css + style_n, (int)sizeof style_css - style_n, "height:%.1fpt;", row_min_height_pt);
    if (vertical || row_i < 2)
        style_n += fz_snprintf(style_css + style_n, (int)sizeof style_css - style_n, "vertical-align:middle;");

    word_resolve_cell_borders(tc, table_b, row_i, n_rows, col, n_cols, colspan, rowspan, &top, &right, &bottom, &left);
    word_append_css_border_edge(style_css, sizeof style_css, "top", &top);
    word_append_css_border_edge(style_css, sizeof style_css, "right", &right);
    word_append_css_border_edge(style_css, sizeof style_css, "bottom", &bottom);
    word_append_css_border_edge(style_css, sizeof style_css, "left", &left);

    fz_write_string(ctx, info->out, "<td");
    if (rowspan > 1) fz_write_printf(ctx, info->out, " rowspan=\"%d\"", rowspan);
    if (colspan > 1) fz_write_printf(ctx, info->out, " colspan=\"%d\"", colspan);
    /* Width + resolved OOXML borders on each cell (no blanket td border). */
    if (style_css[0]) fz_write_printf(ctx, info->out, " style=\"%s\"", style_css);
    fz_write_string(ctx, info->out, ">");
    if (info) {
        saved_vertical = info->emit_vertical_breaks;
        saved_row = info->table_row;
        info->emit_vertical_breaks = vertical;
        info->table_row = row_i;
    }
    for (n = fz_xml_down(tc); n; n = fz_xml_next(n)) {
        if (fz_xml_is_tag(n, "tcPr")) continue;
        if (fz_xml_is_tag(n, "p"))
            emit_paragraph(ctx, n, info);
        else if (fz_xml_is_tag(n, "tbl"))
            emit_table(ctx, n, info);
    }
    if (info) {
        info->emit_vertical_breaks = saved_vertical;
        info->table_row = saved_row;
    }
    fz_write_string(ctx, info->out, "</td>");
}

/* Word w:trHeight (twips). hRule atLeast/exact → pt floor for the row. */
static float word_tr_min_height_pt(fz_xml* tr) {
    fz_xml* trpr;
    fz_xml* th;
    const char* val;
    const char* rule;
    int tw;

    trpr = fz_xml_find_down(tr, "trPr");
    if (!trpr) return 0;
    th = fz_xml_find_down(trpr, "trHeight");
    if (!th) return 0;
    val = fz_xml_att_alt(th, "w:val", "val");
    if (!val || !val[0]) return 0;
    tw = fz_atoi(val);
    if (tw <= 0) return 0;
    rule = fz_xml_att_alt(th, "w:hRule", "hRule");
    /* auto = content-sized; atLeast / exact / missing → honor val as floor. */
    if (rule && !strcmp(rule, "auto")) return 0;
    return (float)tw / 20.0f;
}

static void emit_table_row(fz_context* ctx, fz_xml* tr, doc_info* info, int row_i, int n_rows, int n_cols,
                           const word_tbl_border_set* table_b, const int* grid_w, int ngrid, int grid_sum) {
    fz_xml* n;
    int col = 0;
    float row_h = word_tr_min_height_pt(tr);

    fz_write_string(ctx, info->out, "<tr>");
    for (n = fz_xml_down(tr); n; n = fz_xml_next(n)) {
        int vm;
        int gs;
        int rs;
        if (fz_xml_is_tag(n, "trPr") || fz_xml_is_tag(n, "tblPrEx")) continue;
        if (!fz_xml_is_tag(n, "tc")) continue;
        gs = word_tc_grid_span(n);
        vm = word_tc_vmerge_kind(n);
        if (vm == 2) {
            /* Continuation of a vertical merge — already covered by rowspan. */
            col += gs;
            continue;
        }
        rs = (vm == 1) ? word_vmerge_rowspan(tr, col) : 1;
        emit_table_cell(ctx, n, info, rs, gs, col, row_i, n_rows, n_cols, table_b, grid_w, ngrid, grid_sum, row_h);
        col += gs;
    }
    fz_write_string(ctx, info->out, "</tr>\n");
}

static int word_tbl_grid_widths(fz_xml* tbl, int* widths, int max_n, int* sum_out) {
    fz_xml* grid;
    fz_xml* n;
    int nw = 0;
    int sum = 0;

    if (sum_out) *sum_out = 0;
    grid = fz_xml_find_down(tbl, "tblGrid");
    if (!grid || !widths || max_n < 1) return 0;
    for (n = fz_xml_down(grid); n && nw < max_n; n = fz_xml_next(n)) {
        const char* w;
        int tw;
        if (!fz_xml_is_tag(n, "gridCol")) continue;
        w = fz_xml_att_alt(n, "w:w", "w");
        tw = w ? fz_atoi(w) : 0;
        if (tw <= 0) tw = 1;
        widths[nw++] = tw;
        sum += tw;
    }
    if (sum_out) *sum_out = sum;
    return nw;
}

static void emit_table(fz_context* ctx, fz_xml* tbl, doc_info* info) {
    fz_xml* n;
    int grid_w[64];
    int ngrid;
    int grid_sum = 0;
    int n_rows;
    int n_cols;
    int row_i;
    word_tbl_border_set table_b;
    const char* dump_geo = getenv("SUMATRA_DUMP_TABLE_GEOMETRY");

    close_word_list(ctx, info);
    if (info) info->in_table++;
    ngrid = word_tbl_grid_widths(tbl, grid_w, (int)(sizeof grid_w / sizeof grid_w[0]), &grid_sum);
    n_rows = word_tbl_count_rows(tbl);
    n_cols = ngrid > 0 ? ngrid : 1;
    word_resolve_table_borders(tbl, info, &table_b);

    if (dump_geo && dump_geo[0]) {
        int gi;
        fz_write_printf(ctx, fz_stddbg(ctx), "TABLE_GEO n_rows=%d n_cols=%d ngrid=%d grid_sum=%d\n", n_rows, n_cols,
                        ngrid, grid_sum);
        for (gi = 0; gi < ngrid; gi++)
            fz_write_printf(ctx, fz_stddbg(ctx), "  gridCol[%d]=%d (%.2f%%)\n", gi, grid_w[gi],
                            grid_sum > 0 ? 100.0 * grid_w[gi] / (double)grid_sum : 0);
    }

    if (info && info->pending_page_break && !word_consume_section_page(ctx, info)) {
        fz_write_string(ctx, info->out, "<table style=\"page-break-before:always\">\n");
        info->pending_page_break = 0;
    } else
        fz_write_string(ctx, info->out, "<table>\n");
    row_i = 0;
    for (n = fz_xml_down(tbl); n; n = fz_xml_next(n)) {
        if (fz_xml_is_tag(n, "tblPr") || fz_xml_is_tag(n, "tblGrid")) continue;
        if (fz_xml_is_tag(n, "tr")) {
            if (dump_geo && dump_geo[0]) {
                fz_xml* tc;
                int col = 0;
                fz_write_printf(ctx, fz_stddbg(ctx), "ROW %d\n", row_i);
                for (tc = fz_xml_down(n); tc; tc = fz_xml_next(tc)) {
                    int vm, gs, rs, ii;
                    double wsum = 0;
                    if (!fz_xml_is_tag(tc, "tc")) continue;
                    gs = word_tc_grid_span(tc);
                    vm = word_tc_vmerge_kind(tc);
                    if (vm == 2) {
                        fz_write_printf(ctx, fz_stddbg(ctx), "  cell col=%d span=%d vMerge=continue (skip td)\n", col,
                                        gs);
                        col += gs;
                        continue;
                    }
                    rs = (vm == 1) ? word_vmerge_rowspan(n, col) : 1;
                    for (ii = 0; ii < gs && col + ii < ngrid; ii++) wsum += grid_w[col + ii];
                    fz_write_printf(ctx, fz_stddbg(ctx),
                                    "  cell col=%d span=%d rowspan=%d vMerge=%s html_width=%.2f%%\n", col, gs, rs,
                                    vm == 1 ? "restart" : "none", (grid_sum > 0) ? (100.0 * wsum / grid_sum) : 0.0);
                    col += gs;
                }
            }
            emit_table_row(ctx, n, info, row_i, n_rows, n_cols, &table_b, ngrid > 0 ? grid_w : NULL, ngrid, grid_sum);
            row_i++;
        }
    }
    fz_write_string(ctx, info->out, "</table>\n");
    if (info && info->in_table > 0) info->in_table--;
}

static void emit_word_body(fz_context* ctx, fz_xml* body, doc_info* info) {
    fz_xml* n;

    for (n = fz_xml_down(body); n; n = fz_xml_next(n)) {
        if (fz_xml_is_tag(n, "sectPr")) continue;
        if (fz_xml_is_tag(n, "p")) {
            /* Consecutive space-padded 落款 lines: keep the block flush-right, but
             * center shorter lines (日期) under the longest (发文机关). Word does
             * this with extra leading spaces; we strip those and use padding-right. */
            if (word_paragraph_is_signoff(n, info)) {
                fz_xml* paras[8];
                float widths[8];
                float max_w = 0;
                int count = 0;
                int i;
                fz_xml* m;

                for (m = n; m && count < (int)(sizeof paras / sizeof paras[0]); m = fz_xml_next(m)) {
                    char plain[512];
                    float w;
                    if (!fz_xml_is_tag(m, "p") || !word_paragraph_is_signoff(m, info)) break;
                    word_paragraph_plain_text(m, plain, (int)sizeof plain);
                    w = word_plain_visual_em(plain);
                    paras[count] = m;
                    widths[count] = w;
                    if (w > max_w) max_w = w;
                    count++;
                }
                if (count >= 2) {
                    for (i = 0; i < count; i++) {
                        info->signoff_pad_em = (max_w - widths[i]) * 0.5f;
                        emit_paragraph(ctx, paras[i], info);
                        info->signoff_pad_em = 0;
                    }
                    n = paras[count - 1];
                    continue;
                }
            }
            emit_paragraph(ctx, n, info);
        } else if (fz_xml_is_tag(n, "tbl"))
            emit_table(ctx, n, info);
        else if (fz_xml_is_tag(n, "sdt")) {
            fz_xml* content = fz_xml_find_down(n, "sdtContent");
            if (content) emit_word_body(ctx, content, info);
        }
    }
    close_word_list(ctx, info);
}

/* Fallback DFS walker for non-Word packages (pptx slides, hwpx, etc.). */
static void process_doc_stream_fallback(fz_context* ctx, fz_xml* xml, doc_info* info, int do_pages) {
    fz_xml* pos;
    fz_xml* next;
    const char* paragraph_style = NULL;
    const char* inline_style = NULL;

    /* First off, see if we can do page numbers. */
    if (do_pages) {
        pos = fz_xml_find_dfs(xml, "lastRenderedPageBreak", NULL, NULL);
        if (pos) {
            fz_write_string(ctx, info->out, "<div id=\"page1\">\n");
            info->page = 1;
        }
    }

    pos = xml;
    while (pos) {
        if (fz_xml_is_tag(pos, "t")) {
            show_text(ctx, pos, info);
        } else if (fz_xml_is_tag(pos, "br")) {
            if (paragraph_style && strcmp(paragraph_style, "pre"))
                fz_write_printf(ctx, info->out, "<br/>\n");
            else
                fz_write_printf(ctx, info->out, "\n");
        } else if (fz_xml_is_tag(pos, "footnoteReference")) {
            show_footnote(ctx, pos, info);
        } else if (fz_xml_is_tag(pos, "tabs")) {
            /* Skip tab definitions. */
        } else if (fz_xml_is_tag(pos, "pStyle")) {
            paragraph_style = fz_xml_att(pos, "w:val");
            if (paragraph_style) {
                if (!strcmp(paragraph_style, "BodyText"))
                    paragraph_style = NULL;
                else if (!strcmp(paragraph_style, "Heading1"))
                    paragraph_style = "h1";
                else if (!strcmp(paragraph_style, "Heading2"))
                    paragraph_style = "h2";
                else if (!strcmp(paragraph_style, "Heading3"))
                    paragraph_style = "h3";
                else if (!strcmp(paragraph_style, "Heading4"))
                    paragraph_style = "h4";
                else if (!strcmp(paragraph_style, "Heading5"))
                    paragraph_style = "h5";
                else if (!strcmp(paragraph_style, "Heading6"))
                    paragraph_style = "h6";
                else if (!strcmp(paragraph_style, "SourceCode"))
                    paragraph_style = "pre";
                else
                    paragraph_style = NULL;

                if (paragraph_style) fz_write_printf(ctx, info->out, "<%s>", paragraph_style);
            }
        } else if (fz_xml_is_tag(pos, "rStyle")) {
            inline_style = fz_xml_att(pos, "w:val");
            if (inline_style) {
                if (!strcmp(inline_style, "VerbatimChar"))
                    inline_style = "tt";
                else
                    inline_style = NULL;
                if (inline_style) fz_write_printf(ctx, info->out, "<%s>", inline_style);
            }
        } else {
            fz_xml* down;
            if (fz_xml_is_tag(pos, "lineBreak"))
                fz_write_string(ctx, info->out, "\n");
            else if (fz_xml_is_tag(pos, "p"))
                fz_write_string(ctx, info->out, "<p>");
            else if (fz_xml_is_tag(pos, "tab"))
                fz_write_string(ctx, info->out, "\t");
            else if (do_pages && fz_xml_is_tag(pos, "lastRenderedPageBreak")) {
                emit_page_break(ctx, info);
            }
            down = fz_xml_down(pos);
            if (down) {
                pos = down;
                continue;
            }
        }
        next = fz_xml_next(pos);
        if (next) {
            pos = next;
            continue;
        }

        while (1) {
            pos = fz_xml_up(pos);
            if (pos == NULL) break;
            if (fz_xml_is_tag(pos, "p")) {
                if (paragraph_style) {
                    fz_write_printf(ctx, info->out, "</%s>", paragraph_style);
                    paragraph_style = NULL;
                }
                fz_write_string(ctx, info->out, "</p>\n");
            } else if (fz_xml_is_tag(pos, "r")) {
                if (inline_style) {
                    fz_write_printf(ctx, info->out, "</%s>", inline_style);
                    inline_style = NULL;
                }
            }
            next = fz_xml_next(pos);
            if (next) {
                pos = next;
                break;
            }
        }
    }

    if (do_pages && info->page) fz_write_string(ctx, info->out, "\n</div>\n");
}

static void process_doc_stream(fz_context* ctx, fz_xml* xml, doc_info* info, int do_pages) {
    fz_xml* body;

#ifdef DEBUG_OFFICE_TO_HTML
    fz_write_printf(ctx, fz_stddbg(ctx), "process_doc_stream:\n");
    fz_output_xml(ctx, fz_stddbg(ctx), xml, 0);
#endif

    info->do_pages = do_pages;
    info->list_kind = 0;

    body = fz_xml_find_dfs(xml, "body", NULL, NULL);
    if (body) {
        if (do_pages && fz_xml_find_dfs(body, "lastRenderedPageBreak", NULL, NULL)) {
            fz_write_string(ctx, info->out, "<div id=\"page1\">\n");
            info->page = 1;
        }
        emit_word_body(ctx, body, info);
        if (do_pages && info->page) fz_write_string(ctx, info->out, "\n</div>\n");
        return;
    }

    process_doc_stream_fallback(ctx, xml, info, do_pages);
}

static void process_item(fz_context* ctx, fz_archive* arch, const char* file, doc_info* info, int do_pages) {
    fz_xml* xml = fz_parse_xml_archive_entry(ctx, arch, file, 1);

    fz_try(ctx) process_doc_stream(ctx, xml, info, do_pages);
    fz_always(ctx) fz_drop_xml(ctx, xml);
    fz_catch(ctx) fz_rethrow(ctx);
}

static void process_rootfile(fz_context* ctx, fz_archive* arch, const char* file, doc_info* info) {
    fz_xml* xml = fz_parse_xml_archive_entry(ctx, arch, file, 0);

    fz_try(ctx) {
        /* FIXME: Should really search for these just inside 'spine'. */
        fz_xml* pos = fz_xml_find_dfs(xml, "itemref", NULL, NULL);
        while (pos) {
            char* idref = fz_xml_att(pos, "idref");
            fz_xml* item = fz_xml_find_dfs(xml, "item", "id", idref);
            while (item) {
                char* type = fz_xml_att(item, "media-type");
                char* href = fz_xml_att(item, "href");
                if (type && href && !strcmp(type, "application/xml")) {
                    process_item(ctx, arch, href, info, 1);
                }
                item = fz_xml_find_next_dfs(pos, "item", "id", idref);
            }
            pos = fz_xml_find_next_dfs(pos, "itemref", NULL, NULL);
        }
    }
    fz_always(ctx) fz_drop_xml(ctx, xml);
    fz_catch(ctx) fz_rethrow(ctx);
}

/* XLSX support */
static char* make_rel_name(fz_context* ctx, const char* file) {
    size_t z = strlen(file);
    char* s = fz_malloc(ctx, z + 12);
    char* t;
    const char* p;
    const char* slash = file;

    for (p = file; *p != 0; p++)
        if (*p == '/') slash = p + 1;

    t = s;
    if (slash != file) {
        memcpy(t, file, slash - file);
        t += slash - file;
    }
    memcpy(t, "_rels/", 6);
    t += 6;
    memcpy(t, file + (slash - file), z - (slash - file));
    t += z - (slash - file);
    memcpy(t, ".rels", 6);

    return s;
}

static char* lookup_rel(fz_context* ctx, fz_xml* rels, const char* id) {
    fz_xml* pos;

    if (id == NULL) return NULL;

    pos = fz_xml_find_dfs(rels, "Relationship", NULL, NULL);
    while (pos) {
        char* id2 = fz_xml_att(pos, "Id");

        if (id2 && !strcmp(id, id2)) return fz_xml_att(pos, "Target");

        pos = fz_xml_find_next_dfs(pos, "Relationship", NULL, NULL);
    }

    return NULL;
}

static void send_cell_formatting(fz_context* ctx, doc_info* info) {
    if (info->col_signalled == 0) {
        fz_write_string(ctx, info->out, "<tr>\n");
        info->col_signalled = 1;
        if (info->col_at > 1) fz_write_string(ctx, info->out, "<td>");
    }

    /* Send the label */
    while (info->col_signalled < info->col_at) {
        fz_write_string(ctx, info->out, "</td>");
        info->col_signalled++;
        if (info->col_signalled < info->col_at) fz_write_string(ctx, info->out, "<td>");
    }
    if (info->sheet_name && info->sheet_name[0])
        fz_write_printf(ctx, info->out, "<td id=\"%s!%s\">", info->sheet_name, info->label);
    else
        fz_write_printf(ctx, info->out, "<td id=\"%s\">", info->label);
}

static void show_shared_string(fz_context* ctx, fz_xml* v, doc_info* info) {
    const char* t = fz_xml_text(fz_xml_down(v));
    int n = fz_atoi(t);

    if (n < 0 || n >= info->shared_string_len) return;

    if (info->shared_strings[n] == NULL || info->shared_strings[n][0] == 0) return;

    send_cell_formatting(ctx, info);
    /* Then send the strings. */
    doc_escape(ctx, info->out, info->shared_strings[n]);
}

static int col_from_label(const char* label) {
    int col = 0;
    int len = 26;
    int base = 0;

    /* If we can't read the column, return 0. */
    if (label == NULL || *label < 'A' || *label > 'Z') return 0;

    /*	Each section (A-Z, AA-ZZ, AAA-ZZZ etc) is of len 'len', and starts
     *	at base index 'base'. Each section is 26 times as long, and starts
     *	at base + len from the previous section.
     *
     *	A:	col = 26 * 0 + 0 + 0
     *	AA:	col = (26 * 0 + 0 + 0) * 26 + 0 + 26 = 26
     *	AAA:	col = (((26 * 0 + 0 + 0) * 26 + 0 + 26)*26 + 0 + 26*26 = 26 + 26 * 26
     */
    do {
        col = 26 * col + (*label++) - 'A' + base;
        base += len;
        len *= 26;
    } while (*label >= 'A' && *label <= 'Z');

    return col + 1;
}

static void show_cell_text(fz_context* ctx, fz_xml* top, doc_info* info) {
    fz_xml* pos = top;
    fz_xml* next;

    while (pos) {
        char* text = fz_xml_text(pos);

        if (text) {
            send_cell_formatting(ctx, info);
            doc_escape(ctx, info->out, text);
        }

        /* Always try to move down. */
        next = fz_xml_down(pos);
        if (next) {
            /* We can move down, easy! */
            pos = next;
            continue;
        }

        if (pos == top) break;

        /* We can't move down, try moving to next. */
        next = fz_xml_next(pos);
        if (next) {
            /* We can move to next, easy! */
            pos = next;
            continue;
        }

        /* If we can't go down, or next, pop up until we
         * find somewhere we can go next from. */
        while (1) {
            /* OK. So move up. */
            pos = fz_xml_up(pos);
            /* Check for hitting the top. */
            if (pos == top) pos = NULL;
            if (pos == NULL) break;
            next = fz_xml_next(pos);
            if (next) {
                pos = next;
                break;
            }
        }
    }
}

static void arrived_at_cell(fz_context* ctx, doc_info* info, const char* label) {
    int col;

    /* If we have a label queued, and no label is given here, then we're
     * processing a 'cell' callback after having had a 'cellname'
     * callback. So don't signal it twice! */
    if (label == NULL && info->label) return;

    col = label ? col_from_label(label) : 0;

    fz_free(ctx, info->label);
    info->label = NULL;
    info->label = label ? fz_strdup(ctx, label) : NULL;
    info->col_at = col;
}

static void show_cell(fz_context* ctx, fz_xml* cell, doc_info* info) {
    char* t = fz_xml_att(cell, "t");
    fz_xml* v = fz_xml_find_down(cell, "v");
    const char* r = fz_xml_att(cell, "r");

    arrived_at_cell(ctx, info, r);
    if (t && t[0] == 's' && t[1] == 0)
        show_shared_string(ctx, v, info);
    else
        show_cell_text(ctx, v, info);
}

static void new_row(fz_context* ctx, doc_info* info) {
    if (info->col_signalled) {
        /* We've sent at least one cell. So need to close the
         * td and tr */
        fz_write_string(ctx, info->out, "</td>\n</tr>\n");
    } else {
        /* We've not sent anything for this row. Keep the counts
         * correct. */
        fz_write_string(ctx, info->out, "<tr></tr>\n");
    }
    info->col_at = 1;
    info->col_signalled = 0;
    fz_free(ctx, info->label);
    info->label = NULL;
}

static void process_sheet(fz_context* ctx, fz_archive* arch, const char* name, const char* file, doc_info* info) {
    fz_xml* xml = fz_parse_xml_archive_entry(ctx, arch, file, 1);

#ifdef DEBUG_OFFICE_TO_HTML
    fz_write_printf(ctx, fz_stddbg(ctx), "process_sheet:\n");
    fz_output_xml(ctx, fz_stddbg(ctx), xml, 0);
#endif

    fz_write_printf(ctx, info->out, "<table id=\"%s\">\n", name);

    info->sheet_name = name;
    info->col_at = 0;
    info->col_signalled = 0;

    fz_try(ctx) {
        fz_xml* pos = xml;
        fz_xml* next;

        while (pos) {
            /* When we arrive on a node, check if it's a cell. */
            if (fz_xml_is_tag(pos, "c")) {
                show_cell(ctx, pos, info);
                /* Do NOT go down, we've already dealt with that. */
            } else {
                /* Try to move down. */
                next = fz_xml_down(pos);
                if (next) {
                    /* We can move down, easy! */
                    pos = next;
                    continue;
                }
            }
            /* Try moving to next. */
            next = fz_xml_next(pos);
            if (next) {
                /* We can move to next, easy! */
                pos = next;
                continue;
            }

            /* If we can't go down, or next, pop up until we
             * find somewhere we can go next from. */
            while (1) {
                /* OK. So move up. */
                pos = fz_xml_up(pos);
                /* Check for hitting the top. */
                if (pos == NULL) break;

                /* We've returned to a node. See if it's a 'row'. */
                if (fz_xml_is_tag(pos, "row")) new_row(ctx, info);

                next = fz_xml_next(pos);
                if (next) {
                    pos = next;
                    break;
                }
            }
        }
        if (info->col_signalled) fz_write_printf(ctx, info->out, "</td>\n</tr>\n");
        fz_write_printf(ctx, info->out, "</table>\n");
    }
    fz_always(ctx) fz_drop_xml(ctx, xml);
    fz_catch(ctx) fz_rethrow(ctx);
}

static void process_slide(fz_context* ctx, fz_archive* arch, const char* file, doc_info* info) {
    fz_write_printf(ctx, info->out, "<div id=\"slide%d\">\n", info->page++);
    process_item(ctx, arch, file, info, 0);
    fz_write_printf(ctx, info->out, "</div>\n");
}

static char* make_absolute_path(fz_context* ctx, const char* abs, const char* rel) {
    const char* a = abs;
    const char* aslash = a;
    int up = 0;
    size_t z1, z2;
    char* s;

    if (rel == NULL) return NULL;
    if (abs == NULL || *rel == '/') return fz_strdup(ctx, rel);

    for (a = abs; *a != 0; a++)
        if (*a == '/') aslash = a + 1;

    while (rel[0] == '.') {
        if (rel[1] == '/')
            rel += 2;
        else if (rel[1] == '.' && rel[2] == '/')
            rel += 3, up++;
        else
            fz_throw(ctx, FZ_ERROR_FORMAT, "Unresolvable path");
    }
    if (rel[0] == 0) fz_throw(ctx, FZ_ERROR_FORMAT, "Unresolvable path");

    while (up) {
        while (aslash != abs && aslash[-1] != '/') aslash--;

        up--;
    }

    z1 = aslash - abs;
    z2 = strlen(rel);
    s = fz_malloc(ctx, z1 + z2 + 1);
    if (z1) memcpy(s, abs, z1);
    memcpy(s + z1, rel, z2 + 1);

    return s;
}

static char* collate_t_content(fz_context* ctx, fz_xml* top) {
    char* val = NULL;
    fz_xml* next;
    fz_xml* pos = fz_xml_down(top);

    while (pos != top) {
        /* Capture all the 't' content. */
        if (fz_xml_is_tag(pos, "t")) {
            /* Remember the content. */
            char* s = fz_xml_text(fz_xml_down(pos));

            if (s == NULL) {
                /* Do nothing */
            } else if (val == NULL)
                val = fz_strdup(ctx, s);
            else {
                char* val2;
                size_t z1 = strlen(val);
                size_t z2 = strlen(s) + 1;
                fz_try(ctx) {
                    val2 = fz_malloc(ctx, z1 + z2);
                }
                fz_catch(ctx) {
                    fz_free(ctx, val);
                    fz_rethrow(ctx);
                }
                memcpy(val2, val, z1);
                memcpy(val2 + z1, s, z2);
                fz_free(ctx, val);
                val = val2;
            }
            /* Do NOT go down, we've already dealt with that. */
        } else if (fz_xml_is_tag(pos, "rPr") || fz_xml_is_tag(pos, "rPh")) {
            /* We do not want the 't' content from within these. */
        } else {
            /* Try to move down. */
            next = fz_xml_down(pos);
            if (next) {
                /* We can move down, easy! */
                pos = next;
                continue;
            }
        }
        /* Try moving to next. */
        next = fz_xml_next(pos);
        if (next) {
            /* We can move to next, easy! */
            pos = next;
            continue;
        }

        /* If we can't go down, or next, pop up until we
         * find somewhere we can go next from. */
        while (1) {
            /* OK. So move up. */
            pos = fz_xml_up(pos);
            /* Check for hitting the top. */
            if (pos == top) break;
            next = fz_xml_next(pos);
            if (next) {
                pos = next;
                break;
            }
        }
    }

    return val;
}

static fz_xml* try_parse_xml_archive_entry(fz_context* ctx, fz_archive* arch, const char* filename,
                                           int preserve_white) {
    if (!fz_has_archive_entry(ctx, arch, filename)) return NULL;

    return fz_parse_xml_archive_entry(ctx, arch, filename, preserve_white);
}

static void load_shared_strings(fz_context* ctx, fz_archive* arch, fz_xml* rels, doc_info* info, const char* file) {
    fz_xml* pos = fz_xml_find_dfs(rels, "Relationship", "Type",
                                  "http://schemas.openxmlformats.org/officeDocument/2006/relationships/sharedStrings");
    const char* ss_file = fz_xml_att(pos, "Target");
    char* resolved = NULL;
    fz_xml* xml = NULL;
    char* str = NULL;

    if (ss_file == NULL) return;

    fz_var(xml);
    fz_var(str);
    fz_var(resolved);

    fz_try(ctx) {
        resolved = make_absolute_path(ctx, file, ss_file);
        xml = fz_parse_xml_archive_entry(ctx, arch, resolved, 1);

        pos = fz_xml_find_dfs(xml, "si", NULL, NULL);
        while (pos) {
            int n = info->shared_string_len;
            str = collate_t_content(ctx, pos);

            if (n == info->shared_string_max) {
                int max = info->shared_string_max;
                int newmax = max ? max * 2 : 1024;
                char** arr = fz_realloc(ctx, info->shared_strings, sizeof(*arr) * newmax);
                memset(&arr[max], 0, sizeof(*arr) * (newmax - max));
                info->shared_strings = arr;
                info->shared_string_max = newmax;
            }

            info->shared_strings[n] = str;
            str = NULL;
            info->shared_string_len++;
            pos = fz_xml_find_next_dfs(pos, "si", NULL, NULL);
        }
    }
    fz_always(ctx) {
        fz_drop_xml(ctx, xml);
        fz_free(ctx, resolved);
        fz_free(ctx, str);
    }
    fz_catch(ctx) fz_rethrow(ctx);
}

static void load_footnotes(fz_context* ctx, fz_archive* arch, fz_xml* rels, doc_info* info, const char* file) {
    char* resolved = NULL;
    fz_xml* xml = NULL;
    char* str = NULL;

    fz_var(xml);
    fz_var(str);
    fz_var(resolved);

    fz_try(ctx) {
        fz_xml* pos;

        resolved = make_absolute_path(ctx, file, "footnotes.xml");
        xml = try_parse_xml_archive_entry(ctx, arch, resolved, 1);
        if (xml == NULL) break;

        pos = fz_xml_find_dfs(xml, "footnote", NULL, NULL);
        while (pos) {
            int n = fz_atoi(fz_xml_att(pos, "w:id"));

            str = collate_t_content(ctx, pos);

            if (str && n >= 0) {
                if (n >= info->footnotes_max) {
                    int max = info->footnotes_max;
                    int newmax = max ? max * 2 : 1024;
                    char** arr;
                    if (newmax < n) newmax = n + 1;
                    arr = fz_realloc(ctx, info->footnotes, sizeof(*arr) * newmax);
                    memset(&arr[max], 0, sizeof(*arr) * (newmax - max));
                    info->footnotes = arr;
                    info->footnotes_max = newmax;
                }

                info->footnotes[n] = str;
                str = NULL;
            }
            pos = fz_xml_find_next_dfs(pos, "footnote", NULL, NULL);
        }
    }
    fz_always(ctx) {
        fz_drop_xml(ctx, xml);
        fz_free(ctx, resolved);
        fz_free(ctx, str);
    }
    fz_catch(ctx) fz_rethrow(ctx);
}

static void process_office_document(fz_context* ctx, fz_archive* arch, const char* file, doc_info* info) {
    char* file_rels;
    fz_xml* xml = NULL;
    fz_xml* rels = NULL;
    char* resolved_rel = NULL;

    if (file == NULL) return;

    file_rels = make_rel_name(ctx, file);

    fz_var(resolved_rel);

    fz_var(rels);
    fz_var(xml);

    fz_try(ctx) {
        fz_xml* pos;

        rels = fz_parse_xml_archive_entry(ctx, arch, file_rels, 0);
        xml = fz_parse_xml_archive_entry(ctx, arch, file, 1);

        /* XLSX */
        pos = fz_xml_find_dfs(xml, "sheet", NULL, NULL);
        if (pos) {
            load_shared_strings(ctx, arch, rels, info, file);
            while (pos) {
                char* name = fz_xml_att(pos, "name");
                char* id = fz_xml_att(pos, "r:id");
                char* sheet = lookup_rel(ctx, rels, id);

                if (sheet) {
                    resolved_rel = make_absolute_path(ctx, file, sheet);
                    process_sheet(ctx, arch, name, resolved_rel, info);
                    fz_free(ctx, resolved_rel);
                    resolved_rel = NULL;
                }
                pos = fz_xml_find_next_dfs(pos, "sheet", NULL, NULL);
            }
            break;
        }

        /* Let's try it as a powerpoint */
        pos = fz_xml_find_dfs(xml, "sldId", NULL, NULL);
        if (pos) {
            while (pos) {
                char* id = fz_xml_att(pos, "r:id");
                char* sheet = lookup_rel(ctx, rels, id);

                if (sheet) {
                    resolved_rel = make_absolute_path(ctx, file, sheet);
                    process_slide(ctx, arch, resolved_rel, info);
                    fz_free(ctx, resolved_rel);
                    resolved_rel = NULL;
                }
                pos = fz_xml_find_next_dfs(pos, "sldId", NULL, NULL);
            }
            break;
        }

        /* Let's try it as word. */
        {
            info->arch = arch;
            info->doc_file = file;
            info->doc_rels = rels;
            load_word_theme_fonts(ctx, arch, info);
            load_word_styles(ctx, arch, info, file);
            if (info->default_pstyle && info->doc_body_font_pt < 0.5f)
                info->doc_body_font_pt = word_style_resolve_font_pt(info, info->default_pstyle);
            load_word_numbering(ctx, arch, info, file);
            load_footnotes(ctx, arch, rels, info, file);
            /* do_pages=0: ignore Word lastRenderedPageBreak page <div>s. Mid-paragraph
             * breaks already no-op, but empty/spacer paras at page edges still left gaps;
             * MuPDF reflow paginates the continuous HTML. */
            process_doc_stream(ctx, xml, info, 0);
        }
    }
    fz_always(ctx) {
        fz_drop_xml(ctx, xml);
        fz_drop_xml(ctx, rels);
        fz_free(ctx, resolved_rel);
        fz_free(ctx, file_rels);
    }
    fz_catch(ctx) fz_rethrow(ctx);
}

static void process_office_document_properties(fz_context* ctx, fz_archive* arch, const char* file, doc_info* info) {
    fz_xml* xml = NULL;
    char* title;

    fz_var(xml);

    fz_try(ctx) {
        fz_xml* pos;

        xml = fz_parse_xml_archive_entry(ctx, arch, file, 1);

        pos = fz_xml_find_dfs(xml, "title", NULL, NULL);
        title = fz_xml_text(fz_xml_down(pos));
        if (title) {
            fz_write_string(ctx, info->out, "<title>");
            doc_escape(ctx, info->out, title);
            fz_write_string(ctx, info->out, "</title>");
        }
    }
    fz_always(ctx) {
        fz_drop_xml(ctx, xml);
    }
    fz_catch(ctx) fz_rethrow(ctx);
}

static fz_buffer* fz_office_to_html(fz_context* ctx, fz_html_font_set* set, fz_buffer* buffer_in, fz_archive* dir,
                                    fz_office_to_html_opts* opts) {
    fz_stream* stream = NULL;
    fz_archive* archive = NULL;
    fz_buffer* buffer_out = NULL;
    fz_xml* xml = NULL;
    fz_xml* pos = NULL;
    fz_xml* rels = NULL;
    const char* schema = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument";
    const char* schema_props = "http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties";
    doc_info info = {0};
    int i;

    fz_var(archive);
    fz_var(stream);
    fz_var(buffer_out);
    fz_var(xml);
    fz_var(rels);

    if (opts) info.opts = *opts;

    fz_try(ctx) {
        if (buffer_in) {
            stream = fz_open_buffer(ctx, buffer_in);
            archive = fz_open_archive_with_stream(ctx, stream);
        } else
            archive = fz_keep_archive(ctx, dir);
        buffer_out = fz_new_buffer(ctx, 1024);
        info.out = fz_new_output_with_buffer(ctx, buffer_out);

        /* Is it an HWPX ?*/
        xml = try_parse_xml_archive_entry(ctx, archive, "META-INF/container.xml", 0);
        if (xml) {
            pos = fz_xml_find_dfs(xml, "rootfile", "media-type", "application/hwpml-package+xml");
            if (!pos) fz_throw(ctx, FZ_ERROR_FORMAT, "Archive not hwpx.");

            while (pos) {
                const char* file = fz_xml_att(pos, "full-path");
                process_rootfile(ctx, archive, file, &info);
                pos = fz_xml_find_next_dfs(pos, "rootfile", "media-type", "application/hwpml-package+xml");
            }
            fz_close_output(ctx, info.out);
            break;
        }

        /* Try other types */
        {
            float page_mt, page_mr, page_mb, page_ml, page_w, page_h;
            const char* office_doc = NULL;

            xml = try_parse_xml_archive_entry(ctx, archive, "_rels/.rels", 0);

            fz_write_string(ctx, info.out, "<html>\n");
            fz_write_string(ctx, info.out, "<head>\n");

            pos = fz_xml_find_dfs(xml, "Relationship", "Type", schema_props);
            if (pos) {
                const char* file = fz_xml_att(pos, "Target");
                process_office_document_properties(ctx, archive, file, &info);
            }

            /* Match Word printable width so centered titles wrap like in Word
             * (titles often have no <w:br/> — only page margins force the break). */
            pos = fz_xml_find_dfs(xml, "Relationship", "Type", schema);
            if (pos) office_doc = fz_xml_att(pos, "Target");
            word_load_page_box(ctx, archive, office_doc, &page_mt, &page_mr, &page_mb, &page_ml, &page_w, &page_h);
            info.sect_pw = page_w;
            info.sect_ph = page_h;
            info.sect_mt = page_mt;
            info.sect_mr = page_mr;
            info.sect_mb = page_mb;
            info.sect_ml = page_ml;
            info.page_content_pt = page_w - page_ml - page_mr;
            if (info.page_content_pt < 200.0f) info.page_content_pt = page_w > 200.0f ? page_w * 0.72f : 440.0f;

            /* Resolve Normal/default body size before emitting CSS (runs often omit w:sz). */
            if (office_doc) {
                load_word_theme_fonts(ctx, archive, &info);
                load_word_styles(ctx, archive, &info, office_doc);
                if (info.default_pstyle && info.doc_body_font_pt < 0.5f)
                    info.doc_body_font_pt = word_style_resolve_font_pt(&info, info.default_pstyle);
            }

            {
                float body_pt = info.doc_body_font_pt > 0.5f ? info.doc_body_font_pt : 12.0f;
                fz_write_string(ctx, info.out, "<style type=\"text/css\">\n");
                fz_write_printf(ctx, info.out, "@page{size:%.1fpt %.1fpt;margin:%.1fpt %.1fpt %.1fpt %.1fpt;}\n",
                                page_w, page_h, page_mt, page_mr, page_mb, page_ml);
                fz_write_printf(
                    ctx, info.out,
                    /* Prefer 公文 faces (宋体/仿宋) over YaHei; runs override via rFonts.
                     * Body size from styles.xml Normal when present (often 16pt 仿宋公文). */
                    "body{font-family:\"宋体\",\"SimSun\",\"NSimSun\",\"FangSong\",\"FangSong_GB2312\","
                    "\"Times New Roman\",serif;font-size:%.1fpt;line-height:1.55;margin:0;padding:0;color:#222;}\n",
                    body_pt);
            }
            fz_write_string(ctx, info.out,
                            /* Word already sets run sizes. h1{1.7em} wrapped titles such as
                             * 整改清单 onto a second line that Word keeps on one line. */
                            "h1,h2,h3,h4,h5,h6{font-size:1em;font-weight:inherit;line-height:inherit;margin:0;}\n"
                            /* Typed 一、/（一）/1. outline: keep body font — only TOC level changes. */
                            "h1.Outline,h2.Outline,h3.Outline,h4.Outline,h5.Outline,h6.Outline{"
                            "font-size:1em;font-weight:normal;line-height:inherit;margin:0;}\n"
                            "p{margin:0;}\n"
                            "span.w-num{font:inherit;}\n"
                            ".Title{font-size:1.85em;font-weight:600;text-align:center;margin:1.2em 0 .6em;}\n"
                            ".Subtitle{font-size:1.25em;text-align:center;color:#444;margin:0 0 1em;}\n"
                            "table{border-collapse:collapse;border-spacing:0;margin:.8em 0;width:100%;"
                            "max-width:100%;table-layout:fixed;empty-cells:show;page-break-inside:auto;}\n"
                            "td,th{padding:.2em .35em;vertical-align:top;overflow-wrap:break-word;}\n"
                            "table p{margin:.12em 0;text-indent:0;}\n"
                            "img{max-width:100%;height:auto;display:block;margin:.6em auto;}\n"
                            "ul,ol{margin:.5em 0;padding-left:1.7em;}\n"
                            "li{margin:.2em 0;}\n"
                            "pre{font-family:Consolas,\"Courier New\",monospace;white-space:pre-wrap;"
                            "background:#f6f6f6;padding:.6em .8em;border-radius:3px;}\n"
                            "h6.WordBookmark{font-size:0.95em;font-weight:600;color:#444;margin:.8em 0 .3em;}\n"
                            "</style>\n");
            fz_write_string(ctx, info.out, "</head>\n");

            fz_write_string(ctx, info.out, "<body>\n");
            pos = fz_xml_find_dfs(xml, "Relationship", "Type", schema);
            if (!pos) fz_throw(ctx, FZ_ERROR_FORMAT, "Archive not docx.");

            while (pos) {
                const char* file = fz_xml_att(pos, "Target");
                if (file) process_office_document(ctx, archive, file, &info);
                pos = fz_xml_find_next_dfs(pos, "Relationship", "Type", schema);
            }

            fz_write_string(ctx, info.out, "</body>\n</html>\n");
        }

        fz_close_output(ctx, info.out);
    }
    fz_always(ctx) {
        fz_drop_xml(ctx, rels);
        fz_drop_xml(ctx, xml);
        for (i = 0; i < info.shared_string_len; ++i) fz_free(ctx, info.shared_strings[i]);
        fz_free(ctx, info.shared_strings);
        for (i = 0; i < info.footnotes_max; ++i) fz_free(ctx, info.footnotes[i]);
        fz_free(ctx, info.footnotes);
        drop_word_styles(ctx, &info);
        fz_drop_output(ctx, info.out);
        fz_free(ctx, info.label);
        fz_drop_archive(ctx, archive);
        fz_drop_stream(ctx, stream);
    }
    fz_catch(ctx) {
        fz_drop_buffer(ctx, buffer_out);
        fz_rethrow(ctx);
    }

#ifdef DEBUG_OFFICE_TO_HTML
    {
        unsigned char* storage;
        size_t len = fz_buffer_storage(ctx, buffer_out, &storage);
        fz_write_printf(ctx, fz_stddbg(ctx), "fz_office_to_html: Output buffer, len=%zd:\n", len);
        fz_write_buffer(ctx, fz_stddbg(ctx), buffer_out);
    }
#endif

    /* Optional dump for browser A/B: set SUMATRA_DUMP_OFFICE_HTML=<path.html> */
    {
        const char* dump_path = getenv("SUMATRA_DUMP_OFFICE_HTML");
        if (dump_path && dump_path[0] && buffer_out) {
            fz_output* dump = NULL;
            fz_var(dump);
            fz_try(ctx) {
                dump = fz_new_output_with_path(ctx, dump_path, 0);
                fz_write_buffer(ctx, dump, buffer_out);
                fz_close_output(ctx, dump);
            }
            fz_always(ctx) fz_drop_output(ctx, dump);
            fz_catch(ctx) fz_report_error(ctx);
        }
    }

    return buffer_out;
}

/* Office document handler */

static fz_buffer* office_to_html(fz_context* ctx, fz_html_font_set* set, fz_buffer* buf, fz_archive* zip) {
    fz_office_to_html_opts opts = {0};

    return fz_office_to_html(ctx, set, buf, zip, &opts);
}

static const fz_htdoc_format_t fz_htdoc_office = {"Office document", office_to_html, 0, 1, FZ_HTML_FLAVOR_DEFAULT};

static fz_document* office_open_document(fz_context* ctx, const fz_document_handler* handler, fz_stream* file,
                                         fz_stream* accel, fz_archive* zip, void* state) {
    fz_archive* arch = NULL;
    fz_document* doc = NULL;

    fz_var(arch);
    fz_var(doc);

    /* HTML image loads use doc->zip. Opening from a bare stream used to leave
     * zip NULL after convert_to_html dropped its temporary archive — images
     * then failed with "cannot load image src='word/media/...'". Keep the
     * OOXML zip on the document for the whole lifetime. */
    fz_try(ctx) {
        if (zip)
            arch = fz_keep_archive(ctx, zip);
        else if (file)
            arch = fz_open_archive_with_stream(ctx, file);
        else
            fz_throw(ctx, FZ_ERROR_ARGUMENT, "no office document to open");

        doc = fz_htdoc_open_document_with_stream_and_dir(ctx, NULL, arch, &fz_htdoc_office);
    }
    fz_always(ctx) fz_drop_archive(ctx, arch);
    fz_catch(ctx) fz_rethrow(ctx);

    return doc;
}

static const char* office_extensions[] = {"docx", "xlsx", "pptx", "hwpx", NULL};

static const char* office_mimetypes[] = {
    // DOCX
    "application/vnd.openxmlformats-officedocument.wordprocessingml.document",
    // XLSX
    "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet",
    // PPTX
    "application/vnd.openxmlformats-officedocument.presentationml.presentation",
    // HWPX
    "application/haansofthwpx", "application/vnd.hancom.hwpx", NULL};

/* We are only ever 75% sure here, to allow a 'better' handler, such as sodochandler
 * to override us by returning 100. */
static int office_recognize_doc_content(fz_context* ctx, const fz_document_handler* handler, fz_stream* stream,
                                        fz_archive* zip, void** state,
                                        fz_document_recognize_state_free_fn** free_state) {
    fz_archive* arch = NULL;
    int ret = 0;
    fz_xml* xml = NULL;

    if (state) *state = NULL;
    if (free_state) *free_state = NULL;

    fz_var(arch);
    fz_var(ret);
    fz_var(xml);

    fz_try(ctx) {
        if (stream) {
            arch = fz_try_open_archive_with_stream(ctx, stream);
            if (arch == NULL) break;
        } else
            arch = fz_keep_archive(ctx, zip);

        xml = fz_try_parse_xml_archive_entry(ctx, arch, "META-INF/container.xml", 0);
        if (xml) {
            if (fz_xml_find_dfs(xml, "rootfile", "media-type", "application/hwpml-package+xml")) ret = 75; /* HWPX */
            break;
        }
        xml = fz_try_parse_xml_archive_entry(ctx, arch, "_rels/.rels", 0);
        if (xml) {
            if (fz_xml_find_dfs(xml, "Relationship", "Type",
                                "http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument")) {
                ret = 75; /* DOCX | PPTX | XLSX */
            }
            break;
        }
    }
    fz_always(ctx) {
        fz_drop_xml(ctx, xml);
        fz_drop_archive(ctx, arch);
    }
    fz_catch(ctx) fz_rethrow(ctx);

    return ret;
}

fz_document_handler office_document_handler = {NULL, office_open_document, office_extensions, office_mimetypes,
                                               office_recognize_doc_content};
