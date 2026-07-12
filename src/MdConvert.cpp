/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/FileUtil.h"
#include "utils/WinUtil.h"

#include "MdConvert.h"
#include "Md4cHtml.h"

#include "utils/Log.h"

// Document CSS is parsed with the HTML. MuPDF requires explicit border colors
// (not currentColor) and user CSS must be set before fz_open_document_with_stream.
static const char* kMdDocumentCss = R"(
body {
  margin: 0;
  padding: 1.4em 2em 2.4em;
  line-height: 1.65;
  word-wrap: break-word;
  font-family: sans-serif !important;
  background-color: #ffffff;
  color: #24292f;
}
h1, h2, h3, h4, h5, h6 {
  line-height: 1.25;
  margin: 1.35em 0 0.55em;
  font-weight: 600;
}
h1 { font-size: 2em; margin-top: 0; padding-bottom: 0.25em; border-bottom: 1px solid #d0d7de; }
h2 { font-size: 1.55em; padding-bottom: 0.2em; border-bottom: 1px solid #d0d7de; }
h3 { font-size: 1.25em; }
h4 { font-size: 1.05em; }
p, ul, ol, dl, blockquote, pre, table, hr { margin: 0.85em 0; }
ul, ol { padding-left: 1.6em; }
li { margin: 0.25em 0; }
li > ul, li > ol { margin: 0.2em 0; }
blockquote {
  margin-left: 0;
  padding: 0.2em 0 0.2em 1em;
  border-left: 4px solid #d0d7de;
}
code, kbd, samp, pre {
  font-family: monospace !important;
  font-size: 0.92em;
  font-style: normal !important;
}
p code, li code, td code, th code, h1 code, h2 code, h3 code, h4 code, h5 code, h6 code, dt code, dd code, blockquote code {
  padding: 0.15em 0.35em;
  border-radius: 4px;
  background-color: #eff1f3;
}
pre {
  display: block;
  padding: 0.85em 1em;
  border-radius: 6px;
  overflow-x: auto;
  line-height: 1.45;
  white-space: pre-wrap;
  background-color: #f6f8fa;
  color: #24292f;
  border: none;
}
pre code {
  padding: 0;
  background-color: transparent;
  color: inherit;
}
table {
  border-collapse: collapse;
  width: 100%;
}
th, td {
  border: 1px solid #808080;
  padding: 0.45em 0.65em;
  text-align: left;
  vertical-align: top;
}
thead th {
  font-weight: 600;
  background-color: #f6f8fa;
}
tbody td {
  background-color: transparent;
}
img { max-width: 100%; height: auto; }
hr {
  border: none;
  border-top: 1px solid #808080;
  height: 0;
  margin: 2em 0;
}
a { color: #0969da; text-decoration: underline; }
del { opacity: 0.75; }
input[type="checkbox"] { margin-right: 0.35em; vertical-align: middle; }
)";

static TempStr EscapeHtmlTextTemp(const char* s) {
    TempStr t = str::ReplaceTemp(s, "&", "&amp;");
    if (!t) {
        return (TempStr) "";
    }
    t = str::ReplaceTemp(t, "<", "&lt;");
    if (!t) {
        return (TempStr) "";
    }
    t = str::ReplaceTemp(t, ">", "&gt;");
    return t;
}

ByteSlice MdFileToHTML(const char* path) {
    if (str::IsEmpty(path)) {
        return {};
    }

    ByteSlice fd = file::ReadFile(path);
    if (fd.empty()) {
        logf("MdFileToHTML: failed to read '%s'\n", path);
        return {};
    }

    TempStr utf8 = strconv::UnknownToUtf8Temp((const char*)fd.data(), fd.size());
    fd.Free();
    if (!utf8) {
        return {};
    }

    char* bodyHtml = nullptr;
    size_t bodyLen = 0;
    int ok = Md4cMarkdownToHtmlBody(utf8, str::Len(utf8), &bodyHtml, &bodyLen);
    if (ok != 0 || !bodyHtml) {
        logf("MdFileToHTML: md_html failed for '%s'\n", path);
        Md4cFree(bodyHtml);
        return {};
    }
    defer {
        Md4cFree(bodyHtml);
    };

    TempStr title = path::GetBaseNameTemp(path::GetPathNoExtTemp(path));
    TempStr titleEsc = EscapeHtmlTextTemp(title);
    if (!titleEsc) {
        titleEsc = (TempStr) "Markdown";
    }

    StrBuilder d;
    d.Append("<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n");
    d.Append("<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n<title>");
    if (!d.Append(titleEsc)) {
        return {};
    }
    d.Append("</title>\n<style>\n");
    if (!d.Append(kMdDocumentCss)) {
        return {};
    }
    d.Append("\n</style>\n</head>\n<body>\n");
    if (!d.Append(bodyHtml, bodyLen)) {
        return {};
    }
    d.Append("\n</body>\n</html>\n");

    size_t sz = d.size();
    return {(u8*)d.StealData(), sz};
}
