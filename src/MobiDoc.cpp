/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#include "utils/BaseUtil.h"
#include "utils/BitReader.h"
#include "utils/ByteOrderDecoder.h"
#include "utils/ScopedWin.h"
#include "utils/FileUtil.h"
#include "utils/GuessFileType.h"
#include "utils/GdiPlusUtil.h"

#include "wingui/UIModels.h"

#include "GumboHelpers.h"

#include "DocProperties.h"
#include "DocController.h"
#include "EngineBase.h"
#include "EbookBase.h"
#include "PalmDbReader.h"
#include "MobiDoc.h"

#include "utils/Log.h"

constexpr size_t kInvalidSize = (size_t)-1;

// Parse mobi format http://wiki.mobileread.com/wiki/MOBI
#define COMPRESSION_NONE 1
#define COMPRESSION_PALM 2
#define COMPRESSION_HUFF 17480
#define COMPRESSION_UNSUPPORTED_DRM -1

#define ENCRYPTION_NONE 0
#define ENCRYPTION_OLD 1
#define ENCRYPTION_NEW 2

struct PalmDocHeader {
    u16 compressionType = 0;
    u16 reserved1 = 0;
    u32 uncompressedDocSize = 0;
    u16 recordsCount = 0;
    u16 maxRecSize = 0; // usually (always?) 4096
    // if it's palmdoc, we have currPos, if mobi, encrType/reserved2
    union {
        u32 currPos = 0;
        struct {
            u16 encrType;
            u16 reserved2;
        } mobi;
    };
};
#define kPalmDocHeaderLen 16

// http://wiki.mobileread.com/wiki/MOBI#PalmDOC_Header
static void DecodePalmDocHeader(const u8* buf, PalmDocHeader* hdr) {
    ByteOrderDecoder d(buf, kPalmDocHeaderLen, ByteOrderDecoder::BigEndian);
    hdr->compressionType = d.UInt16();
    hdr->reserved1 = d.UInt16();
    hdr->uncompressedDocSize = d.UInt32();
    hdr->recordsCount = d.UInt16();
    hdr->maxRecSize = d.UInt16();
    hdr->currPos = d.UInt32();

    ReportIf(kPalmDocHeaderLen != d.Offset());
}

// http://wiki.mobileread.com/wiki/MOBI#MOBI_Header
// Note: the real length of MobiHeader is in MobiHeader.hdrLen. This is just
// the size of the struct
#define kMobiHeaderLen 232
// length up to MobiHeader.exthFlags
#define kMobiHeaderMinLen 116
struct MobiHeader {
    char id[4];
    u32 hdrLen; // including 4 id bytes
    u32 type;
    u32 textEncoding;
    u32 uniqueId;
    u32 mobiFormatVersion;
    u32 ortographicIdxRec; // -1 if no ortographics index
    u32 inflectionIdxRec;
    u32 namesIdxRec;
    u32 keysIdxRec;
    u32 extraIdx0Rec;
    u32 extraIdx1Rec;
    u32 extraIdx2Rec;
    u32 extraIdx3Rec;
    u32 extraIdx4Rec;
    u32 extraIdx5Rec;
    u32 firstNonBookRec;
    u32 fullNameOffset; // offset in record 0
    u32 fullNameLen;
    // Low byte is main language e.g. 09 = English,
    // next byte is dialect, 08 = British, 04 = US.
    // Thus US English is 1033, UK English is 2057
    u32 locale;
    u32 inputDictLanguage;
    u32 outputDictLanguage;
    u32 minRequiredMobiFormatVersion;
    u32 imageFirstRec;
    u32 huffmanFirstRec;
    u32 huffmanRecCount;
    u32 huffmanTableOffset;
    u32 huffmanTableLen;
    u32 exthFlags; // bitfield. if bit 6 (0x40) is set => there's an EXTH record
    char reserved1[32];
    u32 drmOffset;       // -1 if no drm info
    u32 drmEntriesCount; // -1 if no drm
    u32 drmSize;
    u32 drmFlags;
    char reserved2[62];
    // A set of binary flags, some of which indicate extra data at the end of each text block.
    // This only seems to be valid for Mobipocket format version 5 and 6 (and higher?), when
    // the header length is 228 (0xE4) or 232 (0xE8).
    u16 extraDataFlags;
    i32 indxRec;
};

static_assert(kMobiHeaderLen == sizeof(MobiHeader), "wrong size of MobiHeader structure");

// Uncompress source data compressed with PalmDoc compression into a buffer.
// http://wiki.mobileread.com/wiki/PalmDOC#Format
// Returns false on decoding errors
static bool PalmdocUncompress(const u8* src, size_t srcLen, StrBuilder& dst) {
    const u8* srcEnd = src + srcLen;
    while (src < srcEnd) {
        u8 c = *src++;
        if ((c >= 1) && (c <= 8)) {
            if (src + c > srcEnd) {
                return false;
            }
            dst.Append(src, c);
            src += c;
        } else if (c < 128) {
            dst.AppendChar((char)c);
        } else if (c < 192) {
            if (src + 1 > srcEnd) {
                return false;
            }
            u16 c2 = (c << 8) | (u8)*src++;
            u16 back = (c2 >> 3) & 0x07ff;
            if (back > dst.size() || 0 == back) {
                return false;
            }
            for (u8 n = (c2 & 7) + 3; n > 0; n--) {
                char ctmp = dst.at(dst.size() - back);
                dst.AppendChar(ctmp);
            }
        } else {
            // c >= 192
            dst.AppendChar(' ');
            dst.AppendChar((char)(c ^ 0x80));
        }
    }

    return true;
}

#define kHuffHeaderLen 24
struct HuffHeader {
    char id[4]; // "HUFF"
    u32 hdrLen; // should be 24
    // offset of 256 4-byte elements of cache data, in big endian
    u32 cacheOffset; // should be 24 as well
    // offset of 64 4-byte elements of base table data, in big endian
    u32 baseTableOffset; // should be 24 + 1024
    // like cacheOffset except data is in little endian
    u32 cacheLEOffset; // should be 24 + 1024 + 256
    // like baseTableOffset except data is in little endian
    u32 baseTableLEOffset; // should be 24 + 1024 + 256 + 1024
};
static_assert(kHuffHeaderLen == sizeof(HuffHeader), "wrong size of HuffHeader structure");

#define kCdicHeaderLen 16
struct CdicHeader {
    char id[4]; // "CIDC"
    u32 hdrLen; // should be 16
    u32 unknown;
    u32 codeLen;
};

static_assert(kCdicHeaderLen == sizeof(CdicHeader), "wrong size of CdicHeader structure");

#define kCacheItemCount 256
#define kCacheDataLen (kCacheItemCount * sizeof(u32))
#define kBaseTableItemCount 64
#define kBaseTableDataLen (kBaseTableItemCount * sizeof(u32))

#define kHuffRecordMinLen (kHuffHeaderLen + kCacheDataLen + kBaseTableDataLen)
#define kHuffRecordLen (kHuffHeaderLen + 2 * kCacheDataLen + 2 * kBaseTableDataLen)

#define kCdicsMax 32

class HuffDicDecompressor {
    u32 cacheTable[kCacheItemCount]{};
    u32 baseTable[kBaseTableItemCount]{};

    size_t dictsCount = 0;
    // owned by the creator (in our case: by the PdbReader)
    u8* dicts[kCdicsMax]{};
    u32 dictSize[kCdicsMax]{};

    u32 codeLength = 0;

    int recursionDepth = 0;

  public:
    HuffDicDecompressor();

    bool SetHuffData(u8* huffData, size_t huffDataLen);
    bool AddCdicData(u8* cdicData, u32 cdicDataLen);
    bool Decompress(u8* src, size_t srcSize, StrBuilder& dst);
    bool DecodeOne(u32 code, StrBuilder& dst);
};

HuffDicDecompressor::HuffDicDecompressor() {}

