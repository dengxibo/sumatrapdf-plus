// Copyright 2026 Authors of SumatraPDF
// License: GPLv3

#include "utils/BaseUtil.h"
#include "utils/FileUtil.h"
#include "utils/WinUtil.h"
#include "utils/Log.h"
#include "utils/ZipUtil.h"
#include "WordToc.h"

extern "C" {
#include <mupdf/fitz.h>
}

#include <ole2.h>

#ifndef wdFormatDocument
#define wdFormatDocument 0
#endif

static constexpr const char* kPropPrefix = "SumatraPDF.Toc";
static constexpr int kChunkChars = 200;

void WordTocModelFree(WordTocModel* m) {
    if (!m) {
        return;
    }
    DeletePdfTocEditNodes(m->roots);
    delete m;
}

char* WordTocFormatUri(int pageNo, float x, float y) {
    if (pageNo < 1) {
        pageNo = 1;
    }
    int xi = (int)(x >= 0 ? x * 100.f + 0.5f : x * 100.f - 0.5f);
    int yi = (int)(y >= 0 ? y * 100.f + 0.5f : y * 100.f - 0.5f);
    return str::Format("sumatra-word-toc?p=%d&x=%d&y=%d", pageNo, xi, yi);
}

static int ParseIntAt(const char* s, const char** endOut) {
    int sign = 1;
    if (*s == '-') {
        sign = -1;
        s++;
    }
    int v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
    }
    if (endOut) {
        *endOut = s;
    }
    return sign * v;
}

bool WordTocParseUri(const char* uri, int* pageNo, float* x, float* y) {
    if (pageNo) {
        *pageNo = 0;
    }
    if (x) {
        *x = 0;
    }
    if (y) {
        *y = 0;
    }
    if (!uri || !str::StartsWith(uri, "sumatra-word-toc?")) {
        return false;
    }
    const char* p = str::Find(uri, "p=");
    const char* xs = str::Find(uri, "x=");
    const char* ys = str::Find(uri, "y=");
    if (!p || !xs || !ys) {
        return false;
    }
    int page = ParseIntAt(p + 2, nullptr);
    int xi = ParseIntAt(xs + 2, nullptr);
    int yi = ParseIntAt(ys + 2, nullptr);
    if (page < 1) {
        return false;
    }
    if (pageNo) {
        *pageNo = page;
    }
    if (x) {
        *x = xi / 100.f;
    }
    if (y) {
        *y = yi / 100.f;
    }
    return true;
}

static void SetErr(char** errOut, const char* msg) {
    if (!errOut) {
        return;
    }
    str::Free(*errOut);
    *errOut = str::Dup(msg ? msg : "Could not write the Word table of contents.");
}

static char* SanitizeTitle(const char* title) {
    if (!title) {
        return str::Dup("");
    }
    char* s = str::Dup(title);
    for (char* p = s; *p; p++) {
        if (*p == '\n' || *p == '\r' || *p == '\x1f') {
            *p = ' ';
        }
    }
    return s;
}

static void AppendNodes(StrBuilder& b, const Vec<PdfTocEditNode*>& nodes, int level) {
    for (PdfTocEditNode* n : nodes) {
        if (!n) {
            continue;
        }
        int page = 1;
        float x = 0;
        float y = 0;
        if (!WordTocParseUri(n->uri, &page, &x, &y) || page < 1) {
            page = 1;
        }
        int xi = (int)(x >= 0 ? x * 100.f + 0.5f : x * 100.f - 0.5f);
        int yi = (int)(y >= 0 ? y * 100.f + 0.5f : y * 100.f - 0.5f);
        AutoFreeStr title(SanitizeTitle(n->title));
        b.AppendFmt("%d\x1f%d\x1f%s\x1f%d\x1f%d\x1f%d\n", level, n->isOpen ? 1 : 0, title.Get() ? title.Get() : "",
                    page, xi, yi);
        AppendNodes(b, n->children, level + 1);
    }
}

