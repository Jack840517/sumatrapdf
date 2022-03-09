/* Copyright 2021 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

/* Code related to:
 * user-initiated search
 * DDE commands, including search
 */

#include "utils/BaseUtil.h"
#include "utils/ScopedWin.h"
#include "utils/FileUtil.h"
#include "utils/UITask.h"
#include "utils/WinUtil.h"
#include "utils/Log.h"

#include "wingui/TreeModel.h"
#include "DisplayMode.h"
#include "Controller.h"
#include "EngineBase.h"
#include "EngineAll.h"
#include "SettingsStructs.h"
#include "GlobalPrefs.h"
#include "ChmModel.h"
#include "DisplayModel.h"
#include "PdfSync.h"
#include "ProgressUpdateUI.h"
#include "TextSelection.h"
#include "TextSearch.h"
#include "Notifications.h"
#include "SumatraPDF.h"
#include "WindowInfo.h"
#include "TabInfo.h"
#include "resource.h"
#include "Commands.h"
#include "AppTools.h"
#include "SearchAndDDE.h"
#include "Selection.h"
#include "SumatraDialogs.h"
#include "Translations.h"

// open file command
//  format: [Open("<pdffilepath>"[,<newwindow>,<setfocus>,<forcerefresh>])]
//    if newwindow = 1 then a new window is created even if the file is already open
//    if focus = 1 then the focus is set to the window
//  eg: [Open("c:\file.pdf", 1, 1, 0)]

bool gIsStartup = false;
WStrVec gDdeOpenOnStartup;

Kind NG_FIND_PROGRESS = "findProgress";

// don't show the Search UI for document types that don't
// support extracting text and/or navigating to a specific
// text selection; default to showing it, since most users
// will never use a format that does not support search
bool NeedsFindUI(WindowInfo* win) {
    if (!win->IsDocLoaded()) {
        return true;
    }
    if (!win->AsFixed()) {
        return false;
    }
    if (win->AsFixed()->GetEngine()->IsImageCollection()) {
        return false;
    }
    return true;
}

void OnMenuFind(WindowInfo* win) {
    if (win->AsChm()) {
        win->AsChm()->FindInCurrentPage();
        return;
    }

    if (!win->AsFixed() || !NeedsFindUI(win)) {
        return;
    }

    // copy any selected text to the find bar, if it's still empty
    DisplayModel* dm = win->AsFixed();
    if (dm->textSelection->result.len > 0 && Edit_GetTextLength(win->hwndFindBox) == 0) {
        AutoFreeWstr selection(dm->textSelection->ExtractText(L" "));
        str::NormalizeWSInPlace(selection);
        if (!str::IsEmpty(selection.Get())) {
            win::SetText(win->hwndFindBox, selection);
            Edit_SetModify(win->hwndFindBox, TRUE);
        }
    }

    // Don't show a dialog if we don't have to - use the Toolbar instead
    if (gGlobalPrefs->showToolbar && !win->isFullScreen && !win->presentation) {
        if (IsFocused(win->hwndFindBox)) {
            SendMessageW(win->hwndFindBox, WM_SETFOCUS, 0, 0);
        } else {
            SetFocus(win->hwndFindBox);
        }
        return;
    }

    WCHAR* previousFind = win::GetTextTemp(win->hwndFindBox).Get();
    WORD state = (WORD)SendMessageW(win->hwndToolbar, TB_GETSTATE, CmdFindMatch, 0);
    bool matchCase = (state & TBSTATE_CHECKED) != 0;

    AutoFreeWstr findString(Dialog_Find(win->hwndFrame, previousFind, &matchCase));
    if (!findString) {
        return;
    }

    win::SetText(win->hwndFindBox, findString);
    Edit_SetModify(win->hwndFindBox, TRUE);

    bool matchCaseChanged = matchCase != (0 != (state & TBSTATE_CHECKED));
    if (matchCaseChanged) {
        if (matchCase) {
            state |= TBSTATE_CHECKED;
        } else {
            state &= ~TBSTATE_CHECKED;
        }
        SendMessageW(win->hwndToolbar, TB_SETSTATE, CmdFindMatch, state);
        dm->textSearch->SetSensitive(matchCase);
    }

    FindTextOnThread(win, TextSearchDirection::Forward, true);
}

void OnMenuFindNext(WindowInfo* win) {
    if (!win->IsDocLoaded() || !NeedsFindUI(win)) {
        return;
    }
    if (SendMessageW(win->hwndToolbar, TB_ISBUTTONENABLED, CmdFindNext, 0)) {
        FindTextOnThread(win, TextSearchDirection::Forward, true);
    }
}

