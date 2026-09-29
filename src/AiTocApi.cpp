/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// AI printed-TOC recognition via an OpenAI-compatible vision API.
//
// This is a C++ port of the autoContents Python pipeline (mainprogress/):
//  - stage A: per-page title/page extraction prompt + CSV repair + null-page
//    fill (qwen_vl_extract.py)
//  - stage B: per-page level determination with the first page as few-shot
//    (determine_toc_levels.py) + level post-processing and normalization
//    (content_postprocessor.py)
//  - stage C (on demand): AI page-offset sampling for scanned books
//    (pdf_metadata_extractor.py calculate_offset / calculate_roman_offset)
//  - sliding-window AI detection of the printed-TOC page range
//    (pdf_metadata_extractor.py extract_toc_info)
//
// Everything here runs on worker threads; the caller marshals progress and
// results back to the UI thread.

#include "utils/BaseUtil.h"
#include "utils/ScopedWin.h"
#include "utils/FileUtil.h"
#include "utils/GdiPlusUtil.h"
#include "utils/JsonParser.h"
#include "utils/CryptoUtil.h"
#include "utils/ThreadUtil.h"
#include "utils/WinUtil.h"
#include "utils/Log.h"

#include "wingui/UIModels.h"

#include "Settings.h"
#include "DocProperties.h"
#include "DisplayMode.h"
#include "DocController.h"
#include "EngineBase.h"
#include "DisplayModel.h"
#include "RenderCache.h"
#include "TocCalib.h"

#include "AiTocApi.h"

#include <wincodec.h>
#include <wininet.h>
#include <oleauto.h>
#include <ctype.h>

using Gdiplus::Bitmap;
using Gdiplus::Color;
using Gdiplus::Graphics;
using Gdiplus::Ok;
using Gdiplus::SolidBrush;
using Gdiplus::StringAlignmentCenter;
using Gdiplus::StringFormat;

static int ApiConcurrency(const AiTocApiConfig& cfg) {
    return cfg.concurrency < 1 ? 1 : cfg.concurrency > 8 ? 8 : cfg.concurrency;
}
static const int kApiMaxAttempts = 3;
static const int kApiRequestTimeoutMs = 180 * 1000;
static const float kApiImageLongEdge = 1500.f;
static const int kApiJpegQuality = 85;
static const int kDetectMaxScanPages = 100;
static const int kDetectInitialPages = 20;
static const int kDetectExpandStep = 10;
static const int kDetectMaxRetries = 5;
static const int kRomanOffsetMaxPages = 8;

// ---------------------------------------------------------------------------
// small string helpers

static char* DupRange(const char* start, size_t len) {
    char* s = (char*)malloc(len + 1);
    if (!s) {
        return nullptr;
    }
    memcpy(s, start, len);
    s[len] = 0;
    return s;
}

static void TrimInPlace(char* s) {
    if (!s) {
        return;
    }
    size_t n = strlen(s);
    size_t b = 0;
    while (b < n && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) {
        b++;
    }
    size_t e = n;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) {
        e--;
    }
    memmove(s, s + b, e - b);
    s[e - b] = 0;
}

static bool ParseIntStrict(const char* s, int* out) {
    if (!s || !*s) {
        return false;
    }
    bool neg = false;
    const char* p = s;
    if (*p == '-') {
        neg = true;
        p++;
    }
    if (!*p) {
        return false;
    }
    i64 v = 0;
    for (; *p; p++) {
        if (*p < '0' || *p > '9') {
            return false;
        }
        v = v * 10 + (*p - '0');
        if (v > 1000000) {
            return false;
        }
    }
    *out = neg ? -(int)v : (int)v;
    return true;
}

// Splits a block of text into lines (malloc'd). The caller frees them.
static void SplitLines(const char* text, Vec<char*>& linesOut) {
    if (!text) {
        return;
    }
    const char* p = text;
    while (*p) {
        const char* nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        char* line = DupRange(p, len);
        if (line) {
            TrimInPlace(line);
            linesOut.Append(line);
        }
        if (!nl) {
            break;
        }
        p = nl + 1;
    }
}

static void FreeCStrVec(Vec<char*>& v) {
    for (size_t i = 0; i < v.size(); i++) {
        str::Free(v[i]);
    }
    v.Reset();
}

static void AppendJsonEscaped(StrBuilder& out, const char* s) {
    out.AppendChar('"');
    if (s) {
        for (const u8* p = (const u8*)s; *p; p++) {
            u8 c = *p;
            if (c == '"') {
                out.Append("\\\"");
            } else if (c == '\\') {
                out.Append("\\\\");
            } else if (c == '\n') {
                out.Append("\\n");
            } else if (c == '\r') {
                out.Append("\\r");
            } else if (c == '\t') {
                out.Append("\\t");
            } else if (c < 0x20) {
                out.AppendFmt("\\u%04x", (int)c);
            } else {
                out.AppendChar((char)c);
            }
        }
    }
    out.AppendChar('"');
}

// ---------------------------------------------------------------------------
// base64

static void Base64Encode(const u8* data, size_t len, StrBuilder& out) {
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i = 0;
    while (i < len) {
        size_t n = len - i;
        u32 b0 = data[i];
        u32 b1 = n > 1 ? data[i + 1] : 0;
        u32 b2 = n > 2 ? data[i + 2] : 0;
        u32 t = (b0 << 16) | (b1 << 8) | b2;
        out.AppendChar(tbl[(t >> 18) & 63]);
        out.AppendChar(tbl[(t >> 12) & 63]);
        out.AppendChar(n > 1 ? tbl[(t >> 6) & 63] : '=');
        out.AppendChar(n > 2 ? tbl[t & 63] : '=');
        i += n >= 3 ? 3 : n;
    }
}

// ---------------------------------------------------------------------------
// CSV helpers (port of the Python csv module usage in autoContents)

// Parses one CSV line honoring quotes and doubled quotes inside quotes.
static void CsvParseLine(const char* line, Vec<char*>& fieldsOut) {
    StrBuilder cur;
    const char* p = line;
    bool inQuotes = false;
    while (*p) {
        char c = *p;
        if (inQuotes) {
            if (c == '"') {
                if (p[1] == '"') {
                    cur.AppendChar('"');
                    p += 2;
                    continue;
                }
                inQuotes = false;
                p++;
                continue;
            }
            cur.AppendChar(c);
            p++;
            continue;
        }
        if (c == '"' && cur.IsEmpty()) {
            inQuotes = true;
            p++;
            continue;
        }
        if (c == ',') {
            fieldsOut.Append(str::Dup(cur.Get() ? cur.Get() : ""));
            cur.Reset();
            p++;
            continue;
        }
        cur.AppendChar(c);
        p++;
    }
    fieldsOut.Append(str::Dup(cur.Get() ? cur.Get() : ""));
}

static void CsvWriteField(StrBuilder& out, const char* field) {
    if (!field) {
        field = "";
    }
    bool needQuote = false;
    for (const char* p = field; *p; p++) {
        if (*p == '"' || *p == ',' || *p == '\r' || *p == '\n') {
            needQuote = true;
            break;
        }
    }
    if (!needQuote) {
        out.Append(field);
        return;
    }
    out.AppendChar('"');
    for (const char* p = field; *p; p++) {
        if (*p == '"') {
            out.AppendChar('"');
        }
        out.AppendChar(*p);
    }
    out.AppendChar('"');
}

static void CsvWriteRow(StrBuilder& out, Vec<char*>& cols) {
    for (size_t i = 0; i < cols.size(); i++) {
        if (i > 0) {
            out.AppendChar(',');
        }
        CsvWriteField(out, cols[i]);
    }
}

// Port of validate_and_fix_csv_content (qwen_vl_extract.py for 2 columns,
// determine_toc_levels.py for 3): fixes fullwidth commas, merges extra
// columns into the title. Returns false (and leaves out empty) when the
// content cannot be repaired to wantCols columns.
static bool ValidateAndFixCsv(const char* content, int wantCols, StrBuilder& out) {
    out.Reset();
    Vec<char*> lines;
    SplitLines(content, lines);
    StrBuilder fixed;
    int rowCount = 0;
    for (size_t li = 0; li < lines.size(); li++) {
        char* line = lines[li];
        if (!line[0]) {
            continue;
        }
        Vec<char*> row;
        CsvParseLine(line, row);
        // Replace the trailing fullwidth comma U+FF0C (up to wantCols-1 times).
        for (int attempt = 0; attempt < wantCols - 1 && row.size() < (size_t)wantCols; attempt++) {
            const char* fw = nullptr;
            for (const char* p = line; *p;) {
                if ((u8)p[0] == 0xEF && (u8)p[1] == 0xBC && (u8)p[2] == 0x8C) {
                    fw = p;
                    p += 3;
                } else {
                    p++;
                }
            }
            if (!fw) {
                break;
            }
            size_t idx = (size_t)(fw - line);
            char* fixedLine = (char*)malloc(strlen(line) + 2);
            memcpy(fixedLine, line, idx);
            fixedLine[idx] = ',';
            strcpy(fixedLine + idx + 1, line + idx + 3);
            FreeCStrVec(row);
            row.Reset();
            CsvParseLine(fixedLine, row);
            str::Free(line);
            lines[li] = fixedLine;
            line = fixedLine;
        }
        if (row.size() > (size_t)wantCols) {
            // Merge the leading columns into the title.
            int mergeCount = (int)row.size() - wantCols + 1;
            StrBuilder title;
            for (int i = 0; i < mergeCount; i++) {
                if (i > 0) {
                    title.AppendChar(',');
                }
                title.Append(row[i]);
            }
            Vec<char*> merged;
            merged.Append(str::Dup(title.Get() ? title.Get() : ""));
            for (size_t i = mergeCount; i < row.size(); i++) {
                merged.Append(str::Dup(row[i]));
            }
            CsvWriteRow(fixed, merged);
            FreeCStrVec(merged);
            rowCount++;
        } else if (row.size() == (size_t)wantCols) {
            CsvWriteRow(fixed, row);
            rowCount++;
        } else {
            // Cannot be repaired: keep the original line; the final check fails.
            fixed.Append(line);
            rowCount++;
        }
        fixed.AppendChar('\n');
        FreeCStrVec(row);
    }
    FreeCStrVec(lines);
    // Final strict check: every line must have exactly wantCols columns.
    if (rowCount == 0) {
        return false;
    }
    Vec<char*> vlines;
    SplitLines(fixed.Get(), vlines);
    bool valid = vlines.size() > 0;
    for (size_t i = 0; i < vlines.size(); i++) {
        Vec<char*> row;
        CsvParseLine(vlines[i], row);
        if (row.size() != (size_t)wantCols) {
            valid = false;
            FreeCStrVec(row);
            break;
        }
        FreeCStrVec(row);
    }
    FreeCStrVec(vlines);
    if (!valid) {
        return false;
    }
    out.Append(fixed.Get() ? fixed.Get() : "");
    return true;
}

// ---------------------------------------------------------------------------
// roman numerals

static bool IsRomanToken(const char* s) {
    if (!s || !*s) {
        return false;
    }
    for (const char* p = s; *p; p++) {
        char c = (char)tolower((u8)*p);
        if (c != 'i' && c != 'v' && c != 'x' && c != 'l' && c != 'c' && c != 'd' && c != 'm') {
            return false;
        }
    }
    return strlen(s) <= 8;
}

