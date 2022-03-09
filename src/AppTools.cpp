/* Copyright 2021 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/WinDynCalls.h"
#include "utils/CmdLineArgsIter.h"
#include "utils/DbgHelpDyn.h"
#include "utils/FileUtil.h"
#include "utils/WinUtil.h"
#include "utils/CryptoUtil.h"
#include "utils/DirIter.h"
#include "utils/RegistryPaths.h"

#include "AppTools.h"
#include "SumatraConfig.h"
#include "Translations.h"
#include "Version.h"

/* Returns true, if a Registry entry indicates that this executable has been
   created by an installer (and should be updated through an installer) */
bool HasBeenInstalled() {
    // see GetInstallationDir() in Installer.cpp
    const WCHAR* appName = GetAppNameTemp();
    AutoFreeWstr regPathUninst = str::Join(L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\", appName);
    AutoFreeWstr installedPath = ReadRegStr2(regPathUninst, L"InstallLocation");
    if (!installedPath) {
        return false;
    }

    auto exePath = GetExePathTemp();
    if (exePath.empty()) {
        return false;
    }

    if (!str::EndsWithI(installedPath, L".exe")) {
        WCHAR* tmp = path::Join(installedPath, path::GetBaseNameTemp(exePath));
        installedPath.Set(tmp);
    }
    return path::IsSame(installedPath, exePath);
}

/* Return false if this program has been started from "Program Files" directory
   (which is an indicator that it has been installed) or from the last known
   location of a SumatraPDF installation: */
bool IsRunningInPortableMode() {
    // cache the result so that it will be consistent during the lifetime of the process
    static int sCacheIsPortable = -1; // -1 == uninitialized, 0 == installed, 1 == portable
    if (sCacheIsPortable != -1) {
        return sCacheIsPortable != 0;
    }
    sCacheIsPortable = 1;

    if (HasBeenInstalled()) {
        sCacheIsPortable = 0;
        return false;
    }

    auto exePath = GetExePathTemp().Get();
    WCHAR* programFilesDir = GetSpecialFolderTemp(CSIDL_PROGRAM_FILES).Get();
    // if we can't get a path, assume we're not running from "Program Files"
    if (!exePath || !programFilesDir) {
        return true;
    }

    // check if one of the exePath's parent directories is "Program Files"
    // (or a junction to it)
    WCHAR* baseName;
    while ((baseName = (WCHAR*)path::GetBaseNameTemp(exePath)) > exePath) {
        baseName[-1] = '\0';
        if (path::IsSame(programFilesDir, exePath)) {
            sCacheIsPortable = 0;
            return false;
        }
    }

    return true;
}

bool IsDllBuild() {
    HRSRC resSrc = FindResourceW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), RT_RCDATA);
    return resSrc != nullptr;
}

static AutoFreeWstr gAppDataDir;

void SetAppDataPath(const WCHAR* path) {
    gAppDataDir.Set(path::Normalize(path));
}

/* Generate the full path for a filename used by the app in the userdata path. */
/* Caller needs to free() the result. */
WCHAR* AppGenDataFilename(const WCHAR* fileName) {
    if (!fileName) {
        return nullptr;
    }

    if (gAppDataDir && dir::Exists(gAppDataDir)) {
        return path::Join(gAppDataDir, fileName);
    }

    if (IsRunningInPortableMode()) {
        /* Use the same path as the binary */
        return path::GetPathOfFileInAppDir(fileName);
    }

    WCHAR* path = GetSpecialFolderTemp(CSIDL_LOCAL_APPDATA, true).Get();
    if (!path) {
        return nullptr;
    }
    const WCHAR* appName = GetAppNameTemp();
    path = path::Join(path, appName);
    if (!path) {
        return nullptr;
    }
    bool ok = dir::Create(path);
    if (!ok) {
        return nullptr;
    }
    return path::Join(path, fileName);
}

