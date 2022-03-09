/* Copyright 2021 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#include "utils/BaseUtil.h"

#include "utils/ScopedWin.h"
#include "utils/FileUtil.h"
#include "utils/GuessFileType.h"
#include "utils/ByteReader.h"
#include "utils/TgaReader.h"
#include "utils/WebpReader.h"
#include "utils/WinUtil.h"
#include "utils/GdiPlusUtil.h"

#if COMPILER_MSVC
#pragma warning(disable : 4668)
#endif
#include <wincodec.h>

#include "utils/Log.h"

using Gdiplus::Bitmap;
using Gdiplus::BitmapData;
using Gdiplus::CharacterRange;
using Gdiplus::Font;
using Gdiplus::Graphics;
using Gdiplus::Matrix;
using Gdiplus::MatrixOrderAppend;
using Gdiplus::Ok;
using Gdiplus::Region;
using Gdiplus::Status;
using Gdiplus::StringFormat;
using Gdiplus::StringFormatFlagsMeasureTrailingSpaces;

Gdiplus::RectF RectToRectF(const Gdiplus::Rect r) {
    return Gdiplus::RectF((float)r.X, (float)r.Y, (float)r.Width, (float)r.Height);
}

// Get width of each character and add them up.
// Doesn't seem to be any different than MeasureTextAccurate() i.e. it still
// underreports the width
RectF MeasureTextAccurate2(Graphics* g, Font* f, const WCHAR* s, int len) {
    CrashIf(0 >= len);
    FixedArray<Region, 1024> regionBuf(len);
    Region* r = regionBuf.Get();
    StringFormat sf(StringFormat::GenericTypographic());
    sf.SetFormatFlags(sf.GetFormatFlags() | StringFormatFlagsMeasureTrailingSpaces);
    Gdiplus::RectF layoutRect;
    FixedArray<CharacterRange, 1024> charRangesBuf(len);
    CharacterRange* charRanges = charRangesBuf.Get();
    for (int i = 0; i < len; i++) {
        charRanges[i].First = i;
        charRanges[i].Length = 1;
    }
    sf.SetMeasurableCharacterRanges(len, charRanges);
    Status status = g->MeasureCharacterRanges(s, len, f, layoutRect, &sf, len, r);
    CrashIf(status != Ok);
    Gdiplus::RectF bbox;
    float maxDy = 0;
    float totalDx = 0;
    for (int i = 0; i < len; i++) {
        r[i].GetBounds(&bbox, g);
        if (bbox.Height > maxDy) {
            maxDy = bbox.Height;
        }
        totalDx += bbox.Width;
    }
    bbox.Width = totalDx;
    bbox.Height = maxDy;
    return RectF{bbox};
}

// note: gdi+ seems to under-report the width, the longer the text, the
// bigger the difference. I'm trying to correct for that with those magic values
#define PER_CHAR_DX_ADJUST .2f
#define PER_STR_DX_ADJUST 1.f

// http://www.codeproject.com/KB/GDI-plus/measurestring.aspx
RectF MeasureTextAccurate(Graphics* g, Font* f, const WCHAR* s, int len) {
    if (0 == len) {
        return RectF(0, 0, 0, 0); // TODO: should set height to font's height
    }
    // note: frankly, I don't see a difference between those StringFormat variations
    StringFormat sf(StringFormat::GenericTypographic());
    sf.SetFormatFlags(sf.GetFormatFlags() | StringFormatFlagsMeasureTrailingSpaces);
    // StringFormat sf(StringFormat::GenericDefault());
    // StringFormat sf;
    Gdiplus::RectF layoutRect;
    CharacterRange cr(0, len);
    sf.SetMeasurableCharacterRanges(1, &cr);
    Region r;
    Status status = g->MeasureCharacterRanges(s, len, f, layoutRect, &sf, 1, &r);
    if (status != Ok) {
        // TODO: remove whem we figure out why we crash
        if (!s) {
            s = L"<null>";
        }
        char* s2 = ToUtf8Temp(s, (size_t)len);
        if (len > 256) {
            s2[256] = 0;
        }
        logf("MeasureTextAccurate: status: %d, font: %p, len: %d, s: '%s'\n", (int)status, f, len, s2);
        // CrashIf(status != Ok);
    }
    Gdiplus::RectF bbox;
    r.GetBounds(&bbox, g);
    if (bbox.Width != 0) {
        bbox.Width += PER_STR_DX_ADJUST + (PER_CHAR_DX_ADJUST * (float)len);
    }
    return RectF{bbox};
}

// this usually reports size that is too large
RectF MeasureTextStandard(Graphics* g, Font* f, const WCHAR* s, int len) {
    Gdiplus::RectF bbox;
    Gdiplus::PointF pz(0, 0);
    g->MeasureString(s, len, f, pz, &bbox);
    return RectF{bbox};
}

RectF MeasureTextQuick(Graphics* g, Font* f, const WCHAR* s, int len) {
    CrashIf(0 >= len);

    static Vec<Font*> fontCache;
    static Vec<bool> fixCache;

    Gdiplus::RectF bbox;
    g->MeasureString(s, len, f, Gdiplus::PointF(0, 0), &bbox);
    int idx = fontCache.Find(f);
    if (-1 == idx) {
        LOGFONTW lfw;
        Status ok = f->GetLogFontW(g, &lfw);
        bool isItalicOrMonospace = Ok != ok || lfw.lfItalic || str::Eq(lfw.lfFaceName, L"Courier New") ||
                                   str::Find(lfw.lfFaceName, L"Consol") || str::EndsWith(lfw.lfFaceName, L"Mono") ||
                                   str::EndsWith(lfw.lfFaceName, L"Typewriter");
        fontCache.Append(f);
        fixCache.Append(isItalicOrMonospace);
        idx = (int)fontCache.size() - 1;
    }
    // most documents look good enough with these adjustments
    if (!fixCache.at(idx)) {
        float correct = 0;
        for (int i = 0; i < len; i++) {
            switch (s[i]) {
                case 'i':
                case 'l':
                    correct += 0.2f;
                    break;
                case 't':
                case 'f':
                case 'I':
                    correct += 0.1f;
                    break;
                case '.':
                case ',':
                case '!':
                    correct += 0.1f;
                    break;
            }
        }
        bbox.Width *= (1.0f - correct / len) * 0.99f;
    }
    bbox.Height *= 0.95f;
    return RectF{bbox};
}

RectF MeasureText(Graphics* g, Font* f, const WCHAR* s, size_t len, TextMeasureAlgorithm algo) {
    // TODO: ideally we should not be here with len == 0. This
    // might indicate a problem with fromatter code. See internals-en.epub
    // for a repro
    if (-1 == len || 0 == len) {
        len = str::Len(s);
    }
    CrashIf((len == 0) || (len > INT_MAX));
    if (algo) {
        return algo(g, f, s, (int)len);
    }
    auto bbox = MeasureTextAccurate(g, f, s, static_cast<int>(len));
    return bbox;
}

// returns number of characters of string s that fits in a given width dx
// note: could be speed up a bit because in our use case we already know
// the width of the whole string so we could supply it to the function, but
// this shouldn't happen often, so that's fine. It's also possible that
// a smarter approach is possible, but this usually only does 3 MeasureText
// calls, so it's not that bad
size_t StringLenForWidth(Graphics* g, Font* f, const WCHAR* s, size_t len, float dx, TextMeasureAlgorithm algo) {
    auto r = MeasureText(g, f, s, len, algo);
    if (r.dx <= dx) {
        return len;
    }
    // make the best guess of the length that fits
    size_t n = (size_t)((dx / r.dx) * (float)len);
    CrashIf((0 == n) || (n > len));
    r = MeasureText(g, f, s, n, algo);
    // find the length len of s that fits within dx iff width of len+1 exceeds dx
    int dir = 1; // increasing length
    if (r.dx > dx) {
        dir = -1; // decreasing length
    }
    for (;;) {
        n += dir;
        r = MeasureText(g, f, s, n, algo);
        if (1 == dir) {
            // if advancing length, we know that previous string did fit, so if
            // the new one doesn't fit, the previous length was the right one
            if (r.dx > dx) {
                return n - 1;
            }
        } else {
            // if decreasing length, we know that previous string didn't fit, so if
            // the one one fits, it's of the correct length
            if (r.dx < dx) {
                return n;
            }
        }
    }
}

// TODO: not quite sure why spaceDx1 != spaceDx2, using spaceDx2 because
// is smaller and looks as better spacing to me
float GetSpaceDx(Graphics* g, Font* f, TextMeasureAlgorithm algo) {
    RectF bbox;
#if 0
    bbox = MeasureText(g, f, L" ", 1, algo);
    float spaceDx1 = bbox.dx;
    return spaceDx1;
#else
    // this method seems to return (much) smaller size that measuring
    // the space itself
    bbox = MeasureText(g, f, L"wa", 2, algo);
    float l1 = bbox.dx;
    bbox = MeasureText(g, f, L"w a", 3, algo);
    float l2 = bbox.dx;
    float spaceDx2 = l2 - l1;
    return spaceDx2;
#endif
}

void GetBaseTransform(Matrix& m, Gdiplus::RectF pageRect, float zoom, int rotation) {
    rotation = rotation % 360;
    if (rotation < 0) {
        rotation = rotation + 360;
    }
    if (90 == rotation) {
        m.Translate(0, -pageRect.Height, MatrixOrderAppend);
    } else if (180 == rotation) {
        m.Translate(-pageRect.Width, -pageRect.Height, MatrixOrderAppend);
    } else if (270 == rotation) {
        m.Translate(-pageRect.Width, 0, MatrixOrderAppend);
    } else if (0 == rotation) {
        m.Translate(0, 0, MatrixOrderAppend);
    } else {
        CrashIf(true);
    }

    m.Scale(zoom, zoom, MatrixOrderAppend);
    m.Rotate((float)rotation, MatrixOrderAppend);
}

static Bitmap* WICDecodeImageFromStream(IStream* stream) {
    ScopedCom com;

#define HR(hr)      \
    if (FAILED(hr)) \
        return nullptr;
    ScopedComPtr<IWICImagingFactory> pFactory;
    if (!pFactory.Create(CLSID_WICImagingFactory)) {
        return nullptr;
    }
    ScopedComPtr<IWICBitmapDecoder> pDecoder;
    HR(pFactory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &pDecoder));
    ScopedComPtr<IWICBitmapFrameDecode> srcFrame;
    HR(pDecoder->GetFrame(0, &srcFrame));
    ScopedComPtr<IWICFormatConverter> pConverter;
    HR(pFactory->CreateFormatConverter(&pConverter));
    HR(pConverter->Initialize(srcFrame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.f,
                              WICBitmapPaletteTypeCustom));

    uint w, h;
    HR(pConverter->GetSize(&w, &h));
    double xres, yres;
    HR(pConverter->GetResolution(&xres, &yres));
    Bitmap bmp(w, h, PixelFormat32bppARGB);
    Gdiplus::Rect bmpRect(0, 0, w, h);
    BitmapData bmpData;
    Status ok = bmp.LockBits(&bmpRect, Gdiplus::ImageLockModeWrite, PixelFormat32bppARGB, &bmpData);
    if (ok != Ok) {
        return nullptr;
    }
    HR(pConverter->CopyPixels(nullptr, bmpData.Stride, bmpData.Stride * h, (BYTE*)bmpData.Scan0));
    bmp.UnlockBits(&bmpData);
    bmp.SetResolution((float)xres, (float)yres);
#undef HR

    // hack to avoid the use of ::new (because there won't be a corresponding ::delete)
    return bmp.Clone(0, 0, w, h, PixelFormat32bppARGB);
}

static Bitmap* DecodeWithWIC(ByteSlice bmpData) {
    auto strm = CreateStreamFromData(bmpData);
    ScopedComPtr<IStream> stream(strm);
    if (!stream) {
        return nullptr;
    }
    auto bmp = WICDecodeImageFromStream(stream);
    return bmp;
}

static Bitmap* DecodeWithGdiplus(ByteSlice bmpData) {
    auto strm = CreateStreamFromData(bmpData);
    ScopedComPtr<IStream> stream(strm);
    if (!stream) {
        return nullptr;
    }
    Bitmap* bmp = Gdiplus::Bitmap::FromStream(stream);
    if (!bmp) {
        return nullptr;
    }
    if (bmp->GetLastStatus() != Gdiplus::Ok) {
        delete bmp;
        return nullptr;
    }
    return bmp;
}

Bitmap* BitmapFromDataWin(ByteSlice bmpData) {
    Kind format = GuessFileTypeFromContent(bmpData);
    if (kindFileTga == format) {
        return tga::ImageFromData(bmpData);
    }
    if (kindFileWebp == format) {
        return webp::ImageFromData(bmpData);
    }

    // those are potentially multi-image formats and WICDecodeImageFromStream
    // doesn't support that
    // TODO: more formats? webp?
    bool tryGdiplusFirst = (kindFileTiff == format) || (kindFileGif == format);

    Bitmap* bmp{nullptr};
    if (tryGdiplusFirst) {
        bmp = DecodeWithGdiplus(bmpData);
        ;
    }
    if (!bmp) {
        bmp = DecodeWithWIC(bmpData);
    }
    if (!bmp && !tryGdiplusFirst) {
        bmp = DecodeWithGdiplus(bmpData);
    }
    return bmp;
}

#define JP2_JP2H 0x6a703268 /**< JP2 header box (super-box) */
#define JP2_IHDR 0x69686472 /**< Image header box */