void OnMenuFindPrev(WindowInfo* win) {
    if (!win->IsDocLoaded() || !NeedsFindUI(win)) {
        return;
    }
    if (SendMessageW(win->hwndToolbar, TB_ISBUTTONENABLED, CmdFindPrev, 0)) {
        FindTextOnThread(win, TextSearchDirection::Backward, true);
    }
}

void OnMenuFindMatchCase(WindowInfo* win) {
    if (!win->IsDocLoaded() || !NeedsFindUI(win)) {
        return;
    }
    WORD state = (WORD)SendMessageW(win->hwndToolbar, TB_GETSTATE, CmdFindMatch, 0);
    win->AsFixed()->textSearch->SetSensitive((state & TBSTATE_CHECKED) != 0);
    Edit_SetModify(win->hwndFindBox, TRUE);
}

void OnMenuFindSel(WindowInfo* win, TextSearchDirection direction) {
    if (!win->IsDocLoaded() || !NeedsFindUI(win)) {
        return;
    }
    DisplayModel* dm = win->AsFixed();
    if (!win->currentTab->selectionOnPage || 0 == dm->textSelection->result.len) {
        return;
    }

    AutoFreeWstr selection(dm->textSelection->ExtractText(L" "));
    str::NormalizeWSInPlace(selection);
    if (str::IsEmpty(selection.Get())) {
        return;
    }

    win::SetText(win->hwndFindBox, selection);
    AbortFinding(win, false); // cancel "find as you type"
    Edit_SetModify(win->hwndFindBox, FALSE);
    dm->textSearch->SetLastResult(dm->textSelection);

    FindTextOnThread(win, direction, true);
}

static void ShowSearchResult(WindowInfo* win, TextSel* result, bool addNavPt) {
    CrashIf(0 == result->len || !result->pages || !result->rects);
    if (0 == result->len || !result->pages || !result->rects) {
        return;
    }

    DisplayModel* dm = win->AsFixed();
    if (addNavPt || !dm->PageShown(result->pages[0]) ||
        (dm->GetZoomVirtual() == ZOOM_FIT_PAGE || dm->GetZoomVirtual() == ZOOM_FIT_CONTENT)) {
        win->ctrl->GoToPage(result->pages[0], addNavPt);
    }

    dm->textSelection->CopySelection(dm->textSearch);
    UpdateTextSelection(win, false);
    dm->ShowResultRectToScreen(result);
    RepaintAsync(win, 0);
}

void ClearSearchResult(WindowInfo* win) {
    DeleteOldSelectionInfo(win, true);
    RepaintAsync(win, 0);
}

static void UpdateFindStatusTask(WindowInfo* win, NotificationWnd* wnd, int current, int total) {
    if (!WindowInfoStillValid(win) || win->findCanceled) {
        return;
    }
    if (win->notifications->Contains(wnd)) {
        wnd->UpdateProgress(current, total);
    } else {
        // the search has been canceled by closing the notification
        win->findCanceled = true;
    }
}

struct FindThreadData : public ProgressUpdateUI {
    WindowInfo* win{nullptr};
    TextSearchDirection direction{TextSearchDirection::Forward};
    bool wasModified{false};
    AutoFreeWstr text;
    // owned by win->notifications, as FindThreadData
    // can be deleted before the notification times out
    NotificationWnd* wnd{nullptr};
    HANDLE thread{nullptr};

    FindThreadData(WindowInfo* win, TextSearchDirection direction, const WCHAR* text, bool wasModified) {
        this->win = win;
        this->direction = direction;
        this->text = str::Dup(text);
        this->wasModified = wasModified;
    }
    ~FindThreadData() override {
        CloseHandle(thread);
    }

    void ShowUI(bool showProgress) {
        const LPARAM disable = (LPARAM)MAKELONG(0, 0);

        if (showProgress) {
            auto notificationsInCb = this->win->notifications;
            wnd = new NotificationWnd(win->hwndCanvas, 0);
            wnd->wndRemovedCb = [notificationsInCb](NotificationWnd* wnd) {
                notificationsInCb->RemoveNotification(wnd);
            };
            wnd->Create(L"", _TR("Searching %d of %d..."));
            win->notifications->Add(wnd, NG_FIND_PROGRESS);
        }

        SendMessageW(win->hwndToolbar, TB_ENABLEBUTTON, CmdFindPrev, disable);
        SendMessageW(win->hwndToolbar, TB_ENABLEBUTTON, CmdFindNext, disable);
        SendMessageW(win->hwndToolbar, TB_ENABLEBUTTON, CmdFindMatch, disable);
    }

