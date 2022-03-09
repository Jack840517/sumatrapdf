/* Copyright 2021 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/ScopedWin.h"
#include "utils/WinDynCalls.h"
#include "utils/CryptoUtil.h"
#include "utils/DirIter.h"
#include "utils/Dpi.h"
#include "utils/FileUtil.h"
#include "utils/FileWatcher.h"
#include "utils/GuessFileType.h"
#include "utils/HtmlParserLookup.h"
#include "utils/HttpUtil.h"
#include "utils/SquareTreeParser.h"
#include "utils/ThreadUtil.h"
#include "utils/UITask.h"
#include "utils/WinUtil.h"
#include "utils/GdiPlusUtil.h"
#include "utils/Archive.h"

#include "wingui/WinGui.h"
#include "wingui/Layout.h"
#include "wingui/Window.h"
#include "wingui/StaticCtrl.h"
#include "wingui/ButtonCtrl.h"
#include "wingui/TreeModel.h"
#include "wingui/TreeCtrl.h"
#include "wingui/SplitterWnd.h"
#include "wingui/LabelWithCloseWnd.h"
#include "wingui/FrameRateWnd.h"
#include "wingui/TooltipCtrl.h"
#include "wingui/DropDownCtrl.h"
#include "wingui/TabsCtrl.h"

#include "Annotation.h"
#include "DisplayMode.h"
#include "Controller.h"
#include "EngineBase.h"
#include "EngineAll.h"
#include "PdfCreator.h"
#include "SettingsStructs.h"
#include "GlobalPrefs.h"
#include "ChmModel.h"
#include "PalmDbReader.h"
#include "EbookBase.h"
#include "EbookDoc.h"
#include "MobiDoc.h"
#include "DisplayModel.h"
#include "FileHistory.h"
#include "PdfSync.h"
#include "RenderCache.h"
#include "ProgressUpdateUI.h"
#include "TextSelection.h"
#include "TextSearch.h"
#include "AppColors.h"
#include "SumatraPDF.h"
#include "Notifications.h"
#include "WindowInfo.h"
#include "TabInfo.h"
#include "UpdateCheck.h"
#include "resource.h"
#include "Commands.h"
#include "Flags.h"
#include "AppPrefs.h"
#include "AppTools.h"
#include "AppUtil.h"
#include "Canvas.h"
#include "Caption.h"
#include "CrashHandler.h"
#include "ExternalViewers.h"
#include "Favorites.h"
#include "FileThumbnails.h"
#include "Menu.h"
#include "Print.h"
#include "SearchAndDDE.h"
#include "Selection.h"
#include "StressTesting.h"
#include "SumatraAbout.h"
#include "SumatraDialogs.h"
#include "SumatraProperties.h"
#include "TableOfContents.h"
#include "Tabs.h"
#include "Toolbar.h"
#include "Translations.h"
#include "uia/Provider.h"
#include "Version.h"
#include "SumatraConfig.h"
#include "EditAnnotations.h"

#include "utils/Log.h"

using std::placeholders::_1;

#define RESTRICTIONS_FILE_NAME L"sumatrapdfrestrict.ini"

#define DEFAULT_LINK_PROTOCOLS L"http,https,mailto"
#define DEFAULT_FILE_PERCEIVED_TYPES L"audio,video,webpage"

/* if true, we're in debug mode where we show links as blue rectangle on
   the screen. Makes debugging code related to links easier. */
#if defined(DEBUG)
bool gDebugShowLinks = true;
#else
bool gDebugShowLinks = false;
#endif

// used to show it in debug, but is not very useful,
// so always disable
bool gShowFrameRate = false;

// in plugin mode, the window's frame isn't drawn and closing and
// fullscreen are disabled, so that SumatraPDF can be displayed
// embedded (e.g. in a web browser)
const WCHAR* gPluginURL = nullptr; // owned by Flags in WinMain

static Kind NG_PERSISTENT_WARNING = "persistentWarning";
static Kind NG_PAGE_INFO_HELPER = "pageInfoHelper";

#define SPLITTER_DX 5
#define SIDEBAR_MIN_WIDTH 150

#define SPLITTER_DY 4
#define TOC_MIN_DY 100

// minimum size of the window
constexpr LONG MIN_WIN_DX = 480;
constexpr LONG MIN_WIN_DY = 320;

FileHistory gFileHistory;
Favorites gFavorites;

HBITMAP gBitmapReloadingCue;
RenderCache gRenderCache;
HCURSOR gCursorDrag;

// set after mouse shortcuts involving the Alt key (so that the menu bar isn't activated)
bool gSuppressAltKey = false;

bool gCrashOnOpen = false;

// in restricted mode, some features can be disabled (such as
// opening files, printing, following URLs), so that SumatraPDF
// can be used as a PDF reader on locked down systems
static Perm gPolicyRestrictions = Perm::RestrictedUse;
// only the listed protocols will be passed to the OS for
// opening in e.g. a browser or an email client (ignored,
// if gPolicyRestrictions doesn't contain Perm::DiskAccess)
static WStrVec gAllowedLinkProtocols;
// only files of the listed perceived types will be opened
// externally by LinkHandler::LaunchFile (i.e. when clicking
// on an in-document link); examples: "audio", "video", ...
static WStrVec gAllowedFileTypes;

// workaround for OnMenuExit
// if this flag is set, CloseWindow will not save prefs before closing the window.
static bool gDontSavePrefs = false;

static void CloseDocumentInCurrentTab(WindowInfo*, bool keepUIEnabled = false, bool deleteModel = false);
static void UpdatePageInfoHelper(WindowInfo*, NotificationWnd* wnd = nullptr, int pageNo = -1);
static void OnSidebarSplitterMove(SplitterMoveEvent*);
static void OnFavSplitterMove(SplitterMoveEvent*);
static void DownloadDebugSymbols();

void SetCurrentLang(const char* langCode) {
    if (!langCode) {
        return;
    }
    str::ReplaceWithCopy(&gGlobalPrefs->uiLanguage, langCode);
    trans::SetCurrentLangByCode(langCode);
}

void InitializePolicies(bool restrict) {
    // default configuration should be to restrict everything
    CrashIf(gPolicyRestrictions != Perm::RestrictedUse);
    CrashIf(gAllowedLinkProtocols.size() != 0 || gAllowedFileTypes.size() != 0);

    // the -restrict command line flag overrides any sumatrapdfrestrict.ini configuration
    if (restrict) {
        return;
    }

    // allow to restrict SumatraPDF's functionality from an INI file in the
    // same directory as SumatraPDF.exe (see ../docs/sumatrapdfrestrict.ini)
    // (if the file isn't there, everything is allowed)
    AutoFreeWstr restrictPath(path::GetPathOfFileInAppDir(RESTRICTIONS_FILE_NAME));
    if (!file::Exists(restrictPath)) {
        gPolicyRestrictions = Perm::All;
        gAllowedLinkProtocols.Split(DEFAULT_LINK_PROTOCOLS, L",");
        gAllowedFileTypes.Split(DEFAULT_FILE_PERCEIVED_TYPES, L",");
        return;
    }

    AutoFree restrictData(file::ReadFile(restrictPath));
    SquareTree sqt(restrictData.data);
    SquareTreeNode* polsec = sqt.root ? sqt.root->GetChild("Policies") : nullptr;
    // if the restriction file is broken, err on the side of full restriction
    if (!polsec) {
        return;
    }

    static struct {
        const char* name;
        Perm perm;
    } policies[] = {
        {"InternetAccess", Perm::InternetAccess},     {"DiskAccess", Perm::DiskAccess},
        {"SavePreferences", Perm::SavePreferences},   {"RegistryAccess", Perm::RegistryAccess},
        {"PrinterAccess", Perm::PrinterAccess},       {"CopySelection", Perm::CopySelection},
        {"FullscreenAccess", Perm::FullscreenAccess},
    };

    // enable policies as indicated in sumatrapdfrestrict.ini
    for (size_t i = 0; i < dimof(policies); i++) {
        const char* value = polsec->GetValue(policies[i].name);
        if (value && atoi(value) != 0) {
            gPolicyRestrictions = gPolicyRestrictions | policies[i].perm;
        }
    }

    // determine the list of allowed link protocols and perceived file types
    if ((gPolicyRestrictions & Perm::DiskAccess) != (Perm)0) {
        const char* value;
        value = polsec->GetValue("LinkProtocols");
        if (value != nullptr) {
            auto protocols = ToWstrTemp(value);
            str::ToLowerInPlace(protocols);
            str::TransCharsInPlace(protocols, L" :;", L",,,");
            gAllowedLinkProtocols.Split(protocols, L",", true);
        }
        value = polsec->GetValue("SafeFileTypes");
        if (value != nullptr) {
            auto protocols = ToWstrTemp(value);
            str::ToLowerInPlace(protocols);
            str::TransCharsInPlace(protocols, L" :;", L",,,");
            gAllowedFileTypes.Split(protocols, L",", true);
        }
    }
}

void RestrictPolicies(Perm revokePermission) {
    gPolicyRestrictions = (gPolicyRestrictions | Perm::RestrictedUse) & ~revokePermission;
}

bool HasPermission(Perm permission) {
    return (permission & gPolicyRestrictions) == permission;
}

// lets the shell open a URI for any supported scheme in
// the appropriate application (web browser, mail client, etc.)
bool SumatraLaunchBrowser(const WCHAR* url) {
    if (gPluginMode) {
        // pass the URI back to the browser
        CrashIf(gWindows.empty());
        if (gWindows.empty()) {
            return false;
        }
        HWND plugin = gWindows.at(0)->hwndFrame;
        HWND parent = GetAncestor(plugin, GA_PARENT);
        auto urlA(ToUtf8Temp(url));
        if (!parent || !urlA.Get() || (urlA.size() > 4096)) {
            return false;
        }
        COPYDATASTRUCT cds = {0x4C5255 /* URL */, (DWORD)urlA.size() + 1, urlA.Get()};
        return SendMessageW(parent, WM_COPYDATA, (WPARAM)plugin, (LPARAM)&cds);
    }

    if (!HasPermission(Perm::DiskAccess)) {
        return false;
    }

    // check if this URL's protocol is allowed
    AutoFreeWstr protocol;
    if (!str::Parse(url, L"%S:", &protocol)) {
        return false;
    }
    str::ToLowerInPlace(protocol);
    if (!gAllowedLinkProtocols.Contains(protocol)) {
        return false;
    }

    return LaunchFile(url, nullptr, L"open");
}

bool DocIsSupportedFileType(Kind kind) {
    if (EpubDoc::IsSupportedFileType(kind)) {
        return true;
    }
    if (Fb2Doc::IsSupportedFileType(kind)) {
        return true;
    }
    if (MobiDoc::IsSupportedFileType(kind)) {
        return true;
    }
    if (PalmDoc::IsSupportedFileType(kind)) {
        return true;
    }
    return false;
}

// lets the shell open a file of any supported perceived type
// in the default application for opening such files
bool OpenFileExternally(const WCHAR* path) {
    if (!HasPermission(Perm::DiskAccess) || gPluginMode) {
        return false;
    }

    // check if this file's perceived type is allowed
    const WCHAR* ext = path::GetExtTemp(path);
    AutoFreeWstr perceivedType(ReadRegStr(HKEY_CLASSES_ROOT, ext, L"PerceivedType"));
    // since we allow following hyperlinks, also allow opening local webpages
    if (str::EndsWithI(path, L".htm") || str::EndsWithI(path, L".html") || str::EndsWithI(path, L".xhtml")) {
        perceivedType.SetCopy(L"webpage");
    }
    str::ToLowerInPlace(perceivedType);
    if (gAllowedFileTypes.Contains(L"*")) {
        /* allow all file types (not recommended) */;
    } else if (!perceivedType || !gAllowedFileTypes.Contains(perceivedType)) {
        return false;
    }

    // TODO: only do this for trusted files (cf. IsUntrustedFile)?
    return LaunchFile(path);
}

void SwitchToDisplayMode(WindowInfo* win, DisplayMode displayMode, bool keepContinuous) {
    if (!win->IsDocLoaded()) {
        return;
    }

    win->ctrl->SetDisplayMode(displayMode, keepContinuous);
    UpdateToolbarState(win);
}

TabInfo* FindTabByFile(const WCHAR* file) {
    AutoFreeWstr normFile(path::Normalize(file));

    for (WindowInfo* win : gWindows) {
        for (TabInfo* tab : win->tabs) {
            WCHAR* fp = tab->filePath;
            if (!fp || !path::IsSame(tab->filePath, normFile)) {
                continue;
            }
            return tab;
        }
    }
    return nullptr;
}

// ok for tab to be null
void SelectTabInWindow(TabInfo* tab) {
    if (!tab || !tab->win) {
        return;
    }
    auto win = tab->win;
    if (tab == win->currentTab) {
        return;
    }
    TabsSelect(win, win->tabs.Find(tab));
}

// Find the first window showing a given PDF file
WindowInfo* FindWindowInfoByFile(const WCHAR* file, bool focusTab) {
    TabInfo* tab = FindTabByFile(file);
    if (!tab) {
        return nullptr;
    }
    if (focusTab) {
        SelectTabInWindow(tab);
    }
    return tab->win;
}

// Find the first window that has been produced from <file>
WindowInfo* FindWindowInfoBySyncFile(const WCHAR* file, bool focusTab) {
    for (WindowInfo* win : gWindows) {
        Vec<Rect> rects;
        uint page;
        auto dm = win->AsFixed();
        if (dm && dm->pdfSync && dm->pdfSync->SourceToDoc(file, 0, 0, &page, rects) != PDFSYNCERR_UNKNOWN_SOURCEFILE) {
            return win;
        }
        if (focusTab && win->tabs.size() > 1) {
            // bring a background tab to the foreground
            for (TabInfo* tab : win->tabs) {
                if (tab != win->currentTab && tab->AsFixed() && tab->AsFixed()->pdfSync &&
                    tab->AsFixed()->pdfSync->SourceToDoc(file, 0, 0, &page, rects) != PDFSYNCERR_UNKNOWN_SOURCEFILE) {
                    TabsSelect(win, win->tabs.Find(tab));
                    return win;
                }
            }
        }
    }
    return nullptr;
}

class HwndPasswordUI : public PasswordUI {
    HWND hwnd;
    size_t pwdIdx;

  public:
    explicit HwndPasswordUI(HWND hwnd) : hwnd(hwnd), pwdIdx(0) {
    }

    WCHAR* GetPassword(const WCHAR* fileName, u8* fileDigest, u8 decryptionKeyOut[32], bool* saveKey) override;
};

/* Get password for a given 'fileName', can be nullptr if user cancelled the
   dialog box or if the encryption key has been filled in instead.
   Caller needs to free() the result. */
WCHAR* HwndPasswordUI::GetPassword(const WCHAR* fileNameW, u8* fileDigest, u8 decryptionKeyOut[32], bool* saveKey) {
    char* fileName = ToUtf8Temp(fileNameW);
    FileState* fileFromHistory = gFileHistory.Find(fileName, nullptr);
    if (fileFromHistory && fileFromHistory->decryptionKey) {
        AutoFree fingerprint(str::MemToHex(fileDigest, 16));
        *saveKey = str::StartsWith(fileFromHistory->decryptionKey, fingerprint.Get());
        if (*saveKey && str::HexToMem(fileFromHistory->decryptionKey + 32, decryptionKeyOut, 32)) {
            return nullptr;
        }
    }

    *saveKey = false;

    // try the list of default passwords before asking the user
    if (pwdIdx < gGlobalPrefs->defaultPasswords->size()) {
        char* pwd = gGlobalPrefs->defaultPasswords->at(pwdIdx++);
        WCHAR* pwdW = strconv::Utf8ToWstr(pwd);
        return pwdW;
    }

    if (IsStressTesting()) {
        return nullptr;
    }

    // extract the filename from the URL in plugin mode instead
    // of using the more confusing temporary filename
    AutoFreeWstr urlName;
    if (gPluginMode) {
        urlName.Set(url::GetFileName(gPluginURL));
        if (urlName) {
            fileNameW = urlName;
        }
    }
    fileNameW = path::GetBaseNameTemp(fileNameW);

    // check if the window is still valid as it might have been closed by now
    if (!IsWindow(hwnd)) {
        CrashIf(true);
        hwnd = GetForegroundWindow();
    }
    // make sure that the password dialog is visible
    win::ToForeground(hwnd);

    bool* rememberPwd = gGlobalPrefs->rememberOpenedFiles ? saveKey : nullptr;
    return Dialog_GetPassword(hwnd, fileNameW, rememberPwd);
}

// update global windowState for next default launch when either
// no pdf is opened or a document without window dimension information
void RememberDefaultWindowPosition(WindowInfo* win) {
    // ignore spurious WM_SIZE and WM_MOVE messages happening during initialization
    if (!IsWindowVisible(win->hwndFrame)) {
        return;
    }

    if (win->presentation) {
        gGlobalPrefs->windowState = win->windowStateBeforePresentation;
    } else if (win->isFullScreen) {
        gGlobalPrefs->windowState = WIN_STATE_FULLSCREEN;
    } else if (IsZoomed(win->hwndFrame)) {
        gGlobalPrefs->windowState = WIN_STATE_MAXIMIZED;
    } else if (!IsIconic(win->hwndFrame)) {
        gGlobalPrefs->windowState = WIN_STATE_NORMAL;
    }

    gGlobalPrefs->sidebarDx = WindowRect(win->hwndTocBox).dx;

    /* don't update the window's dimensions if it is maximized, mimimized or fullscreened */
    if (WIN_STATE_NORMAL == gGlobalPrefs->windowState && !IsIconic(win->hwndFrame) && !win->presentation) {
        // TODO: Use Get/SetWindowPlacement (otherwise we'd have to separately track
        //       the non-maximized dimensions for proper restoration)
        gGlobalPrefs->windowPos = WindowRect(win->hwndFrame);
    }
}

static void UpdateDisplayStateWindowRect(WindowInfo* win, FileState* fs, bool updateGlobal = true) {
    if (updateGlobal) {
        RememberDefaultWindowPosition(win);
    }

    fs->windowState = gGlobalPrefs->windowState;
    fs->windowPos = gGlobalPrefs->windowPos;
    fs->sidebarDx = gGlobalPrefs->sidebarDx;
}

static void UpdateSidebarDisplayState(TabInfo* tab, FileState* fs) {
    CrashIf(!tab);
    WindowInfo* win = tab->win;
    fs->showToc = tab->showToc;
    if (win->tocLoaded && tab == win->currentTab) {
        TocTree* tocTree = tab->ctrl->GetToc();
        UpdateTocExpansionState(tab->tocState, win->tocTreeCtrl, tocTree);
    }
    *fs->tocState = tab->tocState;
}

void UpdateTabFileDisplayStateForTab(TabInfo* tab) {
    if (!tab || !tab->ctrl) {
        return;
    }
    WindowInfo* win = tab->win;
    // TODO: this is called multiple times for each tab
    RememberDefaultWindowPosition(win);
    char* fp = ToUtf8Temp(tab->filePath);
    FileState* fs = gFileHistory.Find(fp, nullptr);
    if (!fs) {
        return;
    }
    tab->ctrl->GetDisplayState(fs);
    UpdateDisplayStateWindowRect(win, fs, false);
    UpdateSidebarDisplayState(tab, fs);
}

bool IsUIRightToLeft() {
    return trans::IsCurrLangRtl();
}

uint MbRtlReadingMaybe() {
    if (IsUIRightToLeft()) {
        return MB_RTLREADING;
    }
    return 0;
}

void MessageBoxWarning(HWND hwnd, const WCHAR* msg, const WCHAR* title) {
    uint type = MB_OK | MB_ICONEXCLAMATION | MbRtlReadingMaybe();
    if (!title) {
        title = _TR("Warning");
    }
    MessageBoxW(hwnd, msg, title, type);
}

// updates the layout for a window to either left-to-right or right-to-left
// depending on the currently used language (cf. IsUIRightToLeft)
static void UpdateWindowRtlLayout(WindowInfo* win) {
    bool isRTL = IsUIRightToLeft();
    bool wasRTL = (GetWindowLongW(win->hwndFrame, GWL_EXSTYLE) & WS_EX_LAYOUTRTL) != 0;
    if (wasRTL == isRTL) {
        return;
    }

    bool tocVisible = win->tocVisible;
    bool favVisible = gGlobalPrefs->showFavorites;
    if (tocVisible || favVisible) {
        SetSidebarVisibility(win, false, false);
    }

    // https://www.microsoft.com/middleeast/msdn/mirror.aspx
    SetRtl(win->hwndFrame, isRTL);

    SetRtl(win->hwndTocBox, isRTL);
    HWND tocBoxTitle = win->tocLabelWithClose->hwnd;
    SetRtl(tocBoxTitle, isRTL);

    SetRtl(win->hwndFavBox, isRTL);
    HWND favBoxTitle = win->favLabelWithClose->hwnd;
    SetRtl(favBoxTitle, isRTL);
    win->favTreeCtrl->SetRtl(isRTL);

    SetRtl(win->hwndReBar, isRTL);
    SetRtl(win->hwndToolbar, isRTL);
    SetRtl(win->hwndFindBox, isRTL);
    SetRtl(win->hwndFindText, isRTL);
    SetRtl(win->hwndTbInfoText, isRTL);
    SetRtl(win->hwndPageText, isRTL);

    SetRtl(win->hwndCaption, isRTL);
    SetCaptionButtonsRtl(win->caption, isRTL);

    // TODO: why isn't SetWindowPos(..., SWP_FRAMECHANGED) enough?
    SendMessageW(win->hwndFrame, WM_DWMCOMPOSITIONCHANGED, 0, 0);
    RelayoutCaption(win);
    // TODO: make tab bar RTL aware
    // SetRtl(win->tabsCtrl->hwnd, isRTL);

    win->notifications->Relayout();

    // TODO: also update the canvas scrollbars (?)

    // ensure that the ToC sidebar is on the correct side and that its
    // title and close button are also correctly laid out
    if (tocVisible || favVisible) {
        SetSidebarVisibility(win, tocVisible, favVisible);
        if (tocVisible) {
            SendMessageW(win->hwndTocBox, WM_SIZE, 0, 0);
        }
        if (favVisible) {
            SendMessageW(win->hwndFavBox, WM_SIZE, 0, 0);
        }
    }
}

void RebuildMenuBarForWindow(WindowInfo* win) {
    HMENU oldMenu = win->menu;
    win->menu = BuildMenu(win);
    if (!win->presentation && !win->isFullScreen && !win->isMenuHidden) {
        SetMenu(win->hwndFrame, win->menu);
    }
    FreeMenuOwnerDrawInfoData(oldMenu);
    DestroyMenu(oldMenu);
}

static bool ShouldSaveThumbnail(FileState& ds) {
    // don't create thumbnails if we won't be needing them at all
    if (!HasPermission(Perm::SavePreferences)) {
        return false;
    }

    // don't create thumbnails for files that won't need them anytime soon
    Vec<FileState*> list;
    gFileHistory.GetFrequencyOrder(list);
    int idx = list.Find(&ds);
    if (idx < 0 || kFileHistoryMaxFrequent * 2 <= idx) {
        return false;
    }

    if (HasThumbnail(ds)) {
        return false;
    }
    return true;
}

// TODO: replace with std::function
class ThumbnailRenderingTask : public RenderingCallback {
    std::function<void(RenderedBitmap*)> saveThumbnail;

  public:
    explicit ThumbnailRenderingTask(const std::function<void(RenderedBitmap*)>& saveThumbnail)
        : saveThumbnail(saveThumbnail) {
    }
    ~ThumbnailRenderingTask() override = default;

