/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "PdfTocEditModel.h"

PdfTocEditNode::~PdfTocEditNode() {
    str::Free(title);
    str::Free(uri);
    for (PdfTocEditNode* child : children) {
        delete child;
    }
}

void DeletePdfTocEditNodes(Vec<PdfTocEditNode*>& nodes) {
    for (PdfTocEditNode* node : nodes) {
        delete node;
    }
    nodes.Clear();
}

PdfTocEditNode* NewPdfTocEditNode(const char* title, const char* uri) {
    auto node = new PdfTocEditNode;
    node->title = str::Dup(title ? title : "");
    node->uri = str::Dup(uri);
    return node;
}

void CopyPdfTocPath(Vec<int>* dst, const Vec<int>& src) {
    if (!dst) {
        return;
    }
    dst->Clear();
    for (int idx : src) {
        dst->Append(idx);
    }
}

void PdfTocPathFromVec(PdfTocPath& dst, const Vec<int>& src) {
    int n = src.Size();
    int maxn = (int)dimof(dst.idx);
    ReportIf(n > maxn);
    if (n > maxn) {
        n = maxn;
    }
    dst.len = n;
    for (int i = 0; i < n; i++) {
        dst.idx[i] = src.At(i);
    }
}

void PdfTocPathToVec(const PdfTocPath& src, Vec<int>& dst) {
    dst.Clear();
    for (int i = 0; i < src.len; i++) {
        dst.Append(src.idx[i]);
    }
}

int ComparePdfTocPath(const PdfTocPath& a, const PdfTocPath& b) {
    int n = a.len < b.len ? a.len : b.len;
    for (int i = 0; i < n; i++) {
        if (a.idx[i] != b.idx[i]) {
            return a.idx[i] - b.idx[i];
        }
    }
    return a.len - b.len;
}

bool PdfTocPathStartsWith(const PdfTocPath& path, const PdfTocPath& prefix) {
    if (path.len < prefix.len) {
        return false;
    }
    for (int i = 0; i < prefix.len; i++) {
        if (path.idx[i] != prefix.idx[i]) {
            return false;
        }
    }
    return true;
}

void SortPdfTocPathsDocumentOrder(Vec<PdfTocPath>& paths, bool reverse) {
    int n = paths.Size();
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            int cmp = ComparePdfTocPath(paths.At(i), paths.At(j));
            bool swap = reverse ? cmp < 0 : cmp > 0;
            if (swap) {
                PdfTocPath tmp = paths.At(i);
                paths.At(i) = paths.At(j);
                paths.At(j) = tmp;
            }
        }
    }
}

void PruneNestedPdfTocPaths(Vec<PdfTocPath>& paths) {
    SortPdfTocPathsDocumentOrder(paths, false);
    Vec<PdfTocPath> kept;
    for (int i = 0; i < paths.Size(); i++) {
        const PdfTocPath& path = paths.At(i);
        bool nested = false;
        for (int k = 0; k < kept.Size(); k++) {
            if (PdfTocPathStartsWith(path, kept.At(k)) && path.len > kept.At(k).len) {
                nested = true;
                break;
            }
        }
        if (!nested) {
            kept.Append(path);
        }
    }
    paths = kept;
}

PdfTocEditNode* PdfTocNodeAtPath(Vec<PdfTocEditNode*>& roots, const Vec<int>& path) {
    Vec<PdfTocEditNode*>* nodes = &roots;
    PdfTocEditNode* node = nullptr;
    for (int idx : path) {
        if (!nodes->isValidIndex(idx)) {
            return nullptr;
        }
        node = nodes->At(idx);
        nodes = &node->children;
    }
    return node;
}

PdfTocEditNode* PdfTocNodeAtPath(Vec<PdfTocEditNode*>& roots, const PdfTocPath& path) {
    Vec<int> v;
    PdfTocPathToVec(path, v);
    return PdfTocNodeAtPath(roots, v);
}

Vec<PdfTocEditNode*>* PdfTocParentNodesAtPath(Vec<PdfTocEditNode*>& roots, const Vec<int>& path) {
    if (path.empty()) {
        return &roots;
    }
    Vec<PdfTocEditNode*>* nodes = &roots;
    for (int i = 0; i + 1 < path.Size(); i++) {
        int idx = path.At(i);
        if (!nodes->isValidIndex(idx)) {
            return nullptr;
        }
        nodes = &nodes->At(idx)->children;
    }
    return nodes;
}

