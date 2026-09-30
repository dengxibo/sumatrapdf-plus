/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/HtmlParserLookup.h"
#include "utils/HtmlPullParser.h"

#include "EpubMediaOverlay.h"

// --- clock values (SMIL 3.0 / EPUB 3.3 H.4) ---

static bool ParseDigits(const char*& s, const char* end, i64* valOut, int* nDigitsOut) {
    i64 v = 0;
    int n = 0;
    while (s < end && *s >= '0' && *s <= '9') {
        if (v > (i64)1e15) {
            return false;
        }
        v = v * 10 + (*s - '0');
        s++;
        n++;
    }
    *valOut = v;
    *nDigitsOut = n;
    return n > 0;
}

// ".fraction" scaled to microseconds (extra digits are truncated)
static bool ParseFractionUs(const char*& s, const char* end, i64 unitUs, i64* usOut) {
    *usOut = 0;
    if (s >= end || *s != '.') {
        return true;
    }
    s++;
    if (s >= end || *s < '0' || *s > '9') {
        return false;
    }
    // accumulate as a fraction of unitUs with up to 12 significant digits
    double frac = 0;
    double scale = 0.1;
    int n = 0;
    while (s < end && *s >= '0' && *s <= '9') {
        if (n < 12) {
            frac += (*s - '0') * scale;
            scale /= 10;
        }
        n++;
        s++;
    }
    *usOut = (i64)(frac * (double)unitUs + 0.5);
    return true;
}

bool EpubParseClockValueN(const char* s, size_t n, i64* usOut) {
    if (!s || !usOut) {
        return false;
    }
    const char* end = s + n;
    while (s < end && str::IsWs(*s)) {
        s++;
    }
    while (end > s && str::IsWs(end[-1])) {
        end--;
    }
    if (s >= end) {
        return false;
    }

    constexpr i64 kSec = 1000000;
    constexpr i64 kMin = 60 * kSec;
    constexpr i64 kHour = 60 * kMin;

    int colons = 0;
    for (const char* p = s; p < end; p++) {
        if (*p == ':') {
            colons++;
        }
    }

    if (colons > 0) {
        // Full-clock-val: Hours ":" Minutes ":" Seconds ("." Fraction)?
        // Partial-clock-val: Minutes ":" Seconds ("." Fraction)?
        if (colons > 2) {
            return false;
        }
        i64 parts[3] = {};
        int nParts = colons + 1;
        const char* p = s;
        for (int i = 0; i < nParts; i++) {
            int nd = 0;
            if (!ParseDigits(p, end, &parts[i], &nd)) {
                return false;
            }
            bool isHours = (nParts == 3 && i == 0);
            if (!isHours && nd != 2) {
                return false;
            }
            if (i < nParts - 1) {
                if (p >= end || *p != ':') {
                    return false;
                }
                p++;
            }
        }
        i64 fracUs = 0;
        if (!ParseFractionUs(p, end, kSec, &fracUs) || p != end) {
            return false;
        }
        i64 hours = nParts == 3 ? parts[0] : 0;
        i64 minutes = parts[nParts - 2];
        i64 seconds = parts[nParts - 1];
        if (minutes > 59 || seconds > 59) {
            return false;
        }
        if (hours > 1000000) {
            return false;
        }
        *usOut = hours * kHour + minutes * kMin + seconds * kSec + fracUs;
        return true;
    }

    // Timecount-val: Timecount ("." Fraction)? (Metric)?
    const char* p = s;
    i64 whole = 0;
    int nd = 0;
    if (!ParseDigits(p, end, &whole, &nd)) {
        return false;
    }
    const char* fracStart = p;
    size_t metricLen = 0;
    const char* metric = nullptr;
    {
        const char* q = p;
        if (q < end && *q == '.') {
            q++;
            while (q < end && *q >= '0' && *q <= '9') {
                q++;
            }
        }
        metric = q;
        metricLen = (size_t)(end - q);
    }
    i64 unit = kSec;
    if (metricLen == 0 || str::EqNIx(metric, metricLen, "s")) {
        unit = kSec;
    } else if (str::EqNIx(metric, metricLen, "h")) {
        unit = kHour;
    } else if (str::EqNIx(metric, metricLen, "min")) {
        unit = kMin;
    } else if (str::EqNIx(metric, metricLen, "ms")) {
        unit = 1000;
    } else {
        return false;
    }
    // metric names are case-sensitive in SMIL
    if (metricLen > 0 && (metric[0] < 'a' || metric[0] > 'z')) {
        return false;
    }
    p = fracStart;
    i64 fracUs = 0;
    if (!ParseFractionUs(p, metric, unit, &fracUs) || p != metric) {
        return false;
    }
    if (whole > (i64)9e18 / unit) {
        return false;
    }
    *usOut = whole * unit + fracUs;
    return true;
}