static char* BuildPayload(WordTocModel* m) {
    StrBuilder b;
    b.Append("SMPT1\n");
    if (m) {
        AppendNodes(b, m->roots, 0);
    }
    return b.StealData();
}

static char* FieldDup(const char* start, const char* end) {
    if (!start) {
        return str::Dup("");
    }
    if (!end || end < start) {
        end = start + str::Len(start);
    }
    size_t n = (size_t)(end - start);
    char* s = AllocArray<char>(n + 1);
    if (n) {
        memcpy(s, start, n);
    }
    s[n] = 0;
    return s;
}

static bool ParsePayload(const char* s, WordTocModel* m) {
    if (!m || !s || !str::StartsWith(s, "SMPT1\n")) {
        return false;
    }
    DeletePdfTocEditNodes(m->roots);
    s += 6;
    Vec<PdfTocEditNode*> stack;
    while (*s) {
        const char* lineEnd = str::FindChar(s, '\n');
        const char* next = lineEnd ? lineEnd + 1 : s + str::Len(s);
        if (lineEnd && lineEnd > s && lineEnd[-1] == '\r') {
            lineEnd--;
        }
        if (lineEnd == s || (!lineEnd && !*s)) {
            s = next;
            continue;
        }
        const char* end = lineEnd ? lineEnd : next;
        const char* f[6]{};
        const char* fe[6]{};
        const char* p = s;
        int nFields = 0;
        for (; nFields < 6; nFields++) {
            f[nFields] = p;
            const char* cut = p;
            while (cut < end && *cut != '\x1f') {
                cut++;
            }
            fe[nFields] = cut;
            if (cut >= end) {
                nFields++;
                break;
            }
            p = cut + 1;
        }
        if (nFields < 6) {
            s = next;
            continue;
        }
        AutoFreeStr levelStr(FieldDup(f[0], fe[0]));
        AutoFreeStr openStr(FieldDup(f[1], fe[1]));
        AutoFreeStr title(FieldDup(f[2], fe[2]));
        AutoFreeStr pageStr(FieldDup(f[3], fe[3]));
        AutoFreeStr xStr(FieldDup(f[4], fe[4]));
        AutoFreeStr yStr(FieldDup(f[5], fe[5]));
        int level = ParseIntAt(levelStr, nullptr);
        int open = ParseIntAt(openStr, nullptr);
        int page = ParseIntAt(pageStr, nullptr);
        int xi = ParseIntAt(xStr, nullptr);
        int yi = ParseIntAt(yStr, nullptr);
        if (page < 1) {
            page = 1;
        }
        if (level < 0) {
            level = 0;
        }
        auto* node = NewPdfTocEditNode(title, WordTocFormatUri(page, xi / 100.f, yi / 100.f));
        node->isOpen = open != 0;
        while (stack.Size() > level) {
            stack.RemoveAt(stack.Size() - 1);
        }
        if (level == 0 || stack.empty()) {
            m->roots.Append(node);
        } else {
            stack.Last()->children.Append(node);
        }
        stack.Append(node);
        s = next;
    }
    return true;
}

static void AppendXmlText(StrBuilder& b, const char* s) {
    if (!s) {
        return;
    }
    for (; *s; s++) {
        if (*s == '&') {
            b.Append("&amp;");
        } else if (*s == '<') {
            b.Append("&lt;");
        } else if (*s == '>') {
            b.Append("&gt;");
        } else {
            b.AppendChar(*s);
        }
    }
}

static char* XmlUnescapeDup(const char* start, const char* end) {
    StrBuilder b;
    for (const char* p = start; p < end; p++) {
        if (*p == '&') {
            if (str::StartsWith(p, "&amp;")) {
                b.AppendChar('&');
                p += 4;
            } else if (str::StartsWith(p, "&lt;")) {
                b.AppendChar('<');
                p += 3;
            } else if (str::StartsWith(p, "&gt;")) {
                b.AppendChar('>');
                p += 3;
            } else if (str::StartsWith(p, "&quot;")) {
                b.AppendChar('"');
                p += 5;
            } else if (str::StartsWith(p, "&apos;")) {
                b.AppendChar('\'');
                p += 5;
            } else {
                b.AppendChar('&');
            }
        } else {
            b.AppendChar(*p);
        }
    }
    return b.StealData();
}

