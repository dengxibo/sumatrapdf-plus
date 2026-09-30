/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"

#include "EpubMediaOverlay.h"

#include "utils/UtAssert.h"

static bool ClockIs(const char* s, i64 expectUs) {
    i64 us = -12345;
    return EpubParseClockValue(s, &us) && us == expectUs;
}

static bool ClockRejected(const char* s) {
    i64 us = 0;
    return !EpubParseClockValue(s, &us);
}

static void ClockTests() {
    constexpr i64 S = 1000000;
    // EPUB 3.3 H.4 examples
    utassert(ClockIs("5:34:31.396", (5 * 3600 + 34 * 60 + 31) * S + 396000));
    utassert(ClockIs("124:59:36", (124 * 3600 + 59 * 60 + 36) * S));
    utassert(ClockIs("0:05:01.2", (5 * 60 + 1) * S + 200000));
    utassert(ClockIs("0:00:04", 4 * S));
    utassert(ClockIs("09:58", (9 * 60 + 58) * S));
    utassert(ClockIs("00:56.78", 56 * S + 780000));
    utassert(ClockIs("76.2s", 76 * S + 200000));
    utassert(ClockIs("7.75h", (7 * 3600 + 45 * 60) * S));
    utassert(ClockIs("13min", 13 * 60 * S));
    utassert(ClockIs("2345ms", 2345000));
    utassert(ClockIs("12.345", 12 * S + 345000));
    utassert(ClockIs(" 3s ", 3 * S));
    utassert(ClockIs("0s", 0));
    utassert(ClockIs("0:23:23.84", (23 * 60 + 23) * S + 840000));
    utassert(ClockIs("1.5ms", 1500));

    utassert(ClockRejected(""));
    utassert(ClockRejected("   "));
    utassert(ClockRejected("-1s"));
    utassert(ClockRejected("1:60:00"));
    utassert(ClockRejected("00:61"));
    utassert(ClockRejected("1:2:3"));
    utassert(ClockRejected("abc"));
    utassert(ClockRejected("1.2.3"));
    utassert(ClockRejected("5x"));
    utassert(ClockRejected("5S"));
    utassert(ClockRejected("1:00:00:00"));
    utassert(ClockRejected("12."));
    utassert(ClockRejected("99999999999999999999s"));
}

static bool ResolvesTo(const char* base, const char* href, const char* expectPath, const char* expectFrag) {
    char* frag = nullptr;
    char* path = EpubResolvePath(base, href, &frag);
    bool ok = str::Eq(path, expectPath) && str::Eq(frag, expectFrag);
    str::Free(path);
    str::Free(frag);
    return ok;
}

static bool PathRejected(const char* base, const char* href) {
    char* frag = nullptr;
    char* path = EpubResolvePath(base, href, &frag);
    bool rejected = path == nullptr;
    str::Free(path);
    str::Free(frag);
    return rejected;
}

static void PathTests() {
    utassert(ResolvesTo("OEBPS", "Text/ch1.xhtml", "OEBPS/Text/ch1.xhtml", nullptr));
    utassert(ResolvesTo("OEBPS/Text", "../Audio/a.mp3", "OEBPS/Audio/a.mp3", nullptr));
    utassert(ResolvesTo("OEBPS/Text", "ch1.xhtml#p1", "OEBPS/Text/ch1.xhtml", "p1"));
    utassert(ResolvesTo("OEBPS", "./a/../b/c%20d.xhtml#x%41", "OEBPS/b/c d.xhtml", "xA"));
    utassert(ResolvesTo("", "content.opf", "content.opf", nullptr));
    utassert(ResolvesTo("OEBPS", "ch.xhtml#", "OEBPS/ch.xhtml", nullptr));
    utassert(ResolvesTo("OEBPS/Text", "/OEBPS/x.xhtml", "OEBPS/x.xhtml", nullptr));
    utassert(ResolvesTo("OEBPS", "a.xhtml?q=1#f", "OEBPS/a.xhtml", "f"));

    utassert(PathRejected("OEBPS", "http://example.com/a.mp3"));
    utassert(PathRejected("OEBPS", "https://example.com/a.mp3"));
    utassert(PathRejected("OEBPS", "file:///C:/Windows/win.ini"));
    utassert(PathRejected("OEBPS", "data:audio/mp3;base64,AAAA"));
    utassert(PathRejected("OEBPS", "javascript:alert(1)"));
    utassert(PathRejected("OEBPS", "C:\\Windows\\win.ini"));
    utassert(PathRejected("OEBPS", "\\\\server\\share\\a.mp3"));
    utassert(PathRejected("OEBPS", "//server/a.mp3"));
    utassert(PathRejected("OEBPS", "../../etc/passwd"));
    utassert(PathRejected("OEBPS", "%2e%2e/%2e%2e/x"));
    utassert(PathRejected("OEBPS", "a%5cb"));
    utassert(PathRejected("OEBPS", ""));
    utassert(PathRejected("OEBPS", nullptr));
}

