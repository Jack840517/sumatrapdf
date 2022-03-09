/* Copyright 2021 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#pragma warning(disable : 4611) // interaction between '_setjmp' and C++ object destruction is non-portable

extern "C" {
#include <mupdf/fitz.h>
#include "../mupdf/source/fitz/color-imp.h"
#include "../mupdf/source/fitz/image-imp.h"
}

#include "utils/BaseUtil.h"
#include "utils/WinUtil.h"
#include "utils/GdiPlusUtil.h"
#include "utils/FileUtil.h"

#include "FzImgReader.h"

static Gdiplus::Bitmap* ImageFromJpegData(fz_context* ctx, const u8* data, int len) {
    int w = 0, h = 0, xres = 0, yres = 0;
    fz_colorspace* cs = nullptr;
    fz_stream* stm = nullptr;
    uint8_t orient = 0;

    fz_var(cs);
    fz_var(stm);
    fz_var(orient);

    fz_try(ctx) {
        fz_load_jpeg_info(ctx, data, len, &w, &h, &xres, &yres, &cs, &orient);
        stm = fz_open_memory(ctx, data, len);
        stm = fz_open_dctd(ctx, stm, -1, 0, nullptr);
    }
    fz_catch(ctx) {
        fz_drop_colorspace(ctx, cs);
        cs = nullptr;
    }

    Gdiplus::PixelFormat fmt = fz_device_rgb(ctx) == cs    ? PixelFormat24bppRGB
                               : fz_device_gray(ctx) == cs ? PixelFormat24bppRGB
                               : fz_device_cmyk(ctx) == cs ? PixelFormat32bppCMYK
                                                           : PixelFormatUndefined;
    if (PixelFormatUndefined == fmt || w <= 0 || h <= 0 || !cs) {
        fz_drop_stream(ctx, stm);
        fz_drop_colorspace(ctx, cs);
        return nullptr;
    }

    Gdiplus::Bitmap bmp(w, h, fmt);
    bmp.SetResolution(xres, yres);

    Gdiplus::Rect bmpRect(0, 0, w, h);
    Gdiplus::BitmapData bmpData;
    Gdiplus::Status ok = bmp.LockBits(&bmpRect, Gdiplus::ImageLockModeWrite, fmt, &bmpData);
    if (ok != Gdiplus::Ok) {
        fz_drop_stream(ctx, stm);
        fz_drop_colorspace(ctx, cs);
        return nullptr;
    }

    fz_var(bmp);
    fz_var(bmpRect);

    fz_try(ctx) {
        for (int y = 0; y < h; y++) {
            u8* line = (u8*)bmpData.Scan0 + y * bmpData.Stride;
            for (int x = 0; x < w; x++) {
                int read = (int)fz_read(ctx, stm, line, cs->n);
                if (read != cs->n) {
                    fz_throw(ctx, FZ_ERROR_GENERIC, "insufficient data for image");
                }
                if (3 == cs->n) { // RGB -> BGR
                    std::swap(line[0], line[2]);
                    line += 3;
                } else if (1 == cs->n) { // gray -> BGR
                    line[1] = line[2] = line[0];
                    line += 3;
                } else if (4 == cs->n) { // CMYK color inversion
                    for (int k = 0; k < 4; k++) {
                        line[k] = 255 - line[k];
                    }
                    line += 4;
                }
            }
        }
    }
    fz_always(ctx) {
        bmp.UnlockBits(&bmpData);
        fz_drop_stream(ctx, stm);
        fz_drop_colorspace(ctx, cs);
    }
    fz_catch(ctx) {
        return nullptr;
    }

    // hack to avoid the use of ::new (because there won't be a corresponding ::delete)
    return bmp.Clone(0, 0, w, h, fmt);
}

// had to create a copy of fz_convert_pixmap to ensure we always get the alpha
static fz_pixmap* fz_convert_pixmap2(fz_context* ctx, fz_pixmap* pix, fz_colorspace* ds, fz_colorspace* prf,
                                     fz_default_colorspaces* default_cs, fz_color_params color_params, int keep_alpha) {
    fz_pixmap* cvt;

    if (!ds && !keep_alpha) {
        fz_throw(ctx, FZ_ERROR_GENERIC, "cannot both throw away and keep alpha");
    }

    cvt = fz_new_pixmap(ctx, ds, pix->w, pix->h, pix->seps, keep_alpha);

    cvt->xres = pix->xres;
    cvt->yres = pix->yres;
    cvt->x = pix->x;
    cvt->y = pix->y;
    if (pix->flags & FZ_PIXMAP_FLAG_INTERPOLATE) {
        cvt->flags |= FZ_PIXMAP_FLAG_INTERPOLATE;
    } else {
        cvt->flags &= ~FZ_PIXMAP_FLAG_INTERPOLATE;
    }

    fz_try(ctx) {
        fz_convert_pixmap_samples(ctx, pix, cvt, prf, default_cs, color_params, 1);
    }
    fz_catch(ctx) {
        fz_drop_pixmap(ctx, cvt);
        fz_rethrow(ctx);
    }

    return cvt;
}

static Gdiplus::Bitmap* ImageFromJp2Data(fz_context* ctx, const u8* data, int len) {
    fz_pixmap* pix = nullptr;
    fz_pixmap* pix_argb = nullptr;

    fz_var(pix);
    fz_var(pix_argb);

    fz_try(ctx) {
        pix = fz_load_jpx(ctx, data, len, nullptr);
    }
    fz_catch(ctx) {
        pix = nullptr;
    }
    if (!pix) {
        return nullptr;
    }

    int w = pix->w, h = pix->h;
    Gdiplus::PixelFormat pixelFormat = PixelFormat32bppARGB;
    Gdiplus::Bitmap bmp(w, h, pixelFormat);
    bmp.SetResolution(pix->xres, pix->yres);

    Gdiplus::Rect bmpRect(0, 0, w, h);
    Gdiplus::BitmapData bmpData;
    Gdiplus::Status ok = bmp.LockBits(&bmpRect, Gdiplus::ImageLockModeWrite, pixelFormat, &bmpData);
    if (ok != Gdiplus::Ok) {
        fz_drop_pixmap(ctx, pix);
        return nullptr;
    }

    fz_var(bmp);
    fz_var(bmpRect);

    fz_try(ctx) {
        fz_colorspace* csdest = fz_device_bgr(ctx);
        fz_color_params colparms = fz_default_color_params;
        fz_colorspace* prf = nullptr;
        int alpha = 1;
        // TODO: could be optimized by creating a bitmap with bmpData.Scan0 as data
        // Or creating Bitmap after a fact with pix_argb->samples
        pix_argb = fz_convert_pixmap2(ctx, pix, csdest, prf, nullptr, colparms, alpha);
        u8* bmpPixels = (u8*)bmpData.Scan0;
        size_t dataSize = pix_argb->stride * h;
        memcpy(bmpPixels, pix_argb->samples, dataSize);
    }
    fz_always(ctx) {
        bmp.UnlockBits(&bmpData);
        fz_drop_pixmap(ctx, pix);
        fz_drop_pixmap(ctx, pix_argb);
    }
    fz_catch(ctx) {
        return nullptr;
    }

    // hack to avoid the use of ::new (because there won't be a corresponding ::delete)
    return bmp.Clone(0, 0, w, h, pixelFormat);
}

Gdiplus::Bitmap* FzImageFromData(ByteSlice d) {
    const u8* data = (const u8*)d.data();
    size_t len = d.size();
    if (len > INT_MAX || len < 12) {
        return nullptr;
    }

    fz_context* ctx = fz_new_context(nullptr, nullptr, 0);
    if (!ctx) {
        return nullptr;
    }

    Gdiplus::Bitmap* result = nullptr;
    if (str::StartsWith(data, "\xFF\xD8")) {
        result = ImageFromJpegData(ctx, data, (int)len);
    } else if (memeq(data, "\0\0\0\x0CjP  \x0D\x0A\x87\x0A", 12)) {
        result = ImageFromJp2Data(ctx, data, (int)len);
    }

    fz_drop_context(ctx);

    return result;
}

Gdiplus::Bitmap* BitmapFromData(ByteSlice bmpData) {
    auto res = BitmapFromDataWin(bmpData);
    if (res) {
        return res;
    }
    return FzImageFromData(bmpData);
}

RenderedBitmap* LoadRenderedBitmap(const char* path) {
    if (!path) {
        return nullptr;
    }
    AutoFree data(file::ReadFile(path));
    if (!data.data) {
        return nullptr;
    }
    Gdiplus::Bitmap* bmp = BitmapFromData(data.AsSpan());
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