bool HuffDicDecompressor::DecodeOne(u32 code, StrBuilder& dst) {
    u16 dict = (u16)(code >> codeLength);
    if (dict >= dictsCount) {
        logf("invalid dict value\n");
        return false;
    }
    code &= ((1 << (codeLength)) - 1);
    u16 offset = UInt16BE(dicts[dict] + code * 2);

    if ((u32)offset + 2 > dictSize[dict]) {
        logf("invalid offset\n");
        return false;
    }
    u16 symLen = UInt16BE(dicts[dict] + offset);
    u8* p = dicts[dict] + offset + 2;
    if ((u32)(symLen & 0x7fff) > dictSize[dict] - offset - 2) {
        logf("invalid symLen\n");
        return false;
    }

    if (!(symLen & 0x8000)) {
        if (recursionDepth > 20) {
            logf("infinite recursion\n");
            return false;
        }
        recursionDepth++;
        if (!Decompress(p, symLen, dst)) {
            recursionDepth--;
            return false;
        }
        recursionDepth--;
    } else {
        symLen &= 0x7fff;
        if (symLen > 127) {
            logf("symLen too big\n");
            return false;
        }
        dst.Append((char*)p, symLen);
    }
    return true;
}

bool HuffDicDecompressor::Decompress(u8* src, size_t srcSize, StrBuilder& dst) {
    u32 bitsConsumed = 0;
    u32 bits = 0;
    u32 loopCount = 0;

    BitReader br(src, srcSize);

    for (;;) {
        loopCount++;
        if (bitsConsumed > br.BitsLeft()) {
            logf("not enough data\n");
            return false;
        }
        br.Eat(bitsConsumed);
        if (0 == br.BitsLeft()) {
            break;
        }

        bits = br.Peek(32);
        if (br.BitsLeft() < 8 && 0 == bits) {
            break;
        }
        u32 v = cacheTable[bits >> 24];
        u32 codeLen = v & 0x1f;
        if (!codeLen) {
            logf("corrupted table, zero code len\n");
            return false;
        }
        bool isTerminal = (v & 0x80) != 0;

        u32 code;
        if (isTerminal) {
            code = (v >> 8) - (bits >> (32 - codeLen));
        } else {
            u32 baseVal;
            codeLen -= 1;
            do {
                codeLen++;
                if (codeLen > 32) {
                    logf("code len > 32 bits\n");
                    return false;
                }
                baseVal = baseTable[codeLen * 2 - 2];
                code = (bits >> (32 - codeLen));
            } while (baseVal > code);
            code = baseTable[codeLen * 2 - 1] - (bits >> (32 - codeLen));
        }

        if (!DecodeOne(code, dst)) {
            return false;
        }
        bitsConsumed = codeLen;
    }

    if (br.BitsLeft() > 0 && 0 != bits) {
        logf("compressed data left\n");
    }
    return true;
}

static void ReadHuffReader(HuffHeader& huffHdr, ByteOrderDecoder& d) {
    d.Bytes(huffHdr.id, 4);
    huffHdr.hdrLen = d.UInt32();
    huffHdr.cacheOffset = d.UInt32();
    huffHdr.baseTableOffset = d.UInt32();
    huffHdr.cacheLEOffset = d.UInt32();
    huffHdr.baseTableLEOffset = d.UInt32();
    ReportIf(d.Offset() != kHuffHeaderLen);
}

bool HuffDicDecompressor::SetHuffData(u8* huffData, size_t huffDataLen) {
    // for now catch cases where we don't have both big endian and little endian
    // versions of the data
    // ReportIf(kHuffRecordLen != huffDataLen);
    // but conservatively assume we only need big endian version
    if (huffDataLen < kHuffRecordMinLen) {
        return false;
    }

    ByteOrderDecoder d(huffData, huffDataLen, ByteOrderDecoder::BigEndian);
    HuffHeader huffHdr;
    ReadHuffReader(huffHdr, d);

    if (!str::EqN(huffHdr.id, "HUFF", 4)) {
        return false;
    }

    ReportIf(huffHdr.hdrLen != kHuffHeaderLen);
    if (huffHdr.hdrLen != kHuffHeaderLen) {
        return false;
    }
    if (huffHdr.cacheOffset != kHuffHeaderLen) {
        return false;
    }
    if (huffHdr.baseTableOffset != huffHdr.cacheOffset + kCacheDataLen) {
        return false;
    }
    // we conservatively use the big-endian version of the data,
    for (int i = 0; i < kCacheItemCount; i++) {
        cacheTable[i] = d.UInt32();
    }
    for (int i = 0; i < kBaseTableItemCount; i++) {
        baseTable[i] = d.UInt32();
    }
    ReportIf(d.Offset() != kHuffRecordMinLen);
    return true;
}

bool HuffDicDecompressor::AddCdicData(u8* cdicData, u32 cdicDataLen) {
    if (dictsCount >= kCdicsMax) {
        return false;
    }
    if (cdicDataLen < kCdicHeaderLen) {
        return false;
    }
    if (!str::EqN("CDIC", (char*)cdicData, 4)) {
        return false;
    }
    u32 hdrLen = UInt32BE(cdicData + 4);
    u32 codeLen = UInt32BE(cdicData + 12);
    if (0 == codeLength) {
        codeLength = codeLen;
    } else {
        ReportIf(codeLen != codeLength);
        codeLength = std::min(codeLength, codeLen);
    }
    ReportIf(hdrLen != kCdicHeaderLen);
    if (hdrLen != kCdicHeaderLen) {
        return false;
    }
    u32 size = cdicDataLen - hdrLen;

    u32 maxSize = 2u * (1u << codeLength);
    if (maxSize >= size) {
        return false;
    }
    dicts[dictsCount] = cdicData + hdrLen;
    dictSize[dictsCount] = size;
    ++dictsCount;
    return true;
}

static void DecodeMobiDocHeader(const u8* buf, size_t bufLen, MobiHeader* hdr) {
    memset(hdr, 0, sizeof(MobiHeader));
    hdr->drmEntriesCount = (u32)-1;

    size_t decLen = std::min(bufLen, (size_t)kMobiHeaderLen);
    ByteOrderDecoder d(buf, decLen, ByteOrderDecoder::BigEndian);
    d.Bytes(hdr->id, 4);
    hdr->hdrLen = d.UInt32();
    hdr->type = d.UInt32();
    hdr->textEncoding = d.UInt32();
    hdr->uniqueId = d.UInt32();
    hdr->mobiFormatVersion = d.UInt32();
    hdr->ortographicIdxRec = d.UInt32();
    hdr->inflectionIdxRec = d.UInt32();
    hdr->namesIdxRec = d.UInt32();
    hdr->keysIdxRec = d.UInt32();
    hdr->extraIdx0Rec = d.UInt32();
    hdr->extraIdx1Rec = d.UInt32();
    hdr->extraIdx2Rec = d.UInt32();
    hdr->extraIdx3Rec = d.UInt32();
    hdr->extraIdx4Rec = d.UInt32();
    hdr->extraIdx5Rec = d.UInt32();
    hdr->firstNonBookRec = d.UInt32();
    hdr->fullNameOffset = d.UInt32();
    hdr->fullNameLen = d.UInt32();
    hdr->locale = d.UInt32();
    hdr->inputDictLanguage = d.UInt32();
    hdr->outputDictLanguage = d.UInt32();
    hdr->minRequiredMobiFormatVersion = d.UInt32();
    hdr->imageFirstRec = d.UInt32();
    hdr->huffmanFirstRec = d.UInt32();
    hdr->huffmanRecCount = d.UInt32();
    hdr->huffmanTableOffset = d.UInt32();
    hdr->huffmanTableLen = d.UInt32();
    hdr->exthFlags = d.UInt32();
    ReportIf(kMobiHeaderMinLen != d.Offset());

    if (hdr->hdrLen < kMobiHeaderMinLen + 48) {
        return;
    }

    d.Bytes(hdr->reserved1, 32);
    hdr->drmOffset = d.UInt32();
    hdr->drmEntriesCount = d.UInt32();
    hdr->drmSize = d.UInt32();
    hdr->drmFlags = d.UInt32();

    if (hdr->hdrLen < 228) { // magic number at which extraDataFlags becomes valid
        return;
    }

    d.Bytes(hdr->reserved2, 62);
    hdr->extraDataFlags = d.UInt16();
    if (hdr->hdrLen >= 232) {
        hdr->indxRec = d.UInt32();
    }
}