char* AppGenDataFilenameTemp(const char* fileName) {
    if (!fileName) {
        return nullptr;
    }
    WCHAR* tmp = ToWstrTemp(fileName);
    WCHAR* path = AppGenDataFilename(tmp);
    char* res = ToUtf8Temp(path);
    str::Free(path);
    return res;
}

#if 0
WCHAR* PathForFileInAppDataDir(const WCHAR* fileName) {
    if (!fileName) {
        return nullptr;
    }

    /* Use local (non-roaming) app data directory */
    TempWstr dataDir = GetSpecialFolderTemp(CSIDL_LOCAL_APPDATA, true);
    AutoFreeWstr dir = path::Join(dataDir.Get(), APP_NAME_STR);
    bool ok = dir::Create(dir);
    if (!ok) {
        return nullptr;
    }

    return path::Join(dir, fileName);
}
#endif

/*
Structure of registry entries for associating Sumatra with PDF files.

The following paths exist under both HKEY_LOCAL_MACHINE and HKEY_CURRENT_USER.
HKCU has precedence over HKLM.

Software\Classes\.pdf default key is name of reg entry describing the app
  handling opening PDF files. In our case it's SumatraPDF
Software\Classes\.pdf\OpenWithProgids
  should contain SumatraPDF so that it's easier for the user to later
  restore SumatraPDF to become the default app through Windows Explorer,
  cf. http://msdn.microsoft.com/en-us/library/cc144148(v=vs.85).aspx

Software\Classes\SumatraPDF\DefaultIcon = $exePath,1
  1 means the second icon resource within the executable
Software\Classes\SumatraPDF\shell\open\command = "$exePath" "%1"
  tells how to call sumatra to open PDF file. %1 is replaced by PDF file path

Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\.pdf\Progid
  should be SumatraPDF (FoxIt takes it over); only needed for HKEY_CURRENT_USER
  TODO: No other app seems to set this one, and only UserChoice seems to make
        a difference - is this still required for Windows XP?

Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\.pdf\Application
  should be SumatraPDF.exe; only needed for HKEY_CURRENT_USER
  Windows XP seems to use this instead of:

Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\.pdf\UserChoice\Progid
  should be SumatraPDF as well (also only needed for HKEY_CURRENT_USER);
  this key is used for remembering a user's choice with Explorer's Open With dialog
  and can't be written to - so we delete it instead!

HKEY_CLASSES_ROOT\.pdf\OpenWithList
  list of all apps that can be used to open PDF files. We don't touch that.

HKEY_CLASSES_ROOT\.pdf default comes from either HKCU\Software\Classes\.pdf or
HKLM\Software\Classes\.pdf (HKCU has priority over HKLM)

Note: When making changes below, please also adjust WriteExtendedFileExtensionInfo(),
UnregisterFromBeingDefaultViewer() and RemoveOwnRegistryKeys() in Installer.cpp.

*/