bool EpubParseClockValue(const char* s, i64* usOut) {
    if (!s) {
        return false;
    }
    return EpubParseClockValueN(s, str::Len(s), usOut);
}

// --- paths ---

static char* DupIn(Arena* arena, const char* s, size_t len = (size_t)-1) {
    if (!s) {
        return nullptr;
    }
    if (arena) {
        return str::Dup(arena, s, len);
    }
    return str::Dup(s, len);
}

static bool HrefHasScheme(const char* s) {
    // RFC 3986 scheme: ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ) ":"
    const char* p = s;
    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z'))) {
        return false;
    }
    p++;
    while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '+' || *p == '-' ||
           *p == '.') {
        p++;
    }
    // "C:" (one letter) is a drive letter, also rejected by the caller
    return *p == ':';
}

TempStr EpubDirOfTemp(const char* path) {
    if (!path) {
        return str::DupTemp("");
    }
    const char* slash = str::FindCharLast(path, '/');
    if (!slash) {
        return str::DupTemp("");
    }
    return str::DupTemp(path, (size_t)(slash - path));
}

char* EpubResolvePath(const char* baseDir, const char* href, char** fragmentOut, Arena* arena) {
    if (fragmentOut) {
        *fragmentOut = nullptr;
    }
    if (!href) {
        return nullptr;
    }
    while (str::IsWs(*href)) {
        href++;
    }
    if (HrefHasScheme(href)) {
        return nullptr;
    }
    if (href[0] == '\\' || str::FindChar(href, '\\')) {
        return nullptr;
    }
    if (href[0] == '/' && href[1] == '/') {
        return nullptr; // scheme-relative URL (network)
    }

    const char* hash = str::FindChar(href, '#');
    size_t pathLen = hash ? (size_t)(hash - href) : str::Len(href);
    const char* query = (const char*)memchr(href, '?', pathLen);
    if (query) {
        pathLen = (size_t)(query - href);
    }

    if (pathLen == 0) {
        // empty or fragment-only: refers to the referencing document itself, which in SMIL / OPF
        // is never a valid target
        return nullptr;
    }
    StrBuilder joined;
    bool rooted = href[0] == '/';
    if (!rooted && !str::IsEmpty(baseDir)) {
        joined.Append(baseDir);
        joined.AppendChar('/');
    }
    joined.Append(href, pathLen);
    char* decoded = str::Dup(joined.Get());
    url::DecodeInPlace(decoded);
    if (str::FindChar(decoded, '\\') || str::FindChar(decoded, ':')) {
        str::Free(decoded);
        return nullptr;
    }

    // remove dot segments; ".." above the root is an escape attempt
    Vec<char*> segs;
    bool escaped = false;
    char* p = decoded;
    while (*p) {
        char* seg = p;
        while (*p && *p != '/') {
            p++;
        }
        if (*p == '/') {
            *p++ = 0;
        }
        if (seg[0] == 0 || str::Eq(seg, ".")) {
            continue;
        }
        if (str::Eq(seg, "..")) {
            if (segs.size() == 0) {
                escaped = true;
                break;
            }
            segs.RemoveLast();
            continue;
        }
        segs.Append(seg);
    }
    char* res = nullptr;
    if (!escaped && segs.size() > 0) {
        StrBuilder out;
        for (int i = 0; i < segs.Size(); i++) {
            if (i > 0) {
                out.AppendChar('/');
            }
            out.Append(segs[i]);
        }
        res = DupIn(arena, out.Get());
    }
    str::Free(decoded);
    if (!res) {
        return nullptr;
    }

    if (fragmentOut && hash && hash[1]) {
        char* frag = str::Dup(hash + 1);
        url::DecodeInPlace(frag);
        *fragmentOut = DupIn(arena, frag);
        str::Free(frag);
    }
    return res;
}

// --- epub:type semantics ---

static bool TokenListContains(const char* list, const char* token) {
    if (!list || !token) {
        return false;
    }
    size_t tlen = str::Len(token);
    const char* p = list;
    while (*p) {
        while (*p && str::IsWs(*p)) {
            p++;
        }
        const char* start = p;
        while (*p && !str::IsWs(*p)) {
            p++;
        }
        size_t n = (size_t)(p - start);
        if (n == tlen && memcmp(start, token, n) == 0) {
            return true;
        }
    }
    return false;
}

