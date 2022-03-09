/* Copyright 2021 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

struct PageRange {
    int start{1};
    // end == INT_MAX means to the last page
    int end{INT_MAX};
};

struct Flags {
    WStrVec fileNames;
    // pathsToBenchmark contain 2 strings per each file to benchmark:
    // - name of the file to benchmark
    // - optional (nullptr if not available) string that represents which pages
    //   to benchmark. It can also be a string "loadonly" which means we'll
    //   only benchmark loading of the catalog
    WStrVec pathsToBenchmark;
    bool exitWhenDone{false};
    bool printDialog{false};
    WCHAR* printerName{nullptr};
    WCHAR* printSettings{nullptr};
    WCHAR* forwardSearchOrigin{nullptr};
    int forwardSearchLine{0};
    bool reuseDdeInstance{false};
    WCHAR* destName{nullptr};
    int pageNumber = {-1};
    bool restrictedUse{false};
    bool enterPresentation{false};
    bool enterFullScreen{false};
    DisplayMode startView{DisplayMode::Automatic};
    float startZoom = INVALID_ZOOM;
    Point startScroll{-1, -1};
    bool showConsole{false};
    HWND hwndPluginParent{nullptr};
    WCHAR* pluginURL{nullptr};
    bool exitImmediately{false};
    bool silent{false};
    WCHAR* appdataDir{nullptr};
    WCHAR* inverseSearchCmdLine{nullptr};
    bool invertColors{false};
    bool regress{false};
    bool tester{false};
    // -new-window, if true and we're using tabs, opens
    // the document in new window
    bool inNewWindow{false};
    WCHAR* search{nullptr};

    // stress-testing related
    WCHAR* stressTestPath{nullptr};
    // nullptr is equivalent to "*" (i.e. all files)
    WCHAR* stressTestFilter{nullptr};
    WCHAR* stressTestRanges{nullptr};
    int stressTestCycles{1};
    int stressParallelCount{1};
    bool stressRandomizeFiles{false};

    // related to testing
    bool testRenderPage{false};
    bool testExtractPage{false};
    int testPageNo{0};
    bool testApp{false};

    bool crashOnOpen{false};

    // deprecated flags
    char* lang{nullptr};
    WStrVec globalPrefArgs;

    // related to installer
    bool showHelp{false};
    WCHAR* installDir{nullptr};
    bool install{false};
    bool uninstall{false};
    bool withFilter{false};
    bool withPreview{false};
    bool justExtractFiles{false};
    bool registerAsDefault{false};
    bool log{false};

    // for internal use
    WCHAR* updateSelfTo{nullptr};
    WCHAR* deleteFile{nullptr};

    WCHAR* toEpubPath{nullptr};

    // for some commands, will sleep for sleepMs milliseconds
    // before proceeding
    int sleepMs{0};

    Flags() = default;
    ~Flags();
};

void ParseFlags(const WCHAR* cmdLine, Flags&);

bool IsValidPageRange(const WCHAR* ranges);
bool IsBenchPagesInfo(const WCHAR* s);
bool ParsePageRanges(const WCHAR* ranges, Vec<PageRange>& result);