static int Utf8Seq(const char* s) {
    unsigned char c = (unsigned char)*s;
    int n = 1;
    if (c < 0x80) {
        n = 1;
    } else if ((c & 0xE0) == 0xC0) {
        n = 2;
    } else if ((c & 0xF0) == 0xE0) {
        n = 3;
    } else if ((c & 0xF8) == 0xF0) {
        n = 4;
    }
    for (int i = 1; i < n; i++) {
        if (!s[i]) {
            return 1;
        }
    }
    return n;
}

struct TocChunk {
    int idx = 0;
    char* text = nullptr;
};

static void FreeChunks(Vec<TocChunk>& chunks) {
    for (TocChunk& c : chunks) {
        str::Free(c.text);
        c.text = nullptr;
    }
    chunks.Reset();
}

static void SplitPayload(const char* payload, Vec<char*>& chunks) {
    if (!payload) {
        return;
    }
    const char* p = payload;
    if (!*p) {
        chunks.Append(str::Dup(""));
        return;
    }
    while (*p) {
        const char* start = p;
        int chars = 0;
        while (*p && chars < kChunkChars) {
            p += Utf8Seq(p);
            chars++;
        }
        size_t n = (size_t)(p - start);
        char* chunk = AllocArray<char>(n + 1);
        if (n) {
            memcpy(chunk, start, n);
        }
        chunk[n] = 0;
        chunks.Append(chunk);
    }
}

static char* BuildCustomXml(const char* existing, const Vec<char*>& chunks) {
    int nextPid = 2;
    const char* close = existing ? str::Find(existing, "</Properties>") : nullptr;
    if (close) {
        StrBuilder kept;
        const char* p = existing;
        while (p < close) {
            const char* prop = str::Find(p, "<property");
            if (!prop || prop > close) {
                kept.Append(p, (size_t)(close - p));
                break;
            }
            kept.Append(p, (size_t)(prop - p));
            const char* propEnd = str::Find(prop, "</property>");
            if (!propEnd || propEnd > close) {
                kept.Append(prop, (size_t)(close - prop));
                break;
            }
            propEnd += str::Len("</property>");
            AutoFreeStr elem(FieldDup(prop, propEnd));
            const char* nameAt = str::Find(elem, "name=\"");
            bool drop = false;
            if (nameAt) {
                nameAt += 6;
                const char* nameEnd = str::FindChar(nameAt, '"');
                AutoFreeStr name(FieldDup(nameAt, nameEnd));
                drop = str::Eq(name, kPropPrefix) || str::StartsWith(name, "SumatraPDF.Toc.");
            }
            const char* pidAt = str::Find(elem, "pid=\"");
            if (pidAt) {
                int pid = ParseIntAt(pidAt + 5, nullptr);
                if (pid >= nextPid) {
                    nextPid = pid + 1;
                }
            }
            if (!drop) {
                kept.Append(elem);
            }
            p = propEnd;
        }
        StrBuilder out;
        out.Append(kept.Get(), kept.size());
        for (int i = 0; i < chunks.Size(); i++) {
            out.AppendFmt(
                "<property fmtid=\"{D5CDD505-2E9C-101B-9397-08002B2CF9AE}\" pid=\"%d\" "
                "name=\"SumatraPDF.Toc.%d\"><vt:lpwstr>",
                nextPid++, i);
            AppendXmlText(out, chunks[i]);
            out.Append("</vt:lpwstr></property>");
        }
        out.Append("</Properties>");
        return out.StealData();
    }

    StrBuilder out;
    out.Append(
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<Properties xmlns=\"http://schemas.openxmlformats.org/officeDocument/2006/custom-properties\" "
        "xmlns:vt=\"http://schemas.openxmlformats.org/officeDocument/2006/docPropsVTypes\">");
    for (int i = 0; i < chunks.Size(); i++) {
        out.AppendFmt(
            "<property fmtid=\"{D5CDD505-2E9C-101B-9397-08002B2CF9AE}\" pid=\"%d\" "
            "name=\"SumatraPDF.Toc.%d\"><vt:lpwstr>",
            nextPid++, i);
        AppendXmlText(out, chunks[i]);
        out.Append("</vt:lpwstr></property>");
    }
    out.Append("</Properties>");
    return out.StealData();
}

