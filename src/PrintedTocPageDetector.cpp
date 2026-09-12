/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */
#include "PrintedTocPageDetector.h"
#include "utils/FileUtil.h"
#include <math.h>

namespace {
// Dimensionless tolerances, shared by measurement and sequence inference.
constexpr float kAnchorTolerance = 0.025f;
constexpr float kEnter = 6.2f;
constexpr float kStay = 4.0f;
constexpr float kParagraphBoundary = 0.55f;
float Unit(float v) {
    return v < 0 ? 0 : (v > 1 ? 1 : v);
}
float Min(float a, float b) {
    return a < b ? a : b;
}
int CompareFloat(const float* a, const float* b) {
    return *a < *b ? -1 : (*a > *b ? 1 : 0);
}
float Median(Vec<float>& v) {
    if (v.IsEmpty()) return 0;
    v.SortTyped(CompareFloat);
    return v[v.Size() / 2];
}
int Glyphs(const char* s) {
    int n = 0;
    for (const unsigned char* p = (const unsigned char*)s; p && *p; p++) {
        if ((*p & 0xc0) != 0x80 && *p > 32) n++;
    }
    return n;
}
bool HasText(const char* s, int len) {
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 0xe0 || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) return true;
    }
    return false;
}
// Recognize only a terminal page-like token. No title, level or destination
// interpretation, and no confidence filter on the OCR text.
int PageSuffix(const char* s) {
    int end = (int)str::Len(s);
    while (end && (s[end - 1] == ' ' || s[end - 1] == ')' || s[end - 1] == ']')) end--;
    int start = end;
    while (start && s[start - 1] >= '0' && s[start - 1] <= '9') start--;
    if (start < end && end - start <= 4) return start;
    start = end;
    while (start && strchr("ivxlcdmIVXLCDM", s[start - 1])) start--;
    if (start < end && end - start <= 8 &&
        (start == 0 || !((s[start - 1] >= 'a' && s[start - 1] <= 'z') || (s[start - 1] >= 'A' && s[start - 1] <= 'Z'))))
        return start;
    return -1;
}
bool Heading(const char* s, bool excluded) {
    char compact[160]{};
    int n = 0;
    for (const unsigned char* p = (const unsigned char*)s; *p && n < 159; p++) {
        if (*p > 32) compact[n++] = (*p >= 'A' && *p <= 'Z') ? *p + 32 : *p;
    }
    if (excluded)
        return str::Eq(compact, "index") || str::Eq(compact, "references") || str::Eq(compact, "bibliography") ||
               str::Eq(compact, "索引") || str::Eq(compact, "参考文献") || str::Eq(compact, "参考书目");
    return str::Eq(compact, "目录") || str::Eq(compact, "目次") || str::Eq(compact, "contents") ||
           str::Eq(compact, "tableofcontents");
}
struct Row {
    float x = 0, right = 0, y = 0, h = 0;
    bool entry = false;
    bool longText = false;
};
float Similarity(const TocPageFeatures& a, const TocPageFeatures& b) {
    float leftA = 0, leftB = 0;
    for (int i = 0; i < 10; i++) {
        leftA += (i + 0.5f) * 0.1f * a.leftHistogram[i];
        leftB += (i + 0.5f) * 0.1f * b.leftHistogram[i];
    }
    float distance = 1;
    for (int i = 0; i < a.columns; i++)
        for (int j = 0; j < b.columns; j++) {
            float absolute = fabsf(a.anchorX[i] - b.anchorX[j]);
            // Mirrored inner/outer margins translate the complete layout.
            float relative = fabsf((a.anchorX[i] - leftA) - (b.anchorX[j] - leftB));
            distance = Min(distance, Min(absolute, relative));
        }
    float left = 0, y = 0;
    for (int i = 0; i < 10; i++) left += Min(a.leftHistogram[i], b.leftHistogram[i]);
    for (int i = 0; i < 8; i++) y += Min(a.yHistogram[i], b.yHistogram[i]);
    float height = Unit(1 - fabsf(a.meanHeight - b.meanHeight) / 0.025f);
    float density = Unit(1 - fabsf(a.density - b.density) / 0.3f);
    if (a.inlineLabels >= 0.5f && b.inlineLabels >= 0.5f) {
        return 0.35f + 0.3f * left + 0.1f * y + 0.15f * height + 0.1f * density;
    }
    if (!a.columns || !b.columns) return 0;
    return 0.5f * Unit(1 - distance / 0.08f) + 0.2f * left + 0.1f * y + 0.1f * height + 0.1f * density;
}
bool Boundary(const TocPageFeatures& p) {
    return p.excludedHeading || p.paragraph >= kParagraphBoundary || p.titlePage >= 0.8f || p.tableImage >= 0.8f;
}
} // namespace