static bool IsValidCompression(int comprType) {
    return (COMPRESSION_NONE == comprType) || (COMPRESSION_PALM == comprType) || (COMPRESSION_HUFF == comprType);
}

MobiDoc::MobiDoc(const char* filePath) {
    docTocIndex = kInvalidSize;
    fileName = str::Dup(filePath);
}

MobiDoc::~MobiDoc() {
    free(fileName);
    free(images);
    free(kf8FragInsertPos);
    delete huffDic;
    delete doc;
    delete pdbReader;
}

bool MobiDoc::ParseHeader() {
    ReportIf(!pdbReader);
    if (!pdbReader) {
        return false;
    }

    if (pdbReader->GetRecordCount() == 0) {
        return false;
    }

    docType = GetPdbDocType(pdbReader->GetDbType());
    if (PdbDocType::Unknown == docType) {
        logf("unknown pdb type/creator\n");
        return false;
    }

    auto rec = pdbReader->GetRecord(0);
    u8* firstRecData = rec.data();
    size_t recSize = rec.size();
    if (!firstRecData || recSize < kPalmDocHeaderLen) {
        log("failed to read record 0\n");
        return false;
    }

    PalmDocHeader palmDocHdr;
    DecodePalmDocHeader(firstRecData, &palmDocHdr);
    compressionType = palmDocHdr.compressionType;
    if (!IsValidCompression(compressionType)) {
        logf("MobiDoc::ParseHeader: unknown compression type %d\n", (int)compressionType);
        return false;
    }
    if (PdbDocType::Mobipocket == docType) {
        // TODO: this needs to be surfaced to the client so
        // that we can show the right error message
        if (palmDocHdr.mobi.encrType != ENCRYPTION_NONE) {
            logf("encryption is unsupported\n");
            return false;
        }
    }
    docRecCount = palmDocHdr.recordsCount;
    if (docRecCount == pdbReader->GetRecordCount()) {
        // catch the case where a broken document has an off-by-one error
        // cf. https://code.google.com/p/sumatrapdf/issues/detail?id=2529
        docRecCount--;
    }
    docUncompressedSize = palmDocHdr.uncompressedDocSize;

    if (kPalmDocHeaderLen == recSize) {
        // TODO: calculate imageFirstRec / imagesCount
        return PdbDocType::Mobipocket != docType;
    }
    if (kPalmDocHeaderLen + kMobiHeaderMinLen > recSize) {
        logf("not enough data for decoding MobiHeader\n");
        // id and hdrLen
        return false;
    }

    MobiHeader mobiHdr;
    size_t mobiDataLen = recSize - kPalmDocHeaderLen;
    DecodeMobiDocHeader(firstRecData + kPalmDocHeaderLen, mobiDataLen, &mobiHdr);
    if (!str::EqN("MOBI", mobiHdr.id, 4)) {
        logf("MobiHeader.id is not 'MOBI'\n");
        return false;
    }
    if (mobiHdr.drmEntriesCount != (u32)-1) {
        logf("DRM is unsupported\n");
        // load an empty document and display a warning
        compressionType = COMPRESSION_UNSUPPORTED_DRM;
        char* v = strconv::WStrToCodePage(mobiHdr.textEncoding, L"DRM");
        AddProp(props, kPropUnsupportedFeatures, v);
        str::Free(v);
    }
    textEncoding = mobiHdr.textEncoding;
    mobiFormatVersion = mobiHdr.mobiFormatVersion;
    // NCX (table of contents) index record. MOBI header offset 0xE4 (the
    // record-relative 0xF4 used by KindleUnpack). (u32)-1 means "no NCX".
    if ((u32)mobiHdr.indxRec != (u32)-1 && mobiHdr.indxRec != 0) {
        ncxIndexRec = (u32)mobiHdr.indxRec;
    }
    if (mobiHdr.hdrLen >= 236 && mobiDataLen >= 240) {
        const u8* mobiBase = firstRecData + kPalmDocHeaderLen;
        ByteOrderDecoder kd(mobiBase + 232, 8, ByteOrderDecoder::BigEndian);
        kf8SkelIdx = kd.UInt32();
        kf8FragIdx = kd.UInt32();
    }

    if (pdbReader->GetRecordCount() > mobiHdr.imageFirstRec) {
        imageFirstRec = mobiHdr.imageFirstRec;
        if (0 == imageFirstRec) {
            // I don't think this should ever happen but I've seen it
            imagesCount = 0;
        } else {
            imagesCount = pdbReader->GetRecordCount() - imageFirstRec;
        }
    }
    if (kPalmDocHeaderLen + (size_t)mobiHdr.hdrLen > recSize) {
        logf("MobiHeader too big\n");
        return false;
    }

    bool hasExtraFlags = (mobiHdr.hdrLen >= 228); // TODO: also only if mobiFormatVersion >= 5?
    if (hasExtraFlags) {
        u16 flags = mobiHdr.extraDataFlags;
        multibyte = ((flags & 1) != 0);
        while (flags > 1) {
            if (0 != (flags & 2)) {
                trailersCount++;
            }
            flags = flags >> 1;
        }
    }

    if (COMPRESSION_HUFF == compressionType) {
        ReportIf(PdbDocType::Mobipocket != docType);
        rec = pdbReader->GetRecord(mobiHdr.huffmanFirstRec);
        size_t huffRecSize = rec.size();
        u8* recData = rec.data();
        if (!recData) {
            return false;
        }
        ReportIf(nullptr != huffDic);
        huffDic = new HuffDicDecompressor();
        if (!huffDic->SetHuffData((u8*)recData, huffRecSize)) {
            return false;
        }
        size_t cdicsCount = mobiHdr.huffmanRecCount - 1;
        if (cdicsCount > kCdicsMax) {
            logf("MobiDoc::ParseHeader: cdicsCount: %d, kCdicsMax: %d\n", (int)cdicsCount, kCdicsMax);
            ReportDebugIf(true);
            return false;
        }
        for (size_t i = 0; i < cdicsCount; i++) {
            rec = pdbReader->GetRecord(mobiHdr.huffmanFirstRec + 1 + i);
            recData = rec.data();
            huffRecSize = rec.size();
            if (!recData) {
                return false;
            }
            if (huffRecSize > (u32)-1) {
                return false;
            }
            if (!huffDic->AddCdicData((u8*)recData, (u32)huffRecSize)) {
                return false;
            }
        }
    }

    if ((mobiHdr.exthFlags & 0x40)) {
        u32 offset = kPalmDocHeaderLen + mobiHdr.hdrLen;
        DecodeExthHeader(firstRecData + offset, recSize - offset);
    }

    LoadImages();
    return true;
}

bool MobiDoc::DecodeExthHeader(const u8* data, size_t dataLen) {
    if (dataLen < 12 || !memeq(data, "EXTH", 4)) {
        return false;
    }

    ByteOrderDecoder d(data, dataLen, ByteOrderDecoder::BigEndian);
    d.Skip(4);
    u32 hdrLen = d.UInt32();
    u32 count = d.UInt32();
    if (hdrLen > dataLen) {
        return false;
    }

    for (u32 i = 0; i < count; i++) {
        if (d.Offset() > dataLen - 8) {
            return false;
        }
        u32 type = d.UInt32();
        u32 length = d.UInt32();
        if (length < 8 || length > dataLen - d.Offset() + 8) {
            return false;
        }
        d.Skip(length - 8);

        const char* prop;
        switch (type) {
            case 100:
                prop = kPropAuthor;
                break;
            case 105:
                prop = kPropSubject;
                break;
            case 106:
                prop = kPropCreationDate;
                break;
            case 108:
                prop = kPropCreatorApp;
                break;
            case 109:
                prop = kPropCopyright;
                break;
            case 201:
                if (length == 12 && imageFirstRec) {
                    d.Unskip(4);
                    coverImageRec = imageFirstRec + d.UInt32();
                }
                continue;
            case 503:
                prop = kPropTitle;
                break;
            default:
                continue;
        }
        TempStr value = str::DupTemp((char*)(data + d.Offset() - length + 8), length - 8);
        if (!str::IsEmpty(value)) {
            AddProp(props, prop, value);
        }
    }

    return true;
}

