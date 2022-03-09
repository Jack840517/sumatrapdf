/* Copyright 2021 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#include "utils/BaseUtil.h"
#include "utils/FileUtil.h"
#include "utils/ScopedWin.h"
#include "utils/WinUtil.h"

#include "utils/Log.h"

// we pad data read with 3 zeros for convenience. That way returned
// data is a valid null-terminated string or WCHAR*.
// 3 is for absolute worst case of WCHAR* where last char was partially written
#define ZERO_PADDING_COUNT 3

namespace path {

bool IsSep(char c) {
    return '\\' == c || '/' == c;
}

// do not free, returns pointer inside <path>
const char* GetBaseNameTemp(const char* path) {
    const char* s = path + str::Len(path);
    for (; s > path; s--) {
        if (IsSep(s[-1])) {
            break;
        }
    }
    return s;
}

std::string_view GetBaseName(std::string_view path) {
    const char* end = path.data() + path.size();
    const char* s = end;
    for (; s > path.data(); s--) {
        if (IsSep(s[-1])) {
            break;
        }
    }
    const char* res = str::Dup(s, end - s);
    return res;
}

// do not free, returns pointer inside <path>
const char* GetExtTemp(const char* path) {
    const char* ext = nullptr;
    char c = *path;
    while (c) {
        if (c == '.') {
            ext = path;
        } else if (IsSep(c)) {
            ext = nullptr;
        }
        path++;
        c = *path;
    }
    if (nullptr == ext) {
        return path; // empty string
    }
    return ext;
}

char* Join(const char* path, const char* fileName, Allocator* allocator) {
    if (IsSep(*fileName)) {
        fileName++;
    }
    const char* sepStr = nullptr;
    if (!IsSep(path[str::Len(path) - 1])) {
        sepStr = "\\";
    }
    return str::Join(path, sepStr, fileName, allocator);
}

bool IsDirectory(std::wstring_view path) {
    DWORD attrs = GetFileAttributesW(path.data());
    if (INVALID_FILE_ATTRIBUTES == attrs) {
        return false;
    }
    return (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool IsDirectory(std::string_view path) {
    auto pathW = ToWstrTemp(path);
    DWORD attrs = GetFileAttributesW(pathW);
    if (INVALID_FILE_ATTRIBUTES == attrs) {
        return false;
    }
    return (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool IsSep(WCHAR c) {
    return '\\' == c || '/' == c;
}

// do not free, returns pointer inside <path>
const WCHAR* GetBaseNameTemp(const WCHAR* path) {
    const WCHAR* end = path + str::Len(path);
    while (end > path) {
        if (IsSep(end[-1])) {
            break;
        }
        --end;
    }
    return end;
}

// returns extension e.g. ".pdf"
// do not free, returns pointer inside <path>
const WCHAR* GetExtTemp(const WCHAR* path) {
    const WCHAR* ext = path + str::Len(path);
    while ((ext > path) && !IsSep(*ext)) {
        if (*ext == '.') {
            return ext;
        }
        ext--;
    }
    return path + str::Len(path);
}

// caller has to free() the results
WCHAR* GetDir(const WCHAR* path) {
    const WCHAR* baseName = GetBaseNameTemp(path);
    if (baseName == path) {
        // relative directory
        return str::Dup(L".");
    }
    if (baseName == path + 1) {
        // relative root
        return str::Dup(path, 1);
    }
    if (baseName == path + 3 && path[1] == ':') {
        // local drive root
        return str::Dup(path, 3);
    }
    if (baseName == path + 2 && str::StartsWith(path, L"\\\\")) {
        // server root
        return str::Dup(path);
    }
    // any subdirectory
    return str::Dup(path, baseName - path - 1);
}

// caller has to free() the results
std::string_view GetDir(std::string_view pathSV) {
    const char* path = pathSV.data();
    const char* baseName = GetBaseNameTemp(path);
    if (baseName == path) {
        // relative directory
        return str::Dup(".");
    }
    if (baseName == path + 1) {
        // relative root
        return str::Dup(path, 1);
    }
    if (baseName == path + 3 && path[1] == ':') {
        // local drive root
        return str::Dup(path, 3);
    }
    if (baseName == path + 2 && str::StartsWith(path, "\\\\")) {
        // server root
        return str::Dup(path);
    }
    // any subdirectory
    return str::Dup(path, baseName - path - 1);
}

WCHAR* Join(const WCHAR* path, const WCHAR* fileName, const WCHAR* fileName2) {
    // TODO: not sure if should allow null path
    if (IsSep(*fileName)) {
        fileName++;
    }
    const WCHAR* sepStr = nullptr;
    size_t pathLen = str::Len(path);
    if (pathLen > 0) {
        if (!IsSep(path[pathLen - 1])) {
            sepStr = L"\\";
        }
    }
    WCHAR* res = str::Join(path, sepStr, fileName);
    if (fileName2) {
        WCHAR* toFree = res;
        res = Join(res, fileName2);
        free(toFree);
    }
    return res;
}

// Normalize a file path.
//  remove relative path component (..\ and .\),
//  replace slashes by backslashes,
//  convert to long form.
//
// Returns a pointer to a memory allocated block containing the normalized string.
//   The caller is responsible for freeing the block.
//   Returns nullptr if the file does not exist or if a memory allocation fails.
//
// Precondition: the file must exist on the file system.
//
// Note:
//   - the case of the root component is preserved
//   - the case of rest is set to the way it is stored on the file system
//
// e.g. suppose the a file "C:\foo\Bar.Pdf" exists on the file system then
//    "c:\foo\bar.pdf" becomes "c:\foo\Bar.Pdf"
//    "C:\foo\BAR.PDF" becomes "C:\foo\Bar.Pdf"
WCHAR* Normalize(const WCHAR* path) {
    // convert to absolute path, change slashes into backslashes
    DWORD cch = GetFullPathNameW(path, 0, nullptr, nullptr);
    if (!cch) {
        return str::Dup(path);
    }

    AutoFreeWstr fullpath(AllocArray<WCHAR>(cch));
    GetFullPathNameW(path, cch, fullpath, nullptr);
    // convert to long form
    cch = GetLongPathName(fullpath, nullptr, 0);
    if (!cch) {
        return fullpath.StealData();
    }

    AutoFreeWstr normpath(AllocArray<WCHAR>(cch));
    GetLongPathName(fullpath, normpath, cch);
    if (cch <= MAX_PATH) {
        return normpath.StealData();
    }

    // handle overlong paths: first, try to shorten the path
    cch = GetShortPathName(fullpath, nullptr, 0);
    if (cch && cch <= MAX_PATH) {
        AutoFreeWstr shortpath(AllocArray<WCHAR>(cch));
        GetShortPathName(fullpath, shortpath, cch);
        if (str::Len(path::GetBaseNameTemp(normpath)) + path::GetBaseNameTemp(shortpath) - shortpath < MAX_PATH) {
            // keep the long filename if possible
            *(WCHAR*)path::GetBaseNameTemp(shortpath) = '\0';
            return str::Join(shortpath, path::GetBaseNameTemp(normpath));
        }
        return shortpath.StealData();
    }
    // else mark the path as overlong
    if (str::StartsWith(normpath.Get(), L"\\\\?\\")) {
        return normpath.StealData();
    }
    return str::Join(L"\\\\?\\", normpath);
}

// Normalizes the file path and the converts it into a short form that
// can be used for interaction with non-UNICODE aware applications
WCHAR* ShortPath(const WCHAR* path) {
    AutoFreeWstr normpath(Normalize(path));
    DWORD cch = GetShortPathName(normpath, nullptr, 0);
    if (!cch) {
        return normpath.StealData();
    }
    WCHAR* shortpath = AllocArray<WCHAR>(cch);
    GetShortPathName(normpath, shortpath, cch);
    return shortpath;
}

static bool IsSameFileHandleInformation(BY_HANDLE_FILE_INFORMATION& fi1, BY_HANDLE_FILE_INFORMATION fi2) {
    if (fi1.dwVolumeSerialNumber != fi2.dwVolumeSerialNumber) {
        return false;
    }
    if (fi1.nFileIndexLow != fi2.nFileIndexLow) {
        return false;
    }
    if (fi1.nFileIndexHigh != fi2.nFileIndexHigh) {
        return false;
    }
    if (fi1.nFileSizeLow != fi2.nFileSizeLow) {
        return false;
    }
    if (fi1.nFileSizeHigh != fi2.nFileSizeHigh) {
        return false;
    }
    if (fi1.dwFileAttributes != fi2.dwFileAttributes) {
        return false;
    }
    if (fi1.nNumberOfLinks != fi2.nNumberOfLinks) {
        return false;
    }
    if (!FileTimeEq(fi1.ftLastWriteTime, fi2.ftLastWriteTime)) {
        return false;
    }
    if (!FileTimeEq(fi1.ftCreationTime, fi2.ftCreationTime)) {
        return false;
    }
    return true;
}

// Code adapted from
// http://stackoverflow.com/questions/562701/best-way-to-determine-if-two-path-reference-to-same-file-in-c-c/562830#562830
// Determine if 2 paths point ot the same file...
bool IsSame(const WCHAR* path1, const WCHAR* path2) {
    if (str::EqI(path1, path2)) {
        return true;
    }

    // we assume that if the last part doesn't match, they can't be the same
    const WCHAR* base1 = path::GetBaseNameTemp(path1);
    const WCHAR* base2 = path::GetBaseNameTemp(path2);
    if (!str::EqI(base1, base2)) {
        return false;
    }

    bool isSame = false;
    bool needFallback = true;
    // CreateFile might fail for already opened files
    HANDLE h1 = CreateFileW(path1, 0, 0, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    HANDLE h2 = CreateFileW(path2, 0, 0, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);

    if (h1 != INVALID_HANDLE_VALUE && h2 != INVALID_HANDLE_VALUE) {
        BY_HANDLE_FILE_INFORMATION fi1, fi2;
        if (GetFileInformationByHandle(h1, &fi1) && GetFileInformationByHandle(h2, &fi2)) {
            isSame = IsSameFileHandleInformation(fi1, fi2);
            needFallback = false;
        }
    }

    CloseHandle(h1);
    CloseHandle(h2);

    if (!needFallback) {
        return isSame;
    }

    AutoFreeWstr npath1(Normalize(path1));
    AutoFreeWstr npath2(Normalize(path2));
    // consider the files different, if their paths can't be normalized
    return npath1 && str::EqI(npath1, npath2);
}

bool HasVariableDriveLetter(const char* path) {
    char root[] = R"(?:\)";
    root[0] = (char)toupper(path[0]);
    if (root[0] < 'A' || 'Z' < root[0]) {
        return false;
    }

    uint driveType = GetDriveTypeA(root);
    switch (driveType) {
        case DRIVE_REMOVABLE:
        case DRIVE_CDROM:
        case DRIVE_NO_ROOT_DIR:
            return true;
    }
    return false;
}

bool HasVariableDriveLetter(const WCHAR* path) {
    WCHAR root[] = L"?:\\";
    root[0] = towupper(path[0]);
    if (root[0] < 'A' || 'Z' < root[0]) {
        return false;
    }

    uint driveType = GetDriveTypeW(root);
    switch (driveType) {
        case DRIVE_REMOVABLE:
        case DRIVE_CDROM:
        case DRIVE_NO_ROOT_DIR:
            return true;
    }
    return false;
}

bool IsOnFixedDrive(const WCHAR* path) {
    if (PathIsNetworkPath(path)) {
        return false;
    }

    uint type;
    WCHAR root[MAX_PATH];
    if (GetVolumePathName(path, root, dimof(root))) {
        type = GetDriveType(root);
    } else {
        type = GetDriveType(path);
    }
    return DRIVE_FIXED == type;
}

static bool MatchWildcardsRec(const WCHAR* fileName, const WCHAR* filter) {
#define AtEndOf(str) (*(str) == '\0')
    switch (*filter) {
        case '\0':
        case ';':
            return AtEndOf(fileName);
        case '*':
            filter++;
            while (!AtEndOf(fileName) && !MatchWildcardsRec(fileName, filter)) {
                fileName++;
            }
            return !AtEndOf(fileName) || AtEndOf(filter) || *filter == ';';
        case '?':
            return !AtEndOf(fileName) && MatchWildcardsRec(fileName + 1, filter + 1);
        default:
            return towlower(*fileName) == towlower(*filter) && MatchWildcardsRec(fileName + 1, filter + 1);
    }
#undef AtEndOf
}

/* matches the filename of a path against a list of semicolon
   separated filters as used by the common file dialogs
   (e.g. "*.pdf;*.xps;?.*" will match all PDF and XPS files and
   all filenames consisting of only a single character and
   having any extension) */
