/* Copyright 2021 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

bool HasBeenInstalled();
bool IsRunningInPortableMode();
bool IsDllBuild();

WCHAR* AppGenDataFilename(const WCHAR* fileName);
char* AppGenDataFilenameTemp(const char* fileName);
void SetAppDataPath(const WCHAR* path);

void DoAssociateExeWithPdfExtension(HKEY hkey);
bool IsExeAssociatedWithPdfExtension();

WCHAR* AutoDetectInverseSearchCommands(HWND);

bool ExtendedEditWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

void EnsureAreaVisibility(Rect& rect);
Rect GetDefaultWindowPos();
void SaveCallstackLogs();
#if 0
WCHAR* PathForFileInAppDataDir(const WCHAR* fileName);
#endif

WCHAR* FormatFileSize(i64);
WCHAR* FormatFileSizeNoTrans(i64);
