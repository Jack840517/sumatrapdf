/* Copyright 2021 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/ScopedWin.h"
#include "utils/WinUtil.h"
#include "utils/FileUtil.h"
#include "utils/SettingsUtil.h"

#include "DisplayMode.h"
#define INCLUDE_SETTINGSSTRUCTS_METADATA
#include "SettingsStructs.h"
#include "GlobalPrefs.h"

#include "utils/Log.h"

GlobalPrefs* gGlobalPrefs = nullptr;

FileState* NewDisplayState(const char* filePath) {
    FileState* fs = (FileState*)DeserializeStruct(&gFileStateInfo, nullptr);
    SetFileStatePath(fs, filePath);
    return fs;
}

void DeleteDisplayState(FileState* fs) {
    delete fs->thumbnail;
    FreeStruct(&gFileStateInfo, fs);
}

Favorite* NewFavorite(int pageNo, const WCHAR* name, const WCHAR* pageLabel) {
    Favorite* fav = (Favorite*)DeserializeStruct(&gFavoriteInfo, nullptr);
    fav->pageNo = pageNo;
    fav->name = strconv::WstrToUtf8(name);
    fav->pageLabel = strconv::WstrToUtf8(pageLabel);
    return fav;
}

void DeleteFavorite(Favorite* fav) {
    FreeStruct(&gFavoriteInfo, fav);
}

GlobalPrefs* NewGlobalPrefs(const char* data) {
    return (GlobalPrefs*)DeserializeStruct(&gGlobalPrefsInfo, data);
}

// prevData is used to preserve fields that exists in prevField but not in GlobalPrefs
ByteSlice SerializeGlobalPrefs(GlobalPrefs* prefs, const char* prevData) {
    if (!prefs->rememberStatePerDocument || !prefs->rememberOpenedFiles) {
        for (FileState* fs : *prefs->fileStates) {
            fs->useDefaultState = true;
        }
        // prevent unnecessary settings from being written out
        u16 fieldCount = 0;
        while (++fieldCount <= dimof(gFileStateFields)) {
            // count the number of fields up to and including useDefaultState
            if (gFileStateFields[fieldCount - 1].offset == offsetof(FileState, useDefaultState)) {
                break;
            }
        }
        // restore the correct fieldCount ASAP after serialization
        gFileStateInfo.fieldCount = fieldCount;
    }

    ByteSlice serialized = SerializeStruct(&gGlobalPrefsInfo, prefs, prevData);

    if (!prefs->rememberStatePerDocument || !prefs->rememberOpenedFiles) {
        gFileStateInfo.fieldCount = dimof(gFileStateFields);
    }

    return serialized;
}

void DeleteGlobalPrefs(GlobalPrefs* gp) {
    if (!gp) {
        return;
    }

    for (FileState* ds : *gp->fileStates) {
        delete ds->thumbnail;
    }
    FreeStruct(&gGlobalPrefsInfo, gp);
}

SessionData* NewSessionData() {
    return (SessionData*)DeserializeStruct(&gSessionDataInfo, nullptr);
}

TabState* NewTabState(FileState* fs) {
    TabState* state = (TabState*)DeserializeStruct(&gTabStateInfo, nullptr);
    str::ReplaceWithCopy(&state->filePath, fs->filePath);
    str::ReplaceWithCopy(&state->displayMode, fs->displayMode);
    state->pageNo = fs->pageNo;
    str::ReplaceWithCopy(&state->zoom, fs->zoom);
    state->rotation = fs->rotation;
    state->scrollPos = fs->scrollPos;
    state->showToc = fs->showToc;
    *state->tocState = *fs->tocState;
    return state;
}

void ResetSessionState(Vec<SessionData*>* sessionData) {
    CrashIf(!sessionData);
    if (!sessionData) {
        return;
    }
    for (SessionData* data : *sessionData) {
        FreeStruct(&gSessionDataInfo, data);
    }
    sessionData->Reset();
}

ParsedColor* GetParsedColor(const char* s, ParsedColor& parsed) {
    if (parsed.wasParsed) {
        return &parsed;
    }
    ParseColor(parsed, s);
    return &parsed;
}

void SetFileStatePath(FileState* fs, const char* path) {
    if (fs->filePath && str::EqI(fs->filePath, path)) {
        return;
    }
    str::ReplaceWithCopy(&fs->filePath, path);
}

void SetFileStatePath(FileState* fs, const WCHAR* path) {
    char* pathA = ToUtf8Temp(path);
    SetFileStatePath(fs, pathA);
}