static bool PdfTocPathForNodeRec(Vec<PdfTocEditNode*>& nodes, PdfTocEditNode* target, Vec<int>& pathOut) {
    for (int i = 0; i < nodes.Size(); i++) {
        PdfTocEditNode* node = nodes.At(i);
        pathOut.Append(i);
        if (node == target) {
            return true;
        }
        if (PdfTocPathForNodeRec(node->children, target, pathOut)) {
            return true;
        }
        pathOut.RemoveLast();
    }
    return false;
}

bool PdfTocPathForNode(Vec<PdfTocEditNode*>& roots, PdfTocEditNode* node, Vec<int>& pathOut) {
    pathOut.Clear();
    if (!node) {
        return false;
    }
    return PdfTocPathForNodeRec(roots, node, pathOut);
}

bool PdfTocPathForNode(Vec<PdfTocEditNode*>& roots, PdfTocEditNode* node, PdfTocPath& pathOut) {
    Vec<int> v;
    if (!PdfTocPathForNode(roots, node, v)) {
        pathOut.len = 0;
        return false;
    }
    PdfTocPathFromVec(pathOut, v);
    return true;
}

static bool FindPdfTocNodeSiblings(Vec<PdfTocEditNode*>& nodes, PdfTocEditNode* target, Vec<PdfTocEditNode*>** sibsOut,
                                   int* idxOut) {
    for (int i = 0; i < nodes.Size(); i++) {
        PdfTocEditNode* node = nodes.At(i);
        if (node == target) {
            *sibsOut = &nodes;
            *idxOut = i;
            return true;
        }
        if (FindPdfTocNodeSiblings(node->children, target, sibsOut, idxOut)) {
            return true;
        }
    }
    return false;
}

static bool UnlinkPdfTocNode(Vec<PdfTocEditNode*>& roots, PdfTocEditNode* node) {
    Vec<PdfTocEditNode*>* sibs = nullptr;
    int idx = -1;
    if (!FindPdfTocNodeSiblings(roots, node, &sibs, &idx)) {
        return false;
    }
    sibs->RemoveAt(idx);
    return true;
}

static bool PdfTocNodeContains(PdfTocEditNode* ancestor, PdfTocEditNode* node) {
    if (!ancestor || !node) {
        return false;
    }
    if (ancestor == node) {
        return true;
    }
    for (PdfTocEditNode* child : ancestor->children) {
        if (PdfTocNodeContains(child, node)) {
            return true;
        }
    }
    return false;
}

static void CollectPdfTocNodesFromPaths(Vec<PdfTocEditNode*>& roots, const Vec<PdfTocPath>& paths,
                                        Vec<PdfTocEditNode*>& nodesOut) {
    nodesOut.Clear();
    Vec<PdfTocPath> pruned;
    for (int i = 0; i < paths.Size(); i++) {
        pruned.Append(paths.At(i));
    }
    PruneNestedPdfTocPaths(pruned);
    for (int i = 0; i < pruned.Size(); i++) {
        PdfTocEditNode* node = PdfTocNodeAtPath(roots, pruned.At(i));
        if (node && !nodesOut.Contains(node)) {
            nodesOut.Append(node);
        }
    }
}

static void CollectResultPaths(Vec<PdfTocEditNode*>& roots, Vec<PdfTocEditNode*>& nodes,
                               Vec<PdfTocPath>* resultPathsOut) {
    if (!resultPathsOut) {
        return;
    }
    resultPathsOut->Clear();
    for (PdfTocEditNode* node : nodes) {
        PdfTocPath path;
        if (PdfTocPathForNode(roots, node, path)) {
            resultPathsOut->Append(path);
        }
    }
}

