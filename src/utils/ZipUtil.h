/* Copyright 2021 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

extern "C" {
typedef struct ar_stream_s ar_stream;
typedef struct ar_archive_s ar_archive;
}

class ZipCreator {
    ISequentialStream* stream;
    str::Str centraldir;
    size_t bytesWritten;
    size_t fileCount;

    bool WriteData(const void* data, size_t size);
    bool AddFileData(const char* nameUtf8, const void* data, size_t size, u32 dosdate = 0);

  public:
    explicit ZipCreator(const WCHAR* zipFilePath);
    explicit ZipCreator(ISequentialStream* stream);
    ~ZipCreator();

    ZipCreator(ZipCreator const&) = delete;
    ZipCreator& operator=(ZipCreator const&) = delete;

    bool AddFile(const WCHAR* filePath, const WCHAR* nameInZip = nullptr);
    bool AddFileFromDir(const WCHAR* filePath, const WCHAR* dir);
    bool AddDir(const WCHAR* dirPath, bool recursive = false);
    bool Finish();
};

IStream* OpenDirAsZipStream(const WCHAR* dirPath, bool recursive = false);
