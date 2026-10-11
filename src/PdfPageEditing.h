/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */
#pragma once

// Caller owns page references and serializes document access.
inline void PdfRestorePageOrder(fz_context* ctx, pdf_document* doc, pdf_obj* const* pages, int count) {
    if (count < 1) fz_throw(ctx, FZ_ERROR_ARGUMENT, "Keep at least one page");
    // Inherited resources/boxes must become local before changing /Parent.
    for (int i = 0; i < count; i++) pdf_flatten_inheritable_page_items(ctx, pages[i]);
    pdf_delete_page_range(ctx, doc, 0, pdf_count_pages(ctx, doc));
    for (int i = 0; i < count; i++) pdf_insert_page(ctx, doc, -1, pages[i]);
}

inline void PdfGraftPageWithAnnotations(fz_context* ctx, pdf_document* dst, pdf_graft_map* map, pdf_document* src,
                                        int from, int at) {
    pdf_graft_mapped_page(ctx, map, at, src, from);
    if (at < 0) at = pdf_count_pages(ctx, dst) - 1;
    pdf_obj* sourcePage = pdf_lookup_page_obj(ctx, src, from);
    pdf_obj* targetPage = pdf_lookup_page_obj(ctx, dst, at);
    // Streams/resources may be shared, but each page needs its own mutable
    // Contents array so later crop/deskew wrappers affect only that copy.
    pdf_obj* contents = pdf_dict_get(ctx, targetPage, PDF_NAME(Contents));
    if (pdf_is_array(ctx, contents))
        pdf_dict_put_drop(ctx, targetPage, PDF_NAME(Contents), pdf_copy_array(ctx, contents));
    pdf_obj* sourceAnnots = pdf_dict_get(ctx, sourcePage, PDF_NAME(Annots));
    pdf_obj* targetAnnots = pdf_dict_put_array(ctx, targetPage, PDF_NAME(Annots), pdf_array_len(ctx, sourceAnnots));
    for (int i = 0; i < pdf_array_len(ctx, sourceAnnots); i++) {
        pdf_obj* source = pdf_array_get(ctx, sourceAnnots, i);
        if (pdf_name_eq(ctx, pdf_dict_get(ctx, source, PDF_NAME(Subtype)), PDF_NAME(Popup))) continue;
        pdf_obj* copy = pdf_copy_dict(ctx, source);
        pdf_obj* grafted = nullptr;
        fz_var(grafted);
        fz_try(ctx) {
            // Back references would otherwise graft the original page tree.
            pdf_dict_del(ctx, copy, PDF_NAME(P));
            pdf_dict_del(ctx, copy, PDF_NAME(Parent));
            pdf_dict_del(ctx, copy, PDF_NAME(Popup));
            pdf_dict_del(ctx, copy, PDF_NAME(IRT));
            pdf_dict_del(ctx, copy, PDF_NAME(Dest));
            pdf_obj* action = pdf_dict_get(ctx, copy, PDF_NAME(A));
            if (!pdf_name_eq(ctx, pdf_dict_get(ctx, action, PDF_NAME(S)), PDF_NAME(URI))) {
                pdf_dict_del(ctx, copy, PDF_NAME(A));
            }
            grafted = pdf_graft_mapped_object(ctx, map, copy);
            pdf_obj* ref = pdf_add_object(ctx, dst, grafted);
            pdf_dict_put(ctx, grafted, PDF_NAME(P), targetPage);
            pdf_array_push_drop(ctx, targetAnnots, ref);
        }
        fz_always(ctx) {
            pdf_drop_obj(ctx, grafted);
            pdf_drop_obj(ctx, copy);
        }
        fz_catch(ctx) {
            fz_rethrow(ctx);
        }
    }
}

inline void PdfInsertImagePage(fz_context* ctx, pdf_document* doc, fz_image* image) {
    int xres, yres;
    fz_image_resolution(image, &xres, &yres);
    float width = image->w * 72.f / (xres > 0 ? xres : 96);
    float height = image->h * 72.f / (yres > 0 ? yres : 96);
    pdf_obj* resources = pdf_new_dict(ctx, doc, 1);
    fz_buffer* contents = nullptr;
    pdf_obj* page = nullptr;
    fz_var(contents);
    fz_var(page);
    fz_try(ctx) {
        pdf_obj* images = pdf_dict_put_dict(ctx, resources, PDF_NAME(XObject), 1);
        pdf_dict_puts_drop(ctx, images, "Im0", pdf_add_image(ctx, doc, image));
        contents = fz_new_buffer(ctx, 100);
        fz_append_printf(ctx, contents, "q %g 0 0 %g 0 0 cm /Im0 Do Q\n", width, height);
        page = pdf_add_page(ctx, doc, fz_make_rect(0, 0, width, height), 0, resources, contents);
        pdf_insert_page(ctx, doc, -1, page);
    }
    fz_always(ctx) {
        pdf_drop_obj(ctx, page);
        fz_drop_buffer(ctx, contents);
        pdf_drop_obj(ctx, resources);
    }
    fz_catch(ctx) {
        fz_rethrow(ctx);
    }
}

inline void PdfWriteRemappedPageLabels(fz_context* ctx, pdf_document* doc, const char* const* oldLabels,
                                       const int* order, int count, int inserted, int at) {
    pdf_obj* labels = pdf_new_dict(ctx, doc, 1);
    fz_try(ctx) {
        pdf_obj* nums = pdf_dict_put_array(ctx, labels, PDF_NAME(Nums), (count + inserted) * 2);
        int sourceIndex = 0;
        for (int i = 0; i < count + inserted; i++) {
            char number[32];
            fz_snprintf(number, sizeof(number), "%d", i + 1);
            const char* label = i >= at && i < at + inserted ? number : oldLabels[order[sourceIndex++] - 1];
            pdf_obj* entry = pdf_new_dict(ctx, doc, 1);
            pdf_dict_put_text_string(ctx, entry, PDF_NAME(P), label ? label : "");
            pdf_array_push_drop(ctx, nums, pdf_new_int(ctx, i));
            pdf_array_push_drop(ctx, nums, entry);
        }
        pdf_obj* root = pdf_dict_get(ctx, pdf_trailer(ctx, doc), PDF_NAME(Root));
        pdf_dict_put(ctx, root, PDF_NAME(PageLabels), labels);
    }
    fz_always(ctx) {
        pdf_drop_obj(ctx, labels);
    }
    fz_catch(ctx) {
        fz_rethrow(ctx);
    }
}
