/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "mupdf/fitz.h"
#include <stdio.h>

int main(int argc, char** argv) {
    fz_context* ctx = fz_new_context(NULL, NULL, FZ_STORE_DEFAULT);
    fz_document* doc = NULL;
    fz_stext_page* text = NULL;
    int result = 0;
    if (!ctx || argc != 2) return 2;
    fz_register_document_handlers(ctx);
    fz_var(doc);
    fz_var(text);
    fz_var(result);
    fz_try(ctx) {
        fz_stext_options opts = {0};
        int inherited = 0, explicit_size = 0, complex = 0, images = 0;
        doc = fz_open_document(ctx, argv[1]);
        fz_layout_document(ctx, doc, 595, 842, 12);
        opts.flags = FZ_STEXT_PRESERVE_IMAGES;
        text = fz_new_stext_page_from_page_number(ctx, doc, 0, &opts);
        for (fz_stext_block* block = text->first_block; block; block = block->next) {
            if (block->type == FZ_STEXT_BLOCK_IMAGE) images++;
            if (block->type != FZ_STEXT_BLOCK_TEXT) continue;
            for (fz_stext_line* line = block->u.t.first_line; line; line = line->next) {
                fz_stext_char* ch = line->first_char;
                if (!ch) continue;
                if (ch->c == 'I' && fz_abs(ch->size - 16) < 0.1f) inherited++;
                if (ch->c == 'E' && fz_abs(ch->size - 10) < 0.1f) explicit_size++;
                if (ch->c == 'C' && fz_abs(ch->size - 11) < 0.1f) complex++;
            }
        }
        if (inherited != 1 || explicit_size != 1 || complex != 1 || images != 2)
            fz_throw(ctx, FZ_ERROR_GENERIC, "format regression: inherited=%d explicit=%d complex=%d vectors=%d",
                     inherited, explicit_size, complex, images);
    }
    fz_always(ctx) {
        fz_drop_stext_page(ctx, text);
        fz_drop_document(ctx, doc);
    }
    fz_catch(ctx) {
        fprintf(stderr, "%s\n", fz_caught_message(ctx));
        result = 1;
    }
    fz_drop_context(ctx);
    if (!result) puts("PASS: inherited/explicit/complex sizes, WordArt and VML rule");
    return result;
}