    void HideUI(bool success, bool loopedAround) const {
        LPARAM enable = (LPARAM)MAKELONG(1, 0);

        SendMessageW(win->hwndToolbar, TB_ENABLEBUTTON, CmdFindPrev, enable);
        SendMessageW(win->hwndToolbar, TB_ENABLEBUTTON, CmdFindNext, enable);
        SendMessageW(win->hwndToolbar, TB_ENABLEBUTTON, CmdFindMatch, enable);

        if (!win->notifications->Contains(wnd)) {
            /* our notification has been replaced or closed (or never created) */;
        } else if (!success && !loopedAround) {
            // i.e. canceled
            win->notifications->RemoveNotification(wnd);
        } else if (!success && loopedAround) {
            wnd->UpdateMessage(_TR("No matches were found"), 3000);
        } else {
            AutoFreeWstr label(win->ctrl->GetPageLabel(win->AsFixed()->textSearch->GetSearchHitStartPageNo()));
            AutoFreeWstr buf(str::Format(_TR("Found text at page %s"), label.Get()));
            if (loopedAround) {
                buf.Set(str::Format(_TR("Found text at page %s (again)"), label.Get()));
                MessageBeep(MB_ICONINFORMATION);
            }
            wnd->UpdateMessage(buf, 3000, loopedAround);
        }
    }

    void UpdateProgress(int current, int total) override {
        if (!wnd || WasCanceled()) {
            return;
        }
        uitask::Post([=] { UpdateFindStatusTask(win, wnd, current, total); });
    }

    bool WasCanceled() override {
        return !WindowInfoStillValid(win) || win->findCanceled;
    }
};

static void FindEndTask(WindowInfo* win, FindThreadData* ftd, TextSel* textSel, bool wasModifiedCanceled,
                        bool loopedAround) {
    if (!WindowInfoStillValid(win)) {
        delete ftd;
        return;
    }
    if (win->findThread != ftd->thread) {
        // Race condition: FindTextOnThread/AbortFinding was
        // called after the previous find thread ended but
        // before this FindEndTask could be executed
        delete ftd;
        return;
    }
    if (!win->IsDocLoaded()) {
        // the UI has already been disabled and hidden
    } else if (textSel) {
        ShowSearchResult(win, textSel, wasModifiedCanceled);
        ftd->HideUI(true, loopedAround);
    } else {
        // nothing found or search canceled
        ClearSearchResult(win);
        ftd->HideUI(false, !wasModifiedCanceled);
    }
    win->findThread = nullptr;
    delete ftd;
}

static DWORD WINAPI FindThread(LPVOID data) {
    FindThreadData* ftd = (FindThreadData*)data;
    CrashIf(!(ftd && ftd->win && ftd->win->ctrl && ftd->win->ctrl->AsFixed()));
    WindowInfo* win = ftd->win;
    DisplayModel* dm = win->AsFixed();

    TextSel* rect;
    dm->textSearch->SetDirection(ftd->direction);
    if (ftd->wasModified || !win->ctrl->ValidPageNo(dm->textSearch->GetCurrentPageNo()) ||
        !dm->GetPageInfo(dm->textSearch->GetCurrentPageNo())->visibleRatio) {
        rect = dm->textSearch->FindFirst(win->ctrl->CurrentPageNo(), ftd->text, ftd);
    } else {
        rect = dm->textSearch->FindNext(ftd);
    }

    bool loopedAround = false;
    if (!win->findCanceled && !rect) {
        // With no further findings, start over (unless this was a new search from the beginning)
        int startPage = (TextSearchDirection::Forward == ftd->direction) ? 1 : win->ctrl->PageCount();
        if (!ftd->wasModified || win->ctrl->CurrentPageNo() != startPage) {
            loopedAround = true;
            rect = dm->textSearch->FindFirst(startPage, ftd->text, ftd);
        }
    }

    // wait for FindTextOnThread to return so that
    // FindEndTask closes the correct handle to
    // the current find thread
    while (!win->findThread) {
        Sleep(1);
    }

    if (!win->findCanceled && rect) {
        uitask::Post([=] { FindEndTask(win, ftd, rect, ftd->wasModified, loopedAround); });
    } else {
        uitask::Post([=] { FindEndTask(win, ftd, nullptr, win->findCanceled, false); });
    }
    DestroyTempAllocator();
    return 0;
}

void AbortFinding(WindowInfo* win, bool hideMessage) {
    if (win->findThread) {
        win->findCanceled = true;
        WaitForSingleObject(win->findThread, INFINITE);
    }
    win->findCanceled = false;

    if (hideMessage) {
        win->notifications->RemoveForGroup(NG_FIND_PROGRESS);
    }
}

// wasModified
//   if true, starting a search for new term
//   if false, searching for the next occurence of previous term
// TODO: should detect wasModified by comparing with the last search result
void FindTextOnThread(WindowInfo* win, TextSearchDirection direction, const WCHAR* text, bool wasModified,
                      bool showProgress) {
    AbortFinding(win, true);
    if (str::IsEmpty(text)) {
        return;
    }
    FindThreadData* ftd = new FindThreadData(win, direction, text, wasModified);
    ftd->ShowUI(showProgress);
    win->findThread = nullptr;
    win->findThread = CreateThread(nullptr, 0, FindThread, ftd, 0, nullptr);
    ftd->thread = win->findThread; // safe because only accesssed on ui thread
}