static const char* kContainer = R"XML(<?xml version="1.0"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles>
    <rootfile full-path="OEBPS/package.opf" media-type="application/oebps-package+xml"/>
  </rootfiles>
</container>)XML";

static const char* kOpf = R"XML(<?xml version="1.0" encoding="UTF-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="uid">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:identifier id="uid">urn:uuid:1</dc:identifier>
    <dc:title>T</dc:title>
    <meta property="media:duration" refines="#mo1">0:00:20</meta>
    <meta property="media:duration" refines="#mo2">15.5s</meta>
    <meta property="media:duration">0:00:35.5</meta>
    <meta property="media:narrator">Jane</meta>
    <meta property="media:active-class">-epub-media-overlay-active</meta>
    <meta property="media:playback-active-class">-epub-media-overlay-playing</meta>
    <meta property="rendition:layout">reflowable</meta>
  </metadata>
  <manifest>
    <item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav scripted"/>
    <item id="c1" href="Text/c1.xhtml" media-type="application/xhtml+xml" media-overlay="mo1"/>
    <item id="c2" href="Text/c2.xhtml" media-type="application/xhtml+xml" media-overlay="mo2"/>
    <item id="c3" href="Text/c3.xhtml" media-type="application/xhtml+xml" media-overlay="missing"/>
    <item id="c4" href="Text/c4.xhtml" media-type="application/xhtml+xml" media-overlay="notsmil"/>
    <item id="mo1" href="Smil/c1.smil" media-type="application/smil+xml"/>
    <item id="mo2" href="Smil/c2.smil" media-type="application/smil+xml"/>
    <item id="notsmil" href="x.css" media-type="text/css"/>
    <item id="remote" href="https://example.com/a.mp3" media-type="audio/mpeg"/>
  </manifest>
  <spine page-progression-direction="rtl">
    <itemref idref="c1"/>
    <itemref idref="c2" linear="no" properties="rendition:layout-pre-paginated"/>
    <itemref idref="c3"/>
    <itemref idref="c4"/>
    <itemref idref="nope"/>
  </spine>
</package>)XML";

static const char* kSmil1 = R"XML(<?xml version="1.0" encoding="UTF-8"?>
<smil xmlns="http://www.w3.org/ns/SMIL" xmlns:epub="http://www.idpf.org/2007/ops" version="3.0">
  <body epub:textref="../Text/c1.xhtml">
    <!-- a comment <par> -->
    <par id="p1">
      <text src="../Text/c1.xhtml#s1"/>
      <audio src="../Audio/c1.mp3" clipBegin="0s" clipEnd="5s"/>
    </par>
    <seq id="fig" epub:textref="../Text/c1.xhtml#fig" epub:type="figure">
      <par id="p2"><text src="../Text/c1.xhtml#cap"/><audio src="../Audio/c1.mp3" clipBegin="0:00:05" clipEnd="0:00:10.250"/></par>
      <seq id="fn" epub:textref="../Text/c1.xhtml#fn" epub:type="footnote">
        <par id="p3"><text src="../Text/c1.xhtml#fn1"/><audio src="../Audio/c1.mp3" clipBegin="10.25s" clipEnd="12s"/></par>
      </seq>
    </seq>
    <par id="p4"><text src="../Text/c1.xhtml#s2"/></par>
    <par id="p5"><audio src="../Audio/c1.mp3" clipBegin="12s" clipEnd="13s"/></par>
    <par id="p6"><text src="../Text/c1.xhtml#s3"/><audio src="../Audio/c1.mp3" clipBegin="13s" clipEnd="12s"/></par>
    <par id="p7" epub:type="pagebreak"><text src="../Text/c1.xhtml#pg2"/><audio src="https://evil.example/a.mp3"/></par>
    <par id="p8"><text src="../Text/c1.xhtml#s4"/><audio src="../Audio/c1.mp3" clipBegin="14s"/></par>
  </body>