TocPageFeatures MeasureTocPage(const PtPageData& page, int documentPages) {
    TocPageFeatures f;
    f.page = page.pageIndex;
    if (page.width <= 0 || page.height <= 0) return f;
    Vec<Row> rows;
    Vec<float> anchors;
    Vec<float> heights;
    int leaderCount = 0, numericCount = 0, longCount = 0, pairedNumbers = 0, inlineCount = 0;
    // Use raw OCR boxes: reconstruction may merge the two sides of a dense
    // scan, or reject a low-confidence row before the page can be measured.
    for (const PtToken& t : page.tokens) {
        if (!t.text || t.box.IsEmpty()) continue;
        float y = t.box.MidY() / page.height;
        if (y < 0.025f || y > 0.96f) continue; // running folios are not entries
        int glyphs = Glyphs(t.text);
        if (!glyphs) continue;
        Row r;
        r.x = t.box.x0 / page.width;
        r.right = t.box.x1 / page.width;
        r.y = y;
        r.h = t.box.Dy() / page.height;
        if (y < 0.4f && glyphs < 35) {
            f.title |= Heading(t.text, false);
            f.excludedHeading |= Heading(t.text, true);
            // OCR often detects the spaced Chinese heading as two boxes.
            if (glyphs <= 2) {
                for (const PtToken& other : page.tokens) {
                    if (&other == &t || !other.text || Glyphs(other.text) > 2) continue;
                    if (other.box.x0 <= t.box.x0 || other.box.x0 - t.box.x1 > 0.12f * page.width) continue;
                    if (fabsf(other.box.MidY() - t.box.MidY()) > 0.4f * t.box.Dy()) continue;
                    f.title |= Heading(str::FormatTemp("%s%s", t.text, other.text), false);
                }
            }
        }
        int suffix = PageSuffix(t.text);
        bool text = HasText(t.text, (int)str::Len(t.text));
        bool pageLike = suffix >= 0;
        if (pageLike && !text) numericCount++;
        bool rightRegion = r.right >= 0.65f || (r.right >= 0.38f && r.right <= 0.58f);
        bool inlineLabel = suffix > 0 && t.text[suffix - 1] == '/';
        rightRegion |= inlineLabel;
        if (pageLike && rightRegion) {
            f.pageNumbers++;
            // A suffix in a wide OCR box, or a separate digit box paired
            // with text to its left at the same baseline, is sufficient.
            r.entry = suffix > 0 && HasText(t.text, suffix) && t.box.Dx() / page.width > 0.12f;
            if (!r.entry && !text) {
                float nearest = 1;
                for (const PtToken& other : page.tokens) {
                    if (&other == &t || !other.text || !HasText(other.text, (int)str::Len(other.text))) continue;
                    float dy = fabsf(other.box.MidY() - t.box.MidY()) / page.height;
                    float tolerance = 0.45f * Min(other.box.Dy(), t.box.Dy()) / page.height;
                    float gap = (t.box.x0 - other.box.x1) / page.width;
                    if (dy <= tolerance && gap >= -0.01f && gap < nearest && other.box.x0 < t.box.x0) {
                        nearest = gap;
                        r.entry = true;
                        r.x = other.box.x0 / page.width;
                    }
                }
            }
            if (r.entry) {
                anchors.Append(r.right);
                if (!text) pairedNumbers++;
                if (inlineLabel) inlineCount++;
            }
        }
        bool leader = str::Find(t.text, "...") || str::Find(t.text, "……") || str::Find(t.text, "··") ||
                      str::Find(t.text, "．．") || str::Find(t.text, "…");
        if (leader && r.entry) leaderCount++;
        // Wide *sentences* without terminal page structure, not text count
        // alone: dense TOC pages must not acquire a body-text penalty.
        r.longText = text && !r.entry && glyphs >= 28 && r.right - r.x >= 0.28f;
        if (text) {
            rows.Append(r);
            heights.Append(r.h);
            f.density += (r.right - r.x) * r.h;
        } else if (r.entry) {
            rows.Append(r);
        }
    }
    // A title and its separately detected page number are one entry row;
    // the title must not also count as a body paragraph.
    for (const Row& r : rows) {
        if (!r.longText) continue;
        bool paired = false;
        for (const Row& entry : rows) {
            if (entry.entry && fabsf(entry.x - r.x) < 0.01f && fabsf(entry.y - r.y) <= 0.45f * Min(entry.h, r.h)) {
                paired = true;
                break;
            }
        }
        if (!paired) longCount++;
    }
    f.lines = rows.Size() - pairedNumbers;
    f.meanHeight = Median(heights);
    // Count at most two stable right-edge clusters; each needs repeated
    // support, so an isolated footer/decimal cannot produce full alignment.
    Vec<float> remaining = anchors;
    int clustered = 0;
    for (int c = 0; c < 2; c++) {
        int best = 0;
        float center = 0;
        for (float x : remaining) {
            if (c && fabsf(x - f.anchorX[0]) < 0.18f) continue;
            int count = 0;
            for (float other : remaining)
                if (fabsf(other - x) <= kAnchorTolerance) count++;
            if (count > best) {
                best = count;
                center = x;
            }
        }
        if (best < 2) break;
        f.anchorX[f.columns++] = center;
        clustered += best;
        for (int i = remaining.Size() - 1; i >= 0; i--)
            if (fabsf(remaining[i] - center) <= kAnchorTolerance) remaining.RemoveAt(i);
    }
    f.entries = anchors.Size();
    float support = Unit((float)clustered / 6);
    f.pageColumn = f.entries ? support * clustered / f.entries : 0;
    f.leaders = f.entries ? (float)leaderCount / f.entries : 0;
    f.inlineLabels = f.entries ? (float)inlineCount / f.entries : 0;
    Vec<float> entryY, lefts;
    for (const Row& r : rows) {
        if (!r.entry) continue;
        entryY.Append(r.y);
        lefts.Append(r.x);
        f.leftHistogram[(int)Min(9, r.x * 10)] += 1.0f;
        f.yHistogram[(int)Min(7, r.y * 8)] += 1.0f;
    }
    for (float& v : f.leftHistogram) v /= (float)(f.entries > 0 ? f.entries : 1);
    for (float& v : f.yHistogram) v /= (float)(f.entries > 0 ? f.entries : 1);
    entryY.SortTyped(CompareFloat);
    Vec<float> gaps;
    for (int i = 1; i < entryY.Size(); i++) {
        float gap = entryY[i] - entryY[i - 1];
        if (gap > 0.005f) gaps.Append(gap);
    }
    float medianGap = Median(gaps);
    int regular = 0;
    for (float g : gaps)
        if (fabsf(g - medianGap) <= medianGap * 0.35f) regular++;
    f.spacing = gaps.Size() ? (float)regular / gaps.Size() : 0;
    lefts.SortTyped(CompareFloat);
    int bands = 0;
    for (int i = 0; i < lefts.Size();) {
        int j = i + 1;
        while (j < lefts.Size() && lefts[j] - lefts[i] < 0.025f) j++;
        if (j - i >= 2) bands++;
        i = j;
    }
    f.indentation = bands >= 2 && bands <= 4 ? 1 : (bands == 1 ? 0.3f : 0);
    f.paragraph = f.lines ? Unit((float)longCount / f.lines * 1.5f) : 0;
    f.titlePage = f.lines > 0 && f.lines <= 5 && f.entries < 2 ? 1.f : 0.f;
    // Several unpaired numeric cells indicate a table/diagram, never treat
    // all rasterized pages as images (that would penalize every scanned TOC).
    f.tableImage = numericCount >= 6 ? Unit((float)(numericCount - f.entries) / numericCount) : 0;
    float ratio = f.lines ? Unit((float)f.entries / f.lines) : 0;
    f.raw = 3.0f * f.pageColumn + 2.0f * ratio + 1.5f * Unit((float)f.entries / 10) + 1.5f * f.inlineLabels +
            0.4f * f.leaders + 0.3f * f.indentation + 0.4f * f.spacing - 5.0f * f.paragraph - 2.5f * f.titlePage -
            3.0f * f.tableImage - (f.excludedHeading ? 6 : 0);
    float prior = documentPages > 0 ? 0.3f * (1 - Unit((float)f.page / documentPages)) : 0;
    f.start = f.raw + (f.title ? 2.5f : 0) + prior;
    f.smoothed = f.raw;
    if (f.excludedHeading)
        f.reason = "Index/References heading";
    else if (f.paragraph >= kParagraphBoundary)
        f.reason = "paragraph-like body text";
    else if (f.titlePage)
        f.reason = "isolated title / sparse non-entry page";
    else if (f.tableImage >= 0.8f)
        f.reason = "unpaired table/diagram numbers";
    return f;
}