void FindTextOnThread(WindowInfo* win, TextSearchDirection direction, bool showProgress) {
    WCHAR* text = win::GetTextTemp(win->hwndFindBox).Get();
    bool wasModified = Edit_GetModify(win->hwndFindBox);
    Edit_SetModify(win->hwndFindBox, FALSE);
    FindTextOnThread(win, direction, text, wasModified, showProgress);
}

void PaintForwardSearchMark(WindowInfo* win, HDC hdc) {
    CrashIf(!win->AsFixed());
    DisplayModel* dm = win->AsFixed();
    int pageNo = win->fwdSearchMark.page;
    PageInfo* pageInfo = dm->GetPageInfo(pageNo);
    if (!pageInfo || 0.0 == pageInfo->visibleRatio) {
        return;
    }

    int hiLiWidth = gGlobalPrefs->forwardSearch.highlightWidth;
    int hiLiOff = gGlobalPrefs->forwardSearch.highlightOffset;

    // Draw the rectangles highlighting the forward search results
    Vec<Rect> rects;
    for (size_t i = 0; i < win->fwdSearchMark.rects.size(); i++) {
        Rect rect = win->fwdSearchMark.rects.at(i);
        rect = dm->CvtToScreen(pageNo, ToRectF(rect));
        if (hiLiOff > 0) {
            float zoom = dm->GetZoomReal(pageNo);
            rect.x = std::max(pageInfo->pageOnScreen.x, 0) + (int)(hiLiOff * zoom);
            rect.dx = (int)((hiLiWidth > 0 ? hiLiWidth : 15.0) * zoom);
            rect.y -= 4;
            rect.dy += 8;
        }
        rects.Append(rect);
    }

    BYTE alpha = (BYTE)(0x5f * 1.0f * (HIDE_FWDSRCHMARK_STEPS - win->fwdSearchMark.hideStep) / HIDE_FWDSRCHMARK_STEPS);
    ParsedColor* parsedCol = GetPrefsColor(gGlobalPrefs->forwardSearch.highlightColor);
    PaintTransparentRectangles(hdc, win->canvasRc, rects, parsedCol->col, alpha, 0);
}

// returns true if the double-click was handled and false if it wasn't
bool OnInverseSearch(WindowInfo* win, int x, int y) {
    if (!HasPermission(Perm::DiskAccess) || gPluginMode) {
        return false;
    }
    TabInfo* tab = win->currentTab;
    if (!tab || tab->GetEngineType() != kindEngineMupdf) {
        return false;
    }
    DisplayModel* dm = tab->AsFixed();

    // Clear the last forward-search result
    win->fwdSearchMark.rects.Reset();
    InvalidateRect(win->hwndCanvas, nullptr, FALSE);

    // On double-clicking error message will be shown to the user
    // if the PDF does not have a synchronization file
    if (!dm->pdfSync) {
        int err = Synchronizer::Create(tab->filePath, dm->GetEngine(), &dm->pdfSync);
        if (err == PDFSYNCERR_SYNCFILE_NOTFOUND) {
            // We used to warn that "No synchronization file found" at this
            // point if gGlobalPrefs->enableTeXEnhancements is set; we no longer
            // so do because a double-click has several other meanings
            // (selecting a word or an image, navigating quickly using links)
            // and showing an unrelated warning in all those cases seems wrong
            return false;
        }
        if (err != PDFSYNCERR_SUCCESS) {
            win->ShowNotification(_TR("Synchronization file cannot be opened"));
            return true;
        }
        gGlobalPrefs->enableTeXEnhancements = true;
    }

    int pageNo = dm->GetPageNoByPoint(Point(x, y));
    if (!tab->ctrl->ValidPageNo(pageNo)) {
        return false;
    }

    Point pt = ToPoint(dm->CvtFromScreen(Point(x, y), pageNo));
    AutoFreeWstr srcfilepath;
    uint line, col;
    int err = dm->pdfSync->DocToSource(pageNo, pt, srcfilepath, &line, &col);
    if (err != PDFSYNCERR_SUCCESS) {
        win->ShowNotification(_TR("No synchronization info at this position"));
        return true;
    }

    if (!file::Exists(srcfilepath)) {
        // if the source file is missing, check if it's been moved to the same place as
        // the PDF document (which happens if all files are moved together)
        AutoFreeWstr altsrcpath(path::GetDir(tab->filePath));
        altsrcpath.Set(path::Join(altsrcpath, path::GetBaseNameTemp(srcfilepath)));
        if (!str::Eq(altsrcpath, srcfilepath) && file::Exists(altsrcpath)) {
            srcfilepath.Set(altsrcpath.StealData());
        }
    }

    WCHAR* inverseSearch = ToWstrTemp(gGlobalPrefs->inverseSearchCmdLine);
    WCHAR* toFree{nullptr};
    if (!inverseSearch) {
        // Detect a text editor and use it as the default inverse search handler for now
        inverseSearch = AutoDetectInverseSearchCommands(nullptr);
        toFree = inverseSearch;
    }

    AutoFreeWstr cmdline;
    if (inverseSearch) {
        cmdline.Set(dm->pdfSync->PrepareCommandline(inverseSearch, srcfilepath, line, col));
    }
    if (!str::IsEmpty(cmdline.Get())) {
        // resolve relative paths with relation to SumatraPDF.exe's directory
        AutoFreeWstr appDir = GetExeDir();
        AutoCloseHandle process(LaunchProcess(cmdline, appDir));
        if (!process) {
            win->ShowNotification(
                _TR("Cannot start inverse search command. Please check the command line in the settings."));
        }
    } else if (gGlobalPrefs->enableTeXEnhancements) {
        win->ShowNotification(
            _TR("Cannot start inverse search command. Please check the command line in the settings."));
    }

    if (toFree) {
        str::Free(toFree);
    }

    return true;
}