bool Match(const WCHAR* path, const WCHAR* filter) {
    path = GetBaseNameTemp(path);
    while (str::FindChar(filter, L';')) {
        if (MatchWildcardsRec(path, filter)) {
            return true;
        }
        filter = str::FindChar(filter, L';') + 1;
    }
    return MatchWildcardsRec(path, filter);
}

bool IsAbsolute(const WCHAR* path) {
    return !PathIsRelative(path);
}

// returns the path to either the %TEMP% directory or a
// non-existing file inside whose name starts with filePrefix
WCHAR* GetTempFilePath(const WCHAR* filePrefix) {
    WCHAR tempDir[MAX_PATH - 14] = {0};
    DWORD res = ::GetTempPathW(dimof(tempDir), tempDir);
    if (!res || res >= dimof(tempDir)) {
        return nullptr;
    }
    if (!filePrefix) {
        return str::Dup(tempDir);
    }
    WCHAR path[MAX_PATH] = {0};
    if (!GetTempFileNameW(tempDir, filePrefix, 0, path)) {
        return nullptr;
    }
    return str::Dup(path);
}

// returns a path to the application module's directory
// with either the given fileName or the module's name
// (module is the EXE or DLL in which path::GetPathOfFileInAppDir resides)
WCHAR* GetPathOfFileInAppDir(const WCHAR* fileName) {
    WCHAR modulePath[MAX_PATH] = {0};
    GetModuleFileName(GetInstance(), modulePath, dimof(modulePath));
    modulePath[dimof(modulePath) - 1] = '\0';
    if (!fileName) {
        return str::Dup(modulePath);
    }
    AutoFreeWstr moduleDir = path::GetDir(modulePath);
    AutoFreeWstr path = path::Join(moduleDir, fileName);
    return path::Normalize(path);
}
} // namespace path

