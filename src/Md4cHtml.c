/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "Md4cHtml.h"

#include "md4c-html.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    char* data;
    size_t size;
    size_t cap;
} MdHtmlBuf;

static int MdHtmlBufGrow(MdHtmlBuf* b, size_t need) {
    if (b->size + need + 1 <= b->cap) {
        return 1;
    }
    size_t newCap = b->cap ? b->cap * 2 : 4096;
    while (newCap < b->size + need + 1) {
        newCap *= 2;
    }
    char* nd = (char*)realloc(b->data, newCap);
    if (!nd) {
        return 0;
    }
    b->data = nd;
    b->cap = newCap;
    return 1;
}

static void MdHtmlAppend(const MD_CHAR* text, MD_SIZE size, void* userdata) {
    MdHtmlBuf* b = (MdHtmlBuf*)userdata;
    if (!text || size == 0) {
        return;
    }
    if (!MdHtmlBufGrow(b, size)) {
        return;
    }
    memcpy(b->data + b->size, text, size);
    b->size += size;
    b->data[b->size] = '\0';
}

int Md4cMarkdownToHtmlBody(const char* markdown, size_t markdownLen, char** htmlOut, size_t* htmlLenOut) {
    if (!markdown || !htmlOut || !htmlLenOut) {
        return -1;
    }
    *htmlOut = NULL;
    *htmlLenOut = 0;

    MdHtmlBuf buf;
    buf.data = NULL;
    buf.size = 0;
    buf.cap = 0;
    unsigned parserFlags = MD_DIALECT_GITHUB;
    unsigned renderFlags = MD_HTML_FLAG_SKIP_UTF8_BOM;
    int ok = md_html(markdown, (MD_SIZE)markdownLen, MdHtmlAppend, &buf, parserFlags, renderFlags);
    if (ok != 0) {
        free(buf.data);
        return ok;
    }
    if (!buf.data || buf.size == 0) {
        buf.data = (char*)malloc(8);
        if (!buf.data) {
            return -1;
        }
        memcpy(buf.data, "<p></p>", 8);
        buf.size = 7;
        buf.data[7] = '\0';
    }
    *htmlOut = buf.data;
    *htmlLenOut = buf.size;
    return 0;
}

void Md4cFree(void* p) {
    free(p);
}
