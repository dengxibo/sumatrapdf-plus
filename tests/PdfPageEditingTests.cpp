#include <stdio.h>
#include <assert.h>
extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}
#include "../src/PdfPageEditing.h"
#include "../src/PdfPageSelection.h"

int main() {
    for (int count = 1; count <= 8; count++) {
        for (int mask = 1; mask < (1 << count); mask++) {
            int selected[8], selectedCount = 0;
            // Deliberately reverse the user's click order.
            for (int p = count; p >= 1; p--)
                if (mask & (1 << (p - 1))) selected[selectedCount++] = p;
            for (int before = 1; before <= count + 1; before++) {
                int output[8]{};
                int first = PdfMoveSelectedPages(count, selected, selectedCount, before, output);
                assert(first >= 1 && first <= count);
                int moved = 0, lastMoved = 0, lastKept = 0, seen = 0;
                for (int i = 0; i < count; i++) {
                    int page = output[i];
                    assert(page >= 1 && page <= count && !(seen & (1 << (page - 1))));
                    seen |= 1 << (page - 1);
                    if (mask & (1 << (page - 1))) {
                        assert(page > lastMoved);
                        assert(i == first - 1 + moved++);
                        lastMoved = page;
                    } else {
                        assert(page > lastKept);
                        lastKept = page;
                    }
                }
                assert(moved == selectedCount && seen == (1 << count) - 1);
            }
        }
    }
    fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_UNLIMITED);
    pdf_document* doc = pdf_create_document(ctx);
    pdf_document* copy = pdf_create_document(ctx);
    pdf_graft_map* map = pdf_new_graft_map(ctx, copy);
    pdf_obj* original[3]{};
    int failed = 0;
    fz_var(failed);
    fz_try(ctx) {
        for (int i = 0; i < 3; i++) {
            fz_buffer* contents = fz_new_buffer_from_copied_data(ctx, (const unsigned char*)"q Q\n", 4);
            pdf_obj* resources = pdf_new_dict(ctx, doc, 0);
            original[i] = pdf_add_page(ctx, doc, fz_make_rect(0, 0, 100 + i * 20, 200), 0, resources, contents);
            pdf_insert_page(ctx, doc, -1, original[i]);
            pdf_drop_obj(ctx, resources);
            fz_drop_buffer(ctx, contents);
        }
        pdf_obj* annots = pdf_dict_put_array(ctx, original[0], PDF_NAME(Annots), 1);
        pdf_obj* annot = pdf_new_dict(ctx, doc, 5);
        pdf_dict_put(ctx, annot, PDF_NAME(Subtype), PDF_NAME(Text));
        pdf_dict_put_rect(ctx, annot, PDF_NAME(Rect), fz_make_rect(5, 5, 25, 25));
        pdf_dict_put_text_string(ctx, annot, PDF_NAME(Contents), "keep this note");
        pdf_dict_put(ctx, annot, PDF_NAME(P), original[0]);
        pdf_array_push_drop(ctx, annots, pdf_add_object_drop(ctx, doc, annot));
        // Exercise inherited attributes, not only flat synthetic pages.
        pdf_obj* parent = pdf_dict_get(ctx, original[0], PDF_NAME(Parent));
        pdf_dict_put_rect(ctx, parent, PDF_NAME(MediaBox), fz_make_rect(0, 0, 100, 200));
        pdf_dict_del(ctx, original[0], PDF_NAME(MediaBox));
        pdf_obj* moved[] = {original[2], original[0], original[1]};
        PdfRestorePageOrder(ctx, doc, moved, 3);
        assert(pdf_to_num(ctx, pdf_lookup_page_obj(ctx, doc, 0)) == pdf_to_num(ctx, original[2]));
        fz_rect bounds;
        fz_matrix matrix;
        pdf_page_obj_transform(ctx, original[0], &bounds, &matrix);
        assert(bounds.x1 - bounds.x0 == 100);
        pdf_obj* kept[] = {original[0], original[2]};
        PdfRestorePageOrder(ctx, doc, kept, 2);
        assert(pdf_count_pages(ctx, doc) == 2);
        PdfRestorePageOrder(ctx, doc, original, 3);
        assert(pdf_count_pages(ctx, doc) == 3);
        for (int i = 0; i < 3; i++)
            assert(pdf_to_num(ctx, pdf_lookup_page_obj(ctx, doc, i)) == pdf_to_num(ctx, original[i]));
        pdf_obj* originalContents = pdf_dict_get(ctx, original[0], PDF_NAME(Contents));
        pdf_obj* contentsArray = pdf_new_array(ctx, doc, 1);
        pdf_array_push(ctx, contentsArray, originalContents);
        pdf_dict_put_drop(ctx, original[0], PDF_NAME(Contents), pdf_add_object_drop(ctx, doc, contentsArray));
        PdfGraftPageWithAnnotations(ctx, copy, map, doc, 0, -1);
        PdfGraftPageWithAnnotations(ctx, copy, map, doc, 0, -1);
        assert(pdf_count_pages(ctx, copy) == 2);
        pdf_obj* page0 = pdf_lookup_page_obj(ctx, copy, 0);
        pdf_obj* page1 = pdf_lookup_page_obj(ctx, copy, 1);
        pdf_obj* contents0 = pdf_dict_get(ctx, page0, PDF_NAME(Contents));
        pdf_obj* contents1 = pdf_dict_get(ctx, page1, PDF_NAME(Contents));
        assert(pdf_resolve_indirect(ctx, contents0) != pdf_resolve_indirect(ctx, contents1));
        pdf_array_push(ctx, contents1, pdf_array_get(ctx, contents1, 0));
        assert(pdf_array_len(ctx, contents0) == 1 && pdf_array_len(ctx, contents1) == 2);
        assert(pdf_array_len(ctx, pdf_dict_get(ctx, original[0], PDF_NAME(Contents))) == 1);
        pdf_obj* a0 = pdf_array_get(ctx, pdf_dict_get(ctx, page0, PDF_NAME(Annots)), 0);
        pdf_obj* a1 = pdf_array_get(ctx, pdf_dict_get(ctx, page1, PDF_NAME(Annots)), 0);
        assert(pdf_objcmp(ctx, a0, a1) != 0);
        assert(!strcmp(pdf_dict_get_text_string(ctx, a0, PDF_NAME(Contents)), "keep this note"));
        assert(pdf_to_num(ctx, pdf_dict_get(ctx, a0, PDF_NAME(P))) == pdf_to_num(ctx, page0));
        pdf_dict_put_text_string(ctx, a1, PDF_NAME(Contents), "independent copy");
        assert(!strcmp(pdf_dict_get_text_string(ctx, a0, PDF_NAME(Contents)), "keep this note"));
        const char* oldLabels[] = {"i", "A-1", "A-2"};
        const int labelOrder[] = {3, 1, 2};
        PdfWriteRemappedPageLabels(ctx, doc, oldLabels, labelOrder, 3, 1, 1);
        pdf_obj* labelNums = pdf_dict_getp(ctx, pdf_trailer(ctx, doc), "Root/PageLabels/Nums");
        assert(pdf_array_len(ctx, labelNums) == 8);
        const char* expectedLabels[] = {"A-2", "2", "i", "A-1"};
        for (int i = 0; i < 4; i++) {
            assert(pdf_to_int(ctx, pdf_array_get(ctx, labelNums, 2 * i)) == i);
            assert(!strcmp(pdf_dict_get_text_string(ctx, pdf_array_get(ctx, labelNums, 2 * i + 1), PDF_NAME(P)),
                           expectedLabels[i]));
        }
        fz_buffer* buffer = fz_new_buffer(ctx, 1024);
        fz_output* output = fz_new_output_with_buffer(ctx, buffer);
        pdf_write_options options = pdf_default_write_options;
        pdf_write_document(ctx, copy, output, &options);
        fz_close_output(ctx, output);
        unsigned char* bytes;
        size_t size = fz_buffer_storage(ctx, buffer, &bytes);
        fz_stream* stream = fz_open_memory(ctx, bytes, size);
        pdf_document* reopened = pdf_open_document_with_stream(ctx, stream);
        assert(pdf_count_pages(ctx, reopened) == 2);
        assert(pdf_array_len(ctx, pdf_dict_get(ctx, pdf_lookup_page_obj(ctx, reopened, 0), PDF_NAME(Annots))) == 1);
        pdf_drop_document(ctx, reopened);
        fz_drop_stream(ctx, stream);
        fz_drop_output(ctx, output);
        fz_drop_buffer(ctx, buffer);
        fz_pixmap* pixels = fz_new_pixmap(ctx, fz_device_rgb(ctx), 12, 8, nullptr, 0);
        fz_clear_pixmap_with_value(ctx, pixels, 200);
        fz_image* image = fz_new_image_from_pixmap(ctx, pixels, nullptr);
        int prior = pdf_count_pages(ctx, copy);
        PdfInsertImagePage(ctx, copy, image);
        assert(pdf_count_pages(ctx, copy) == prior + 1);
        pdf_page* imagePage = pdf_load_page(ctx, copy, prior);
        fz_pixmap* raster = fz_new_pixmap_from_page(ctx, (fz_page*)imagePage, fz_scale(4, 4), fz_device_rgb(ctx), 0);
        assert(raster->w > 0 && raster->h > 0);
        assert(raster->samples[0] >= 198 && raster->samples[0] <= 202);
        fz_drop_pixmap(ctx, raster);
        pdf_drop_page(ctx, imagePage);
        fz_drop_image(ctx, image);
        fz_drop_pixmap(ctx, pixels);
        puts(
            "All selection/drop boundaries, page deletion/undo, inherited geometry, independent annotations, "
            "save/reopen and image insertion/render passed.");
    }
    fz_catch(ctx) {
        fprintf(stderr, "%s\n", fz_caught_message(ctx));
        failed = 1;
    }
    for (auto* page : original) pdf_drop_obj(ctx, page);
    pdf_drop_graft_map(ctx, map);
    pdf_drop_document(ctx, copy);
    pdf_drop_document(ctx, doc);
    fz_drop_context(ctx);
    return failed;
}