    void Callback(RenderedBitmap* bmp) override {
        saveThumbnail(bmp);
        delete this;
    }
};

struct ControllerCallbackHandler : ControllerCallback {
    WindowInfo* win{nullptr};

  public:
    explicit ControllerCallbackHandler(WindowInfo* win) : win(win) {
    }
    ~ControllerCallbackHandler() override = default;

    void Repaint() override {
        RepaintAsync(win, 0);
    }
    void PageNoChanged(Controller* ctrl, int pageNo) override;
    void UpdateScrollbars(Size canvas) override;
    void RequestRendering(int pageNo) override;
    void CleanUp(DisplayModel* dm) override;
    void RenderThumbnail(DisplayModel* dm, Size size, const onBitmapRenderedCb&) override;
    void GotoLink(IPageDestination* dest) override {
        win->linkHandler->GotoLink(dest);
    }
    void FocusFrame(bool always) override;
    void SaveDownload(const WCHAR* url, ByteSlice data) override;
};

void ControllerCallbackHandler::RenderThumbnail(DisplayModel* dm, Size size, const onBitmapRenderedCb& saveThumbnail) {
    auto engine = dm->GetEngine();
    RectF pageRect = engine->PageMediabox(1);
    if (pageRect.IsEmpty()) {
        // saveThumbnail must always be called for clean-up code
        saveThumbnail(nullptr);
        return;
    }

    pageRect = engine->Transform(pageRect, 1, 1.0f, 0);
    float zoom = size.dx / (float)pageRect.dx;
    if (pageRect.dy > (float)size.dy / zoom) {
        pageRect.dy = (float)size.dy / zoom;
    }
    pageRect = engine->Transform(pageRect, 1, 1.0f, 0, true);

    // TODO: this is leaking?
    RenderingCallback* callback = new ThumbnailRenderingTask(saveThumbnail);
    gRenderCache.Render(dm, 1, 0, zoom, pageRect, *callback);
    // cppcheck-suppress memleak
}

static void CreateThumbnailForFile(WindowInfo* win, FileState& ds) {
    if (!ShouldSaveThumbnail(ds)) {
        return;
    }

    CrashIf(!win->IsDocLoaded());
    if (!win->IsDocLoaded()) {
        return;
    }

    // don't create thumbnails for password protected documents
    // (unless we're also remembering the decryption key anyway)
    auto* model = win->AsFixed();
    if (model) {
        auto* engine = model->GetEngine();
        bool withPwd = engine->IsPasswordProtected();
        AutoFree decrKey(engine->GetDecryptionKey());
        if (withPwd && !decrKey) {
            RemoveThumbnail(ds);
            return;
        }
    }

    WCHAR* filePath = str::Dup(win->ctrl->GetFilePath());
    win->ctrl->CreateThumbnail(Size(THUMBNAIL_DX, THUMBNAIL_DY), [=](RenderedBitmap* bmp) {
        uitask::Post([=] {
            if (bmp) {
                char* filePathA = ToUtf8Temp(filePath);
                SetThumbnail(gFileHistory.Find(filePathA, nullptr), bmp);
            }
            free(filePath);
        });
    });
}

/* Send the request to render a given page to a rendering thread */
void ControllerCallbackHandler::RequestRendering(int pageNo) {
    CrashIf(!win->AsFixed());
    if (!win->AsFixed()) {
        return;
    }

    DisplayModel* dm = win->AsFixed();
    // don't render any plain images on the rendering thread,
    // they'll be rendered directly in DrawDocument during
    // WM_PAINT on the UI thread
    if (dm->ShouldCacheRendering(pageNo)) {
        gRenderCache.RequestRendering(dm, pageNo);
    }
}

void ControllerCallbackHandler::CleanUp(DisplayModel* dm) {
    gRenderCache.CancelRendering(dm);
    gRenderCache.FreeForDisplayModel(dm);
}

void ControllerCallbackHandler::FocusFrame(bool always) {
    if (always || !FindWindowInfoByHwnd(GetFocus())) {
        SetFocus(win->hwndFrame);
    }
}

void ControllerCallbackHandler::SaveDownload(const WCHAR* url, ByteSlice data) {
    AutoFreeWstr fileName(url::GetFileName(url));
    // LinkSaver linkSaver(win->currentTab, win->hwndFrame, fileName);
    SaveDataToFile(win->hwndFrame, fileName, data);
}

void ControllerCallbackHandler::UpdateScrollbars(Size canvas) {
    CrashIf(!win->AsFixed());
    DisplayModel* dm = win->AsFixed();

    SCROLLINFO si = {0};
    si.cbSize = sizeof(si);
    si.fMask = SIF_ALL;

    Size viewPort = dm->GetViewPort().Size();

    if (viewPort.dx >= canvas.dx) {
        si.nPos = 0;
        si.nMin = 0;
        si.nMax = 99;
        si.nPage = 100;
    } else {
        si.nPos = dm->GetViewPort().x;
        si.nMin = 0;
        si.nMax = canvas.dx - 1;
        si.nPage = viewPort.dx;
    }
    ShowScrollBar(win->hwndCanvas, SB_HORZ, viewPort.dx < canvas.dx);
    SetScrollInfo(win->hwndCanvas, SB_HORZ, &si, TRUE);

    if (viewPort.dy >= canvas.dy) {
        si.nPos = 0;
        si.nMin = 0;
        si.nMax = 99;
        si.nPage = 100;
    } else {
        si.nPos = dm->GetViewPort().y;
        si.nMin = 0;
        si.nMax = canvas.dy - 1;
        si.nPage = viewPort.dy;

        if (ZOOM_FIT_PAGE != dm->GetZoomVirtual()) {
            // keep the top/bottom 5% of the previous page visible after paging down/up
            si.nPage = (uint)(si.nPage * 0.95);
            si.nMax -= viewPort.dy - si.nPage;
        }
    }
    ShowScrollBar(win->hwndCanvas, SB_VERT, viewPort.dy < canvas.dy);
    SetScrollInfo(win->hwndCanvas, SB_VERT, &si, TRUE);
}

// The current page edit box is updated with the current page number
void ControllerCallbackHandler::PageNoChanged(Controller* ctrl, int pageNo) {
    // discard page number change requests from documents
    // loaded asynchronously in a background tab
    if (win->ctrl != ctrl) {
        return;
    }

    CrashIf(!win->ctrl || win->ctrl->PageCount() <= 0);
    if (!win->ctrl || win->ctrl->PageCount() == 0) {
        return;
    }

    if (INVALID_PAGE_NO != pageNo) {
        AutoFreeWstr buf(win->ctrl->GetPageLabel(pageNo));
        win::SetText(win->hwndPageBox, buf);
        ToolbarUpdateStateForWindow(win, false);
        if (win->ctrl->HasPageLabels()) {
            UpdateToolbarPageText(win, win->ctrl->PageCount(), true);
        }
    }
    if (pageNo == win->currPageNo) {
        return;
    }

    UpdateTocSelection(win, pageNo);
    win->currPageNo = pageNo;

    NotificationWnd* wnd = win->notifications->GetForGroup(NG_PAGE_INFO_HELPER);
    if (wnd) {
        CrashIf(!win->AsFixed());
        UpdatePageInfoHelper(win, wnd, pageNo);
    }
}

static Controller* CreateControllerForEngine(EngineBase* engine, const WCHAR* filePath, PasswordUI* pwdUI,
                                             WindowInfo* win) {
    int nPages = engine ? engine->PageCount() : 0;
    logf(L"CreateControllerForEngine: '%s', %d pages\n", filePath), nPages;
    if (!win->cbHandler) {
        win->cbHandler = new ControllerCallbackHandler(win);
    }
    Controller* ctrl = nullptr;
    ctrl = new DisplayModel(engine, win->cbHandler);
    CrashIf(!ctrl || !ctrl->AsFixed() || ctrl->AsChm());
    return ctrl;
}

// TODO: remove when we figure out why this ctrl->GetFilePath() is not always same as path
static NO_INLINE void VerifyController(Controller* ctrl, const WCHAR* path) {
    if (!ctrl) {
        return;
    }
    if (str::Eq(ctrl->GetFilePath(), path)) {
        return;
    }
    const WCHAR* ctrlFilePath = ctrl->GetFilePath();
    char* s1 = ctrlFilePath ? strconv::WstrToUtf8(ctrlFilePath) : str::Dup("<null>");
    char* s2 = path ? strconv::WstrToUtf8(path) : str::Dup("<null>");
    logf("VerifyController: ctrl->FilePath: '%s', filePath: '%s'\n", s1, s2);
    CrashIf(true);
    str::Free(s1);
    str::Free(s2);
}

static Controller* CreateForChm(const WCHAR* path, PasswordUI* pwdUI, WindowInfo* win) {
    Kind kind = GuessFileType(path, true);

    bool isChm = ChmModel::IsSupportedFileType(kind);
    if (!isChm) {
        return nullptr;
    }
    ChmModel* chmModel = ChmModel::Create(path, win->cbHandler);
    if (!chmModel) {
        return nullptr;
    }
    // make sure that MSHTML can't be used as a potential exploit
    // vector through another browser and our plugin (which doesn't
    // advertise itself for Chm documents but could be tricked into
    // loading one nonetheless); note: this crash should never happen,
    // since gGlobalPrefs->chmUI.useFixedPageUI is set in SetupPluginMode
    CrashAlwaysIf(gPluginMode);
    // if CLSID_WebBrowser isn't available, fall back on ChmEngine
    Controller* ctrl = nullptr;
    if (!chmModel->SetParentHwnd(win->hwndCanvas)) {
        delete chmModel;
        EngineBase* engine = CreateEngine(path, pwdUI, true);
        if (!engine) {
            return nullptr;
        }
        CrashIf(engine->kind != kindEngineChm);
        ctrl = new DisplayModel(engine, win->cbHandler);
        CrashIf(!ctrl || !ctrl->AsFixed() || ctrl->AsChm());
    } else {
        // another ChmModel might still be active
        chmModel->RemoveParentHwnd();
        ctrl = chmModel;
    }
    CrashIf(ctrl && (!ctrl->AsChm() || ctrl->AsFixed()));
    VerifyController(ctrl, path);
    return ctrl;
}

static Controller* CreateControllerForFile(const WCHAR* path, PasswordUI* pwdUI, WindowInfo* win) {
    if (!win->cbHandler) {
        win->cbHandler = new ControllerCallbackHandler(win);
    }

    Controller* ctrl = nullptr;

    bool chmInFixedUI = gGlobalPrefs->chmUI.useFixedPageUI;

    // TODO: sniff file content only once
    EngineBase* engine = CreateEngine(path, pwdUI, chmInFixedUI);

    if (engine) {
        ctrl = new DisplayModel(engine, win->cbHandler);
        CrashIf(!ctrl || !ctrl->AsFixed() || ctrl->AsChm());
        VerifyController(ctrl, path);
        logf(L"CreateControllerForFile: '%s', %d pages\n", path, engine->PageCount());
        return ctrl;
    }

    if (chmInFixedUI) {
        return nullptr;
    }
    ctrl = CreateForChm(path, pwdUI, win);
    if (!ctrl) {
        return nullptr;
    }
    logf(L"CreateControllerForFile: '%s', %d pages\n", path, ctrl->PageCount());
    return ctrl;
}

static void SetFrameTitleForTab(TabInfo* tab, bool needRefresh) {
    const WCHAR* titlePath = tab->filePath;
    if (!gGlobalPrefs->fullPathInTitle) {
        titlePath = path::GetBaseNameTemp(titlePath);
    }

    AutoFreeWstr docTitle(str::Dup(L""));
    if (tab->ctrl) {
        WCHAR* title = tab->ctrl->GetProperty(DocumentProperty::Title);
        if (title != nullptr) {
            str::NormalizeWSInPlace(title);
            docTitle.Set(title);
            if (!str::IsEmpty(title)) {
                docTitle.Set(str::Format(L"- [%s] ", title));
            }
        }
    }

    if (!IsUIRightToLeft()) {
        tab->frameTitle.Set(str::Format(L"%s %s- %s", titlePath, docTitle.Get(), SUMATRA_WINDOW_TITLE));
    } else {
        // explicitly revert the title, so that filenames aren't garbled
        tab->frameTitle.Set(str::Format(L"%s %s- %s", SUMATRA_WINDOW_TITLE, docTitle.Get(), titlePath));
    }
    if (needRefresh && tab->ctrl) {
        // TODO: this isn't visible when tabs are used
        tab->frameTitle.Set(str::Format(_TR("[Changes detected; refreshing] %s"), tab->frameTitle.Get()));
    }
}

static void UpdateUiForCurrentTab(WindowInfo* win) {
    // hide the scrollbars before any other relayouting (for assertion in WindowInfo::GetViewPortSize)
    if (!win->AsFixed()) {
        ShowScrollBar(win->hwndCanvas, SB_BOTH, FALSE);
    }

    // menu for chm and ebook docs is different, so we have to re-create it
    RebuildMenuBarForWindow(win);
    // the toolbar isn't supported for ebook docs (yet)
    ShowOrHideToolbar(win);
    // TODO: unify?
    ToolbarUpdateStateForWindow(win, true);
    UpdateToolbarState(win);

    int pageCount = win->ctrl ? win->ctrl->PageCount() : 0;
    UpdateToolbarPageText(win, pageCount);
    UpdateToolbarFindText(win);

    OnMenuFindMatchCase(win);
    UpdateFindbox(win);

    win::SetText(win->hwndFrame, win->currentTab->frameTitle);
    UpdateCurrentTabBgColor(win);

    bool onlyNumbers = !win->ctrl || !win->ctrl->HasPageLabels();
    SetWindowStyle(win->hwndPageBox, ES_NUMBER, onlyNumbers);
}

static bool showTocByDefault(const WCHAR* path) {
    if (!gGlobalPrefs->showToc) {
        return false;
    }
    // we don't want to show toc by default for comic book files
    Kind kind = GuessFileTypeFromName(path);
    bool showByDefault = !IsEngineCbxSupportedFileType(kind);
    return showByDefault;
}

// meaning of the internal values of LoadArgs:
// isNewWindow : if true then 'win' refers to a newly created window that needs
//   to be resized and placed
// placeWindow : if true then the Window will be moved/sized according
//   to the 'state' information even if the window was already placed
//   before (isNewWindow=false)
static void LoadDocIntoCurrentTab(const LoadArgs& args, Controller* ctrl, FileState* fs) {
    WindowInfo* win = args.win;
    CrashIf(!win);
    if (!win) {
        return;
    }
    TabInfo* tab = win->currentTab;
    CrashIf(!tab);

    // Never load settings from a preexisting state if the user doesn't wish to
    // (unless we're just refreshing the document, i.e. only if state && !state->useDefaultState)
    if (!fs && gGlobalPrefs->rememberStatePerDocument) {
        char* fn = ToUtf8Temp(args.fileName);
        fs = gFileHistory.Find(fn, nullptr);
        if (fs) {
            if (fs->windowPos.IsEmpty()) {
                fs->windowPos = gGlobalPrefs->windowPos;
            }
            EnsureAreaVisibility(fs->windowPos);
        }
    }
    if (fs && fs->useDefaultState) {
        fs = nullptr;
    }

    DisplayMode displayMode = gGlobalPrefs->defaultDisplayModeEnum;
    float zoomVirtual = gGlobalPrefs->defaultZoomFloat;
    ScrollState ss(1, -1, -1);
    int rotation = 0;
    bool showToc = showTocByDefault(args.fileName);
    bool showAsFullScreen = WIN_STATE_FULLSCREEN == gGlobalPrefs->windowState;
    int showType = SW_NORMAL;
    if (gGlobalPrefs->windowState == WIN_STATE_MAXIMIZED || showAsFullScreen) {
        showType = SW_MAXIMIZE;
    }

    if (fs) {
        ss.page = fs->pageNo;
        displayMode = DisplayModeFromString(fs->displayMode, DisplayMode::Automatic);
        showAsFullScreen = WIN_STATE_FULLSCREEN == fs->windowState;
        if (fs->windowState == WIN_STATE_NORMAL) {
            showType = SW_NORMAL;
        } else if (fs->windowState == WIN_STATE_MAXIMIZED || showAsFullScreen) {
            showType = SW_MAXIMIZE;
        } else if (fs->windowState == WIN_STATE_MINIMIZED) {
            showType = SW_MINIMIZE;
        }
        showToc = fs->showToc;
        if (win->ctrl && win->presentation) {
            showToc = tab->showTocPresentation;
        }
    }

    AbortFinding(args.win, false);

    Controller* prevCtrl = win->ctrl;
    tab->ctrl = ctrl;
    win->ctrl = tab->ctrl;

    // ToC items might hold a reference to an Engine, so make sure to
    // delete them before destroying the whole DisplayModel
    // (same for linkOnLastButtonDown)
    ClearTocBox(win);
    ClearMouseState(win);

    CrashIf(win->IsAboutWindow() || win->IsDocLoaded() != (win->ctrl != nullptr));
    // TODO: https://code.google.com/p/sumatrapdf/issues/detail?id=1570
    if (win->ctrl) {
        DisplayModel* dm = win->AsFixed();
        if (dm) {
            int dpi = gGlobalPrefs->customScreenDPI;
            if (dpi == 0) {
                dpi = DpiGetForHwnd(win->hwndFrame);
            }
            dm->SetInitialViewSettings(displayMode, ss.page, win->GetViewPortSize(), dpi);
            // TODO: also expose Manga Mode for image folders?
            if (tab->GetEngineType() == kindEngineComicBooks || tab->GetEngineType() == kindEngineImageDir) {
                dm->SetDisplayR2L(fs ? fs->displayR2L : gGlobalPrefs->comicBookUI.cbxMangaMode);
            }
            if (prevCtrl && prevCtrl->AsFixed() && str::Eq(win->ctrl->GetFilePath(), prevCtrl->GetFilePath())) {
                gRenderCache.KeepForDisplayModel(prevCtrl->AsFixed(), dm);
                dm->CopyNavHistory(*prevCtrl->AsFixed());
            }
            // tell UI Automation about content change
            if (win->uiaProvider) {
                win->uiaProvider->OnDocumentLoad(dm);
            }
        } else if (win->AsChm()) {
            win->AsChm()->SetParentHwnd(win->hwndCanvas);
            win->ctrl->SetDisplayMode(displayMode);
            ss.page = limitValue(ss.page, 1, win->ctrl->PageCount());
            win->ctrl->GoToPage(ss.page, false);
        } else {
            CrashIf(true);
        }
    } else {
        fs = nullptr;
    }
    delete prevCtrl;

    if (fs) {
        CrashIf(!win->IsDocLoaded());
        zoomVirtual = ZoomFromString(fs->zoom, ZOOM_FIT_PAGE);
        if (win->ctrl->ValidPageNo(ss.page)) {
            if (ZOOM_FIT_CONTENT != zoomVirtual) {
                ss.x = fs->scrollPos.x;
                ss.y = fs->scrollPos.y;
            }
            // else let win->AsFixed()->Relayout() scroll to fit the page (again)
        } else if (win->ctrl->PageCount() > 0) {
            ss.page = limitValue(ss.page, 1, win->ctrl->PageCount());
        }
        // else let win->ctrl->GoToPage(ss.page, false) verify the page number
        rotation = fs->rotation;
        tab->tocState = *fs->tocState;
    }

    // DisplayModel needs a valid zoom value before any relayout
    // caused by showing/hiding UI elements happends
    if (win->AsFixed()) {
        win->AsFixed()->Relayout(zoomVirtual, rotation);
    } else if (win && win->ctrl && win->IsDocLoaded()) {
        win->ctrl->SetZoomVirtual(zoomVirtual, nullptr);
    }

    // TODO: why is this needed?
    if (!args.isNewWindow && win->IsDocLoaded()) {
        win->RedrawAll();
    }

    SetFrameTitleForTab(tab, false);
    UpdateUiForCurrentTab(win);

    if (HasPermission(Perm::DiskAccess) && tab->GetEngineType() == kindEngineMupdf) {
        CrashIf(!win->AsFixed() || win->AsFixed()->pdfSync);
        int res = Synchronizer::Create(args.fileName, win->AsFixed()->GetEngine(), &win->AsFixed()->pdfSync);
        // expose SyncTeX in the UI
        if (PDFSYNCERR_SUCCESS == res) {
            gGlobalPrefs->enableTeXEnhancements = true;
        }
    }

    bool shouldPlace = args.isNewWindow || args.placeWindow && fs;
    if (args.noPlaceWindow) {
        shouldPlace = false;
    }
    if (shouldPlace) {
        if (args.isNewWindow && fs && !fs->windowPos.IsEmpty()) {
            // Make sure it doesn't have a position like outside of the screen etc.
            Rect rect = ShiftRectToWorkArea(fs->windowPos);
            // This shouldn't happen until !win.IsAboutWindow(), so that we don't
            // accidentally update gGlobalState with this window's dimensions
            MoveWindow(win->hwndFrame, rect);
        }
        if (args.showWin) {
            ShowWindow(win->hwndFrame, showType);
        }
        if (win) {
            UpdateWindow(win->hwndFrame);
        }
    }

    // if the window isn't shown and win.canvasRc is still empty, zoom
    // has not been determined yet
    // cf. https://code.google.com/p/sumatrapdf/issues/detail?id=2541
    // CrashIf(win->IsDocLoaded() && args.showWin && win->canvasRc.IsEmpty() && !win->AsChm());

    SetSidebarVisibility(win, showToc, gGlobalPrefs->showFavorites);
    // restore scroll state after the canvas size has been restored
    if ((args.showWin || ss.page != 1) && win->AsFixed()) {
        win->AsFixed()->SetScrollState(ss);
    }

    win->RedrawAll(true);
    TabsOnChangedDoc(win);

    if (!win->IsDocLoaded()) {
        return;
    }

    AutoFreeWstr unsupported(win->ctrl->GetProperty(DocumentProperty::UnsupportedFeatures));
    if (unsupported) {
        unsupported.Set(str::Format(_TR("This document uses unsupported features (%s) and might not render properly"),
                                    unsupported.Get()));
        win->ShowNotification(unsupported, NotificationOptions::Warning, NG_PERSISTENT_WARNING);
    }

    // This should only happen after everything else is ready
    if ((args.isNewWindow || args.placeWindow) && args.showWin && showAsFullScreen) {
        EnterFullScreen(win);
    }
    if (!args.isNewWindow && win->presentation && win->ctrl) {
        win->ctrl->SetPresentationMode(true);
    }
}