// Show the result of a PDF forward-search synchronization (initiated by a DDE command)
void ShowForwardSearchResult(WindowInfo* win, const WCHAR* fileName, uint line, uint /* col */, uint ret, uint page,
                             Vec<Rect>& rects) {
    CrashIf(!win->AsFixed());
    DisplayModel* dm = win->AsFixed();
    win->fwdSearchMark.rects.Reset();
    const PageInfo* pi = dm->GetPageInfo(page);
    if ((ret == PDFSYNCERR_SUCCESS) && (rects.size() > 0) && (nullptr != pi)) {
        // remember the position of the search result for drawing the rect later on
        win->fwdSearchMark.rects = rects;
        win->fwdSearchMark.page = page;
        win->fwdSearchMark.show = true;
        win->fwdSearchMark.hideStep = 0;
        if (!gGlobalPrefs->forwardSearch.highlightPermanent) {
            SetTimer(win->hwndCanvas, HIDE_FWDSRCHMARK_TIMER_ID, HIDE_FWDSRCHMARK_DELAY_IN_MS, nullptr);
        }

        // Scroll to show the overall highlighted zone
        int pageNo = page;
        Rect overallrc = rects.at(0);
        for (size_t i = 1; i < rects.size(); i++) {
            overallrc = overallrc.Union(rects.at(i));
        }
        TextSel res = {1, 1, &pageNo, &overallrc};
        if (!dm->PageVisible(page)) {
            win->ctrl->GoToPage(page, true);
        }
        if (!dm->ShowResultRectToScreen(&res)) {
            RepaintAsync(win, 0);
        }
        if (IsIconic(win->hwndFrame)) {
            ShowWindowAsync(win->hwndFrame, SW_RESTORE);
        }
        return;
    }

    AutoFreeWstr buf;
    if (ret == PDFSYNCERR_SYNCFILE_NOTFOUND) {
        win->ShowNotification(_TR("No synchronization file found"));
    } else if (ret == PDFSYNCERR_SYNCFILE_CANNOT_BE_OPENED) {
        win->ShowNotification(_TR("Synchronization file cannot be opened"));
    } else if (ret == PDFSYNCERR_INVALID_PAGE_NUMBER) {
        buf.Set(str::Format(_TR("Page number %u inexistant"), page));
    } else if (ret == PDFSYNCERR_NO_SYNC_AT_LOCATION) {
        win->ShowNotification(_TR("No synchronization info at this position"));
    } else if (ret == PDFSYNCERR_UNKNOWN_SOURCEFILE) {
        buf.Set(str::Format(_TR("Unknown source file (%s)"), fileName));
    } else if (ret == PDFSYNCERR_NORECORD_IN_SOURCEFILE) {
        buf.Set(str::Format(_TR("Source file %s has no synchronization point"), fileName));
    } else if (ret == PDFSYNCERR_NORECORD_FOR_THATLINE) {
        buf.Set(str::Format(_TR("No result found around line %u in file %s"), line, fileName));
    } else if (ret == PDFSYNCERR_NOSYNCPOINT_FOR_LINERECORD) {
        buf.Set(str::Format(_TR("No result found around line %u in file %s"), line, fileName));
    }
    if (buf) {
        win->ShowNotification(buf);
    }
}

// DDE commands handling