TocPageInterval DetectTocPageInterval(Vec<TocPageFeatures>& pages) {
    TocPageInterval best;
    float bestScore = 0;
    for (auto& p : pages) {
        p.selected = false;
        p.state = "NON_TOC";
        p.neighbor = p.similarity = 0;
        p.smoothed = p.raw;
        p.reason = p.excludedHeading
                       ? "Index/References heading"
                       : (p.paragraph >= kParagraphBoundary
                              ? "paragraph-like body text"
                              : (p.titlePage ? "isolated title / sparse non-entry page"
                                             : (p.tableImage >= 0.8f ? "unpaired table/diagram numbers"
                                                                     : "insufficient interval evidence")));
    }
    // SEARCHING -> IN_TOC -> AFTER_TOC. Compare complete candidates instead
    // of making the first incidental heading permanently authoritative.
    for (int seed = 0; seed < pages.Size(); seed++) {
        const auto& first = pages[seed];
        if (first.start < kEnter || first.entries < 3 || Boundary(first)) continue;
        int start = seed, end = seed, misses = 0;
        float quality = first.start;
        // A sparse first page can be recovered by the strong next page, but
        // only with actual entry structure (never fixed +/- N expansion).
        while (start > 0 && pages[start - 1].page + 1 == pages[start].page) {
            const auto& p = pages[start - 1];
            float similarity = Similarity(p, pages[start]);
            if (Boundary(p) || p.entries < 2 || similarity < 0.30f || p.raw + 1.5f * similarity < kStay - 0.5f) break;
            quality += p.raw;
            start--;
        }
        for (int i = seed + 1; i < pages.Size(); i++) {
            if (pages[i].page != pages[i - 1].page + 1) break;
            const auto& p = pages[i];
            float similarity = Similarity(p, pages[end]);
            bool stay = !Boundary(p) && p.entries >= 2 && similarity >= 0.55f && p.raw + 1.5f * similarity >= kStay;
            if (stay) {
                // Heal one uncertain page, but never a clear body/Index page.
                if (misses && Boundary(pages[i - 1])) break;
                end = i;
                quality += p.raw;
                misses = 0;
            } else if (++misses >= 2)
                break;
        }
        if (quality > bestScore) {
            bestScore = quality;
            best.startPage = pages[start].page;
            best.endPage = pages[end].page;
            best.confidence = Unit(quality / ((end - start + 1) * 8));
        }
    }
    int previous = -1;
    for (int i = 0; i < pages.Size(); i++) {
        auto& p = pages[i];
        p.selected = best.startPage > 0 && p.page >= best.startPage && p.page <= best.endPage;
        p.neighbor = 0;
        if (!p.selected) continue;
        if (previous >= 0) {
            p.similarity = Similarity(p, pages[previous]);
            p.neighbor = 1.5f * p.similarity;
        }
        p.smoothed = p.raw + p.neighbor;
        p.state = p.start >= kEnter ? "TOC_STRONG" : "TOC_WEAK";
        p.reason = p.page == best.startPage
                       ? "START: title/entry structure + aligned page column"
                       : (p.entries < 2 ? "CONTINUE: one uncertain page between supported TOC pages"
                                        : (p.page == best.endPage ? "END: last supported page before confirmed boundary"
                                                                  : "CONTINUE: entry structure + similar layout"));
        previous = i;
    }
    return best;
}