static int RomanToInt(const char* s) {
    if (!IsRomanToken(s)) {
        return 0;
    }
    int vals[26] = {};
    vals['i' - 'a'] = 1;
    vals['v' - 'a'] = 5;
    vals['x' - 'a'] = 10;
    vals['l' - 'a'] = 50;
    vals['c' - 'a'] = 100;
    vals['d' - 'a'] = 500;
    vals['m' - 'a'] = 1000;
    int result = 0;
    int n = (int)strlen(s);
    for (int i = 0; i < n; i++) {
        int cur = vals[tolower((u8)s[i]) - 'a'];
        int next = i + 1 < n ? vals[tolower((u8)s[i + 1]) - 'a'] : 0;
        if (cur < next) {
            result -= cur;
        } else {
            result += cur;
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// prompts (verbatim ports from autoContents mainprogress/)

// qwen_vl_extract.py PROMPT_TEXT
static const char* kExtractPrompt =
    "# 任务目标\n"
    "分析提供的图片并提取目录信息。提取目标为每个目录项的标题和页码。\n"
    "\n"
    "# 输出格式要求\n"
    "1. 数据格式：仅输出CSV格式数据，禁止包含任何其他文本、代码解释或说明。\n"
    "2. 表头设置：必须包含表头 title,page_number。第一列为标题，第二列为页码。\n"
    "3. 分隔符：严格使用半角逗号 `,` 作为列分隔符，禁止使用全角逗号 `，`。\n"
    "\n"
    "# 内容提取规则\n"
    "1. 完整性与筛选：提取所有目录项目，不遗漏任何带有页码的条目。\n"
    "2. "
    "无页码条目判定：针对无页码条目需依据语义判断。若为篇、章级别的大标题，需推算并填补实际页码；若存在大量无页码的节、"
    "子节等次级标题，则直接忽略。\n"
    "3. 所见即所得：提取页面真实存在的信息，禁止自行推测或补充未显示的层级标题。例如页面以 4.5.1 "
    "开头，绝对禁止自行补充第四章及4.5节的标题，仅提取当前可见的内容。\n"
    "4. 忠于原文：严格保留原始标题的文字、数字形式及前缀，禁止增添、删减或修改。\n"
    "   * 示例：图上为 `第7章 总结`，则提取为 `第7章 总结`。\n"
    "   * 示例：图上为 `7章 不良案例`，则提取为 `7章 不良案例`，禁止修改为 `第7章 不良案例` 或 `7 不良案例`。\n"
    "   * 示例：图上为 `01 花草篇`，则提取为 `01 花草篇`，禁止修改为 `花草篇`。\n"
    "\n"
    "5. 语言保留：严格保留原始文字（包括繁体中文、英文等），禁止进行翻译。\n"
    "6. 符号替换：将带圈数字替换为常规阿拉伯数字。例如将 ① 替换为 1。\n"
    "7. 标题页码分割：准确区分紧跟在标题后的页码，避免将页码提取为标题的一部分。\n"
    "   * 示例：`1 绪论` 和 `第一章 绪论` 为合理标题。若出现 `第一章 绪论 / 1` 或 `第一章 绪论 1`，末尾的 `1` "
    "应当作为页码提取，标题仅为 `第一章 绪论`。\n"
    "8. 标点符号规范：包含中文的标题统一使用全角标点符号（如 `：` 和 `，`）。纯英文标题使用半角标点。\n"
    "9. 剔除连接符：去除标题与页码之间或标题内部用于排版的引导点 `·` "
    "或类似连接符。仅保留语义上确实作为省略号存在的符号。\n"
    "   * 正确示例：`Part25 写给想成为动画作者的人,122`\n"
    "   * 错误示例：`Part 25……写给想成为动画作者的人,122`\n"
    "10. 标题完整性：保持章节编号与标题内容的完整关联，禁止因排版结构将其拆分为独立的两行。\n"
    "    * 正确示例：`第1章 基本知识,1`\n"
    "    * 错误示例：`第1章,null` 换行 `基本知识,1`\n"
    "11. 排除页眉页脚：忽略分布在页面边缘的书籍名称、页眉或章节导航等非目录主体内容。\n"
    "12. 页面上出现xx篇、xx章时，尽管它们没有页码，但仍然应提取，它们必然是目录的一部分。\n"
    "\n"
    "# 页码处理规则\n"
    "1. "
    "缺失页码推算：若篇、章等高级别条目缺失页码，需根据其下级首个条目的页码或相邻条目进行合理推算并填补。禁止出现null的"
    "结果。\n"
    "   * 示例：第1篇的页码丢失，但第1篇第1章的页码为2，则推测第1篇的页码为2。\n"
    "\n"
    "# 空格与排版规则\n"
    "1. 纯中文目录：章节编号与具体标题之间仅保留1个半角空格。禁止在中文词组内部、数字与中文字符之间添加多余空格。\n"
    "   * 正确示例：`第1章 自动控制概述`；`第2章 超前滞后校正与PID校正`\n"
    "   * 错误示例：`第 1章 自动控制概述`；`第1 章自动控制概述`；`第2章 超前滞后校正与 PID校正`；`第2章 "
    "超前滞后校正与PID 校正`；`第 1 章 自动控制概述`\n"
    "2. 纯英文目录：遵循标准英语语法，单词、数字与符号之间保留常规空格。\n"
    "3. 混合目录：中文部分执行中文空格规则，英文部分执行英文空格规则。";

// qwen_vl_extract.py IMPORTANT_NOTE
static const char* kExtractImportantNote =
    "\n1. 忠于原文。图上为 `01 花草篇`，则提取为 `01 花草篇`，禁止修改为 `花草篇`。\n"
    "2. 页面上出现xx篇、xx章时，尽管它们没有页码，但仍然应提取，它们必然是目录的一部分。\n";

// determine_toc_levels.py PROMPT_TEXT
static const char* kLevelPrompt =
    "# 任务目标\n"
    "请分析提供的图片以及对应的目录数据（标题和页码），判断每个目录项所属的层级（level）。\n"
    "\n"
    "# 输出格式要求\n"
    "1. **必须且仅输出 CSV 格式数据**，包含表头 `title,page_number,level`。\n"
    "2. 严禁输出 Markdown 代码块标记（如 ```csv），严禁输出任何解释性文字。\n"
    "3. CSV 内容示例：\n"
    "title,page_number,level\n"
    "第一章 函数极限连续,1,1\n"
    "第一节 函数,1,2\n"
    "一、函数的概念,1,3\n"
    "\n"
    "# 层级判定规则\n"
    "1. "
    "视觉特征优先：应优先根据图片呈现的视觉特征（如颜色、字体、字号、缩进等）判定目录层级，也需要结合语义进行推断。\n"
    "2. 除非是第一页目录，否则第一行的标题未必是第一层级的，它可能隶属于上一页的其他章节。\n"
    "3. 规避层级判定错误：\n"
    "   - 规避误区：对于形式不同（例如字号不同、字体不同、缩进不同、颜色不同等）的标题，层级一定是不同的。\n"
    "   - 尊重常识：`篇`和`部分`一般是最高级；其次为`章`；然后是`节`等。\n"
    "   - 节与子节的层级关系：例如 2.4（节）与 2.4.1（子节）绝对不可处于同一目录层级，子节的层级必须比节低一级。\n"
    "4. "
    "思考题、练习题等，应该是作为`章`的下一级，而不应该与`章`"
    "处于在同一层级。但切记不要直接删掉这些页码为null的标题，这违背了第一条注意事项的要求。\n"
    "\n"
    "# 注意事项：\n"
    "- 输出的 CSV 行数应与输入的 CSV "
    "数据的标题数量严格一致，严禁省略。切记不要直接删掉这些页码为null的标题，你应该推断它们，而不是删除它们。\n"
    "- level 列必须是整数。\n"
    "- 部分情况下，识别的标题末尾会附带一个页码，如果遇到这种情况，请去掉那个页码。\n"
    "- 部分情况下页码会出现null，此时请进行简要推断，例如将其与它附近的页码设置为一致。\n"
    "- 一般来说，前言、推荐序、致谢、参考文献等，应该是第一层级。\n";

// pdf_metadata_extractor.py fetch_toc_from_image prompt (f-string port)
static TempStr BuildDetectPromptTemp(int startPage) {
    return str::FormatTemp(
        "这是一张由几个连续的 PDF 页面横向拼接而成的图片。每张图片下方标注了它的物理页码（例如 PDFNumber "
        "%d）。\n"
        "请找出这几页中，属于目录的起始页码和结束页码。\n"
        "\n"
        "【目录的严格定义】：一页中必须存在多个“标题 - "
        "页码”对。如果某一页没有这个特征（例如纯文本正文、封面、版权页、序言），则它绝对"
        "不是目录。只要不存在页码，那这页绝对不是目录。相对应地，如果一页有这个特征，那么它必然是目录。\n"
        "\n"
        "【注意】\n"
        "1. 应以下方的 PDFNumber 作为页码。例如 PDFNumber 为 2-3 的图片是目录，那么起始页码就是 2，结束页码就是 3。\n"
        "2. 注意空白页不可以算作目录的一部分，空白页一定不是目录！\n"
        "3. 例如 PDFNumber 为 16 的图片是空白，为 17-18 的图片是目录，那么起始页码就是 17（切记，不是 "
        "16！），结束页码就是 "
        "18。请切记不要把空白页面识别为目录的一部分，这会导致后续程序出现严重错误！！！\n"
        "\n"
        "【输出要求】：\n"
        "1. 仅输出 JSON 格式，不要包含任何 markdown 标记（如 ```json ）、解释或额外文本。\n"
        "2. 如果这几页中存在目录，输出格式为：{\"toc_start\": 起始页码，\"toc_end\": 结束页码}\n"
        "3. 如果这几页中没有任何一页是目录，输出格式为：{\"toc_start\": null, \"toc_end\": null}",
        startPage);
}

// pdf_metadata_extractor.py resolve_conflict prompt
static TempStr BuildSingleVotePromptTemp(int pageNo) {
    return str::FormatTemp(
        "这是一张 PDF 页面的图片，物理页码为 %d。\n"
        "请判断这一页是否是目录。目录的严格定义为：这张图中是否能提取出多个标题 - 页码对。\n"
        "【输出要求】：\n"
        "1. 仅输出 JSON 格式，不要包含任何 markdown 标记。\n"
        "2. 如果是目录，输出：{\"is_toc\": true}\n"
        "3. 如果不是目录，输出：{\"is_toc\": false}",
        pageNo);
}

// pdf_metadata_extractor.py fetch_single_offset prompt
static TempStr BuildArabicOffsetPromptTemp(int pageNo) {
    return str::FormatTemp(
        "你是一个专业的文档页码识别专家。你的任务是识别图片中页面底部或顶部标注的实际印刷页码，并计算正文偏移量。\n"
        "计算公式：正文偏移量 = PDF 物理页码 - 印刷页码。\n"
        "\n"
        "当前图片的 PDF 物理页码是：%d\n"
        "\n"
        "【示例 1】\n"
        "物理页码：25\n"
        "图片中底部写着：\"10\"\n"
        "计算：25 - 10 = 15\n"
        "输出：15\n"
        "\n"
        "【示例 2】\n"
        "物理页码：12\n"
        "图片中顶部写着：\"- 2 -\"\n"
        "计算：12 - 2 = 10\n"
        "输出：10\n"
        "\n"
        "【示例 3】\n"
        "物理页码：100\n"
        "图片中没有明确的阿拉伯数字页码\n"
        "输出：Error\n"
        "\n"
        "【错误示例】\n"
        "物理页码：25\n"
        "图片中顶部写着：\"20\"\n"
        "计算：25 - 20 = -5\n"
        "输出：-5\n"
        "\n"
        "请仔细观察图片，找到印刷页码，并严格按照上述格式，仅输出计算后的正文偏移量数字。不要输出任何解释。",
        pageNo);
}

// pdf_metadata_extractor.py fetch_single_roman_offset prompt
static TempStr BuildRomanOffsetPromptTemp(int pageNo) {
    return str::FormatTemp(
        "你是一个专业的文档页码识别专家。你的任务是识别图片中页面底部或顶部标注的罗马数字页码，并计算前言偏移量。\n"
        "计算公式：前言偏移量 = PDF 物理页码 - 罗马数字页码对应的阿拉伯数字值。\n"
        "\n"
        "当前图片的 PDF 物理页码是：%d\n"
        "\n"
        "【示例 1】\n"
        "物理页码：5\n"
        "图片中底部写着：\"iii\"\n"
        "罗马数字 iii = 3\n"
        "计算：5 - 3 = 2\n"
        "输出：2\n"
        "\n"
        "【示例 2】\n"
        "物理页码：10\n"
        "图片中底部写着：\"xv\"\n"
        "罗马数字 xv = 15\n"
        "计算：10 - 15 = -5\n"
        "输出：-5\n"
        "\n"
        "【示例 3】\n"
        "物理页码：3\n"
        "图片中没有罗马数字页码（可能是阿拉伯数字或无页码）\n"
        "输出：Error\n"
        "\n"
        "请仔细观察图片，找到罗马数字页码，并严格按照上述格式，仅输出计算后的前言偏移量数字。不要输出任何解释。",
        pageNo);
}

// ---------------------------------------------------------------------------
// HTTP POST (WinINet; utils/HttpUtil's HttpPost discards the response body)

struct HttpPostResult {
    DWORD status = 0;
    StrBuilder body;
};

static bool HttpJsonRaw(const char* url, const char* apiKey, const char* method, const char* body, int timeoutMs,
                        HttpPostResult& res, char** errOut) {
    *errOut = nullptr;
    HINTERNET hInet = nullptr;
    HINTERNET hConn = nullptr;
    HINTERNET hReq = nullptr;
    bool ok = false;

    char host[256] = {};
    char path[2048] = {};
    char extra[1024] = {};
    char scheme[16] = {};
    URL_COMPONENTSA uc = {};
    uc.dwStructSize = sizeof(uc);
    uc.lpszScheme = scheme;
    uc.dwSchemeLength = 15;
    uc.lpszHostName = host;
    uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2047;
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = 1023;
    if (!InternetCrackUrlA(url, 0, 0, &uc)) {
        *errOut = str::Format("Invalid API base URL '%s'.", url ? url : "");
        return false;
    }
    bool isHttps = uc.nScheme == INTERNET_SCHEME_HTTPS;
    INTERNET_PORT port = uc.nPort ? uc.nPort : (isHttps ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT);

    hInet = InternetOpenA("SumatraPDF-Plus/AiTocApi", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!hInet) {
        *errOut = str::Dup("Cannot initialize WinINet.");
        return false;
    }
    // Raise WinINet's low default per-host connection cap: the job runner uses
    // 4 parallel vision calls, and with the default cap the queued requests
    // can stall behind a long-running reasoning-model reply until timeout.
    DWORD maxConns = 8;
    InternetSetOptionA(hInet, INTERNET_OPTION_MAX_CONNS_PER_SERVER, &maxConns, sizeof(maxConns));
    InternetSetOptionA(hInet, INTERNET_OPTION_MAX_CONNS_PER_1_0_SERVER, &maxConns, sizeof(maxConns));
    hConn = InternetConnectA(hInet, host, port, nullptr, nullptr, INTERNET_SERVICE_HTTP, 0, 0);
    if (!hConn) {
        *errOut = str::Format("Cannot connect to '%s'.", host);
        goto Exit;
    }
    {
        DWORD flags = INTERNET_FLAG_NO_UI | INTERNET_FLAG_RELOAD;
        if (isHttps) {
            flags |= INTERNET_FLAG_SECURE;
        }
        hReq = HttpOpenRequestA(hConn, method, path, nullptr, nullptr, nullptr, flags, 0);
        if (!hReq) {
            *errOut = str::Dup("Cannot create the HTTP request.");
            goto Exit;
        }
        DWORD connectTimeout = 30 * 1000;
        DWORD recvTimeout = (DWORD)timeoutMs;
        InternetSetOptionA(hReq, INTERNET_OPTION_CONNECT_TIMEOUT, &connectTimeout, sizeof(connectTimeout));
        InternetSetOptionA(hReq, INTERNET_OPTION_SEND_TIMEOUT, &recvTimeout, sizeof(recvTimeout));
        InternetSetOptionA(hReq, INTERNET_OPTION_RECEIVE_TIMEOUT, &recvTimeout, sizeof(recvTimeout));
        char headers[1200] = {};
        _snprintf_s(headers, _TRUNCATE, "Content-Type: application/json\r\nAuthorization: Bearer %s\r\n",
                    apiKey ? apiKey : "");
        if (!HttpSendRequestA(hReq, headers, (DWORD)-1, (LPVOID)body, body ? (DWORD)strlen(body) : 0)) {
            DWORD gle = GetLastError();
            *errOut = str::Format("Network error talking to the API (WinINet error %u).", (unsigned)gle);
            goto Exit;
        }
        DWORD status = 0;
        DWORD len = sizeof(status);
        HttpQueryInfoA(hReq, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &len, nullptr);
        res.status = status;
        for (;;) {
            char buf[8192];
            DWORD n = 0;
            if (!InternetReadFile(hReq, buf, sizeof(buf), &n)) {
                *errOut = str::Dup("Failed reading the API response.");
                goto Exit;
            }
            if (n == 0) {
                break;
            }
            res.body.Append(buf, n);
        }
        ok = true;
    }
Exit:
    if (hReq) {
        InternetCloseHandle(hReq);
    }
    if (hConn) {
        InternetCloseHandle(hConn);
    }
    if (hInet) {
        InternetCloseHandle(hInet);
    }
    return ok;
}

// ---------------------------------------------------------------------------
// chat completions plumbing

struct ChatPart {
    const char* text = nullptr;
    const char* b64 = nullptr; // non-null: a base64 JPEG (no data: prefix)
};

static TempStr ChatCompletionsUrlTemp(const char* baseUrl) {
    char* trimmed = str::Dup(baseUrl ? baseUrl : "");
    size_t n = strlen(trimmed);
    while (n > 0 && trimmed[n - 1] == '/') {
        trimmed[--n] = 0;
    }
    TempStr url = str::FormatTemp("%s/chat/completions", trimmed);
    str::Free(trimmed);
    return url;
}

static void BuildChatBody(const AiTocApiConfig& cfg, const ChatPart* parts, int nParts, bool disableThinking,
                          StrBuilder& out) {
    out.Append("{\"model\": ");
    AppendJsonEscaped(out, cfg.model ? cfg.model : "");
    out.Append(", \"messages\": [{\"role\": \"user\", \"content\": [");
    for (int i = 0; i < nParts; i++) {
        if (i > 0) {
            out.Append(", ");
        }
        const ChatPart& p = parts[i];
        if (p.b64) {
            out.Append("{\"type\": \"image_url\", \"image_url\": {\"url\": \"data:image/jpeg;base64,");
            out.Append(p.b64);
            out.Append("\"}}");
        } else {
            out.Append("{\"type\": \"text\", \"text\": ");
            AppendJsonEscaped(out, p.text);
            out.AppendChar('}');
        }
    }
    out.Append("]}], \"temperature\": 0");
    if (disableThinking) {
        // Match autoContents: request no reasoning phase on every provider.
        out.Append(", \"enable_thinking\": false");
    }
    out.AppendChar('}');
}

struct ChatResponseVisitor : json::ValueVisitor {
    StrBuilder content;
    StrBuilder errMsg;

    bool Visit(const char* path, const char* value, json::Type type) override {
        if (type != json::Type::String) {
            return true;
        }
        if (str::Eq(path, "/choices[0]/message/content")) {
            content.Append(value);
        } else if (str::Eq(path, "/error/message")) {
            errMsg.Append(value);
        }
        return true;
    }
};

// Some OpenAI-compatible providers reject enable_thinking. Cache explicit
// rejections per endpoint and model so later calls avoid an extra HTTP round trip.
struct UnsupportedThinkingConfig {
    char* baseUrl = nullptr;
    char* model = nullptr;
    UnsupportedThinkingConfig* next = nullptr;
};
static SRWLOCK gUnsupportedThinkingLock = SRWLOCK_INIT;
static UnsupportedThinkingConfig* gUnsupportedThinkingConfigs = nullptr;

static bool IsThinkingUnsupported(const AiTocApiConfig& cfg) {
    AcquireSRWLockShared(&gUnsupportedThinkingLock);
    bool found = false;
    for (auto* item = gUnsupportedThinkingConfigs; item; item = item->next) {
        if (str::Eq(item->baseUrl, cfg.baseUrl) && str::Eq(item->model, cfg.model)) {
            found = true;
            break;
        }
    }
    ReleaseSRWLockShared(&gUnsupportedThinkingLock);
    return found;
}

static void RememberThinkingUnsupported(const AiTocApiConfig& cfg) {
    AcquireSRWLockExclusive(&gUnsupportedThinkingLock);
    for (auto* item = gUnsupportedThinkingConfigs; item; item = item->next) {
        if (str::Eq(item->baseUrl, cfg.baseUrl) && str::Eq(item->model, cfg.model)) {
            ReleaseSRWLockExclusive(&gUnsupportedThinkingLock);
            return;
        }
    }
    auto* item = (UnsupportedThinkingConfig*)malloc(sizeof(UnsupportedThinkingConfig));
    if (item) {
        item->baseUrl = str::Dup(cfg.baseUrl);
        item->model = str::Dup(cfg.model);
        if (item->baseUrl && item->model) {
            item->next = gUnsupportedThinkingConfigs;
            gUnsupportedThinkingConfigs = item;
        } else {
            str::Free(item->baseUrl);
            str::Free(item->model);
            free(item);
        }
    }
    ReleaseSRWLockExclusive(&gUnsupportedThinkingLock);
}

static void StripCodeFencesInPlace(char* s) {
    TrimInPlace(s);
    if (str::StartsWith(s, "```csv")) {
        memmove(s, s + 6, strlen(s + 6) + 1);
    } else if (str::StartsWith(s, "```")) {
        memmove(s, s + 3, strlen(s + 3) + 1);
    }
    size_t n = strlen(s);
    if (n >= 3 && str::Eq(s + n - 3, "```")) {
        s[n - 3] = 0;
    }
    TrimInPlace(s);
}

bool AiTocApiIsConfigured(const AiTocApiConfig& cfg) {
    return !str::IsEmptyOrWhiteSpace(cfg.baseUrl) && !str::IsEmptyOrWhiteSpace(cfg.key) &&
           !str::IsEmptyOrWhiteSpace(cfg.model);
}

// A single vision request. On failure *errOut is set (str::Format/Dup).
static bool VisionCallOnce(const AiTocApiConfig& cfg, const ChatPart* parts, int nParts, int timeoutMs,
                           StrBuilder& contentOut, char** errOut) {
    *errOut = nullptr;
    if (!AiTocApiIsConfigured(cfg)) {
        *errOut = str::Dup("The AI TOC API is not configured (set base URL, key and model in Settings).");
        return false;
    }
    TempStr url = ChatCompletionsUrlTemp(cfg.baseUrl);
    bool disableThinking = !IsThinkingUnsupported(cfg);
    HttpPostResult res;
    for (;;) {
        StrBuilder body;
        BuildChatBody(cfg, parts, nParts, disableThinking, body);
        if (!HttpJsonRaw(url, cfg.key, "POST", body.Get(), timeoutMs, res, errOut)) {
            return false;
        }
        const char* reply = res.body.Get();
        if (disableThinking && (res.status == 400 || res.status == 422) && reply &&
            str::FindI(reply, "enable_thinking")) {
            RememberThinkingUnsupported(cfg);
            disableThinking = false;
            res.status = 0;
            res.body.Reset();
            continue;
        }
        break;
    }
    const char* respText = res.body.Get();
    if (!respText || !*respText) {
        *errOut = str::Format("The API returned an empty response (HTTP %u).", (unsigned)res.status);
        return false;
    }
    ChatResponseVisitor v;
    json::Parse(respText, &v);
    if (res.status != 200) {
        const char* detail = v.errMsg.Get();
        if (!detail || !*detail) {
            detail = respText;
        }
        size_t detailLen = strlen(detail);
        if (detailLen > 300) {
            detailLen = 300;
        }
        char* truncated = DupRange(detail, detailLen);
        *errOut = str::Format("API error (HTTP %u): %s", (unsigned)res.status, truncated ? truncated : "");
        str::Free(truncated);
        return false;
    }
    const char* content = v.content.Get();
    if (!content || !*content) {
        const char* detail = v.errMsg.Get();
        *errOut = str::Format("The API returned no text content%s%s.", detail && *detail ? ": " : "",
                              detail && *detail ? detail : "");
        return false;
    }
    contentOut.Append(content);
    return true;
}

// Validation hook for VisionCallWithRetry: rejects a reply to force a retry.
typedef bool (*ApiReplyValidator)(const char* content, void* ctx, char** errOut);

struct ApiCallStats {
    int attempts = 0;
    ULONGLONG elapsedMs = 0;
};

static bool VisionCallWithRetry(const AiTocApiConfig& cfg, const ChatPart* parts, int nParts,
                                ApiReplyValidator validator, void* validatorCtx, AiTocApiCancelFn canceled,
                                void* cancelCtx, StrBuilder& contentOut, char** errOut, ApiCallStats* stats = nullptr) {
    *errOut = nullptr;
    ULONGLONG started = GetTickCount64();
    if (stats) {
        *stats = {};
    }
    defer {
        if (stats) {
            stats->elapsedMs = GetTickCount64() - started;
        }
    };
    for (int attempt = 0; attempt < kApiMaxAttempts; attempt++) {
        if (stats) {
            stats->attempts = attempt + 1;
        }
        if (canceled && canceled(cancelCtx)) {
            *errOut = str::Dup("Canceled.");
            return false;
        }
        char* err = nullptr;
        StrBuilder raw;
        if (VisionCallOnce(cfg, parts, nParts, kApiRequestTimeoutMs, raw, &err)) {
            char* content = str::Dup(raw.Get() ? raw.Get() : "");
            StripCodeFencesInPlace(content);
            if (!validator) {
                contentOut.Append(content);
                str::Free(content);
                return true;
            }
            char* vErr = nullptr;
            if (validator(content, validatorCtx, &vErr)) {
                contentOut.Append(content);
                str::Free(content);
                return true;
            }
            logf("AiTocApi: reply rejected on attempt %d: %s\n", attempt + 1, vErr ? vErr : "");
            str::Free(err);
            err = vErr ? vErr : str::Dup("The model reply did not pass validation.");
            str::Free(content);
        }
        if (attempt == kApiMaxAttempts - 1) {
            *errOut = err ? err : str::Dup("The API request failed.");
            return false;
        }
        logf("AiTocApi: attempt %d failed: %s\n", attempt + 1, err ? err : "");
        str::Free(err);
        int waitMs = 1000 << attempt;
        for (int t = 0; t < waitMs; t += 100) {
            if (canceled && canceled(cancelCtx)) {
                *errOut = str::Dup("Canceled.");
                return false;
            }
            Sleep(100);
        }
    }
    *errOut = str::Dup("The API request failed.");
    return false;
}

// ---------------------------------------------------------------------------
// bounded-parallel job runner

struct ApiJobQueue {
    int count = 0;
    void (*fn)(void* ctx, int idx) = nullptr;
    void* ctx = nullptr;
    volatile LONG next = 0;

    static void Trampoline(ApiJobQueue* q) {
        for (;;) {
            LONG i = InterlockedIncrement(&q->next) - 1;
            if (i >= q->count) {
                return;
            }
            q->fn(q->ctx, (int)i);
        }
    }
};

static void RunApiJobs(ApiJobQueue& q, int parallel) {
    if (q.count < 1) {
        return;
    }
    int n = q.count < parallel ? q.count : parallel;
    if (n > 8) {
        n = 8;
    }
    HANDLE handles[8] = {};
    for (int i = 0; i < n; i++) {
        handles[i] = StartThread(MkFunc0<ApiJobQueue>(ApiJobQueue::Trampoline, &q), "AiTocApiJob");
    }
    for (int i = 0; i < n; i++) {
        if (handles[i]) {
            WaitForSingleObject(handles[i], INFINITE);
            CloseHandle(handles[i]);
        }
    }
}

// ---------------------------------------------------------------------------
// page rendering + JPEG encoding

static RenderedBitmap* RenderPageForApi(EngineBase* engine, int pageNo, float longEdge) {
    RectF box = engine->PageMediabox(pageNo);
    if (box.IsEmpty()) {
        return nullptr;
    }
    float longest = box.dx > box.dy ? box.dx : box.dy;
    if (longest <= 0) {
        return nullptr;
    }
    float zoom = longEdge / longest;
    RenderPageArgs args(pageNo, zoom, 0, nullptr, RenderTarget::Export);
    return engine->RenderPage(args);
}

static bool EncodeHBitmapAsJpeg(HBITMAP hbmp, int quality, StrBuilder& out, char** errOut) {
    *errOut = nullptr;
    ScopedComPtr<IWICImagingFactory> factory;
    ScopedComPtr<IWICBitmap> wicBitmap;
    ScopedComPtr<IStream> stream;
    ScopedComPtr<IWICBitmapEncoder> encoder;
    ScopedComPtr<IWICBitmapFrameEncode> frame;
    ScopedComPtr<IPropertyBag2> props;
    bool ok = false;

    if (!factory.Create(CLSID_WICImagingFactory)) {
        *errOut = str::Dup("WIC is unavailable.");
        return false;
    }
    if (FAILED(factory->CreateBitmapFromHBITMAP(hbmp, nullptr, WICBitmapUsePremultipliedAlpha, &wicBitmap))) {
        *errOut = str::Dup("Cannot create a WIC bitmap.");
        return false;
    }
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) {
        *errOut = str::Dup("Cannot create a memory stream.");
        return false;
    }
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder))) {
        *errOut = str::Dup("Cannot create the JPEG encoder.");
        return false;
    }
    if (FAILED(encoder->Initialize(stream, WICBitmapEncoderNoCache))) {
        *errOut = str::Dup("Cannot initialize the JPEG encoder.");
        return false;
    }
    if (FAILED(encoder->CreateNewFrame(&frame, &props))) {
        *errOut = str::Dup("Cannot create a JPEG frame.");
        return false;
    }
    if (props) {
        PROPBAG2 opt = {};
        opt.pstrName = (LPOLESTR)L"ImageQuality";
        VARIANT var;
        VariantInit(&var);
        var.vt = VT_R4;
        var.fltVal = (float)quality / 100.0f;
        props->Write(1, &opt, &var);
        VariantClear(&var);
    }
    if (FAILED(frame->Initialize(props))) {
        *errOut = str::Dup("Cannot initialize the JPEG frame.");
        return false;
    }
    {
        UINT w = 0;
        UINT h = 0;
        wicBitmap->GetSize(&w, &h);
        if (FAILED(frame->SetSize(w, h))) {
            *errOut = str::Dup("Cannot set the JPEG frame size.");
            return false;
        }
        WICPixelFormatGUID pixFmt = GUID_WICPixelFormat24bppBGR;
        if (FAILED(frame->SetPixelFormat(&pixFmt))) {
            *errOut = str::Dup("Cannot set the JPEG pixel format.");
            return false;
        }
        if (FAILED(frame->WriteSource(wicBitmap, nullptr))) {
            *errOut = str::Dup("Cannot write the JPEG frame.");
            return false;
        }
    }
    if (FAILED(frame->Commit()) || FAILED(encoder->Commit())) {
        *errOut = str::Dup("Cannot finish the JPEG encoding.");
        return false;
    }
    {
        LARGE_INTEGER zero = {};
        stream->Seek(zero, STREAM_SEEK_SET, nullptr);
        for (;;) {
            char buf[8192];
            ULONG n = 0;
            if (FAILED(stream->Read(buf, sizeof(buf), &n)) || n == 0) {
                break;
            }
            out.Append(buf, n);
        }
        ok = out.size() > 0;
        if (!ok) {
            *errOut = str::Dup("JPEG encoding produced no data.");
        }
    }
    return ok;
}

