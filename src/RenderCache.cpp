/* Copyright 2021 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/ScopedWin.h"
#include "utils/WinUtil.h"
#include "utils/Timer.h"

#include "wingui/TreeModel.h"
#include "DisplayMode.h"
#include "Controller.h"
#include "EngineBase.h"
#include "EngineAll.h"
#include "SettingsStructs.h"
#include "DisplayModel.h"
#include "GlobalPrefs.h"
#include "RenderCache.h"
#include "TextSelection.h"

#define NO_LOG
#include "utils/Log.h"

#pragma warning(disable : 28159) // silence /analyze: Consider using 'GetTickCount64' instead of 'GetTickCount'

// TODO: remove this and always conserve memory?
/* Define if you want to conserve memory by always freeing cached bitmaps
   for pages not visible. Disabling this might lead to pages not rendering
   due to insufficient (GDI) memory. */
#define CONSERVE_MEMORY

bool gShowTileLayout = false;

RenderCache::RenderCache() : maxTileSize({GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)}) {
    // enable when debugging RenderCache logic
    // gEnableDbgLog = true;

    isRemoteSession = GetSystemMetrics(SM_REMOTESESSION);
    textColor = WIN_COL_BLACK;
    backgroundColor = WIN_COL_WHITE;

    InitializeCriticalSection(&cacheAccess);
    InitializeCriticalSection(&requestAccess);

    startRendering = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    renderThread = CreateThread(nullptr, 0, RenderCacheThread, this, 0, nullptr);
    CrashIf(nullptr == renderThread);
}

RenderCache::~RenderCache() {
    EnterCriticalSection(&requestAccess);
    EnterCriticalSection(&cacheAccess);

    CloseHandle(renderThread);
    CloseHandle(startRendering);
    CrashIf(curReq || 0 != requestCount || 0 != cacheCount);

    LeaveCriticalSection(&cacheAccess);
    DeleteCriticalSection(&cacheAccess);
    LeaveCriticalSection(&requestAccess);
    DeleteCriticalSection(&requestAccess);
}

/* Find a bitmap for a page defined by <dm> and <pageNo> and optionally also
   <rotation> and <zoom> in the cache - call DropCacheEntry when you
   no longer need a found entry. */
BitmapCacheEntry* RenderCache::Find(DisplayModel* dm, int pageNo, int rotation, float zoom, TilePosition* tile) {
    ScopedCritSec scope(&cacheAccess);
    rotation = NormalizeRotation(rotation);
    for (int i = 0; i < cacheCount; i++) {
        BitmapCacheEntry* e = cache[i];
        if ((dm == e->dm) && (pageNo == e->pageNo) && (rotation == e->rotation) &&
            (INVALID_ZOOM == zoom || zoom == e->zoom) && (!tile || e->tile == *tile)) {
            e->refs++;
            CrashIf(i != e->cacheIdx);
            return e;
        }
    }
    return nullptr;
}

bool RenderCache::Exists(DisplayModel* dm, int pageNo, int rotation, float zoom, TilePosition* tile) {
    BitmapCacheEntry* entry = Find(dm, pageNo, rotation, zoom, tile);
    if (entry) {
        DropCacheEntry(entry);
    }
    return entry != nullptr;
}

bool RenderCache::DropCacheEntry(BitmapCacheEntry* entry) {
    ScopedCritSec scope(&cacheAccess);
    CrashIf(!entry);
    if (!entry) {
        return false;
    }
    int idx = entry->cacheIdx;
    CrashIf(idx < 0);
    CrashIf(idx >= cacheCount);
    if ((idx < 0) || (idx >= cacheCount)) {
        return false;
    }
    CrashIf(entry->refs <= 0);
    --entry->refs;
    if (entry->refs > 0) {
        return false;
    }
    CrashIf(entry->refs != 0);
    CrashIf(cache[idx] != entry);
    logf("RenderCache::DropCacheEntry: pageNo: %d, rotation: %d, zoom: %.2f\n", entry->pageNo, entry->rotation,
         entry->zoom);

    delete entry;

    // fast removal by replacing freed item with the item at the end
    cache[idx] = nullptr;
    int lastIdx = cacheCount - 1;
    if ((lastIdx >= 0) && (idx != lastIdx)) {
        cache[idx] = cache[lastIdx];
        cache[idx]->cacheIdx = idx;
        cache[lastIdx] = nullptr;
    }
    cacheCount--;
    CrashIf(cacheCount < 0);
    return true;
}