void ReloadDocument(WindowInfo* win, bool autoRefresh) {
    // TODO: must disable reload for EngineMulti representing a directory
    TabInfo* tab = win->currentTab;

    // we can't reload while having annotations window open because
    // that invalidates the mupdf objects that we hold in editAnnotsWindow
    // TODO: a better approach would be to have a callback that editAnnotsWindow
    // would register for and re-do its state
    if (!tab || tab->editAnnotsWindow) {
        return;
    }
    if (!win->IsDocLoaded()) {
        if (!autoRefresh) {
            LoadArgs args(tab->filePath, win);
            args.forceReuse = true;
            args.noSavePrefs = true;
            LoadDocument(args);
        }
        return;
    }

    HwndPasswordUI pwdUI(win->hwndFrame);
    Controller* ctrl = CreateControllerForFile(tab->filePath, &pwdUI, win);
    // We don't allow PDF-repair if it is an autorefresh because
    // a refresh event can occur before the file is finished being written,
    // in which case the repair could fail. Instead, if the file is broken,
    // we postpone the reload until the next autorefresh event
    if (!ctrl && autoRefresh) {
        SetFrameTitleForTab(tab, true);
        win::SetText(win->hwndFrame, tab->frameTitle);
        return;
    }

    char* tabFilePath = ToUtf8Temp(tab->filePath);
    FileState* fs = NewDisplayState(tabFilePath);
    tab->ctrl->GetDisplayState(fs);
    UpdateDisplayStateWindowRect(win, fs);
    UpdateSidebarDisplayState(tab, fs);
    // Set the windows state based on the actual window's placement
    int wstate = WIN_STATE_NORMAL;
    if (win->isFullScreen) {
        wstate = WIN_STATE_FULLSCREEN;
    } else {
        if (IsZoomed(win->hwndFrame)) {
            wstate = WIN_STATE_MAXIMIZED;
        } else if (IsIconic(win->hwndFrame)) {
            wstate = WIN_STATE_MINIMIZED;
        }
    }
    fs->windowState = wstate;
    fs->useDefaultState = false;

    LoadArgs args(tab->filePath, win);
    args.showWin = true;
    args.placeWindow = false;
    LoadDocIntoCurrentTab(args, ctrl, fs);

    if (!ctrl) {
        DeleteDisplayState(fs);
        return;
    }

    tab->reloadOnFocus = false;

    if (gGlobalPrefs->showStartPage) {
        // refresh the thumbnail for this file
        FileState* state = gFileHistory.Find(fs->filePath, nullptr);
        if (state) {
            CreateThumbnailForFile(win, *state);
        }
    }

    if (tab->AsFixed()) {
        // save a newly remembered password into file history so that
        // we don't ask again at the next refresh
        AutoFree decryptionKey(tab->AsFixed()->GetEngine()->GetDecryptionKey());
        if (decryptionKey) {
            FileState* fs2 = gFileHistory.Find(fs->filePath, nullptr);
            if (fs2 && !str::Eq(fs2->decryptionKey, decryptionKey)) {
                free(fs2->decryptionKey);
                fs2->decryptionKey = decryptionKey.Release();
            }
        }
    }

    DeleteDisplayState(fs);
}

static void CreateSidebar(WindowInfo* win) {
    win->sidebarSplitter = new SplitterCtrl(win->hwndFrame);
    win->sidebarSplitter->type = SplitterType::Vert;
    win->sidebarSplitter->onSplitterMove = OnSidebarSplitterMove;
    bool ok = win->sidebarSplitter->Create();
    CrashIf(!ok);

    CreateToc(win);

    win->favSplitter = new SplitterCtrl(win->hwndFrame);
    win->favSplitter->type = SplitterType::Horiz;
    win->favSplitter->onSplitterMove = OnFavSplitterMove;
    ok = win->favSplitter->Create();
    CrashIf(!ok);

    CreateFavorites(win);

    if (win->tocVisible) {
        RepaintNow(win->hwndTocBox);
    }

    if (gGlobalPrefs->showFavorites) {
        RepaintNow(win->hwndFavBox);
    }
}

static void UpdateToolbarSidebarText(WindowInfo* win) {
    UpdateToolbarPageText(win, -1);
    UpdateToolbarFindText(win);
    UpdateToolbarButtonsToolTipsForWindow(win);

    win->tocLabelWithClose->SetLabel(_TR("Bookmarks"));
    win->favLabelWithClose->SetLabel(_TR("Favorites"));
}

static WindowInfo* CreateWindowInfo() {
    Rect windowPos = gGlobalPrefs->windowPos;
    if (!windowPos.IsEmpty()) {
        EnsureAreaVisibility(windowPos);
    } else {
        windowPos = GetDefaultWindowPos();
    }
    // we don't want the windows to overlap so shift each window by a bit
    int nShift = (int)gWindows.size();
    windowPos.x += (nShift * 15); // TODO: DPI scale

    const WCHAR* clsName = FRAME_CLASS_NAME;
    const WCHAR* title = SUMATRA_WINDOW_TITLE;
    DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
    int x = windowPos.x;
    int y = windowPos.y;
    int dx = windowPos.dx;
    int dy = windowPos.dy;
    HINSTANCE h = GetModuleHandle(nullptr);
    HWND hwndFrame = CreateWindowExW(0, clsName, title, style, x, y, dx, dy, nullptr, nullptr, h, nullptr);
    if (!hwndFrame) {
        return nullptr;
    }

    CrashIf(nullptr != FindWindowInfoByHwnd(hwndFrame));
    WindowInfo* win = new WindowInfo(hwndFrame);

    // don't add a WS_EX_STATICEDGE so that the scrollbars touch the
    // screen's edge when maximized (cf. Fitts' law) and there are
    // no additional adjustments needed when (un)maximizing
    clsName = CANVAS_CLASS_NAME;
    style = WS_CHILD | WS_HSCROLL | WS_VSCROLL | WS_CLIPCHILDREN;
    /* position and size determined in OnSize */
    win->hwndCanvas = CreateWindowExW(0, clsName, nullptr, style, 0, 0, 0, 0, hwndFrame, nullptr, h, nullptr);
    if (!win->hwndCanvas) {
        delete win;
        return nullptr;
    }

    if (gShowFrameRate) {
        win->frameRateWnd = new FrameRateWnd();
        win->frameRateWnd->Create(win->hwndCanvas);
    }

    // hide scrollbars to avoid showing/hiding on empty window
    ShowScrollBar(win->hwndCanvas, SB_BOTH, FALSE);

    CrashIf(win->menu);
    win->menu = BuildMenu(win);
    win->isMenuHidden = !gGlobalPrefs->showMenubar;
    if (!win->isMenuHidden) {
        SetMenu(win->hwndFrame, win->menu);
    }

    ShowWindow(win->hwndCanvas, SW_SHOW);
    UpdateWindow(win->hwndCanvas);

    win->infotip = new TooltipCtrl(win->hwndCanvas);
    win->infotip->Create();

    CreateCaption(win);
    CreateTabbar(win);
    CreateToolbar(win);
    CreateSidebar(win);
    UpdateFindbox(win);
    if (HasPermission(Perm::DiskAccess) && !gPluginMode) {
        DragAcceptFiles(win->hwndCanvas, TRUE);
    }

    gWindows.Append(win);
    // needed for RTL languages
    UpdateWindowRtlLayout(win);
    UpdateToolbarSidebarText(win);

    if (touch::SupportsGestures()) {
        GESTURECONFIG gc = {0, GC_ALLGESTURES, 0};
        touch::SetGestureConfig(win->hwndCanvas, 0, 1, &gc, sizeof(GESTURECONFIG));
    }

    SetTabsInTitlebar(win, gGlobalPrefs->useTabs);

    return win;
}

WindowInfo* CreateAndShowWindowInfo(SessionData* data) {
    // CreateWindowInfo shouldn't change the windowState value
    int windowState = gGlobalPrefs->windowState;
    WindowInfo* win = CreateWindowInfo();
    if (!win) {
        return nullptr;
    }
    CrashIf(windowState != gGlobalPrefs->windowState);

    if (data) {
        windowState = data->windowState;
        Rect rect = ShiftRectToWorkArea(data->windowPos);
        MoveWindow(win->hwndFrame, rect);
        // TODO: also restore data->sidebarDx
    }

    if (WIN_STATE_FULLSCREEN == windowState || WIN_STATE_MAXIMIZED == windowState) {
        ShowWindow(win->hwndFrame, SW_MAXIMIZE);
    } else {
        ShowWindow(win->hwndFrame, SW_SHOW);
    }
    UpdateWindow(win->hwndFrame);

    SetSidebarVisibility(win, false, gGlobalPrefs->showFavorites);
    ToolbarUpdateStateForWindow(win, true);

    if (WIN_STATE_FULLSCREEN == windowState) {
        EnterFullScreen(win);
    }
    return win;
}

void DeleteWindowInfo(WindowInfo* win) {
    DeletePropertiesWindow(win->hwndFrame);

    gWindows.Remove(win);

    ImageList_Destroy((HIMAGELIST)SendMessageW(win->hwndToolbar, TB_GETIMAGELIST, 0, 0));
    DragAcceptFiles(win->hwndCanvas, FALSE);

    CrashIf(win->findThread && WaitForSingleObject(win->findThread, 0) == WAIT_TIMEOUT);
    CrashIf(win->printThread && WaitForSingleObject(win->printThread, 0) == WAIT_TIMEOUT);

    if (win->uiaProvider) {
        // tell UIA to release all objects cached in its store
        UiaReturnRawElementProvider(win->hwndCanvas, 0, 0, nullptr);
    }

    delete win;
}

static void RenameFileInHistory(const WCHAR* oldPath, const WCHAR* newPath) {
    if (path::IsSame(oldPath, newPath)) {
        return;
    }
    char* newPathA = ToUtf8Temp(newPath);
    FileState* fs = gFileHistory.Find(newPathA, nullptr);
    bool oldIsPinned = false;
    int oldOpenCount = 0;
    if (fs) {
        oldIsPinned = fs->isPinned;
        oldOpenCount = fs->openCount;
        gFileHistory.Remove(fs);
        // TODO: merge favorites as well?
        if (fs->favorites->size() > 0) {
            UpdateFavoritesTreeForAllWindows();
        }
        DeleteDisplayState(fs);
    }
    char* oldPathA = ToUtf8Temp(oldPath);
    fs = gFileHistory.Find(oldPathA, nullptr);
    if (fs) {
        char* filePathA = ToUtf8Temp(newPath);
        SetFileStatePath(fs, filePathA);
        // merge Frequently Read data, so that a file
        // doesn't accidentally vanish from there
        fs->isPinned = fs->isPinned || oldIsPinned;
        fs->openCount += oldOpenCount;
        // the thumbnail is recreated by LoadDocument
        delete fs->thumbnail;
        fs->thumbnail = nullptr;
    }
}

// document path is either a file or a directory
// (when browsing images inside directory).
bool DocumentPathExists(const WCHAR* path) {
    if (file::Exists(path) || dir::Exists(path)) {
        return true;
    }
    if (str::FindChar(path + 2, ':')) {
        // remove information needed for pointing at embedded documents
        // (e.g. "C:\path\file.pdf:3:0") to check at least whether the
        // container document exists
        AutoFreeWstr realPath(str::Dup(path, str::FindChar(path + 2, ':') - path));
        return file::Exists(realPath);
    }
    return false;
}

#if 0
// Load a file into a new or existing window, show error message
// if loading failed, set the right window position (based on history
// settings for this file or default position), update file history,
// update frequently read information, generate a thumbnail if necessary
// TODO: write me
static WindowInfo* LoadDocumentNew(LoadArgs& args)
{
    AutoFreeWstr fullPath(path::Normalize(args.fileName));
    // TODO: try to find file on other drives if doesn't exist

    CrashIf(true);
    return nullptr;
}
#endif

static void scheduleReloadTab(TabInfo* tab) {
    uitask::Post([=] {
        // tab might have been closed, so first ensure it's still valid
        // https://github.com/sumatrapdfreader/sumatrapdf/issues/1958
        WindowInfo* win = FindWindowInfoByTabInfo(tab);
        if (win == nullptr) {
            return;
        }
        tab->reloadOnFocus = true;
        if (tab == win->currentTab) {
            // delay the reload slightly, in case we get another request immediately after this one
            SetTimer(win->hwndCanvas, AUTO_RELOAD_TIMER_ID, AUTO_RELOAD_DELAY_IN_MS, nullptr);
        }
    });
}

// TODO: eventually I would like to move all loading to be async. To achieve that
// we need clear separatation of loading process into 2 phases: loading the
// file (and showing progress/load failures in topmost window) and placing
// the loaded document in the window (either by replacing document in existing
// window or creating a new window for the document)
WindowInfo* LoadDocument(LoadArgs& args) {
    CrashAlwaysIf(gCrashOnOpen);

    int threadID = (int)GetCurrentThreadId();
    AutoFreeWstr fullPath(path::Normalize(args.fileName));
    WindowInfo* win = args.win;
    {
        auto path = ToUtf8Temp(fullPath);
        logf("LoadDocument: '%s' tid=%d\n", path.Get(), threadID);
    }

    bool failEarly = win && !args.forceReuse && !DocumentPathExists(fullPath);
    // try to find inexistent files with history data
    // on a different removable drive before failing
    char* fullPathA = ToUtf8Temp(fullPath);
    if (failEarly && gFileHistory.Find(fullPathA, nullptr)) {
        AutoFreeWstr adjPath(str::Dup(fullPath));
        if (AdjustVariableDriveLetter(adjPath)) {
            RenameFileInHistory(fullPath, adjPath);
            fullPath.Set(adjPath.StealData());
            fullPathA = ToUtf8Temp(fullPath);
            failEarly = false;
        }
    }

    // fail with a notification if the file doesn't exist and
    // there is a window the user has just been interacting with
    if (failEarly) {
        AutoFreeWstr msg(str::Format(_TR("File %s not found"), fullPath.Get()));
        win->ShowNotification(msg, NotificationOptions::Highlight);
        // display the notification ASAP (prefs::Save() can introduce a notable delay)
        win->RedrawAll(true);

        if (gFileHistory.MarkFileInexistent(fullPathA)) {
            // TODO: handle this better. see https://github.com/sumatrapdfreader/sumatrapdf/issues/1674
            if (!args.noSavePrefs) {
                prefs::Save();
            }
            // update the Frequently Read list
            if (1 == gWindows.size() && gWindows.at(0)->IsAboutWindow()) {
                gWindows.at(0)->RedrawAll(true);
            }
        }
        return nullptr;
    }

    bool openNewTab = gGlobalPrefs->useTabs && !args.forceReuse;
    if (openNewTab && !args.win) {
        // modify the args so that we always reuse the same window
        // TODO: enable the tab bar if tabs haven't been initialized
        if (!gWindows.empty()) {
            win = args.win = gWindows.Last();
            args.isNewWindow = false;
        }
    }

    if (!win && 1 == gWindows.size() && gWindows.at(0)->IsAboutWindow()) {
        win = gWindows.at(0);
        args.win = win;
        args.isNewWindow = false;
    } else if (!win || !openNewTab && !args.forceReuse && win->IsDocLoaded()) {
        WindowInfo* currWin = win;
        win = CreateWindowInfo();
        if (!win) {
            return nullptr;
        }
        args.win = win;
        args.isNewWindow = true;
        if (currWin) {
            RememberFavTreeExpansionState(currWin);
            win->expandedFavorites = currWin->expandedFavorites;
        }
    }

    HwndPasswordUI pwdUI(win->hwndFrame);
    Controller* ctrl = nullptr;
    if (args.engine != nullptr) {
        ctrl = CreateControllerForEngine(args.engine, fullPath, &pwdUI, win);
    } else {
        ctrl = CreateControllerForFile(fullPath, &pwdUI, win);
    }

    if (!ctrl) {
        // TODO: same message as in Canvas.cpp to not introduce
        // new translation. Find a better message e.g. why failed.
        WCHAR* msg = str::Format(_TR("Error loading %s"), fullPath.Get());
        win->ShowNotification(msg, NotificationOptions::Highlight);
        str::Free(msg);
        ShowWindow(win->hwndFrame, SW_SHOW);

        // display the notification ASAP (prefs::Save() can introduce a notable delay)
        win->RedrawAll(true);

        fullPathA = ToUtf8Temp(fullPath);
        if (gFileHistory.MarkFileInexistent(fullPathA)) {
            // TODO: handle this better. see https://github.com/sumatrapdfreader/sumatrapdf/issues/1674
            if (!args.noSavePrefs) {
                prefs::Save();
            }
            // update the Frequently Read list
            if (1 == gWindows.size() && gWindows.at(0)->IsAboutWindow()) {
                gWindows.at(0)->RedrawAll(true);
            }
        }
        return win;
    }
    CrashIf(openNewTab && args.forceReuse);
    if (win->IsAboutWindow()) {
        // invalidate the links on the Frequently Read page
        DeleteVecMembers(win->staticLinks);
        // there's no tab to reuse at this point
        args.forceReuse = false;
    } else {
        // TODO: figure out why happens. seen in 2019/12/11/3e06348ed000006.txt
        if (!args.forceReuse && !openNewTab) {
            logf("LoadDocument: got !args.forceReuse && !openNewTab\n");
        }
        ReportIf(!args.forceReuse && !openNewTab);
        if (openNewTab) {
            SaveCurrentTabInfo(args.win);
        }
        CloseDocumentInCurrentTab(win, true, args.forceReuse);
    }
    if (!args.forceReuse) {
        // insert a new tab for the loaded document
        win->currentTab = CreateNewTab(win, fullPath);
        logf("LoadDocument: !forceReuse, created win->currentTab at 0x%p\n", win->currentTab);
    } else {
        win->currentTab->filePath.SetCopy(fullPath);
        auto path = ToUtf8Temp(fullPath);
        logf("LoadDocument: forceReuse, set win->currentTab (0x%p) filePath to '%s'\n", win->currentTab, path.Get());
    }

    args.fileName = fullPath;
    // TODO: stop remembering/restoring window positions when using tabs?
    args.placeWindow = !gGlobalPrefs->useTabs;
    LoadDocIntoCurrentTab(args, ctrl, nullptr);

    if (gPluginMode) {
        // hide the menu for embedded documents opened from the plugin
        SetMenu(win->hwndFrame, nullptr);
        return win;
    }

    auto currTab = win->currentTab;
    auto path = ToUtf8Temp(currTab->filePath);
    int nPages = 0;
    if (currTab->ctrl) {
        nPages = currTab->ctrl->PageCount();
    }
    logf("LoadDocument: after LoadDocIntoCurrentTab win->currentTab is 0x%p, path: '%s', %d pages\n", currTab,
         path.Get(), nPages);

    // TODO: figure why we hit this.
    // happens when opening 3 files via "Open With"
    // the first file is loaded via cmd-line arg, the rest
    // via DDE Open command.
    CrashIf(currTab->watcher);

    if (gGlobalPrefs->reloadModifiedDocuments) {
        currTab->watcher = FileWatcherSubscribe(win->currentTab->filePath, [currTab] { scheduleReloadTab(currTab); });
    }

    if (gGlobalPrefs->rememberOpenedFiles) {
        CrashIf(!str::Eq(fullPath, win->currentTab->filePath));
        fullPathA = ToUtf8Temp(fullPath);
        FileState* ds = gFileHistory.MarkFileLoaded(fullPathA);
        if (gGlobalPrefs->showStartPage) {
            CreateThumbnailForFile(win, *ds);
        }
        // TODO: this seems to save the state of file that we just opened
        // add a way to skip saving currTab?
        if (!args.noSavePrefs) {
            prefs::Save();
        }
    }

    // Add the file also to Windows' recently used documents (this doesn't
    // happen automatically on drag&drop, reopening from history, etc.)
    if (HasPermission(Perm::DiskAccess) && !gPluginMode && !IsStressTesting()) {
        SHAddToRecentDocs(SHARD_PATH, fullPath);
    }

    return win;
}

// Loads document data into the WindowInfo.
void LoadModelIntoTab(TabInfo* tab) {
    if (!tab) {
        return;
    }
    WindowInfo* win = tab->win;
    CloseDocumentInCurrentTab(win, true);

    win->currentTab = tab;
    win->ctrl = tab->ctrl;

    if (win->AsChm()) {
        win->AsChm()->SetParentHwnd(win->hwndCanvas);
    } else if (win->AsFixed() && win->uiaProvider) {
        // tell UI Automation about content change
        win->uiaProvider->OnDocumentLoad(win->AsFixed());
    }

    UpdateUiForCurrentTab(win);

    if (win->presentation != PM_DISABLED) {
        SetSidebarVisibility(win, tab->showTocPresentation, gGlobalPrefs->showFavorites);
    } else {
        SetSidebarVisibility(win, tab->showToc, gGlobalPrefs->showFavorites);
    }

    if (win->AsFixed()) {
        if (tab->canvasRc != win->canvasRc) {
            win->ctrl->SetViewPortSize(win->GetViewPortSize());
        }
        DisplayModel* dm = win->AsFixed();
        dm->SetScrollState(dm->GetScrollState());
        if (dm->GetPresentationMode() != (win->presentation != PM_DISABLED)) {
            dm->SetPresentationMode(!dm->GetPresentationMode());
        }
    } else if (win->AsChm()) {
        win->ctrl->GoToPage(win->ctrl->CurrentPageNo(), false);
    }
    tab->canvasRc = win->canvasRc;

    win->showSelection = tab->selectionOnPage != nullptr;
    if (win->uiaProvider) {
        win->uiaProvider->OnSelectionChanged();
    }

    SetFocus(win->hwndFrame);
    win->RedrawAll(true);

    if (tab->reloadOnFocus) {
        tab->reloadOnFocus = false;
        ReloadDocument(win, true);
    }
}

static void UpdatePageInfoHelper(WindowInfo* win, NotificationWnd* wnd, int pageNo) {
    if (!win->ctrl->ValidPageNo(pageNo)) {
        pageNo = win->ctrl->CurrentPageNo();
    }
    AutoFreeWstr pageInfo(str::Format(L"%s %d / %d", _TR("Page:"), pageNo, win->ctrl->PageCount()));
    if (win->ctrl->HasPageLabels()) {
        AutoFreeWstr label(win->ctrl->GetPageLabel(pageNo));
        pageInfo.Set(str::Format(L"%s %s (%d / %d)", _TR("Page:"), label.Get(), pageNo, win->ctrl->PageCount()));
    }
    if (!wnd) {
        auto options = NotificationOptions::Persist;
        win->ShowNotification(pageInfo, options, NG_PAGE_INFO_HELPER);
    } else {
        wnd->UpdateMessage(pageInfo);
    }
}

static void TogglePageInfoHelper(WindowInfo* win) {
    NotificationWnd* wnd = win->notifications->GetForGroup(NG_PAGE_INFO_HELPER);
    if (wnd) {
        win->notifications->RemoveForGroup(NG_PAGE_INFO_HELPER);
        return;
    }
    UpdatePageInfoHelper(win, nullptr, -1);
}

enum class MeasurementUnit { pt, mm, in };

static WCHAR* FormatCursorPosition(EngineBase* engine, PointF pt, MeasurementUnit unit) {
    if (pt.x < 0) {
        pt.x = 0;
    }
    if (pt.y < 0) {
        pt.y = 0;
    }
    pt.x /= engine->GetFileDPI();
    pt.y /= engine->GetFileDPI();

    // for MeasurementUnit::in
    float factor = 1;
    const WCHAR* unitName = L"in";
    switch (unit) {
        case MeasurementUnit::pt:
            factor = 72;
            unitName = L"pt";
            break;

        case MeasurementUnit::mm:
            factor = 25.4f;
            unitName = L"mm";
            break;
    }

    AutoFreeWstr xPos(str::FormatFloatWithThousandSep((double)pt.x * (double)factor));
    AutoFreeWstr yPos(str::FormatFloatWithThousandSep((double)pt.y * (double)factor));
    if (unit != MeasurementUnit::in) {
        // use similar precision for all units
        if (str::IsDigit(xPos[str::Len(xPos) - 2])) {
            xPos[str::Len(xPos) - 1] = '\0';
        }
        if (str::IsDigit(yPos[str::Len(yPos) - 2])) {
            yPos[str::Len(yPos) - 1] = '\0';
        }
    }
    return str::Format(L"%s x %s %s", xPos.Get(), yPos.Get(), unitName);
}

