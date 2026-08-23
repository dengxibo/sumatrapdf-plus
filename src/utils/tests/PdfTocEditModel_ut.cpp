/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "PdfTocEditModel.h"
#include "utils/UtAssert.h"

static PdfTocEditNode* MakeNode(const char* title) {
    return NewPdfTocEditNode(title, "uri");
}

static Vec<int> PathOf(int a, int b = -1, int c = -1) {
    Vec<int> p;
    p.Append(a);
    if (b >= 0) {
        p.Append(b);
    }
    if (c >= 0) {
        p.Append(c);
    }
    return p;
}

static PdfTocPath TP(int a, int b = -1, int c = -1) {
    PdfTocPath p;
    PdfTocPathFromVec(p, PathOf(a, b, c));
    return p;
}

static void DumpRec(StrBuilder& s, Vec<PdfTocEditNode*>& nodes) {
    for (int i = 0; i < nodes.Size(); i++) {
        if (i > 0) {
            s.Append(", ");
        }
        PdfTocEditNode* n = nodes.At(i);
        s.Append(n->title ? n->title : "");
        if (!n->children.empty()) {
            s.Append("[");
            DumpRec(s, n->children);
            s.Append("]");
        }
    }
}

static void ExpectDump(Vec<PdfTocEditNode*>& roots, const char* expected) {
    StrBuilder s;
    DumpRec(s, roots);
    utassert(str::Eq(s.LendData(), expected));
}

static Vec<PdfTocEditNode*> SampleTree() {
    auto a = MakeNode("A");
    auto b = MakeNode("B");
    b->children.Append(MakeNode("B1"));
    b->children.Append(MakeNode("B2"));
    auto c = MakeNode("C");
    Vec<PdfTocEditNode*> roots;
    roots.Append(a);
    roots.Append(b);
    roots.Append(c);
    return roots;
}

static void ExpectPathEq(const Vec<int>& got, int a, int b = -1, int c = -1) {
    Vec<int> want = PathOf(a, b, c);
    utassert(got.Size() == want.Size());
    for (int i = 0; i < got.Size(); i++) {
        utassert(got.At(i) == want.At(i));
    }
}