namespace file {

FILE* OpenFILE(const char* path) {
    CrashIf(!path);
    if (!path) {
        return nullptr;
    }
    auto pathW = ToWstrTemp(path);
    return OpenFILE(pathW.Get());
}

ByteSlice ReadFileWithAllocator(const char* filePath, Allocator* allocator) {
#if 0 // OS_WIN
    WCHAR buf[512];
    strconv::Utf8ToWcharBuf(filePath, str::Len(filePath), buf, dimof(buf));
    return ReadFileWithAllocator(buf, fileSizeOut, allocator);
#else
    char* d = nullptr;
    int res;
    FILE* fp = OpenFILE(filePath);
    if (!fp) {
        return {};
    }
    defer {
        fclose(fp);
    };
    res = fseek(fp, 0, SEEK_END);
    if (res != 0) {
        return {};
    }
    size_t size = ftell(fp);
    size_t nRead = 0;
    if (addOverflows<size_t>(size, ZERO_PADDING_COUNT)) {
        goto Error;
    }
    d = (char*)Allocator::AllocZero(allocator, size + ZERO_PADDING_COUNT);
    if (!d) {
        goto Error;
    }
    res = fseek(fp, 0, SEEK_SET);
    if (res != 0) {
        return {};
    }

    nRead = fread((void*)d, 1, size, fp);
    if (nRead != size) {
        int err = ferror(fp);
        int isEof = feof(fp);
        logf("ReadFileWithAllocator: fread() failed, path: '%s', size: %d, nRead: %d, err: %d, isEof: %d\n", filePath,
             (int)size, (int)nRead, err, isEof);
        // we should either get eof or err
        // either way shouldn't happen because we're reading the exact size of file
        // I've seen this in crash reports so maybe the files are over-written
        // between the time I do fseek() and fread()
        CrashIf(!(isEof || (err != 0)));
        goto Error;
    }

    return {(u8*)d, size};
Error:
    Allocator::Free(allocator, (void*)d);
    return {};
#endif
}

ByteSlice ReadFile(std::string_view path) {
    return ReadFileWithAllocator(path.data(), nullptr);
}

ByteSlice ReadFile(const WCHAR* filePath) {
    auto path = ToUtf8Temp(filePath);
    return ReadFileWithAllocator(path.Get(), nullptr);
}

bool WriteFile(const char* filePath, ByteSlice d) {
    auto buf = ToWstrTemp(filePath);
    return WriteFile(buf, d);
}

bool Exists(std::string_view path) {
    WCHAR* wpath = ToWstrTemp(path);
    bool exists = Exists(wpath);
    return exists;
}

HANDLE OpenReadOnly(const WCHAR* filePath) {
    return CreateFileW(filePath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
}

HANDLE OpenReadOnly(std::string_view path) {
    auto filePath = ToWstrTemp(path);
    return CreateFileW(filePath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
}

FILE* OpenFILE(const WCHAR* path) {
    if (!path) {
        return nullptr;
    }
    return _wfopen(path, L"rb");
}

bool Exists(const WCHAR* filePath) {
    if (nullptr == filePath) {
        return false;
    }

    WIN32_FILE_ATTRIBUTE_DATA fileInfo;
    BOOL res = GetFileAttributesEx(filePath, GetFileExInfoStandard, &fileInfo);
    if (0 == res) {
        return false;
    }

    if (fileInfo.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        return false;
    }
    return true;
}

// returns -1 on error (can't use INVALID_FILE_SIZE because it won't cast right)
i64 GetSize(std::string_view filePath) {
    CrashIf(filePath.empty());
    if (filePath.empty()) {
        return -1;
    }

    AutoCloseHandle h = OpenReadOnly(filePath);
    if (!h.IsValid()) {
        return -1;
    }

    // Don't use GetFileAttributesEx to retrieve the file size, as
    // that function doesn't interact well with symlinks, etc.
    LARGE_INTEGER size{};
    BOOL ok = GetFileSizeEx(h, &size);
    if (!ok) {
        return -1;
    }
    return size.QuadPart;
}

ByteSlice ReadFileWithAllocator(const WCHAR* path, Allocator* allocator) {
    auto pathA = ToUtf8Temp(path);
    return ReadFileWithAllocator(pathA.Get(), allocator);
}

// buf must be at least toRead in size (note: it won't be zero-terminated)
// returns -1 for error
int ReadN(const WCHAR* filePath, char* buf, size_t toRead) {
    AutoCloseHandle h = OpenReadOnly(filePath);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }

    ZeroMemory(buf, toRead);
    DWORD nRead = 0;
    BOOL ok = ReadFile(h, (void*)buf, (DWORD)toRead, &nRead, nullptr);
    if (!ok) {
        return -1;
    }
    return (int)nRead;
}

bool WriteFile(const WCHAR* filePath, ByteSlice d) {
    const void* data = d.data();
    size_t dataLen = d.size();
    DWORD access = GENERIC_WRITE;
    DWORD share = FILE_SHARE_READ;
    DWORD flags = FILE_ATTRIBUTE_NORMAL;
    auto fh = CreateFileW(filePath, access, share, nullptr, CREATE_ALWAYS, flags, nullptr);
    if (INVALID_HANDLE_VALUE == fh) {
        return false;
    }
    AutoCloseHandle h(fh);

    DWORD size = 0;
    BOOL ok = WriteFile(h, data, (DWORD)dataLen, &size, nullptr);
    CrashIf(ok && (dataLen != (size_t)size));
    return ok && dataLen == (size_t)size;
}

// Return true if the file wasn't there or was successfully deleted
bool Delete(const WCHAR* filePath) {
    BOOL ok = DeleteFileW(filePath);
    ok |= (GetLastError() == ERROR_FILE_NOT_FOUND);
    if (!ok) {
        LogLastError();
        return false;
    }
    return true;
}

bool Delete(const char* filePathA) {
    if (!filePathA) {
        return false;
    }
    WCHAR* filePath = ToWstrTemp(filePathA);
    BOOL ok = DeleteFileW(filePath);
    ok |= (GetLastError() == ERROR_FILE_NOT_FOUND);
    if (!ok) {
        LogLastError();
        return false;
    }
    return true;
}

bool Copy(const WCHAR* dst, const WCHAR* src, bool dontOverwrite) {
    BOOL ok = CopyFileW(src, dst, (BOOL)dontOverwrite);
    if (!ok) {
        LogLastError();
        return false;
    }
    return true;
}

FILETIME GetModificationTime(const WCHAR* filePath) {
    FILETIME lastMod = {0};
    AutoCloseHandle h(OpenReadOnly(filePath));
    if (h.IsValid()) {
        GetFileTime(h, nullptr, nullptr, &lastMod);
    }
    return lastMod;
}

FILETIME GetModificationTime(const char* filePath) {
    FILETIME lastMod = {0};
    AutoCloseHandle h(OpenReadOnly(filePath));
    if (h.IsValid()) {
        GetFileTime(h, nullptr, nullptr, &lastMod);
    }
    return lastMod;
}

bool SetModificationTime(const WCHAR* filePath, FILETIME lastMod) {
    AutoCloseHandle h(CreateFile(filePath, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr));
    if (INVALID_HANDLE_VALUE == h) {
        return false;
    }
    return SetFileTime(h, nullptr, nullptr, &lastMod);
}

// return true if a file starts with string s of size len
bool StartsWithN(const WCHAR* filePath, const char* s, size_t len) {
    AutoFree buf(AllocArray<char>(len));
    if (!buf) {
        return false;
    }

    if (!ReadN(filePath, buf.Get(), len)) {
        return false;
    }
    return memeq(buf, s, len);
}

// return true if a file starts with null-terminated string s
bool StartsWith(const WCHAR* filePath, const char* s) {
    return file::StartsWithN(filePath, s, str::Len(s));
}

int GetZoneIdentifier(const char* filePath) {
    AutoFreeStr path(str::Join(filePath, ":Zone.Identifier"));
    auto pathW = ToWstrTemp(path.AsView());
    return GetPrivateProfileIntW(L"ZoneTransfer", L"ZoneId", URLZONE_INVALID, pathW);
}

bool SetZoneIdentifier(const char* filePath, int zoneId) {
    AutoFreeStr path(str::Join(filePath, ":Zone.Identifier"));
    AutoFreeWstr id(str::Format(L"%d", zoneId));
    auto pathW = ToWstrTemp(path.AsView());
    return WritePrivateProfileStringW(L"ZoneTransfer", L"ZoneId", id, pathW);
}

bool DeleteZoneIdentifier(const char* filePath) {
    AutoFreeStr path(str::Join(filePath, ":Zone.Identifier"));
    auto pathW = ToWstrTemp(path.AsView());
    return !!DeleteFileW(pathW);
}

} // namespace file

namespace dir {

// TODO: duplicate with path::IsDirectory()
bool Exists(const WCHAR* dir) {
    if (nullptr == dir) {
        return false;
    }

    WIN32_FILE_ATTRIBUTE_DATA fileInfo;
    BOOL res = GetFileAttributesEx(dir, GetFileExInfoStandard, &fileInfo);
    if (0 == res) {
        return false;
    }

    return (fileInfo.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

// Return true if a directory already exists or has been successfully created
bool Create(const WCHAR* dir) {
    BOOL ok = CreateDirectoryW(dir, nullptr);
    if (ok) {
        return true;
    }
    return ERROR_ALREADY_EXISTS == GetLastError();
}

// creates a directory and all its parent directories that don't exist yet
bool CreateAll(const WCHAR* dir) {
    AutoFreeWstr parent(path::GetDir(dir));
    if (!str::Eq(parent, dir) && !Exists(parent)) {
        CreateAll(parent);
    }
    return Create(dir);
}

bool CreateForFile(const WCHAR* path) {
    AutoFreeWstr dir(path::GetDir(path));
    return CreateAll(dir);
}

// remove directory and all its children
bool RemoveAll(const WCHAR* dir) {
    // path must be doubly terminated
    // (https://docs.microsoft.com/en-us/windows/desktop/api/shellapi/ns-shellapi-_shfileopstructa)
    size_t n = str::Len(dir) + 2;
    AutoFreeWstr path = AllocArray<WCHAR>(n);
    str::BufSet(path, n, dir);
    FILEOP_FLAGS flags = FOF_NO_UI;
    uint op = FO_DELETE;
    SHFILEOPSTRUCTW shfo = {nullptr, op, path, nullptr, flags, FALSE, nullptr, nullptr};
    int res = SHFileOperationW(&shfo);
    return res == 0;
}

} // namespace dir

bool FileTimeEq(const FILETIME& a, const FILETIME& b) {
    return a.dwLowDateTime == b.dwLowDateTime && a.dwHighDateTime == b.dwHighDateTime;
}