void UpdateCursorPositionHelper(WindowInfo* win, Point pos, NotificationWnd* wnd) {
    static auto unit = MeasurementUnit::pt;
    // toggle measurement unit by repeatedly invoking the helper
    if (!wnd && win->notifications->GetForGroup(NG_CURSOR_POS_HELPER)) {
        switch (unit) {
            case MeasurementUnit::pt:
                unit = MeasurementUnit::mm;
                break;
            case MeasurementUnit::mm:
                unit = MeasurementUnit::in;
                break;
            case MeasurementUnit::in:
                unit = MeasurementUnit::pt;
                break;
            default:
                CrashAlwaysIf(true);
        }
        wnd = win->notifications->GetForGroup(NG_CURSOR_POS_HELPER);
    }

    CrashIf(!win->AsFixed());
    EngineBase* engine = win->AsFixed()->GetEngine();
    PointF pt = win->AsFixed()->CvtFromScreen(pos);
    AutoFreeWstr posStr(FormatCursorPosition(engine, pt, unit)), selStr;
    if (!win->selectionMeasure.IsEmpty()) {
        pt = PointF(win->selectionMeasure.dx, win->selectionMeasure.dy);
        selStr.Set(FormatCursorPosition(engine, pt, unit));
    }

    AutoFreeWstr posInfo(str::Format(L"%s %s", _TR("Cursor position:"), posStr.Get()));
    if (selStr) {
        posInfo.Set(str::Format(L"%s - %s %s", posInfo.Get(), _TR("Selection:"), selStr.Get()));
    }
    if (!wnd) {
        win->ShowNotification(posInfo, NotificationOptions::Persist, NG_CURSOR_POS_HELPER);
    } else {
        wnd->UpdateMessage(posInfo);
    }
}

void AssociateExeWithPdfExtension() {
    if (!HasPermission(Perm::RegistryAccess)) {
        return;
    }

    DoAssociateExeWithPdfExtension(HKEY_CURRENT_USER);
    DoAssociateExeWithPdfExtension(HKEY_LOCAL_MACHINE);

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSHNOWAIT, nullptr, nullptr);

    // Remind the user, when a different application takes over
    str::ReplaceWithCopy(&gGlobalPrefs->associatedExtensions, ".pdf");
    gGlobalPrefs->associateSilently = false;
}

// re-render the document currently displayed in this window
void WindowInfoRerender(WindowInfo* win, bool includeNonClientArea) {
    DisplayModel* dm = win->AsFixed();
    if (!dm) {
        return;
    }
    gRenderCache.CancelRendering(dm);
    gRenderCache.KeepForDisplayModel(dm, dm);
    if (includeNonClientArea) {
        win->RedrawAllIncludingNonClient(true);
    } else {
        win->RedrawAll(true);
    }
}

static void RerenderEverything() {
    for (auto* win : gWindows) {
        WindowInfoRerender(win);
    }
}

static void RerenderFixedPage() {
    for (auto* win : gWindows) {
        if (win->AsFixed()) {
            WindowInfoRerender(win, true);
        }
    }
}

void UpdateDocumentColors() {
    COLORREF text, bg;
    GetFixedPageUiColors(text, bg);

    if ((text == gRenderCache.textColor) && (bg == gRenderCache.backgroundColor)) {
        return; // colors didn't change
    }

    gRenderCache.textColor = text;
    gRenderCache.backgroundColor = bg;
    RerenderEverything();
}

void UpdateFixedPageScrollbarsVisibility() {
    bool hideScrollbars = gGlobalPrefs->fixedPageUI.hideScrollbars;
    bool scrollbarsVisible = false; // assume no scrollbars by default

    // iterate through each fixed page window to check whether scrollbars are shown
    for (auto* win : gWindows) {
        if (auto* pdfWin = win->AsFixed()) {
            scrollbarsVisible = pdfWin->IsVScrollbarVisible() || pdfWin->IsHScrollbarVisible();
            if (scrollbarsVisible) {
                break;
            }
        }
    }

    bool rerenderRequired = (hideScrollbars && scrollbarsVisible) || (!hideScrollbars && !scrollbarsVisible);
    if (!rerenderRequired) {
        return;
    }
    RerenderFixedPage();
}

static void OnMenuExit() {
    if (gPluginMode) {
        return;
    }

    for (WindowInfo* win : gWindows) {
        if (!CanCloseWindow(win)) {
            return;
        }
    }

    // we want to preserve the session state of all windows,
    // so we save it now
    // since we are closing the windows one by one,
    // CloseWindow() must not save the session state every time
    // (or we will end up with just the last window)
    prefs::Save();
    gDontSavePrefs = true;

    // CloseWindow removes the WindowInfo from gWindows,
    // so use a stable copy for iteration
    Vec<WindowInfo*> toClose = gWindows;
    for (WindowInfo* win : toClose) {
        CloseWindow(win, true, false);
    }
}

// closes a document inside a WindowInfo and optionally turns it into
// about window (set keepUIEnabled if a new document will be loaded
// into the tab right afterwards and LoadDocIntoCurrentTab would revert
// the UI disabling afterwards anyway)
static void CloseDocumentInCurrentTab(WindowInfo* win, bool keepUIEnabled, bool deleteModel) {
    bool wasntFixed = !win->AsFixed();
    if (win->AsChm()) {
        win->AsChm()->RemoveParentHwnd();
    }
    ClearTocBox(win);
    AbortFinding(win, true);

    win->linkOnLastButtonDown = nullptr;
    delete win->annotationOnLastButtonDown;
    win->annotationOnLastButtonDown = nullptr;

    win->fwdSearchMark.show = false;
    if (win->uiaProvider) {
        win->uiaProvider->OnDocumentUnload();
    }
    win->ctrl = nullptr;
    auto currentTab = win->currentTab;
    if (deleteModel) {
        delete currentTab->ctrl;
        currentTab->ctrl = nullptr;
        FileWatcherUnsubscribe(win->currentTab->watcher);
        win->currentTab->watcher = nullptr;
    } else {
        win->currentTab = nullptr;
    }
    win->notifications->RemoveForGroup(NG_RESPONSE_TO_ACTION);
    win->notifications->RemoveForGroup(NG_PAGE_INFO_HELPER);
    win->notifications->RemoveForGroup(NG_CURSOR_POS_HELPER);
    // TODO: this can cause a mouse capture to stick around when called from LoadModelIntoTab (cf. OnSelectionStop)
    win->mouseAction = MouseAction::Idle;

    DeletePropertiesWindow(win->hwndFrame);
    DeleteOldSelectionInfo(win, true);

    if (!keepUIEnabled) {
        SetSidebarVisibility(win, false, gGlobalPrefs->showFavorites);
        ToolbarUpdateStateForWindow(win, true);
        UpdateToolbarPageText(win, 0);
        UpdateToolbarFindText(win);
        UpdateFindbox(win);
        UpdateTabWidth(win);
        if (wasntFixed) {
            // restore the full menu and toolbar
            RebuildMenuBarForWindow(win);
            ShowOrHideToolbar(win);
        }
        ShowScrollBar(win->hwndCanvas, SB_BOTH, FALSE);
        win->RedrawAll();
        win::SetText(win->hwndFrame, SUMATRA_WINDOW_TITLE);
        CrashIf(win->tabs.size() != 0 || win->currentTab);
    }

    // Note: this causes https://code.google.com/p/sumatrapdf/issues/detail?id=2702. For whatever reason
    // edit ctrl doesn't receive WM_KILLFOCUS if we do SetFocus() here, even if we call SetFocus() later on
    // SetFocus(win->hwndFrame);
}

bool SaveAnnotationsToMaybeNewPdfFile(TabInfo* tab) {
    WCHAR dstFileName[MAX_PATH + 1] = {0};

    OPENFILENAME ofn = {0};
    str::WStr fileFilter(256);
    fileFilter.Append(_TR("PDF documents"));
    fileFilter.Append(L"\1*.pdf\1");
    fileFilter.Append(L"\1*.*\1");
    str::TransCharsInPlace(fileFilter.Get(), L"\1", L"\0");

    // TODO: automatically construct "foo.pdf" => "foo Copy.pdf"
    EngineBase* engine = tab->AsFixed()->GetEngine();
    const WCHAR* srcFileName = engine->FileName();
    str::BufSet(dstFileName, dimof(dstFileName), srcFileName);

    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = tab->win->hwndFrame;
    ofn.lpstrFile = dstFileName;
    ofn.nMaxFile = dimof(dstFileName);
    ofn.lpstrFilter = fileFilter.Get();
    ofn.nFilterIndex = 1;
    // ofn.lpstrTitle = _TR("Rename To");
    // ofn.lpstrInitialDir = initDir;
    ofn.lpstrDefExt = L".pdf";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;

    bool ok = GetSaveFileNameW(&ofn);
    if (!ok) {
        return false;
    }
    TempStr dstFilePath = ToUtf8Temp(dstFileName);
    ok = EngineMupdfSaveUpdated(engine, dstFilePath, [&tab, &dstFilePath](std::string_view mupdfErr) {
        str::Str msg;
        // TODO: duplicated string
        msg.AppendFmt(_TRA("Saving of '%s' failed with: '%s'"), dstFilePath.Get(), mupdfErr.data());
        tab->win->ShowNotification(msg.AsView(), NotificationOptions::Warning);
    });
    if (!ok) {
        return false;
    }

    auto win = tab->win;
    UpdateTabFileDisplayStateForTab(tab);
    CloseDocumentInCurrentTab(win, true, true);
    SetFocus(win->hwndFrame);

    AutoFreeWstr newPath(path::Normalize(dstFileName));
    // TODO: this should be 'duplicate FileInHistory"
    RenameFileInHistory(srcFileName, newPath);

    LoadArgs args(dstFileName, win);
    args.forceReuse = true;
    LoadDocument(args);

    str::Str msg;
    msg.AppendFmt(_TRA("Saved annotations to '%s'"), dstFilePath.Get());
    tab->win->ShowNotification(msg.AsView());
    return true;
}

enum class SaveChoice {
    Discard,
    SaveNew,
    SaveExisting,
    Cancel,
};

SaveChoice ShouldSaveAnnotationsDialog(HWND hwndParent, const WCHAR* filePath) {
    auto fileName = path::GetBaseNameTemp(filePath);
    auto mainInstr = str::Format(_TR("Unsaved annotations in '%s'"), fileName);
    auto content = _TR("Save annotations?");

    constexpr int kBtnIdDiscard = 100;
    constexpr int kBtnIdSaveToExisting = 101;
    constexpr int kBtnIdSaveToNew = 102;
    constexpr int kBtnIdCancel = 103;
    TASKDIALOGCONFIG dialogConfig{};
    TASKDIALOG_BUTTON buttons[4];

    buttons[0].nButtonID = kBtnIdSaveToExisting;
    buttons[0].pszButtonText = _TR("&Save to existing PDF");
    buttons[1].nButtonID = kBtnIdSaveToNew;
    buttons[1].pszButtonText = _TR("Save to &new PDF");
    buttons[2].nButtonID = kBtnIdDiscard;
    buttons[2].pszButtonText = _TR("&Discard changes");
    buttons[3].nButtonID = IDCANCEL;
    buttons[3].pszButtonText = _TR("&Cancel");

    DWORD flags =
        TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT | TDF_ENABLE_HYPERLINKS | TDF_POSITION_RELATIVE_TO_WINDOW;
    if (trans::IsCurrLangRtl()) {
        flags |= TDF_RTL_LAYOUT;
    }
    dialogConfig.cbSize = sizeof(TASKDIALOGCONFIG);
    dialogConfig.pszWindowTitle = _TR("Unsaved annotations");
    dialogConfig.pszMainInstruction = mainInstr;
    dialogConfig.pszContent = content;
    dialogConfig.nDefaultButton = IDCANCEL;
    dialogConfig.dwFlags = flags;
    dialogConfig.cxWidth = 0;
    dialogConfig.pfCallback = nullptr;
    dialogConfig.dwCommonButtons = 0;
    dialogConfig.cButtons = dimof(buttons);
    dialogConfig.pButtons = &buttons[0];
    dialogConfig.pszMainIcon = TD_INFORMATION_ICON;
    dialogConfig.hwndParent = hwndParent;

    int buttonPressedId{0};

    auto hr = TaskDialogIndirect(&dialogConfig, &buttonPressedId, nullptr, nullptr);
    CrashIf(hr == E_INVALIDARG);
    bool discard = (hr != S_OK) || (buttonPressedId == kBtnIdDiscard);
    if (discard) {
        return SaveChoice::Discard;
    }
    switch (buttonPressedId) {
        case kBtnIdSaveToExisting:
            return SaveChoice::SaveExisting;
        case kBtnIdSaveToNew:
            return SaveChoice::SaveNew;
        case kBtnIdDiscard:
            return SaveChoice::Discard;
        case IDCANCEL:
            return SaveChoice::Cancel;
    }
    ReportIf(true);
    return SaveChoice::Cancel;
}

// if returns true, can proceed with closing
// if returns false, should cancel closing
static bool MaybeSaveAnnotations(TabInfo* tab) {
    if (!tab) {
        return true;
    }
    // TODO: hacky because CloseCurrentTab() can call CloseWindow() and
    // they both ask to save annotations
    // Could determine in CloseCurrentTab() if will CloseWindow() and
    // not ask
    if (tab->askedToSaveAnnotations) {
        return true;
    }

    DisplayModel* dm = tab->AsFixed();
    if (!dm) {
        return true;
    }
    EngineBase* engine = dm->GetEngine();
    // shouldn't really happen but did happen.
    // don't block stress testing if opening a document flags it hasving unsaved annotations
    if (IsStressTesting()) {
        return true;
    }
    bool shouldConfirm = EngineHasUnsavedAnnotations(engine);
    if (!shouldConfirm) {
        return true;
    }
    tab->askedToSaveAnnotations = true;
    auto choice = ShouldSaveAnnotationsDialog(tab->win->hwndFrame, dm->GetFilePath());
    switch (choice) {
        case SaveChoice::Discard:
            return true;
        case SaveChoice::SaveNew:
            SaveAnnotationsToMaybeNewPdfFile(tab);
            break;
        case SaveChoice::SaveExisting: {
            TempStr path = ToUtf8Temp(engine->FileName());
            bool ok = EngineMupdfSaveUpdated(engine, {}, [&tab, &path](std::string_view mupdfErr) {
                str::Str msg;
                // TODO: duplicated message
                msg.AppendFmt(_TRA("Saving of '%s' failed with: '%s'"), path.Get(), mupdfErr.data());
                tab->win->ShowNotification(msg.AsView(), NotificationOptions::Warning);
            });
        } break;
        case SaveChoice::Cancel:
            tab->askedToSaveAnnotations = false;
            return false;
        default:
            CrashIf(true);
    }
    return true;
}

// closes the current tab, selecting the next one
// if there's only a single tab left, the window is closed if there
// are other windows, else the Frequently Read page is displayed
void CloseCurrentTab(WindowInfo* win, bool quitIfLast) {
    CrashIf(!win);
    if (!win) {
        return;
    }

    AbortFinding(win, true);
    ClearFindBox(win);

    // TODO: maybe should have a way to over-ride this for unconditional close?
    bool canClose = MaybeSaveAnnotations(win->currentTab);
    if (!canClose) {
        return;
    }

    bool didSavePrefs = false;
    size_t tabCount = win->tabs.size();
    if (tabCount == 1 || (tabCount == 0 && quitIfLast)) {
        if (CanCloseWindow(win)) {
            CloseWindow(win, quitIfLast, false);
            didSavePrefs = true; // in CloseWindow()
        }
    } else {
        CrashIf(gPluginMode && !gWindows.Contains(win));
        TabsOnCloseDoc(win);
    }
    if (!didSavePrefs) {
        prefs::Save();
    }
}

bool CanCloseWindow(WindowInfo* win) {
    CrashIf(!win);
    if (!win) {
        return false;
    }
    // a plugin window should only be closed when its parent is destroyed
    if (gPluginMode && !gWindows.Contains(win)) {
        return false;
    }

    if (win->printThread && !win->printCanceled && WaitForSingleObject(win->printThread, 0) == WAIT_TIMEOUT) {
        int res = MessageBox(win->hwndFrame, _TR("Printing is still in progress. Abort and quit?"),
                             _TR("Printing in progress."), MB_ICONEXCLAMATION | MB_YESNO | MbRtlReadingMaybe());
        if (IDNO == res) {
            return false;
        }
    }

    return true;
}

/* Close the document associated with window 'hwnd'.
   Closes the window unless this is the last window in which
   case it switches to empty window and disables the "File\Close"
   menu item. */
void CloseWindow(WindowInfo* win, bool quitIfLast, bool forceClose) {
    CrashIf(!win);
    if (!win) {
        return;
    }

    CrashIf(forceClose && !quitIfLast);
    if (forceClose) {
        quitIfLast = true;
    }

    // when used as an embedded plugin, closing should happen automatically
    // when the parent window is destroyed (cf. WM_DESTROY)
    if (gPluginMode && !gWindows.Contains(win) && !forceClose) {
        return;
    }

    AbortFinding(win, true);
    AbortPrinting(win);

    for (auto& tab : win->tabs) {
        if (tab->AsFixed()) {
            tab->AsFixed()->dontRenderFlag = true;
        }
    }

    if (win->presentation) {
        ExitFullScreen(win);
    }

    bool canCloseWindow = true;
    for (auto& tab : win->tabs) {
        bool canCloseTab = MaybeSaveAnnotations(tab);
        if (!canCloseTab) {
            canCloseWindow = false;
        }
    }

    // TODO: should be more intelligent i.e. close the tabs we can and only
    // leave those where user cancelled closing
    // would have to remember a list of tabs to not close above
    // if list not empty, only close the tabs not on the list
    if (!canCloseWindow) {
        return;
    }

    bool lastWindow = (1 == gWindows.size());
    // RememberDefaultWindowPosition becomes a no-op once the window is hidden
    RememberDefaultWindowPosition(win);
    // hide the window before saving prefs (closing seems slightly faster that way)
    if (!lastWindow || quitIfLast) {
        ShowWindow(win->hwndFrame, SW_HIDE);
    }
    if (!gDontSavePrefs) {
        // if we are exiting the application by File->Exit,
        // OnMenuExit will have called prefs::Save() already
        // and we skip the call here to avoid saving incomplete session info
        // (because some windows might have been closed already)
        prefs::Save();
    }
    TabsOnCloseWindow(win);

    if (forceClose) {
        // WM_DESTROY has already been sent, so don't destroy win->hwndFrame again
        DeleteWindowInfo(win);
    } else if (lastWindow && !quitIfLast) {
        /* last window - don't delete it */
        CloseDocumentInCurrentTab(win);
        SetFocus(win->hwndFrame);
        CrashIf(!gWindows.Contains(win));
    } else {
        FreeMenuOwnerDrawInfoData(win->menu);
        HWND hwndToDestroy = win->hwndFrame;
        DeleteWindowInfo(win);
        DestroyWindow(hwndToDestroy);
    }

    if (lastWindow && quitIfLast) {
        logf("Calling PostQuitMessage() in CloseWindow() because closing lastWindow\n");
        CrashIf(gWindows.size() != 0);
        PostQuitMessage(0);
    }
}

// returns false if no filter has been appended
static bool AppendFileFilterForDoc(Controller* ctrl, str::WStr& fileFilter) {
    // TODO: use ctrl->GetDefaultFileExt()
    Kind type = nullptr;
    if (ctrl->AsFixed()) {
        type = ctrl->AsFixed()->engineType;
    } else if (ctrl->AsChm()) {
        type = kindEngineChm;
    }

    auto ext = ctrl->GetDefaultFileExt();
    if (str::EqI(ext, L".xps")) {
        fileFilter.Append(_TR("XPS documents"));
    } else if (type == kindEngineDjVu) {
        fileFilter.Append(_TR("DjVu documents"));
    } else if (type == kindEngineComicBooks) {
        fileFilter.Append(_TR("Comic books"));
    } else if (type == kindEngineImage) {
        fileFilter.AppendFmt(_TR("Image files (*.%s)"), ctrl->GetDefaultFileExt() + 1);
    } else if (type == kindEngineImageDir) {
        return false; // only show "All files"
    } else if (type == kindEnginePostScript) {
        fileFilter.Append(_TR("Postscript documents"));
    } else if (type == kindEngineChm) {
        fileFilter.Append(_TR("CHM documents"));
    } else if (type == kindEngineEpub) {
        fileFilter.Append(_TR("EPUB ebooks"));
    } else if (type == kindEngineMobi) {
        fileFilter.Append(_TR("Mobi documents"));
    } else if (type == kindEngineFb2) {
        fileFilter.Append(_TR("FictionBook documents"));
    } else if (type == kindEnginePdb) {
        fileFilter.Append(_TR("PalmDoc documents"));
    } else if (type == kindEngineTxt) {
        fileFilter.Append(_TR("Text documents"));
    } else {
        fileFilter.Append(_TR("PDF documents"));
    }
    return true;
}