void DoAssociateExeWithPdfExtension(HKEY hkey) {
    auto exePath = GetExePathTemp();
    if (exePath.empty()) {
        return;
    }

    AutoFreeWstr REG_CLASSES_APP = str::Join(LR"(Software\Classes\)", GetAppNameTemp());

    AutoFreeWstr prevHandler;
    // Remember the previous default app for the Uninstaller
    prevHandler.Set(ReadRegStr(hkey, REG_CLASSES_PDF, nullptr));

    bool ok = false;
    const WCHAR* appName = GetAppNameTemp();
    if (prevHandler && !str::Eq(prevHandler, appName)) {
        WriteRegStr(hkey, REG_CLASSES_APP, L"previous.pdf", prevHandler);
    }

    WriteRegStr(hkey, REG_CLASSES_APP, nullptr, _TR("PDF Document"));
    AutoFreeWstr icon_path = str::Join(exePath, L",1");
    {
        AutoFreeWstr key = str::Join(REG_CLASSES_APP, LR"(\DefaultIcon)");
        WriteRegStr(hkey, key, nullptr, icon_path);
    }

    {
        AutoFreeWstr key = str::Join(REG_CLASSES_APP, LR"(\shell)");
        WriteRegStr(hkey, key, nullptr, L"open");
    }

    // "${exePath}" "%1" %*
    AutoFreeWstr cmdPath = str::Format(LR"("%s" "%%1" %%*)", exePath.Get());
    {
        AutoFreeWstr key = str::Join(REG_CLASSES_APP, LR"(\shell\open\command)");
        ok = WriteRegStr(hkey, key, nullptr, cmdPath);
    }

    // register for printing: "${exePath}" -print-to-default "%1"
    cmdPath.Set(str::Format(LR"("%s" -print-to-default "%%1")", exePath.Get()));
    {
        AutoFreeWstr key = str::Join(REG_CLASSES_APP, LR"(\shell\print\command)");
        WriteRegStr(hkey, key, nullptr, cmdPath);
    }

    // register for printing to specific printer:
    // "${exePath}" -print-to "%2" "%1"
    cmdPath.Set(str::Format(LR"("%s" -print-to "%%2" "%%1")", exePath.Get()));
    {
        AutoFreeWstr key = str::Join(REG_CLASSES_APP, LR"(\shell\printto\command)");
        WriteRegStr(hkey, key, nullptr, cmdPath);
    };

    // Only change the association if we're confident, that we've registered ourselves well enough
    if (!ok) {
        return;
    }

    WriteRegStr(hkey, REG_CLASSES_PDF, nullptr, appName);
    // TODO: also add SumatraPDF to the Open With lists for the other supported extensions?
    WriteRegStr(hkey, REG_CLASSES_PDF LR"(\OpenWithProgids)", appName, L"");
    if (hkey == HKEY_CURRENT_USER) {
        WriteRegStr(hkey, REG_EXPLORER_PDF_EXT, L"Progid", appName);
        CrashIf(hkey == nullptr); // to appease prefast
        SHDeleteValue(hkey, REG_EXPLORER_PDF_EXT, L"Application");
        DeleteRegKey(hkey, REG_EXPLORER_PDF_EXT LR"(\UserChoice)", true);
    }
}

// verify that all registry entries that need to be set in order to associate
// Sumatra with .pdf files exist and have the right values
bool IsExeAssociatedWithPdfExtension() {
    // this one doesn't have to exist but if it does, it must be APP_NAME_STR
    const WCHAR* appName = GetAppNameTemp();

    AutoFreeWstr tmp(ReadRegStr(HKEY_CURRENT_USER, REG_EXPLORER_PDF_EXT, L"Progid"));
    if (tmp && !str::Eq(tmp, appName)) {
        return false;
    }

    // this one doesn't have to exist but if it does, it must be APP_NAME_STR.exe
    tmp.Set(ReadRegStr(HKEY_CURRENT_USER, REG_EXPLORER_PDF_EXT, L"Application"));
    AutoFreeWstr exeName = str::Join(appName, L".exe");
    if (tmp && !str::EqI(tmp, exeName)) {
        return false;
    }

    // this one doesn't have to exist but if it does, it must be APP_NAME_STR
    tmp.Set(ReadRegStr(HKEY_CURRENT_USER, REG_EXPLORER_PDF_EXT LR"(\UserChoice)", L"Progid"));
    if (tmp && !str::Eq(tmp, appName)) {
        return false;
    }

    // HKEY_CLASSES_ROOT\.pdf default key must exist and be equal to APP_NAME_STR
    tmp.Set(ReadRegStr(HKEY_CLASSES_ROOT, L".pdf", nullptr));
    if (!str::Eq(tmp, appName)) {
        return false;
    }

    // HKEY_CLASSES_ROOT\SumatraPDF\shell\open default key must be: open
    {
        AutoFreeWstr key = str::Join(appName, LR"(\shell)");
        tmp.Set(ReadRegStr(HKEY_CLASSES_ROOT, key, nullptr));
    }
    if (!str::EqI(tmp, L"open")) {
        return false;
    }

    // HKEY_CLASSES_ROOT\SumatraPDF\shell\open\command default key must be: "${exe_path}" "%1"
    {
        AutoFreeWstr key = str::Join(appName, LR"(\shell\open\command)");
        tmp.Set(ReadRegStr(HKEY_CLASSES_ROOT, key, nullptr));
    }
    if (!tmp) {
        return false;
    }

    auto exePath = GetExePathTemp().Get();
    if (!exePath || !str::Find(tmp, LR"("%1")")) {
        return false;
    }

    CmdLineArgsIter argList(tmp);
    bool hasPerc1 = false;
    for (int i = 1; i < argList.nArgs; i++) {
        if (str::Eq(argList.args[i], L"%1")) {
            hasPerc1 = true;
        }
    }
    if (!hasPerc1) {
        return false;
    }

    return path::IsSame(exePath, argList.at(0));
}