static bool FreeIfFull(RenderCache* rc, const PageRenderRequest& req) {
    int n = rc->cacheCount;
    if (n < MAX_BITMAPS_CACHED) {
        return true;
    }

    DisplayModel* dm = req.dm;
    // free an invisible page of the same DisplayModel ...
    for (int i = 0; i < n; i++) {
        auto entry = rc->cache[i];
        if (entry->dm == dm && !dm->PageVisibleNearby(entry->pageNo)) {
            bool didDrop = rc->DropCacheEntry(entry);
            if (didDrop) {
                return true;
            }
        }
    }

    // ... or just the oldest cached page
    for (int i = 0; i < n; i++) {
        auto entry = rc->cache[i];
        if (entry->dm == dm) {
            // don't free pages from the document we're currently displaying
            // as it leads to flicker
            // TODO: it can still flicker if the dm is from a visible tab
            // in a different window, but it's harder to detect
            continue;
        }
        bool didDrop = rc->DropCacheEntry(entry);
        if (didDrop) {
            return true;
        }
    }
    return false;
}

void RenderCache::Add(PageRenderRequest& req, RenderedBitmap* bmp) {
    ScopedCritSec scope(&cacheAccess);
    CrashIf(!req.dm);

    req.rotation = NormalizeRotation(req.rotation);
    CrashIf(cacheCount > MAX_BITMAPS_CACHED);

    /* It's possible there still is a cached bitmap with different zoom/rotation */
    FreePage(req.dm, req.pageNo, &req.tile);

    bool hasSpace = FreeIfFull(this, req);
    CrashIf(!hasSpace); // TODO: FreeIfFull() might actually fail to free
    CrashIf(cacheCount > MAX_BITMAPS_CACHED);

    // Copy the PageRenderRequest as it will be reused
    auto entry = new BitmapCacheEntry(req.dm, req.pageNo, req.rotation, req.zoom, req.tile, bmp);
    entry->cacheIdx = cacheCount;
    cache[cacheCount] = entry;
    cacheCount++;
}

static RectF GetTileRect(RectF pagerect, TilePosition tile) {
    CrashIf(tile.res > 30);
    RectF rect;
    rect.dx = pagerect.dx / (1ULL << tile.res);
    rect.dy = pagerect.dy / (1ULL << tile.res);
    rect.x = pagerect.x + tile.col * rect.dx;
    rect.y = pagerect.y + ((1ULL << tile.res) - tile.row - 1) * rect.dy;
    return rect;
}

// get the coordinates of a specific tile
static Rect GetTileRectDevice(EngineBase* engine, int pageNo, int rotation, float zoom, TilePosition tile) {
    RectF mediabox = engine->PageMediabox(pageNo);
    if (tile.res > 0 && tile.res != INVALID_TILE_RES) {
        mediabox = GetTileRect(mediabox, tile);
    }
    RectF pixelbox = engine->Transform(mediabox, pageNo, zoom, rotation);
    return pixelbox.Round();
}

static RectF GetTileRectUser(EngineBase* engine, int pageNo, int rotation, float zoom, TilePosition tile) {
    Rect pixelbox = GetTileRectDevice(engine, pageNo, rotation, zoom, tile);
    return engine->Transform(ToRectF(pixelbox), pageNo, zoom, rotation, true);
}

static Rect GetTileOnScreen(EngineBase* engine, int pageNo, int rotation, float zoom, TilePosition tile,
                            Rect pageOnScreen) {
    Rect bbox = GetTileRectDevice(engine, pageNo, rotation, zoom, tile);
    bbox.Offset(pageOnScreen.x, pageOnScreen.y);
    return bbox;
}