u32 EpubMoFlagsForEpubType(const char* epubType) {
    if (str::IsEmpty(epubType)) {
        return 0;
    }
    // EPUB 3.3 9.4.1 / 9.4.2 (non-exhaustive lists; terms from the Structural Semantics Vocabulary)
    static const char* skippable[] = {"footnote", "endnote", "rearnote", "note", "pagebreak", "sidebar", "annotation"};
    static const char* escapable[] = {"table", "table-row", "table-cell", "list", "list-item", "figure", "aside"};
    u32 flags = 0;
    for (const char* t : skippable) {
        if (TokenListContains(epubType, t)) {
            flags |= kEpubMoSkippable;
        }
    }
    for (const char* t : escapable) {
        if (TokenListContains(epubType, t)) {
            flags |= kEpubMoEscapable;
        }
    }
    return flags;
}

// --- package ---

EpubPackage::EpubPackage() {
    arena = ArenaNew();
}

EpubPackage::~EpubPackage() {
    for (EpubMoDocument* d : overlays) {
        delete d;
    }
    ArenaDelete(arena);
}

void EpubPackage::AddDiag(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char* s = str::FmtV(fmt, args);
    va_end(args);
    if (s) {
        diagnostics.Append(s);
        str::Free(s);
    }
}

bool EpubPackage::HasMediaOverlays() const {
    for (const EpubSpineItem& it : spine) {
        if (it.overlayIdx >= 0) {
            return true;
        }
    }
    return false;
}

int EpubPackage::FindManifestById(const char* id) const {
    if (!id) {
        return -1;
    }
    for (int i = 0; i < manifest.Size(); i++) {
        if (str::Eq(manifest[i].id, id)) {
            return i;
        }
    }
    return -1;
}

int EpubPackage::FindManifestByPath(const char* path) const {
    if (!path) {
        return -1;
    }
    for (int i = 0; i < manifest.Size(); i++) {
        if (str::Eq(manifest[i].path, path)) {
            return i;
        }
    }
    return -1;
}

int EpubPackage::FindSpineByPath(const char* path) const {
    int mi = FindManifestByPath(path);
    if (mi < 0) {
        return -1;
    }
    for (int i = 0; i < spine.Size(); i++) {
        if (spine[i].manifestIdx == mi) {
            return i;
        }
    }
    return -1;
}

const char* EpubPackage::SpinePath(int spineIdx) const {
    if (spineIdx < 0 || spineIdx >= spine.Size()) {
        return nullptr;
    }
    int mi = spine[spineIdx].manifestIdx;
    return mi >= 0 ? manifest[mi].path : nullptr;
}

EpubMoDocument* EpubPackage::OverlayForSpine(int spineIdx) const {
    if (spineIdx < 0 || spineIdx >= spine.Size()) {
        return nullptr;
    }
    int oi = spine[spineIdx].overlayIdx;
    return oi >= 0 ? overlays[oi] : nullptr;
}

static char* AttrDup(Arena* arena, HtmlToken* tok, const char* name, bool ns = false) {
    AttrInfo* a = ns ? tok->GetAttrByNameNS(name, nullptr) : tok->GetAttrByName(name);
    if (!a || !a->val) {
        return nullptr;
    }
    TempStr v = ResolveHtmlEntitiesTemp(a->val, a->valLen);
    if (!v) {
        return nullptr;
    }
    str::TrimWSInPlace(v, str::TrimOpt::Both);
    return str::Dup(arena, v);
}

static TempStr TextUntilEndTag(HtmlPullParser& parser) {
    StrBuilder sb;
    HtmlToken* tok;
    while ((tok = parser.Next()) != nullptr && !tok->IsError()) {
        if (tok->IsText()) {
            sb.Append(tok->s, tok->sLen);
            continue;
        }
        break;
    }
    TempStr v = ResolveHtmlEntitiesTemp(sb.Get(), (size_t)sb.Size());
    if (v) {
        str::TrimWSInPlace(v, str::TrimOpt::Both);
    }
    return v;
}

char* EpubParseContainerXml(const char* xml, size_t len) {
    if (!xml) {
        return nullptr;
    }
    HtmlPullParser parser(xml, len);
    HtmlToken* tok;
    while ((tok = parser.Next()) != nullptr && !tok->IsError()) {
        if (!(tok->IsStartTag() || tok->IsEmptyElementEndTag()) || !tok->NameIsNS("rootfile", nullptr)) {
            continue;
        }
        AttrInfo* mt = tok->GetAttrByName("media-type");
        if (mt && !mt->ValIs("application/oebps-package+xml")) {
            continue;
        }
        AttrInfo* fp = tok->GetAttrByName("full-path");
        if (!fp || fp->valLen == 0) {
            continue;
        }
        TempStr v = ResolveHtmlEntitiesTemp(fp->val, fp->valLen);
        char* frag = nullptr;
        char* path = EpubResolvePath("", v, &frag, nullptr);
        str::Free(frag);
        return path;
    }
    return nullptr;
}

