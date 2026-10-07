#pragma once

// A reading note has quoted source text, editable commentary, and optional
// provenance. Geometry belongs to provenance, never to the quotation.
inline void AppendReadingNoteMarkdown(StrBuilder& out, int number, const char* type, const char* excerpt,
                                      const char* note, const char* author, time_t date, bool geometry,
                                      int pageNo = 0) {
    out.AppendFmt("### %02d\n\n", number);
    if (!geometry && !str::IsEmptyOrWhiteSpace(excerpt)) {
        out.Append("> ");
        for (const char* p = excerpt; *p; p++) {
            if (*p == '\r') continue;
            if (*p == '\\' || *p == '`' || *p == '*' || *p == '_' || *p == '[' || *p == ']' || *p == '<' || *p == '>')
                out.AppendChar('\\');
            out.AppendChar(*p);
            if (*p == '\n' && p[1]) out.Append("> ");
        }
        out.Append("\n\n");
    }
    if (!str::IsEmptyOrWhiteSpace(note)) {
        out.AppendFmt("**%s**\n\n", _TRA("Note"));
        out.Append(note);
        out.Append("\n\n");
    }
    out.Append("<details>\n");
    out.AppendFmt("<summary>%s</summary>\n\n", type);
    if (pageNo > 0) out.AppendFmt("%s %d\n\n", _TRA("Page"), pageNo);
    if (geometry && !str::IsEmpty(excerpt)) out.AppendFmt("`%s`\n\n", excerpt);
    if (!str::IsEmpty(author)) out.AppendFmt("%s %s\n\n", _TRA("Author:"), author);
    if (date > 0) {
        struct tm utc{};
        gmtime_s(&utc, &date);
        char value[64]{};
        strftime(value, sizeof(value), "%Y-%m-%d %H:%M UTC", &utc);
        out.AppendFmt("%s %s\n\n", _TRA("Date:"), value);
    }
    out.Append("</details>\n\n");
}