</smil>)XML";

static const char* kSmil2Prefixed = R"XML(<smil:smil xmlns:smil="http://www.w3.org/ns/SMIL" version="3.0">
<smil:body><smil:par id="a"><smil:text src="../Text/c2.xhtml#t1"/><smil:audio src="../Audio/c2.mp3" clipBegin="0s" clipEnd="15.5s"/></smil:par></smil:body>
</smil:smil>)XML";

static void PackageTests() {
    char* opfPath = EpubParseContainerXml(kContainer, str::Len(kContainer));
    utassert(str::Eq(opfPath, "OEBPS/package.opf"));

    EpubPackage pkg;
    utassert(EpubParsePackage(&pkg, opfPath, kOpf, str::Len(kOpf)));
    str::Free(opfPath);
    utassert(str::Eq(pkg.version, "3.0"));
    utassert(str::Eq(pkg.navPath, "OEBPS/nav.xhtml"));
    utassert(str::Eq(pkg.activeClass, "-epub-media-overlay-active"));
    utassert(str::Eq(pkg.playbackActiveClass, "-epub-media-overlay-playing"));
    utassert(str::Eq(pkg.narrator, "Jane"));
    utassert(str::Eq(pkg.pageProgressionDirection, "rtl"));
    utassert(pkg.totalDurationUs == 35500000);
    utassert(!pkg.prePaginated);
    utassert(pkg.spine.Size() == 5);
    utassert(pkg.spine[0].linear && !pkg.spine[1].linear);
    utassert(!pkg.spine[0].prePaginated && pkg.spine[1].prePaginated);
    utassert(pkg.HasMediaOverlays());
    utassert(pkg.overlays.Size() == 2);
    utassert(pkg.spine[0].overlayIdx == 0 && pkg.spine[1].overlayIdx == 1);
    utassert(pkg.spine[2].overlayIdx == -1 && pkg.spine[3].overlayIdx == -1 && pkg.spine[4].manifestIdx == -1);
    utassert(str::Eq(pkg.SpinePath(1), "OEBPS/Text/c2.xhtml"));
    utassert(pkg.FindSpineByPath("OEBPS/Text/c2.xhtml") == 1);
    int mo1 = pkg.FindManifestById("mo1");
    utassert(mo1 >= 0 && pkg.manifest[mo1].durationUs == 20000000);
    utassert(str::Eq(pkg.overlays[0]->smilPath, "OEBPS/Smil/c1.smil"));
    int remote = pkg.FindManifestById("remote");
    utassert(remote >= 0 && pkg.manifest[remote].path == nullptr);
    // missing overlay, non-SMIL overlay, unknown idref, remote item
    utassert(pkg.diagnostics.Size() == 4);

    EpubMoDocument* d1 = pkg.overlays[0];
    utassert(EpubParseSmil(&pkg, d1, kSmil1, str::Len(kSmil1)));
    // p5 (no text) dropped; p6 keeps text but loses its invalid audio
    utassert(d1->pars.Size() == 7);
    utassert(str::Eq(d1->pars[0].id, "p1"));
    utassert(str::Eq(d1->pars[0].textPath, "OEBPS/Text/c1.xhtml"));
    utassert(str::Eq(d1->pars[0].textFragment, "s1"));
    utassert(str::Eq(d1->pars[0].audioPath, "OEBPS/Audio/c1.mp3"));
    utassert(d1->pars[0].clipBeginUs == 0 && d1->pars[0].clipEndUs == 5000000);
    utassert(d1->pars[1].clipBeginUs == 5000000 && d1->pars[1].clipEndUs == 10250000);
    utassert(d1->pars[1].flags == kEpubMoEscapable && d1->pars[1].seqIdx == 0);
    utassert(d1->pars[2].flags == (kEpubMoEscapable | kEpubMoSkippable) && d1->pars[2].seqIdx == 1);
    utassert(d1->pars[3].audioPath == nullptr && d1->pars[3].flags == 0 && d1->pars[3].seqIdx == -1);
    utassert(str::Eq(d1->pars[4].id, "p6") && d1->pars[4].audioPath == nullptr);
    utassert(str::Eq(d1->pars[5].id, "p7") && d1->pars[5].audioPath == nullptr);
    utassert(d1->pars[5].flags == kEpubMoSkippable);
    utassert(d1->pars[6].clipBeginUs == 14000000 && d1->pars[6].clipEndUs == kEpubMoClipToEnd);
    utassert(d1->seqs.Size() == 2);
    utassert(d1->seqs[0].firstPar == 1 && d1->seqs[0].endPar == 3);
    utassert(d1->seqs[1].firstPar == 2 && d1->seqs[1].endPar == 3 && d1->seqs[1].parentSeq == 0);
    utassert(str::Eq(d1->seqs[0].textFragment, "fig"));

    EpubMoDocument* d2 = pkg.overlays[1];
    utassert(EpubParseSmil(&pkg, d2, kSmil2Prefixed, str::Len(kSmil2Prefixed)));
    utassert(d2->pars.Size() == 1);
    utassert(str::Eq(d2->pars[0].textFragment, "t1"));
    utassert(d2->pars[0].clipEndUs == 15500000);

    StrBuilder dump;
    EpubMoDumpTimeline(&pkg, dump);
    utassert(str::Find(dump.Get(), "OEBPS/Text/c1.xhtml#s1 OEBPS/Audio/c1.mp3 0:00:00.000-0:00:05.000"));
    utassert(str::Find(dump.Get(), "(non-linear)"));

    EpubMoDocument bad;
    bad.smilPath = "x.smil";
    utassert(!EpubParseSmil(&pkg, &bad, "", 0));
    utassert(bad.failed);
    const char* garbage = "<smil><body><par><text src='http://x/y#z'/></par></body>";
    utassert(!EpubParseSmil(&pkg, &bad, garbage, str::Len(garbage)));
}