void WriteTocPageDiagnostics(const char* output, const Vec<TocPageFeatures>& pages, const TocPageInterval& interval) {
    PtJsonBuf json;
    json.Raw(str::FormatTemp("{\"startPage\":%d,\"endPage\":%d,\"confidence\":%.3f,\"pages\":[", interval.startPage,
                             interval.endPage, interval.confidence));
    bool first = true;
    for (const auto& p : pages) {
        json.Sep(first);
        json.Raw(
            str::FormatTemp("{\"page\":%d,\"raw\":%.3f,\"start\":%.3f,\"smoothed\":%.3f,"
                            "\"title\":%s,\"lines\":%d,\"entries\":%d,\"pageNumbers\":%d,\"columns\":%d,"
                            "\"pageColumn\":%.3f,\"leaders\":%.3f,\"indentation\":%.3f,\"spacing\":%.3f,"
                            "\"paragraph\":%.3f,\"tableImage\":%.3f,\"titlePage\":%.3f,\"neighbor\":%.3f,"
                            "\"similarity\":%.3f,\"selected\":%s,\"state\":",
                            p.page, p.raw, p.start, p.smoothed, p.title ? "true" : "false", p.lines, p.entries,
                            p.pageNumbers, p.columns, p.pageColumn, p.leaders, p.indentation, p.spacing, p.paragraph,
                            p.tableImage, p.titlePage, p.neighbor, p.similarity, p.selected ? "true" : "false"));
        json.Escaped(p.state);
        json.Raw(",\"reason\":");
        json.Escaped(p.reason);
        json.Raw("}");
    }
    json.Raw("]}");
    AutoFree data(json.Steal());
    dir::CreateForFile(output);
    file::WriteFile(output, ByteSlice(data.Get()));
}
