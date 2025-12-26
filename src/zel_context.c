#include "zel_internal.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static int zelValidateHeader(const ZELFileHeader *h) {
    if (memcmp(h->magic, "ZEL0", 4) != 0)
        return 0;
    if (h->version != 1)
        return 0;
    if (h->width == 0 || h->height == 0)
        return 0;
    if (h->zoneWidth == 0 || h->zoneHeight == 0)
        return 0;
    if ((h->width % h->zoneWidth) != 0 || (h->height % h->zoneHeight) != 0)
        return 0;

    uint32_t zonesPerRow = h->width / h->zoneWidth;
    uint32_t zonesPerCol = h->height / h->zoneHeight;
    uint32_t zoneCount = zonesPerRow * zonesPerCol;

    if (zonesPerRow == 0 || zonesPerCol == 0 || zoneCount == 0)
        return 0;
    if (zoneCount > UINT16_MAX)
        return 0;
    if (h->colorFormat != ZEL_COLOR_FORMAT_INDEXED8)
        return 0;
    return 1;
}

int zelIsValidColorEncoding(uint8_t encoding) {
    return encoding == ZEL_COLOR_RGB565_LE || encoding == ZEL_COLOR_RGB565_BE;
}

uint16_t zelSwapRgb565(uint16_t value) {
    return (uint16_t)(((value & 0x00FFu) << 8) | ((value & 0xFF00u) >> 8));
}

int zelRangeFits(size_t offset, size_t length, size_t limit) {
    if (length > limit)
        return 0;
    return offset <= limit - length;
}

ZELResult zelReadAt(const ZELContext *ctx, size_t offset, void *dst, size_t length) {
    if (!ctx || (!dst && length > 0))
        return ZEL_ERR_INVALID_ARGUMENT;
    if (length == 0)
        return ZEL_OK;

    if (!zelRangeFits(offset, length, ctx->size))
        return ZEL_ERR_CORRUPT_DATA;

    if (ctx->data) {
        memcpy(dst, ctx->data + offset, length);
        return ZEL_OK;
    }

    if (!ctx->stream.read)
        return ZEL_ERR_INTERNAL;

    size_t bytesRead = ctx->stream.read(ctx->stream.userData, offset, dst, length);
    if (bytesRead != length)
        return ZEL_ERR_IO;

    return ZEL_OK;
}

static ZELContext *zelCreateContext(void) {
    ZELContext *ctx = (ZELContext *)malloc(sizeof(ZELContext));
    if (!ctx)
        return NULL;

    memset(ctx, 0, sizeof(ZELContext));
    ctx->globalPaletteEncoding = ZEL_COLOR_RGB565_LE;
    ctx->globalPaletteConvertedEncoding = (ZELColorEncoding)255;
    ctx->outputColorEncoding = ZEL_COLOR_RGB565_LE;
    return ctx;
}

ZELColorEncoding zelSelectOutputEncoding(const ZELContext *ctx, ZELColorEncoding sourceEncoding) {
    if (ctx->hasCustomOutputEncoding)
        return ctx->outputColorEncoding;
    return sourceEncoding;
}

uint8_t *zelAcquireZoneScratch(const ZELContext *ctx, size_t neededBytes) {
    if (!ctx || neededBytes == 0)
        return NULL;

    ZELContext *mutableCtx = (ZELContext *)ctx;
    if (mutableCtx->zoneScratchCapacity < neededBytes) {
        uint8_t *newBuf = (uint8_t *)realloc(mutableCtx->zoneScratch, neededBytes);
        if (!newBuf)
            return NULL;
        mutableCtx->zoneScratch = newBuf;
        mutableCtx->zoneScratchCapacity = neededBytes;
    }

    return mutableCtx->zoneScratch;
}

