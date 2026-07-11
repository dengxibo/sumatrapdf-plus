/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#ifdef __cplusplus
extern "C" {
#endif

// Convert Markdown to an HTML body fragment (no html/head/body wrapper).
// Returns 0 on success. Caller frees *htmlOut with Md4cFree().
int Md4cMarkdownToHtmlBody(const char* markdown, size_t markdownLen, char** htmlOut, size_t* htmlLenOut);
void Md4cFree(void* p);

#ifdef __cplusplus
}
#endif