static void EpubTypeTests() {
    utassert(EpubMoFlagsForEpubType(nullptr) == 0);
    utassert(EpubMoFlagsForEpubType("chapter") == 0);
    utassert(EpubMoFlagsForEpubType("noteref") == 0);
    utassert(EpubMoFlagsForEpubType("footnote") == kEpubMoSkippable);
    utassert(EpubMoFlagsForEpubType("table-cell") == kEpubMoEscapable);
    utassert(EpubMoFlagsForEpubType(" aside  footnote ") == (kEpubMoSkippable | kEpubMoEscapable));
}

static bool CssColorIs(const char* css, const char* cls, u32 expect, bool expectBg) {
    u32 rgb = 0;
    bool bg = !expectBg;
    return EpubCssFindClassColor(css, str::Len(css), cls, &rgb, &bg) && rgb == expect && bg == expectBg;
}

static void CssTests() {
    const char* cls = "-epub-media-overlay-active";
    utassert(CssColorIs(".-epub-media-overlay-active { background-color: #ff0; }", cls, 0xffff00, true));
    utassert(
        CssColorIs("p { margin: 1.5em } span.-epub-media-overlay-active{color:rgb(10, 20, 30)}", cls, 0x0a141e, false));
    utassert(
        CssColorIs(".-epub-media-overlay-active { color: red } .x, .-epub-media-overlay-active "
                   "{ background: #123456 url(a.png) }",
                   cls, 0x123456, true));
    utassert(CssColorIs(".-epub-media-overlay-active{background-color:Yellow}", cls, 0xffff00, true));
    u32 rgb = 0;
    utassert(!EpubCssFindClassColor(".-epub-media-overlay-activex{color:red}", 40, cls, &rgb, nullptr));
    const char* noColor = ".-epub-media-overlay-active { font-weight: bold }";
    utassert(!EpubCssFindClassColor(noColor, str::Len(noColor), cls, &rgb, nullptr));
}

void EpubMediaOverlay_UnitTests() {
    CssTests();
    ClockTests();
    PathTests();
    EpubTypeTests();
    PackageTests();
}