static bool NameIsTocProp(const char* name, int* idxOut) {
    if (!name) {
        return false;
    }
    if (str::Eq(name, kPropPrefix)) {
        if (idxOut) {
            *idxOut = 0;
        }
        return true;
    }
    if (!str::StartsWith(name, "SumatraPDF.Toc.")) {
        return false;
    }
    const char* num = name + str::Len("SumatraPDF.Toc.");
    if (*num < '0' || *num > '9') {
        return false;
    }
    const char* end = nullptr;
    int idx = ParseIntAt(num, &end);
    if (!end || *end) {
        return false;
    }
    if (idxOut) {
        *idxOut = idx;
    }
    return true;
}

static bool CollectPayload(const char* customXml, StrBuilder& payload) {
    if (!customXml) {
        return false;
    }
    Vec<TocChunk> chunks;
    const char* p = customXml;
    bool any = false;
    while ((p = str::Find(p, "name=\""))) {
        p += 6;
        const char* nameEnd = str::FindChar(p, '"');
        if (!nameEnd) {
            break;
        }
        AutoFreeStr name(FieldDup(p, nameEnd));
        int idx = 0;
        bool mine = NameIsTocProp(name, &idx);
        p = nameEnd + 1;
        if (!mine) {
            continue;
        }
        const char* tag = str::Find(p, "<vt:lpwstr");
        const char* nextProp = str::Find(p, "<property");
        if (!tag || (nextProp && tag > nextProp)) {
            continue;
        }
        const char* gt = str::FindChar(tag, '>');
        if (!gt) {
            break;
        }
        const char* valEnd = str::Find(gt + 1, "</vt:lpwstr>");
        if (!valEnd) {
            break;
        }
        TocChunk c;
        c.idx = idx;
        c.text = XmlUnescapeDup(gt + 1, valEnd);
        chunks.Append(c);
        any = true;
        p = valEnd;
    }
    if (!any) {
        FreeChunks(chunks);
        return false;
    }
    for (int i = 1; i < chunks.Size(); i++) {
        TocChunk key = chunks[i];
        int j = i;
        while (j > 0 && chunks[j - 1].idx > key.idx) {
            chunks[j] = chunks[j - 1];
            j--;
        }
        chunks[j] = key;
    }
    for (TocChunk& c : chunks) {
        if (c.text) {
            payload.Append(c.text);
        }
    }
    FreeChunks(chunks);
    return true;
}

static char* EnsureOverride(const char* xml, const char* marker, const char* insertBefore, const char* snippet) {
    if (!xml) {
        return nullptr;
    }
    if (str::FindI(xml, marker)) {
        return str::Dup(xml);
    }
    const char* close = str::Find(xml, insertBefore);
    if (!close) {
        return str::Dup(xml);
    }
    StrBuilder b;
    b.Append(xml, (size_t)(close - xml));
    b.Append(snippet);
    b.Append(close);
    return b.StealData();
}

static int MaxRelId(const char* xml) {
    int maxId = 0;
    const char* p = xml;
    while (p && (p = str::Find(p, "Id=\"rId"))) {
        p += 7;
        const char* end = nullptr;
        int id = ParseIntAt(p, &end);
        if (id > maxId) {
            maxId = id;
        }
        p = end;
    }
    return maxId;
}