static bool IsTileVisible(DisplayModel* dm, int pageNo, TilePosition tile, float fuzz = 0) {
    if (!dm) {
        return false;
    }
    PageInfo* pageInfo = dm->GetPageInfo(pageNo);
    EngineBase* engine = dm->GetEngine();
    if (!engine || !pageInfo) {
        return false;
    }
    int rotation = dm->GetRotation();
    float zoom = dm->GetZoomReal(pageNo);
    Rect r = pageInfo->pageOnScreen;
    Rect tileOnScreen = GetTileOnScreen(engine, pageNo, rotation, zoom, tile, r);
    // consider nearby tiles visible depending on the fuzz factor
    tileOnScreen.x -= (int)(tileOnScreen.dx * fuzz * 0.5);
    tileOnScreen.dx = (int)(tileOnScreen.dx * (fuzz + 1));
    tileOnScreen.y -= (int)(tileOnScreen.dy * fuzz * 0.5);
    tileOnScreen.dy = (int)(tileOnScreen.dy * (fuzz + 1));
    Rect screen(Point(), dm->GetViewPort().Size());
    return !tileOnScreen.Intersect(screen).IsEmpty();
}

/* Free all bitmaps in the cache that are of a specific page (or all pages
   of the given DisplayModel, or even all invisible pages). */
void RenderCache::FreePage(DisplayModel* dm, int pageNo, TilePosition* tile) {
    logf("RenderCache::FreePage: dm: 0x%p, pageNo: %d\n", dm, pageNo);
    ScopedCritSec scope(&cacheAccess);

    // must go from end becaues freeing changes the cache
    for (int i = cacheCount - 1; i >= 0; i--) {
        BitmapCacheEntry* entry = cache[i];
        bool shouldFree;
        if (dm && pageNo != INVALID_PAGE_NO) {
            // a specific page
            shouldFree = (entry->dm == dm) && (entry->pageNo == pageNo);
            if (tile) {
                // a given tile of the page or all tiles not rendered at a given resolution
                // (and at resolution 0 for quick zoom previews)
                shouldFree =
                    shouldFree && (entry->tile == *tile ||
                                   tile->row == (USHORT)-1 && entry->tile.res > 0 && entry->tile.res != tile->res ||
                                   tile->row == (USHORT)-1 && entry->tile.res == 0 && entry->outOfDate);
            }
        } else if (dm) {
            // all pages of this DisplayModel
            shouldFree = (entry->dm == dm);
        } else {
            // all invisible pages resp. page tiles
            shouldFree = !entry->dm->PageVisibleNearby(entry->pageNo);
            if (!shouldFree && entry->tile.res > 1) {
                shouldFree = !IsTileVisible(entry->dm, entry->pageNo, entry->tile, 2.0);
            }
        }
        if (shouldFree) {
            DropCacheEntry(entry);
        }
    }
}

void RenderCache::FreeForDisplayModel(DisplayModel* dm) {
    FreePage(dm);
}

void RenderCache::FreeNotVisible() {
    FreePage();
}

// keep the cached bitmaps for visible pages to avoid flickering during a reload.
// mark invisible pages as out-of-date to prevent inconsistencies
void RenderCache::KeepForDisplayModel(DisplayModel* oldDm, DisplayModel* newDm) {
    ScopedCritSec scope(&cacheAccess);
    for (int i = 0; i < cacheCount; i++) {
        BitmapCacheEntry* entry = cache[i];
        if (entry->dm != oldDm) {
            continue;
        }
        if (oldDm->PageVisible(entry->pageNo)) {
            entry->dm = newDm;
        }
        // make sure that the page is rerendered eventually
        entry->zoom = INVALID_ZOOM;
        entry->outOfDate = true;
    }
}

// marks all tiles containing rect of pageNo as out of date
void RenderCache::Invalidate(DisplayModel* dm, int pageNo, RectF rect) {
    ScopedCritSec scopeReq(&requestAccess);

    ClearQueueForDisplayModel(dm, pageNo);
    if (curReq && curReq->dm == dm && curReq->pageNo == pageNo) {
        AbortCurrentRequest();
    }

    ScopedCritSec scopeCache(&cacheAccess);

    RectF mediabox = dm->GetEngine()->PageMediabox(pageNo);
    for (int i = 0; i < cacheCount; i++) {
        auto e = cache[i];
        if (e->dm == dm && e->pageNo == pageNo && !GetTileRect(mediabox, e->tile).Intersect(rect).IsEmpty()) {
            e->zoom = INVALID_ZOOM;
            e->outOfDate = true;
        }
    }
}

