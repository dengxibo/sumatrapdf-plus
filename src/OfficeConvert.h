// Copyright 2026 Authors of SumatraPDF
// License: GPLv3
// Convert classic OLE .doc (and similar) to OOXML via Microsoft Word COM when
// Word is installed. Read-only viewing path — never edits the source file.

#pragma once

// Returns a newly allocated path to a temporary .docx (caller frees with
// str::Free), or nullptr if Word is unavailable / conversion fails.
// outErr may receive a short English reason (optional, static/owned by caller buffer).
char* ConvertOleOfficeToDocx(const char* srcPath, char* errOut, int errOutLen);

// True if path is the process-wide cached Word conversion output (do not delete yet).
bool IsCachedOleOfficeDocx(const char* path);

// Drop the cached .doc → .docx mapping so the next open converts the file again.
// Does not delete the temp docx; the engine that still has it open frees it.
void ForgetCachedOleOfficeDocx(const char* srcPath);