bool ApplyPdfTocEditToModel(Vec<PdfTocEditNode*>& roots, PdfTocEditAction action, const Vec<int>& path,
                            const char* title, const char* uri, Vec<int>* resultPathOut) {
    Vec<PdfTocEditNode*>* siblings = PdfTocParentNodesAtPath(roots, path);
    int idx = path.empty() ? -1 : path.Last();
    PdfTocEditNode* node = path.empty() ? nullptr : PdfTocNodeAtPath(roots, path);
    Vec<int> result;
    CopyPdfTocPath(&result, path);

    switch (action) {
        case PdfTocEditAction::AddAfter: {
            auto added = NewPdfTocEditNode(title, uri);
            if (path.empty()) {
                roots.Append(added);
                result.Append(roots.Size() - 1);
            } else if (siblings && node) {
                siblings->InsertAt(idx + 1, added);
                result.Last() = idx + 1;
            } else {
                delete added;
                return false;
            }
        } break;
        case PdfTocEditAction::AddChild: {
            if (!node) {
                return false;
            }
            node->children.InsertAt(0, NewPdfTocEditNode(title, uri));
            result.Append(0);
        } break;
        case PdfTocEditAction::Update:
            if (!node || str::IsEmpty(title)) {
                return false;
            }
            str::ReplaceWithCopy(&node->title, title);
            if (uri) {
                str::ReplaceWithCopy(&node->uri, uri);
            }
            break;
        case PdfTocEditAction::Delete:
            if (!siblings || !node) {
                return false;
            }
            siblings->RemoveAt(idx);
            delete node;
            result.Clear();
            break;
        case PdfTocEditAction::MoveUp:
            if (!siblings || !node || idx <= 0) {
                return false;
            }
            siblings->At(idx) = siblings->At(idx - 1);
            siblings->At(idx - 1) = node;
            result.Last() = idx - 1;
            break;
        case PdfTocEditAction::MoveDown:
            if (!siblings || !node || idx < 0 || idx + 1 >= siblings->Size()) {
                return false;
            }
            siblings->At(idx) = siblings->At(idx + 1);
            siblings->At(idx + 1) = node;
            result.Last() = idx + 1;
            break;
        case PdfTocEditAction::Promote: {
            if (!siblings || !node || path.Size() < 2) {
                return false;
            }
            Vec<int> parentPath;
            for (int i = 0; i + 1 < path.Size(); i++) {
                parentPath.Append(path.At(i));
            }
            Vec<PdfTocEditNode*>* parentSiblings = PdfTocParentNodesAtPath(roots, parentPath);
            if (!parentSiblings) {
                return false;
            }
            int parentIdx = parentPath.Last();
            siblings->RemoveAt(idx);
            parentSiblings->InsertAt(parentIdx + 1, node);
            CopyPdfTocPath(&result, parentPath);
            result.Last() = parentIdx + 1;
        } break;
        case PdfTocEditAction::Demote: {
            if (!siblings || !node || idx <= 0) {
                return false;
            }
            PdfTocEditNode* previous = siblings->At(idx - 1);
            siblings->RemoveAt(idx);
            previous->children.Append(node);
            result.Last() = idx - 1;
            result.Append(previous->children.Size() - 1);
        } break;
    }
    CopyPdfTocPath(resultPathOut, result);
    return true;
}

bool PdfTocEditDeleteMany(Vec<PdfTocEditNode*>& roots, const Vec<PdfTocPath>& paths) {
    Vec<PdfTocEditNode*> nodes;
    CollectPdfTocNodesFromPaths(roots, paths, nodes);
    if (nodes.empty()) {
        return false;
    }
    for (PdfTocEditNode* node : nodes) {
        if (!UnlinkPdfTocNode(roots, node)) {
            return false;
        }
    }
    for (PdfTocEditNode* node : nodes) {
        delete node;
    }
    return true;
}

bool PdfTocEditMoveUpMany(Vec<PdfTocEditNode*>& roots, const Vec<PdfTocPath>& paths, Vec<PdfTocPath>* resultPathsOut) {
    Vec<PdfTocEditNode*> moving;
    CollectPdfTocNodesFromPaths(roots, paths, moving);
    if (moving.empty()) {
        return false;
    }

    bool any = false;
    Vec<Vec<PdfTocEditNode*>*> seenSibs;
    for (PdfTocEditNode* node : moving) {
        Vec<PdfTocEditNode*>* sibs = nullptr;
        int idx = -1;
        if (!FindPdfTocNodeSiblings(roots, node, &sibs, &idx) || seenSibs.Contains(sibs)) {
            continue;
        }
        seenSibs.Append(sibs);

        Vec<int> selectedIdx;
        for (int i = 0; i < sibs->Size(); i++) {
            if (moving.Contains(sibs->At(i))) {
                selectedIdx.Append(i);
            }
        }
        int runStart = 0;
        while (runStart < selectedIdx.Size()) {
            int runEnd = runStart;
            while (runEnd + 1 < selectedIdx.Size() && selectedIdx.At(runEnd + 1) == selectedIdx.At(runEnd) + 1) {
                runEnd++;
            }
            int first = selectedIdx.At(runStart);
            int last = selectedIdx.At(runEnd);
            if (first > 0) {
                PdfTocEditNode* prev = sibs->At(first - 1);
                for (int i = first - 1; i < last; i++) {
                    sibs->At(i) = sibs->At(i + 1);
                }
                sibs->At(last) = prev;
                any = true;
            }
            runStart = runEnd + 1;
        }
    }
    if (!any) {
        return false;
    }
    CollectResultPaths(roots, moving, resultPathsOut);
    return true;
}