// determine the count of tiles required for a page at a given zoom level
USHORT RenderCache::GetTileRes(DisplayModel* dm, int pageNo) const {
    auto engine = dm->GetEngine();
    RectF mediabox = engine->PageMediabox(pageNo);
    float zoom = dm->GetZoomReal(pageNo);
    float zoomVirt = dm->GetZoomVirtual();
    Rect viewPort = dm->GetViewPort();
    int rotation = dm->GetRotation();
    RectF pixelbox = engine->Transform(mediabox, pageNo, zoom, rotation);

    float factorW = (float)pixelbox.dx / (maxTileSize.dx + 1);
    float factorH = (float)pixelbox.dy / (maxTileSize.dy + 1);
    // using the geometric mean instead of the maximum factor
    // so that the tile area doesn't get too small in comparison
    // to maxTileSize (but remains smaller)
    float factorAvg = sqrtf(factorW * factorH);

    // use larger tiles when fitting page or width or when a page is smaller
    // than the visible canvas width/height or when rendering pages
    // without clipping optimizations
    if (zoomVirt == ZOOM_FIT_PAGE || zoomVirt == ZOOM_FIT_WIDTH || pixelbox.dx <= viewPort.dx ||
        pixelbox.dy < viewPort.dy || !engine->HasClipOptimizations(pageNo)) {
        factorAvg /= 2.0;
    }

    USHORT res = 0;
    if (factorAvg > 1.5) {
        res = (USHORT)ceilf(log(factorAvg) / log(2.0f));
    }
    // limit res to 30, so that (1 << res) doesn't overflow for 32-bit signed int
    return std::min(res, (USHORT)30);
}

// get the maximum resolution available for the given page
USHORT RenderCache::GetMaxTileRes(DisplayModel* dm, int pageNo, int rotation) {
    ScopedCritSec scope(&cacheAccess);
    USHORT maxRes = 0;
    for (int i = 0; i < cacheCount; i++) {
        auto e = cache[i];
        if (e->dm == dm && e->pageNo == pageNo && e->rotation == rotation) {
            maxRes = std::max(e->tile.res, maxRes);
        }
    }
    return maxRes;
}

// reduce the size of tiles in order to hopefully use less memory overall
bool RenderCache::ReduceTileSize() {
    logf("RenderCache::ReduceTileSize(): reducing tile size (current: %d x %d)\n", maxTileSize.dx, maxTileSize.dy);
    if (maxTileSize.dx < 200 || maxTileSize.dy < 200) {
        return false;
    }

    ScopedCritSec scope1(&requestAccess);
    ScopedCritSec scope2(&cacheAccess);

    if (maxTileSize.dx > maxTileSize.dy) {
        maxTileSize.dx /= 2;
    } else {
        maxTileSize.dy /= 2;
    }

    // invalidate all rendered bitmaps and all requests
    while (cacheCount > 0) {
        FreeForDisplayModel(cache[0]->dm);
    }
    while (requestCount > 0) {
        ClearQueueForDisplayModel(requests[0].dm);
    }
    AbortCurrentRequest();

    return true;
}

void RenderCache::RequestRendering(DisplayModel* dm, int pageNo) {
    TilePosition tile(GetTileRes(dm, pageNo), 0, 0);
    // only honor the request if there's a good chance that the
    // rendered tile will actually be used
    if (tile.res > 1) {
        return;
    }

    RequestRendering(dm, pageNo, tile);
    // render both tiles of the first row when splitting a page in four
    // (which always happens on larger displays for Fit Width)
    if (tile.res == 1 && !IsRenderQueueFull()) {
        tile.col = 1;
        RequestRendering(dm, pageNo, tile, false);
    }
}