struct PendingMeta {
    const char* property;
    const char* refines;
    const char* value;
};

bool EpubParsePackage(EpubPackage* pkg, const char* opfPath, const char* xml, size_t len) {
    if (!pkg || !opfPath || !xml) {
        return false;
    }
    Arena* a = pkg->arena;
    pkg->opfPath = str::Dup(a, opfPath);
    pkg->opfDir = str::Dup(a, EpubDirOfTemp(opfPath));

    Vec<PendingMeta> metas;
    Vec<const char*> itemrefProps;
    bool sawPackage = false;
    bool inSpine = false;

    HtmlPullParser parser(xml, len);
    HtmlToken* tok;
    while ((tok = parser.Next()) != nullptr) {
        if (tok->IsError()) {
            pkg->AddDiag("opf: XML error near offset %d", (int)(tok->s ? tok->s - xml : 0));
            break;
        }
        if (tok->IsEndTag()) {
            if (tok->NameIsNS("spine", nullptr)) {
                inSpine = false;
            }
            continue;
        }
        if (!tok->IsTag()) {
            continue;
        }
        bool isEmpty = tok->IsEmptyElementEndTag();
        if (tok->NameIsNS("package", nullptr)) {
            sawPackage = true;
            pkg->version = AttrDup(a, tok, "version");
            continue;
        }
        if (tok->NameIsNS("item", nullptr)) {
            EpubManifestItem it;
            it.id = AttrDup(a, tok, "id");
            const char* href = AttrDup(a, tok, "href");
            it.mediaType = AttrDup(a, tok, "media-type");
            it.properties = AttrDup(a, tok, "properties");
            it.mediaOverlay = AttrDup(a, tok, "media-overlay");
            char* frag = nullptr;
            it.path = EpubResolvePath(pkg->opfDir, href, &frag, a);
            if (!it.path) {
                // remote resources (http:) are legal for audio/video but never fetched
                pkg->AddDiag("opf: item '%s' href '%s' is not a local container path, ignored", it.id ? it.id : "",
                             href ? href : "");
            }
            if (it.path && TokenListContains(it.properties, "nav") && !pkg->navPath) {
                pkg->navPath = it.path;
            }
            pkg->manifest.Append(it);
            continue;
        }
        if (tok->NameIsNS("spine", nullptr)) {
            inSpine = !isEmpty;
            pkg->pageProgressionDirection = AttrDup(a, tok, "page-progression-direction");
            continue;
        }
        if (tok->NameIsNS("itemref", nullptr) && inSpine) {
            EpubSpineItem si;
            si.idref = AttrDup(a, tok, "idref");
            const char* linear = AttrDup(a, tok, "linear");
            si.linear = !(linear && str::Eq(linear, "no"));
            itemrefProps.Append(AttrDup(a, tok, "properties"));
            pkg->spine.Append(si);
            continue;
        }
        if (tok->NameIsNS("meta", nullptr)) {
            const char* property = AttrDup(a, tok, "property");
            if (!property) {
                continue; // EPUB 2 name/content meta
            }
            PendingMeta m;
            m.property = property;
            m.refines = AttrDup(a, tok, "refines");
            m.value = isEmpty ? nullptr : str::Dup(a, TextUntilEndTag(parser));
            metas.Append(m);
            continue;
        }
    }
    if (!sawPackage) {
        pkg->AddDiag("opf: no <package> element");
        return false;
    }

    for (PendingMeta& m : metas) {
        if (!m.value) {
            continue;
        }
        const char* refinesId = m.refines && m.refines[0] == '#' ? m.refines + 1 : nullptr;
        if (str::Eq(m.property, "media:duration")) {
            i64 us = 0;
            if (!EpubParseClockValue(m.value, &us)) {
                pkg->AddDiag("opf: invalid media:duration '%s'", m.value);
                continue;
            }
            if (!m.refines) {
                pkg->totalDurationUs = us;
            } else {
                int mi = pkg->FindManifestById(refinesId);
                if (mi >= 0) {
                    pkg->manifest[mi].durationUs = us;
                } else {
                    pkg->AddDiag("opf: media:duration refines unknown id '%s'", m.refines);
                }
            }
        } else if (str::Eq(m.property, "media:active-class") && !m.refines) {
            pkg->activeClass = m.value;
        } else if (str::Eq(m.property, "media:playback-active-class") && !m.refines) {
            pkg->playbackActiveClass = m.value;
        } else if (str::Eq(m.property, "media:narrator") && !pkg->narrator) {
            pkg->narrator = m.value;
        } else if (str::Eq(m.property, "rendition:layout") && !m.refines) {
            pkg->prePaginated = str::Eq(m.value, "pre-paginated");
        }
    }

    for (int i = 0; i < pkg->spine.Size(); i++) {
        EpubSpineItem& si = pkg->spine[i];
        si.manifestIdx = pkg->FindManifestById(si.idref);
        const char* props = itemrefProps[i];
        si.prePaginated = pkg->prePaginated;
        if (TokenListContains(props, "rendition:layout-pre-paginated")) {
            si.prePaginated = true;
        } else if (TokenListContains(props, "rendition:layout-reflowable")) {
            si.prePaginated = false;
        }
        if (si.manifestIdx < 0) {
            pkg->AddDiag("opf: spine itemref idref '%s' not in manifest", si.idref ? si.idref : "");
            continue;
        }
        EpubManifestItem& mi = pkg->manifest[si.manifestIdx];
        if (!mi.mediaOverlay) {
            continue;
        }
        int smilIdx = pkg->FindManifestById(mi.mediaOverlay);
        if (smilIdx < 0) {
            pkg->AddDiag("opf: media-overlay '%s' of '%s' not in manifest", mi.mediaOverlay, mi.id ? mi.id : "");
            continue;
        }
        EpubManifestItem& smil = pkg->manifest[smilIdx];
        if (!str::Eq(smil.mediaType, "application/smil+xml")) {
            pkg->AddDiag("opf: media-overlay '%s' has media-type '%s', expected application/smil+xml", mi.mediaOverlay,
                         smil.mediaType ? smil.mediaType : "");
            continue;
        }
        if (!smil.path) {
            continue;
        }
        int oi = -1;
        for (int k = 0; k < pkg->overlays.Size(); k++) {
            if (pkg->overlays[k]->manifestIdx == smilIdx) {
                oi = k;
                break;
            }
        }
        if (oi < 0) {
            auto* doc = new EpubMoDocument();
            doc->manifestIdx = smilIdx;
            doc->smilPath = smil.path;
            oi = pkg->overlays.Size();
            pkg->overlays.Append(doc);
        }
        si.overlayIdx = oi;
    }
    return true;
}