// List of rules used to detect TeX editors.

// type of path information retrieved from the registy
enum EditorPathType {
    BinaryPath,  // full path to the editor's binary file
    BinaryDir,   // directory containing the editor's binary file
    SiblingPath, // full path to a sibling file of the editor's binary file
};

static struct {
    const char* binaryFilename;    // Editor's binary file name
    const char* inverseSearchArgs; // Parameters to be passed to the editor;
                                   // use placeholder '%f' for path to source file and '%l' for line number.
    EditorPathType Type;           // Type of the path information obtained from the registry
    HKEY RegRoot;                  // Root of the regkey
    const WCHAR* RegKey;           // Registry key path
    const WCHAR* RegValue;         // Registry value name
} editorRules[] = {
    {"WinEdt.exe", "\"[Open(|%f|);SelPar(%l,8)]\"", BinaryPath, HKEY_LOCAL_MACHINE,
     REG_WIN_CURR L"\\App Paths\\WinEdt.exe", nullptr},
    {"WinEdt.exe", "\"[Open(|%f|);SelPar(%l,8)]\"", BinaryDir, HKEY_CURRENT_USER, L"Software\\WinEdt", L"Install Root"},
    {"notepad++.exe", "-n%l \"%f\"", BinaryPath, HKEY_LOCAL_MACHINE, REG_WIN_CURR L"\\App Paths\\notepad++.exe",
     nullptr},
    {"notepad++.exe", "-n%l \"%f\"", BinaryDir, HKEY_LOCAL_MACHINE, L"Software\\Notepad++", nullptr},
    {"notepad++.exe", "-n%l \"%f\"", BinaryPath, HKEY_LOCAL_MACHINE, REG_WIN_CURR L"\\Uninstall\\Notepad++",
     L"DisplayIcon"},
    {"sublime_text.exe", "\"%f:%l\"", BinaryDir, HKEY_LOCAL_MACHINE, REG_WIN_CURR L"\\Uninstall\\Sublime Text 3_is1",
     L"InstallLocation"},
    {"sublime_text.exe", "\"%f:%l\"", BinaryPath, HKEY_LOCAL_MACHINE, REG_WIN_CURR L"\\Uninstall\\Sublime Text 3_is1",
     L"DisplayIcon"},
    {"sublime_text.exe", "\"%f:%l\"", BinaryDir, HKEY_LOCAL_MACHINE, REG_WIN_CURR L"\\Uninstall\\Sublime Text 2_is1",
     L"InstallLocation"},
    {"sublime_text.exe", "\"%f:%l\"", BinaryPath, HKEY_LOCAL_MACHINE, REG_WIN_CURR L"\\Uninstall\\Sublime Text 2_is1",
     L"DisplayIcon"},
    {"TeXnicCenter.exe", "/ddecmd \"[goto('%f', '%l')]\"", BinaryDir, HKEY_LOCAL_MACHINE,
     L"Software\\ToolsCenter\\TeXnicCenterNT", L"AppPath"},
    {"TeXnicCenter.exe", "/ddecmd \"[goto('%f', '%l')]\"", BinaryDir, HKEY_LOCAL_MACHINE,
     REG_WIN_CURR L"\\Uninstall\\TeXnicCenter_is1", L"InstallLocation"},
    {"TeXnicCenter.exe", "/ddecmd \"[goto('%f', '%l')]\"", BinaryDir, HKEY_LOCAL_MACHINE,
     REG_WIN_CURR L"\\Uninstall\\TeXnicCenter Alpha_is1", L"InstallLocation"},
    {"TEXCNTR.exe", "/ddecmd \"[goto('%f', '%l')]\"", BinaryDir, HKEY_LOCAL_MACHINE,
     L"Software\\ToolsCenter\\TeXnicCenter", L"AppPath"},
    {"TEXCNTR.exe", "/ddecmd \"[goto('%f', '%l')]\"", BinaryDir, HKEY_LOCAL_MACHINE,
     REG_WIN_CURR L"\\Uninstall\\TeXnicCenter_is1", L"InstallLocation"},
    {"WinShell.exe", "-c \"%f\" -l %l", BinaryDir, HKEY_LOCAL_MACHINE, REG_WIN_CURR L"\\Uninstall\\WinShell_is1",
     L"InstallLocation"},
    {"gvim.exe", "\"%f\" +%l", BinaryPath, HKEY_LOCAL_MACHINE, L"Software\\Vim\\Gvim", L"path"},
    {// TODO: add this rule only if the latex-suite for ViM is installed
     // (http://vim-latex.sourceforge.net/documentation/latex-suite.txt)
     "gvim.exe", "-c \":RemoteOpen +%l %f\"", BinaryPath, HKEY_LOCAL_MACHINE, L"Software\\Vim\\Gvim", L"path"},
    {"texmaker.exe", "\"%f\" -line %l", SiblingPath, HKEY_LOCAL_MACHINE, REG_WIN_CURR L"\\Uninstall\\Texmaker",
     L"UninstallString"},
    {
        "TeXworks.exe", "-p=%l \"%f\"", BinaryDir, HKEY_LOCAL_MACHINE,
        REG_WIN_CURR "\\Uninstall\\{41DA4817-4D2A-4D83-AD02-6A2D95DC8DCB}_is1", L"InstallLocation",
        // TODO: find a way to detect where emacs is installed
        // L"emacsclientw.exe",L"+%l \"%f\"", BinaryPath, HKEY_LOCAL_MACHINE, L"???", L"???",
    }};

