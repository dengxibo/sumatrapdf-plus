/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#ifndef PdfTocEditModel_h
#define PdfTocEditModel_h

#include "utils/BaseUtil.h"

// In-memory PDF outline tree used by EngineMupdf TOC editing and unit tests.

enum class PdfTocEditAction {
    AddAfter,
    AddChild,
    Update,
    Delete,
    MoveUp,
    MoveDown,
    Promote,
    Demote,
};

enum class PdfTocDropPos {
    Before,
    After,
    Child,
};

struct PdfTocEditNode {
    char* title = nullptr;
    char* uri = nullptr;
    bool isOpen = false;
    int flags = 0;
    int r = 0;
    int g = 0;
    int b = 0;
    Vec<PdfTocEditNode*> children;

    ~PdfTocEditNode();
};

// POD outline path so it can live in Vec<> (Vec only memcpy's elements).
struct PdfTocPath {
    int idx[16]{};
    int len = 0;
};

void DeletePdfTocEditNodes(Vec<PdfTocEditNode*>& nodes);
PdfTocEditNode* NewPdfTocEditNode(const char* title, const char* uri);
void CopyPdfTocPath(Vec<int>* dst, const Vec<int>& src);
void PdfTocPathFromVec(PdfTocPath& dst, const Vec<int>& src);
void PdfTocPathToVec(const PdfTocPath& src, Vec<int>& dst);
int ComparePdfTocPath(const PdfTocPath& a, const PdfTocPath& b);
bool PdfTocPathStartsWith(const PdfTocPath& path, const PdfTocPath& prefix);
void SortPdfTocPathsDocumentOrder(Vec<PdfTocPath>& paths, bool reverse);
void PruneNestedPdfTocPaths(Vec<PdfTocPath>& paths);
PdfTocEditNode* PdfTocNodeAtPath(Vec<PdfTocEditNode*>& roots, const Vec<int>& path);
PdfTocEditNode* PdfTocNodeAtPath(Vec<PdfTocEditNode*>& roots, const PdfTocPath& path);
Vec<PdfTocEditNode*>* PdfTocParentNodesAtPath(Vec<PdfTocEditNode*>& roots, const Vec<int>& path);
bool PdfTocPathForNode(Vec<PdfTocEditNode*>& roots, PdfTocEditNode* node, Vec<int>& pathOut);
bool PdfTocPathForNode(Vec<PdfTocEditNode*>& roots, PdfTocEditNode* node, PdfTocPath& pathOut);

bool ApplyPdfTocEditToModel(Vec<PdfTocEditNode*>& roots, PdfTocEditAction action, const Vec<int>& path,
                            const char* title, const char* uri, Vec<int>* resultPathOut);
bool PdfTocEditDeleteMany(Vec<PdfTocEditNode*>& roots, const Vec<PdfTocPath>& paths);
bool PdfTocEditMoveUpMany(Vec<PdfTocEditNode*>& roots, const Vec<PdfTocPath>& paths, Vec<PdfTocPath>* resultPathsOut);
bool PdfTocEditMoveDownMany(Vec<PdfTocEditNode*>& roots, const Vec<PdfTocPath>& paths, Vec<PdfTocPath>* resultPathsOut);
bool PdfTocEditPromoteMany(Vec<PdfTocEditNode*>& roots, const Vec<PdfTocPath>& paths, Vec<PdfTocPath>* resultPathsOut);
bool PdfTocEditDemoteMany(Vec<PdfTocEditNode*>& roots, const Vec<PdfTocPath>& paths, Vec<PdfTocPath>* resultPathsOut);
bool PdfTocEditMoveMany(Vec<PdfTocEditNode*>& roots, const Vec<PdfTocPath>& srcPaths, const Vec<int>& destPath,
                        PdfTocDropPos pos, Vec<PdfTocPath>* resultPathsOut);

#endif
