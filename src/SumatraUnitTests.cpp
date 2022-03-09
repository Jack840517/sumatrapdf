/* Copyright 2021 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "AppUtil.h"
#include "utils/FileUtil.h"
#include "utils/WinUtil.h"
#include "utils/StrFormat.h"
#include "utils/ScopedWin.h"

#include "wingui/TreeModel.h"
#include "DisplayMode.h"
#include "Controller.h"
#include "EngineBase.h"
#include "SettingsStructs.h"
#include "GlobalPrefs.h"
#include "Flags.h"

#include <float.h>
#include <math.h>

// must be last to over-write assert()
#include "utils/UtAssert.h"

#define utassert_fequal(a, b) utassert(fabs(a - b) < FLT_EPSILON);

static void ParseCommandLineTest() {
    {
        Flags i;
        ParseFlags(L"SumatraPDF.exe -bench foo.pdf", i);
        utassert(2 == i.pathsToBenchmark.size());
        utassert(str::Eq(L"foo.pdf", i.pathsToBenchmark.at(0)));
        utassert(nullptr == i.pathsToBenchmark.at(1));
    }

    {
        Flags i;
        ParseFlags(L"SumatraPDF.exe -bench foo.pdf -fwdsearch-width 5", i);
        utassert(i.globalPrefArgs.size() == 2);
        const WCHAR* s = i.globalPrefArgs.at(0);
        utassert(str::Eq(s, L"-fwdsearch-width"));
        s = i.globalPrefArgs.at(1);
        utassert(str::Eq(s, L"5"));
        utassert(2 == i.pathsToBenchmark.size());
        utassert(str::Eq(L"foo.pdf", i.pathsToBenchmark.at(0)));
        utassert(nullptr == i.pathsToBenchmark.at(1));
    }

    {
        Flags i;
        ParseFlags(L"SumatraPDF.exe -bench bar.pdf loadonly", i);
        utassert(2 == i.pathsToBenchmark.size());
        utassert(str::Eq(L"bar.pdf", i.pathsToBenchmark.at(0)));
        utassert(str::Eq(L"loadonly", i.pathsToBenchmark.at(1)));
    }

    {
        Flags i;
        ParseFlags(L"SumatraPDF.exe -bench bar.pdf 1 -set-color-range 0x123456 #abCDef", i);
        utassert(i.globalPrefArgs.size() == 3);
        utassert(2 == i.pathsToBenchmark.size());
        utassert(str::Eq(L"bar.pdf", i.pathsToBenchmark.at(0)));
        utassert(str::Eq(L"1", i.pathsToBenchmark.at(1)));
    }

    {
        Flags i;
        ParseFlags(L"SumatraPDF.exe -bench bar.pdf 1-5,3   -bench some.pdf 1,3,8-34", i);
        utassert(4 == i.pathsToBenchmark.size());
        utassert(str::Eq(L"bar.pdf", i.pathsToBenchmark.at(0)));
        utassert(str::Eq(L"1-5,3", i.pathsToBenchmark.at(1)));
        utassert(str::Eq(L"some.pdf", i.pathsToBenchmark.at(2)));
        utassert(str::Eq(L"1,3,8-34", i.pathsToBenchmark.at(3)));
    }

    {
        Flags i;
        ParseFlags(L"SumatraPDF.exe -presentation -bgcolor 0xaa0c13 foo.pdf -invert-colors bar.pdf", i);
        utassert(true == i.enterPresentation);
        utassert(true == i.invertColors);
        utassert(2 == i.fileNames.size());
        utassert(0 == i.fileNames.Find(L"foo.pdf"));
        utassert(1 == i.fileNames.Find(L"bar.pdf"));
    }

    {
        Flags i;
        ParseFlags(L"SumatraPDF.exe -bg-color 0xaa0c13 -invertcolors rosanna.pdf", i);
        utassert(true == i.invertColors);
        utassert(1 == i.fileNames.size());
        utassert(0 == i.fileNames.Find(L"rosanna.pdf"));
    }

    {
        Flags i;
        ParseFlags(LR"(SumatraPDF.exe "foo \" bar \\.pdf" un\"quoted.pdf)", i);
        utassert(2 == i.fileNames.size());
        utassert(0 == i.fileNames.Find(LR"(foo " bar \\.pdf)"));
        utassert(1 == i.fileNames.Find(LR"(un"quoted.pdf)"));
    }

    {
        Flags i;
        ParseFlags(L"SumatraPDF.exe -page 37 -view continuousfacing -zoom fitcontent -scroll 45,1234 -reuse-instance",
                   i);
        utassert(0 == i.fileNames.size());
        utassert(i.pageNumber == 37);
        utassert(i.startView == DisplayMode::ContinuousFacing);
        utassert(i.startZoom == ZOOM_FIT_CONTENT);
        utassert(i.startScroll.x == 45 && i.startScroll.y == 1234);
    }

    {
        Flags i;
        ParseFlags(LR"(SumatraPDF.exe -view "single page" -zoom 237.45 -scroll -21,-1)", i);
        utassert(0 == i.fileNames.size());
        utassert(i.startView == DisplayMode::SinglePage);
        utassert(i.startZoom == 237.45f);
        utassert(i.startScroll.x == -21 && i.startScroll.y == -1);
    }

    {
        Flags i;
        ParseFlags(L"SumatraPDF.exe -zoom 35%", i);
        utassert(0 == i.fileNames.size());
        utassert(i.startZoom == 35.f);
    }

    {
        Flags i;
        ParseFlags(L"SumatraPDF.exe -zoom fit-content", i);
        utassert(i.startZoom == ZOOM_FIT_CONTENT);
        utassert(0 == i.fileNames.size());
    }
}

static void BenchRangeTest() {
    utassert(IsBenchPagesInfo(L"1"));
    utassert(IsBenchPagesInfo(L"2-4"));
    utassert(IsBenchPagesInfo(L"5,7"));
    utassert(IsBenchPagesInfo(L"6,8,"));
    utassert(IsBenchPagesInfo(L"1-3,4,6-9,13"));
    utassert(IsBenchPagesInfo(L"2-"));
    utassert(IsBenchPagesInfo(L"loadonly"));

    utassert(!IsBenchPagesInfo(L""));
    utassert(!IsBenchPagesInfo(L"-2"));
    utassert(!IsBenchPagesInfo(L"2--4"));
    utassert(!IsBenchPagesInfo(L"4-2"));
    utassert(!IsBenchPagesInfo(L"1-3,loadonly"));
    utassert(!IsBenchPagesInfo(nullptr));
}

static void versioncheck_test() {
    utassert(IsValidProgramVersion("1"));
    utassert(IsValidProgramVersion("1.1"));
    utassert(IsValidProgramVersion("1.1.1\r\n"));
    utassert(IsValidProgramVersion("2662"));

    utassert(!IsValidProgramVersion("1.1b"));
    utassert(!IsValidProgramVersion("1..1"));
    utassert(!IsValidProgramVersion("1.1\r\n.1"));

    utassert(CompareVersion("0.9.3.900", "0.9.3") > 0);
    utassert(CompareVersion("1.09.300", "1.09.3") > 0);
    utassert(CompareVersion("1.9.1", "1.09.3") < 0);
    utassert(CompareVersion("1.2.0", "1.2") == 0);
    utassert(CompareVersion("1.3.0", "2662") < 0);
}

static void hexstrTest() {
    u8 buf[6] = {1, 2, 33, 255, 0, 18};
    u8 buf2[6] = {0};
    AutoFree s(_MemToHex(&buf));
    utassert(str::Eq(s, "010221ff0012"));
    bool ok = _HexToMem(s, &buf2);
    utassert(ok);
    utassert(memeq(buf, buf2, sizeof(buf)));

    FILETIME ft1, ft2;
    GetSystemTimeAsFileTime(&ft1);
    s.Set(_MemToHex(&ft1));
    _HexToMem(s, &ft2);
    DWORD diff = FileTimeDiffInSecs(ft1, ft2);
    utassert(0 == diff);
    utassert(FileTimeEq(ft1, ft2));

    s.Set(str::MemToHex(nullptr, 0));
    utassert(str::Eq(s, ""));
    ok = str::HexToMem(s, nullptr, 0);
    utassert(ok);
}

static void assertSerializedColor(COLORREF c, const char* s) {
    char* s2 = SerializeColor(c);
    utassert(str::Eq(s2, s));
    str::Free(s2);
}

static void colorTest() {
    COLORREF c = 0;
    bool ok = ParseColor(&c, "0x01020304");
    utassert(ok);
    assertSerializedColor(c, "#01020304");

    ok = ParseColor(&c, "#01020304");
    utassert(ok);
    assertSerializedColor(c, "#01020304");

    COLORREF c2 = MkColor(2, 3, 4, 1);
    assertSerializedColor(c2, "#01020304");
    utassert(c == c2);

    c2 = MkColor(5, 7, 6, 8);
    assertSerializedColor(c2, "#08050706");
    ok = ParseColor(&c, "#08050706");
    utassert(ok);
    utassert(c == c2);
}

void SumatraPDF_UnitTests() {
    colorTest();
    BenchRangeTest();
    ParseCommandLineTest();
    versioncheck_test();
    hexstrTest();
}