// --- SMIL ---

bool EpubParseSmil(EpubPackage* pkg, EpubMoDocument* doc, const char* xml, size_t len) {
    if (!pkg || !doc) {
        return false;
    }
    doc->parsed = true;
    doc->pars.Reset();
    doc->seqs.Reset();
    if (!xml || len == 0) {
        doc->failed = true;
        pkg->AddDiag("smil %s: missing or empty", doc->smilPath);
        return false;
    }
    Arena* a = pkg->arena;
    TempStr smilDir = EpubDirOfTemp(doc->smilPath);

    // stack of open containers: -2 = body, -3 = other, >= 0 = seq index, -4 = par
    Vec<int> stack;
    bool inBody = false;
    bool sawSmil = false;
    int curPar = -1;
    bool parHasText = false;

    HtmlPullParser parser(xml, len);
    HtmlToken* tok;
    while ((tok = parser.Next()) != nullptr) {
        if (tok->IsError()) {
            pkg->AddDiag("smil %s: XML error near offset %d", doc->smilPath, (int)(tok->s ? tok->s - xml : 0));
            break;
        }
        if (!tok->IsTag()) {
            continue;
        }
        if (tok->IsEndTag()) {
            if (tok->NameIsNS("body", nullptr)) {
                inBody = false;
                stack.Reset();
            } else if (tok->NameIsNS("seq", nullptr) && stack.Size() > 0) {
                int top = stack.Last();
                if (top >= 0) {
                    doc->seqs[top].endPar = doc->pars.Size();
                }
                stack.Pop();
            } else if (tok->NameIsNS("par", nullptr) && curPar >= 0) {
                if (!parHasText) {
                    pkg->AddDiag("smil %s: par '%s' has no text element, dropped", doc->smilPath,
                                 doc->pars[curPar].id ? doc->pars[curPar].id : "");
                    doc->pars.RemoveAt(curPar);
                }
                curPar = -1;
            }
            continue;
        }
        bool isEmpty = tok->IsEmptyElementEndTag();
        if (tok->NameIsNS("smil", nullptr)) {
            sawSmil = true;
            continue;
        }
        if (tok->NameIsNS("body", nullptr)) {
            inBody = true;
            continue;
        }
        if (!inBody) {
            continue;
        }
        int parentSeq = -1;
        for (int i = stack.Size() - 1; i >= 0; i--) {
            if (stack[i] >= 0) {
                parentSeq = stack[i];
                break;
            }
        }
        u32 inherited = parentSeq >= 0 ? doc->seqs[parentSeq].flags : 0;

        if (tok->NameIsNS("seq", nullptr)) {
            EpubMoSeq seq;
            seq.id = AttrDup(a, tok, "id");
            seq.epubType = AttrDup(a, tok, "type", true);
            seq.flags = inherited | EpubMoFlagsForEpubType(seq.epubType);
            seq.parentSeq = parentSeq;
            seq.firstPar = doc->pars.Size();
            seq.endPar = seq.firstPar;
            const char* textref = AttrDup(a, tok, "textref", true);
            if (textref) {
                char* frag = nullptr;
                seq.textPath = EpubResolvePath(smilDir, textref, &frag, a);
                seq.textFragment = frag;
            } else {
                pkg->AddDiag("smil %s: seq '%s' without epub:textref", doc->smilPath, seq.id ? seq.id : "");
            }
            int idx = doc->seqs.Size();
            doc->seqs.Append(seq);
            if (!isEmpty) {
                stack.Append(idx);
            }
            continue;
        }
        if (tok->NameIsNS("par", nullptr)) {
            EpubMoPar par;
            par.id = AttrDup(a, tok, "id");
            par.epubType = AttrDup(a, tok, "type", true);
            par.flags = inherited | EpubMoFlagsForEpubType(par.epubType);
            par.seqIdx = parentSeq;
            if (isEmpty) {
                pkg->AddDiag("smil %s: empty par '%s' dropped", doc->smilPath, par.id ? par.id : "");
                continue;
            }
            curPar = doc->pars.Size();
            parHasText = false;
            doc->pars.Append(par);
            continue;
        }
        if (curPar < 0) {
            continue;
        }
        EpubMoPar& par = doc->pars[curPar];
        if (tok->NameIsNS("text", nullptr)) {
            const char* src = AttrDup(a, tok, "src");
            char* frag = nullptr;
            char* path = EpubResolvePath(smilDir, src, &frag, a);
            if (!path) {
                pkg->AddDiag("smil %s: text src '%s' rejected", doc->smilPath, src ? src : "");
                continue;
            }
            par.textPath = path;
            par.textFragment = frag;
            parHasText = true;
            continue;
        }
        if (tok->NameIsNS("audio", nullptr)) {
            const char* src = AttrDup(a, tok, "src");
            char* frag = nullptr;
            char* path = EpubResolvePath(smilDir, src, &frag, a);
            if (!path) {
                pkg->AddDiag("smil %s: audio src '%s' is not a local container path, clip is silent", doc->smilPath,
                             src ? src : "");
                continue;
            }
            const char* cb = AttrDup(a, tok, "clipBegin");
            const char* ce = AttrDup(a, tok, "clipEnd");
            i64 b = 0;
            i64 e = kEpubMoClipToEnd;
            if (cb && !EpubParseClockValue(cb, &b)) {
                pkg->AddDiag("smil %s: invalid clipBegin '%s'", doc->smilPath, cb);
                b = 0;
            }
            if (ce && !EpubParseClockValue(ce, &e)) {
                pkg->AddDiag("smil %s: invalid clipEnd '%s'", doc->smilPath, ce);
                e = kEpubMoClipToEnd;
            }
            if (e != kEpubMoClipToEnd && e <= b) {
                pkg->AddDiag("smil %s: par '%s' clipEnd <= clipBegin, audio ignored", doc->smilPath,
                             par.id ? par.id : "");
                continue;
            }
            par.audioPath = path;
            par.clipBeginUs = b;
            par.clipEndUs = e;
            continue;
        }
    }
    if (curPar >= 0 && !parHasText) {
        doc->pars.RemoveAt(curPar);
    }
    for (EpubMoSeq& s : doc->seqs) {
        if (s.endPar < s.firstPar) {
            s.endPar = doc->pars.Size();
        }
    }
    if (!sawSmil) {
        pkg->AddDiag("smil %s: no <smil> root", doc->smilPath);
    }
    doc->failed = doc->pars.Size() == 0;
    return !doc->failed;
}

