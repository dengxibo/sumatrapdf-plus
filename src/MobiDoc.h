/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

class HuffDicDecompressor;
class PdbReader;

struct MobiDoc {
    char* fileName = nullptr;

    PdbReader* pdbReader = nullptr;

    PdbDocType docType = PdbDocType::Unknown;
    size_t docRecCount = 0;
    int compressionType = 0;
    size_t docUncompressedSize = 0;
    int textEncoding = CP_UTF8;
    size_t docTocIndex = 0;
    u32 mobiFormatVersion = 0;
    u32 ncxIndexRec = (u32)-1;
    u32 kf8SkelIdx = (u32)-1;
    u32 kf8FragIdx = (u32)-1;
    u32 kf8FragIdxUsed = (u32)-1;
    i32* kf8FragInsertPos = nullptr;
    size_t kf8FragInsertPosCount = 0;

    bool multibyte = false;
    size_t trailersCount = 0;
    size_t imageFirstRec = 0; // 0 if no images
    size_t coverImageRec = 0; // 0 if no cover image

    ByteSlice* images = nullptr;

    HuffDicDecompressor* huffDic = nullptr;

    Props props;

    explicit MobiDoc(const char* filePath);

    bool ParseHeader();
    bool LoadDocRecordIntoBuffer(size_t recNo, StrBuilder& strOut);
    size_t DecompressRecords();
    void LoadImages();
    bool LoadImage(size_t imageNo);
    bool LoadForPdbReader(PdbReader* pdbReader);
    bool DecodeExthHeader(const u8* data, size_t dataLen);
    bool BuildKf8FragmentTable();

  public:
    StrBuilder* doc = nullptr;

    size_t imagesCount = 0;

    ~MobiDoc();

    ByteSlice GetHtmlData() const;
    ByteSlice* GetCoverImage();
    ByteSlice* GetImage(size_t imgRecIndex) const;
    ByteSlice* GetImageByResourceIndex(size_t resourceIndex) const;
    const char* GetFileName() const { return fileName; }
    TempStr GetPropertyTemp(const char* name);
    PdbDocType GetDocType() const { return docType; }

    bool HasToc();
    size_t GetTocFilePos() const { return docTocIndex; }
    bool ParseToc(EbookTocVisitor* visitor);
    bool ParseNcxToc(EbookTocVisitor* visitor);
    int ResolveKindlePos(const char* url) const;
    size_t Kf8FragmentCount() const { return kf8FragInsertPosCount; }
    u32 GetKf8SkelIdx() const { return kf8SkelIdx; }
    u32 GetKf8FragIdx() const { return kf8FragIdx; }
    u32 GetKf8FragIdxUsed() const { return kf8FragIdxUsed; }

    static bool IsSupportedFileType(Kind);
    static MobiDoc* CreateFromFile(const char* fileName);
    static MobiDoc* CreateFromStream(IStream* stream);
};