static char* EnsureCustomRel(const char* xml) {
    if (!xml) {
        return nullptr;
    }
    if (str::FindI(xml, "custom-properties")) {
        return str::Dup(xml);
    }
    const char* close = str::Find(xml, "</Relationships>");
    if (!close) {
        return str::Dup(xml);
    }
    int id = MaxRelId(xml) + 1;
    StrBuilder b;
    b.Append(xml, (size_t)(close - xml));
    b.AppendFmt(
        "<Relationship Id=\"rId%d\" "
        "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/custom-properties\" "
        "Target=\"docProps/custom.xml\"/>",
        id);
    b.Append(close);
    return b.StealData();
}

struct ZipPart {
    char* name = nullptr;
    u8* data = nullptr;
    size_t size = 0;
};

static void FreeZipParts(Vec<ZipPart*>& parts) {
    for (ZipPart* p : parts) {
        if (!p) {
            continue;
        }
        str::Free(p->name);
        str::Free(p->data);
        delete p;
    }
    parts.Reset();
}

static char* NormZipName(const char* name) {
    char* n = str::Dup(name ? name : "");
    str::TransCharsInPlace(n, "\\", "/");
    while (n[0] == '/') {
        memmove(n, n + 1, str::Len(n));
    }
    return n;
}

static ZipPart* FindPartI(Vec<ZipPart*>& parts, const char* name) {
    for (ZipPart* p : parts) {
        if (p && p->name && str::EqI(p->name, name)) {
            return p;
        }
    }
    return nullptr;
}

static void ReplacePartText(ZipPart* part, char* text) {
    if (!part) {
        str::Free(text);
        return;
    }
    str::Free(part->data);
    part->data = (u8*)text;
    part->size = text ? str::Len(text) : 0;
}

static bool ReadZipParts(const char* path, Vec<ZipPart*>& parts, char** errOut) {
    fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    if (!ctx) {
        SetErr(errOut, "Could not read the Word package.");
        return false;
    }
    fz_archive* arch = nullptr;
    bool ok = false;
    fz_try(ctx) {
        // Prefer fz_open_archive: it is exported from libmupdf.dll (PdfFilter /
        // PdfPreview / SumatraPDF-dll). fz_open_zip_archive is not in the .def.
        arch = fz_open_archive(ctx, path);
        int n = fz_count_archive_entries(ctx, arch);
        for (int i = 0; i < n; i++) {
            const char* raw = fz_list_archive_entry(ctx, arch, i);
            if (!raw || !raw[0] || str::EndsWith(raw, "/")) {
                continue;
            }
            fz_buffer* buf = nullptr;
            fz_try(ctx) {
                buf = fz_read_archive_entry(ctx, arch, raw);
            }
            fz_catch(ctx) {
                fz_report_error(ctx);
                buf = nullptr;
            }
            if (!buf) {
                continue;
            }
            unsigned char* data = nullptr;
            size_t len = fz_buffer_storage(ctx, buf, &data);
            auto* part = new ZipPart();
            part->name = NormZipName(raw);
            part->data = AllocArray<u8>(len + 1);
            if (len && data) {
                memcpy(part->data, data, len);
            }
            part->data[len] = 0;
            part->size = len;
            fz_drop_buffer(ctx, buf);
            bool dup = FindPartI(parts, part->name) != nullptr;
            if (dup) {
                str::Free(part->name);
                str::Free(part->data);
                delete part;
            } else {
                parts.Append(part);
            }
        }
        ok = FindPartI(parts, "word/document.xml") != nullptr;
    }
    fz_always(ctx) {
        fz_drop_archive(ctx, arch);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        ok = false;
    }
    fz_drop_context(ctx);
    if (!ok) {
        SetErr(errOut, "Could not read the Word package.");
        FreeZipParts(parts);
    }
    return ok;
}

static bool WriteZipParts(const char* dst, Vec<ZipPart*>& parts, char** errOut) {
    ZipCreator zc(dst);
    bool ok = true;
    for (ZipPart* p : parts) {
        if (!p || !p->name) {
            continue;
        }
        const void* data = p->data ? (const void*)p->data : "";
        ok = ok && zc.AddFileData(p->name, data, p->size);
    }
    ok = ok && zc.Finish();
    if (!ok) {
        file::Delete(dst);
        SetErr(errOut, "Could not write the Word package.");
    }
    return ok;
}