#define EOF_REC 0xe98e0d0a
#define FLIS_REC 0x464c4953 // 'FLIS'
#define FCIS_REC 0x46434953 // 'FCIS
#define FDST_REC 0x46445354 // 'FDST'
#define DATP_REC 0x44415450 // 'DATP'
#define SRCS_REC 0x53524353 // 'SRCS'
#define VIDE_REC 0x56494445 // 'VIDE'
#define RESC_REC 0x52455343 // 'RESC'

static bool IsEofRecord(const ByteSlice& d) {
    return (4 == d.size()) && (EOF_REC == UInt32BE(d.data()));
}

static bool KnownNonImageRec(const ByteSlice& d) {
    if (d.size() < 4) {
        return false;
    }
    u32 sig = UInt32BE(d.data());

    switch (sig) {
        case FLIS_REC:
        case FCIS_REC:
        case FDST_REC:
        case DATP_REC:
        case SRCS_REC:
        case VIDE_REC:
        case RESC_REC:
            return true;
    }
    return false;
}

static bool KnownImageFormat(const ByteSlice& d) {
    Kind kind = GuessFileTypeFromContent(d);
    return kind != nullptr;
}

// return false if we should stop loading images (because we
// encountered eof record or ran out of memory)
bool MobiDoc::LoadImage(size_t imageNo) {
    size_t imageRec = imageFirstRec + imageNo;

    auto rec = pdbReader->GetRecord(imageRec);
    if (rec.Size() < 4) {
        return false;
    }
    if (IsEofRecord(rec)) {
        return false;
    }
    if (KnownNonImageRec(rec)) {
        return true;
    }
    if (!KnownImageFormat(rec)) {
        u32 sig = UInt32BE(rec.data());
        logf("MobiDoc::LoadImage: unknown record type 0x%08X\n", sig);
        return true;
    }
    images[imageNo] = rec;
    return true;
}

void MobiDoc::LoadImages() {
    if (0 == imagesCount) {
        return;
    }
    images = AllocArray<ByteSlice>(imagesCount);

    for (size_t i = 0; i < imagesCount; i++) {
        if (!LoadImage(i)) {
            return;
        }
    }
}

// imgRecIndex corresponds to recindex attribute of <img> tag
// as far as I can tell, this means: it starts at 1
// returns nullptr if there is no image (e.g. it's not a format we
// recognize)
ByteSlice* MobiDoc::GetImage(size_t imgRecIndex) const {
    if ((imgRecIndex > imagesCount) || (imgRecIndex < 1)) {
        return nullptr;
    }
    --imgRecIndex;
    if (images[imgRecIndex].empty()) {
        return nullptr;
    }
    return &images[imgRecIndex];
}

// KF8/AZW3 images use src="kindle:embed:XXXX" where XXXX is a base-32 resource index
// into the dense list of image records (skipping FLIS/FDST/etc.).
ByteSlice* MobiDoc::GetImageByResourceIndex(size_t resourceIndex) const {
    size_t seen = 0;
    for (size_t i = 0; i < imagesCount; i++) {
        if (images[i].empty()) {
            continue;
        }
        if (seen == resourceIndex) {
            return &images[i];
        }
        seen++;
    }
    return nullptr;
}

ByteSlice* MobiDoc::GetCoverImage() {
    if (!coverImageRec || coverImageRec < imageFirstRec) {
        return nullptr;
    }
    size_t imageNo = coverImageRec - imageFirstRec;
    if (imageNo >= imagesCount || images[imageNo].empty()) {
        return nullptr;
    }
    return &images[imageNo];
}

// each record can have extra data at the end, which we must discard
// returns kInvalidSize on error
static size_t GetRealRecordSize(const u8* recData, size_t recLen, size_t trailersCount, bool multibyte) {
    for (size_t i = 0; i < trailersCount; i++) {
        if (recLen < 4) {
            return kInvalidSize;
        }
        u32 n = 0;
        for (size_t j = 0; j < 4; j++) {
            u8 v = recData[recLen - 4 + j];
            if (0 != (v & 0x80)) {
                n = 0;
            }
            n = (n << 7) | (v & 0x7f);
        }
        if (n > recLen) {
            return kInvalidSize;
        }
        recLen -= n;
    }

    if (multibyte) {
        if (0 == recLen) {
            return kInvalidSize;
        }
        u8 n = (recData[recLen - 1] & 3) + 1;
        if (n > recLen) {
            return kInvalidSize;
        }
        recLen -= n;
    }

    return recLen;
}

// Load a given record of a document into strOut, uncompressing if necessary.
// Returns false if error.
bool MobiDoc::LoadDocRecordIntoBuffer(size_t recNo, StrBuilder& strOut) {
    auto rec = pdbReader->GetRecord(recNo);
    u8* recData = rec.data();
    if (nullptr == recData) {
        return false;
    }
    size_t recSize = GetRealRecordSize((const u8*)recData, rec.size(), trailersCount, multibyte);
    if (kInvalidSize == recSize) {
        return false;
    }

    if (COMPRESSION_NONE == compressionType) {
        strOut.Append((const char*)recData, recSize);
        return true;
    }
    if (COMPRESSION_PALM == compressionType) {
        bool ok = PalmdocUncompress(recData, recSize, strOut);
        if (!ok) {
            logf("PalmDoc decompression failed\n");
        }
        return ok;
    }
    if (COMPRESSION_HUFF == compressionType && huffDic) {
        bool ok = huffDic->Decompress((u8*)recData, recSize, strOut);
        if (!ok) {
            logf("HuffDic decompression failed\n");
        }
        return ok;
    }
    if (COMPRESSION_UNSUPPORTED_DRM == compressionType) {
        // ensure a single blank page
        if (1 == recNo) {
            strOut.Append("&nbsp;");
        }
        return true;
    }

    CrashMe();
    return false;
}

// Parallel decompression of the MOBI/KF8 text records.
//
// The text records are independently compressed (PalmDoc or HUFF/CDIC), and
// both PdbReader::GetRecord() and the decompressors only read shared, immutable
// state (the record table, the file bytes, the Huff dictionary/cache tables).
// So we can decompress a disjoint, contiguous range of records on each worker
// thread into its own buffer, then concatenate the buffers in record order to
// reproduce the exact same byte stream the serial loop would have produced.
struct MobiDecompressChunk {
    MobiDoc* doc = nullptr;
    size_t firstRec = 0; // inclusive, 1-based record number
    size_t lastRec = 0;  // inclusive
    StrBuilder* out = nullptr;
    size_t nFailed = 0;
};

static DWORD WINAPI MobiDecompressChunkThread(void* arg) {
    auto* c = (MobiDecompressChunk*)arg;
    for (size_t i = c->firstRec; i <= c->lastRec; i++) {
        if (!c->doc->LoadDocRecordIntoBuffer(i, *c->out)) {
            c->nFailed++;
        }
    }
    return 0;
}

// decompress all docRecCount records into *doc, using multiple threads when the
// book is large enough to benefit. Returns the number of records that failed.
size_t MobiDoc::DecompressRecords() {
    size_t nFailed = 0;

    size_t nThreads = 1;
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    if (si.dwNumberOfProcessors > 1) {
        nThreads = si.dwNumberOfProcessors;
    }
    if (nThreads > 8) {
        nThreads = 8;
    }
    // threading overhead isn't worth it for small books
    const size_t kMinRecsForParallel = 64;
    if (nThreads > docRecCount / 32) {
        nThreads = docRecCount / 32;
    }
    if (docRecCount < kMinRecsForParallel || nThreads <= 1) {
        for (size_t i = 1; i <= docRecCount; i++) {
            if (!LoadDocRecordIntoBuffer(i, *doc)) {
                nFailed++;
            }
        }
        return nFailed;
    }

    Vec<MobiDecompressChunk*> chunks;
    HANDLE threads[8]{};
    size_t nLaunched = 0;
    size_t perChunk = docRecCount / nThreads;
    // approximate per-chunk output size to minimize reallocs
    size_t capHint = (docUncompressedSize / nThreads) + 64 * 1024;
    size_t recStart = 1;
    for (size_t t = 0; t < nThreads; t++) {
        size_t recEnd = (t == nThreads - 1) ? docRecCount : (recStart + perChunk - 1);
        auto* c = new MobiDecompressChunk();
        c->doc = this;
        c->firstRec = recStart;
        c->lastRec = recEnd;
        c->out = new StrBuilder(capHint);
        chunks.Append(c);
        recStart = recEnd + 1;
    }

    for (size_t t = 0; t < chunks.size(); t++) {
        HANDLE h = CreateThread(nullptr, 0, MobiDecompressChunkThread, chunks[t], 0, nullptr);
        if (h) {
            threads[nLaunched++] = h;
        } else {
            // couldn't spawn: decompress this chunk inline
            MobiDecompressChunkThread(chunks[t]);
        }
    }
    if (nLaunched > 0) {
        WaitForMultipleObjects((DWORD)nLaunched, threads, TRUE, INFINITE);
        for (size_t t = 0; t < nLaunched; t++) {
            CloseHandle(threads[t]);
        }
    }

    // concatenate in record order, exactly reproducing the serial output
    for (size_t t = 0; t < chunks.size(); t++) {
        MobiDecompressChunk* c = chunks[t];
        doc->Append(*c->out);
        nFailed += c->nFailed;
        delete c->out;
        delete c;
    }
    return nFailed;
}