bool PdfTocEditMoveDownMany(Vec<PdfTocEditNode*>& roots, const Vec<PdfTocPath>& paths,
                            Vec<PdfTocPath>* resultPathsOut) {
    Vec<PdfTocEditNode*> moving;
    CollectPdfTocNodesFromPaths(roots, paths, moving);
    if (moving.empty()) {
        return false;
    }

    bool any = false;
    Vec<Vec<PdfTocEditNode*>*> seenSibs;
    for (PdfTocEditNode* node : moving) {
        Vec<PdfTocEditNode*>* sibs = nullptr;
        int idx = -1;
        if (!FindPdfTocNodeSiblings(roots, node, &sibs, &idx) || seenSibs.Contains(sibs)) {
            continue;
        }
        seenSibs.Append(sibs);

        Vec<int> selectedIdx;
        for (int i = 0; i < sibs->Size(); i++) {
            if (moving.Contains(sibs->At(i))) {
                selectedIdx.Append(i);
            }
        }
        int runEnd = selectedIdx.Size() - 1;
        while (runEnd >= 0) {
            int runStart = runEnd;
            while (runStart - 1 >= 0 && selectedIdx.At(runStart - 1) == selectedIdx.At(runStart) - 1) {
                runStart--;
            }
            int first = selectedIdx.At(runStart);
            int last = selectedIdx.At(runEnd);
            if (last + 1 < sibs->Size()) {
                PdfTocEditNode* next = sibs->At(last + 1);
                for (int i = last + 1; i > first; i--) {
                    sibs->At(i) = sibs->At(i - 1);
                }
                sibs->At(first) = next;
                any = true;
            }
            runEnd = runStart - 1;
        }
    }
    if (!any) {
        return false;
    }
    CollectResultPaths(roots, moving, resultPathsOut);
    return true;
}

bool PdfTocEditPromoteMany(Vec<PdfTocEditNode*>& roots, const Vec<PdfTocPath>& paths, Vec<PdfTocPath>* resultPathsOut) {
    Vec<PdfTocEditNode*> moving;
    CollectPdfTocNodesFromPaths(roots, paths, moving);
    if (moving.empty()) {
        return false;
    }

    struct PromoteRec {
        PdfTocEditNode* node = nullptr;
        PdfTocEditNode* parent = nullptr;
    };
    Vec<PromoteRec> recs;
    for (PdfTocEditNode* node : moving) {
        Vec<int> path;
        if (!PdfTocPathForNode(roots, node, path) || path.Size() < 2) {
            continue;
        }
        Vec<int> parentPath;
        for (int i = 0; i + 1 < path.Size(); i++) {
            parentPath.Append(path.At(i));
        }
        PdfTocEditNode* parent = PdfTocNodeAtPath(roots, parentPath);
        if (!parent) {
            continue;
        }
        PromoteRec rec;
        rec.node = node;
        rec.parent = parent;
        recs.Append(rec);
    }
    if (recs.empty()) {
        return false;
    }

    Vec<PdfTocEditNode*> promoted;
    for (int i = 0; i < recs.Size(); i++) {
        if (!UnlinkPdfTocNode(roots, recs.At(i).node)) {
            return false;
        }
        promoted.Append(recs.At(i).node);
    }

    Vec<PdfTocEditNode*> doneParents;
    for (int i = 0; i < recs.Size(); i++) {
        PdfTocEditNode* parent = recs.At(i).parent;
        if (doneParents.Contains(parent)) {
            continue;
        }
        doneParents.Append(parent);
        Vec<PdfTocEditNode*>* grandSibs = nullptr;
        int parentIdx = -1;
        if (!FindPdfTocNodeSiblings(roots, parent, &grandSibs, &parentIdx)) {
            return false;
        }
        int insertAt = parentIdx + 1;
        for (int k = 0; k < recs.Size(); k++) {
            if (recs.At(k).parent != parent) {
                continue;
            }
            grandSibs->InsertAt(insertAt, recs.At(k).node);
            insertAt++;
        }
    }
    CollectResultPaths(roots, promoted, resultPathsOut);
    return true;
}

