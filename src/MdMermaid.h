/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Replace ```mermaid fenced blocks in an md4c HTML body with rendered SVG.
// Uses WebView2 on the UI thread when available. Returns nullptr on OOM.
char* MdReplaceMermaidBlocks(const char* bodyHtml, size_t bodyLen, size_t* outLen);

bool MdBodyHasMermaidBlocks(const char* bodyHtml, size_t bodyLen);

void MdMermaidShutdown();