// adapted from http://cpansearch.perl.org/src/RJRAY/Image-Size-3.230/lib/Image/Size.pm
Size BitmapSizeFromData(ByteSlice d) {
    Size result;
    ByteReader r(d);
    size_t len = d.size();
    u8* data = d.data();
    Kind kind = GuessFileTypeFromContent(d);

    if (kind == kindFileBmp) {
        if (len >= sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER)) {
            BITMAPINFOHEADER bmi;
            bool ok = r.UnpackLE(&bmi, sizeof(bmi), "3d2w6d", sizeof(BITMAPFILEHEADER));
            CrashIf(!ok);
            result.dx = bmi.biWidth;
            result.dy = bmi.biHeight;
        }
    } else if (kind == kindFileGif) {
        if (len >= 13) {
            // find the first image's actual size instead of using the
            // "logical screen" size which is sometimes too large
            size_t idx = 13;
            // skip the global color table
            if ((r.Byte(10) & 0x80)) {
                idx += (size_t)3 * (size_t)((size_t)1 << ((r.Byte(10) & 0x07) + 1));
            }
            while (idx + 8 < len) {
                if (r.Byte(idx) == 0x2C) {
                    result.dx = r.WordLE(idx + 5);
                    result.dy = r.WordLE(idx + 7);
                    break;
                } else if (r.Byte(idx) == 0x21 && r.Byte(idx + 1) == 0xF9) {
                    idx += 8;
                } else if (r.Byte(idx) == 0x21 && r.Byte(idx + 1) == 0xFE) {
                    const u8* commentEnd = r.Find(idx + 2, 0x00);
                    idx = commentEnd ? commentEnd - data + 1 : len;
                } else if (r.Byte(idx) == 0x21 && r.Byte(idx + 1) == 0x01 && idx + 15 < len) {
                    const u8* textDataEnd = r.Find(idx + 15, 0x00);
                    idx = textDataEnd ? textDataEnd - data + 1 : len;
                } else if (r.Byte(idx) == 0x21 && r.Byte(idx + 1) == 0xFF && idx + 14 < len) {
                    const u8* applicationDataEnd = r.Find(idx + 14, 0x00);
                    idx = applicationDataEnd ? applicationDataEnd - data + 1 : len;
                } else {
                    break;
                }
            }
        }
    } else if (kind == kindFileJpeg) {
        // find the last start of frame marker for non-differential Huffman/arithmetic coding
        for (size_t idx = 2; idx + 9 < len && r.Byte(idx) == 0xFF;) {
            if (0xC0 <= r.Byte(idx + 1) && r.Byte(idx + 1) <= 0xC3 ||
                0xC9 <= r.Byte(idx + 1) && r.Byte(idx + 1) <= 0xCB) {
                result.dx = r.WordBE(idx + 7);
                result.dy = r.WordBE(idx + 5);
            }
            idx += (size_t)r.WordBE(idx + 2) + 2;
        }
    } else if (kind == kindFileJxr || kind == kindFileTiff) {
        if (len >= 10) {
            bool isBE = r.Byte(0) == 'M', isJXR = r.Byte(2) == 0xBC;
            CrashIf(!isBE && r.Byte(0) != 'I' || isJXR && isBE);
            const WORD WIDTH = isJXR ? 0xBC80 : 0x0100, HEIGHT = isJXR ? 0xBC81 : 0x0101;
            size_t idx = r.DWord(4, isBE);
            WORD count = idx <= len - 2 ? r.Word(idx, isBE) : 0;
            for (idx += 2; count > 0 && idx <= len - 12; count--, idx += 12) {
                WORD tag = r.Word(idx, isBE), type = r.Word(idx + 2, isBE);
                if (r.DWord(idx + 4, isBE) != 1) {
                    continue;
                } else if (WIDTH == tag && 4 == type) {
                    result.dx = r.DWord(idx + 8, isBE);
                } else if (WIDTH == tag && 3 == type) {
                    result.dx = r.Word(idx + 8, isBE);
                } else if (WIDTH == tag && 1 == type) {
                    result.dx = r.Byte(idx + 8);
                } else if (HEIGHT == tag && 4 == type) {
                    result.dy = r.DWord(idx + 8, isBE);
                } else if (HEIGHT == tag && 3 == type) {
                    result.dy = r.Word(idx + 8, isBE);
                } else if (HEIGHT == tag && 1 == type) {
                    result.dy = r.Byte(idx + 8);
                }
            }
        }
    } else if (kind == kindFilePng) {
        if (len >= 24 && str::StartsWith(data + 12, "IHDR")) {
            result.dx = r.DWordBE(16);
            result.dy = r.DWordBE(20);
        }
    } else if (kind == kindFileTga) {
        if (len >= 16) {
            result.dx = r.WordLE(12);
            result.dy = r.WordLE(14);
        }
    } else if (kind == kindFileWebp) {
        if (len >= 30 && str::StartsWith(data + 12, "VP8 ")) {
            result.dx = r.WordLE(26) & 0x3fff;
            result.dy = r.WordLE(28) & 0x3fff;
        } else {
            result = webp::SizeFromData(d);
        }
    } else if (kind == kindFileJp2) {
        if (len >= 32) {
            size_t idx = 0;
            while (idx < len - 32) {
                u32 boxLen = r.DWordBE(idx);
                u32 boxType = r.DWordBE(idx + 4);
                if (JP2_JP2H == boxType) {
                    idx += 8;
                    u32 boxLen2 = r.DWordBE(idx);
                    u32 boxType2 = r.DWordBE(idx + 4);
                    bool isIhdr = boxType2 == JP2_IHDR;
                    idx += 8;
                    if (isIhdr && boxLen2 <= (boxLen - 8)) {
                        result.dx = r.DWordBE(idx);
                        result.dy = r.DWordBE(idx + 4);
                        if (result.dx > 64 * 1024 || result.dy > 64 * 1024) {
                            // sanity check, assuming that images that big can't
                            // possibly be valid
                            result.dx = 0;
                            result.dy = 0;
                        }
                    }
                    break;
                } else if (boxLen != 0 && idx < UINT32_MAX - boxLen) {
                    idx += boxLen;
                } else {
                    break;
                }
            }
        }
    }

    if (result.IsEmpty()) {
        // let GDI+ extract the image size if we've failed
        // (currently happens for animated GIF)
        Bitmap* bmp = BitmapFromDataWin(d);
        if (bmp) {
            result = Size(bmp->GetWidth(), bmp->GetHeight());
        }
        delete bmp;
    }
    return result;
}