bool WordTocLoadStored(const char* docxPath, WordTocModel* m) {
    if (!docxPath || !m || !file::Exists(docxPath)) {
        return false;
    }
    Vec<ZipPart*> parts;
    char* err = nullptr;
    if (!ReadZipParts(docxPath, parts, &err)) {
        str::Free(err);
        return false;
    }
    ZipPart* custom = FindPartI(parts, "docProps/custom.xml");
    bool ok = false;
    if (custom && custom->data) {
        StrBuilder payload;
        if (CollectPayload((const char*)custom->data, payload)) {
            ok = ParsePayload(payload.Get(), m);
        }
    }
    FreeZipParts(parts);
    return ok;
}

bool WordTocWriteDocx(const char* srcDocx, const char* dstDocx, WordTocModel* m, char** errOut) {
    if (errOut) {
        *errOut = nullptr;
    }
    if (!srcDocx || !dstDocx || !m || str::EqI(srcDocx, dstDocx)) {
        SetErr(errOut, "Could not write the Word package.");
        return false;
    }
    Vec<ZipPart*> parts;
    if (!ReadZipParts(srcDocx, parts, errOut)) {
        return false;
    }
    AutoFreeStr payload(BuildPayload(m));
    Vec<char*> chunks;
    SplitPayload(payload, chunks);
    defer {
        for (char* c : chunks) {
            str::Free(c);
        }
    };

    ZipPart* custom = FindPartI(parts, "docProps/custom.xml");
    const char* existing = (custom && custom->data) ? (const char*)custom->data : nullptr;
    char* customXml = BuildCustomXml(existing, chunks);
    if (custom) {
        ReplacePartText(custom, customXml);
    } else {
        auto* part = new ZipPart();
        part->name = str::Dup("docProps/custom.xml");
        part->data = (u8*)customXml;
        part->size = customXml ? str::Len(customXml) : 0;
        parts.Append(part);
        ZipPart* types = FindPartI(parts, "[Content_Types].xml");
        if (types && types->data) {
            char* neu =
                EnsureOverride((const char*)types->data, "custom-properties+xml", "</Types>",
                               "<Override PartName=\"/docProps/custom.xml\" "
                               "ContentType=\"application/vnd.openxmlformats-officedocument.custom-properties+xml\"/>");
            ReplacePartText(types, neu);
        }
        ZipPart* rels = FindPartI(parts, "_rels/.rels");
        if (rels && rels->data) {
            ReplacePartText(rels, EnsureCustomRel((const char*)rels->data));
        }
    }
    bool ok = WriteZipParts(dstDocx, parts, errOut);
    FreeZipParts(parts);
    return ok;
}

static bool InvokePutBool(IDispatch* obj, const WCHAR* name, bool v) {
    DISPID id = 0;
    LPOLESTR n = (LPOLESTR)name;
    if (FAILED(obj->GetIDsOfNames(IID_NULL, &n, 1, LOCALE_USER_DEFAULT, &id))) {
        return false;
    }
    VARIANT val;
    VariantInit(&val);
    val.vt = VT_BOOL;
    val.boolVal = v ? VARIANT_TRUE : VARIANT_FALSE;
    DISPPARAMS dp{};
    dp.cArgs = 1;
    dp.rgvarg = &val;
    DISPID named = DISPID_PROPERTYPUT;
    dp.cNamedArgs = 1;
    dp.rgdispidNamedArgs = &named;
    HRESULT hr = obj->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYPUT, &dp, nullptr, nullptr, nullptr);
    VariantClear(&val);
    return SUCCEEDED(hr);
}