bool MobiDoc::LoadForPdbReader(PdbReader* pdbReader) {
    this->pdbReader = pdbReader;
    if (!ParseHeader()) {
        return false;
    }

    ReportIf(doc != nullptr);
    doc = new StrBuilder(docUncompressedSize);
    DWORD t0 = GetTickCount();
    size_t nFailed = DecompressRecords();
    DWORD t1 = GetTickCount();
    logf("MobiDoc::LoadForPdbReader: decompressed %zu records (%zu bytes) in %u ms\n", docRecCount, doc->size(),
         t1 - t0);

    // TODO: this is a heuristic for https://github.com/sumatrapdfreader/sumatrapdf/issues/1314
    // It has 29 records that fail to decompress because infinite recursion
    // is detected.
    // Figure out if this is a bug in my decoding.
    if (nFailed > docRecCount / 2) {
        return false;
    }

    // replace unexpected \0 with spaces
    // https://code.google.com/p/sumatrapdf/issues/detail?id=2529
    char* s = doc->Get();
    char* end = s + doc->size();
    while ((s = (char*)memchr(s, '\0', end - s)) != nullptr) {
        *s = ' ';
    }
    if (textEncoding != CP_UTF8) {
        TempStr docUtf8 = strconv::ToMultiByteTemp(doc->Get(), textEncoding, CP_UTF8);
        if (docUtf8) {
            doc->Reset();
            doc->Append(docUtf8);
        }
    }
    BuildKf8FragmentTable();
    return true;
}

static bool ReadIndxStartCount(const u8* data, size_t len, u32* start, u32* count) {
    if (!data || len < 28 || memcmp(data, "INDX", 4) != 0) {
        return false;
    }
    ByteOrderDecoder d(data + 4, 24, ByteOrderDecoder::BigEndian);
    d.UInt32(); // len
    d.UInt32(); // nul1
    d.UInt32(); // type
    d.UInt32(); // gen
    *start = d.UInt32();
    *count = d.UInt32();
    return true;
}

static int ParseKindleBase32(const char* s, size_t len) {
    if (!s || 0 == len) {
        return -1;
    }
    int result = 0;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        int v = -1;
        if (c >= '0' && c <= '9') {
            v = c - '0';
        } else if (c >= 'A' && c <= 'V') {
            v = c - 'A' + 10;
        } else if (c >= 'a' && c <= 'v') {
            v = c - 'a' + 10;
        }
        if (v < 0) {
            return -1;
        }
        result = result * 32 + v;
    }
    return result;
}

static bool AppendKf8FragPos(MobiDoc* doc, i32 pos) {
    size_t newCount = doc->kf8FragInsertPosCount + 1;
    i32* newBuf = (i32*)realloc(doc->kf8FragInsertPos, newCount * sizeof(i32));
    if (!newBuf) {
        return false;
    }
    doc->kf8FragInsertPos = newBuf;
    doc->kf8FragInsertPos[doc->kf8FragInsertPosCount++] = pos;
    return true;
}

static size_t ParseKf8FragmentTableAt(MobiDoc* doc, u32 baseIdx) {
    if (!doc->pdbReader || baseIdx == (u32)-1 || 0 == baseIdx) {
        return 0;
    }
    if (baseIdx >= doc->pdbReader->GetRecordCount()) {
        return 0;
    }

    free(doc->kf8FragInsertPos);
    doc->kf8FragInsertPos = nullptr;
    doc->kf8FragInsertPosCount = 0;

    auto headerRec = doc->pdbReader->GetRecord(baseIdx);
    const u8* headerData = headerRec.data();
    size_t headerLen = headerRec.size();
    u32 idxStart = 0, sectionCount = 0;
    if (!ReadIndxStartCount(headerData, headerLen, &idxStart, &sectionCount)) {
        return 0;
    }

    for (u32 section = 1; section <= sectionCount; section++) {
        size_t recNo = baseIdx + section;
        if (recNo >= doc->pdbReader->GetRecordCount()) {
            break;
        }
        auto dataRec = doc->pdbReader->GetRecord(recNo);
        const u8* recData = dataRec.data();
        size_t recLen = dataRec.size();
        u32 idxt = 0, ecnt = 0;
        if (!ReadIndxStartCount(recData, recLen, &idxt, &ecnt)) {
            continue;
        }
        if (idxt + 4 + ecnt * 2 > recLen) {
            continue;
        }
        for (u32 j = 0; j < ecnt; j++) {
            size_t posOff = idxt + 4 + j * 2;
            u16 sp = ((u16)recData[posOff] << 8) | recData[posOff + 1];
            u16 ep = 0;
            if (j + 1 < ecnt) {
                size_t endOff = idxt + 4 + (j + 1) * 2;
                ep = ((u16)recData[endOff] << 8) | recData[endOff + 1];
            } else {
                ep = (u16)idxt;
            }
            if (sp >= recLen || ep > recLen || ep <= sp) {
                continue;
            }
            u8 tlen = recData[sp];
            if (0 == tlen || sp + 1 + tlen > ep) {
                continue;
            }
            char ident[32]{};
            size_t copyLen = std::min((size_t)tlen, sizeof(ident) - 1);
            memcpy(ident, recData + sp + 1, copyLen);
            if (!AppendKf8FragPos(doc, atoi(ident))) {
                return doc->kf8FragInsertPosCount;
            }
        }
    }

    return doc->kf8FragInsertPosCount;
}

bool MobiDoc::BuildKf8FragmentTable() {
    if (kf8FragInsertPosCount > 0) {
        return true;
    }
    if (mobiFormatVersion < 8 || !pdbReader) {
        return false;
    }

    u32 bestIdx = 0;
    size_t bestCount = 0;
    const u32 candidates[2] = {kf8FragIdx, kf8SkelIdx};
    for (int i = 0; i < 2; i++) {
        u32 idx = candidates[i];
        if (idx == (u32)-1 || 0 == idx) {
            continue;
        }
        size_t n = ParseKf8FragmentTableAt(this, idx);
        if (n > bestCount) {
            bestCount = n;
            bestIdx = idx;
        }
    }
    if (0 == bestIdx) {
        return false;
    }
    kf8FragIdxUsed = bestIdx;
    ParseKf8FragmentTableAt(this, bestIdx);
    return kf8FragInsertPosCount > 0;
}

int MobiDoc::ResolveKindlePos(const char* url) const {
    if (!str::StartsWith(url, "kindle:pos:fid:")) {
        return -1;
    }
    const char* fid = url + 15;
    const char* offMarker = str::Find(fid, ":off:");
    if (!offMarker) {
        return -1;
    }
    size_t fidLen = (size_t)(offMarker - fid);
    const char* off = offMarker + 5;
    size_t offLen = str::Len(off);

    int row = ParseKindleBase32(fid, fidLen);
    int offVal = ParseKindleBase32(off, offLen);
    if (row < 0 || offVal < 0) {
        return -1;
    }
    if ((size_t)row >= kf8FragInsertPosCount) {
        return -1;
    }
    i64 pos = (i64)kf8FragInsertPos[row] + offVal;
    if (pos < 0) {
        return -1;
    }
    return (int)pos;
}