bool PdfTocEditDemoteMany(Vec<PdfTocEditNode*>& roots, const Vec<PdfTocPath>& paths, Vec<PdfTocPath>* resultPathsOut) {
    Vec<PdfTocEditNode*> moving;
    CollectPdfTocNodesFromPaths(roots, paths, moving);
    if (moving.empty()) {
        return false;
    }

    struct DemoteRec {
        PdfTocEditNode* node = nullptr;
        PdfTocEditNode* destParent = nullptr;
    };
    Vec<DemoteRec> recs;
    for (PdfTocEditNode* node : moving) {
        Vec<PdfTocEditNode*>* sibs = nullptr;
        int idx = -1;
        if (!FindPdfTocNodeSiblings(roots, node, &sibs, &idx) || idx <= 0) {
            continue;
        }
        int prev = idx - 1;
        while (prev >= 0 && moving.Contains(sibs->At(prev))) {
            prev--;
        }
        if (prev < 0) {
            continue;
        }
        DemoteRec rec;
        rec.node = node;
        rec.destParent = sibs->At(prev);
        recs.Append(rec);
    }
    if (recs.empty()) {
        return false;
    }

    Vec<PdfTocEditNode*> demoted;
    for (int i = 0; i < recs.Size(); i++) {
        if (!UnlinkPdfTocNode(roots, recs.At(i).node)) {
            return false;
        }
        recs.At(i).destParent->children.Append(recs.At(i).node);
        demoted.Append(recs.At(i).node);
    }
    CollectResultPaths(roots, demoted, resultPathsOut);
    return true;
}

bool PdfTocEditMoveMany(Vec<PdfTocEditNode*>& roots, const Vec<PdfTocPath>& srcPaths, const Vec<int>& destPath,
                        PdfTocDropPos pos, Vec<PdfTocPath>* resultPathsOut) {
    Vec<PdfTocEditNode*> moving;
    CollectPdfTocNodesFromPaths(roots, srcPaths, moving);
    if (moving.empty()) {
        return false;
    }

    PdfTocEditNode* dest = nullptr;
    Vec<PdfTocEditNode*>* destSibs = &roots;
    int destIdx = 0;
    if (!destPath.empty()) {
        dest = PdfTocNodeAtPath(roots, destPath);
        if (!dest) {
            return false;
        }
        for (PdfTocEditNode* node : moving) {
            if (PdfTocNodeContains(node, dest)) {
                return false;
            }
        }
        if (!FindPdfTocNodeSiblings(roots, dest, &destSibs, &destIdx)) {
            return false;
        }
    } else if (pos == PdfTocDropPos::Child) {
        return false;
    }

    if (pos == PdfTocDropPos::Child) {
        for (PdfTocEditNode* node : moving) {
            if (!UnlinkPdfTocNode(roots, node)) {
                return false;
            }
            dest->children.Append(node);
        }
        CollectResultPaths(roots, moving, resultPathsOut);
        return true;
    }

    int insertAt = destPath.empty() ? (pos == PdfTocDropPos::Before ? 0 : roots.Size()) : destIdx;
    if (!destPath.empty() && pos == PdfTocDropPos::After) {
        insertAt++;
    }
    int beforeInsert = 0;
    for (PdfTocEditNode* node : moving) {
        int idx = destSibs->Find(node);
        if (idx >= 0 && idx < insertAt) {
            beforeInsert++;
        }
    }
    insertAt -= beforeInsert;

    for (PdfTocEditNode* node : moving) {
        if (!UnlinkPdfTocNode(roots, node)) {
            return false;
        }
    }
    if (insertAt < 0) {
        insertAt = 0;
    }
    if (insertAt > destSibs->Size()) {
        insertAt = destSibs->Size();
    }
    for (int i = 0; i < moving.Size(); i++) {
        destSibs->InsertAt(insertAt + i, moving.At(i));
    }
    CollectResultPaths(roots, moving, resultPathsOut);
    return true;
}
