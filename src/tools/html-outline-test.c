/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

/* Regression: heading block margins must never be read as flow pointers. */
#include "mupdf/fitz.h"
#include <stdio.h>
#include <string.h>

/* The Markdown refresh path reparses and then rebuilds the heading outline. */
void fz_htdoc_reparse_html(fz_context* ctx, fz_document* doc, fz_buffer* buf, float w, float h, float em);

int main(void) {
    const char* html =
        "<html><head><style>h1,h2 { margin: 24px 12px; }</style></head><body>"
        "<h1>Alpha <em>Beta</em></h1><p>Body</p><h2>Child</h2>"
        "<h1>Next</h1></body></html>";
    fz_context* ctx = fz_new_context(NULL, NULL, FZ_STORE_DEFAULT);
    fz_document* doc = NULL;
    fz_stream* stream = NULL;
    fz_buffer* buffer = NULL;
    fz_outline* outline = NULL;
    int result = 0;
    if (!ctx) {
        return 1;
    }
    fz_register_document_handlers(ctx);
    fz_var(doc);
    fz_var(stream);
    fz_var(buffer);
    fz_var(outline);
    fz_var(result);
    fz_try(ctx) {
        stream = fz_open_memory(ctx, (const unsigned char*)html, strlen(html));
        doc = fz_open_document_with_stream(ctx, "text/html", stream);
        buffer = fz_new_buffer_from_copied_data(ctx, (const unsigned char*)html, strlen(html));
        for (int pass = 0; pass < 3; pass++) {
            fz_layout_document(ctx, doc, 400 + pass * 200, 600, 12 + pass * 2);
            fz_htdoc_reparse_html(ctx, doc, buffer, 400 + pass * 200, 600, 12 + pass * 2);
            outline = fz_load_outline(ctx, doc);
            if (!outline || !outline->title || strcmp(outline->title, "Alpha Beta") || !outline->down ||
                strcmp(outline->down->title, "Child") || !outline->next || strcmp(outline->next->title, "Next") ||
                outline->next->next) {
                fz_throw(ctx, FZ_ERROR_GENERIC, "incorrect heading hierarchy or text");
            }
            fz_drop_outline(ctx, outline);
            outline = NULL;
        }
    }
    fz_always(ctx) {
        fz_drop_outline(ctx, outline);
        fz_drop_document(ctx, doc);
        fz_drop_stream(ctx, stream);
        fz_drop_buffer(ctx, buffer);
    }
    fz_catch(ctx) {
        fprintf(stderr, "%s\n", fz_caught_message(ctx));
        result = 1;
    }
    fz_drop_context(ctx);
    if (!result) {
        puts("PASS: heading text and hierarchy after three layouts");
    }
    return result;
}
