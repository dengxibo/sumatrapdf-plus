// Minimal md4c HTML dump for cmd/test-md-convert.ts (no SumatraPDF utils).
#include "md4c-html.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char* data;
    size_t size;
    size_t cap;
} Buf;

static int grow(Buf* b, size_t need) {
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

static void append(const MD_CHAR* text, MD_SIZE size, void* userdata) {
    Buf* b = (Buf*)userdata;
    if (!text || size == 0) {
        return;
    }
    if (!grow(b, size)) {
        return;
    }
    memcpy(b->data + b->size, text, size);
    b->size += size;
    b->data[b->size] = '\0';
}

static char* readFile(const char* path, size_t* lenOut) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return NULL;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    char* buf = (char*)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    *lenOut = n;
    return buf;
}

static int writeFile(const char* path, const char* data, size_t len) {
    FILE* f = fopen(path, "wb");
    if (!f) {
        return 0;
    }
    size_t n = fwrite(data, 1, len, f);
    fclose(f);
    return n == len;
}

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: md-html-dump <input.md> <output.html>\n");
        return 2;
    }
    size_t mdLen = 0;
    char* md = readFile(argv[1], &mdLen);
    if (!md) {
        fprintf(stderr, "failed to read '%s'\n", argv[1]);
        return 1;
    }

    Buf body = {NULL, 0, 0};
    int ok = md_html(md, (MD_SIZE)mdLen, append, &body, MD_DIALECT_GITHUB, MD_HTML_FLAG_SKIP_UTF8_BOM);
    free(md);
    if (ok != 0 || !body.data) {
        fprintf(stderr, "md_html failed\n");
        free(body.data);
        return 1;
    }

    static const char* kCss = R"(
table, th, td { border: 1px solid #808080; }
table { border-collapse: collapse; width: 100%; }
th, td { padding: 0.45em 0.65em; }
pre { background-color: #f6f8fa; border: 1px solid #d0d7de; padding: 0.85em 1em; }
:not(pre) > code { background-color: #eff1f3; padding: 0.15em 0.35em; }
hr { border: none; border-top: 1px solid #808080; margin: 2em 0; }
blockquote { border-left: 4px solid #d0d7de; padding-left: 1em; }
)";

    size_t cssLen = strlen(kCss);
    size_t outCap = 512 + cssLen + body.size;
    char* out = (char*)malloc(outCap);
    if (!out) {
        free(body.data);
        return 1;
    }
    int n = snprintf(
        out, outCap,
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><style>%s</style></head><body>\n%s\n</body></html>\n", kCss,
        body.data);
    if (n < 0 || (size_t)n >= outCap) {
        free(out);
        free(body.data);
        return 1;
    }
    if (!writeFile(argv[2], out, (size_t)n)) {
        fprintf(stderr, "failed to write '%s'\n", argv[2]);
        free(out);
        free(body.data);
        return 1;
    }
    free(out);
    free(body.data);
    return 0;
}