// Renders a page and produces the base64 JPEG used in the API request bodies.
static bool RenderPageJpegBase64(EngineBase* engine, int pageNo, float longEdge, int quality, char** b64Out,
                                 StrBuilder* bytesOut, char** errOut) {
    *b64Out = nullptr;
    *errOut = nullptr;
    RenderedBitmap* bmp = RenderPageForApi(engine, pageNo, longEdge);
    if (!bmp) {
        *errOut = str::Format("Cannot render page %d.", pageNo);
        return false;
    }
    StrBuilder jpeg;
    bool ok = EncodeHBitmapAsJpeg(bmp->GetBitmap(), quality, jpeg, errOut);
    delete bmp;
    if (!ok) {
        return false;
    }
    StrBuilder b64;
    Base64Encode((const u8*)jpeg.Get(), jpeg.size(), b64);
    *b64Out = str::Dup(b64.Get() ? b64.Get() : "");
    if (bytesOut) {
        bytesOut->Append(jpeg.Get(), jpeg.size());
    }
    return *b64Out != nullptr;
}

// Composes several consecutive pages side by side with a "PDFNumber N" label
// under each one (port of autoContents create_concat_image_b64).
static bool RenderConcatPagesJpegBase64(EngineBase* engine, int startPage, int endPage, char** b64Out,
                                        StrBuilder* bytesOut, int* nPagesOut, char** errOut) {
    *b64Out = nullptr;
    *errOut = nullptr;
    *nPagesOut = 0;
    int n = endPage - startPage + 1;
    if (n < 1) {
        *errOut = str::Dup("Empty page window.");
        return false;
    }
    // Keep each RenderedBitmap alive until the composed image is finished.
    // GetBitmap() is borrowed; deleting its owner invalidates the HBITMAP.
    Vec<RenderedBitmap*> bitmaps;
    Vec<int> pageNos;
    Vec<int> widths;
    Vec<int> heights;
    int maxH = 0;
    int totalW = 0;
    for (int p = startPage; p <= endPage; p++) {
        RenderedBitmap* bmp = RenderPageForApi(engine, p, kApiImageLongEdge);
        if (!bmp) {
            continue;
        }
        Size sz = GetBitmapSize(bmp->GetBitmap());
        if (sz.dx <= 0 || sz.dy <= 0) {
            delete bmp;
            continue;
        }
        bitmaps.Append(bmp);
        pageNos.Append(p);
        widths.Append(sz.dx);
        heights.Append(sz.dy);
        totalW += sz.dx;
        if (sz.dy > maxH) {
            maxH = sz.dy;
        }
    }
    if (bitmaps.Size() == 0) {
        *errOut = str::Dup("Cannot render the pages for TOC detection.");
        return false;
    }
    int newH = (int)(maxH * 1.2f);
    if (newH < maxH + 1) {
        newH = maxH + 1;
    }
    bool ok = false;
    {
        Gdiplus::Bitmap canvas(totalW, newH, PixelFormat32bppARGB);
        Gdiplus::Graphics g(&canvas);
        g.Clear(Color(255, 255, 255));
        int fontPx = (int)(maxH * 0.05f);
        if (fontPx < 40) {
            fontPx = 40;
        }
        Gdiplus::Font font(L"Segoe UI", (Gdiplus::REAL)fontPx, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        SolidBrush brush(Color(255, 0, 0, 0));
        StringFormat fmt;
        fmt.SetAlignment(StringAlignmentCenter);
        fmt.SetLineAlignment(StringAlignmentCenter);
        int x = 0;
        for (size_t i = 0; i < bitmaps.size(); i++) {
            Bitmap pageBmp(bitmaps[i]->GetBitmap(), nullptr);
            if (pageBmp.GetLastStatus() == Ok) {
                g.DrawImage(&pageBmp, x, 0, widths[i], heights[i]);
            }
            char label[64] = {};
            _snprintf_s(label, _TRUNCATE, "PDFNumber %d", pageNos[i]);
            Gdiplus::RectF rect((Gdiplus::REAL)x, (Gdiplus::REAL)maxH, (Gdiplus::REAL)widths[i],
                                (Gdiplus::REAL)(newH - maxH));
            WCHAR labelW[64] = {};
            MultiByteToWideChar(CP_ACP, 0, label, -1, labelW, 64);
            g.DrawString(labelW, -1, &font, rect, &fmt, &brush);
            x += widths[i];
        }
        HBITMAP composed = nullptr;
        if (canvas.GetHBITMAP(Color(255, 255, 255), &composed) == Ok && composed) {
            StrBuilder jpeg;
            char* encErr = nullptr;
            if (EncodeHBitmapAsJpeg(composed, kApiJpegQuality, jpeg, &encErr)) {
                StrBuilder b64;
                Base64Encode((const u8*)jpeg.Get(), jpeg.size(), b64);
                *b64Out = str::Dup(b64.Get() ? b64.Get() : "");
                if (bytesOut) {
                    bytesOut->Append(jpeg.Get(), jpeg.size());
                }
                *nPagesOut = (int)bitmaps.size();
                ok = *b64Out != nullptr;
            } else {
                *errOut = encErr ? encErr : str::Dup("Cannot encode the composed image.");
            }
            DeleteObject(composed);
        } else {
            *errOut = str::Dup("Cannot compose the TOC detection image.");
        }
    }
    for (size_t i = 0; i < bitmaps.size(); i++) {
        delete bitmaps[i];
    }
    return ok;
}

// ---------------------------------------------------------------------------
// debug session dir (mirrors the autoContents data dir; helps diagnosis)

struct ApiDebugDir {
    char* dir = nullptr;

    ~ApiDebugDir() { str::Free(dir); }

    void Init() {
        TempStr root = path::JoinTemp(GetTempDirTemp(), "SumatraPDF-Plus\\AI-TOC");
        TempStr d = path::JoinTemp(root, str::FormatTemp("api-%u", (unsigned)GetTickCount()));
        if (dir::CreateAll(d)) {
            dir = str::Dup(d);
        }
        logf("AiTocApi: debug dir %s\n", dir ? dir : "(none)");
    }

    void SaveBytes(const char* name, const char* data, size_t len) const {
        if (!dir || !data || len == 0) {
            return;
        }
        TempStr path = path::JoinTemp(dir, name);
        file::WriteFile(path, ByteSlice((const u8*)data, len));
    }

    void SaveText(const char* name, const char* text) const {
        if (!text) {
            return;
        }
        SaveBytes(name, text, strlen(text));
    }
};

static void SaveApiCallStats(const ApiDebugDir* dbg, const char* name, const ApiCallStats& stats, bool success) {
    if (!dbg) {
        return;
    }
    dbg->SaveText(name, str::FormatTemp("elapsed_ms=%llu\nattempts=%d\nsuccess=%d\n",
                                        (unsigned long long)stats.elapsedMs, stats.attempts, success ? 1 : 0));
}

// ---------------------------------------------------------------------------
// per-page data

struct ApiTocEntry {
    char* title = nullptr;
    int level = 1;
    int pageNum = 0;           // arabic printed page (0 = none)
    char* pageLabel = nullptr; // roman / letter label (nullptr = none)
    bool hasPdfPage = false;
    int pdfPage = 0;
};

struct CsvFixCtx {
    StrBuilder fixed;
};

struct ApiPageJob {
    int pageNo = 0;
    char* b64 = nullptr;
    char* csvExtract = nullptr; // stage A output (2-col CSV, nulls filled)
    char* levelsCsv = nullptr;  // stage B raw reply (few-shot input)
    Vec<ApiTocEntry> entries;   // stage B parsed entries
    CsvFixCtx fix;
    bool extractOk = false;
    bool levelsOk = false;
    char* err = nullptr;
    int maxLevel = 0;
};

static void FreeApiPageJob(ApiPageJob* j) {
    if (!j) {
        return;
    }
    str::Free(j->b64);
    str::Free(j->csvExtract);
    str::Free(j->levelsCsv);
    str::Free(j->err);
    for (size_t i = 0; i < j->entries.size(); i++) {
        str::Free(j->entries[i].title);
        str::Free(j->entries[i].pageLabel);
    }
    j->entries.Reset();
    delete j;
}

// Normalizes a printed page token from a model reply: digits become an
// arabic page, roman/letter labels are kept as labels, "12-13" style ranges
// fall back to their first number.
static void NormalizePageToken(const char* raw, int* numOut, char** labelOut) {
    *numOut = 0;
    *labelOut = nullptr;
    if (!raw) {
        return;
    }
    char* s = str::Dup(raw);
    TrimInPlace(s);
    if (!s[0]) {
        str::Free(s);
        return;
    }
    bool allDigits = true;
    for (char* p = s; *p; p++) {
        if (*p < '0' || *p > '9') {
            allDigits = false;
            break;
        }
    }
    if (allDigits) {
        *numOut = atoi(s);
        str::Free(s);
        return;
    }
    if (IsRomanToken(s)) {
        for (char* p = s; *p; p++) {
            *p = (char)tolower((u8)*p);
        }
        *labelOut = s;
        return;
    }
    {
        size_t letters = 0;
        while (s[letters] && isalpha((u8)s[letters])) {
            letters++;
        }
        if (letters >= 1 && letters <= 3) {
            bool onlyDigits = s[letters] != 0;
            size_t digits = 0;
            for (const char* p = s + letters; *p; p++) {
                if (!isdigit((u8)*p)) {
                    onlyDigits = false;
                    break;
                }
                digits++;
            }
            if (onlyDigits && digits >= 1 && digits <= 4) {
                *labelOut = s;
                return;
            }
        }
    }
    {
        const char* p = s;
        while (*p && !isdigit((u8)*p)) {
            p++;
        }
        if (*p) {
            int v = 0;
            int guard = 0;
            while (*p && isdigit((u8)*p) && guard < 9) {
                v = v * 10 + (*p - '0');
                p++;
                guard++;
            }
            *numOut = v;
        }
    }
    str::Free(s);
}

// ---------------------------------------------------------------------------
// stage A: per-page title/page extraction (qwen_vl_extract.py)

static bool ValidateCsv2Reply(const char* content, void* ctx, char** errOut) {
    *errOut = nullptr;
    auto* fix = (CsvFixCtx*)ctx;
    if (!ValidateAndFixCsv(content, 2, fix->fixed)) {
        *errOut = str::Dup("CSV format validation failed (need 2 columns).");
        return false;
    }
    return true;
}

struct Row2 {
    char* title = nullptr;
    char* page = nullptr;
};

static void ParseCsv2Rows(const char* csv, Vec<Row2>& rowsOut) {
    Vec<char*> lines;
    SplitLines(csv, lines);
    size_t firstDataLine = 0;
    if (lines.size() > 0) {
        // Skip the header only when it actually is one; a header-less reply
        // must not lose its first data row.
        Vec<char*> hdr;
        CsvParseLine(lines[0], hdr);
        if (hdr.size() > 0) {
            char* c0 = hdr[0];
            for (char* p = c0; *p; p++) {
                *p = (char)tolower((u8)*p);
            }
            if (str::Eq(c0, "title") || (hdr.size() > 1 && str::EqI(hdr[1], "page_number"))) {
                firstDataLine = 1;
            }
        }
        FreeCStrVec(hdr);
    }
    for (size_t i = firstDataLine; i < lines.size(); i++) {
        if (!lines[i][0]) {
            continue;
        }
        Vec<char*> cols;
        CsvParseLine(lines[i], cols);
        if (cols.size() >= 2) {
            Row2 r;
            r.title = str::Dup(cols[0]);
            r.page = str::Dup(cols[1]);
            rowsOut.Append(r);
        } else if (cols.size() == 1 && cols[0][0]) {
            Row2 r;
            r.title = str::Dup(cols[0]);
            r.page = str::Dup("");
            rowsOut.Append(r);
        }
        FreeCStrVec(cols);
    }
    FreeCStrVec(lines);
}

// Port of fix_null_page_numbers: fill a missing page from the next row that
// has one, otherwise from the previous row.
static void FixNullPageNumbers(Vec<Row2>& rows) {
    auto isValid = [](const char* p) {
        if (!p) {
            return false;
        }
        return !str::EqI(p, "") && !str::EqI(p, "null") && !str::EqI(p, "none");
    };
    int n = (int)rows.size();
    for (int i = 0; i < n; i++) {
        if (isValid(rows[i].page)) {
            continue;
        }
        bool found = false;
        for (int j = i + 1; j < n; j++) {
            if (isValid(rows[j].page)) {
                str::ReplaceWithCopy(&rows[i].page, rows[j].page);
                found = true;
                break;
            }
        }
        if (found) {
            continue;
        }
        for (int k = i - 1; k >= 0; k--) {
            if (isValid(rows[k].page)) {
                str::ReplaceWithCopy(&rows[i].page, rows[k].page);
                break;
            }
        }
    }
}

static char* BuildCsv2Text(Vec<Row2>& rows) {
    StrBuilder out;
    out.Append("title,page_number\n");
    for (size_t i = 0; i < rows.size(); i++) {
        CsvWriteField(out, rows[i].title);
        out.AppendChar(',');
        CsvWriteField(out, rows[i].page);
        out.AppendChar('\n');
    }
    return out.StealData();
}

static void FreeCsv2Rows(Vec<Row2>& rows) {
    for (size_t i = 0; i < rows.size(); i++) {
        str::Free(rows[i].title);
        str::Free(rows[i].page);
    }
    rows.Reset();
}

struct StageJobCtx {
    const AiTocApiConfig* cfg = nullptr;
    Vec<ApiPageJob*>* jobs = nullptr;
    int offset = 0;
    AiTocApiCancelFn canceled = nullptr;
    void* cancelCtx = nullptr;
    const ApiDebugDir* dbg = nullptr;
    volatile LONG done = 0;
    AiTocApiProgressFn progress = nullptr;
    void* progressCtx = nullptr;
    AiTocApiStage stage = AiTocApiStage::Extract;
};

static void ExtractJobFn(void* ctx, int idx) {
    auto* jc = (StageJobCtx*)ctx;
    ApiPageJob* j = (*jc->jobs)[(size_t)(idx + jc->offset)];
    if (jc->canceled && jc->canceled(jc->cancelCtx)) {
        return;
    }
    ChatPart parts[5] = {};
    parts[0].text = "当前页图片（需处理）：";
    parts[1].text = kExtractPrompt;
    parts[2].b64 = j->b64;
    parts[3].text = kExtractPrompt;
    parts[4].text = kExtractImportantNote;
    StrBuilder reply;
    char* err = nullptr;
    ApiCallStats stats;
    if (VisionCallWithRetry(*jc->cfg, parts, 5, ValidateCsv2Reply, &j->fix, jc->canceled, jc->cancelCtx, reply, &err,
                            &stats)) {
        Vec<Row2> rows;
        ParseCsv2Rows(j->fix.fixed.Get(), rows);
        if (rows.size() > 0) {
            FixNullPageNumbers(rows);
            j->csvExtract = BuildCsv2Text(rows);
            j->extractOk = true;
            if (jc->dbg) {
                jc->dbg->SaveText(str::FormatTemp("page_%04d_extract.csv", j->pageNo), j->csvExtract);
            }
        } else {
            str::Free(err);
            err = str::Dup("The model reply contained no TOC rows.");
        }
        FreeCsv2Rows(rows);
    }
    SaveApiCallStats(jc->dbg, str::FormatTemp("page_%04d_extract.meta.txt", j->pageNo), stats, j->extractOk);
    if (!j->extractOk) {
        j->err = err ? err : str::Dup("Extraction failed.");
        logf("AiTocApi: extract page %d failed: %s\n", j->pageNo, j->err);
    } else {
        str::Free(err);
    }
    LONG done = InterlockedIncrement(&jc->done);
    if (jc->progress) {
        jc->progress(jc->progressCtx, jc->stage, (int)done, jc->jobs->Size());
    }
}

// ---------------------------------------------------------------------------
// stage B: per-page level determination (determine_toc_levels.py)

static bool ValidateCsv3Reply(const char* content, void* ctx, char** errOut) {
    *errOut = nullptr;
    auto* fix = (CsvFixCtx*)ctx;
    if (!ValidateAndFixCsv(content, 3, fix->fixed)) {
        *errOut = str::Dup("CSV format validation failed (need 3 columns).");
        return false;
    }
    return true;
}

static bool ParseLevelsCsv(const char* csv, Vec<ApiTocEntry>& out, char** errOut) {
    *errOut = nullptr;
    Vec<char*> lines;
    SplitLines(csv, lines);
    if (lines.size() < 2) {
        FreeCStrVec(lines);
        *errOut = str::Dup("The level CSV has no data rows.");
        return false;
    }
    Vec<char*> hdr;
    CsvParseLine(lines[0], hdr);
    int titleIdx = -1;
    int pageIdx = -1;
    int levelIdx = -1;
    for (size_t i = 0; i < hdr.size(); i++) {
        char* c = hdr[i];
        for (char* p = c; *p; p++) {
            *p = (char)tolower((u8)*p);
        }
        if (str::Eq(c, "title")) {
            titleIdx = (int)i;
        } else if (str::Eq(c, "page_number")) {
            pageIdx = (int)i;
        } else if (str::Eq(c, "level")) {
            levelIdx = (int)i;
        }
    }
    FreeCStrVec(hdr);
    if (titleIdx < 0 || pageIdx < 0 || levelIdx < 0) {
        FreeCStrVec(lines);
        *errOut = str::Dup("The level CSV is missing title/page_number/level columns.");
        return false;
    }
    bool any = false;
    for (size_t i = 1; i < lines.size(); i++) {
        if (!lines[i][0]) {
            continue;
        }
        Vec<char*> cols;
        CsvParseLine(lines[i], cols);
        char* title = titleIdx < (int)cols.size() ? cols[titleIdx] : nullptr;
        char* pageTok = pageIdx < (int)cols.size() ? cols[pageIdx] : nullptr;
        char* levelTok = levelIdx < (int)cols.size() ? cols[levelIdx] : nullptr;
        if (title) {
            TrimInPlace(title);
        }
        if (pageTok) {
            TrimInPlace(pageTok);
        }
        if (levelTok) {
            TrimInPlace(levelTok);
        }
        int level = 0;
        if (title && title[0] && levelTok && ParseIntStrict(levelTok, &level) && level >= 1) {
            ApiTocEntry e;
            e.title = str::Dup(title);
            e.level = level;
            NormalizePageToken(pageTok, &e.pageNum, &e.pageLabel);
            out.Append(e);
            any = true;
        }
        FreeCStrVec(cols);
    }
    FreeCStrVec(lines);
    if (!any) {
        *errOut = str::Dup("The level CSV contained no usable rows.");
        return false;
    }
    return true;
}

static int EntrySortRank(const ApiTocEntry& e) {
    if (e.pageLabel) {
        return 0; // roman / label first
    }
    if (e.pageNum > 0) {
        return 1; // arabic
    }
    return 2; // no page
}

static int EntrySortNumber(const ApiTocEntry& e) {
    if (e.pageLabel) {
        return RomanToInt(e.pageLabel);
    }
    return e.pageNum;
}

// Stable insertion sort on the (is_roman, page) key, null pages last.
static void StableSortEntries(Vec<ApiTocEntry>& entries) {
    for (size_t i = 1; i < entries.size(); i++) {
        ApiTocEntry key = entries[i];
        size_t j = i;
        while (j > 0) {
            int r1 = EntrySortRank(entries[j - 1]);
            int r2 = EntrySortRank(key);
            bool move = (r1 > r2) || (r1 == r2 && EntrySortNumber(entries[j - 1]) > EntrySortNumber(key));
            if (!move) {
                break;
            }
            entries[j] = entries[j - 1];
            j--;
        }
        entries[j] = key;
    }
}

static void LevelsJobFn(void* ctx, int idx) {
    auto* lc = (StageJobCtx*)ctx;
    int jobIdx = idx + lc->offset;
    ApiPageJob* j = (*lc->jobs)[(size_t)jobIdx];
    if (lc->canceled && lc->canceled(lc->cancelCtx)) {
        return;
    }
    bool isFirst = jobIdx == 0;
    ApiPageJob* first = (*lc->jobs)[0];
    StrBuilder input;
    input.Append(kLevelPrompt);
    input.Append("\n\n当前页提取的原始 CSV 数据如下：\n");
    input.Append(j->csvExtract ? j->csvExtract : "");
    StrBuilder firstResult;
    ChatPart parts[6] = {};
    int nParts = 0;
    if (isFirst) {
        parts[nParts++].text = "当前页图片（需处理，作为后续页面的参考示例）：";
        parts[nParts++].text = input.Get();
        parts[nParts++].b64 = j->b64;
    } else {
        firstResult.Append("参考结果 (CSV 格式):\n");
        firstResult.Append(first->levelsCsv ? first->levelsCsv : "");
        parts[nParts++].text = "参考示例（第一页图片及其正确的 CSV 格式层级分析结果）：";
        parts[nParts++].b64 = first->b64;
        parts[nParts++].text = firstResult.Get();
        parts[nParts++].text = "---\n请严格参照上述示例的 CSV 格式和层级判断标准，分析以下当前页图片：";
        parts[nParts++].b64 = j->b64;
        parts[nParts++].text = input.Get();
    }
    StrBuilder reply;
    char* err = nullptr;
    ApiCallStats stats;
    if (VisionCallWithRetry(*lc->cfg, parts, nParts, ValidateCsv3Reply, &j->fix, lc->canceled, lc->cancelCtx, reply,
                            &err, &stats)) {
        char* levelsCsv = str::Dup(j->fix.fixed.Get() ? j->fix.fixed.Get() : "");
        char* parseErr = nullptr;
        Vec<ApiTocEntry> entries;
        if (ParseLevelsCsv(levelsCsv, entries, &parseErr)) {
            StableSortEntries(entries);
            for (size_t i = 0; i < entries.size(); i++) {
                j->entries.Append(entries[i]);
                if (entries[i].level > j->maxLevel) {
                    j->maxLevel = entries[i].level;
                }
            }
            str::ReplaceWithCopy(&j->levelsCsv, levelsCsv);
            j->levelsOk = true;
        }
        entries.Reset();
        str::Free(levelsCsv);
        if (!j->levelsOk) {
            str::Free(err);
            err = parseErr ? parseErr : str::Dup("The level reply could not be parsed.");
        }
        if (j->levelsOk && lc->dbg) {
            lc->dbg->SaveText(str::FormatTemp("page_%04d_levels.csv", j->pageNo), j->levelsCsv);
        }
    }
    SaveApiCallStats(lc->dbg, str::FormatTemp("page_%04d_levels.meta.txt", j->pageNo), stats, j->levelsOk);
    if (!j->levelsOk) {
        j->err = err ? err : str::Dup("Level determination failed.");
        logf("AiTocApi: levels page %d failed: %s\n", j->pageNo, j->err);
    } else {
        str::Free(err);
    }
    LONG done = InterlockedIncrement(&lc->done);
    if (lc->progress) {
        lc->progress(lc->progressCtx, lc->stage, (int)done, lc->jobs->Size());
    }
}

// Level post-processing (determine_toc_levels.post_process_levels): later
// pages whose deepest level is shallower than the first page's are shifted
// down so cross-page hierarchy stays consistent.
static void AdjustPageLevels(Vec<ApiPageJob*>& jobs) {
    if (jobs.Size() < 2) {
        return;
    }
    int firstMax = jobs[0]->maxLevel;
    for (size_t i = 1; i < jobs.size(); i++) {
        ApiPageJob* j = jobs[i];
        if (j->maxLevel > 0 && j->maxLevel < firstMax) {
            int diff = firstMax - j->maxLevel;
            for (size_t k = 0; k < j->entries.size(); k++) {
                j->entries[k].level += diff;
            }
            j->maxLevel += diff;
        }
    }
}

// Merges the per-page entries in reading order and normalizes the minimum
// level to 1 (content_postprocessor.normalize_levels).
static void MergeAndNormalize(Vec<ApiPageJob*>& jobs, Vec<ApiTocEntry>& out) {
    int minLevel = 0;
    for (size_t i = 0; i < jobs.size(); i++) {
        ApiPageJob* j = jobs[i];
        for (size_t k = 0; k < j->entries.size(); k++) {
            int lv = j->entries[k].level;
            if (minLevel == 0 || lv < minLevel) {
                minLevel = lv;
            }
        }
    }
    int shift = minLevel > 1 ? minLevel - 1 : 0;
    for (size_t i = 0; i < jobs.size(); i++) {
        ApiPageJob* j = jobs[i];
        for (size_t k = 0; k < j->entries.size(); k++) {
            const ApiTocEntry& src = j->entries[k];
            ApiTocEntry e;
            e.title = str::Dup(src.title);
            e.level = src.level - shift;
            if (e.level < 1) {
                e.level = 1;
            }
            e.pageNum = src.pageNum;
            if (src.pageLabel) {
                e.pageLabel = str::Dup(src.pageLabel);
            }
            out.Append(e);
        }
    }
}

// ---------------------------------------------------------------------------
// stage C: AI page-offset sampling (pdf_metadata_extractor.py)

static int ModeOfInts(Vec<int>& vals) {
    if (vals.Size() == 0) {
        return 0;
    }
    int bestVal = 0;
    int bestCount = 0;
    for (size_t i = 0; i < vals.size(); i++) {
        int count = 0;
        for (size_t k = 0; k < vals.size(); k++) {
            if (vals[k] == vals[i]) {
                count++;
            }
        }
        if (count > bestCount) {
            bestCount = count;
            bestVal = vals[i];
        }
    }
    return bestCount > 0 ? bestVal : 0;
}

static int ModeCount(Vec<int>& vals) {
    int bestCount = 0;
    for (size_t i = 0; i < vals.size(); i++) {
        int count = 0;
        for (size_t k = 0; k < vals.size(); k++) {
            if (vals[k] == vals[i]) {
                count++;
            }
        }
        if (count > bestCount) {
            bestCount = count;
        }
    }
    return bestCount;
}

struct OffsetJobCtx {
    const AiTocApiConfig* cfg = nullptr;
    EngineBase* engine = nullptr;
    Vec<int> pages;
    Vec<int> results; // 0 = no valid offset (offsets are usually >= 0)
    Vec<int> valid;
    bool roman = false;
    AiTocApiCancelFn canceled = nullptr;
    void* cancelCtx = nullptr;
    const ApiDebugDir* dbg = nullptr;
};

static void OffsetJobFn(void* ctx, int idx) {
    auto* oc = (OffsetJobCtx*)ctx;
    int pageNo = oc->pages[(size_t)idx];
    if (oc->canceled && oc->canceled(oc->cancelCtx)) {
        return;
    }
    char* b64 = nullptr;
    char* err = nullptr;
    if (!RenderPageJpegBase64(oc->engine, pageNo, kApiImageLongEdge, kApiJpegQuality, &b64, nullptr, &err)) {
        str::Free(err);
        return;
    }
    ChatPart parts[2] = {};
    TempStr prompt = oc->roman ? BuildRomanOffsetPromptTemp(pageNo) : BuildArabicOffsetPromptTemp(pageNo);
    parts[0].text = prompt;
    parts[1].b64 = b64;
    StrBuilder reply;
    char* callErr = nullptr;
    if (VisionCallWithRetry(*oc->cfg, parts, 2, nullptr, nullptr, oc->canceled, oc->cancelCtx, reply, &callErr)) {
        char* text = str::Dup(reply.Get() ? reply.Get() : "");
        TrimInPlace(text);
        int v = 0;
        if (ParseIntStrict(text, &v)) {
            oc->results[(size_t)idx] = v;
            oc->valid[(size_t)idx] = 1;
        }
        if (oc->dbg) {
            oc->dbg->SaveText(str::FormatTemp("offset_p%d.txt", pageNo), text);
        }
        str::Free(text);
    }
    str::Free(callErr);
    str::Free(b64);
}

// Samples a page offset with the model: draws up to 10 pages, asks for the
// printed page number, and returns the mode of the valid offsets.
static int SampleOffsetWithAI(const AiTocApiConfig& cfg, EngineBase* engine, Vec<int>& pool, bool roman,
                              AiTocApiCancelFn canceled, void* cancelCtx, const ApiDebugDir& dbg,
                              AiTocApiProgressFn progress, void* progressCtx) {
    if (pool.Size() == 0) {
        return -1;
    }
    // Fisher-Yates shuffle so the sample is random like autoContents.
    for (size_t i = pool.size(); i > 1; i--) {
        size_t j = (size_t)(rand() % (int)i);
        int tmp = pool[i - 1];
        pool[i - 1] = pool[j];
        pool[j] = tmp;
    }
    int firstRound = pool.Size() < 5 ? (int)pool.Size() : 5;
    Vec<int> allValid;
    for (int round = 0; round < 2; round++) {
        int take = round == 0 ? firstRound : 5;
        if (round == 1) {
            if (ModeCount(allValid) >= 4) {
                break;
            }
            if ((int)pool.Size() - firstRound < 5) {
                break;
            }
            take = 5;
        }
        OffsetJobCtx oc;
        oc.cfg = &cfg;
        oc.engine = engine;
        oc.roman = roman;
        oc.canceled = canceled;
        oc.cancelCtx = cancelCtx;
        oc.dbg = &dbg;
        int begin = round == 0 ? 0 : firstRound;
        for (int i = 0; i < take; i++) {
            int p = pool[(size_t)(begin + i)];
            oc.pages.Append(p);
            oc.results.Append(0);
            oc.valid.Append(0);
        }
        ApiJobQueue q;
        q.count = oc.pages.Size();
        q.fn = OffsetJobFn;
        q.ctx = &oc;
        RunApiJobs(q, ApiConcurrency(cfg));
        for (size_t i = 0; i < oc.pages.size(); i++) {
            if (oc.valid[i]) {
                allValid.Append(oc.results[i]);
            }
        }
        if (progress) {
            progress(progressCtx, AiTocApiStage::Offset, (int)allValid.Size(), 10);
        }
        if (canceled && canceled(cancelCtx)) {
            break;
        }
        if (round == 0) {
            firstRound = 5;
        }
    }
    if (allValid.Size() == 0) {
        return -1;
    }
    return ModeOfInts(allValid);
}

// ---------------------------------------------------------------------------
// items JSON

static char* BuildItemsJson(Vec<ApiTocEntry>& entries) {
    StrBuilder out;
    out.Append("{\"items\": [");
    for (size_t i = 0; i < entries.size(); i++) {
        ApiTocEntry& e = entries[i];
        if (i > 0) {
            out.Append(", ");
        }
        out.Append("{\"title\": ");
        AppendJsonEscaped(out, e.title ? e.title : "");
        out.Append(", \"page\": ");
        if (e.pageNum > 0) {
            out.AppendFmt("%d", e.pageNum);
        } else if (e.pageLabel && e.pageLabel[0]) {
            AppendJsonEscaped(out, e.pageLabel);
        } else {
            out.Append("null");
        }
        if (e.hasPdfPage) {
            out.AppendFmt(", \"pdf_page\": %d", e.pdfPage);
        }
        out.AppendFmt(", \"level\": %d}", e.level);
    }
    out.Append("]}");
    return out.StealData();
}

// ---------------------------------------------------------------------------
// public API

bool AiTocApiRunRecognition(EngineBase* engine, const Vec<int>& pages, const AiTocApiConfig& cfg,
                            AiTocApiCancelFn canceled, void* cancelCtx, AiTocApiProgressFn progress, void* progressCtx,
                            char** itemsJsonOut, int* arabicOffsetOut, int* romanOffsetOut, char** errorOut) {
    *itemsJsonOut = nullptr;
    *errorOut = nullptr;
    *arabicOffsetOut = -1;
    *romanOffsetOut = -1;
    if (!engine || pages.Size() == 0) {
        *errorOut = str::Dup("No TOC pages were selected.");
        return false;
    }
    if (!AiTocApiIsConfigured(cfg)) {
        *errorOut = str::Dup("The AI TOC API is not configured (set base URL, key and model in Settings).");
        return false;
    }
    ApiDebugDir dbg;
    dbg.Init();

    Vec<ApiPageJob*> jobs;
    for (int i = 0; i < pages.Size(); i++) {
        auto* j = new ApiPageJob();
        j->pageNo = pages[i];
        jobs.Append(j);
    }
    auto cleanupJobs = [&]() {
        for (size_t i = 0; i < jobs.size(); i++) {
            FreeApiPageJob(jobs[i]);
        }
        jobs.Reset();
    };
    auto fail = [&](char* err) {
        *errorOut = err ? err : str::Dup("The AI TOC recognition failed.");
        cleanupJobs();
        return false;
    };

    // 1) render the confirmed TOC pages
    for (int i = 0; i < jobs.Size(); i++) {
        if (canceled && canceled(cancelCtx)) {
            return fail(str::Dup("Canceled."));
        }
        ApiPageJob* j = jobs[i];
        StrBuilder bytes;
        char* err = nullptr;
        if (!RenderPageJpegBase64(engine, j->pageNo, kApiImageLongEdge, kApiJpegQuality, &j->b64, &bytes, &err)) {
            return fail(err);
        }
        dbg.SaveBytes(str::FormatTemp("page_%d.jpg", j->pageNo), bytes.Get(), bytes.size());
        if (progress) {
            progress(progressCtx, AiTocApiStage::Render, i + 1, jobs.Size());
        }
    }

    // 2) stage A: title/page extraction
    {
        StageJobCtx sc;
        sc.cfg = &cfg;
        sc.jobs = &jobs;
        sc.canceled = canceled;
        sc.cancelCtx = cancelCtx;
        sc.dbg = &dbg;
        sc.progress = progress;
        sc.progressCtx = progressCtx;
        sc.stage = AiTocApiStage::Extract;
        ApiJobQueue q;
        q.count = jobs.Size();
        q.fn = ExtractJobFn;
        q.ctx = &sc;
        RunApiJobs(q, ApiConcurrency(cfg));
        if (canceled && canceled(cancelCtx)) {
            return fail(str::Dup("Canceled."));
        }
        for (size_t i = 0; i < jobs.size(); i++) {
            if (!jobs[i]->extractOk) {
                char* msg = jobs[i]->err ? str::Dup(jobs[i]->err)
                                         : str::Format("Extracting TOC data from page %d failed.", jobs[i]->pageNo);
                return fail(msg);
            }
        }
    }

    // 3) stage B: levels; the first page runs first and acts as the few-shot
    {
        StageJobCtx lc;
        lc.cfg = &cfg;
        lc.jobs = &jobs;
        lc.canceled = canceled;
        lc.cancelCtx = cancelCtx;
        lc.dbg = &dbg;
        lc.progress = progress;
        lc.progressCtx = progressCtx;
        lc.stage = AiTocApiStage::Levels;
        LevelsJobFn(&lc, 0);
        if (canceled && canceled(cancelCtx)) {
            return fail(str::Dup("Canceled."));
        }
        if (!jobs[0]->levelsOk) {
            char* msg = jobs[0]->err ? str::Dup(jobs[0]->err)
                                     : str::Format("Determining levels on page %d failed.", jobs[0]->pageNo);
            return fail(msg);
        }
        if (jobs.Size() > 1) {
            int remaining = jobs.Size() - 1;
            ApiJobQueue q;
            q.count = remaining;
            q.fn = LevelsJobFn;
            q.ctx = &lc;
            lc.offset = 1;
            lc.done = 1; // page 0 already reported
            RunApiJobs(q, ApiConcurrency(cfg));
        }
        if (canceled && canceled(cancelCtx)) {
            return fail(str::Dup("Canceled."));
        }
        for (size_t i = 0; i < jobs.size(); i++) {
            if (!jobs[i]->levelsOk) {
                char* msg = jobs[i]->err ? str::Dup(jobs[i]->err)
                                         : str::Format("Determining levels on page %d failed.", jobs[i]->pageNo);
                return fail(msg);
            }
        }
    }

    // 4) merge + normalize
    AdjustPageLevels(jobs);
    Vec<ApiTocEntry> entries;
    MergeAndNormalize(jobs, entries);
    if (entries.Size() == 0) {
        return fail(str::Dup("The AI reply contained no TOC items."));
    }

    // 5) page-offset sampling (only when the local text-layer strategies fail)
    bool anyArabic = false;
    bool anyRoman = false;
    for (size_t i = 0; i < entries.size(); i++) {
        if (entries[i].pageNum > 0) {
            anyArabic = true;
        }
        if (entries[i].pageLabel && IsRomanToken(entries[i].pageLabel)) {
            anyRoman = true;
        }
    }
    int totalPages = engine->PageCount();
    if (anyArabic) {
        bool needArabic = true;
        Vec<int> pdgMap;
        if (TocCalibBuildPdgPrintedMap(engine, pdgMap)) {
            needArabic = false; // the import path can map printed pages via PDG names
        } else if (TocCalibEstimateArabicOffset(engine, pages.Last()) >= 0) {
            needArabic = false; // footer/header text layer provides the offset
        }
        if (needArabic && totalPages > 0) {
            int start = (int)(totalPages * 0.2) + 1;
            int end = (int)(totalPages * 0.8);
            if (end < start) {
                start = 1;
                end = totalPages;
            }
            if (start < 1) {
                start = 1;
            }
            if (end > totalPages) {
                end = totalPages;
            }
            Vec<int> pool;
            for (int p = start; p <= end; p++) {
                pool.Append(p);
            }
            int off = SampleOffsetWithAI(cfg, engine, pool, false, canceled, cancelCtx, dbg, progress, progressCtx);
            if (off >= 0) {
                *arabicOffsetOut = off;
                logf("AiTocApi: AI arabic offset=%d\n", off);
            }
        }
    }
    if (anyRoman && !engine->HasPageLabels()) {
        int end = (int)(totalPages * 0.2);
        if (end > kRomanOffsetMaxPages) {
            end = kRomanOffsetMaxPages;
        }
        Vec<int> pool;
        for (int p = 1; p <= end; p++) {
            pool.Append(p);
        }
        int off = SampleOffsetWithAI(cfg, engine, pool, true, canceled, cancelCtx, dbg, progress, progressCtx);
        if (off >= 0) {
            *romanOffsetOut = off;
            logf("AiTocApi: AI roman offset=%d\n", off);
            for (size_t i = 0; i < entries.size(); i++) {
                ApiTocEntry& e = entries[i];
                if (!e.pageLabel || !IsRomanToken(e.pageLabel)) {
                    continue;
                }
                int rv = RomanToInt(e.pageLabel);
                if (rv <= 0) {
                    continue;
                }
                int pdf = rv + off;
                if (pdf < 1) {
                    pdf = 1;
                }
                if (totalPages > 0 && pdf > totalPages) {
                    pdf = totalPages;
                }
                e.hasPdfPage = true;
                e.pdfPage = pdf;
            }
        }
    }

    if (canceled && canceled(cancelCtx)) {
        return fail(str::Dup("Canceled."));
    }
    *itemsJsonOut = BuildItemsJson(entries);
    for (size_t i = 0; i < entries.size(); i++) {
        str::Free(entries[i].title);
        str::Free(entries[i].pageLabel);
    }
    entries.Reset();
    cleanupJobs();
    if (!*itemsJsonOut) {
        *errorOut = str::Dup("Could not build the TOC JSON.");
        return false;
    }
    logf("AiTocApi: recognition done, items json %d bytes\n", (int)strlen(*itemsJsonOut));
    return true;
}

// ---------------------------------------------------------------------------
// sliding-window AI detection of the printed-TOC page range

struct DetectWindowJob {
    int start = 0;
    int end = 0;
};

struct TocRangeVisitor : json::ValueVisitor {
    int start = 0;
    int end = 0;
    bool hasStart = false;
    bool hasEnd = false;

    bool Visit(const char* path, const char* value, json::Type type) override {
        if (str::Eq(path, "/toc_start")) {
            int v = 0;
            if (type == json::Type::Number && ParseIntStrict(value, &v)) {
                start = v;
                hasStart = true;
            }
        } else if (str::Eq(path, "/toc_end")) {
            int v = 0;
            if (type == json::Type::Number && ParseIntStrict(value, &v)) {
                end = v;
                hasEnd = true;
            }
        }
        return true;
    }
};

struct SingleVoteVisitor : json::ValueVisitor {
    bool isToc = false;

    bool Visit(const char* path, const char* value, json::Type type) override {
        if (str::Eq(path, "/is_toc") && type == json::Type::Bool) {
            isToc = str::Eq(value, "true");
        }
        return true;
    }
};

struct DetectCtx {
    const AiTocApiConfig* cfg = nullptr;
    EngineBase* engine = nullptr;
    Vec<DetectWindowJob> windows;
    Vec<int>* isToc = nullptr;
    Vec<int>* notToc = nullptr;
    int totalPages = 0;
    AiTocApiCancelFn canceled = nullptr;
    void* cancelCtx = nullptr;
    const ApiDebugDir* dbg = nullptr;
    AiTocApiProgressFn progress = nullptr;
    void* progressCtx = nullptr;
    int totalWindows = 0;
    volatile LONG completedWindows = 0;
    // API outcome aggregation so a total failure can be reported as an API
    // error instead of the misleading "no TOC pages detected".
    volatile LONG apiOkCount = 0;
    volatile LONG apiFailCount = 0;
    char* firstApiErr = nullptr; // owned; guarded by InterlockedCompareExchangePointer
};

static void DetectWindowJobFn(void* ctx, int idx) {
    auto* dc = (DetectCtx*)ctx;
    if (idx < 0 || (size_t)idx >= dc->windows.size()) {
        return;
    }
    DetectWindowJob win = dc->windows[(size_t)idx];
    if (dc->canceled && dc->canceled(dc->cancelCtx)) {
        return;
    }
    defer {
        LONG done = InterlockedIncrement(&dc->completedWindows);
        if (dc->progress) {
            dc->progress(dc->progressCtx, AiTocApiStage::Extract, (int)done, dc->totalWindows);
        }
    };
    char* b64 = nullptr;
    StrBuilder bytes;
    int nPages = 0;
    char* err = nullptr;
    if (!RenderConcatPagesJpegBase64(dc->engine, win.start, win.end, &b64, &bytes, &nPages, &err)) {
        str::Free(err);
        return;
    }
    if (dc->dbg) {
        dc->dbg->SaveBytes(str::FormatTemp("detect_%d_%d.jpg", win.start, win.end), bytes.Get(), bytes.size());
    }
    ChatPart parts[2] = {};
    parts[0].text = BuildDetectPromptTemp(win.start);
    parts[1].b64 = b64;
    StrBuilder reply;
    char* callErr = nullptr;
    ApiCallStats stats;
    bool callOk =
        VisionCallWithRetry(*dc->cfg, parts, 2, nullptr, nullptr, dc->canceled, dc->cancelCtx, reply, &callErr, &stats);
    SaveApiCallStats(dc->dbg, str::FormatTemp("detect_%d_%d.meta.txt", win.start, win.end), stats, callOk);
    if (callOk) {
        InterlockedIncrement(&dc->apiOkCount);
        if (dc->dbg) {
            dc->dbg->SaveText(str::FormatTemp("detect_%d_%d.json", win.start, win.end), reply.Get());
        }
        TocRangeVisitor v;
        json::Parse(reply.Get(), &v);
        if (v.hasStart && v.hasEnd && v.start >= 1 && v.end >= v.start) {
            for (int p = win.start; p <= win.end; p++) {
                if (p < 1 || p > dc->totalPages) {
                    continue;
                }
                if (p >= v.start && p <= v.end) {
                    InterlockedIncrement((volatile LONG*)&(*dc->isToc)[p]);
                } else {
                    InterlockedIncrement((volatile LONG*)&(*dc->notToc)[p]);
                }
            }
        }
    } else {
        InterlockedIncrement(&dc->apiFailCount);
        // remember only the first error for the summary; duplicates are freed
        char* prev = (char*)InterlockedCompareExchangePointer((PVOID*)&dc->firstApiErr, callErr, nullptr);
        if (prev) {
            str::Free(callErr);
            callErr = nullptr; // ownership moved to dc->firstApiErr on first failure
        }
        logf("AiTocApi: detect window %d-%d failed: %s\n", win.start, win.end,
             dc->firstApiErr ? dc->firstApiErr : "(no error)");
    }
    str::Free(callErr);
    str::Free(b64);
}

struct SingleVoteCtx {
    const AiTocApiConfig* cfg = nullptr;
    EngineBase* engine = nullptr;
    Vec<int>* pages = nullptr;
    Vec<int>* votes = nullptr; // 1 = toc, 0 = not toc
    AiTocApiCancelFn canceled = nullptr;
    void* cancelCtx = nullptr;
    const ApiDebugDir* dbg = nullptr;
    volatile LONG apiOkCount = 0;
    volatile LONG apiFailCount = 0;
    char* firstApiErr = nullptr; // owned; guarded by InterlockedCompareExchangePointer
};

static void SingleVoteJobFn(void* ctx, int idx) {
    auto* sc = (SingleVoteCtx*)ctx;
    if (idx < 0 || (size_t)idx >= sc->pages->size()) {
        return;
    }
    int pageNo = (*sc->pages)[(size_t)idx];
    if (sc->canceled && sc->canceled(sc->cancelCtx)) {
        return;
    }
    char* b64 = nullptr;
    char* err = nullptr;
    if (!RenderPageJpegBase64(sc->engine, pageNo, kApiImageLongEdge, kApiJpegQuality, &b64, nullptr, &err)) {
        str::Free(err);
        return;
    }
    ChatPart parts[2] = {};
    parts[0].text = BuildSingleVotePromptTemp(pageNo);
    parts[1].b64 = b64;
    StrBuilder reply;
    char* callErr = nullptr;
    if (VisionCallWithRetry(*sc->cfg, parts, 2, nullptr, nullptr, sc->canceled, sc->cancelCtx, reply, &callErr)) {
        InterlockedIncrement(&sc->apiOkCount);
        SingleVoteVisitor v;
        json::Parse(reply.Get(), &v);
        if (v.isToc) {
            InterlockedExchange((volatile LONG*)&(*sc->votes)[(size_t)idx], 1);
        }
        if (sc->dbg) {
            sc->dbg->SaveText(str::FormatTemp("vote_p%d.json", pageNo), reply.Get());
        }
    } else {
        InterlockedIncrement(&sc->apiFailCount);
        char* prev = (char*)InterlockedCompareExchangePointer((PVOID*)&sc->firstApiErr, callErr, nullptr);
        if (prev) {
            str::Free(callErr);
            callErr = nullptr;
        }
        logf("AiTocApi: vote page %d failed: %s\n", pageNo, sc->firstApiErr ? sc->firstApiErr : "(no error)");
    }
    str::Free(callErr);
    str::Free(b64);
}

bool AiTocApiDetectTocPages(EngineBase* engine, const AiTocApiConfig& cfg, AiTocApiCancelFn canceled, void* cancelCtx,
                            AiTocApiProgressFn progress, void* progressCtx, Vec<int>& pagesOut, char** errorOut) {
    *errorOut = nullptr;
    if (!engine) {
        *errorOut = str::Dup("No document is open.");
        return false;
    }
    if (!AiTocApiIsConfigured(cfg)) {
        *errorOut = str::Dup("The AI TOC API is not configured (set base URL, key and model in Settings).");
        return false;
    }
    int total = engine->PageCount();
    if (total < 1) {
        *errorOut = str::Dup("The document has no pages.");
        return false;
    }
    ApiDebugDir dbg;
    dbg.Init();

    Vec<int> isToc;
    Vec<int> notToc;
    for (int i = 0; i <= total; i++) {
        isToc.Append(0);
        notToc.Append(0);
    }
    DetectCtx dc;
    dc.cfg = &cfg;
    dc.engine = engine;
    dc.isToc = &isToc;
    dc.notToc = &notToc;
    dc.totalPages = total;
    dc.canceled = canceled;
    dc.cancelCtx = cancelCtx;
    dc.dbg = &dbg;
    dc.progress = progress;
    dc.progressCtx = progressCtx;
    auto runBatch = [&](int fromPage, int toPage) {
        dc.windows.Reset();
        for (int i = fromPage; i < toPage; i += 2) {
            if (i > total) {
                break;
            }
            DetectWindowJob w;
            w.start = i;
            int e = i + 3;
            if (e > total) {
                e = total;
            }
            w.end = e;
            dc.windows.Append(w);
        }
        if (dc.windows.Size() == 0) {
            return;
        }
        dc.totalWindows = (int)dc.completedWindows + dc.windows.Size();
        if (progress) {
            progress(progressCtx, AiTocApiStage::Extract, (int)dc.completedWindows, dc.totalWindows);
        }
        ApiJobQueue q;
        q.count = dc.windows.Size();
        q.fn = DetectWindowJobFn;
        q.ctx = &dc;
        RunApiJobs(q, ApiConcurrency(cfg));
    };

    int currentLimit = total < kDetectInitialPages ? total : kDetectInitialPages;
    int lastScanned = 0;
    int retryCount = 0;
    bool tocFound = false;
    bool wasCanceled = false;
    for (;;) {
        runBatch(lastScanned + 1, currentLimit + 1);
        lastScanned = currentLimit;
        if (canceled && canceled(cancelCtx)) {
            wasCanceled = true;
            break;
        }
        if (!tocFound) {
            bool found = false;
            for (int p = 1; p <= currentLimit; p++) {
                if (isToc[p] > 0) {
                    found = true;
                    break;
                }
            }
            if (found) {
                tocFound = true;
            } else {
                if (retryCount < kDetectMaxRetries && currentLimit < total && currentLimit < kDetectMaxScanPages) {
                    retryCount++;
                    int nextLimit = currentLimit + kDetectExpandStep;
                    if (nextLimit > kDetectMaxScanPages) {
                        nextLimit = kDetectMaxScanPages;
                    }
                    if (nextLimit <= currentLimit) {
                        break;
                    }
                    currentLimit = nextLimit;
                    continue;
                }
                break;
            }
        }
        if (currentLimit < total && currentLimit < kDetectMaxScanPages && isToc[currentLimit] > 0) {
            int nextLimit = currentLimit + kDetectExpandStep;
            if (nextLimit > kDetectMaxScanPages) {
                nextLimit = kDetectMaxScanPages;
            }
            if (nextLimit <= currentLimit) {
                break;
            }
            currentLimit = nextLimit;
            continue;
        }
        break;
    }
    if (wasCanceled) {
        *errorOut = str::Dup("Canceled.");
        str::Free(dc.firstApiErr);
        return false;
    }
    Vec<int> conflict;
    Vec<int> finalPages;
    for (int p = 1; p <= total; p++) {
        if (isToc[p] > 0 && notToc[p] > 0) {
            conflict.Append(p);
        } else if (isToc[p] > 0) {
            finalPages.Append(p);
        }
    }
    LONG voteOkCount = -1;
    LONG voteFailCount = 0;
    char* voteFirstErr = nullptr;
    if (conflict.Size() > 0) {
        Vec<int> votes;
        for (size_t i = 0; i < conflict.size(); i++) {
            votes.Append(0);
        }
        SingleVoteCtx sc;
        sc.cfg = &cfg;
        sc.engine = engine;
        sc.pages = &conflict;
        sc.votes = &votes;
        sc.canceled = canceled;
        sc.cancelCtx = cancelCtx;
        sc.dbg = &dbg;
        ApiJobQueue q;
        q.count = conflict.Size();
        q.fn = SingleVoteJobFn;
        q.ctx = &sc;
        RunApiJobs(q, ApiConcurrency(cfg));
        voteOkCount = sc.apiOkCount;
        voteFailCount = sc.apiFailCount;
        voteFirstErr = sc.firstApiErr; // ownership moves out of sc
        sc.firstApiErr = nullptr;
        for (size_t i = 0; i < conflict.size(); i++) {
            if (votes[i] == 1) {
                finalPages.Append(conflict[i]);
            }
        }
    }
    if (progress) {
        progress(progressCtx, AiTocApiStage::Extract, dc.totalWindows, dc.totalWindows);
    }
    if (finalPages.Size() == 0) {
        // Distinguish "the API itself failed" from "the API looked and found
        // no printed TOC" - swallowed errors previously masked real failures
        // (e.g. HTTP 401) as a harmless "nothing detected".
        if (dc.apiOkCount == 0 && dc.apiFailCount > 0 && dc.firstApiErr && *dc.firstApiErr) {
            *errorOut = str::Format("The API request failed for all %d window(s). First error: %s",
                                    (int)dc.apiFailCount, dc.firstApiErr);
        } else if (voteOkCount == 0 && voteFailCount > 0 && voteFirstErr && *voteFirstErr) {
            *errorOut = str::Format("The API request failed for all %d conflict-page vote(s). First error: %s",
                                    (int)voteFailCount, voteFirstErr);
        } else {
            *errorOut = str::Dup("No printed TOC pages were detected by the API.");
        }
        str::Free(dc.firstApiErr);
        str::Free(voteFirstErr);
        return false;
    }
    str::Free(dc.firstApiErr);
    str::Free(voteFirstErr);

    // Longest continuous run (pages are collected in ascending order).
    int bestStart = finalPages[0];
    int bestEnd = finalPages[0];
    int bestLen = 0;
    int curStart = finalPages[0];
    int curEnd = finalPages[0];
    for (size_t i = 1; i < finalPages.size(); i++) {
        if (finalPages[i] - finalPages[i - 1] <= 1) {
            curEnd = finalPages[i];
        } else {
            int len = curEnd - curStart + 1;
            if (len > bestLen) {
                bestLen = len;
                bestStart = curStart;
                bestEnd = curEnd;
            }
            curStart = finalPages[i];
            curEnd = finalPages[i];
        }
    }
    {
        int len = curEnd - curStart + 1;
        if (len > bestLen) {
            bestLen = len;
            bestStart = curStart;
            bestEnd = curEnd;
        }
    }
    for (int p = bestStart; p <= bestEnd; p++) {
        pagesOut.Append(p);
    }
    logf("AiTocApi: detected TOC pages %d-%d (from %d voted pages)\n", bestStart, bestEnd, (int)finalPages.size());
    return true;
}

// ---------------------------------------------------------------------------
// settings-related helpers

bool AiTocApiTestConnection(const char* baseUrl, const char* key, const char* model, char** msgOut) {
    *msgOut = nullptr;
    AiTocApiConfig cfg;
    cfg.baseUrl = baseUrl;
    cfg.key = key;
    cfg.model = model;
    if (!AiTocApiIsConfigured(cfg)) {
        *msgOut = str::Dup("Please fill in the base URL, API key and model first.");
        return false;
    }
    ChatPart parts[1] = {};
    parts[0].text = "Reply with the single word: ok";
    StrBuilder content;
    char* err = nullptr;
    if (!VisionCallOnce(cfg, parts, 1, 30 * 1000, content, &err)) {
        *msgOut = err ? err : str::Dup("The API request failed.");
        return false;
    }
    char* reply = str::Dup(content.Get() ? content.Get() : "");
    TrimInPlace(reply);
    if (strlen(reply) > 200) {
        reply[200] = 0;
    }
    *msgOut = str::Format("The API responded successfully. Model reply: %s", reply);
    str::Free(reply);
    return true;
}

struct ModelsResponseVisitor : json::ValueVisitor {
    Vec<char*> models;
    char* error = nullptr;

    ~ModelsResponseVisitor() override {
        str::Free(error);
        for (auto* model : models) {
            str::Free(model);
        }
        models.Reset();
    }

    bool Visit(const char* path, const char* value, json::Type type) override {
        if (type != json::Type::String) {
            return true;
        }
        if (str::Eq(path, "/error/message")) {
            str::ReplaceWithCopy(&error, value);
        } else if (str::StartsWith(path, "/data[")) {
            const char* end = str::FindChar(path, ']');
            if (end && str::Eq(end, "]/id") && value && *value) {
                models.Append(str::Dup(value));
            }
        }
        return true;
    }
};

bool AiTocApiFetchModels(const char* baseUrl, const char* key, Vec<char*>& modelsOut, char** errorOut) {
    *errorOut = nullptr;
    if (str::IsEmptyOrWhiteSpace(baseUrl) || str::IsEmptyOrWhiteSpace(key)) {
        *errorOut = str::Dup("Please fill in the base URL and API key first.");
        return false;
    }
    char* trimmed = str::Dup(baseUrl);
    size_t n = strlen(trimmed);
    while (n > 0 && trimmed[n - 1] == '/') {
        trimmed[--n] = 0;
    }
    AutoFree url(str::Format("%s/models", trimmed));
    str::Free(trimmed);
    HttpPostResult res;
    if (!HttpJsonRaw(url, key, "GET", nullptr, 30 * 1000, res, errorOut)) {
        return false;
    }
    ModelsResponseVisitor v;
    const char* body = res.body.Get();
    if (body) {
        json::Parse(body, &v);
    }
    if (res.status != 200) {
        *errorOut = str::Format("Could not load models (HTTP %u): %s", (unsigned)res.status,
                                v.error ? v.error : "The provider rejected GET /models.");
        return false;
    }
    if (v.models.IsEmpty()) {
        *errorOut = str::Dup("The provider returned no models. Enter a model name manually.");
        return false;
    }
    for (auto* model : v.models) {
        modelsOut.Append(str::Dup(model));
    }
    return true;
}

struct LlmCfg {
    char* id = nullptr;
    char* key = nullptr;
    char* baseUrl = nullptr;
    char* model = nullptr;
};

struct LlmConfigFileVisitor : json::ValueVisitor {
    Vec<LlmCfg> configs;
    char* activeId = nullptr;

    ~LlmConfigFileVisitor() override {
        str::Free(activeId);
        for (size_t i = 0; i < configs.size(); i++) {
            str::Free(configs[i].id);
            str::Free(configs[i].key);
            str::Free(configs[i].baseUrl);
            str::Free(configs[i].model);
        }
        configs.Reset();
    }

    LlmCfg& EntryAt(int idx) {
        while ((int)configs.size() <= idx) {
            LlmCfg e;
            configs.Append(e);
        }
        return configs[(size_t)idx];
    }

    bool Visit(const char* path, const char* value, json::Type type) override {
        if (type != json::Type::String) {
            return true;
        }
        if (str::Eq(path, "/active_id")) {
            str::ReplaceWithCopy(&activeId, value);
            return true;
        }
        if (str::StartsWith(path, "/configs[")) {
            const char* p = path + str::Len("/configs[");
            int idx = 0;
            bool hasIdx = false;
            while (*p >= '0' && *p <= '9') {
                idx = idx * 10 + (*p - '0');
                p++;
                hasIdx = true;
            }
            if (!hasIdx || *p++ != ']' || *p++ != '/') {
                return true;
            }
            LlmCfg& c = EntryAt(idx);
            if (str::Eq(p, "id")) {
                str::ReplaceWithCopy(&c.id, value);
            } else if (str::Eq(p, "api_key")) {
                str::ReplaceWithCopy(&c.key, value);
            } else if (str::Eq(p, "base_url")) {
                str::ReplaceWithCopy(&c.baseUrl, value);
            } else if (str::Eq(p, "model")) {
                str::ReplaceWithCopy(&c.model, value);
            }
            return true;
        }
        // Flat format: {"base_url":..., "api_key":..., "model":...}
        if (str::Eq(path, "/api_key") || str::Eq(path, "/base_url") || str::Eq(path, "/model")) {
            LlmCfg& c = EntryAt(0);
            if (str::Eq(path, "/api_key")) {
                str::ReplaceWithCopy(&c.key, value);
            } else if (str::Eq(path, "/base_url")) {
                str::ReplaceWithCopy(&c.baseUrl, value);
            } else {
                str::ReplaceWithCopy(&c.model, value);
            }
        }
        return true;
    }
};

// Resolves an autoContents "$ENV_VAR$" reference; otherwise copies the value.
static char* ResolveEnvRef(const char* v) {
    if (!v || !*v) {
        return nullptr;
    }
    size_t n = strlen(v);
    if (n >= 2 && v[0] == '$' && v[n - 1] == '$') {
        char* name = DupRange(v + 1, n - 2);
        char* val = nullptr;
        char* buf = nullptr;
        size_t sz = 0;
        if (name && _dupenv_s(&buf, &sz, name) == 0 && buf && buf[0]) {
            val = str::Dup(buf);
        }
        free(buf);
        str::Free(name);
        return val;
    }
    return str::Dup(v);
}

bool AiTocApiLoadConfigFile(const char* path, char** baseUrlOut, char** keyOut, char** modelOut) {
    *baseUrlOut = nullptr;
    *keyOut = nullptr;
    *modelOut = nullptr;
    if (!path || !*path || !file::Exists(path)) {
        return false;
    }
    ByteSlice data = file::ReadFile(path);
    if (!data.d || data.sz == 0) {
        data.Free();
        return false;
    }
    char* text = DupRange((const char*)data.d, data.sz);
    data.Free();
    if (!text) {
        return false;
    }
    LlmConfigFileVisitor v;
    bool parsed = json::Parse(text, &v);
    free(text);
    if (!parsed || v.configs.Size() == 0) {
        return false;
    }
    LlmCfg* chosen = nullptr;
    if (v.activeId) {
        for (size_t i = 0; i < v.configs.size(); i++) {
            if (v.configs[i].id && str::Eq(v.configs[i].id, v.activeId)) {
                chosen = &v.configs[i];
                break;
            }
        }
    }
    if (!chosen) {
        chosen = &v.configs[0];
    }
    *baseUrlOut = ResolveEnvRef(chosen->baseUrl);
    *keyOut = ResolveEnvRef(chosen->key);
    *modelOut = ResolveEnvRef(chosen->model);
    return *baseUrlOut != nullptr || *keyOut != nullptr || *modelOut != nullptr;
}

struct AiTocStoredProfile {
    char* name = nullptr;
    char* baseUrl = nullptr;
    char* key = nullptr;
    char* model = nullptr;
    int concurrency = 4;

    void Free() {
        str::Free(name);
        str::Free(baseUrl);
        str::Free(key);
        str::Free(model);
    }
};

struct AiTocStoredProfilesVisitor : json::ValueVisitor {
    Vec<AiTocStoredProfile> profiles;
    int active = 0;

    ~AiTocStoredProfilesVisitor() {
        for (auto& p : profiles) {
            p.Free();
        }
        profiles.Reset();
    }

    bool Visit(const char* path, const char* value, json::Type type) override {
        if (str::Eq(path, "/active") && type == json::Type::Number) {
            active = atoi(value);
            return true;
        }
        if (!str::StartsWith(path, "/profiles[")) {
            return true;
        }
        const char* p = path + str::Len("/profiles[");
        char* end = nullptr;
        long idx = strtol(p, &end, 10);
        if (end == p || idx < 0 || idx >= 100 || !end || *end++ != ']' || *end++ != '/') {
            return true;
        }
        while (profiles.Size() <= (int)idx) {
            profiles.Append(AiTocStoredProfile{});
        }
        auto& profile = profiles[(int)idx];
        if (type == json::Type::String) {
            if (str::Eq(end, "name")) {
                str::ReplaceWithCopy(&profile.name, value);
            } else if (str::Eq(end, "base_url")) {
                str::ReplaceWithCopy(&profile.baseUrl, value);
            } else if (str::Eq(end, "api_key")) {
                str::ReplaceWithCopy(&profile.key, value);
            } else if (str::Eq(end, "model")) {
                str::ReplaceWithCopy(&profile.model, value);
            }
        } else if (type == json::Type::Number && str::Eq(end, "concurrency")) {
            profile.concurrency = limitValue(atoi(value), 1, 8);
        }
        return true;
    }
};

static void AiTocAppendJsonString(StrBuilder& out, const char* s) {
    out.AppendChar('"');
    if (s) {
        for (const u8* p = (const u8*)s; *p; p++) {
            u8 c = *p;
            if (c == '"') {
                out.Append("\\\"");
            } else if (c == '\\') {
                out.Append("\\\\");
            } else if (c < 0x20) {
                out.AppendFmt("\\u%04x", (int)c);
            } else {
                out.AppendChar((char)c);
            }
        }
    }
    out.AppendChar('"');
}

static char* AiTocTransformProfilesJsonSecrets(const char* json, bool protect) {
    if (str::IsEmptyOrWhiteSpace(json)) {
        return str::Dup(json ? json : "");
    }
    AiTocStoredProfilesVisitor visitor;
    if (!json::Parse(json, &visitor) || visitor.profiles.IsEmpty()) {
        return str::Dup(json);
    }
    bool changed = false;
    for (auto& p : visitor.profiles) {
        char* next = protect ? ProtectStringDpapi(p.key) : UnprotectStringDpapi(p.key);
        if (!next) {
            // Keep the previous value so a transient DPAPI failure does not wipe keys.
            continue;
        }
        if (!str::Eq(next, p.key ? p.key : "")) {
            changed = true;
        }
        str::Free(p.key);
        p.key = next;
    }
    if (!changed && !protect) {
        // Still rebuild so concurrency/name fields stay consistent; cheap enough.
    }
    StrBuilder out;
    out.AppendFmt("{\"active\":%d,\"profiles\":[", visitor.active);
    for (int i = 0; i < visitor.profiles.Size(); i++) {
        auto& p = visitor.profiles[i];
        if (i) {
            out.AppendChar(',');
        }
        out.Append("{\"name\":");
        AiTocAppendJsonString(out, p.name);
        out.Append(",\"base_url\":");
        AiTocAppendJsonString(out, p.baseUrl);
        out.Append(",\"api_key\":");
        AiTocAppendJsonString(out, p.key);
        out.Append(",\"model\":");
        AiTocAppendJsonString(out, p.model);
        out.AppendFmt(",\"concurrency\":%d}", p.concurrency);
    }
    out.Append("]}");
    return str::Dup(out.Get());
}

// CryptProtectData emits a fresh blob every call, so reusing the last disk form
// when the plaintext is unchanged keeps SaveSettings from rewriting the file.
static char* gAiTocDiskKeyPlain = nullptr;
static char* gAiTocDiskKeyProtected = nullptr;
static char* gAiTocDiskProfilesPlain = nullptr;
static char* gAiTocDiskProfilesProtected = nullptr;

static void AiTocSetDiskKeyCache(const char* plain, const char* protectedStr) {
    str::ReplaceWithCopy(&gAiTocDiskKeyPlain, plain ? plain : "");
    str::ReplaceWithCopy(&gAiTocDiskKeyProtected, protectedStr ? protectedStr : "");
}

static void AiTocSetDiskProfilesCache(const char* plain, const char* protectedStr) {
    str::ReplaceWithCopy(&gAiTocDiskProfilesPlain, plain ? plain : "");
    str::ReplaceWithCopy(&gAiTocDiskProfilesProtected, protectedStr ? protectedStr : "");
}

void AiTocApiUnprotectPrefsSecrets(GlobalPrefs* prefs) {
    if (!prefs) {
        return;
    }
    if (!str::IsEmptyOrWhiteSpace(prefs->aiTocApiKey) && IsDpapiProtectedString(prefs->aiTocApiKey)) {
        char* protectedStr = str::Dup(prefs->aiTocApiKey);
        char* plain = UnprotectStringDpapi(prefs->aiTocApiKey);
        if (plain) {
            AiTocSetDiskKeyCache(plain, protectedStr);
            str::Free(prefs->aiTocApiKey);
            prefs->aiTocApiKey = plain;
        } else {
            logf("AiTocApiUnprotectPrefsSecrets: failed to decrypt AiTocApiKey\n");
        }
        str::Free(protectedStr);
    } else {
        AiTocSetDiskKeyCache(prefs->aiTocApiKey, nullptr);
    }
    if (!str::IsEmptyOrWhiteSpace(prefs->aiTocApiProfiles)) {
        char* diskForm = str::Dup(prefs->aiTocApiProfiles);
        char* next = AiTocTransformProfilesJsonSecrets(prefs->aiTocApiProfiles, false);
        if (next) {
            // Cache disk JSON only when it already stored protected keys.
            bool hadProtected = str::Find(diskForm, "dpapi:") != nullptr;
            AiTocSetDiskProfilesCache(next, hadProtected ? diskForm : nullptr);
            str::Free(prefs->aiTocApiProfiles);
            prefs->aiTocApiProfiles = next;
        }
        str::Free(diskForm);
    } else {
        AiTocSetDiskProfilesCache(prefs->aiTocApiProfiles, nullptr);
    }
}

void AiTocApiProtectPrefsSecretsForSave(GlobalPrefs* prefs, char** plainKeyOut, char** plainProfilesOut) {
    *plainKeyOut = nullptr;
    *plainProfilesOut = nullptr;
    if (!prefs) {
        return;
    }
    *plainKeyOut = prefs->aiTocApiKey;
    *plainProfilesOut = prefs->aiTocApiProfiles;

    if (gAiTocDiskKeyProtected && gAiTocDiskKeyProtected[0] && str::Eq(prefs->aiTocApiKey, gAiTocDiskKeyPlain)) {
        prefs->aiTocApiKey = str::Dup(gAiTocDiskKeyProtected);
    } else {
        char* protectedKey = ProtectStringDpapi(prefs->aiTocApiKey);
        if (protectedKey) {
            AiTocSetDiskKeyCache(*plainKeyOut, protectedKey);
            prefs->aiTocApiKey = protectedKey;
        } else if (!str::IsEmptyOrWhiteSpace(*plainKeyOut)) {
            logf("AiTocApiProtectPrefsSecretsForSave: failed to encrypt AiTocApiKey; keeping plaintext\n");
            prefs->aiTocApiKey = *plainKeyOut;
            *plainKeyOut = nullptr;
        }
    }

    if (gAiTocDiskProfilesProtected && gAiTocDiskProfilesProtected[0] &&
        str::Eq(prefs->aiTocApiProfiles, gAiTocDiskProfilesPlain)) {
        prefs->aiTocApiProfiles = str::Dup(gAiTocDiskProfilesProtected);
    } else {
        char* protectedProfiles = AiTocTransformProfilesJsonSecrets(prefs->aiTocApiProfiles, true);
        if (protectedProfiles) {
            AiTocSetDiskProfilesCache(*plainProfilesOut, protectedProfiles);
            prefs->aiTocApiProfiles = protectedProfiles;
        } else {
            prefs->aiTocApiProfiles = *plainProfilesOut;
            *plainProfilesOut = nullptr;
        }
    }
}

void AiTocApiRestorePrefsSecretsAfterSave(GlobalPrefs* prefs, char* plainKey, char* plainProfiles) {
    if (!prefs) {
        str::Free(plainKey);
        str::Free(plainProfiles);
        return;
    }
    if (plainKey) {
        if (prefs->aiTocApiKey != plainKey) {
            str::Free(prefs->aiTocApiKey);
        }
        prefs->aiTocApiKey = plainKey;
    }
    if (plainProfiles) {
        if (prefs->aiTocApiProfiles != plainProfiles) {
            str::Free(prefs->aiTocApiProfiles);
        }
        prefs->aiTocApiProfiles = plainProfiles;
    }
}
