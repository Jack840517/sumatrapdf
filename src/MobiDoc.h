/* Copyright 2021 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

class HuffDicDecompressor;
class PdbReader;

class MobiDoc {
    WCHAR* fileName = nullptr;

    PdbReader* pdbReader = nullptr;

    PdbDocType docType = PdbDocType::Unknown;
    size_t docRecCount = 0;
    int compressionType = 0;
    size_t docUncompressedSize = 0;
    int textEncoding = CP_UTF8;
    size_t docTocIndex = 0;

    bool multibyte = false;
    size_t trailersCount = 0;
    size_t imageFirstRec = 0; // 0 if no images
    size_t coverImageRec = 0; // 0 if no cover image

    ByteSlice* images = nullptr;

    HuffDicDecompressor* huffDic = nullptr;

    struct Metadata {
        DocumentProperty prop;
        char* value;
    };
    Vec<Metadata> props;

    explicit MobiDoc(const WCHAR* filePath);

    bool ParseHeader();
    bool LoadDocRecordIntoBuffer(size_t recNo, str::Str& strOut);
    void LoadImages();
    bool LoadImage(size_t imageNo);
    bool LoadDocument(PdbReader* pdbReader);
    bool DecodeExthHeader(const u8* data, size_t dataLen);

  public:
    str::Str* doc = nullptr;

    size_t imagesCount = 0;

    ~MobiDoc();

    [[nodiscard]] ByteSlice GetHtmlData() const;
    ByteSlice* GetCoverImage();
    [[nodiscard]] ByteSlice* GetImage(size_t imgRecIndex) const;
    [[nodiscard]] const WCHAR* GetFileName() const {
        return fileName;
    }
    WCHAR* GetProperty(DocumentProperty prop);
    [[nodiscard]] PdbDocType GetDocType() const {
        return docType;
    }

    bool HasToc();
    bool ParseToc(EbookTocVisitor* visitor);

    static bool IsSupportedFileType(Kind);
    static MobiDoc* CreateFromFile(const WCHAR* fileName);
    static MobiDoc* CreateFromStream(IStream* stream);
};