uint16_t *zelAcquirePaletteScratch(const ZELContext *ctx, size_t neededEntries) {
    if (!ctx || neededEntries == 0)
        return NULL;

    ZELContext *mutableCtx = (ZELContext *)ctx;
    if (mutableCtx->paletteScratchCapacity < neededEntries) {
        size_t neededBytes = neededEntries * sizeof(uint16_t);
        uint16_t *newBuf = (uint16_t *)realloc(mutableCtx->paletteScratch, neededBytes);
        if (!newBuf)
            return NULL;
        mutableCtx->paletteScratch = newBuf;
        mutableCtx->paletteScratchCapacity = neededEntries;
    }

    return mutableCtx->paletteScratch;
}

static ZELResult zelInitializeContext(ZELContext *ctx) {
    if (!ctx)
        return ZEL_ERR_INVALID_ARGUMENT;

    if (ctx->size < sizeof(ZELFileHeader))
        return ZEL_ERR_INVALID_ARGUMENT;

    ZELFileHeader tmpHeader;
    ZELResult result = zelReadAt(ctx, 0, &tmpHeader, sizeof(ZELFileHeader));
    if (result != ZEL_OK)
        return result;

    if (!zelValidateHeader(&tmpHeader))
        return ZEL_ERR_INVALID_MAGIC;

    if (tmpHeader.headerSize > ctx->size)
        return ZEL_ERR_CORRUPT_DATA;

    memcpy(&ctx->header, &tmpHeader, sizeof(ZELFileHeader));

    size_t offset = ctx->header.headerSize;

    if (offset > ctx->size)
        return ZEL_ERR_CORRUPT_DATA;

    if (ctx->header.flags.hasGlobalPalette) {
        if (!zelRangeFits(offset, sizeof(ZELPaletteHeader), ctx->size))
            return ZEL_ERR_CORRUPT_DATA;

        ZELPaletteHeader ph;
        result = zelReadAt(ctx, offset, &ph, sizeof(ZELPaletteHeader));
        if (result != ZEL_OK)
            return result;

        if (!zelIsValidColorEncoding(ph.colorEncoding))
            return ZEL_ERR_UNSUPPORTED_FORMAT;

        if (ph.entryCount == 0)
            return ZEL_ERR_CORRUPT_DATA;

        size_t paletteDataOffset = offset + ph.headerSize;
        size_t paletteBytes = (size_t)ph.entryCount * sizeof(uint16_t);

        if (ph.headerSize < sizeof(ZELPaletteHeader))
            return ZEL_ERR_CORRUPT_DATA;

        if (!zelRangeFits(paletteDataOffset, paletteBytes, ctx->size))
            return ZEL_ERR_CORRUPT_DATA;

        if (ctx->data) {
            ctx->globalPaletteRaw = (const uint16_t *)(ctx->data + paletteDataOffset);
        } else {
            uint16_t *entries = (uint16_t *)malloc(paletteBytes);
            if (!entries)
                return ZEL_ERR_OUT_OF_MEMORY;
            result = zelReadAt(ctx, paletteDataOffset, entries, paletteBytes);
            if (result != ZEL_OK) {
                free(entries);
                return result;
            }
            ctx->globalPaletteRaw = entries;
            ctx->globalPaletteOwned = entries;
        }

        ctx->globalPaletteCount = ph.entryCount;
        ctx->globalPaletteEncoding = (ZELColorEncoding)ph.colorEncoding;

        offset = paletteDataOffset + paletteBytes;
    }

    if (!ctx->header.flags.hasFrameIndexTable)
        return ZEL_ERR_UNSUPPORTED_FORMAT;

    size_t indexBytes = (size_t)ctx->header.frameCount * sizeof(ZELFrameIndexEntry);
    if (!zelRangeFits(offset, indexBytes, ctx->size))
        return ZEL_ERR_CORRUPT_DATA;

    if (ctx->data) {
        ctx->frameIndexTable = (const ZELFrameIndexEntry *)(ctx->data + offset);
    } else {
        ZELFrameIndexEntry *entries = (ZELFrameIndexEntry *)malloc(indexBytes);
        if (!entries)
            return ZEL_ERR_OUT_OF_MEMORY;
        result = zelReadAt(ctx, offset, entries, indexBytes);
        if (result != ZEL_OK) {
            free(entries);
            return result;
        }
        ctx->frameIndexTable = entries;
        ctx->frameIndexOwned = entries;
    }

    return ZEL_OK;
}