static void OnMenuSaveAs(WindowInfo* win) {
    if (!HasPermission(Perm::DiskAccess)) {
        return;
    }
    if (!win->IsDocLoaded()) {
        return;
    }

    auto* ctrl = win->ctrl;
    const WCHAR* srcFileName = ctrl->GetFilePath();
    AutoFreeWstr urlName;
    if (gPluginMode) {
        urlName.Set(url::GetFileName(gPluginURL));
        // fall back to a generic "filename" instead of the more confusing temporary filename
        srcFileName = urlName ? urlName.Get() : L"filename";
    }

    CrashIf(!srcFileName);
    if (!srcFileName) {
        return;
    }

    DisplayModel* dm = win->AsFixed();
    EngineBase* engine = dm ? dm->GetEngine() : nullptr;
    bool canConvertToTXT = engine && !engine->IsImageCollection() && win->currentTab->GetEngineType() != kindEngineTxt;
    bool canConvertToPDF = engine && win->currentTab->GetEngineType() != kindEngineMupdf;
#ifndef DEBUG
    // not ready for document types other than PS and image collections
    if (canConvertToPDF && win->currentTab->GetEngineType() != kindEnginePostScript && !engine->IsImageCollection()) {
        canConvertToPDF = false;
    }
#endif
#ifndef DISABLE_DOCUMENT_RESTRICTIONS
    // Can't save a document's content as plain text if text copying isn't allowed
    if (engine && !engine->AllowsCopyingText()) {
        canConvertToTXT = false;
    }
    // don't allow converting to PDF when printing isn't allowed
    if (engine && !engine->AllowsPrinting()) {
        canConvertToPDF = false;
    }
#endif
    CrashIf(canConvertToTXT &&
            (!engine || engine->IsImageCollection() || kindEngineTxt == win->currentTab->GetEngineType()));
    CrashIf(canConvertToPDF && (!engine || kindEngineMupdf == win->currentTab->GetEngineType()));

    const WCHAR* defExt = ctrl->GetDefaultFileExt();
    // Prepare the file filters (use \1 instead of \0 so that the
    // double-zero terminated string isn't cut by the string handling
    // methods too early on)
    str::WStr fileFilter(256);
    if (AppendFileFilterForDoc(ctrl, fileFilter)) {
        fileFilter.AppendFmt(L"\1*%s\1", defExt);
    }
    if (canConvertToTXT) {
        fileFilter.Append(_TR("Text documents"));
        fileFilter.Append(L"\1*.txt\1");
    }
    if (canConvertToPDF) {
        fileFilter.Append(_TR("PDF documents"));
        fileFilter.Append(L"\1*.pdf\1");
    }
    fileFilter.Append(_TR("All files"));
    fileFilter.Append(L"\1*.*\1");
    str::TransCharsInPlace(fileFilter.Get(), L"\1", L"\0");

    WCHAR dstFileName[MAX_PATH];
    str::BufSet(dstFileName, dimof(dstFileName), path::GetBaseNameTemp(srcFileName));
    if (str::FindChar(dstFileName, ':')) {
        // handle embed-marks (for embedded PDF documents):
        // remove the container document's extension and include
        // the embedding reference in the suggested filename
        WCHAR* colon = (WCHAR*)str::FindChar(dstFileName, ':');
        str::TransCharsInPlace(colon, L":", L"_");
        WCHAR* ext;
        for (ext = colon; ext > dstFileName && *ext != '.'; ext--) {
            // no-op
        }
        if (ext == dstFileName) {
            ext = colon;
        }
        memmove(ext, colon, (str::Len(colon) + 1) * sizeof(WCHAR));
    } else if (str::EndsWithI(dstFileName, defExt)) {
        // Remove the extension so that it can be re-added depending on the chosen filter
        dstFileName[str::Len(dstFileName) - str::Len(defExt)] = '\0';
    }

    OPENFILENAME ofn = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = win->hwndFrame;
    ofn.lpstrFile = dstFileName;
    ofn.nMaxFile = dimof(dstFileName);
    ofn.lpstrFilter = fileFilter.Get();
    ofn.nFilterIndex = 1;
    // defExt can be null, we want to skip '.'
    if (str::Len(defExt) > 0 && defExt[0] == L'.') {
        defExt++;
    }
    ofn.lpstrDefExt = defExt;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    // note: explicitly not setting lpstrInitialDir so that the OS
    // picks a reasonable default (in particular, we don't want this
    // in plugin mode, which is likely the main reason for saving as...)

    bool ok = GetSaveFileNameW(&ofn);
    if (!ok) {
        return;
    }

    WCHAR* realDstFileName = dstFileName;
    bool convertToTXT = canConvertToTXT && str::EndsWithI(dstFileName, L".txt");
    bool convertToPDF = canConvertToPDF && str::EndsWithI(dstFileName, L".pdf");

    // Make sure that the file has a valid ending
    if (!str::EndsWithI(dstFileName, defExt) && !convertToTXT && !convertToPDF) {
        if (canConvertToTXT && 2 == ofn.nFilterIndex) {
            defExt = L".txt";
            convertToTXT = true;
        } else if (canConvertToPDF && (canConvertToTXT ? 3 : 2) == (int)ofn.nFilterIndex) {
            defExt = L".pdf";
            convertToPDF = true;
        }
        realDstFileName = str::Format(L"%s%s", dstFileName, defExt);
    }

    auto pathA(ToUtf8Temp(realDstFileName));
    AutoFreeWstr errorMsg;
    // Extract all text when saving as a plain text file
    if (convertToTXT) {
        str::WStr text(1024);
        for (int pageNo = 1; pageNo <= ctrl->PageCount(); pageNo++) {
            PageText pageText = engine->ExtractPageText(pageNo);
            if (pageText.text != nullptr) {
                WCHAR* tmp = str::Replace(pageText.text, L"\n", L"\r\n");
                text.AppendAndFree(tmp);
            }
            FreePageText(&pageText);
        }

        auto textA = ToUtf8Temp(text.LendData());
        AutoFree textUTF8BOM = str::Join(UTF8_BOM, textA.Get());
        ok = file::WriteFile(realDstFileName, textUTF8BOM.AsSpan());
    } else if (convertToPDF) {
        // Convert the file into a PDF one
        AutoFreeWstr producerName = str::Join(GetAppNameTemp(), L" ", CURR_VERSION_STR);
        PdfCreator::SetProducerName(producerName);
        ok = engine->SaveFileAsPDF(pathA.Get());
        if (!ok && gIsDebugBuild) {
            // rendering includes all page annotations
            ok = PdfCreator::RenderToFile(pathA.Get(), engine);
        }
    } else if (!file::Exists(srcFileName) && engine) {
        // Recreate inexistant files from memory...
        ok = engine->SaveFileAs(pathA.Get());
    } else if (EngineSupportsAnnotations(engine)) {
        // ... as well as files containing annotations ...
        ok = engine->SaveFileAs(pathA.Get());
    } else if (!path::IsSame(srcFileName, realDstFileName)) {
        // ... else just copy the file
        WCHAR* msgBuf;
        ok = CopyFile(srcFileName, realDstFileName, FALSE);
        if (ok) {
            // Make sure that the copy isn't write-locked or hidden
            const DWORD attributesToDrop = FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM;
            DWORD attributes = GetFileAttributes(realDstFileName);
            if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & attributesToDrop)) {
                SetFileAttributes(realDstFileName, attributes & ~attributesToDrop);
            }
        } else if (FormatMessage(
                       FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       nullptr, GetLastError(), 0, (LPWSTR)&msgBuf, 0, nullptr)) {
            errorMsg.Set(str::Format(L"%s\n\n%s", _TR("Failed to save a file"), msgBuf));
            LocalFree(msgBuf);
        }
    }
    if (!ok) {
        const WCHAR* msg = _TR("Failed to save a file");
        if (errorMsg) {
            msg = errorMsg.Get();
        }
        MessageBoxWarning(win->hwndFrame, msg);
    }

    if (ok && IsUntrustedFile(win->ctrl->GetFilePath(), gPluginURL) && !convertToTXT) {
        auto realDstFileNameA = ToUtf8Temp(realDstFileName);
        file::SetZoneIdentifier(realDstFileNameA);
    }

    if (realDstFileName != dstFileName) {
        free(realDstFileName);
    }
}

static void OnMenuShowInFolder(WindowInfo* win) {
    if (!HasPermission(Perm::DiskAccess)) {
        return;
    }
    if (!win->IsDocLoaded()) {
        return;
    }
    if (gPluginMode) {
        return;
    }
    auto* ctrl = win->ctrl;
    auto srcFileName = ctrl->GetFilePath();
    if (!srcFileName) {
        return;
    }

    const WCHAR* process = L"explorer.exe";
    AutoFreeWstr args = str::Format(L"/select,\"%s\"", srcFileName);
    CreateProcessHelper(process, args);
}

static void OnMenuRenameFile(WindowInfo* win) {
    if (!HasPermission(Perm::DiskAccess)) {
        return;
    }
    if (!win->IsDocLoaded()) {
        return;
    }
    if (gPluginMode) {
        return;
    }

    auto* ctrl = win->ctrl;
    AutoFreeWstr srcFileName = str::Dup(ctrl->GetFilePath());
    // this happens e.g. for embedded documents and directories
    if (!file::Exists(srcFileName)) {
        return;
    }

    // Prepare the file filters (use \1 instead of \0 so that the
    // double-zero terminated string isn't cut by the string handling
    // methods too early on)
    const WCHAR* defExt = ctrl->GetDefaultFileExt();
    str::WStr fileFilter(256);
    bool ok = AppendFileFilterForDoc(ctrl, fileFilter);
    CrashIf(!ok);
    fileFilter.AppendFmt(L"\1*%s\1", defExt);
    str::TransCharsInPlace(fileFilter.Get(), L"\1", L"\0");

    WCHAR dstFileName[MAX_PATH];
    str::BufSet(dstFileName, dimof(dstFileName), path::GetBaseNameTemp(srcFileName));
    // Remove the extension so that it can be re-added depending on the chosen filter
    if (str::EndsWithI(dstFileName, defExt)) {
        dstFileName[str::Len(dstFileName) - str::Len(defExt)] = '\0';
    }

    AutoFreeWstr initDir(path::GetDir(srcFileName));

    OPENFILENAME ofn = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = win->hwndFrame;
    ofn.lpstrFile = dstFileName;
    ofn.nMaxFile = dimof(dstFileName);
    ofn.lpstrFilter = fileFilter.Get();
    ofn.nFilterIndex = 1;
    // note: the other two dialogs are named "Open" and "Save As"
    ofn.lpstrTitle = _TR("Rename To");
    ofn.lpstrInitialDir = initDir;
    ofn.lpstrDefExt = defExt + 1;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;

    ok = GetSaveFileNameW(&ofn);
    if (!ok) {
        return;
    }

    UpdateTabFileDisplayStateForTab(win->currentTab);
    CloseDocumentInCurrentTab(win, true, true);
    SetFocus(win->hwndFrame);

    DWORD flags = MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING;
    BOOL moveOk = MoveFileEx(srcFileName.Get(), dstFileName, flags);
    if (!moveOk) {
        LogLastError();
        LoadArgs args(srcFileName, win);
        args.forceReuse = true;
        LoadDocument(args);
        win->ShowNotification(_TR("Failed to rename the file!"), NotificationOptions::Warning);
        return;
    }

    AutoFreeWstr newPath(path::Normalize(dstFileName));
    RenameFileInHistory(srcFileName, newPath);

    LoadArgs args(dstFileName, win);
    args.forceReuse = true;
    LoadDocument(args);
}

static void OnMenuSaveBookmark(WindowInfo* win) {
    if (!HasPermission(Perm::DiskAccess) || gPluginMode) {
        return;
    }
    if (!win->IsDocLoaded()) {
        return;
    }

    auto* ctrl = win->ctrl;
    const WCHAR* defExt = ctrl->GetDefaultFileExt();

    WCHAR dstFileName[MAX_PATH];
    // Remove the extension so that it can be replaced with .lnk
    str::BufSet(dstFileName, dimof(dstFileName), path::GetBaseNameTemp(ctrl->GetFilePath()));
    str::TransCharsInPlace(dstFileName, L":", L"_");
    if (str::EndsWithI(dstFileName, defExt)) {
        dstFileName[str::Len(dstFileName) - str::Len(defExt)] = '\0';
    }

    // Prepare the file filters (use \1 instead of \0 so that the
    // double-zero terminated string isn't cut by the string handling
    // methods too early on)
    AutoFreeWstr fileFilter = str::Format(L"%s\1*.lnk\1", _TR("Bookmark Shortcuts"));
    str::TransCharsInPlace(fileFilter, L"\1", L"\0");

    OPENFILENAME ofn = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = win->hwndFrame;
    ofn.lpstrFile = dstFileName;
    ofn.nMaxFile = dimof(dstFileName);
    ofn.lpstrFilter = fileFilter;
    ofn.nFilterIndex = 1;
    ofn.lpstrDefExt = L"lnk";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;

    if (!GetSaveFileNameW(&ofn)) {
        return;
    }

    AutoFreeWstr fileName(str::Dup(dstFileName));
    if (!str::EndsWithI(dstFileName, L".lnk")) {
        fileName.Set(str::Join(dstFileName, L".lnk"));
    }

    ScrollState ss(win->ctrl->CurrentPageNo(), 0, 0);
    if (win->AsFixed()) {
        ss = win->AsFixed()->GetScrollState();
    }
    const char* viewModeStr = DisplayModeToString(ctrl->GetDisplayMode());
    AutoFreeWstr ZoomVirtual(str::Format(L"%.2f", ctrl->GetZoomVirtual()));
    if (ZOOM_FIT_PAGE == ctrl->GetZoomVirtual()) {
        ZoomVirtual.SetCopy(L"fitpage");
    } else if (ZOOM_FIT_WIDTH == ctrl->GetZoomVirtual()) {
        ZoomVirtual.SetCopy(L"fitwidth");
    } else if (ZOOM_FIT_CONTENT == ctrl->GetZoomVirtual()) {
        ZoomVirtual.SetCopy(L"fitcontent");
    }

    auto viewMode = ToWstrTemp(viewModeStr);
    AutoFreeWstr args = str::Format(L"\"%s\" -page %d -view \"%s\" -zoom %s -scroll %d,%d", ctrl->GetFilePath(),
                                    ss.page, viewMode.Get(), ZoomVirtual.Get(), (int)ss.x, (int)ss.y);
    AutoFreeWstr label = ctrl->GetPageLabel(ss.page);
    const WCHAR* srcFileName = path::GetBaseNameTemp(ctrl->GetFilePath());
    AutoFreeWstr desc = str::Format(_TR("Bookmark shortcut to page %s of %s"), label.Get(), srcFileName);
    auto exePath = GetExePathTemp();
    CreateShortcut(fileName, exePath, args, desc, 1);
}

#if 0
// code adapted from https://support.microsoft.com/kb/131462/en-us
static UINT_PTR CALLBACK FileOpenHook(HWND hDlg, UINT uiMsg, WPARAM wp, LPARAM lp)
{
    switch (uiMsg) {
    case WM_INITDIALOG:
        SetWindowLongPtr(hDlg, GWLP_USERDATA, (LONG_PTR)lp);
        break;
    case WM_NOTIFY:
        if (((LPOFNOTIFY)lp)->hdr.code == CDN_SELCHANGE) {
            LPOPENFILENAME lpofn = (LPOPENFILENAME)GetWindowLongPtr(hDlg, GWLP_USERDATA);
            // make sure that the filename buffer is large enough to hold
            // all the selected filenames
            int cbLength = CommDlg_OpenSave_GetSpec(GetParent(hDlg), nullptr, 0) + MAX_PATH;
            if (cbLength >= 0 && lpofn->nMaxFile < (DWORD)cbLength) {
                WCHAR *oldBuffer = lpofn->lpstrFile;
                lpofn->lpstrFile = (LPWSTR)realloc(lpofn->lpstrFile, cbLength * sizeof(WCHAR));
                if (lpofn->lpstrFile)
                    lpofn->nMaxFile = cbLength;
                else
                    lpofn->lpstrFile = oldBuffer;
            }
        }
        break;
    }

    return 0;
}
#endif

static void OnMenuNewWindow() {
    CreateAndShowWindowInfo(nullptr);
}

// create a new window and load currently shown document into it
// meant to make it easy to compare 2 documents
static void OnDuplicateInNewWindow(WindowInfo* win) {
    if (win->IsAboutWindow()) {
        return;
    }
    if (!win->IsDocLoaded()) {
        return;
    }
    TabInfo* tab = win->currentTab;
    WCHAR* path = tab->filePath;
    CrashIf(!path);
    if (!path) {
        return;
    }
    WindowInfo* newWin = CreateAndShowWindowInfo(nullptr);
    if (!newWin) {
        return;
    }

    // TODO: should copy the display state from current file
    LoadArgs args(path, newWin);
    args.fileName = tab->filePath;
    args.showWin = true;
    args.noPlaceWindow = true;
    LoadDocument(args);
}

// TODO: similar to Installer.cpp
static bool BrowseForFolder(HWND hwnd, const WCHAR* initialFolder, const WCHAR* caption, WCHAR* buf, DWORD cchBuf) {
    if (buf == nullptr || cchBuf < MAX_PATH) {
        return false;
    }

    BROWSEINFO bi = {nullptr};
    bi.hwndOwner = hwnd;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    bi.lpszTitle = caption;
    // bi.lpfn = BrowseCallbackProc;
    bi.lParam = (LPARAM)initialFolder;

    LPITEMIDLIST pidlFolder = SHBrowseForFolder(&bi);
    if (!pidlFolder) {
        return false;
    }
    BOOL ok = SHGetPathFromIDListW(pidlFolder, buf);
    if (!ok) {
        return false;
    }
    IMalloc* pMalloc = nullptr;
    HRESULT hr = SHGetMalloc(&pMalloc);
    if (SUCCEEDED(hr) && pMalloc) {
        pMalloc->Free(pidlFolder);
        pMalloc->Release();
    }
    return true;
}

static void OnMenuOpenFolder(WindowInfo* win) {
    HWND hwnd = win->hwndFrame;
    WCHAR dirW[MAX_PATH + 2] = {0};
    bool ok = BrowseForFolder(hwnd, nullptr, L"Select folder with PDF files", dirW, dimof(dirW));
    if (!ok) {
        return;
    }

    EngineBase* engine = CreateEngineMultiFromDirectory(dirW);
    if (!engine) {
        return;
    }
    LoadArgs args(dirW, win);
    args.engine = engine;
    LoadDocument(args);
}

static void OnMenuOpen(WindowInfo* win) {
    if (!HasPermission(Perm::DiskAccess)) {
        return;
    }

    // don't allow opening different files in plugin mode
    if (gPluginMode) {
        return;
    }

    const struct {
        const WCHAR* name; /* nullptr if only to include in "All supported documents" */
        const WCHAR* filter;
        bool available;
    } fileFormats[] = {
        {_TR("PDF documents"), L"*.pdf", true},
        {_TR("XPS documents"), L"*.xps;*.oxps", true},
        {_TR("DjVu documents"), L"*.djvu", true},
        {_TR("Postscript documents"), L"*.ps;*.eps", IsEnginePsAvailable()},
        {_TR("Comic books"), L"*.cbz;*.cbr;*.cb7;*.cbt", true},
        {_TR("CHM documents"), L"*.chm", true},
        {_TR("SVG documents"), L"*.svg", true},
        {_TR("EPUB ebooks"), L"*.epub", true},
        {_TR("Mobi documents"), L"*.mobi", true},
        {_TR("FictionBook documents"), L"*.fb2;*.fb2z;*.zfb2;*.fb2.zip", true},
        {_TR("PalmDoc documents"), L"*.pdb;*.prc", true},
        {_TR("Images"), L"*.bmp;*.dib;*.gif;*.jpg;*.jpeg;*.jxr;*.png;*.tga;*.tif;*.tiff;*.webp", true},
        {_TR("Text documents"), L"*.txt;*.log;*.nfo;file_id.diz;read.me;*.tcr", true},
    };
    // Prepare the file filters (use \1 instead of \0 so that the
    // double-zero terminated string isn't cut by the string handling
    // methods too early on)
    str::WStr fileFilter;
    fileFilter.Append(_TR("All supported documents"));
    fileFilter.Append(L'\1');
    for (int i = 0; i < dimof(fileFormats); i++) {
        if (fileFormats[i].available) {
            fileFilter.Append(fileFormats[i].filter);
            fileFilter.Append(';');
        }
    }
    CrashIf(fileFilter.Last() != L';');
    fileFilter.Last() = L'\1';
    for (int i = 0; i < dimof(fileFormats); i++) {
        if (fileFormats[i].available && fileFormats[i].name) {
            fileFilter.Append(fileFormats[i].name);
            fileFilter.Append(L'\1');
            fileFilter.Append(fileFormats[i].filter);
            fileFilter.Append(L'\1');
        }
    }
    fileFilter.Append(_TR("All files"));
    fileFilter.Append(L"\1*.*\1");
    str::TransCharsInPlace(fileFilter.Get(), L"\1", L"\0");

    OPENFILENAME ofn = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = win->hwndFrame;

    ofn.lpstrFilter = fileFilter.Get();
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_ALLOWMULTISELECT | OFN_EXPLORER;

    // OFN_ENABLEHOOK disables the new Open File dialog under Windows Vista
    // and later, so don't use it and just allocate enough memory to contain
    // several dozen file paths and hope that this is enough
    // TODO: Use IFileOpenDialog instead (requires a Vista SDK, though)
    ofn.nMaxFile = MAX_PATH * 100;
#if 0
    if (!IsWindowsVistaOrGreater())
    {
        ofn.lpfnHook = FileOpenHook;
        ofn.Flags |= OFN_ENABLEHOOK;
        ofn.nMaxFile = MAX_PATH / 2;
    }
    // note: ofn.lpstrFile can be reallocated by GetOpenFileName -> FileOpenHook
#endif
    AutoFreeWstr file = AllocArray<WCHAR>(ofn.nMaxFile);
    ofn.lpstrFile = file;

    if (!GetOpenFileNameW(&ofn)) {
        return;
    }

    WCHAR* fileName = ofn.lpstrFile + ofn.nFileOffset;
    if (*(fileName - 1)) {
        // special case: single filename without nullptr separator
        LoadArgs args(ofn.lpstrFile, win);
        LoadDocument(args);
        return;
    }

    while (*fileName) {
        AutoFreeWstr filePath = path::Join(ofn.lpstrFile, fileName);
        if (filePath) {
            LoadArgs args(filePath, win);
            LoadDocument(args);
        }
        fileName += str::Len(fileName) + 1;
    }
}

static void BrowseFolder(WindowInfo* win, bool forward) {
    CrashIf(win->IsAboutWindow());
    if (win->IsAboutWindow()) {
        return;
    }
    if (!HasPermission(Perm::DiskAccess) || gPluginMode) {
        return;
    }

    TabInfo* tab = win->currentTab;
    WStrVec files;
    AutoFreeWstr pattern(path::GetDir(tab->filePath));
    // TODO: make pattern configurable (for users who e.g. want to skip single images)?
    pattern.Set(path::Join(pattern, L"*"));
    if (!CollectPathsFromDirectory(pattern, files)) {
        return;
    }

    // remove unsupported files that have never been successfully loaded
    for (size_t i = files.size(); i > 0; i--) {
        WCHAR* path = files.at(i - 1);
        Kind kind = GuessFileTypeFromName(path);
        char* pathA = ToUtf8Temp(path);
        if (!IsSupportedFileType(kind, true) && !DocIsSupportedFileType(kind) && !gFileHistory.Find(pathA, nullptr)) {
            free(files.PopAt(i - 1));
        }
    }

    if (!files.Contains(tab->filePath)) {
        files.Append(str::Dup(tab->filePath));
    }
    files.SortNatural();

    int index = files.Find(tab->filePath);
    if (forward) {
        index = (index + 1) % (int)files.size();
    } else {
        index = (int)(index + files.size() - 1) % files.size();
    }

    // TODO: check for unsaved modifications
    UpdateTabFileDisplayStateForTab(tab);
    LoadArgs args(files.at(index), win);
    args.forceReuse = true;
    LoadDocument(args);
}