static bool InvokePutI4(IDispatch* obj, const WCHAR* name, LONG v) {
    DISPID id = 0;
    LPOLESTR n = (LPOLESTR)name;
    if (FAILED(obj->GetIDsOfNames(IID_NULL, &n, 1, LOCALE_USER_DEFAULT, &id))) {
        return false;
    }
    VARIANT val;
    VariantInit(&val);
    val.vt = VT_I4;
    val.lVal = v;
    DISPPARAMS dp{};
    dp.cArgs = 1;
    dp.rgvarg = &val;
    DISPID named = DISPID_PROPERTYPUT;
    dp.cNamedArgs = 1;
    dp.rgdispidNamedArgs = &named;
    HRESULT hr = obj->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYPUT, &dp, nullptr, nullptr, nullptr);
    VariantClear(&val);
    return SUCCEEDED(hr);
}

static IDispatch* InvokeGetDispatch(IDispatch* obj, const WCHAR* name) {
    DISPID id = 0;
    LPOLESTR n = (LPOLESTR)name;
    if (FAILED(obj->GetIDsOfNames(IID_NULL, &n, 1, LOCALE_USER_DEFAULT, &id))) {
        return nullptr;
    }
    VARIANT resultV;
    VariantInit(&resultV);
    DISPPARAMS dp{};
    HRESULT hr = obj->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYGET, &dp, &resultV, nullptr, nullptr);
    IDispatch* out = nullptr;
    if (SUCCEEDED(hr) && resultV.vt == VT_DISPATCH && resultV.pdispVal) {
        out = resultV.pdispVal;
        resultV.pdispVal = nullptr;
    }
    VariantClear(&resultV);
    return out;
}

static bool WordSaveAsDoc(const char* srcDocx, const char* dstDoc, char** errOut) {
    bool comOwned = false;
    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(hrInit)) {
        comOwned = true;
    } else if (hrInit != RPC_E_CHANGED_MODE && hrInit != S_FALSE) {
        SetErr(errOut, "Word is not available to save a classic .doc file.");
        return false;
    }

    WCHAR srcW[MAX_PATH]{};
    WCHAR dstW[MAX_PATH]{};
    if (!MultiByteToWideChar(CP_UTF8, 0, srcDocx, -1, srcW, dimof(srcW)) ||
        !MultiByteToWideChar(CP_UTF8, 0, dstDoc, -1, dstW, dimof(dstW))) {
        SetErr(errOut, "Path is too long to save the .doc file.");
        if (comOwned) {
            CoUninitialize();
        }
        return false;
    }

    IDispatch* app = nullptr;
    IDispatch* docs = nullptr;
    IDispatch* doc = nullptr;
    bool ok = false;

    CLSID clsid{};
    HRESULT hr = CLSIDFromProgID(L"Word.Application", &clsid);
    if (FAILED(hr)) {
        SetErr(errOut, "Word is not installed; a classic .doc outline cannot be written back.");
        goto cleanup;
    }
    hr = CoCreateInstance(clsid, nullptr, CLSCTX_LOCAL_SERVER, IID_IDispatch, (void**)&app);
    if (FAILED(hr) || !app) {
        SetErr(errOut, "Word is not installed; a classic .doc outline cannot be written back.");
        goto cleanup;
    }
    InvokePutBool(app, L"Visible", false);
    InvokePutI4(app, L"DisplayAlerts", 0);
    docs = InvokeGetDispatch(app, L"Documents");
    if (!docs) {
        SetErr(errOut, "Word could not open the document.");
        goto cleanup;
    }
    {
        DISPID id = 0;
        LPOLESTR n = (LPOLESTR)L"Open";
        if (FAILED(docs->GetIDsOfNames(IID_NULL, &n, 1, LOCALE_USER_DEFAULT, &id))) {
            SetErr(errOut, "Word could not open the document.");
            goto cleanup;
        }
        VARIANT arg;
        VariantInit(&arg);
        arg.vt = VT_BSTR;
        arg.bstrVal = SysAllocString(srcW);
        DISPPARAMS dp{};
        dp.cArgs = 1;
        dp.rgvarg = &arg;
        VARIANT resultV;
        VariantInit(&resultV);
        hr = docs->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_METHOD, &dp, &resultV, nullptr, nullptr);
        if (arg.bstrVal) {
            SysFreeString(arg.bstrVal);
        }
        if (FAILED(hr) || resultV.vt != VT_DISPATCH || !resultV.pdispVal) {
            VariantClear(&resultV);
            SetErr(errOut, "Word could not open the document.");
            goto cleanup;
        }
        doc = resultV.pdispVal;
    }
    {
        DISPID id = 0;
        LPOLESTR n = (LPOLESTR)L"SaveAs2";
        if (FAILED(doc->GetIDsOfNames(IID_NULL, &n, 1, LOCALE_USER_DEFAULT, &id))) {
            n = (LPOLESTR)L"SaveAs";
            if (FAILED(doc->GetIDsOfNames(IID_NULL, &n, 1, LOCALE_USER_DEFAULT, &id))) {
                SetErr(errOut, "Word could not save the .doc file.");
                goto cleanup;
            }
        }
        VARIANT args[2];
        VariantInit(&args[0]);
        VariantInit(&args[1]);
        args[1].vt = VT_BSTR;
        args[1].bstrVal = SysAllocString(dstW);
        args[0].vt = VT_I4;
        args[0].lVal = wdFormatDocument;
        DISPPARAMS dp{};
        dp.cArgs = 2;
        dp.rgvarg = args;
        hr = doc->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_METHOD, &dp, nullptr, nullptr, nullptr);
        if (args[1].bstrVal) {
            SysFreeString(args[1].bstrVal);
        }
        VariantClear(&args[0]);
        if (FAILED(hr)) {
            SetErr(errOut, "Word could not save the .doc file.");
            goto cleanup;
        }
    }
    ok = file::Exists(dstDoc);