void PdfTocEditModel_UnitTests() {
    {
        Vec<PdfTocEditNode*> roots = SampleTree();
        ExpectDump(roots, "A, B[B1, B2], C");
        DeletePdfTocEditNodes(roots);
    }

    {
        Vec<PdfTocEditNode*> roots = SampleTree();
        Vec<int> result;
        utassert(ApplyPdfTocEditToModel(roots, PdfTocEditAction::MoveUp, PathOf(2), nullptr, nullptr, &result));
        ExpectDump(roots, "A, C, B[B1, B2]");
        ExpectPathEq(result, 1);
        DeletePdfTocEditNodes(roots);
    }

    {
        Vec<PdfTocEditNode*> roots = SampleTree();
        utassert(ApplyPdfTocEditToModel(roots, PdfTocEditAction::MoveDown, PathOf(0), nullptr, nullptr, nullptr));
        ExpectDump(roots, "B[B1, B2], A, C");
        DeletePdfTocEditNodes(roots);
    }

    {
        Vec<PdfTocEditNode*> roots = SampleTree();
        utassert(ApplyPdfTocEditToModel(roots, PdfTocEditAction::Promote, PathOf(1, 0), nullptr, nullptr, nullptr));
        ExpectDump(roots, "A, B[B2], B1, C");
        DeletePdfTocEditNodes(roots);
    }

    {
        Vec<PdfTocEditNode*> roots = SampleTree();
        utassert(ApplyPdfTocEditToModel(roots, PdfTocEditAction::Demote, PathOf(2), nullptr, nullptr, nullptr));
        ExpectDump(roots, "A, B[B1, B2, C]");
        DeletePdfTocEditNodes(roots);
    }

    {
        Vec<PdfTocEditNode*> roots = SampleTree();
        utassert(ApplyPdfTocEditToModel(roots, PdfTocEditAction::Delete, PathOf(1), nullptr, nullptr, nullptr));
        ExpectDump(roots, "A, C");
        DeletePdfTocEditNodes(roots);
    }

    {
        Vec<PdfTocEditNode*> roots = SampleTree();
        Vec<PdfTocPath> paths;
        paths.Append(TP(0));
        paths.Append(TP(2));
        utassert(PdfTocEditDeleteMany(roots, paths));
        ExpectDump(roots, "B[B1, B2]");
        DeletePdfTocEditNodes(roots);
    }

    {
        Vec<PdfTocEditNode*> roots = SampleTree();
        Vec<PdfTocPath> paths;
        paths.Append(TP(1));
        paths.Append(TP(1, 0));
        utassert(PdfTocEditDeleteMany(roots, paths));
        ExpectDump(roots, "A, C");
        DeletePdfTocEditNodes(roots);
    }

    {
        Vec<PdfTocPath> paths;
        paths.Append(TP(1, 0));
        paths.Append(TP(1));
        paths.Append(TP(1, 1));
        PruneNestedPdfTocPaths(paths);
        utassert(paths.Size() == 1);
        utassert(paths.At(0).len == 1 && paths.At(0).idx[0] == 1);
    }

    {
        Vec<PdfTocEditNode*> roots = SampleTree();
        Vec<PdfTocPath> src;
        src.Append(TP(1));
        utassert(PdfTocEditMoveMany(roots, src, PathOf(2), PdfTocDropPos::After, nullptr));
        ExpectDump(roots, "A, C, B[B1, B2]");
        DeletePdfTocEditNodes(roots);
    }

    {
        Vec<PdfTocEditNode*> roots = SampleTree();
        Vec<PdfTocPath> src;
        src.Append(TP(1));
        utassert(PdfTocEditMoveMany(roots, src, PathOf(0), PdfTocDropPos::Child, nullptr));
        ExpectDump(roots, "A[B[B1, B2]], C");
        DeletePdfTocEditNodes(roots);
    }

    {
        Vec<PdfTocEditNode*> roots = SampleTree();
        Vec<PdfTocPath> src;
        src.Append(TP(1));
        utassert(!PdfTocEditMoveMany(roots, src, PathOf(1), PdfTocDropPos::Child, nullptr));
        utassert(!PdfTocEditMoveMany(roots, src, PathOf(1, 0), PdfTocDropPos::Before, nullptr));
        ExpectDump(roots, "A, B[B1, B2], C");
        DeletePdfTocEditNodes(roots);
    }

    {
        Vec<PdfTocEditNode*> roots = SampleTree();
        Vec<PdfTocPath> paths;
        paths.Append(TP(1));
        paths.Append(TP(2));
        utassert(PdfTocEditMoveUpMany(roots, paths, nullptr));
        ExpectDump(roots, "B[B1, B2], C, A");
        DeletePdfTocEditNodes(roots);
    }

    {
        Vec<PdfTocEditNode*> roots = SampleTree();
        Vec<PdfTocPath> paths;
        paths.Append(TP(0));
        utassert(!PdfTocEditMoveUpMany(roots, paths, nullptr));
        ExpectDump(roots, "A, B[B1, B2], C");
        DeletePdfTocEditNodes(roots);
    }

    {
        Vec<PdfTocEditNode*> roots = SampleTree();
        Vec<PdfTocPath> paths;
        paths.Append(TP(1, 0));
        paths.Append(TP(1, 1));
        utassert(PdfTocEditPromoteMany(roots, paths, nullptr));
        ExpectDump(roots, "A, B, B1, B2, C");
        DeletePdfTocEditNodes(roots);
    }

    {
        Vec<PdfTocEditNode*> roots = SampleTree();
        Vec<PdfTocPath> paths;
        paths.Append(TP(1));
        paths.Append(TP(2));
        utassert(PdfTocEditDemoteMany(roots, paths, nullptr));
        ExpectDump(roots, "A[B[B1, B2], C]");
        DeletePdfTocEditNodes(roots);
    }

    {
        Vec<PdfTocEditNode*> roots = SampleTree();
        Vec<PdfTocPath> src;
        src.Append(TP(1));
        src.Append(TP(2));
        utassert(PdfTocEditMoveMany(roots, src, PathOf(0), PdfTocDropPos::Before, nullptr));
        ExpectDump(roots, "B[B1, B2], C, A");
        DeletePdfTocEditNodes(roots);
    }

    {
        Vec<PdfTocEditNode*> roots = SampleTree();
        Vec<int> result;
        utassert(ApplyPdfTocEditToModel(roots, PdfTocEditAction::AddAfter, PathOf(0), "A2", "u", &result));
        ExpectDump(roots, "A, A2, B[B1, B2], C");
        ExpectPathEq(result, 1);
        utassert(ApplyPdfTocEditToModel(roots, PdfTocEditAction::AddChild, PathOf(0), "A1", "u", &result));
        ExpectDump(roots, "A[A1], A2, B[B1, B2], C");
        ExpectPathEq(result, 0, 0);
        utassert(ApplyPdfTocEditToModel(roots, PdfTocEditAction::AddChild, PathOf(2), "B0", "u", &result));
        ExpectDump(roots, "A[A1], A2, B[B0, B1, B2], C");
        ExpectPathEq(result, 2, 0);
        DeletePdfTocEditNodes(roots);
    }
}