// don't free the result
ByteSlice MobiDoc::GetHtmlData() const {
    if (doc) {
        return doc->AsByteSlice();
    }
    return {};
}

TempStr MobiDoc::GetPropertyTemp(const char* name) {
    char* v = GetPropValueTemp(props, name);
    if (!v) {
        return nullptr;
    }
    return strconv::StrToUtf8Temp(v, textEncoding);
}

static const GumboNode* FindMobiTocReference(const GumboNode* node) {
    if (!node) {
        return nullptr;
    }
    if (node->type == GUMBO_NODE_ELEMENT && GumboTagNameIs(node, "reference")) {
        const GumboAttribute* type = gumbo_get_attribute(&node->v.element.attributes, "type");
        if (type && str::EqI(type->value, "toc")) {
            return node;
        }
    }
    const GumboVector* children = nullptr;
    if (node->type == GUMBO_NODE_ELEMENT) {
        children = &node->v.element.children;
    } else if (node->type == GUMBO_NODE_DOCUMENT) {
        children = &node->v.document.children;
    }
    if (children) {
        for (unsigned int i = 0; i < children->length; i++) {
            const GumboNode* found = FindMobiTocReference((const GumboNode*)children->data[i]);
            if (found) {
                return found;
            }
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// MOBI/KF8 NCX (table of contents) index parsing.
//
// Some AZW3/MOBI books (e.g. 资治通鉴) store their ToC only in the NCX index
// records, with no inline <a> anchors in the HTML. The NCX is a chain of INDX
// records: a header INDX (with a TAGX tag-definition block), N "entry" INDX
// records, then a few CNCX records holding the label strings. Each entry packs
// its tags using control bytes; for the ToC we care about:
//   tag 1  = pos (filepos, MOBI7)         tag 4  = hlvl (depth)
//   tag 3  = noffs (label offset in CNCX) tag 6  = pos_fid (KF8 fid + offset)
//   tag 21 = parent  22 = first child  23 = last child
// Reference: KindleUnpack (mobi_index.py / mobi_ncx.py).
// ---------------------------------------------------------------------------
static u32 NcxBE32(const u8* p) {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
}
static u16 NcxBE16(const u8* p) {
    return (u16)(((u16)p[0] << 8) | (u16)p[1]);
}

// big-endian, 7 bits per byte, high bit marks the final byte
static u32 NcxReadVarWidth(const u8* data, size_t len, size_t& offset) {
    u32 value = 0;
    while (offset < len) {
        u8 b = data[offset++];
        value = (value << 7) | (b & 0x7f);
        if (b & 0x80) {
            break;
        }
    }
    return value;
}

static int NcxCountSetBits(u32 v) {
    int c = 0;
    while (v) {
        c += (int)(v & 1);
        v >>= 1;
    }
    return c;
}

struct NcxTagx {
    u8 tag;
    u8 numValues;
    u8 mask;
    u8 endFlag;
};

struct NcxIndxHeader {
    u32 hdrLen;
    u32 idxt;
    u32 count;
    u32 nctoc;
};

static bool NcxReadIndxHeader(const u8* data, size_t len, NcxIndxHeader* h) {
    if (!data || len < 0x38 || memcmp(data, "INDX", 4) != 0) {
        return false;
    }
    h->hdrLen = NcxBE32(data + 0x04);
    h->idxt = NcxBE32(data + 0x14);
    h->count = NcxBE32(data + 0x18);
    h->nctoc = NcxBE32(data + 0x34);
    return true;
}

// returns controlByteCount, fills tagsOut from the TAGX block at 'start'
static int NcxReadTagx(const u8* data, size_t len, size_t start, Vec<NcxTagx>& tagsOut) {
    if (start + 12 > len || memcmp(data + start, "TAGX", 4) != 0) {
        return 0;
    }
    u32 firstEntryOffset = NcxBE32(data + start + 4);
    u32 controlByteCount = NcxBE32(data + start + 8);
    for (size_t i = 12; i + 4 <= firstEntryOffset && start + i + 4 <= len; i += 4) {
        NcxTagx t;
        t.tag = data[start + i];
        t.numValues = data[start + i + 1];
        t.mask = data[start + i + 2];
        t.endFlag = data[start + i + 3];
        tagsOut.Append(t);
    }
    return (int)controlByteCount;
}

struct NcxEntry {
    int pos = -1;
    int noffs = -1;
    int hlvl = -1;
    u32 posFid = 0;
    u32 posOff = 0;
    bool hasPosFid = false;
    int parent = -1;
    int child1 = -1;
    int childn = -1;
    char* entryName = nullptr;
};

static void NcxStoreTag(NcxEntry& e, u8 tag, const u32* vals, int nv) {
    if (nv < 1) {
        return;
    }
    switch (tag) {
        case 1:
            e.pos = (int)vals[0];
            break;
        case 3:
            e.noffs = (int)vals[0];
            break;
        case 4:
            e.hlvl = (int)vals[0];
            break;
        case 6:
            e.posFid = vals[0];
            if (nv >= 2) {
                e.posOff = vals[1];
            }
            e.hasPosFid = true;
            break;
        case 21:
            e.parent = (int)vals[0];
            break;
        case 22:
            e.child1 = (int)vals[0];
            break;
        case 23:
            e.childn = (int)vals[0];
            break;
    }
}

// decode one index entry's tag values (port of KindleUnpack getTagMap)
static void NcxParseEntryTags(int controlByteCount, const Vec<NcxTagx>& tags, const u8* data, size_t dataLen,
                              size_t startPos, NcxEntry& e) {
    struct Pending {
        u8 tag;
        int valueCount;
        int valueBytes;
        u8 valuesPerEntry;
    };
    Pending pend[48];
    int npend = 0;
    int controlByteIndex = 0;
    size_t dataStart = startPos + (size_t)controlByteCount;
    for (int ti = 0; ti < tags.Size(); ti++) {
        const NcxTagx& t = tags[ti];
        if (t.endFlag == 0x01) {
            controlByteIndex++;
            continue;
        }
        size_t cbPos = startPos + (size_t)controlByteIndex;
        if (cbPos >= dataLen) {
            break;
        }
        u32 value = (u32)data[cbPos] & t.mask;
        if (value == 0) {
            continue;
        }
        if (value == t.mask) {
            if (NcxCountSetBits(t.mask) > 1) {
                u32 vb = NcxReadVarWidth(data, dataLen, dataStart);
                if (npend < 48) {
                    pend[npend++] = {t.tag, -1, (int)vb, t.numValues};
                }
            } else {
                if (npend < 48) {
                    pend[npend++] = {t.tag, 1, -1, t.numValues};
                }
            }
        } else {
            u32 mask = t.mask;
            while ((mask & 1) == 0) {
                mask >>= 1;
                value >>= 1;
            }
            if (npend < 48) {
                pend[npend++] = {t.tag, (int)value, -1, t.numValues};
            }
        }
    }
    for (int pi = 0; pi < npend; pi++) {
        Pending& p = pend[pi];
        u32 vals[8];
        int nv = 0;
        if (p.valueCount != -1) {
            for (int i = 0; i < p.valueCount; i++) {
                for (int k = 0; k < p.valuesPerEntry; k++) {
                    u32 v = NcxReadVarWidth(data, dataLen, dataStart);
                    if (nv < 8) {
                        vals[nv++] = v;
                    }
                }
            }
        } else {
            int total = 0;
            while (total < p.valueBytes) {
                size_t before = dataStart;
                u32 v = NcxReadVarWidth(data, dataLen, dataStart);
                total += (int)(dataStart - before);
                if (nv < 8) {
                    vals[nv++] = v;
                }
                if (dataStart <= before) {
                    break;
                }
            }
        }
        NcxStoreTag(e, p.tag, vals, nv);
    }
}

static void NcxEncodeBase32(u32 value, int width, char* out, size_t outSize) {
    static const char* kDigits = "0123456789ABCDEFGHIJKLMNOPQRSTUV";
    char tmp[16];
    int n = 0;
    if (value == 0) {
        tmp[n++] = '0';
    }
    while (value > 0 && n < 16) {
        tmp[n++] = kDigits[value % 32];
        value /= 32;
    }
    size_t oi = 0;
    for (int i = n; i < width && oi + 1 < outSize; i++) {
        out[oi++] = '0';
    }
    for (int i = n - 1; i >= 0 && oi + 1 < outSize; i--) {
        out[oi++] = tmp[i];
    }
    out[oi] = 0;
}

// returns a heap-allocated UTF-8 label (caller frees), or nullptr
static char* NcxLabelBytesToUtf8(const char* raw, size_t slen, int textEncoding) {
    if (!raw || slen == 0) {
        return nullptr;
    }
    char* tmp = (char*)malloc(slen + 1);
    if (!tmp) {
        return nullptr;
    }
    memcpy(tmp, raw, slen);
    tmp[slen] = 0;
    const u8* utf8Check = (const u8*)tmp;
    if (isLegalUTF8String(&utf8Check, utf8Check + slen)) {
        return tmp;
    }
    if (textEncoding != CP_UTF8) {
        TempStr u = strconv::ToMultiByteTemp(tmp, textEncoding, CP_UTF8);
        if (u) {
            free(tmp);
            return str::Dup(u);
        }
    }
    TempStr u = strconv::UnknownToUtf8Temp(tmp, slen);
    free(tmp);
    return u ? str::Dup(u) : nullptr;
}

struct NcxCtocMap {
    Vec<u32> keys;
    Vec<char*> labels;
};

static void NcxFreeCtocMap(NcxCtocMap& map) {
    for (char* label : map.labels) {
        free(label);
    }
    map.keys.Reset();
    map.labels.Reset();
}

// Build the CNCX string table the same way as KindleUnpack readCTOC(): keys are byte
// offsets into a virtual CNCX blob (each record adds 0x10000 to the key space).
static void NcxBuildCtocMap(const Vec<ByteSlice>& cncx, int textEncoding, NcxCtocMap& out) {
    u32 recOff = 0;
    for (int ri = 0; ri < cncx.Size(); ri++) {
        const u8* d = cncx[(size_t)ri].data();
        size_t len = cncx[(size_t)ri].size();
        if (!d || len == 0) {
            recOff += 0x10000;
            continue;
        }
        size_t offset = 0;
        while (offset < len) {
            if (d[offset] == 0) {
                break;
            }
            u32 key = recOff + (u32)offset;
            size_t p = offset;
            u32 slen = NcxReadVarWidth(d, len, p);
            if (slen == 0 || p + slen > len) {
                break;
            }
            char* label = NcxLabelBytesToUtf8((const char*)(d + p), slen, textEncoding);
            if (label) {
                out.keys.Append(key);
                out.labels.Append(label);
            }
            offset = p + slen;
        }
        recOff += 0x10000;
    }
}

static char* NcxLookupCtocLabel(const NcxCtocMap& map, u32 noffs) {
    for (int i = 0; i < map.keys.Size(); i++) {
        if (map.keys[i] == noffs) {
            return str::Dup(map.labels[i]);
        }
    }
    return nullptr;
}

static bool NcxEntryLabelLooksValid(const char* label) {
    if (!label || !*label) {
        return false;
    }
    int suspicious = 0;
    int len = 0;
    for (const u8* p = (const u8*)label; *p; p++, len++) {
        if (*p == '?') {
            suspicious++;
        }
    }
    if (len == 0) {
        return false;
    }
    if (suspicious == len) {
        return false;
    }
    return suspicious * 3 <= len * 2;
}

static void NcxFreeEntries(Vec<NcxEntry>& entries) {
    for (NcxEntry& e : entries) {
        free(e.entryName);
        e.entryName = nullptr;
    }
    entries.Reset();
}

struct NcxTocItem {
    char* label = nullptr;
    char link[80]{};
    int level = 1;
};

static void NcxFreeTocItems(Vec<NcxTocItem>& items) {
    for (NcxTocItem& item : items) {
        free(item.label);
        item.label = nullptr;
    }
    items.Reset();
}

bool MobiDoc::ParseNcxToc(EbookTocVisitor* visitor) {
    if (!pdbReader || ncxIndexRec == (u32)-1 || ncxIndexRec == 0) {
        return false;
    }
    size_t recCount = pdbReader->GetRecordCount();
    if (ncxIndexRec >= recCount) {
        return false;
    }

    // 1. header INDX record: index count, TAGX (tag definitions), CNCX count
    ByteSlice mainRec = pdbReader->GetRecord(ncxIndexRec);
    const u8* mdata = mainRec.data();
    size_t mlen = mainRec.size();
    NcxIndxHeader mh;
    if (!NcxReadIndxHeader(mdata, mlen, &mh)) {
        return false;
    }
    Vec<NcxTagx> tags;
    int controlByteCount = NcxReadTagx(mdata, mlen, mh.hdrLen, tags);
    if (controlByteCount <= 0 || tags.Size() == 0) {
        return false;
    }

    // 2. CNCX label records follow the entry records
    Vec<ByteSlice> cncx;
    u32 cncxStart = ncxIndexRec + mh.count + 1;
    for (u32 j = 0; j < mh.nctoc; j++) {
        size_t rn = (size_t)cncxStart + j;
        if (rn >= recCount) {
            break;
        }
        cncx.Append(pdbReader->GetRecord(rn));
    }

    NcxCtocMap ctocMap;
    NcxBuildCtocMap(cncx, textEncoding, ctocMap);

    // 3. entry records: ncxIndexRec+1 .. ncxIndexRec+count
    Vec<NcxEntry> entries;
    const u32 kMaxEntries = 50000;
    for (u32 i = 1; i <= mh.count; i++) {
        size_t rn = (size_t)ncxIndexRec + i;
        if (rn >= recCount) {
            break;
        }
        ByteSlice rec = pdbReader->GetRecord(rn);
        const u8* rd = rec.data();
        size_t rlen = rec.size();
        NcxIndxHeader eh;
        if (!NcxReadIndxHeader(rd, rlen, &eh)) {
            continue;
        }
        size_t idxtPos = eh.idxt;
        u32 entryCount = eh.count;
        if (idxtPos + 4 + (size_t)entryCount * 2 > rlen) {
            continue;
        }
        for (u32 j = 0; j < entryCount && entries.Size() < (int)kMaxEntries; j++) {
            u16 startOff = NcxBE16(rd + idxtPos + 4 + (size_t)j * 2);
            u16 endOff;
            if (j + 1 < entryCount) {
                endOff = NcxBE16(rd + idxtPos + 4 + (size_t)(j + 1) * 2);
            } else {
                endOff = (u16)idxtPos;
            }
            if (startOff >= rlen || endOff > rlen || endOff <= startOff) {
                continue;
            }
            u8 textLen = rd[startOff];
            size_t tagStart = (size_t)startOff + 1 + textLen;
            if (tagStart > rlen) {
                continue;
            }
            NcxEntry e;
            if (textLen > 0) {
                // INDX entry names are UTF-8 in practice (KindleUnpack decodes them as utf-8).
                e.entryName = NcxLabelBytesToUtf8((const char*)(rd + startOff + 1), textLen, CP_UTF8);
                if (!e.entryName) {
                    e.entryName = NcxLabelBytesToUtf8((const char*)(rd + startOff + 1), textLen, textEncoding);
                }
            }
            NcxParseEntryTags(controlByteCount, tags, rd, rlen, tagStart, e);
            entries.Append(e);
        }
    }

    if (entries.Size() == 0) {
        NcxFreeCtocMap(ctocMap);
        return false;
    }

    // 4. build ToC items; validate labels before emitting
    Vec<NcxTocItem> items;
    int validLabels = 0;
    for (int i = 0; i < entries.Size(); i++) {
        NcxEntry& e = entries[i];
        char* label = nullptr;
        if (e.noffs >= 0) {
            label = NcxLookupCtocLabel(ctocMap, (u32)e.noffs);
        }
        if (!label && e.entryName) {
            label = str::Dup(e.entryName);
        }
        if (!label) {
            label = str::Dup("");
        }
        if (NcxEntryLabelLooksValid(label)) {
            validLabels++;
        }

        NcxTocItem item;
        item.label = label;
        item.level = (e.hlvl >= 0) ? e.hlvl + 1 : 1;
        item.link[0] = 0;
        if (e.hasPosFid) {
            char fidStr[16], offStr[24];
            NcxEncodeBase32(e.posFid, 4, fidStr, sizeof(fidStr));
            NcxEncodeBase32(e.posOff, 10, offStr, sizeof(offStr));
            snprintf(item.link, sizeof(item.link), "kindle:pos:fid:%s:off:%s", fidStr, offStr);
        } else if (e.pos >= 0) {
            snprintf(item.link, sizeof(item.link), "%d", e.pos);
        }
        items.Append(item);
    }

    NcxFreeEntries(entries);
    NcxFreeCtocMap(ctocMap);

    if (validLabels < items.Size() / 2) {
        NcxFreeTocItems(items);
        return false;
    }

    for (NcxTocItem& item : items) {
        visitor->Visit(item.label, item.link[0] ? item.link : nullptr, item.level);
    }
    NcxFreeTocItems(items);
    return true;
}

bool MobiDoc::HasToc() {
    if (docTocIndex != kInvalidSize) {
        return docTocIndex < doc->size();
    }
    docTocIndex = doc->size(); // no ToC

    // search for <reference type="toc" filepos="N"/>
    // The <guide>/<reference> element lives in the document <head> at the very
    // start, so only parse an early window. Gumbo-parsing the whole (potentially
    // ~18MB) document here used to add ~1s to the load, blocking the first paint
    // (HasToc runs synchronously on the UI thread while rebuilding the menu/toolbar).
    GumboOptions opts = GumboMakeOptions();
    size_t refParseLen = doc->size();
    const size_t kMaxTocRefParseLen = 256 * 1024; // 256 KB
    if (refParseLen > kMaxTocRefParseLen) {
        refParseLen = kMaxTocRefParseLen;
    }
    GumboOutput* output = gumbo_parse_with_options(&opts, doc->Get(), refParseLen);
    if (!output) {
        return false;
    }
    const GumboNode* ref = FindMobiTocReference(output->document);
    if (ref) {
        const GumboAttribute* filepos = gumbo_get_attribute(&ref->v.element.attributes, "filepos");
        if (filepos) {
            unsigned int pos;
            if (str::Parse(filepos->value, "%u%$", &pos)) {
                docTocIndex = pos;
            }
        }
    }
    gumbo_destroy_output(&opts, output);

    // AZW3/MOBI files often lack <reference type="toc">; fall back to the
    // visible "目录" heading (HTML text node), not metadata substrings.
    if (docTocIndex >= doc->size()) {
        const char* html = doc->Get();
        size_t htmlLen = doc->size();
        const char kTocTitleUtf8[] = "\xe7\x9b\xae\xe5\xbd\x95"; // 目录
        size_t needleLen = sizeof(kTocTitleUtf8) - 1;
        size_t searchLimit = htmlLen;
        if (searchLimit > 200000) {
            searchLimit = 200000;
        }
        for (size_t i = 500; i + needleLen <= searchLimit; i++) {
            if (memcmp(html + i, kTocTitleUtf8, needleLen) != 0) {
                continue;
            }
            if (i > 0 && html[i - 1] == '>' && i + needleLen < htmlLen && html[i + needleLen] == '<') {
                docTocIndex = i;
                break;
            }
        }
        if (docTocIndex >= doc->size()) {
            for (size_t i = 500; i + needleLen <= searchLimit; i++) {
                if (memcmp(html + i, kTocTitleUtf8, needleLen) == 0) {
                    docTocIndex = i;
                    break;
                }
            }
        }
    }

    return docTocIndex < doc->size();
}

static void AppendDeepText(const GumboNode* node, StrBuilder& sb) {
    if (!node) {
        return;
    }
    if (node->type == GUMBO_NODE_TEXT || node->type == GUMBO_NODE_CDATA || node->type == GUMBO_NODE_WHITESPACE) {
        sb.Append(node->v.text.text);
        return;
    }
    if (node->type != GUMBO_NODE_ELEMENT) {
        return;
    }
    const GumboVector* children = &node->v.element.children;
    for (unsigned int i = 0; i < children->length; i++) {
        AppendDeepText((const GumboNode*)children->data[i], sb);
    }
}

struct MobiTocWalker {
    EbookTocVisitor* visitor = nullptr;
    int itemLevel = 0;
    int visitedCount = 0; // number of ToC items actually emitted
    bool done = false;      // stop at the first page break after the ToC block

    void Walk(const GumboNode* node);
    void WalkChildren(const GumboVector* children);
};

void MobiTocWalker::WalkChildren(const GumboVector* children) {
    for (unsigned int i = 0; i < children->length && !done; i++) {
        Walk((const GumboNode*)children->data[i]);
    }
}

void MobiTocWalker::Walk(const GumboNode* node) {
    if (!node || done) {
        return;
    }
    if (node->type == GUMBO_NODE_DOCUMENT) {
        WalkChildren(&node->v.document.children);
        return;
    }
    if (node->type != GUMBO_NODE_ELEMENT) {
        return;
    }
    if (GumboTagNameIs(node, "mbp:pagebreak") || GumboTagNameIs(node, "pagebreak")) {
        done = true;
        return;
    }
    if (GumboTagNameIs(node, "a")) {
        const GumboAttribute* attr = gumbo_get_attribute(&node->v.element.attributes, "filepos");
        if (!attr) {
            attr = gumbo_get_attribute(&node->v.element.attributes, "href");
        }
        if (attr) {
            StrBuilder text;
            AppendDeepText(node, text);
            if (!text.IsEmpty()) {
                visitedCount++;
                visitor->Visit(text.LendData(), attr->value, itemLevel);
            }
        }
        return;
    }
    bool isLevel = GumboTagNameIs(node, "blockquote") || GumboTagNameIs(node, "ul") || GumboTagNameIs(node, "ol");
    if (isLevel) {
        itemLevel++;
    }
    WalkChildren(&node->v.element.children);
    if (isLevel) {
        itemLevel--;
    }
}

static bool ParseInlineHtmlToc(MobiDoc* mb, EbookTocVisitor* visitor) {
    if (!mb->HasToc()) {
        return false;
    }

    GumboOptions opts = GumboMakeOptions();
    size_t tocIndex = mb->GetTocFilePos();
    const char* tocStart = mb->doc->Get() + tocIndex;
    size_t tocLen = mb->doc->size() - tocIndex;
    const size_t kMaxTocParseLen = 2 * 1024 * 1024; // 2 MB
    if (tocLen > kMaxTocParseLen) {
        tocLen = kMaxTocParseLen;
    }
    GumboOutput* output = gumbo_parse_with_options(&opts, tocStart, tocLen);
    if (!output) {
        return false;
    }

    MobiTocWalker walker;
    walker.visitor = visitor;
    walker.Walk(output->document);

    gumbo_destroy_output(&opts, output);
    return walker.visitedCount > 0;
}

bool MobiDoc::ParseToc(EbookTocVisitor* visitor) {
    // Prefer NCX when available; fall back to inline HTML when NCX labels look corrupt.
    if (ncxIndexRec != (u32)-1 && ncxIndexRec != 0) {
        if (ParseNcxToc(visitor)) {
            return true;
        }
    }

    if (ParseInlineHtmlToc(this, visitor)) {
        return true;
    }

    return false;
}

bool MobiDoc::IsSupportedFileType(Kind kind) {
    return kind == kindFileMobi;
}

MobiDoc* MobiDoc::CreateFromFile(const char* fileName) {
    MobiDoc* mb = new MobiDoc(fileName);
    PdbReader* pdbReader = PdbReader::CreateFromFile(fileName);
    if (!pdbReader || !mb->LoadForPdbReader(pdbReader)) {
        delete mb;
        return nullptr;
    }
    return mb;
}

MobiDoc* MobiDoc::CreateFromStream(IStream* stream) {
    MobiDoc* mb = new MobiDoc(nullptr);
    PdbReader* pdbReader = PdbReader::CreateFromStream(stream);
    if (!pdbReader || !mb->LoadForPdbReader(pdbReader)) {
        delete mb;
        return nullptr;
    }
    return mb;
}