cleanup:
    if (doc) {
        DISPID id = 0;
        LPOLESTR n = (LPOLESTR)L"Close";
        if (SUCCEEDED(doc->GetIDsOfNames(IID_NULL, &n, 1, LOCALE_USER_DEFAULT, &id))) {
            VARIANT arg;
            VariantInit(&arg);
            arg.vt = VT_BOOL;
            arg.boolVal = VARIANT_FALSE;
            DISPPARAMS dp{};
            dp.cArgs = 1;
            dp.rgvarg = &arg;
            doc->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_METHOD, &dp, nullptr, nullptr, nullptr);
            VariantClear(&arg);
        }
        doc->Release();
    }
    if (docs) {
        docs->Release();
    }
    if (app) {
        DISPID id = 0;
        LPOLESTR n = (LPOLESTR)L"Quit";
        if (SUCCEEDED(app->GetIDsOfNames(IID_NULL, &n, 1, LOCALE_USER_DEFAULT, &id))) {
            DISPPARAMS dp{};
            app->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_METHOD, &dp, nullptr, nullptr, nullptr);
        }
        app->Release();
    }
    if (comOwned) {
        CoUninitialize();
    }
    if (!ok && errOut && !*errOut) {
        SetErr(errOut, "Word could not save the .doc file.");
    }
    return ok;
}

bool WordTocWriteClassicDoc(const char* srcDocx, const char* dstDoc, WordTocModel* m, char** errOut) {
    if (errOut) {
        *errOut = nullptr;
    }
    if (!srcDocx || !dstDoc || !m) {
        SetErr(errOut, "Could not write the Word table of contents.");
        return false;
    }
    TempStr tmpDir = GetTempDirTemp();
    if (!tmpDir) {
        SetErr(errOut, "Could not write the Word table of contents.");
        return false;
    }
    TempStr tmpDocx = path::JoinTemp(tmpDir, str::FormatTemp("smp-toc-%u.docx", (unsigned)GetTickCount()));
    char* ownedTmp = str::Dup(tmpDocx);
    defer {
        file::Delete(ownedTmp);
        str::Free(ownedTmp);
    };
    if (!WordTocWriteDocx(srcDocx, ownedTmp, m, errOut)) {
        return false;
    }
    return WordSaveAsDoc(ownedTmp, dstDoc, errOut);
}