// --- media:active-class colour ---

static int HexVal(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static bool ParseCssColor(const char* s, const char* end, u32* rgbOut) {
    while (s < end && str::IsWs(*s)) {
        s++;
    }
    if (s < end && *s == '#') {
        s++;
        const char* h = s;
        while (s < end && HexVal(*s) >= 0) {
            s++;
        }
        size_t n = (size_t)(s - h);
        if (n == 3 || n == 4) {
            int r = HexVal(h[0]), g = HexVal(h[1]), b = HexVal(h[2]);
            *rgbOut = (u32)((r * 17) << 16 | (g * 17) << 8 | (b * 17));
            return true;
        }
        if (n == 6 || n == 8) {
            u32 v = 0;
            for (int i = 0; i < 6; i++) {
                v = v << 4 | (u32)HexVal(h[i]);
            }
            *rgbOut = v;
            return true;
        }
        return false;
    }
    if (end - s > 4 && (str::EqNIx(s, 4, "rgb(") || str::EqNIx(s, 5, "rgba("))) {
        s = (const char*)memchr(s, '(', (size_t)(end - s)) + 1;
        int comps[3] = {};
        for (int i = 0; i < 3; i++) {
            while (s < end && (str::IsWs(*s) || *s == ',')) {
                s++;
            }
            int v = 0;
            int nd = 0;
            while (s < end && *s >= '0' && *s <= '9') {
                v = v * 10 + (*s - '0');
                s++;
                nd++;
            }
            if (nd == 0) {
                return false;
            }
            while (s < end && (*s == '.' || (*s >= '0' && *s <= '9'))) {
                s++;
            }
            if (s < end && *s == '%') {
                v = v * 255 / 100;
                s++;
            }
            comps[i] = v > 255 ? 255 : v;
        }
        *rgbOut = (u32)(comps[0] << 16 | comps[1] << 8 | comps[2]);
        return true;
    }
    static const struct {
        const char* name;
        u32 rgb;
    } kNames[] = {
        {"yellow", 0xffff00},    {"red", 0xff0000},        {"lime", 0x00ff00},   {"green", 0x008000},
        {"blue", 0x0000ff},      {"aqua", 0x00ffff},       {"cyan", 0x00ffff},   {"fuchsia", 0xff00ff},
        {"magenta", 0xff00ff},   {"orange", 0xffa500},     {"gold", 0xffd700},   {"pink", 0xffc0cb},
        {"silver", 0xc0c0c0},    {"gray", 0x808080},       {"grey", 0x808080},   {"black", 0x000000},
        {"white", 0xffffff},     {"navy", 0x000080},       {"purple", 0x800080}, {"lightyellow", 0xffffe0},
        {"lightblue", 0xadd8e6}, {"lightgreen", 0x90ee90},
    };
    const char* w = s;
    while (s < end && ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z'))) {
        s++;
    }
    size_t n = (size_t)(s - w);
    for (auto& c : kNames) {
        if (n > 0 && str::EqNIx(w, n, c.name)) {
            *rgbOut = c.rgb;
            return true;
        }
    }
    return false;
}

static bool IsCssIdentChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
           (unsigned char)c >= 0x80;
}