LRESULT OnDDEInitiate(HWND hwnd, WPARAM wp, LPARAM lp) {
    ATOM aServer = GlobalAddAtom(PDFSYNC_DDE_SERVICE);
    ATOM aTopic = GlobalAddAtom(PDFSYNC_DDE_TOPIC);

    if (LOWORD(lp) == aServer && HIWORD(lp) == aTopic) {
        SendMessageW((HWND)wp, WM_DDE_ACK, (WPARAM)hwnd, MAKELPARAM(aServer, 0));
    } else {
        GlobalDeleteAtom(aServer);
        GlobalDeleteAtom(aTopic);
    }
    return 0;
}

// DDE commands

/*
Forward search (synchronization) DDE command

[ForwardSearch(["<pdffilepath>",]"<sourcefilepath>",<line>,<column>[,<newwindow>, <setfocus>])]
eg:
[ForwardSearch("c:\file.pdf","c:\folder\source.tex",298,0)]

if pdffilepath is provided, the file will be opened if no open window can be found for it
if newwindow = 1 then a new window is created even if the file is already open
if focus = 1 then the focus is set to the window
*/
static const WCHAR* HandleSyncCmd(const WCHAR* cmd, DDEACK& ack) {
    AutoFreeWstr pdfFile, srcFile;
    BOOL line = 0, col = 0, newWindow = 0, setFocus = 0;
    const WCHAR* next = str::Parse(cmd, L"[ForwardSearch(\"%S\",%? \"%S\",%u,%u)]", &pdfFile, &srcFile, &line, &col);
    if (!next) {
        next = str::Parse(cmd, L"[ForwardSearch(\"%S\",%? \"%S\",%u,%u,%u,%u)]", &pdfFile, &srcFile, &line, &col,
                          &newWindow, &setFocus);
    }
    // allow to omit the pdffile path, so that editors don't have to know about
    // multi-file projects (requires that the PDF has already been opened)
    if (!next) {
        pdfFile.Reset();
        next = str::Parse(cmd, L"[ForwardSearch(\"%S\",%u,%u)]", &srcFile, &line, &col);
        if (!next) {
            next =
                str::Parse(cmd, L"[ForwardSearch(\"%S\",%u,%u,%u,%u)]", &srcFile, &line, &col, &newWindow, &setFocus);
        }
    }

    if (!next) {
        return nullptr;
    }

    WindowInfo* win = nullptr;
    if (pdfFile) {
        // check if the PDF is already opened
        win = FindWindowInfoByFile(pdfFile, !newWindow);
        // if not then open it
        if (newWindow || !win) {
            LoadArgs args(pdfFile, !newWindow ? win : nullptr);
            win = LoadDocument(args);
        } else if (!win->IsDocLoaded()) {
            ReloadDocument(win, false);
        }
    } else {
        // check if any opened PDF has sync information for the source file
        win = FindWindowInfoBySyncFile(srcFile, true);
        if (win && newWindow) {
            LoadArgs args(win->currentTab->filePath, nullptr);
            win = LoadDocument(args);
        }
    }

    if (!win || !win->currentTab || win->currentTab->GetEngineType() != kindEngineMupdf) {
        return next;
    }

    DisplayModel* dm = win->AsFixed();
    if (!dm->pdfSync) {
        return next;
    }

    ack.fAck = 1;
    uint page;
    Vec<Rect> rects;
    int ret = dm->pdfSync->SourceToDoc(srcFile, line, col, &page, rects);
    ShowForwardSearchResult(win, srcFile, line, col, ret, page, rects);
    if (setFocus) {
        win->Focus();
    }

    return next;
}

/*
Search DDE command

[Search("<pdffile>","<search-term>")]
*/
static const WCHAR* HandleSearchCmd(const WCHAR* cmd, DDEACK& ack) {
    AutoFreeWstr pdfFile;
    AutoFreeWstr term;
    const WCHAR* next = str::Parse(cmd, L"[Search(\"%S\",\"%s\")]", &pdfFile, &term);
    // TODO: should un-quote text to allow searching text with '"' in them
    if (!next) {
        return nullptr;
    }
    if (str::IsEmpty(term.Get())) {
        return next;
    }
    // check if the PDF is already opened
    // TODO: prioritize window with HWND so that if we have the same file
    // opened in multiple tabs / windows, we operate on the one that got the message
    WindowInfo* win = FindWindowInfoByFile(pdfFile, true);
    if (!win) {
        return next;
    }
    if (!win->IsDocLoaded()) {
        ReloadDocument(win, false);
        if (!win->IsDocLoaded()) {
            return next;
        }
    }
    ack.fAck = 1;
    FindTextOnThread(win, TextSearchDirection::Forward, term, true /* wasModified*/, true /*showProgress*/);
    win->Focus();
    return next;
}