/* Render a bitmap for page <pageNo> in <dm>. */
void RenderCache::RequestRendering(DisplayModel* dm, int pageNo, TilePosition tile, bool clearQueueForPage) {
    logf("RenderCache::RequestRendering(): pageNo %d\n", pageNo);
    ScopedCritSec scope(&requestAccess);
    CrashIf(!dm);
    if (!dm || dm->dontRenderFlag) {
        return;
    }

    int rotation = NormalizeRotation(dm->GetRotation());
    float zoom = dm->GetZoomReal(pageNo);

    if (curReq && (curReq->pageNo == pageNo) && (curReq->dm == dm) && (curReq->tile == tile)) {
        if ((curReq->zoom == zoom) && (curReq->rotation == rotation)) {
            /* we're already rendering exactly the same page */
            return;
        }
        /* Currently rendered page is for the same page but with different zoom
        or rotation, so abort it */
        AbortCurrentRequest();
    }

    // clear requests for tiles of different resolution and invisible tiles
    if (clearQueueForPage) {
        ClearQueueForDisplayModel(dm, pageNo, &tile);
    }

    for (int i = 0; i < requestCount; i++) {
        PageRenderRequest* req = &(requests[i]);
        if ((req->pageNo == pageNo) && (req->dm == dm) && (req->tile == tile)) {
            if ((req->zoom == zoom) && (req->rotation == rotation)) {
                /* Request with exactly the same parameters already queued for
                   rendering. Move it to the top of the queue so that it'll
                   be rendered faster. */
                PageRenderRequest tmp;
                tmp = requests[requestCount - 1];
                requests[requestCount - 1] = *req;
                *req = tmp;
            } else {
                /* There was a request queued for the same page but with different
                   zoom or rotation, so only replace this request */
                req->zoom = zoom;
                req->rotation = rotation;
            }
            return;
        }
    }

    if (Exists(dm, pageNo, rotation, zoom, &tile)) {
        /* This page has already been rendered in the correct dimensions
           and isn't about to be rerendered in different dimensions */
        return;
    }

    Render(dm, pageNo, rotation, zoom, &tile);
}

void RenderCache::Render(DisplayModel* dm, int pageNo, int rotation, float zoom, RectF pageRect,
                         RenderingCallback& callback) {
    bool ok = Render(dm, pageNo, rotation, zoom, nullptr, &pageRect, &callback);
    if (!ok) {
        callback.Callback();
    }
}

bool RenderCache::Render(DisplayModel* dm, int pageNo, int rotation, float zoom, TilePosition* tile, RectF* pageRect,
                         RenderingCallback* renderCb) {
    logf("RenderCache::Render(): pageNo %d\n", pageNo);
    CrashIf(!dm);
    if (!dm || dm->dontRenderFlag) {
        return false;
    }

    CrashIf(!(tile || pageRect && renderCb));
    if (!tile && !(pageRect && renderCb)) {
        return false;
    }

    ScopedCritSec scope(&requestAccess);
    PageRenderRequest* newRequest;

    /* add request to the queue */
    if (requestCount == MAX_PAGE_REQUESTS) {
        /* queue is full -> remove the oldest items on the queue */
        if (requests[0].renderCb) {
            requests[0].renderCb->Callback();
        }
        memmove(&(requests[0]), &(requests[1]), sizeof(PageRenderRequest) * (MAX_PAGE_REQUESTS - 1));
        newRequest = &(requests[MAX_PAGE_REQUESTS - 1]);
    } else {
        newRequest = &(requests[requestCount]);
        requestCount++;
    }
    CrashIf(requestCount > MAX_PAGE_REQUESTS);

    newRequest->dm = dm;
    newRequest->pageNo = pageNo;
    newRequest->rotation = rotation;
    newRequest->zoom = zoom;
    if (tile) {
        newRequest->pageRect = GetTileRectUser(dm->GetEngine(), pageNo, rotation, zoom, *tile);
        newRequest->tile = *tile;
    } else if (pageRect) {
        newRequest->pageRect = *pageRect;
        // can't cache bitmaps that aren't for a given tile
        CrashIf(!renderCb);
    } else {
        CrashMe();
    }
    newRequest->abort = false;
    newRequest->abortCookie = nullptr;
    newRequest->timestamp = GetTickCount();
    newRequest->renderCb = renderCb;

    SetEvent(startRendering);

    return true;
}

int RenderCache::GetRenderDelay(DisplayModel* dm, int pageNo, TilePosition tile) {
    ScopedCritSec scope(&requestAccess);

    if (curReq && curReq->pageNo == pageNo && curReq->dm == dm && curReq->tile == tile) {
        return GetTickCount() - curReq->timestamp;
    }

    for (int i = 0; i < requestCount; i++) {
        if (requests[i].pageNo == pageNo && requests[i].dm == dm && requests[i].tile == tile) {
            return GetTickCount() - requests[i].timestamp;
        }
    }

    return RENDER_DELAY_UNDEFINED;
}