// value of property name inside a declaration block [s, end)
static bool FindCssDecl(const char* s, const char* end, const char* name, const char** valOut, const char** valEndOut) {
    size_t nlen = str::Len(name);
    const char* p = s;
    while (p < end) {
        while (p < end && (str::IsWs(*p) || *p == ';')) {
            p++;
        }
        const char* propStart = p;
        while (p < end && *p != ':' && *p != ';') {
            p++;
        }
        if (p >= end || *p != ':') {
            continue;
        }
        const char* propEnd = p;
        while (propEnd > propStart && str::IsWs(propEnd[-1])) {
            propEnd--;
        }
        p++;
        const char* v = p;
        while (p < end && *p != ';') {
            p++;
        }
        if ((size_t)(propEnd - propStart) == nlen && str::EqNIx(propStart, nlen, name)) {
            *valOut = v;
            *valEndOut = p;
            return true;
        }
    }
    return false;
}

bool EpubCssFindClassColor(const char* css, size_t len, const char* className, u32* rgbOut, bool* isBackgroundOut) {
    if (!css || !className || !*className || !rgbOut) {
        return false;
    }
    size_t clen = str::Len(className);
    const char* end = css + len;
    const char* p = css;
    bool found = false;
    while (p < end) {
        const char* dot = (const char*)memchr(p, '.', (size_t)(end - p));
        if (!dot) {
            break;
        }
        p = dot + 1;
        if ((size_t)(end - p) < clen || memcmp(p, className, clen) != 0) {
            continue;
        }
        if (p + clen < end && IsCssIdentChar(p[clen])) {
            continue;
        }
        if (dot > css && IsCssIdentChar(dot[-1]) && !(dot[-1] >= 'a' && dot[-1] <= 'z') &&
            !(dot[-1] >= 'A' && dot[-1] <= 'Z')) {
            // "1.5em" style numbers are not selectors
            continue;
        }
        // must be in a selector: the next '{' comes before the next '}' / ';'
        const char* brace = p;
        while (brace < end && *brace != '{' && *brace != '}' && *brace != ';') {
            brace++;
        }
        if (brace >= end || *brace != '{') {
            continue;
        }
        const char* blockEnd = (const char*)memchr(brace, '}', (size_t)(end - brace));
        if (!blockEnd) {
            break;
        }
        const char *v = nullptr, *ve = nullptr;
        if (FindCssDecl(brace + 1, blockEnd, "background-color", &v, &ve) ||
            FindCssDecl(brace + 1, blockEnd, "background", &v, &ve)) {
            if (ParseCssColor(v, ve, rgbOut)) {
                if (isBackgroundOut) {
                    *isBackgroundOut = true;
                }
                return true;
            }
        }
        if (!found && FindCssDecl(brace + 1, blockEnd, "color", &v, &ve) && ParseCssColor(v, ve, rgbOut)) {
            // keep looking for a later rule with a background
            found = true;
            if (isBackgroundOut) {
                *isBackgroundOut = false;
            }
        }
        p = blockEnd;
    }
    return found;
}

