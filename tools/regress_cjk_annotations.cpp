// Regression test for upstream #6082: render CJK free-text with explicit or absent language.
#include <stdio.h>
#include <string.h>
extern "C" {
#include "mupdf/fitz.h"
#include "mupdf/pdf.h"
void install_load_windows_font_funcs(fz_context* ctx);
}
int main(int argc, char** argv) {
    fz_context* ctx = fz_new_context(NULL, NULL, FZ_STORE_DEFAULT);
    install_load_windows_font_funcs(ctx);
    int failed = 0;
    const fz_matrix identity = {1, 0, 0, 1, 0, 0};
    fz_try(ctx) {
        const char* languages[] = {NULL, "ja", "zh-CN", "zh-TW", "ko"};
        const char* chars[] = {"这", "显", "义", "组", "处", "复"};
        pdf_document* doc = pdf_create_document(ctx);
        fz_rect bounds = {0, 0, 650, 400};
        pdf_obj* obj = pdf_add_page(ctx, doc, bounds, 0, NULL, NULL);
        pdf_insert_page(ctx, doc, -1, obj);
        pdf_drop_obj(ctx, obj);
        pdf_page* page = pdf_load_page(ctx, doc, 0);
        float black[3] = {0, 0, 0};
        for (int row = 0; row < 5; row++) {
            for (int col = 0; col < 6; col++) {
                pdf_annot* annot = pdf_create_annot(ctx, page, PDF_ANNOT_FREE_TEXT);
                fz_rect rect = {float(col * 85 + 5), float(row * 65 + 5), float(col * 85 + 83), float(row * 65 + 63)};
                pdf_set_annot_rect(ctx, annot, rect);
                pdf_set_annot_default_appearance(ctx, annot, "Helv", 20, 3, black);
                if (languages[row])
                    pdf_set_annot_language(ctx, annot,
                                           (row == 1   ? FZ_LANG_ja
                                            : row == 2 ? FZ_LANG_zh_Hans
                                            : row == 3 ? FZ_LANG_zh_Hant
                                                       : FZ_LANG_ko));
                char text[64];
                // A Han character followed by a line break used to select the
                // HTML path, which wrote unsupported characters as Japan1.
                snprintf(text, sizeof(text), "%s\n%s", chars[col], chars[col]);
                pdf_set_annot_contents(ctx, annot, text);
                pdf_update_annot(ctx, annot);
                pdf_obj* ap = pdf_dict_get(ctx, pdf_annot_obj(ctx, annot), PDF_NAME(AP));
                pdf_obj* normal = pdf_dict_get(ctx, ap, PDF_NAME(N));
                pdf_obj* resources = pdf_dict_get(ctx, normal, PDF_NAME(Resources));
                pdf_obj* fonts = pdf_dict_get(ctx, resources, PDF_NAME(Font));
                // HTML uses F0 resources. These characters plus a line break
                // are fully supported by the simple collection fallback.
                if (pdf_dict_gets(ctx, fonts, "F0")) {
                    fprintf(stderr, "Unexpected HTML fallback: language=%s glyph=%d\n",
                            languages[row] ? languages[row] : "unset", col);
                    failed++;
                }
                pdf_drop_annot(ctx, annot);
            }
        }
        pdf_annot* sample = pdf_create_annot(ctx, page, PDF_ANNOT_FREE_TEXT);
        fz_rect sampleRect = {5, 335, 645, 395};
        pdf_set_annot_rect(ctx, sample, sampleRect);
        pdf_set_annot_default_appearance(ctx, sample, "Helv", 20, 3, black);
        pdf_set_annot_language(ctx, sample, FZ_LANG_ja);
        pdf_set_annot_contents(ctx, sample, "这是自由文本，有些字不会显示");
        pdf_update_annot(ctx, sample);
        pdf_drop_annot(ctx, sample);
        pdf_write_options opts = {0};
        pdf_save_document(ctx, doc, "out/cjk-regression.pdf", &opts);
        fz_pixmap* pix = fz_new_pixmap(ctx, fz_device_rgb(ctx), 650, 400, NULL, 0);
        fz_clear_pixmap_with_value(ctx, pix, 255);
        fz_device* dev = fz_new_draw_device(ctx, identity, pix);
        pdf_run_page(ctx, page, dev, identity, NULL);
        fz_close_device(ctx, dev);
        fz_drop_device(ctx, dev);
        unsigned char* samples = fz_pixmap_samples(ctx, pix);
        for (int row = 0; row < 5; row++) {
            for (int col = 0; col < 6; col++) {
                int ink = 0;
                // Exclude the annotation border; only glyph pixels count.
                for (int y = row * 65 + 14; y < row * 65 + 48; y++)
                    for (int x = col * 85 + 14; x < col * 85 + 65; x++)
                        if (samples[(y * 650 + x) * 3] < 100) ink++;
                printf("language=%s glyph=%d ink=%d\n", languages[row] ? languages[row] : "unset", col, ink);
                if (ink < 15) failed++;
            }
        }
        FILE* image = fopen("out/cjk-regression.ppm", "wb");
        fprintf(image, "P6\n650 400\n255\n");
        fwrite(samples, 1, 650 * 400 * 3, image);
        fclose(image);
        fz_drop_pixmap(ctx, pix);
        fz_drop_page(ctx, (fz_page*)page);
        pdf_drop_document(ctx, doc);
        if (argc == 3) {
            doc = pdf_open_document(ctx, argv[1]);
            page = pdf_load_page(ctx, doc, 0);
            for (pdf_annot* annot = pdf_first_annot(ctx, page); annot; annot = pdf_next_annot(ctx, annot)) {
                if (pdf_annot_type(ctx, annot) != PDF_ANNOT_FREE_TEXT) continue;
                pdf_dirty_annot(ctx, annot);
                pdf_update_annot(ctx, annot);
            }
            pdf_save_document(ctx, doc, argv[2], &opts);
            fz_pixmap* samplePix = fz_new_pixmap_from_page(ctx, (fz_page*)page, identity, fz_device_rgb(ctx), 0);
            fz_save_pixmap_as_png(ctx, samplePix, "out/issue-111/regenerated.png");
            fz_drop_pixmap(ctx, samplePix);
            fz_drop_page(ctx, (fz_page*)page);
            pdf_drop_document(ctx, doc);
        }
    }
    fz_catch(ctx) {
        fprintf(stderr, "%s\n", fz_caught_message(ctx));
        failed++;
    }
    fz_drop_context(ctx);
    return failed ? 1 : 0;
}
