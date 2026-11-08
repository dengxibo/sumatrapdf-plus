/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */
#include "mupdf/fitz.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

void fz_register_ofd_handler(fz_context* ctx);

int main(int argc, char** argv) {
    fz_context* ctx = fz_new_context(NULL, NULL, FZ_STORE_DEFAULT);
    if (!ctx || argc < 2) return 1;
    fz_register_document_handlers(ctx);
    fz_register_ofd_handler(ctx);
    int result = 0;
    for (int i = 1; i < argc; ++i) {
        fz_document* doc = NULL;
        fz_outline* outline = NULL;
        fz_pixmap* pix = NULL;
        fz_stext_page* text = NULL;
        fz_buffer* buf = NULL;
        fz_var(doc);
        fz_var(outline);
        fz_var(pix);
        fz_var(text);
        fz_var(buf);
        fz_try(ctx) {
            doc = fz_open_document(ctx, argv[i]);
            int count = fz_count_pages(ctx, doc);
            int fixture = strstr(argv[i], "fixture.") != NULL;
            if (count < 1 || (fixture && count != 2)) fz_throw(ctx, FZ_ERROR_GENERIC, "wrong page count");
            if (fixture) {
                outline = fz_load_outline(ctx, doc);
                if (!outline || !outline->title || strcmp(outline->title, "Second page") || outline->page.page != 1)
                    fz_throw(ctx, FZ_ERROR_GENERIC, "incorrect bookmark destination");
            }
            for (int p = 0; p < count; ++p) {
                pix = fz_new_pixmap_from_page_number(ctx, doc, p, fz_identity, fz_device_rgb(ctx), 0);
                if (fixture) {
                    if (abs(pix->w - 284) > 1 || abs(pix->h - 227) > 1)
                        fz_throw(ctx, FZ_ERROR_GENERIC, "incorrect millimetre conversion");
                    unsigned char* rgb = pix->samples + 40 * pix->stride + 40 * pix->n;
                    if (rgb[0] < 240 || rgb[1] > 20 || rgb[2] > 20)
                        fz_throw(ctx, FZ_ERROR_GENERIC, "missing red vector rectangle");
                    text = fz_new_stext_page_from_page_number(ctx, doc, p, NULL);
                    buf = fz_new_buffer_from_stext_page(ctx, text);
                    if (!strstr(fz_string_from_buffer(ctx, buf), "OFD regression"))
                        fz_throw(ctx, FZ_ERROR_GENERIC, "missing searchable text");
                    fz_drop_buffer(ctx, buf);
                    buf = NULL;
                    fz_drop_stext_page(ctx, text);
                    text = NULL;
                }
                fz_drop_pixmap(ctx, pix);
                pix = NULL;
            }
            printf("PASS: %s (%d pages)\n", argv[i], count);
        }
        fz_always(ctx) {
            fz_drop_buffer(ctx, buf);
            fz_drop_stext_page(ctx, text);
            fz_drop_pixmap(ctx, pix);
            fz_drop_outline(ctx, outline);
            fz_drop_document(ctx, doc);
        }
        fz_catch(ctx) {
            fprintf(stderr, "FAIL: %s: %s\n", argv[i], fz_caught_message(ctx));
            result = 1;
        }
    }
    fz_drop_context(ctx);
    return result;
}