// Detect TeX editors installed on the system and construct the
// corresponding inverse search commands.
//
// Parameters:
//      hwndCombo   -- (optional) handle to a combo list that will be filled with the list of possible inverse search
//      commands.
// Returns:
//      the inverse search command of the first detected editor (the caller needs to free() the result).
WCHAR* AutoDetectInverseSearchCommands(HWND hwndCombo) {
    WCHAR* firstEditor = nullptr;
    WStrList foundExes;

    for (int i = 0; i < dimof(editorRules); i++) {
        auto& rule = editorRules[i];
        AutoFreeWstr path(ReadRegStr(rule.RegRoot, rule.RegKey, rule.RegValue));
        if (!path) {
            continue;
        }

        AutoFreeWstr exePath;
        WCHAR* binaryFileName = ToWstrTemp(rule.binaryFilename);
        WCHAR* inverseSearchArgs = ToWstrTemp(rule.inverseSearchArgs);
        if (rule.Type == SiblingPath) {
            // remove file part
            AutoFreeWstr dir(path::GetDir(path));
            exePath.Set(path::Join(dir, binaryFileName));
        } else if (rule.Type == BinaryDir) {
            exePath.Set(path::Join(path, binaryFileName));
        } else { // if (editor_rules[i].Type == BinaryPath)
            exePath.Set(path.StealData());
        }
        // don't show duplicate entries
        if (foundExes.FindI(exePath) != -1) {
            continue;
        }
        // don't show inexistent paths (and don't try again for them)
        if (!file::Exists(exePath)) {
            foundExes.Append(exePath.StealData());
            continue;
        }

        AutoFreeWstr editorCmd(str::Format(L"\"%s\" %s", exePath.Get(), inverseSearchArgs));

        if (!hwndCombo) {
            // no need to fill a combo box: return immeditately after finding an editor.
            return editorCmd.StealData();
        }

        ComboBox_AddString(hwndCombo, editorCmd);
        if (!firstEditor) {
            firstEditor = editorCmd.StealData();
        }
        foundExes.Append(exePath.StealData());
    }

    // Fall back to notepad as a default handler
    if (!firstEditor) {
        firstEditor = str::Dup(L"notepad %f");
        if (hwndCombo) {
            ComboBox_AddString(hwndCombo, firstEditor);
        }
    }
    return firstEditor;
}