bool RenderCache::GetNextRequest(PageRenderRequest* req) {
    ScopedCritSec scope(&requestAccess);

    if (requestCount == 0) {
        return false;
    }

    CrashIf(requestCount < 0);
    CrashIf(requestCount > MAX_PAGE_REQUESTS);
    requestCount--;
    *req = requests[requestCount];
    curReq = req;
    CrashIf(requestCount < 0);
    CrashIf(req->abort);

    return true;
}

bool RenderCache::ClearCurrentRequest() {
    ScopedCritSec scope(&requestAccess);
    if (curReq) {
        delete curReq->abortCookie;
    }
    curReq = nullptr;

    bool isQueueEmpty = requestCount == 0;
    return isQueueEmpty;
}

/* Wait until rendering of a page beloging to <dm> has finished. */
/* TODO: this might take some time, would be good to show a dialog to let the
   user know he has to wait until we finish */
void RenderCache::CancelRendering(DisplayModel* dm) {
    ClearQueueForDisplayModel(dm);

    for (;;) {
        EnterCriticalSection(&requestAccess);
        if (!curReq || (curReq->dm != dm)) {
            // to be on the safe side
            ClearQueueForDisplayModel(dm);
            LeaveCriticalSection(&requestAccess);
            return;
        }

        AbortCurrentRequest();
        LeaveCriticalSection(&requestAccess);

        /* TODO: busy loop is not good, but I don't have a better idea */
        Sleep(50);
    }
}

void RenderCache::ClearQueueForDisplayModel(DisplayModel* dm, int pageNo, TilePosition* tile) {
    ScopedCritSec scope(&requestAccess);
    int reqCount = requestCount;
    int curPos = 0;
    for (int i = 0; i < reqCount; i++) {
        PageRenderRequest* req = &(requests[i]);
        bool shouldRemove = req->dm == dm && (pageNo == INVALID_PAGE_NO || req->pageNo == pageNo) &&
                            (!tile || req->tile.res != tile->res || !IsTileVisible(dm, req->pageNo, *tile, 0.5));
        if (i != curPos) {
            requests[curPos] = requests[i];
        }
        if (shouldRemove) {
            if (req->renderCb) {
                req->renderCb->Callback();
            }
            requestCount--;
        } else {
            curPos++;
        }
    }
}

void RenderCache::AbortCurrentRequest() {
    ScopedCritSec scope(&requestAccess);
    if (!curReq) {
        return;
    }
    if (curReq->abortCookie) {
        curReq->abortCookie->Abort();
    }
    curReq->abort = true;
}

DWORD WINAPI RenderCache::RenderCacheThread(LPVOID data) {
    RenderCache* cache = (RenderCache*)data;
    PageRenderRequest req;
    RenderedBitmap* bmp;

    for (;;) {
        if (cache->ClearCurrentRequest()) {
            DWORD waitResult = WaitForSingleObject(cache->startRendering, INFINITE);
            // Is it not a page render request?
            if (WAIT_OBJECT_0 != waitResult) {
                continue;
            }
        }

        if (!cache->GetNextRequest(&req)) {
            continue;
        }

        if (!req.dm->PageVisibleNearby(req.pageNo) && !req.renderCb) {
            continue;
        }

        if (req.dm->dontRenderFlag) {
            if (req.renderCb) {
                req.renderCb->Callback();
            }
            continue;
        }

        // make sure that we have extracted page text for
        // all rendered pages to allow text selection and
        // searching without any further delays
        if (!req.dm->textCache->HasTextForPage(req.pageNo)) {
            req.dm->textCache->GetTextForPage(req.pageNo);
        }

        CrashIf(req.abortCookie != nullptr);
        EngineBase* engine = req.dm->GetEngine();
        RenderPageArgs args(req.pageNo, req.zoom, req.rotation, &req.pageRect, RenderTarget::View, &req.abortCookie);
        bmp = engine->RenderPage(args);
        if (req.abort) {
            delete bmp;
            if (req.renderCb) {
                req.renderCb->Callback(nullptr);
            }
            continue;
        }

        if (req.renderCb) {
            // the callback must free the RenderedBitmap
            req.renderCb->Callback(bmp);
            req.renderCb = (RenderingCallback*)1; // will crash if accessed again, which should not happen
        } else {
            // don't replace colors for individual images
            if (bmp && !engine->IsImageCollection()) {
                UpdateBitmapColors(bmp->GetBitmap(), cache->textColor, cache->backgroundColor);
            }
            cache->Add(req, bmp);
            req.dm->RepaintDisplay();
        }
        ResetTempAllocator();
    }
    DestroyTempAllocator();
}