static void RelayoutFrame(WindowInfo* win, bool updateToolbars = true, int sidebarDx = -1) {
    Rect rc = ClientRect(win->hwndFrame);
    // don't relayout while the window is minimized
    if (rc.IsEmpty()) {
        return;
    }

    if (PM_BLACK_SCREEN == win->presentation || PM_WHITE_SCREEN == win->presentation) {
        // make the black/white canvas cover the entire window
        MoveWindow(win->hwndCanvas, rc);
        return;
    }

    DeferWinPosHelper dh;

    // Tabbar and toolbar at the top
    if (!win->presentation && !win->isFullScreen) {
        if (win->tabsInTitlebar) {
            if (dwm::IsCompositionEnabled()) {
                int frameThickness = GetSystemMetrics(SM_CYFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
                rc.y += frameThickness;
                rc.dy -= frameThickness;
            }
            float scale = IsZoomed(win->hwndFrame) ? 1.f : CAPTION_TABBAR_HEIGHT_FACTOR;
            int captionHeight = GetTabbarHeight(win->hwndFrame, scale);
            if (updateToolbars) {
                int captionWidth;
                RECT capButtons;
                if (dwm::IsCompositionEnabled() &&
                    SUCCEEDED(dwm::GetWindowAttribute(win->hwndFrame, DWMWA_CAPTION_BUTTON_BOUNDS, &capButtons,
                                                      sizeof(RECT)))) {
                    Rect wr = WindowRect(win->hwndFrame);
                    POINT pt = {wr.x + capButtons.left, wr.y + capButtons.top};
                    ScreenToClient(win->hwndFrame, &pt);
                    if (IsUIRightToLeft()) {
                        captionWidth = rc.x + rc.dx - pt.x;
                    } else {
                        captionWidth = pt.x - rc.x;
                    }
                } else {
                    captionWidth = rc.dx;
                }
                dh.SetWindowPos(win->hwndCaption, nullptr, rc.x, rc.y, captionWidth, captionHeight, SWP_NOZORDER);
            }
            rc.y += captionHeight;
            rc.dy -= captionHeight;
        } else if (win->tabsVisible) {
            int tabHeight = GetTabbarHeight(win->hwndFrame);
            if (updateToolbars) {
                dh.SetWindowPos(win->tabsCtrl->hwnd, nullptr, rc.x, rc.y, rc.dx, tabHeight, SWP_NOZORDER);
            }
            // TODO: show tab bar also for About window (or hide the toolbar so that it doesn't jump around)
            if (!win->IsAboutWindow()) {
                rc.y += tabHeight;
                rc.dy -= tabHeight;
            }
        }
    }
    if (gGlobalPrefs->showToolbar && !win->presentation && !win->isFullScreen) {
        if (updateToolbars) {
            Rect rcRebar = WindowRect(win->hwndReBar);
            dh.SetWindowPos(win->hwndReBar, nullptr, rc.x, rc.y, rc.dx, rcRebar.dy, SWP_NOZORDER);
        }
        Rect rcRebar = WindowRect(win->hwndReBar);
        rc.y += rcRebar.dy;
        rc.dy -= rcRebar.dy;
    }

    // ToC and Favorites sidebars at the left
    bool showFavorites = gGlobalPrefs->showFavorites && !gPluginMode && HasPermission(Perm::DiskAccess);
    bool tocVisible = win->tocVisible;
    if (tocVisible || showFavorites) {
        Size toc = ClientRect(win->hwndTocBox).Size();
        if (sidebarDx > 0) {
            toc = Size(sidebarDx, rc.y);
        }
        if (0 == toc.dx) {
            // TODO: use saved sidebarDx from saved preferences?
            toc.dx = rc.dx / 4;
        }
        // make sure that the sidebar is never too wide or too narrow
        // note: requires that the main frame is at least 2 * SIDEBAR_MIN_WIDTH
        //       wide (cf. OnFrameGetMinMaxInfo)
        toc.dx = limitValue(toc.dx, SIDEBAR_MIN_WIDTH, rc.dx / 2);

        toc.dy = 0;
        if (tocVisible) {
            if (!showFavorites) {
                toc.dy = rc.dy;
            } else {
                toc.dy = gGlobalPrefs->tocDy;
                if (toc.dy > 0) {
                    toc.dy = limitValue<int>(gGlobalPrefs->tocDy, 0, rc.dy);
                } else {
                    toc.dy = rc.dy / 2; // default value
                }
            }
        }

        if (tocVisible && showFavorites) {
            toc.dy = limitValue(toc.dy, TOC_MIN_DY, rc.dy - TOC_MIN_DY);
        }

        if (tocVisible) {
            Rect rToc(rc.TL(), toc);
            dh.MoveWindow(win->hwndTocBox, rToc);
            if (showFavorites) {
                Rect rSplitV(rc.x, rc.y + toc.dy, toc.dx, SPLITTER_DY);
                dh.MoveWindow(win->favSplitter->hwnd, rSplitV);
                toc.dy += SPLITTER_DY;
            }
        }
        if (showFavorites) {
            Rect rFav(rc.x, rc.y + toc.dy, toc.dx, rc.dy - toc.dy);
            dh.MoveWindow(win->hwndFavBox, rFav);
        }
        Rect rSplitH(rc.x + toc.dx, rc.y, SPLITTER_DX, rc.dy);
        dh.MoveWindow(win->sidebarSplitter->hwnd, rSplitH);

        rc.x += toc.dx + SPLITTER_DX;
        rc.dx -= toc.dx + SPLITTER_DX;
    }

    dh.MoveWindow(win->hwndCanvas, rc);

    dh.End();

    // TODO: if a document with ToC and a broken document are loaded
    //       and the first document is closed with the ToC still visible,
    //       we have tocVisible but !win->ctrl
    if (tocVisible && win->ctrl) {
        // the ToC selection may change due to resizing
        // (and SetSidebarVisibility relies on this for initialization)
        UpdateTocSelection(win, win->ctrl->CurrentPageNo());
    }
}

static void FrameOnSize(WindowInfo* win, __unused int dx, __unused int dy) {
    RelayoutFrame(win);

    if (win->presentation || win->isFullScreen) {
        Rect fullscreen = GetFullscreenRect(win->hwndFrame);
        Rect rect = WindowRect(win->hwndFrame);
        // Windows XP sometimes seems to change the window size on it's own
        if (rect != fullscreen && rect != GetVirtualScreenRect()) {
            MoveWindow(win->hwndFrame, fullscreen);
        }
    }
}

void RelayoutWindow(WindowInfo* win) {
    RelayoutFrame(win);
}

void SetCurrentLanguageAndRefreshUI(const char* langCode) {
    if (!langCode || str::Eq(langCode, trans::GetCurrentLangCode())) {
        return;
    }
    SetCurrentLang(langCode);

    for (WindowInfo* win : gWindows) {
        UpdateWindowRtlLayout(win);
        RebuildMenuBarForWindow(win);
        UpdateToolbarSidebarText(win);
        // About page text is translated during (re)drawing
        if (win->IsAboutWindow()) {
            win->RedrawAll(true);
        }
    }

    prefs::Save();
}

static void OnMenuChangeLanguage(HWND hwnd) {
    const char* newLangCode = Dialog_ChangeLanguge(hwnd, trans::GetCurrentLangCode());
    SetCurrentLanguageAndRefreshUI(newLangCode);
}

static void OnMenuViewShowHideToolbar() {
    gGlobalPrefs->showToolbar = !gGlobalPrefs->showToolbar;
    for (WindowInfo* win : gWindows) {
        ShowOrHideToolbar(win);
    }
}

static void OnMenuViewShowHideScrollbars() {
    gGlobalPrefs->fixedPageUI.hideScrollbars = !gGlobalPrefs->fixedPageUI.hideScrollbars;
    UpdateFixedPageScrollbarsVisibility();
}

static void OnMenuAdvancedOptions() {
    if (!HasPermission(Perm::DiskAccess) || !HasPermission(Perm::SavePreferences)) {
        return;
    }

    AutoFreeWstr path = prefs::GetSettingsPath();
    // TODO: disable/hide the menu item when there's no prefs file
    //       (happens e.g. when run in portable mode from a CD)?
    LaunchFile(path.Get(), nullptr, L"open");
}

static void OnMenuOptions(HWND hwnd) {
    if (!HasPermission(Perm::SavePreferences)) {
        return;
    }

    if (IDOK != Dialog_Settings(hwnd, gGlobalPrefs)) {
        return;
    }

    if (!gGlobalPrefs->rememberOpenedFiles) {
        gFileHistory.Clear(true);
        CleanUpThumbnailCache(gFileHistory);
    }
    UpdateDocumentColors();

    // note: ideally we would also update state for useTabs changes but that's complicated since
    // to do it right we would have to convert tabs to windows. When moving no tabs -> tabs,
    // there's no problem. When moving tabs -> no tabs, a half solution would be to only
    // call SetTabsInTitlebar() for windows that have only one tab, but that's somewhat inconsistent
    prefs::Save();
}

static void OnMenuOptions(WindowInfo* win) {
    OnMenuOptions(win->hwndFrame);
    if (!gWindows.empty() && gWindows.at(0)->IsAboutWindow()) {
        gWindows.at(0)->RedrawAll(true);
    }
}

// toggles 'show pages continuously' state
static void OnMenuViewContinuous(WindowInfo* win) {
    if (!win->IsDocLoaded()) {
        return;
    }

    DisplayMode newMode = win->ctrl->GetDisplayMode();
    switch (newMode) {
        case DisplayMode::SinglePage:
        case DisplayMode::Continuous:
            newMode = IsContinuous(newMode) ? DisplayMode::SinglePage : DisplayMode::Continuous;
            break;
        case DisplayMode::Facing:
        case DisplayMode::ContinuousFacing:
            newMode = IsContinuous(newMode) ? DisplayMode::Facing : DisplayMode::ContinuousFacing;
            break;
        case DisplayMode::BookView:
        case DisplayMode::ContinuousBookView:
            newMode = IsContinuous(newMode) ? DisplayMode::BookView : DisplayMode::ContinuousBookView;
            break;
    }
    SwitchToDisplayMode(win, newMode);
}

static void OnMenuViewMangaMode(WindowInfo* win) {
    CrashIf(!win->currentTab || win->currentTab->GetEngineType() != kindEngineComicBooks);
    if (!win->currentTab || win->currentTab->GetEngineType() != kindEngineComicBooks) {
        return;
    }
    DisplayModel* dm = win->AsFixed();
    dm->SetDisplayR2L(!dm->GetDisplayR2L());
    ScrollState state = dm->GetScrollState();
    dm->Relayout(dm->GetZoomVirtual(), dm->GetRotation());
    dm->SetScrollState(state);
}

/* Zoom document in window 'hwnd' to zoom level 'zoom'.
   'zoom' is given as a floating-point number, 1.0 is 100%, 2.0 is 200% etc.
*/
static void OnMenuZoom(WindowInfo* win, int menuId) {
    if (!win->IsDocLoaded()) {
        return;
    }

    float zoom = ZoomMenuItemToZoom(menuId);
    ZoomToSelection(win, zoom);
}

static void ChangeZoomLevel(WindowInfo* win, float newZoom, bool pagesContinuously) {
    if (!win->IsDocLoaded()) {
        return;
    }

    float zoom = win->ctrl->GetZoomVirtual();
    DisplayMode mode = win->ctrl->GetDisplayMode();
    DisplayMode newMode = pagesContinuously ? DisplayMode::Continuous : DisplayMode::SinglePage;

    if (mode != newMode || zoom != newZoom) {
        float prevZoom = win->currentTab->prevZoomVirtual;
        DisplayMode prevMode = win->currentTab->prevDisplayMode;

        if (mode != newMode) {
            SwitchToDisplayMode(win, newMode);
        }
        OnMenuZoom(win, MenuIdFromVirtualZoom(newZoom));

        // remember the previous values for when the toolbar button is unchecked
        if (INVALID_ZOOM == prevZoom) {
            win->currentTab->prevZoomVirtual = zoom;
            win->currentTab->prevDisplayMode = mode;
        } else {
            // keep the rememberd values when toggling between the two toolbar buttons
            win->currentTab->prevZoomVirtual = prevZoom;
            win->currentTab->prevDisplayMode = prevMode;
        }
    } else if (win->currentTab->prevZoomVirtual != INVALID_ZOOM) {
        float prevZoom = win->currentTab->prevZoomVirtual;
        SwitchToDisplayMode(win, win->currentTab->prevDisplayMode);
        ZoomToSelection(win, prevZoom);
    }
}

static void FocusPageNoEdit(HWND hwndPageBox) {
    if (IsFocused(hwndPageBox)) {
        SendMessageW(hwndPageBox, WM_SETFOCUS, 0, 0);
    } else {
        SetFocus(hwndPageBox);
    }
}

static void OnMenuGoToPage(WindowInfo* win) {
    if (!win->IsDocLoaded()) {
        return;
    }

    // Don't show a dialog if we don't have to - use the Toolbar instead
    if (gGlobalPrefs->showToolbar && !win->isFullScreen && !win->presentation) {
        FocusPageNoEdit(win->hwndPageBox);
        return;
    }

    auto* ctrl = win->ctrl;
    AutoFreeWstr label = ctrl->GetPageLabel(ctrl->CurrentPageNo());
    AutoFreeWstr newPageLabel(Dialog_GoToPage(win->hwndFrame, label, ctrl->PageCount(), !ctrl->HasPageLabels()));
    if (!newPageLabel) {
        return;
    }

    int newPageNo = ctrl->GetPageByLabel(newPageLabel);
    if (ctrl->ValidPageNo(newPageNo)) {
        ctrl->GoToPage(newPageNo, true);
    }
}

void EnterFullScreen(WindowInfo* win, bool presentation) {
    if (!HasPermission(Perm::FullscreenAccess) || gPluginMode) {
        return;
    }

    if (!IsWindowVisible(win->hwndFrame)) {
        return;
    }

    if (presentation ? win->presentation : win->isFullScreen) {
        return;
    }

    CrashIf(presentation ? win->isFullScreen : win->presentation);
    if (presentation) {
        CrashIf(!win->ctrl);
        if (!win->IsDocLoaded()) {
            return;
        }

        if (IsZoomed(win->hwndFrame)) {
            win->windowStateBeforePresentation = WIN_STATE_MAXIMIZED;
        } else {
            win->windowStateBeforePresentation = WIN_STATE_NORMAL;
        }
        win->presentation = PM_ENABLED;
        // hack: this tells OnMouseMove() to hide cursor immediately
        win->dragPrevPos = Point(-2, -3);
    } else {
        win->isFullScreen = true;
    }

    // ToC and Favorites sidebars are hidden when entering presentation mode
    // TODO: make showFavorites a per-window pref
    bool showFavoritesTmp = gGlobalPrefs->showFavorites;
    if (presentation && (win->tocVisible || gGlobalPrefs->showFavorites)) {
        SetSidebarVisibility(win, false, false);
    }

    long ws = GetWindowLong(win->hwndFrame, GWL_STYLE);
    if (!presentation || !win->isFullScreen) {
        win->nonFullScreenWindowStyle = ws;
    }
    // remove window styles that add to non-client area
    ws &= ~(WS_CAPTION | WS_THICKFRAME);
    ws |= WS_MAXIMIZE;

    win->nonFullScreenFrameRect = WindowRect(win->hwndFrame);
    Rect rect = GetFullscreenRect(win->hwndFrame);

    SetMenu(win->hwndFrame, nullptr);
    ShowWindow(win->hwndReBar, SW_HIDE);
    win->tabsCtrl->SetIsVisible(false);
    ShowWindow(win->hwndCaption, SW_HIDE);

    SetWindowLong(win->hwndFrame, GWL_STYLE, ws);
    uint flags = SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_NOZORDER;
    SetWindowPos(win->hwndFrame, nullptr, rect.x, rect.y, rect.dx, rect.dy, flags);

    if (presentation) {
        win->ctrl->SetPresentationMode(true);
    }

    // Make sure that no toolbar/sidebar keeps the focus
    SetFocus(win->hwndFrame);
    // restore gGlobalPrefs->showFavorites changed by SetSidebarVisibility()
    gGlobalPrefs->showFavorites = showFavoritesTmp;
}

void ExitFullScreen(WindowInfo* win) {
    if (!win->isFullScreen && !win->presentation) {
        return;
    }

    bool wasPresentation = PM_DISABLED != win->presentation;
    if (wasPresentation) {
        win->presentation = PM_DISABLED;
        if (win->IsDocLoaded()) {
            win->ctrl->SetPresentationMode(false);
        }
        // re-enable the auto-hidden cursor
        KillTimer(win->hwndCanvas, kHideCursorTimerID);
        SetCursorCached(IDC_ARROW);
        // ensure that no ToC is shown when entering presentation mode the next time
        for (TabInfo* tab : win->tabs) {
            tab->showTocPresentation = false;
        }
    } else {
        win->isFullScreen = false;
    }

    bool tocVisible = win->currentTab && win->currentTab->showToc;
    SetSidebarVisibility(win, tocVisible, gGlobalPrefs->showFavorites);

    if (win->tabsInTitlebar) {
        ShowWindow(win->hwndCaption, SW_SHOW);
    }
    if (win->tabsVisible) {
        win->tabsCtrl->SetIsVisible(true);
    }
    if (gGlobalPrefs->showToolbar) {
        ShowWindow(win->hwndReBar, SW_SHOW);
    }
    if (!win->isMenuHidden) {
        SetMenu(win->hwndFrame, win->menu);
    }

    Rect cr = ClientRect(win->hwndFrame);
    SetWindowLong(win->hwndFrame, GWL_STYLE, win->nonFullScreenWindowStyle);
    uint flags = SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOSIZE | SWP_NOMOVE;
    SetWindowPos(win->hwndFrame, nullptr, 0, 0, 0, 0, flags);
    MoveWindow(win->hwndFrame, win->nonFullScreenFrameRect);
    // TODO: this CrashIf() fires in pre-release e.g. 64011
    // CrashIf(WindowRect(win.hwndFrame) != win.nonFullScreenFrameRect);
    // We have to relayout here, because it isn't done in the SetWindowPos nor MoveWindow,
    // if the client rectangle hasn't changed.
    if (ClientRect(win->hwndFrame) == cr) {
        RelayoutFrame(win);
    }
}

void OnMenuViewFullscreen(WindowInfo* win, bool presentation) {
    bool enterFullScreen = presentation ? !win->presentation : !win->isFullScreen;

    if (win->presentation || win->isFullScreen) {
        ExitFullScreen(win);
    } else {
        RememberDefaultWindowPosition(win);
    }

    if (enterFullScreen && (!presentation || win->IsDocLoaded())) {
        EnterFullScreen(win, presentation);
    }
}

static void OnMenuViewPresentation(WindowInfo* win) {
    // only DisplayModel currently supports an actual presentation mode
    OnMenuViewFullscreen(win, win->AsFixed() != nullptr);
}

// make sure that idx falls within <0, max-1> inclusive range
// negative numbers wrap from the end
static int wrapIdx(int idx, int max) {
    for (; idx < 0; idx += max) {
        idx += max;
    }
    return idx % max;
}

void AdvanceFocus(WindowInfo* win) {
    // Tab order: Frame -> Page -> Find -> ToC -> Favorites -> Frame -> ...

    bool hasToolbar = !win->isFullScreen && !win->presentation && gGlobalPrefs->showToolbar && win->IsDocLoaded();
    int direction = IsShiftPressed() ? -1 : 1;

    const int MAX_WINDOWS = 5;
    HWND tabOrder[MAX_WINDOWS] = {win->hwndFrame};
    int nWindows = 1;
    if (hasToolbar) {
        tabOrder[nWindows++] = win->hwndPageBox;
    }
    if (hasToolbar && NeedsFindUI(win)) {
        tabOrder[nWindows++] = win->hwndFindBox;
    }
    if (win->tocLoaded && win->tocVisible) {
        tabOrder[nWindows++] = win->tocTreeCtrl->hwnd;
    }
    if (gGlobalPrefs->showFavorites) {
        tabOrder[nWindows++] = win->favTreeCtrl->hwnd;
    }
    CrashIf(nWindows > MAX_WINDOWS);

    // find the currently focused element
    HWND focused = GetFocus();
    int i = 0;
    while (i < nWindows) {
        if (tabOrder[i] == focused) {
            break;
        }
        i++;
    }
    // if it's not in the tab order, start at the beginning
    if (i == nWindows) {
        i = wrapIdx(-direction, nWindows);
    }
    // focus the next available element
    i = wrapIdx(i + direction, nWindows);
    SetFocus(tabOrder[i]);
}

// allow to distinguish a '/' caused by VK_DIVIDE (rotates a document)
// from one typed on the main keyboard (focuses the find textbox)
static bool gIsDivideKeyDown = false;

static bool ChmForwardKey(WPARAM key) {
    if ((VK_LEFT == key) || (VK_RIGHT == key)) {
        return true;
    }
    if ((VK_UP == key) || (VK_DOWN == key)) {
        return true;
    }
    if ((VK_HOME == key) || (VK_END == key)) {
        return true;
    }
    if ((VK_PRIOR == key) || (VK_NEXT == key)) {
        return true;
    }
    if ((VK_MULTIPLY == key) || (VK_DIVIDE == key)) {
        return true;
    }
    return false;
}

bool FrameOnKeydown(WindowInfo* win, WPARAM key, LPARAM lp, bool inTextfield) {
    if (PM_BLACK_SCREEN == win->presentation || PM_WHITE_SCREEN == win->presentation) {
        // black/white screen is disabled on any unmodified key press in FrameOnChar
        return true;
    }

    if (VK_ESCAPE == key) {
        CancelDrag(win);
        return true;
    }

    bool isDel = VK_DELETE == key || VK_BACK == key;
    if (isDel && !inTextfield) {
        Point pt{0, 0};
        bool ok = GetCursorPosInHwnd(win->hwndCanvas, pt);
        DisplayModel* dm = win->AsFixed();
        Annotation* annot = nullptr;
        if (ok && dm) {
            int pageNoUnderCursor = dm->GetPageNoByPoint(pt);
            if (pageNoUnderCursor > 0) {
                annot = dm->GetAnnotationAtPos(pt, nullptr);
            }
        }
        if (annot) {
            auto tab = win->currentTab;
            DeleteAnnotationAndUpdateUI(tab, tab->editAnnotsWindow, annot);
            delete annot;
        }

        return true;
    }

    bool isCtrl = IsCtrlPressed();
    bool isShift = IsShiftPressed();

    if (win->tabsVisible && isCtrl && VK_TAB == key) {
        TabsOnCtrlTab(win, isShift);
        return true;
    }

    if ((VK_LEFT == key || VK_RIGHT == key) && isShift && isCtrl && !win->IsAboutWindow() && !inTextfield) {
        // folder browsing should also work when an error page is displayed,
        // so special-case it before the win->IsDocLoaded() check
        BrowseFolder(win, VK_RIGHT == key);
        return true;
    }

    if (!win->IsDocLoaded()) {
        return false;
    }

    DisplayModel* dm = win->AsFixed();
    // some of the chm key bindings are different than the rest and we
    // need to make sure we don't break them
    bool isChm = win->AsChm();

    bool isPageUp = (isCtrl && (VK_UP == key));
    if (!isChm) {
        isPageUp |= (VK_PRIOR == key) && !isCtrl;
    }

    if (isPageUp) {
        int currentPos = GetScrollPos(win->hwndCanvas, SB_VERT);
        if (win->ctrl->GetZoomVirtual() != ZOOM_FIT_CONTENT) {
            SendMessageW(win->hwndCanvas, WM_VSCROLL, SB_PAGEUP, 0);
        }
        if (GetScrollPos(win->hwndCanvas, SB_VERT) == currentPos) {
            win->ctrl->GoToPrevPage(true);
        }
        return true;
    }

    bool isPageDown = (isCtrl && (VK_DOWN == key));
    if (!isChm) {
        isPageDown |= (VK_NEXT == key) && !isCtrl;
    }

    if (isPageDown) {
        int currentPos = GetScrollPos(win->hwndCanvas, SB_VERT);
        if (win->ctrl->GetZoomVirtual() != ZOOM_FIT_CONTENT) {
            SendMessageW(win->hwndCanvas, WM_VSCROLL, SB_PAGEDOWN, 0);
        }
        if (GetScrollPos(win->hwndCanvas, SB_VERT) == currentPos) {
            win->ctrl->GoToNextPage();
        }
        return true;
    }

    if (isChm) {
        if (ChmForwardKey(key)) {
            win->AsChm()->PassUIMsg(WM_KEYDOWN, key, lp);
            return true;
        }
    }
    // lf("key=%d,%c,shift=%d\n", key, (char)key, (int)WasKeyDown(VK_SHIFT));

    if (VK_UP == key) {
        if (dm && dm->NeedVScroll()) {
            SendMessageW(win->hwndCanvas, WM_VSCROLL, isShift ? SB_HPAGEUP : SB_LINEUP, 0);
        } else {
            win->ctrl->GoToPrevPage(true);
        }
    } else if (VK_DOWN == key) {
        if (dm && dm->NeedVScroll()) {
            SendMessageW(win->hwndCanvas, WM_VSCROLL, isShift ? SB_HPAGEDOWN : SB_LINEDOWN, 0);
        } else {
            win->ctrl->GoToNextPage();
        }
    } else if (VK_PRIOR == key && isCtrl) {
        win->ctrl->GoToPrevPage();
    } else if (VK_NEXT == key && isCtrl) {
        win->ctrl->GoToNextPage();
    } else if (VK_HOME == key && isCtrl) {
        win->ctrl->GoToFirstPage();
    } else if (VK_END == key && isCtrl) {
        if (!win->ctrl->GoToLastPage()) {
            SendMessageW(win->hwndCanvas, WM_VSCROLL, SB_BOTTOM, 0);
        }
    } else if (inTextfield) {
        // The remaining keys have a different meaning
        return false;
    } else if (VK_LEFT == key) {
        if (dm && dm->NeedHScroll() && !isCtrl) {
            SendMessageW(win->hwndCanvas, WM_HSCROLL, isShift ? SB_PAGELEFT : SB_LINELEFT, 0);
        } else {
            win->ctrl->GoToPrevPage();
        }
    } else if (VK_RIGHT == key) {
        if (dm && dm->NeedHScroll() && !isCtrl) {
            SendMessageW(win->hwndCanvas, WM_HSCROLL, isShift ? SB_PAGERIGHT : SB_LINERIGHT, 0);
        } else {
            win->ctrl->GoToNextPage();
        }
    } else if (VK_HOME == key) {
        win->ctrl->GoToFirstPage();
    } else if (VK_END == key) {
        if (!win->ctrl->GoToLastPage()) {
            SendMessageW(win->hwndCanvas, WM_VSCROLL, SB_BOTTOM, 0);
        }
    } else if (VK_MULTIPLY == key && dm) {
        dm->RotateBy(90);
    } else if (VK_DIVIDE == key && dm) {
        dm->RotateBy(-90);
        gIsDivideKeyDown = true;
    } else {
        return false;
    }

    return true;
}

static WCHAR SingleCharLowerW(WCHAR c) {
    WCHAR buf[2] = {c, 0};
    CharLowerBuffW(buf, 1);
    return buf[0];
}

static void OnFrameKeyEsc(WindowInfo* win) {
    if (win->findThread) {
        AbortFinding(win, false);
        return;
    }
    if (win->notifications->GetForGroup(NG_PERSISTENT_WARNING)) {
        win->notifications->RemoveForGroup(NG_PERSISTENT_WARNING);
        return;
    }
    if (win->notifications->GetForGroup(NG_PAGE_INFO_HELPER)) {
        win->notifications->RemoveForGroup(NG_PAGE_INFO_HELPER);
        return;
    }
    if (win->notifications->GetForGroup(NG_CURSOR_POS_HELPER)) {
        win->notifications->RemoveForGroup(NG_CURSOR_POS_HELPER);
        return;
    }
    if (win->showSelection) {
        ClearSearchResult(win);
        return;
    }
    if (gGlobalPrefs->escToExit && CanCloseWindow(win)) {
        CloseWindow(win, true, false);
        return;
    }
    if (win->presentation || win->isFullScreen) {
        OnMenuViewFullscreen(win, win->presentation != PM_DISABLED);
        return;
    }
}

static void OnFrameKeyB(WindowInfo* win) {
    auto* ctrl = win->ctrl;
    bool isSinglePage = IsSingle(ctrl->GetDisplayMode());

    DisplayModel* dm = win->AsFixed();
    if (dm && !isSinglePage) {
        bool forward = !IsShiftPressed();
        int currPage = ctrl->CurrentPageNo();
        bool isVisible = dm->FirstBookPageVisible();
        if (forward) {
            isVisible = dm->LastBookPageVisible();
        }
        if (isVisible) {
            return;
        }

        DisplayMode newMode = DisplayMode::BookView;
        if (IsBookView(ctrl->GetDisplayMode())) {
            newMode = DisplayMode::Facing;
        }
        SwitchToDisplayMode(win, newMode, true);

        if (forward && currPage >= ctrl->CurrentPageNo() && (currPage > 1 || newMode == DisplayMode::BookView)) {
            ctrl->GoToNextPage();
        } else if (!forward && currPage <= ctrl->CurrentPageNo()) {
            win->ctrl->GoToPrevPage();
        }
    } else if (false && !isSinglePage) {
        // "e-book view": flip a single page
        bool forward = !IsShiftPressed();
        int nextPage = ctrl->CurrentPageNo() + (forward ? 1 : -1);
        if (ctrl->ValidPageNo(nextPage)) {
            ctrl->GoToPage(nextPage, false);
        }
    } else if (win->presentation) {
        win->ChangePresentationMode(PM_BLACK_SCREEN);
    }
}

static void AddUniquePageNo(Vec<int>& v, int pageNo) {
    for (auto n : v) {
        if (n == pageNo) {
            return;
        }
    }
    v.Append(pageNo);
}

Vec<Annotation*> MakeAnnotationFromSelection(TabInfo* tab, AnnotationType annotType) {
    // converts current selection to annotation (or back to regular text
    // if it's already an annotation)
    Vec<Annotation*> annots;
    DisplayModel* dm = tab->AsFixed();
    if (!dm) {
        return annots;
    }
    auto engine = dm->GetEngine();
    bool supportsAnnots = EngineSupportsAnnotations(engine);
    WindowInfo* win = tab->win;
    bool ok = supportsAnnots && win->showSelection && tab->selectionOnPage;
    if (!ok) {
        return annots;
    }

    Vec<SelectionOnPage>* s = tab->selectionOnPage;
    Vec<int> pageNos;
    for (auto& sel : *s) {
        int pageNo = sel.pageNo;
        if (!dm->ValidPageNo(pageNo)) {
            continue;
        }
        AddUniquePageNo(pageNos, pageNo);
    }
    if (pageNos.empty()) {
        return annots;
    }

    for (auto pageNo : pageNos) {
        Vec<RectF> rects;
        for (auto& sel : *s) {
            if (pageNo != sel.pageNo) {
                continue;
            }
            rects.Append(sel.rect);
        }
        Annotation* annot = EngineMupdfCreateAnnotation(engine, annotType, pageNo, PointF{});
        SetQuadPointsAsRect(annot, rects);
        annots.Append(annot);
    }

    // copy selection to clipboard so that user can use Ctrl-V to set contents
    CopySelectionToClipboard(win);
    DeleteOldSelectionInfo(win, true);
    WindowInfoRerender(win);
    ToolbarUpdateStateForWindow(win, true);
    return annots;
}

static void ShowCursorPositionInDoc(WindowInfo* win) {
    // "cursor position" tip: make figuring out the current
    // cursor position in cm/in/pt possible (for exact layouting)
    if (!win->AsFixed()) {
        return;
    }
    Point pt;
    if (GetCursorPosInHwnd(win->hwndCanvas, pt)) {
        UpdateCursorPositionHelper(win, pt, nullptr);
    }
}

static void FrameOnChar(WindowInfo* win, WPARAM key, LPARAM info = 0) {
    if (PM_BLACK_SCREEN == win->presentation || PM_WHITE_SCREEN == win->presentation) {
        win->ChangePresentationMode(PM_ENABLED);
        return;
    }

    bool isCtrl = IsCtrlPressed();
    bool isShift = IsShiftPressed();
    bool isAlt = IsAltPressed();

    if (key >= 0x100 && info && !isCtrl && !isAlt) {
        // determine the intended keypress by scan code for non-Latin keyboard layouts
        uint vk = MapVirtualKeyW((info >> 16) & 0xFF, MAPVK_VSC_TO_VK);
        if ('A' <= vk && vk <= 'Z') {
            key = vk;
        }
    }

    if (IsCharUpperW((WCHAR)key)) {
        key = (WPARAM)SingleCharLowerW((WCHAR)key);
    }

    switch (key) {
        case VK_ESCAPE:
            OnFrameKeyEsc(win);
            return;
        case 'q':
            // close the current document (it's too easy to press for discarding multiple tabs)
            // quit if this is the last window
            CloseCurrentTab(win, true);
            return;
        case 'r':
            ReloadDocument(win, false);
            return;
        case VK_TAB:
            AdvanceFocus(win);
            break;
    }

    if (!win->IsDocLoaded()) {
        return;
    }

    auto* ctrl = win->ctrl;
    DisplayModel* dm = win->AsFixed();
    auto currentTab = win->currentTab;

    auto openAnnotsInEditWindow = [&win](Vec<Annotation*>& annots, bool isShift) -> void {
        if (annots.empty()) {
            return;
        }
        WindowInfoRerender(win);
        if (isShift) {
            StartEditAnnotations(win->currentTab, annots);
            return;
        }
        auto w = win->currentTab->editAnnotsWindow;
        if (w) {
            for (auto annot : annots) {
                AddAnnotationToEditWindow(w, annot);
            }
        } else {
            DeleteVecMembers(annots);
        }
    };

    switch (key) {
        case VK_SPACE:
        case VK_RETURN:
            FrameOnKeydown(win, isShift ? VK_PRIOR : VK_NEXT, 0);
            break;
        case VK_BACK: {
            bool forward = isShift;
            ctrl->Navigate(forward ? 1 : -1);
        } break;
        case 'g':
            OnMenuGoToPage(win);
            break;
        case 'h':
            FrameOnKeydown(win, VK_LEFT, 0);
            break;
        case 'j':
            FrameOnKeydown(win, VK_DOWN, 0);
            break;
        case 'k':
            FrameOnKeydown(win, VK_UP, 0);
            break;
        case 'l':
            FrameOnKeydown(win, VK_RIGHT, 0);
            break;
        case 'n':
            ctrl->GoToNextPage();
            break;
        case 'p':
            ctrl->GoToPrevPage();
            break;
        case 'z':
            win->ToggleZoom();
            break;
        case '[':
            if (dm) {
                dm->RotateBy(-90);
            }
            break;
        case ']':
            if (dm) {
                dm->RotateBy(90);
            }
            break;
        case 'f':
            if (win->isFullScreen || win->presentation) {
                ExitFullScreen(win);
            } else {
                EnterFullScreen(win);
            }
            break;
        // per https://en.wikipedia.org/wiki/Keyboard_layout
        // almost all keyboard layouts allow to press either
        // '+' or '=' unshifted (and one of them is also often
        // close to '-'); the other two alternatives are for
        // the major exception: the two Swiss layouts
        case '+':
        case '=':
        case 0xE0:
        case 0xE4: {
            float newZoom = ctrl->GetNextZoomStep(ZOOM_MAX);
            ZoomToSelection(win, newZoom, false);
        } break;
        case '-': {
            float newZoom = ctrl->GetNextZoomStep(ZOOM_MIN);
            ZoomToSelection(win, newZoom, false);
        } break;
        case '/':
            if (!gIsDivideKeyDown) {
                OnMenuFind(win);
            }
            gIsDivideKeyDown = false;
            break;
        case 'c':
            OnMenuViewContinuous(win);
            break;
        case 'b':
            OnFrameKeyB(win);
            break;
        case '.':
            // for Logitech's wireless presenters which target PowerPoint's shortcuts
            if (win->presentation) {
                win->ChangePresentationMode(PM_BLACK_SCREEN);
            }
            break;
        case 'w':
            if (win->presentation) {
                win->ChangePresentationMode(PM_WHITE_SCREEN);
            }
            break;
        case 'i':
            // experimental "page info" tip: make figuring out current page and
            // total pages count a one-key action (unless they're already visible)
            if (isShift) {
                gGlobalPrefs->fixedPageUI.invertColors ^= true;
                UpdateDocumentColors();
                UpdateTreeCtrlColors(win);
                // UpdateUiForCurrentTab(win);
            } else {
                if (dm) {
                    TogglePageInfoHelper(win);
                }
            }
            break;
        case 'm':
            ShowCursorPositionInDoc(win);
            break;
        case 'u': {
            auto annots = MakeAnnotationFromSelection(currentTab, AnnotationType::Underline);
            openAnnotsInEditWindow(annots, isShift);
        } break;
        case 'a': {
            auto annots = MakeAnnotationFromSelection(currentTab, AnnotationType::Highlight);
            openAnnotsInEditWindow(annots, isShift);
        } break;
    }
}

static bool FrameOnSysChar(WindowInfo* win, WPARAM key) {
    // use Alt+1 to Alt+8 for selecting the first 8 tabs and Alt+9 for the last tab
    if (win->tabsVisible && ('1' <= key && key <= '9')) {
        TabsSelect(win, key < '9' ? (int)(key - '1') : (int)win->tabs.size() - 1);
        return true;
    }
    // Alt + Space opens a sys menu
    if (key == ' ') {
        OpenSystemMenu(win);
        return true;
    }
    return false;
}

static void OnSidebarSplitterMove(SplitterMoveEvent* ev) {
    SplitterCtrl* splitter = ev->w;
    HWND hwnd = splitter->hwnd;
    WindowInfo* win = FindWindowInfoByHwnd(hwnd);

    Point pcur;
    GetCursorPosInHwnd(win->hwndFrame, pcur);
    int sidebarDx = pcur.x; // without splitter

    // make sure to keep this in sync with the calculations in RelayoutFrame
    // note: without the min/max(..., rToc.dx), the sidebar will be
    //       stuck at its width if it accidentally got too wide or too narrow
    Rect rFrame = ClientRect(win->hwndFrame);
    Rect rToc = ClientRect(win->hwndTocBox);
    int minDx = std::min(SIDEBAR_MIN_WIDTH, rToc.dx);
    int maxDx = std::max(rFrame.dx / 2, rToc.dx);
    if (sidebarDx < minDx || sidebarDx > maxDx) {
        ev->resizeAllowed = false;
        return;
    }

    RelayoutFrame(win, false, sidebarDx);
}

static void OnFavSplitterMove(SplitterMoveEvent* ev) {
    SplitterCtrl* splitter = ev->w;
    HWND hwnd = splitter->hwnd;
    WindowInfo* win = FindWindowInfoByHwnd(hwnd);

    Point pcur;
    GetCursorPosInHwnd(win->hwndTocBox, pcur);
    int tocDy = pcur.y; // without splitter

    // make sure to keep this in sync with the calculations in RelayoutFrame
    Rect rFrame = ClientRect(win->hwndFrame);
    Rect rToc = ClientRect(win->hwndTocBox);
    CrashIf(rToc.dx != ClientRect(win->hwndFavBox).dx);
    int minDy = std::min(TOC_MIN_DY, rToc.dy);
    int maxDy = std::max(rFrame.dy - TOC_MIN_DY, rToc.dy);
    if (tocDy < minDy || tocDy > maxDy) {
        ev->resizeAllowed = false;
        return;
    }
    gGlobalPrefs->tocDy = tocDy;
    RelayoutFrame(win, false, rToc.dx);
}

void SetSidebarVisibility(WindowInfo* win, bool tocVisible, bool showFavorites) {
    if (gPluginMode || !HasPermission(Perm::DiskAccess)) {
        showFavorites = false;
    }

    if (!win->IsDocLoaded() || !win->ctrl->HacToc()) {
        tocVisible = false;
    }

    if (PM_BLACK_SCREEN == win->presentation || PM_WHITE_SCREEN == win->presentation) {
        tocVisible = false;
        showFavorites = false;
    }

    if (tocVisible) {
        LoadTocTree(win);
        CrashIf(!win->tocLoaded);
    }

    if (showFavorites) {
        PopulateFavTreeIfNeeded(win);
    }

    if (!win->currentTab) {
        CrashIf(tocVisible);
    } else if (!win->presentation) {
        win->currentTab->showToc = tocVisible;
    } else if (PM_ENABLED == win->presentation) {
        win->currentTab->showTocPresentation = tocVisible;
    }
    win->tocVisible = tocVisible;

    // TODO: make this a per-window setting as well?
    gGlobalPrefs->showFavorites = showFavorites;

    if ((!tocVisible && IsFocused(win->tocTreeCtrl->hwnd)) || (!showFavorites && IsFocused(win->favTreeCtrl->hwnd))) {
        SetFocus(win->hwndFrame);
    }

    win::SetVisibility(win->sidebarSplitter->hwnd, tocVisible || showFavorites);
    win::SetVisibility(win->hwndTocBox, tocVisible);
    win->sidebarSplitter->isLive = true;

    win::SetVisibility(win->favSplitter->hwnd, tocVisible && showFavorites);
    win::SetVisibility(win->hwndFavBox, showFavorites);
    win->favSplitter->isLive = true;

    RelayoutFrame(win, false);
}

// Tests that various ways to crash will generate crash report.
// Commented-out because they are ad-hoc. Left in code because
// I don't want to write them again if I ever need to test crash reporting
#if 0
#include <signal.h>
static void TestCrashAbort()
{
    raise(SIGABRT);
}

struct Base;
void foo(Base* b);

struct Base {
    Base() {
        foo(this);
    }
    virtual ~Base() = 0;
    virtual void pure() = 0;
};
struct Derived : public Base {
    void pure() { }
};

void foo(Base* b) {
    b->pure();
}

static void TestCrashPureCall()
{
    Derived d; // should crash
}

// tests that making a big allocation with new raises an exception
static int TestBigNew()
{
    size_t size = 1024*1024*1024*1;  // 1 GB should be out of reach
    char *mem = (char*)1;
    while (mem) {
        mem = new char[size];
    }
    // just some code so that compiler doesn't optimize this code to null
    for (size_t i = 0; i < 1024; i++) {
        mem[i] = i & 0xff;
    }
    int res = 0;
    for (size_t i = 0; i < 1024; i++) {
        res += mem[i];
    }
    return res;
}
#endif

static void SaveAnnotationsAndCloseEditAnnowtationsWindow(TabInfo* tab) {
    EngineBase* engine = tab->AsFixed()->GetEngine();
    auto path = ToUtf8Temp(engine->FileName());
    bool ok = EngineMupdfSaveUpdated(engine, {}, [&tab, &path](std::string_view mupdfErr) {
        str::Str msg;
        // TODO: duplicated message
        msg.AppendFmt(_TRA("Saving of '%s' failed with: '%s'"), path.Get(), mupdfErr.data());
        tab->win->ShowNotification(msg.AsView(), NotificationOptions::Warning);
    });
    if (!ok) {
        return;
    }
    str::Str msg;
    msg.AppendFmt(_TRA("Saved annotations to '%s'"), path.Get());
    tab->win->ShowNotification(msg.AsView());

    CloseAndDeleteEditAnnotationsWindow(tab->editAnnotsWindow);
    tab->editAnnotsWindow = nullptr;
}

#if 0
static bool NeedsURLEncoding(WCHAR c) {
    // TODO: implement me
    return false;
}
#endif

static str::WStr URLEncode(const WCHAR* s) {
    WCHAR buf[INTERNET_MAX_URL_LENGTH]{0};
    DWORD cchSizeInOut = dimof(buf) - 1;
    DWORD flags = URL_ESCAPE_AS_UTF8;
    UrlEscapeW(s, buf, &cchSizeInOut, flags);
    return str::WStr(buf);
#if 0
    str::WStr res;
    size_t n = sv.size();
    const WCHAR* s = sv.data();
    for (size_t i = 0; i < n; i++) {
        WCHAR c = s[i];
        if (NeedsURLEncoding(c)) {
            // TODO: implement me
            res.AppendChar(c);
        }
        else {
            res.AppendChar(c);
        }
    }
    return res;
#endif
}

constexpr const WCHAR* kUserLangStr = L"${userlang}";
constexpr const WCHAR* kSelectionStr = L"${selection}";

static void LaunchBrowserWithSelection(TabInfo* tab, const WCHAR* urlPattern) {
    if (!tab || !HasPermission(Perm::InternetAccess) || !HasPermission(Perm::CopySelection)) {
        return;
    }

#if 0 // TODO: get selection from Chm
    if (tab->AsChm()) {
        tab->AsChm()->CopySelection();
        return;
    }
#endif

    bool isTextOnlySelectionOut; // if false, a rectangular selection
    WCHAR* selText = GetSelectedText(tab, L"\n", isTextOnlySelectionOut);
    if (!selText) {
        return;
    }
    str::WStr encodedSelection = URLEncode(selText);
    str::WStr url(urlPattern);
    // assume that user might typo and use e.g. ${userLang} in url
    // so replace with cannonical lower-cased version
    const WCHAR* pos = str::FindI(url.LendData(), kUserLangStr);
    if (pos) {
        memcpy((void*)pos, (void*)kUserLangStr, str::Len(kUserLangStr) * sizeof(kUserLangStr[0]));
    }
    Replace(url, kSelectionStr, encodedSelection.Get());
    const char* lang = trans::GetCurrentLangCode();
    if (str::Eq(lang, "en")) {
        // no point to translate from en => en
        // a hack for google translate: instead of translating to forced language
        // leave dest lang unspecified which presumably will select whatever
        // language the user used last
        Replace(url, L"&tl=${userlang}", L"");
        lang = "de";
    }
    auto langW = ToWstrTemp(lang);
    Replace(url, kUserLangStr, langW);
    LaunchBrowser(url.Get());
    str::Free(selText);
}

// TODO: rather arbitrary divide of responsibility between this and CopySelectionToClipboard()
static void CopySelectionInTabToClipboard(TabInfo* tab) {
    // Don't break the shortcut for text boxes
    if (!tab || !tab->win) {
        return;
    }
    if (IsFocused(tab->win->hwndFindBox) || IsFocused(tab->win->hwndPageBox)) {
        SendMessageW(GetFocus(), WM_COPY, 0, 0);
        return;
    }
    if (!HasPermission(Perm::CopySelection)) {
        return;
    }
    if (tab->AsChm()) {
        tab->AsChm()->CopySelection();
        return;
    }
    if (tab->selectionOnPage) {
        CopySelectionToClipboard(tab->win);
        return;
    }
    // TODO: can this be reached?
    if (tab->AsFixed()) {
        tab->win->ShowNotification(_TR("Select content with Ctrl+left mouse button"));
    }
}

static void OnMenuCustomZoom(WindowInfo* win) {
    if (!win->IsDocLoaded()) {
        return;
    }

    float zoom = win->ctrl->GetZoomVirtual();
    if (!Dialog_CustomZoom(win->hwndFrame, win->AsChm(), &zoom)) {
        return;
    }
    ZoomToSelection(win, zoom);
}

static LRESULT FrameOnCommand(WindowInfo* win, HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    int wmId = LOWORD(wp);

    if (wmId >= 0xF000) {
        // handle system menu messages for the Window menu (needed for Tabs in Titlebar)
        return SendMessageW(hwnd, WM_SYSCOMMAND, wp, lp);
    }

    // check if the menuId belongs to an entry in the list of
    // recently opened files and load the referenced file if it does
    if ((wmId >= CmdFileHistoryFirst) && (wmId <= CmdFileHistoryLast)) {
        FileState* state = gFileHistory.Get(wmId - CmdFileHistoryFirst);
        if (state && HasPermission(Perm::DiskAccess)) {
            LoadArgs args(state->filePath, win);
            LoadDocument(args);
        }
        return 0;
    }

    // 10 submenus max with 10 items each max (=100) plus generous buffer => 200
    static_assert(CmdFavoriteLast - CmdFavoriteFirst == 256, "wrong number of favorite menu ids");
    if ((wmId >= CmdFavoriteFirst) && (wmId <= CmdFavoriteLast)) {
        GoToFavoriteByMenuId(win, wmId);
    }

#if defined(ENABLE_THEME)
    // check if the menuId belongs to a theme
    if ((wmId >= IDM_CHANGE_THEME_FIRST) && (wmId <= IDM_CHANGE_THEME_LAST)) {
        auto newThemeName = GetThemeByIndex(wmId - IDM_CHANGE_THEME_FIRST)->name;
        str::ReplaceWithCopy(&gGlobalPrefs->themeName, newThemeName);
        RelayoutWindow(win);    // fix tabbar height
        UpdateDocumentColors(); // update document colors
        RedrawWindow(win->hwndFrame, nullptr, nullptr,
                     RDW_ERASE | RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN); // paint new theme
        UpdateDocumentColors();        // doing this a second time ensures the frequently read documents are updated
        UpdateAppMenu(win, (HMENU)wp); // update the radio buttons
        prefs::Save();                 // save new preferences
    }
#endif

    if (!win) {
        return DefWindowProc(hwnd, msg, wp, lp);
    }

    TabInfo* tab = win->currentTab;
    if (!win->IsAboutWindow()) {
        if (CmdOpenWithExternalFirst <= wmId && wmId <= CmdOpenWithExternalLast) {
            size_t idx = (size_t)wmId - (size_t)CmdOpenWithExternalFirst;
            ViewWithExternalViewer(tab, idx);
            return 0;
        }
        if (CmdOpenWithFirst < wmId && wmId < CmdOpenWithLast) {
            ViewWithKnownExternalViewer(tab, wmId);
            return 0;
        }
    }

    if (CmdSelectionHandlerFirst <= wmId && wmId < CmdSelectionHandlerLast) {
        SelectionHandler* selectedSH{nullptr};
        for (auto& sh : *gGlobalPrefs->selectionHandlers) {
            if (sh->cmdID == wmId) {
                selectedSH = sh;
                break;
            }
        }
        ReportIf(!selectedSH);
        WCHAR* url = ToWstrTemp(selectedSH->url);
        // try to auto-fix url
        bool isValidURL = str::StartsWithI(url, L"http://") || str::StartsWithI(url, L"https://");
        if (!isValidURL) {
            url = str::JoinTemp(L"https://", url);
        }
        LaunchBrowserWithSelection(tab, url);
        return 0;
    }

    auto* ctrl = win->ctrl;
    // most of them require a win, the few exceptions are no-ops
    switch (wmId) {
        case CmdNewWindow:
            OnMenuNewWindow();
            break;

        case CmdDuplicateInNewWindow:
            OnDuplicateInNewWindow(win);
            break;

        case CmdOpen:
            OnMenuOpen(win);
            break;

        case CmdOpenFolder:
            OnMenuOpenFolder(win);
            break;

        case CmdSaveAs:
            OnMenuSaveAs(win);
            break;

        case CmdRenameFile:
            OnMenuRenameFile(win);
            break;

        case CmdShowInFolder:
            OnMenuShowInFolder(win);
            break;

        case CmdPrint:
            OnMenuPrint(win);
            break;

        case CmdClose:
            CloseCurrentTab(win);
            break;

        case CmdExit:
            OnMenuExit();
            break;

        case CmdRefresh:
            ReloadDocument(win, false);
            break;

        case CmdSaveAsBookmark:
            OnMenuSaveBookmark(win);
            break;

        case CmdZoomFitWidthAndContinuous:
            ChangeZoomLevel(win, ZOOM_FIT_WIDTH, true);
            break;

        case CmdZoomFitPageAndSinglePage:
            ChangeZoomLevel(win, ZOOM_FIT_PAGE, false);
            break;

        case CmdZoomIn:
            if (win->IsDocLoaded()) {
                auto zoom = ctrl->GetNextZoomStep(ZOOM_MAX);
                ZoomToSelection(win, zoom, false);
            }
            break;

        case CmdZoomOut:
            if (win->IsDocLoaded()) {
                auto zoom = ctrl->GetNextZoomStep(ZOOM_MIN);
                ZoomToSelection(win, zoom, false);
            }
            break;

        case CmdZoom6400:
        case CmdZoom3200:
        case CmdZoom1600:
        case CmdZoom800:
        case CmdZoom400:
        case CmdZoom200:
        case CmdZoom150:
        case CmdZoom125:
        case CmdZoom100:
        case CmdZoom50:
        case CmdZoom25:
        case CmdZoom12_5:
        case CmdZoom8_33:
        case CmdZoomFitPage:
        case CmdZoomFitWidth:
        case CmdZoomFitContent:
        case CmdZoomActualSize:
            OnMenuZoom(win, wmId);
            break;

        case CmdZoomCustom:
            OnMenuCustomZoom(win);
            break;

        case CmdViewSinglePage:
            SwitchToDisplayMode(win, DisplayMode::SinglePage, true);
            break;

        case CmdViewFacing:
            SwitchToDisplayMode(win, DisplayMode::Facing, true);
            break;

        case CmdViewBook:
            SwitchToDisplayMode(win, DisplayMode::BookView, true);
            break;

        case CmdViewContinuous:
            OnMenuViewContinuous(win);
            break;

        case CmdViewMangaMode:
            OnMenuViewMangaMode(win);
            break;

        case CmdViewShowHideToolbar:
            OnMenuViewShowHideToolbar();
            break;

        case CmdViewShowHideScrollbars:
            OnMenuViewShowHideScrollbars();
            break;

        case CmdSaveAnnotations:
            SaveAnnotationsAndCloseEditAnnowtationsWindow(tab);
            break;

        case CmdEditAnnotations:
            StartEditAnnotations(tab, nullptr);
            break;

        case CmdViewShowHideMenuBar:
            if (!win->tabsInTitlebar) {
                ShowHideMenuBar(win);
            }
            break;

        case CmdChangeLanguage:
            OnMenuChangeLanguage(win->hwndFrame);
            break;

        case CmdViewBookmarks:
            ToggleTocBox(win);
            break;

        case CmdGoToNextPage:
            if (win->IsDocLoaded()) {
                ctrl->GoToNextPage();
            }
            break;

        case CmdGoToPrevPage:
            if (win->IsDocLoaded()) {
                ctrl->GoToPrevPage();
            }
            break;

        case CmdGoToFirstPage:
            if (win->IsDocLoaded()) {
                ctrl->GoToFirstPage();
            }
            break;

        case CmdGoToLastPage:
            if (win->IsDocLoaded()) {
                ctrl->GoToLastPage();
            }
            break;

        case CmdGoToPage:
            OnMenuGoToPage(win);
            break;

        case CmdViewPresentationMode:
            OnMenuViewPresentation(win);
            break;

        case CmdViewFullScreen:
            OnMenuViewFullscreen(win);
            break;

        case CmdViewRotateLeft:
            if (win->AsFixed()) {
                win->AsFixed()->RotateBy(-90);
            }
            break;

        case CmdViewRotateRight:
            if (win->AsFixed()) {
                win->AsFixed()->RotateBy(90);
            }
            break;

        case CmdFindFirst:
            OnMenuFind(win);
            break;

        case CmdFindNext:
            OnMenuFindNext(win);
            break;

        case CmdFindPrev:
            OnMenuFindPrev(win);
            break;

        case CmdFindMatch:
            OnMenuFindMatchCase(win);
            break;

        case CmdFindNextSel:
            OnMenuFindSel(win, TextSearchDirection::Forward);
            break;

        case CmdFindPrevSel:
            OnMenuFindSel(win, TextSearchDirection::Backward);
            break;

        case CmdHelpVisitWebsite:
            SumatraLaunchBrowser(WEBSITE_MAIN_URL);
            break;

        case CmdHelpOpenManualInBrowser:
            SumatraLaunchBrowser(WEBSITE_MANUAL_URL);
            break;

        case CmdContributeTranslation:
            SumatraLaunchBrowser(WEBSITE_TRANSLATIONS_URL);
            break;

        case CmdHelpAbout:
            OnMenuAbout();
            break;

        case CmdCheckUpdate:
            CheckForUpdateAsync(win, UpdateCheck::UserInitiated);
            break;

        case CmdOptions:
            OnMenuOptions(win);
            break;

        case CmdAdvancedOptions:
            OnMenuAdvancedOptions();
            break;

        case CmdSendByEmail:
            SendAsEmailAttachment(tab, win->hwndFrame);
            break;

        case CmdProperties:
            OnMenuProperties(win);
            break;

        case CmdMoveFrameFocus:
            if (!IsFocused(win->hwndFrame)) {
                SetFocus(win->hwndFrame);
            } else if (win->tocVisible) {
                SetFocus(win->tocTreeCtrl->hwnd);
            }
            break;

        case CmdGoToNavBack:
            if (win->IsDocLoaded()) {
                ctrl->Navigate(-1);
            }
            break;

        case CmdGoToNavForward:
            if (win->IsDocLoaded()) {
                ctrl->Navigate(1);
            }
            break;

        case CmdTranslateSelectionWithGoogle:
            LaunchBrowserWithSelection(
                tab, L"https://translate.google.com/?sl=auto&tl=${userlang}&op=translate&text=${selection}");
            break;

        case CmdTranslateSelectionWithDeepL:
            LaunchBrowserWithSelection(tab, L"https://www.deepl.com/translator#en/${userlang}/${selection}");
            break;

        case CmdSearchSelectionWithGoogle:
            LaunchBrowserWithSelection(tab, L"https://www.google.com/search?q=${selection}");
            break;

        case CmdSearchSelectionWithBing:
            LaunchBrowserWithSelection(tab, L"https://www.bing.com/search?q=${selection}");
            break;

        case CmdCopySelection:
            CopySelectionInTabToClipboard(tab);
            break;

        case CmdSelectAll:
            OnSelectAll(win);
            break;

        case CmdDebugShowLinks:
            gDebugShowLinks = !gDebugShowLinks;
            for (auto& w : gWindows) {
                w->RedrawAll(true);
            }
            break;

        case CmdDebugDownloadSymbols:
            DownloadDebugSymbols();
            break;

        case CmdDebugAnnotations:
            FrameOnChar(win, 'h');
            break;

#if defined(DEBUG)
        case CmdDebugTestApp:
            extern void TestApp(HINSTANCE hInstance);
            TestApp(GetModuleHandle(nullptr));
            break;
#endif

        case CmdDebugShowNotif: {
            win->ShowNotification(L"This is a notification", NotificationOptions::Warning);
            // TODO: this notification covers previous
            // win->ShowNotification(L"This is a second notification\nMy friend.");
        } break;

        case CmdDebugCrashMe:
            CrashMe();
            break;

        case CmdFavoriteAdd:
            AddFavoriteForCurrentPage(win);
            break;

        case CmdFavoriteDel:
            if (win->IsDocLoaded()) {
                DelFavorite(ctrl->GetFilePath(), win->currPageNo);
            }
            break;

        case CmdFavoriteToggle:
            ToggleFavorites(win);
            break;

        default:
            return DefWindowProc(hwnd, msg, wp, lp);
    }

    return 0;
}

static LRESULT OnFrameGetMinMaxInfo(MINMAXINFO* info) {
    // limit windows min width to prevent render loop when siderbar is too big
    info->ptMinTrackSize.x = MIN_WIN_DX - SIDEBAR_MIN_WIDTH + gGlobalPrefs->sidebarDx;
    info->ptMinTrackSize.y = MIN_WIN_DY;
    return 0;
}

HWND gLastActiveFrameHwnd = nullptr;

LRESULT CALLBACK WndProcSumatraFrame(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    WindowInfo* win = FindWindowInfoByHwnd(hwnd);

    // DbgLogMsg("frame:", hwnd, msg, wp, lp);
    if (win && win->tabsInTitlebar) {
        bool callDefault = true;
        LRESULT res = CustomCaptionFrameProc(hwnd, msg, wp, lp, &callDefault, win);
        if (!callDefault) {
            return res;
        }
    }

    LRESULT res = 0;
    if (HandleRegisteredMessages(hwnd, msg, wp, lp, res)) {
        return res;
    }

    switch (msg) {
        case WM_CREATE:
            // do nothing
            goto InitMouseWheelInfo;

        case WM_SIZE:
            if (win && SIZE_MINIMIZED != wp) {
                RememberDefaultWindowPosition(win);
                int dx = LOWORD(lp);
                int dy = HIWORD(lp);
                // dbglog::LogF("dx: %d, dy: %d", dx, dy);
                FrameOnSize(win, dx, dy);
            }
            break;

        case WM_GETMINMAXINFO:
            return OnFrameGetMinMaxInfo((MINMAXINFO*)lp);

        case WM_MOVE:
            if (win) {
                RememberDefaultWindowPosition(win);
            }
            break;

        case WM_INITMENUPOPUP:
            // TODO: should I just build the menu from scratch every time?
            UpdateAppMenu(win, (HMENU)wp);
            break;

        case WM_COMMAND:
            return FrameOnCommand(win, hwnd, msg, wp, lp);

        case WM_MEASUREITEM:
            if (gOwnerDrawMenu) {
                MenuOwnerDrawnMesureItem(hwnd, (MEASUREITEMSTRUCT*)lp);
                return TRUE;
            }
            break;

        case WM_DRAWITEM:
            if (gOwnerDrawMenu) {
                MenuOwnerDrawnDrawItem(hwnd, (DRAWITEMSTRUCT*)lp);
                return TRUE;
            }
            break;

        case WM_ACTIVATE:
            if (wp != WA_INACTIVE) {
                gLastActiveFrameHwnd = hwnd;
            }
            break;

        case WM_APPCOMMAND:
            // both keyboard and mouse drivers should produce WM_APPCOMMAND
            // messages for their special keys, so handle these here and return
            // TRUE so as to not make them bubble up further
            switch (GET_APPCOMMAND_LPARAM(lp)) {
                case APPCOMMAND_BROWSER_BACKWARD:
                    HwndSendCommand(hwnd, CmdGoToNavBack);
                    return TRUE;
                case APPCOMMAND_BROWSER_FORWARD:
                    HwndSendCommand(hwnd, CmdGoToNavForward);
                    return TRUE;
                case APPCOMMAND_BROWSER_REFRESH:
                    HwndSendCommand(hwnd, CmdRefresh);
                    return TRUE;
                case APPCOMMAND_BROWSER_SEARCH:
                    HwndSendCommand(hwnd, CmdFindFirst);
                    return TRUE;
                case APPCOMMAND_BROWSER_FAVORITES:
                    HwndSendCommand(hwnd, CmdViewBookmarks);
                    return TRUE;
            }
            return DefWindowProc(hwnd, msg, wp, lp);

        case WM_CHAR:
            if (win) {
                FrameOnChar(win, wp, lp);
            }
            break;

        case WM_KEYDOWN:
            if (win) {
                FrameOnKeydown(win, wp, lp);
            }
            break;

        case WM_SYSKEYUP:
            // pressing and releasing the Alt key focuses the menu even if
            // the wheel has been used for scrolling horizontally, so we
            // have to suppress that effect explicitly in this situation
            if (VK_MENU == wp && gSuppressAltKey) {
                gSuppressAltKey = false;
                return 0;
            }
            return DefWindowProc(hwnd, msg, wp, lp);

        case WM_SYSCHAR:
            if (win && FrameOnSysChar(win, wp)) {
                return 0;
            }
            return DefWindowProc(hwnd, msg, wp, lp);

        case WM_SYSCOMMAND:
            // temporarily show the menu bar if it has been hidden
            if (wp == SC_KEYMENU && win && win->isMenuHidden) {
                ShowHideMenuBar(win, true);
            }
            return DefWindowProc(hwnd, msg, wp, lp);

        case WM_EXITMENULOOP:
            // hide the menu bar again if it was shown only temporarily
            if (!wp && win && win->isMenuHidden) {
                SetMenu(hwnd, nullptr);
            }
            return DefWindowProc(hwnd, msg, wp, lp);

        case WM_CONTEXTMENU: {
            // opening the context menu with a keyboard doesn't call the canvas'
            // WM_CONTEXTMENU, as it never has the focus (mouse right-clicks are
            // handled as expected)
            int x = GET_X_LPARAM(lp);
            int y = GET_Y_LPARAM(lp);
            if (win && (x == -1) && (y == -1) && !IsFocused(win->tocTreeCtrl->hwnd)) {
                return SendMessageW(win->hwndCanvas, WM_CONTEXTMENU, wp, lp);
            }
            return DefWindowProc(hwnd, msg, wp, lp);
        }

        case WM_SETTINGCHANGE:
        InitMouseWheelInfo:
            UpdateDeltaPerLine();

            if (win) {
                // in tablets it's possible to rotate the screen. if we're
                // in full screen, resize our window to match new screen size
                if (win->presentation) {
                    EnterFullScreen(win, true);
                } else if (win->isFullScreen) {
                    EnterFullScreen(win, false);
                }
            }

            return 0;

        case WM_SYSCOLORCHANGE:
            if (gGlobalPrefs->useSysColors) {
                UpdateDocumentColors();
            }
            break;

        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
            if (!win || !win->IsDocLoaded()) {
                break;
            }
            if (win->AsChm()) {
                return win->AsChm()->PassUIMsg(msg, wp, lp);
            }
            CrashIf(!win->AsFixed());
            // Pass the message to the canvas' window procedure
            // (required since the canvas itself never has the focus and thus
            // never receives WM_MOUSEWHEEL messages)
            return SendMessageW(win->hwndCanvas, msg, wp, lp);

        case WM_CLOSE:
            if (CanCloseWindow(win)) {
                CloseWindow(win, true, false);
            }
            break;

        case WM_DESTROY: {
            /* WM_DESTROY is generated by windows when close button is pressed
               or if we explicitly call DestroyWindow()
               It might be sent as a result of File\Close, in which
               case CloseWindow() has already been called. */
            FreeMenuOwnerDrawInfoData(GetMenu(hwnd));
            if (win) {
                CloseWindow(win, true, true);
            }
        } break;

        case WM_ENDSESSION:
            // TODO: check for unfinished print jobs in WM_QUERYENDSESSION?
            if (wp == TRUE) {
                prefs::Save();
                // we must quit so that we restore opened files on start.
                DestroyWindow(hwnd);
            }
            return 0;

        case WM_DDE_INITIATE:
            if (gPluginMode) {
                break;
            }
            return OnDDEInitiate(hwnd, wp, lp);
        case WM_DDE_EXECUTE:
            return OnDDExecute(hwnd, wp, lp);
        case WM_DDE_TERMINATE:
            return OnDDETerminate(hwnd, wp, lp);

        case WM_COPYDATA:
            return OnCopyData(hwnd, wp, lp);

        case WM_TIMER:
            if (win && win->stressTest) {
                OnStressTestTimer(win, (int)wp);
            }
            break;

        case WM_MOUSEACTIVATE:
            if (win && win->presentation && hwnd != GetForegroundWindow()) {
                return MA_ACTIVATEANDEAT;
            }
            return MA_ACTIVATE;

        case WM_ERASEBKGND:
            return TRUE;

        default:
            return DefWindowProc(hwnd, msg, wp, lp);
    }
    return 0;
}

static TempStr GetFileSizeAsStrTemp(std::string_view path) {
    i64 fileSize = file::GetSize(path);
    AutoFreeWstr fileSizeStr = FormatFileSizeNoTrans(fileSize);
    return ToUtf8Temp(fileSizeStr);
}

void GetProgramInfo(str::Str& s) {
    auto d = ToUtf8Temp(gCrashFilePath);
    s.AppendFmt("Crash file: %s\r\n", d.Get());

    auto exePathW = GetExePathTemp();
    auto exePath = ToUtf8Temp(exePathW.AsView());
    auto fileSizeExe = GetFileSizeAsStrTemp(exePath.AsView());
    s.AppendFmt("Exe: %s %s\r\n", exePath.Get(), fileSizeExe.Get());
    if (IsDllBuild()) {
        // show the size of the dll so that we can verify it's the
        // correct size for the given version
        AutoFreeStr dir = path::GetDir(exePath);
        AutoFreeStr dllPath = path::Join(dir.Get(), "libmupdf.dll", nullptr);
        auto fileSizeDll = GetFileSizeAsStrTemp(dllPath.AsView());
        s.AppendFmt("Dll: %s %s\r\n", dllPath.Get(), fileSizeDll.Get());
    }

    const char* exeType = IsDllBuild() ? "dll" : "static";
    if (builtOn != nullptr) {
        s.AppendFmt("BuiltOn: %s\n", builtOn);
    }
    s.AppendFmt("Type: %s\r\n", exeType);
    s.AppendFmt("Ver: %s", currentVersion);
    if (gIsPreReleaseBuild) {
        s.AppendFmt(" pre-release");
    }
    if (IsProcess64()) {
        s.Append(" 64-bit");
    }
    if (gIsDebugBuild) {
        if (!str::Find(s.Get(), " (dbg)")) {
            s.Append(" (dbg)");
        }
    }
    if (gPluginMode) {
        s.Append(" [plugin]");
    }
    s.Append("\r\n");

    if (gitSha1 != nullptr) {
        s.AppendFmt("Git: %s (https://github.com/sumatrapdfreader/sumatrapdf/commit/%s)\r\n", gitSha1, gitSha1);
    }
}

bool CrashHandlerCanUseNet() {
    return HasPermission(Perm::InternetAccess);
}

void ShowCrashHandlerMessage() {
    log("ShowCrashHandlerMessage()\n");
    // don't show a message box in restricted use, as the user most likely won't be
    // able to do anything about it anyway and it's up to the application provider
    // to fix the unexpected behavior (of which for a restricted set of documents
    // there should be much less, anyway)
    if (!HasPermission(Perm::DiskAccess)) {
        log("ShowCrashHandlerMessage: skipping beacuse !HasPermission(Perm::DiskAccess)\n");
        return;
    }

#if 0
    int res = MessageBox(nullptr, _TR("Sorry, that shouldn't have happened!\n\nPlease press 'Cancel', if you want to help us fix the cause of this crash."), _TR("SumatraPDF crashed"), MB_ICONERROR | MB_OKCANCEL | MbRtlReadingMaybe());
    if (IDCANCEL == res) {
        LaunchBrowser(CRASH_REPORT_URL);
    }
#endif

    const char* msg = "We're sorry, SumatraPDF crashed.\n\nPress 'Cancel' to see crash report.";
    uint flags = MB_ICONERROR | MB_OK | MB_OKCANCEL | MbRtlReadingMaybe();
    flags |= MB_SETFOREGROUND | MB_TOPMOST;

    int res = MessageBoxA(nullptr, msg, "SumatraPDF crashed", flags);
    if (IDCANCEL != res) {
        return;
    }
    if (!gCrashFilePath) {
        log("ShowCrashHandlerMessage: !gCrashFilePath\n");
        return;
    }
    LaunchFile(gCrashFilePath, nullptr, L"open");
    auto url = L"https://www.sumatrapdfreader.org/docs/Submit-crash-report.html";
    LaunchFile(url, nullptr, L"open");
}

static WCHAR* GetSymbolsDir() {
    if (IsRunningInPortableMode()) {
        /* Use the same path as the binary */
        return GetExeDir();
    }
    TempWstr dir = GetSpecialFolderTemp(CSIDL_LOCAL_APPDATA, true);
    return path::Join(dir.Get(), GetAppNameTemp(), L"crashinfo");
}

static void DownloadDebugSymbols() {
    // over-ride the default symbols directory to be more useful
    WCHAR* symDir = GetSymbolsDir();
    SetSymbolsDir(symDir);

    bool ok = CrashHandlerDownloadSymbols();
    char* msg = nullptr;
    if (ok) {
        auto symDirA = ToUtf8Temp(symDir);
        msg = str::Format("Downloaded symbols! to %s", symDirA.Get());
    } else {
        msg = str::Dup("Failed to download symbols.");
    }
    uint flags = MB_ICONINFORMATION | MB_OK | MbRtlReadingMaybe();
    MessageBoxA(nullptr, msg, "Downloading symbols", flags);

    free(msg);
    free(symDir);
}

void ShutdownCleanup() {
    gAllowedFileTypes.Reset();
    gAllowedLinkProtocols.Reset();
    extern void WindowCleanup();
    WindowCleanup();
}