ZELContext *zelOpenMemory(const uint8_t *data, size_t size, ZELResult *outResult) {
    ZELResult result = ZEL_OK;
    ZELContext *ctx = NULL;

    if (!data || size < sizeof(ZELFileHeader)) {
        result = ZEL_ERR_INVALID_ARGUMENT;
        goto fail;
    }

    ctx = zelCreateContext();
    if (!ctx) {
        result = ZEL_ERR_OUT_OF_MEMORY;
        goto fail;
    }

    ctx->data = data;
    ctx->size = size;

    result = zelInitializeContext(ctx);
    if (result != ZEL_OK)
        goto fail;

    if (outResult)
        *outResult = ZEL_OK;
    return ctx;

fail:
    if (ctx)
        zelClose(ctx);
    if (outResult)
        *outResult = result;
    return NULL;
}

ZELContext *zelOpenStream(const ZELInputStream *stream, ZELResult *outResult) {
    ZELResult result = ZEL_OK;
    ZELContext *ctx = NULL;

    if (!stream || !stream->read || stream->size < sizeof(ZELFileHeader)) {
        result = ZEL_ERR_INVALID_ARGUMENT;
        goto fail;
    }

    ctx = zelCreateContext();
    if (!ctx) {
        result = ZEL_ERR_OUT_OF_MEMORY;
        goto fail;
    }

    ctx->data = NULL;
    ctx->size = stream->size;
    ctx->stream = *stream;

    result = zelInitializeContext(ctx);
    if (result != ZEL_OK)
        goto fail;

    if (outResult)
        *outResult = ZEL_OK;
    return ctx;

fail:
    if (ctx)
        zelClose(ctx);
    if (outResult)
        *outResult = result;
    return NULL;
}

void zelClose(ZELContext *ctx) {
    if (!ctx)
        return;

    if (ctx->stream.close)
        ctx->stream.close(ctx->stream.userData);

    if (ctx->globalPaletteConverted)
        free(ctx->globalPaletteConverted);

    if (ctx->globalPaletteOwned)
        free(ctx->globalPaletteOwned);

    if (ctx->zoneScratch)
        free(ctx->zoneScratch);

    if (ctx->frameDataScratch)
        free(ctx->frameDataScratch);

    if (ctx->paletteScratch)
        free(ctx->paletteScratch);

    if (ctx->frameIndexOwned)
        free(ctx->frameIndexOwned);

    free(ctx);
}

uint16_t zelGetWidth(const ZELContext *ctx) {
    return ctx ? ctx->header.width : 0;
}

uint16_t zelGetHeight(const ZELContext *ctx) {
    return ctx ? ctx->header.height : 0;
}

uint32_t zelGetFrameCount(const ZELContext *ctx) {
    return ctx ? ctx->header.frameCount : 0;
}

uint16_t zelGetDefaultFrameDurationMs(const ZELContext *ctx) {
    return ctx ? ctx->header.defaultFrameDuration : 0;
}

uint16_t zelGetZoneWidth(const ZELContext *ctx) {
    return ctx ? ctx->header.zoneWidth : 0;
}

uint16_t zelGetZoneHeight(const ZELContext *ctx) {
    return ctx ? ctx->header.zoneHeight : 0;
}

ZELColorFormat zelGetColorFormat(const ZELContext *ctx) {
    return ctx ? (ZELColorFormat)ctx->header.colorFormat : ZEL_COLOR_FORMAT_INDEXED8;
}

void zelSetOutputColorEncoding(ZELContext *ctx, ZELColorEncoding encoding) {
    if (!ctx)
        return;

    if (!zelIsValidColorEncoding((uint8_t)encoding))
        return;

    if (!ctx->hasCustomOutputEncoding || ctx->outputColorEncoding != encoding) {
        ctx->outputColorEncoding = encoding;
        ctx->hasCustomOutputEncoding = 1;
        ctx->globalPaletteConvertedEncoding = (ZELColorEncoding)255;
    }
}