// TODO: conceptually, RenderCache is not the right place for code that paints
//       (this is the only place that knows about Tiles, though)
int RenderCache::PaintTile(HDC hdc, Rect bounds, DisplayModel* dm, int pageNo, TilePosition tile, Rect tileOnScreen,
                           bool renderMissing, bool* renderOutOfDateCue, bool* renderedReplacement) {
    float zoom = dm->GetZoomReal(pageNo);
    BitmapCacheEntry* entry = Find(dm, pageNo, dm->GetRotation(), zoom, &tile);
    int renderDelay = 0;

    if (!entry) {
        if (!isRemoteSession) {
            if (renderedReplacement) {
                *renderedReplacement = true;
            }
            entry = Find(dm, pageNo, dm->GetRotation(), INVALID_ZOOM, &tile);
        }
        renderDelay = GetRenderDelay(dm, pageNo, tile);
        if (renderMissing && RENDER_DELAY_UNDEFINED == renderDelay && !IsRenderQueueFull()) {
            RequestRendering(dm, pageNo, tile);
        }
    }
    RenderedBitmap* renderedBmp = entry ? entry->bitmap : nullptr;
    HBITMAP hbmp = renderedBmp ? renderedBmp->GetBitmap() : nullptr;

    if (!hbmp) {
        if (entry && !(renderedBmp && ReduceTileSize())) {
            renderDelay = RENDER_DELAY_FAILED;
        } else if (0 == renderDelay) {
            renderDelay = 1;
        }

        if (entry) {
            DropCacheEntry(entry);
        }
        return renderDelay;
    }

    HDC bmpDC = CreateCompatibleDC(hdc);
    if (bmpDC) {
        Size bmpSize = renderedBmp->Size();
        int xSrc = -std::min(tileOnScreen.x, 0);
        int ySrc = -std::min(tileOnScreen.y, 0);
        float factor = std::min(1.0f * bmpSize.dx / tileOnScreen.dx, 1.0f * bmpSize.dy / tileOnScreen.dy);

        HGDIOBJ prevBmp = SelectObject(bmpDC, hbmp);
        int xDst = bounds.x;
        int yDst = bounds.y;
        int dxDst = bounds.dx;
        int dyDst = bounds.dy;
        if (factor != 1.0f) {
            xSrc = (int)(xSrc * factor);
            ySrc = (int)(ySrc * factor);
            int dxSrc = (int)(bounds.dx * factor);
            int dySrc = (int)(bounds.dy * factor);
            StretchBlt(hdc, xDst, yDst, dxDst, dyDst, bmpDC, xSrc, ySrc, dxSrc, dySrc, SRCCOPY);
        } else {
            BitBlt(hdc, xDst, yDst, dxDst, dyDst, bmpDC, xSrc, ySrc, SRCCOPY);
        }

        SelectObject(bmpDC, prevBmp);
        DeleteDC(bmpDC);

        if (gShowTileLayout) {
            HPEN pen = CreatePen(PS_SOLID, 1, RGB(0xff, 0xff, 0x00));
            HGDIOBJ oldPen = SelectObject(hdc, pen);
            PaintRect(hdc, bounds);
            DeletePen(SelectObject(hdc, oldPen));
        }
    }

    if (entry->outOfDate) {
        if (renderOutOfDateCue) {
            *renderOutOfDateCue = true;
        }
        CrashIf(renderedReplacement && !*renderedReplacement);
    }

    DropCacheEntry(entry);
    return 0;
}

static int cmpTilePosition(const void* a, const void* b) {
    const TilePosition *ta = (const TilePosition*)a, *tb = (const TilePosition*)b;
    return ta->res != tb->res ? ta->res - tb->res : ta->row != tb->row ? ta->row - tb->row : ta->col - tb->col;
}

