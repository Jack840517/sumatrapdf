/* Copyright 2021 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

/* How to use:

DirIter di(dir, recursive);
for (const WCHAR *path = di.First(); path; path = di.Next()) {
    // process path
}

*/
class DirIter {
    bool recursive = false;

    WStrVec dirsToVisit;
    std::wstring_view startDir; // we don't own the memory
    AutoFreeWstr currDir;
    bool foundNext = false;

    bool StartDirIter(std::wstring_view dir);
    bool TryNextDir();

  public:
    AutoFreeWstr currPath;
    HANDLE currFindHandle = nullptr;
    WIN32_FIND_DATAW currFindData{};

    explicit DirIter(std::wstring_view dir, bool recur = false) {
        startDir = dir;
        recursive = recur;
    }
    ~DirIter() {
        FindClose(currFindHandle);
    }

    const WCHAR* First();
    const WCHAR* Next();
};

bool CollectPathsFromDirectory(const WCHAR* pattern, WStrVec& paths, bool dirsInsteadOfFiles = false);

bool CollectFilesFromDirectory(std::string_view dir, VecStr& files,
                               const std::function<bool(std::string_view path)>& fileMatches);