#define UWM_DELAYED_SET_FOCUS (WM_APP + 1)
#define UWM_DELAYED_CTRL_BACK (WM_APP + 2)

// selects all text in an edit box if it's selected either
// through a keyboard shortcut or a non-selecting mouse click
// (or responds to Ctrl+Backspace as nowadays expected)
bool ExtendedEditWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM) {
    static bool delayFocus = false;

    switch (msg) {
        case WM_LBUTTONDOWN:
            delayFocus = !IsFocused(hwnd);
            return true;

        case WM_LBUTTONUP:
            if (delayFocus) {
                DWORD sel = Edit_GetSel(hwnd);
                if (LOWORD(sel) == HIWORD(sel)) {
                    PostMessageW(hwnd, UWM_DELAYED_SET_FOCUS, 0, 0);
                }
                delayFocus = false;
            }
            return true;

        case WM_KILLFOCUS:
            return false; // for easier debugging (make setting a breakpoint possible)

        case WM_SETFOCUS:
            if (!delayFocus) {
                PostMessageW(hwnd, UWM_DELAYED_SET_FOCUS, 0, 0);
            }
            return true;

        case UWM_DELAYED_SET_FOCUS:
            Edit_SelectAll(hwnd);
            return true;

        case WM_KEYDOWN:
            if (VK_BACK != wp || !IsCtrlPressed() || IsShiftPressed()) {
                return false;
            }
            PostMessageW(hwnd, UWM_DELAYED_CTRL_BACK, 0, 0);
            return true;

        case UWM_DELAYED_CTRL_BACK: {
            WCHAR* text = win::GetTextTemp(hwnd).Get();
            int selStart = LOWORD(Edit_GetSel(hwnd)), selEnd = selStart;
            // remove the rectangle produced by Ctrl+Backspace
            if (selStart > 0 && text[selStart - 1] == '\x7F') {
                memmove(text + selStart - 1, text + selStart, str::Len(text + selStart - 1) * sizeof(WCHAR));
                win::SetText(hwnd, text);
                selStart = selEnd = selStart - 1;
            }
            // remove the previous word (and any spacing after it)
            for (; selStart > 0 && str::IsWs(text[selStart - 1]); selStart--) {
                ;
            }
            for (; selStart > 0 && !str::IsWs(text[selStart - 1]); selStart--) {
                ;
            }
            Edit_SetSel(hwnd, selStart, selEnd);
            SendMessageW(hwnd, WM_CLEAR, 0, 0);
        }
            return true;

        default:
            return false;
    }
}

/* Default size for the window, happens to be american A4 size (I think) */
#define DEF_PAGE_RATIO (612.0 / 792.0)

#define MIN_WIN_DX 50
#define MIN_WIN_DY 50