int RenderCache::Paint(HDC hdc, Rect bounds, DisplayModel* dm, int pageNo, PageInfo* pageInfo,
                       bool* renderOutOfDateCue) {
    CrashIf(!pageInfo->shown || 0.0 == pageInfo->visibleRatio);

#if 0
    auto timeStart = TimeGet();
    defer {
        auto dur = TimeSinceInMs(timeStart);
        logf("RenderCache::Paint() pageNo: %d, bounds={%d,%d,%d,%d} in %.2f ms\n", pageNo, bounds.x, bounds.y, bounds.dx,
             bounds.dy, dur);
    };
#endif

    if (!dm->ShouldCacheRendering(pageNo)) {
        int rotation = dm->GetRotation();
        float zoom = dm->GetZoomReal(pageNo);
        bounds = pageInfo->pageOnScreen.Intersect(bounds);

        RectF area = ToRectF(bounds);
        area.Offset(-pageInfo->pageOnScreen.x, -pageInfo->pageOnScreen.y);
        area = dm->GetEngine()->Transform(area, pageNo, zoom, rotation, true);

        RenderPageArgs args(pageNo, zoom, rotation, &area);
        RenderedBitmap* bmp = dm->GetEngine()->RenderPage(args);
        bool success = bmp && bmp->GetBitmap() && bmp->StretchDIBits(hdc, bounds);
        delete bmp;

        return success ? 0 : RENDER_DELAY_FAILED;
    }

    int rotation = dm->GetRotation();
    float zoom = dm->GetZoomReal(pageNo);
    USHORT targetRes = GetTileRes(dm, pageNo);
    USHORT maxRes = GetMaxTileRes(dm, pageNo, rotation);
    if (maxRes < targetRes) {
        maxRes = targetRes;
    }

    Vec<TilePosition> queue;
    queue.Append(TilePosition(0, 0, 0));
    int renderDelayMin = RENDER_DELAY_UNDEFINED;
    bool neededScaling = false;

    while (queue.size() > 0) {
        TilePosition tile = queue.PopAt(0);
        Rect tileOnScreen = GetTileOnScreen(dm->GetEngine(), pageNo, rotation, zoom, tile, pageInfo->pageOnScreen);
        if (tileOnScreen.IsEmpty()) {
            // display an error message when only empty tiles should be drawn (i.e. on page loading errors)
            renderDelayMin = std::min(RENDER_DELAY_FAILED, renderDelayMin);
            continue;
        }
        tileOnScreen = pageInfo->pageOnScreen.Intersect(tileOnScreen);
        Rect isect = bounds.Intersect(tileOnScreen);
        if (isect.IsEmpty()) {
            continue;
        }

        bool isTargetRes = tile.res == targetRes;
        int renderDelay = PaintTile(hdc, isect, dm, pageNo, tile, tileOnScreen, isTargetRes, renderOutOfDateCue,
                                    isTargetRes ? &neededScaling : nullptr);
        if (!(isTargetRes && 0 == renderDelay) && tile.res < maxRes) {
            queue.Append(TilePosition(tile.res + 1, tile.row * 2, tile.col * 2));
            queue.Append(TilePosition(tile.res + 1, tile.row * 2, tile.col * 2 + 1));
            queue.Append(TilePosition(tile.res + 1, tile.row * 2 + 1, tile.col * 2));
            queue.Append(TilePosition(tile.res + 1, tile.row * 2 + 1, tile.col * 2 + 1));
        }
        if (isTargetRes && renderDelay != 0) {
            neededScaling = true;
        }
        if (renderDelay == RENDER_DELAY_FAILED || renderDelayMin == RENDER_DELAY_FAILED) {
            renderDelayMin = RENDER_DELAY_FAILED;
        } else {
            renderDelayMin = std::min(renderDelay, renderDelayMin);
        }
        // paint tiles from left to right from top to bottom
        if (tile.res > 0 && queue.size() > 0 && tile.res < queue.at(0).res) {
            queue.Sort(cmpTilePosition);
        }
    }

#ifdef CONSERVE_MEMORY
    if (!neededScaling) {
        if (renderOutOfDateCue) {
            *renderOutOfDateCue = false;
        }
        // free tiles with different resolution
        TilePosition tile(targetRes, (USHORT)-1, 0);
        logf("RenderCache::Paint: calling FreePage() pageNo: %d\n", pageNo);
        FreePage(dm, pageNo, &tile);
    }
    FreeNotVisible();
#endif

    return renderDelayMin;
}