// --- dump ---

static void AppendClock(StrBuilder& out, i64 us) {
    if (us < 0) {
        out.Append("end");
        return;
    }
    i64 ms = us / 1000;
    out.AppendFmt("%d:%02d:%02d.%03d", (int)(ms / 3600000), (int)(ms / 60000 % 60), (int)(ms / 1000 % 60),
                  (int)(ms % 1000));
}

void EpubMoDumpTimeline(EpubPackage* pkg, StrBuilder& out) {
    if (!pkg) {
        return;
    }
    out.AppendFmt("opf: %s version=%s\n", pkg->opfPath ? pkg->opfPath : "", pkg->version ? pkg->version : "");
    if (pkg->activeClass) {
        out.AppendFmt("active-class: %s\n", pkg->activeClass);
    }
    if (pkg->playbackActiveClass) {
        out.AppendFmt("playback-active-class: %s\n", pkg->playbackActiveClass);
    }
    if (pkg->totalDurationUs >= 0) {
        out.Append("duration: ");
        AppendClock(out, pkg->totalDurationUs);
        out.Append("\n");
    }
    int n = 0;
    i64 sum = 0;
    for (int si = 0; si < pkg->spine.Size(); si++) {
        const EpubSpineItem& item = pkg->spine[si];
        EpubMoDocument* doc = pkg->OverlayForSpine(si);
        if (!doc) {
            continue;
        }
        out.AppendFmt("spine[%d] %s%s smil=%s\n", si, pkg->SpinePath(si) ? pkg->SpinePath(si) : "",
                      item.linear ? "" : " (non-linear)", doc->smilPath);
        if (!doc->parsed) {
            out.Append("  (not parsed)\n");
            continue;
        }
        for (int pi = 0; pi < doc->pars.Size(); pi++) {
            const EpubMoPar& p = doc->pars[pi];
            out.AppendFmt("  %4d %s#%s ", n++, p.textPath ? p.textPath : "", p.textFragment ? p.textFragment : "");
            if (p.audioPath) {
                out.AppendFmt("%s ", p.audioPath);
                AppendClock(out, p.clipBeginUs);
                out.Append("-");
                AppendClock(out, p.clipEndUs);
                if (p.clipEndUs >= 0) {
                    sum += p.clipEndUs - p.clipBeginUs;
                }
            } else {
                out.Append("(no audio)");
            }
            if (p.flags & kEpubMoSkippable) {
                out.Append(" skippable");
            }
            if (p.flags & kEpubMoEscapable) {
                out.Append(" escapable");
            }
            out.Append("\n");
        }
    }
    out.Append("clips total: ");
    AppendClock(out, sum);
    out.Append("\n");
    for (int i = 0; i < pkg->diagnostics.Size(); i++) {
        out.AppendFmt("diag: %s\n", pkg->diagnostics.At(i));
    }
}