/*
Open file DDE Command

[Open("<pdffilepath>"[,<newwindow>,<setfocus>,<forcerefresh>])]
*/
static const WCHAR* HandleOpenCmd(const WCHAR* cmd, DDEACK& ack) {
    AutoFreeWstr pdfFile;
    int newWindow = 0;
    BOOL setFocus = 0;
    BOOL forceRefresh = 0;
    const WCHAR* next = str::Parse(cmd, L"[Open(\"%S\")]", &pdfFile);
    if (!next) {
        const WCHAR* pat = L"[Open(\"%S\",%u,%u,%u)]";
        next = str::Parse(cmd, pat, &pdfFile, &newWindow, &setFocus, &forceRefresh);
    }
    if (!next) {
        return nullptr;
    }

    bool focusTab = (newWindow == 0);
    WindowInfo* win = nullptr;
    if (newWindow == 2) {
        // TODO: don't do it if we have a about window
        win = CreateAndShowWindowInfo(nullptr);
    }

    // on startup this is called while LoadDocument is in progress, which causes
    // all sort of mayhem. Queue files to be loaded in a sequence
    if (gIsStartup) {
        gDdeOpenOnStartup.Append(pdfFile.StealData());
        return next;
    }

    if (win == nullptr) {
        win = FindWindowInfoByFile(pdfFile, focusTab);
    }
    if (newWindow || !win) {
        // https://github.com/sumatrapdfreader/sumatrapdf/issues/2315
        // open in the last active window
        if (win == nullptr) {
            win = FindWindowInfoByHwnd(gLastActiveFrameHwnd);
        }
        LoadArgs args(pdfFile, win);
        win = LoadDocument(args);
    } else if (!win->IsDocLoaded()) {
        ReloadDocument(win, false);
        forceRefresh = 0;
    }

    // TODO: not sure why this triggers. Seems to happen when opening multiple files
    // via Open menu in explorer. The first one is opened via cmd-line arg, the
    // rest via DDE.
    // CrashIf(win && win->IsAboutWindow());
    if (!win) {
        return next;
    }

    ack.fAck = 1;
    if (forceRefresh) {
        ReloadDocument(win, true);
    }
    if (setFocus) {
        win->Focus();
    }

    return next;
}

/*
DDE command: jump to named destination in an already opened document.

[GoToNamedDest("<pdffilepath>","<destination name>")]
e.g.:
[GoToNamedDest("c:\file.pdf", "chapter.1")]
*/
static const WCHAR* HandleGotoCmd(const WCHAR* cmd, DDEACK& ack) {
    AutoFreeWstr pdfFile, destName;
    const WCHAR* next = str::Parse(cmd, L"[GotoNamedDest(\"%S\",%? \"%S\")]", &pdfFile, &destName);
    if (!next) {
        return nullptr;
    }

    WindowInfo* win = FindWindowInfoByFile(pdfFile, true);
    if (!win) {
        return next;
    }
    if (!win->IsDocLoaded()) {
        ReloadDocument(win, false);
        if (!win->IsDocLoaded()) {
            return next;
        }
    }

    win->linkHandler->GotoNamedDest(destName);
    ack.fAck = 1;
    win->Focus();
    return next;
}

/*
DDE command: jump to a page in an already opened document.

[GoToPage("<pdffilepath>",<page number>)]
eg:
[GoToPage("c:\file.pdf",37)]
*/
#define DDECOMMAND_PAGE L"GotoPage"

static const WCHAR* HandlePageCmd(__unused HWND hwnd, const WCHAR* cmd, DDEACK& ack) {
    AutoFreeWstr pdfFile;
    uint page = 0;
    const WCHAR* next = str::Parse(cmd, L"[GotoPage(\"%S\",%u)]", &pdfFile, &page);
    if (!next) {
        return nullptr;
    }

    // check if the PDF is already opened
    // TODO: prioritize window with HWND so that if we have the same file
    // opened in multiple tabs / windows, we operate on the one that got the message
    WindowInfo* win = FindWindowInfoByFile(pdfFile, true);
    if (!win) {
        return next;
    }
    if (!win->IsDocLoaded()) {
        ReloadDocument(win, false);
        if (!win->IsDocLoaded()) {
            return next;
        }
    }

    if (!win->ctrl->ValidPageNo(page)) {
        return next;
    }

    win->ctrl->GoToPage(page, true);
    ack.fAck = 1;
    win->Focus();
    return next;
}