void EnsureAreaVisibility(Rect& r) {
    // adjust to the work-area of the current monitor (not necessarily the primary one)
    Rect work = GetWorkAreaRect(r, nullptr);

    // make sure that the window is neither too small nor bigger than the monitor
    if (r.dx < MIN_WIN_DX || r.dx > work.dx) {
        r.dx = std::min((int)((double)work.dy * DEF_PAGE_RATIO), work.dx);
    }
    if (r.dy < MIN_WIN_DY || r.dy > work.dy) {
        r.dy = work.dy;
    }

    // check whether the lower half of the window's title bar is
    // inside a visible working area
    int captionDy = GetSystemMetrics(SM_CYCAPTION);
    Rect halfCaption(r.x, r.y + captionDy / 2, r.dx, captionDy / 2);
    if (halfCaption.Intersect(work).IsEmpty()) {
        r = Rect(work.TL(), r.Size());
    }
}

Rect GetDefaultWindowPos() {
    RECT workArea;
    SystemParametersInfo(SPI_GETWORKAREA, 0, &workArea, 0);
    Rect work = Rect::FromRECT(workArea);

    Rect r = work;
    r.dx = std::min((int)((double)r.dy * DEF_PAGE_RATIO), work.dx);
    r.x = (work.dx - r.dx) / 2;

    return r;
}

void SaveCallstackLogs() {
    AutoFree s = dbghelp::GetCallstacks();
    if (s.empty()) {
        return;
    }
    AutoFreeWstr filePath(AppGenDataFilename(L"callstacks.txt"));
    file::WriteFile(filePath.Get(), s.AsSpan());
}

// TODO: this can be used for extracting other data
#if 0
// cache because calculating md5 of the whole executable
// might be relatively expensive
static AutoFreeWstr gAppMd5;

// return hex version of md5 of app's executable
// nullptr if there was an error
// caller needs to free the result
static const WCHAR* Md5OfAppExe() {
    if (gAppMd5.Get()) {
        return str::Dup(gAppMd5.Get());
    }

    auto appPath = GetExePathTemp();
    if (appPath.empty()) {
        return {};
    }
    AutoFree d = file::ReadFile(appPath.data);
    if (d.empty()) {
        return nullptr;
    }

    u8 md5[16] = {0};
    CalcMD5Digest(d.data, d.size(), md5);

    AutoFree md5HexA(_MemToHex(&md5));
    AutoFreeWstr md5Hex = strconv::Utf8ToWchar(md5HexA.AsView());

    return md5Hex.StealData();
}

// remove all directories except for ours
//. need to avoid acuumulating the directories when testing
// locally or using pre-release builds (both cases where
// exe and its md5 changes frequently)
void RemoveMd5AppDataDirectories() {
    AutoFreeWstr extractedDir = PathForFileInAppDataDir(L"extracted");
    if (extractedDir.empty()) {
        return;
    }

    auto dirs = CollectDirsFromDirectory(extractedDir.data);
    if (dirs.empty()) {
        return;
    }

    AutoFreeWstr md5App = Md5OfAppExe();
    if (md5App.empty()) {
        return;
    }

    AutoFreeWstr md5Dir = path::Join(extractedDir.data, md5App.data);

    for (auto& dir : dirs) {
        const WCHAR* s = dir.data();
        if (str::Eq(s, md5Dir.data)) {
            continue;
        }
        dir::RemoveAll(s);
    }
}