ZELColorEncoding zelGetOutputColorEncoding(const ZELContext *ctx) {
    if (!ctx)
        return ZEL_COLOR_RGB565_LE;

    if (ctx->hasCustomOutputEncoding)
        return ctx->outputColorEncoding;

    return ctx->globalPaletteEncoding;
}

int zelHasGlobalPalette(const ZELContext *ctx) {
    return (ctx && ctx->globalPaletteRaw && ctx->globalPaletteCount > 0);
}

ZELResult zelGetTotalDurationMs(const ZELContext *ctx, uint32_t *outTotalDurationMs) {
    if (!ctx || !outTotalDurationMs)
        return ZEL_ERR_INVALID_ARGUMENT;

    uint32_t total = 0;
    uint32_t frameCount = ctx->header.frameCount;

    for (uint32_t i = 0; i < frameCount; ++i) {
        uint16_t duration = 0;
        ZELResult r = zelGetFrameDurationMs(ctx, i, &duration);
        if (r != ZEL_OK)
            return r;
        total += (uint32_t)duration;
    }

    *outTotalDurationMs = total;
    return ZEL_OK;
}

ZELResult zelFindFrameByTimeMs(const ZELContext *ctx,
                               uint32_t timeMs,
                               uint32_t *outFrameIndex,
                               uint32_t *outFrameStartMs) {
    if (!ctx || !outFrameIndex || !outFrameStartMs)
        return ZEL_ERR_INVALID_ARGUMENT;

    uint32_t totalDuration = 0;
    ZELResult r = zelGetTotalDurationMs(ctx, &totalDuration);
    if (r != ZEL_OK)
        return r;

    if (totalDuration == 0)
        return ZEL_ERR_CORRUPT_DATA;

    uint32_t t = timeMs % totalDuration;
    uint32_t frameCount = ctx->header.frameCount;
    uint32_t accum = 0;

    for (uint32_t i = 0; i < frameCount; ++i) {
        uint16_t duration = 0;
        r = zelGetFrameDurationMs(ctx, i, &duration);
        if (r != ZEL_OK)
            return r;

        uint32_t next = accum + (uint32_t)duration;
        if (t < next) {
            *outFrameIndex = i;
            *outFrameStartMs = accum;
            return ZEL_OK;
        }

        accum = next;
    }

    *outFrameIndex = frameCount - 1;
    *outFrameStartMs = totalDuration - 1;
    return ZEL_OK;
}

const char *zelResultToString(ZELResult result) {
    switch (result) {
        case ZEL_OK:
            return "ZEL_OK";
        case ZEL_ERR_INVALID_ARGUMENT:
            return "ZEL_ERR_INVALID_ARGUMENT";
        case ZEL_ERR_INVALID_MAGIC:
            return "ZEL_ERR_INVALID_MAGIC";
        case ZEL_ERR_UNSUPPORTED_VERSION:
            return "ZEL_ERR_UNSUPPORTED_VERSION";
        case ZEL_ERR_UNSUPPORTED_FORMAT:
            return "ZEL_ERR_UNSUPPORTED_FORMAT";
        case ZEL_ERR_CORRUPT_DATA:
            return "ZEL_ERR_CORRUPT_DATA";
        case ZEL_ERR_OUT_OF_MEMORY:
            return "ZEL_ERR_OUT_OF_MEMORY";
        case ZEL_ERR_OUT_OF_BOUNDS:
            return "ZEL_ERR_OUT_OF_BOUNDS";
        case ZEL_ERR_IO:
            return "ZEL_ERR_IO";
        case ZEL_ERR_INTERNAL:
            return "ZEL_ERR_INTERNAL";
        default:
            return "ZEL_ERR_UNKNOWN";
    }
}