/*
Set view mode and zoom level DDE command

[SetView("<pdffilepath>", "<view mode>", <zoom level>[, <scrollX>, <scrollY>])]
eg:
[SetView("c:\file.pdf", "book view", -2)]

use -1 for ZOOM_FIT_PAGE, -2 for ZOOM_FIT_WIDTH and -3 for ZOOM_FIT_CONTENT
*/
static const WCHAR* HandleSetViewCmd(const WCHAR* cmd, DDEACK& ack) {
    AutoFreeWstr pdfFile, viewMode;
    float zoom = INVALID_ZOOM;
    Point scroll(-1, -1);
    const WCHAR* next = str::Parse(cmd, L"[SetView(\"%S\",%? \"%S\",%f)]", &pdfFile, &viewMode, &zoom);
    if (!next) {
        next =
            str::Parse(cmd, L"[SetView(\"%S\",%? \"%S\",%f,%d,%d)]", &pdfFile, &viewMode, &zoom, &scroll.x, &scroll.y);
    }
    if (!next) {
        return nullptr;
    }

    WindowInfo* win = FindWindowInfoByFile(pdfFile, true);
    if (!win) {
        return next;
    }
    if (!win->IsDocLoaded()) {
        ReloadDocument(win, false);
        if (!win->IsDocLoaded()) {
            return next;
        }
    }

    auto viewModeWstr = ToUtf8Temp(viewMode);
    DisplayMode mode = DisplayModeFromString(viewModeWstr.Get(), DisplayMode::Automatic);
    if (mode != DisplayMode::Automatic) {
        SwitchToDisplayMode(win, mode);
    }

    if (zoom != INVALID_ZOOM) {
        ZoomToSelection(win, zoom);
    }

    if ((scroll.x != -1 || scroll.y != -1) && win->AsFixed()) {
        DisplayModel* dm = win->AsFixed();
        ScrollState ss = dm->GetScrollState();
        ss.x = scroll.x;
        ss.y = scroll.y;
        dm->SetScrollState(ss);
    }

    ack.fAck = 1;
    return next;
}

static void HandleDdeCmds(HWND hwnd, const WCHAR* cmd, DDEACK& ack) {
    while (!str::IsEmpty(cmd)) {
        {
            auto tmp = ToUtf8Temp(cmd);
            logf("HandleDdeCmds: '%s'\n", tmp.Get());
        }

        const WCHAR* nextCmd = HandleSyncCmd(cmd, ack);
        if (!nextCmd) {
            nextCmd = HandleOpenCmd(cmd, ack);
        }
        if (!nextCmd) {
            nextCmd = HandleGotoCmd(cmd, ack);
        }
        if (!nextCmd) {
            nextCmd = HandlePageCmd(hwnd, cmd, ack);
        }
        if (!nextCmd) {
            nextCmd = HandleSetViewCmd(cmd, ack);
        }
        if (!nextCmd) {
            nextCmd = HandleSearchCmd(cmd, ack);
        }
        if (!nextCmd) {
            AutoFreeWstr tmp;
            nextCmd = str::Parse(cmd, L"%S]", &tmp);
        }
        cmd = nextCmd;
    }
}

LRESULT OnDDExecute(HWND hwnd, WPARAM wp, LPARAM lp) {
    UINT_PTR lo = 0, hi = 0;
    if (!UnpackDDElParam(WM_DDE_EXECUTE, lp, &lo, &hi)) {
        return 0;
    }

    DDEACK ack = {0};
    LPVOID command = GlobalLock((HGLOBAL)hi);
    if (!command) {
        return 0;
    }

    AutoFreeWstr cmd;
    if (IsWindowUnicode((HWND)wp)) {
        cmd = str::Dup((WCHAR*)command);
    } else {
        cmd = strconv::AnsiToWstr((const char*)command);
    }
    HandleDdeCmds(hwnd, cmd, ack);
    GlobalUnlock((HGLOBAL)hi);

    lp = ReuseDDElParam(lp, WM_DDE_EXECUTE, WM_DDE_ACK, *(WORD*)&ack, hi);
    PostMessageW((HWND)wp, WM_DDE_ACK, (WPARAM)hwnd, lp);
    return 0;
}

LRESULT OnDDETerminate(HWND hwnd, WPARAM wp, __unused LPARAM lp) {
    // Respond with another WM_DDE_TERMINATE message
    PostMessageW((HWND)wp, WM_DDE_TERMINATE, (WPARAM)hwnd, 0L);
    return 0;
}

LRESULT OnCopyData(__unused HWND hwnd, WPARAM wp, LPARAM lp) {
    COPYDATASTRUCT* cds = (COPYDATASTRUCT*)lp;
    if (!cds || cds->dwData != 0x44646557 /* DdeW */ || wp) {
        return FALSE;
    }

    const WCHAR* cmd = (const WCHAR*)cds->lpData;
    if (cmd[cds->cbData / sizeof(WCHAR) - 1]) {
        return FALSE;
    }

    DDEACK ack = {0};
    HandleDdeCmds(hwnd, cmd, ack);
    return ack.fAck ? TRUE : FALSE;
}