CLSID GetEncoderClsid(const WCHAR* format) {
    CLSID null = {0};
    uint numEncoders, size;
    Status ok = Gdiplus::GetImageEncodersSize(&numEncoders, &size);
    if (ok != Ok || 0 == size) {
        return null;
    }
    ScopedMem<Gdiplus::ImageCodecInfo> codecInfo((Gdiplus::ImageCodecInfo*)malloc(size));
    if (!codecInfo) {
        return null;
    }
    GetImageEncoders(numEncoders, size, codecInfo);
    for (uint j = 0; j < numEncoders; j++) {
        if (str::Eq(codecInfo[j].MimeType, format)) {
            return codecInfo[j].Clsid;
        }
    }
    return null;
}

RenderedBitmap* LoadRenderedBitmapWin(const char* path) {
    if (!path) {
        return nullptr;
    }
    AutoFree data(file::ReadFile(path));
    if (!data.data) {
        return nullptr;
    }
    Gdiplus::Bitmap* bmp = BitmapFromDataWin(data.AsSpan());
    if (!bmp) {
        return nullptr;
    }

    HBITMAP hbmp;
    RenderedBitmap* rendered = nullptr;
    if (bmp->GetHBITMAP((Gdiplus::ARGB)Gdiplus::Color::White, &hbmp) == Gdiplus::Ok) {
        rendered = new RenderedBitmap(hbmp, Size(bmp->GetWidth(), bmp->GetHeight()));
    }
    delete bmp;

    return rendered;
}