// return a path on disk to extracted unrar.dll or nullptr if couldn't extract
// memory has to be freed by the caller
const WCHAR* ExractUnrarDll() {
    RemoveMd5AppDataDirectories();

    AutoFreeWstr extractedDir = PathForFileInAppDataDir(L"extracted");
    if (extractedDir.empty()) {
        return nullptr;
    }

    AutoFreeWstr md5App = Md5OfAppExe();
    if (md5App.empty()) {
        return nullptr;
    }

    AutoFreeWstr md5Dir = path::Join(extractedDir.data, md5App.data);
    AutoFreeWstr dllPath = path::Join(md5Dir.data, unrarFileName);

    if (file::Exists(dllPath.data)) {
        const WCHAR* ret = dllPath.data;
        dllPath = nullptr; // don't free
        return ret;
    }

    bool ok = dir::CreateAll(md5Dir.data);
    if (!ok) {
        return nullptr;
    }

    HGLOBAL res = 0;
    auto h = GetModuleHandle(nullptr);
    WCHAR* resName = MAKEINTRESOURCEW(1);
    HRSRC resSrc = FindResourceW(h, resName, RT_RCDATA);
    if (!resSrc) {
        return nullptr;
    }
    res = LoadResource(nullptr, resSrc);
    if (!res) {
        return nullptr;
    }
    const char* data = (const char*)LockResource(res);
    defer {
        UnlockResource(res);
    };
    DWORD dataSize = SizeofResource(nullptr, resSrc);
    ok = file::WriteFile(dllPath, data, dataSize);
    if (!ok) {
        return nullptr;
    }

    const WCHAR* ret = dllPath;
    dllPath = nullptr; // don't free
    return ret;
}
#endif

constexpr double KB = 1024;
constexpr double MB = (double)1024 * (double)1024;
constexpr double GB = (double)1024 * (double)1024 * (double)1024;

// Format the file size in a short form that rounds to the largest size unit
// e.g. "3.48 GB", "12.38 MB", "23 KB"
// Caller needs to free the result.
static WCHAR* FormatSizeSuccint(i64 size) {
    const WCHAR* unit = nullptr;
    double s = (double)size;

    if (s > GB) {
        s = s / GB;
        unit = _TR("GB");
    } else if (s > MB) {
        s = s / MB;
        unit = _TR("MB");
    } else {
        s = s / KB;
        unit = _TR("KB");
    }

    AutoFreeWstr sizestr = str::FormatFloatWithThousandSep(s);
    if (!unit) {
        return sizestr.StealData();
    }
    return str::Format(L"%s %s", sizestr.Get(), unit);
}

// format file size in a readable way e.g. 1348258 is shown
// as "1.29 MB (1,348,258 Bytes)"
// Caller needs to free the result
WCHAR* FormatFileSize(i64 size) {
    if (size <= 0) {
        return str::Format(L"%d", (int)size);
    }
    AutoFreeWstr n1(FormatSizeSuccint(size));
    AutoFreeWstr n2(str::FormatNumWithThousandSep(size));
    return str::Format(L"%s (%s %s)", n1.Get(), n2.Get(), _TR("Bytes"));
}

// Format the file size in a short form that rounds to the largest size unit
// e.g. "3.48 GB", "12.38 MB", "23 KB"
// To be used in a context where translations are not yet available
// Caller needs to free the result.
static WCHAR* FormatSizeSuccintNoTrans(i64 size) {
    const WCHAR* unit = nullptr;
    double s = (double)size;

    if (s > GB) {
        s = s / GB;
        unit = L"GB";
    } else if (s > MB) {
        s = s / MB;
        unit = L"MB";
    } else {
        s = s / KB;
        unit = L"KB";
    }

    AutoFreeWstr sizestr = str::FormatFloatWithThousandSep(s);
    if (!unit) {
        return sizestr.StealData();
    }
    return str::Format(L"%s %s", sizestr.Get(), unit);
}

// format file size in a readable way e.g. 1348258 is shown
// as "1.29 MB (1,348,258 Bytes)"
// Caller needs to free the result
WCHAR* FormatFileSizeNoTrans(i64 size) {
    if (size <= 0) {
        return str::Format(L"%d", (int)size);
    }
    AutoFreeWstr n1(FormatSizeSuccintNoTrans(size));
    AutoFreeWstr n2(str::FormatNumWithThousandSep(size));
    return str::Format(L"%s (%s %s)", n1.Get(), n2.Get(), L"Bytes");
}
