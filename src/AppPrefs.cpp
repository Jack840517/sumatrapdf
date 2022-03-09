/* Copyright 2021 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/FileUtil.h"
#include "utils/FileWatcher.h"
#include "utils/UITask.h"
#include "utils/ScopedWin.h"
#include "utils/WinUtil.h"
#include "utils/Timer.h"

#include "wingui/TreeModel.h"
#include "DisplayMode.h"
#include "Controller.h"
#include "EngineBase.h"
#include "EngineAll.h"
#include "SumatraConfig.h"
#include "SettingsStructs.h"
#include "DisplayModel.h"
#include "FileHistory.h"
#include "GlobalPrefs.h"
#include "ProgressUpdateUI.h"
#include "Notifications.h"
#include "SumatraPDF.h"
#include "TabInfo.h"
#include "Flags.h"
#include "WindowInfo.h"
#include "AppPrefs.h"
#include "AppTools.h"
#include "Favorites.h"
#include "Toolbar.h"
#include "Translations.h"

#include "utils/Log.h"

// SumatraPDF.cpp
extern void RememberDefaultWindowPosition(WindowInfo* win);

static WatchedFile* gWatchedSettingsFile = nullptr;

// number of weeks past since 2011-01-01
static int GetWeekCount() {
    SYSTEMTIME date20110101 = {0};
    date20110101.wYear = 2011;
    date20110101.wMonth = 1;
    date20110101.wDay = 1;
    FILETIME origTime, currTime;
    BOOL ok = SystemTimeToFileTime(&date20110101, &origTime);
    CrashIf(!ok);
    GetSystemTimeAsFileTime(&currTime);
    return (currTime.dwHighDateTime - origTime.dwHighDateTime) / 1408;
    // 1408 == (10 * 1000 * 1000 * 60 * 60 * 24 * 7) / (1 << 32)
}

static int cmpFloat(const void* a, const void* b) {
    return *(float*)a < *(float*)b ? -1 : *(float*)a > *(float*)b ? 1 : 0;
}

namespace prefs {

const WCHAR* GetSettingsFileNameTemp() {
    return L"SumatraPDF-settings.txt";
}

WCHAR* GetSettingsPath() {
    return AppGenDataFilename(GetSettingsFileNameTemp());
}

/* Caller needs to prefs::CleanUp() */
bool Load() {
    CrashIf(gGlobalPrefs);

    auto timeStart = TimeGet();
    defer {
        auto dur = TimeSinceInMs(timeStart);
        logf("prefs::Load() took %.2f ms\n", dur);
    };

    AutoFreeWstr path = GetSettingsPath();
    AutoFree prefsData = file::ReadFile(path.Get());

    gGlobalPrefs = NewGlobalPrefs(prefsData.data);
    CrashAlwaysIf(!gGlobalPrefs);
    auto* gprefs = gGlobalPrefs;

    // in pre-release builds between 3.1.10079 and 3.1.10377,
    // RestoreSession was a string with the additional option "auto"
    // TODO: remove this after 3.2 has been released
#if defined(DEBUG) || defined(PRE_RELEASE_VER)
    if (!gprefs->restoreSession && prefsData.data && str::Find(prefsData.data, "\nRestoreSession = auto")) {
        gprefs->restoreSession = true;
    }
#endif

    if (!gprefs->uiLanguage || !trans::ValidateLangCode(gprefs->uiLanguage)) {
        // guess the ui language on first start
        str::ReplaceWithCopy(&gprefs->uiLanguage, trans::DetectUserLang());
    }
    gprefs->lastPrefUpdate = file::GetModificationTime(path.Get());
    gprefs->defaultDisplayModeEnum = DisplayModeFromString(gprefs->defaultDisplayMode, DisplayMode::Automatic);
    gprefs->defaultZoomFloat = ZoomFromString(gprefs->defaultZoom, ZOOM_ACTUAL_SIZE);
    CrashIf(!IsValidZoom(gprefs->defaultZoomFloat));

    int weekDiff = GetWeekCount() - gprefs->openCountWeek;
    gprefs->openCountWeek = GetWeekCount();
    if (weekDiff > 0) {
        // "age" openCount statistics (cut in in half after every week)
        for (FileState* fs : *gprefs->fileStates) {
            fs->openCount >>= weekDiff;
        }
    }

    // make sure that zoom levels are in the order expected by DisplayModel
    gprefs->zoomLevels->Sort(cmpFloat);
    while (gprefs->zoomLevels->size() > 0 && gprefs->zoomLevels->at(0) < ZOOM_MIN) {
        gprefs->zoomLevels->PopAt(0);
    }
    while (gprefs->zoomLevels->size() > 0 && gprefs->zoomLevels->Last() > ZOOM_MAX) {
        gprefs->zoomLevels->Pop();
    }

    // TODO: verify that all states have a non-nullptr file path?
    gFileHistory.UpdateStatesSource(gprefs->fileStates);
    auto fontName = ToWstrTemp(gprefs->fixedPageUI.ebookFontName);
    SetDefaultEbookFont(fontName.Get(), gprefs->fixedPageUI.ebookFontSize);

    if (!file::Exists(path.Get())) {
        Save();
    }
    return true;
}

