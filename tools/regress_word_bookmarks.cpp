// Inspect Word bookmark rendering and outline titles without modifying the input.
#include <stdio.h>
#include <string.h>
extern "C" {
#include "mupdf/fitz.h"
void install_load_windows_font_funcs(fz_context* ctx);
}

static void WriteOutline(FILE* out, fz_outline* node) {
    for (; node; node = node->next) {
        fprintf(out, "%s\n", node->title ? node->title : "");
        WriteOutline(out, node->down);
    }
}

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    fz_context* ctx = fz_new_context(NULL, NULL, FZ_STORE_DEFAULT);
    install_load_windows_font_funcs(ctx);
    fz_register_document_handlers(ctx);
    int failed = 0;
    fz_try(ctx) {
        fz_document* doc = fz_open_document(ctx, argv[1]);
        fz_layout_document(ctx, doc, 600, 800, 12);
        char path[1024];
        snprintf(path, sizeof(path), "%s-text.txt", argv[2]);
        FILE* out = fopen(path, "wb");
        if (!out) fz_throw(ctx, FZ_ERROR_SYSTEM, "Cannot write text output");
        int pages = fz_count_pages(ctx, doc);
        for (int i = 0; i < pages; i++) {
            fz_page* page = fz_load_page(ctx, doc, i);
            fz_link* links = fz_load_links(ctx, page);
            for (fz_link* link = links; link; link = link->next) {
                if (link->uri && strchr(link->uri, '#') && !strstr(link->uri, "://")) {
                    fz_location dest = fz_resolve_link(ctx, doc, link->uri, NULL, NULL);
                    if (dest.page < 0) {
                        fprintf(stderr, "Unresolved bookmark: %s\n", link->uri);
                        failed = 1;
                    }
                    printf("Internal bookmark target: %s\n", dest.page >= 0 ? "PASS" : "FAIL");
                }
            }
            fz_drop_link(ctx, links);
            fz_stext_page* text = fz_new_stext_page_from_page(ctx, page, NULL);
            fz_buffer* buf = fz_new_buffer_from_stext_page(ctx, text);
            unsigned char* bytes;
            size_t size = fz_buffer_storage(ctx, buf, &bytes);
            fwrite(bytes, 1, size, out);
            if (i == 0) {
                fz_pixmap* pix = fz_new_pixmap_from_page(ctx, page, fz_identity, fz_device_rgb(ctx), 0);
                snprintf(path, sizeof(path), "%s-page1.png", argv[2]);
                fz_save_pixmap_as_png(ctx, pix, path);
                fz_drop_pixmap(ctx, pix);
            }
            fz_drop_buffer(ctx, buf);
            fz_drop_stext_page(ctx, text);
            fz_drop_page(ctx, page);
        }
        fclose(out);
        snprintf(path, sizeof(path), "%s-outline.txt", argv[2]);
        out = fopen(path, "wb");
        if (!out) fz_throw(ctx, FZ_ERROR_SYSTEM, "Cannot write outline output");
        fz_outline* outline = fz_load_outline(ctx, doc);
        WriteOutline(out, outline);
        fclose(out);
        fz_drop_outline(ctx, outline);
        printf("Rendered %d pages\n", pages);
        fz_drop_document(ctx, doc);
    }
    fz_catch(ctx) {
        fprintf(stderr, "%s\n", fz_caught_message(ctx));
        failed = 1;
    }
    fz_drop_context(ctx);
    return failed;
}