static void RememberSessionState() {
    Vec<SessionData*>* sessionData = gGlobalPrefs->sessionData;
    ResetSessionState(sessionData);

    if (!gGlobalPrefs->rememberOpenedFiles) {
        return;
    }

    if (gWindows.size() == 0) {
        return;
    }

    // don't remember the state if there's only one window with 1 or less
    // opened document.
    if (gWindows.size() == 1 && gWindows[0]->tabs.size() < 2) {
        return;
    }

    for (auto* win : gWindows) {
        if (win->tabs.size() == 0) {
            continue;
        }
        SessionData* data = NewSessionData();
        for (TabInfo* tab : win->tabs) {
            char* fp = ToUtf8Temp(tab->filePath);
            FileState* fs = NewDisplayState(fp);
            if (tab->ctrl) {
                tab->ctrl->GetDisplayState(fs);
            }
            fs->showToc = tab->showToc;
            *fs->tocState = tab->tocState;
            data->tabStates->Append(NewTabState(fs));
            DeleteDisplayState(fs);
        }
        data->tabIndex = win->tabs.Find(win->currentTab) + 1;
        // TODO: allow recording this state without changing gGlobalPrefs
        RememberDefaultWindowPosition(win);
        data->windowState = gGlobalPrefs->windowState;
        data->windowPos = gGlobalPrefs->windowPos;
        data->sidebarDx = gGlobalPrefs->sidebarDx;
        sessionData->Append(data);
    }
}

// called whenever global preferences change or a file is
// added or removed from gFileHistory (in order to keep
// the list of recently opened documents in sync)
bool Save() {
    // don't save preferences without the proper permission
    if (!HasPermission(Perm::SavePreferences)) {
        return false;
    }

    // update display states for all tabs
    for (WindowInfo* win : gWindows) {
        for (TabInfo* tab : win->tabs) {
            UpdateTabFileDisplayStateForTab(tab);
        }
    }
    RememberSessionState();

    // remove entries which should (no longer) be remembered
    gFileHistory.Purge(!gGlobalPrefs->rememberStatePerDocument);
    // update display mode and zoom fields from internal values
    str::ReplaceWithCopy(&gGlobalPrefs->defaultDisplayMode, DisplayModeToString(gGlobalPrefs->defaultDisplayModeEnum));
    ZoomToString(&gGlobalPrefs->defaultZoom, gGlobalPrefs->defaultZoomFloat, nullptr);

    AutoFreeWstr path = GetSettingsPath();
    ReportIf(!path.data);
    if (!path.data) {
        return false;
    }
    ByteSlice prevPrefs = file::ReadFile(path.data);
    const char* prevPrefsData = (char*)prevPrefs.data();
    ByteSlice prefs = SerializeGlobalPrefs(gGlobalPrefs, prevPrefsData);
    defer {
        str::Free(prevPrefs.data());
        str::Free(prefs.data());
    };
    CrashIf(prefs.empty());
    if (prefs.empty()) {
        return false;
    }

    // only save if anything's changed at all
    if (prevPrefs.size() == prefs.size() && str::Eq(prefs, prevPrefs)) {
        return true;
    }

    bool ok = file::WriteFile(path.Get(), prefs);
    if (!ok) {
        return false;
    }
    gGlobalPrefs->lastPrefUpdate = file::GetModificationTime(path.Get());
    return true;
}

// refresh the preferences when a different SumatraPDF process saves them
// or if they are edited by the user using a text editor
bool Reload() {
    AutoFreeWstr path = GetSettingsPath();
    if (!file::Exists(path)) {
        return false;
    }

    // make sure that the settings file is readable - else wait
    // a short while to prevent accidental dataloss
    int tryAgainCount = 5;
    HANDLE h = file::OpenReadOnly(path);
    while (INVALID_HANDLE_VALUE == h && tryAgainCount-- > 0) {
        Sleep(200);
        h = file::OpenReadOnly(path);
    }
    if (INVALID_HANDLE_VALUE == h) {
        // prefer not reloading to resetting all settings
        return false;
    }

    AutoCloseHandle hScope(h);

    FILETIME time = file::GetModificationTime(path);
    if (FileTimeEq(time, gGlobalPrefs->lastPrefUpdate)) {
        return true;
    }

    const char* uiLanguage = str::DupTemp(gGlobalPrefs->uiLanguage);
    bool showToolbar = gGlobalPrefs->showToolbar;
    bool invertColors = gGlobalPrefs->fixedPageUI.invertColors;

    gFileHistory.UpdateStatesSource(nullptr);
    CleanUp();

    bool ok = Load();
    CrashAlwaysIf(!ok || !gGlobalPrefs);

    gGlobalPrefs->fixedPageUI.invertColors = invertColors;

    // TODO: about window doesn't have to be at position 0
    if (gWindows.size() > 0 && gWindows.at(0)->IsAboutWindow()) {
        WindowInfo* win = gWindows.at(0);
        win->HideToolTip();
        DeleteVecMembers(win->staticLinks);
        win->RedrawAll(true);
    }

    if (!str::Eq(uiLanguage, gGlobalPrefs->uiLanguage)) {
        SetCurrentLanguageAndRefreshUI(gGlobalPrefs->uiLanguage);
    }

    for (WindowInfo* win : gWindows) {
        if (gGlobalPrefs->showToolbar != showToolbar) {
            ShowOrHideToolbar(win);
        }
        UpdateFavoritesTree(win);
        UpdateTreeCtrlColors(win);
    }

    UpdateDocumentColors();
    UpdateFixedPageScrollbarsVisibility();
    return true;
}

void CleanUp() {
    DeleteGlobalPrefs(gGlobalPrefs);
    gGlobalPrefs = nullptr;
}

void schedulePrefsReload() {
    uitask::Post(prefs::Reload);
}

void RegisterForFileChanges() {
    if (!HasPermission(Perm::SavePreferences)) {
        return;
    }

    CrashIf(gWatchedSettingsFile); // only call me once
    AutoFreeWstr path = GetSettingsPath();
    gWatchedSettingsFile = FileWatcherSubscribe(path, schedulePrefsReload);
}

void UnregisterForFileChanges() {
    FileWatcherUnsubscribe(gWatchedSettingsFile);
}

}; // namespace prefs
