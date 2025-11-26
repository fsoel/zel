#include "zel/zel.h"

#include "lz4/lz4.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

struct ZELContext {
    const uint8_t *data;
    size_t size;

    ZELFileHeader header;

    const ZELFrameIndexEntry *frameIndexTable;
    const uint16_t *globalPaletteRaw;
    uint16_t *globalPaletteConverted;
    size_t globalPaletteConvertedCapacity;
    uint16_t globalPaletteCount;
    ZELColorEncoding globalPaletteEncoding;
    ZELColorEncoding globalPaletteConvertedEncoding;

    int hasCustomOutputEncoding;
    ZELColorEncoding outputColorEncoding;

    uint8_t *zoneScratch;
    size_t zoneScratchCapacity;
    uint16_t *paletteScratch;
    size_t paletteScratchCapacity;
};

typedef struct {
    uint16_t zoneWidth;
    uint16_t zoneHeight;
    uint32_t zonesPerRow;
    uint32_t zonesPerCol;
    uint32_t zoneCount;
    size_t zonePixelBytes;
} ZELZoneLayout;

typedef struct {
    const ZELFrameHeader *header;
    size_t frameOffset;
    size_t frameSize;
    size_t zoneDataOffset;
    size_t frameDataEnd;
    ZELZoneLayout layout;
} ZELFrameZoneStream;

static int zelIsValidColorEncoding(uint8_t encoding) {
    return encoding == ZEL_COLOR_RGB565_LE || encoding == ZEL_COLOR_RGB565_BE;
}

static uint16_t zelSwapRgb565(uint16_t value) {
    return (uint16_t)(((value & 0x00FFu) << 8) | ((value & 0xFF00u) >> 8));
}

static uint8_t *zelAcquireZoneScratch(const ZELContext *ctx, size_t neededBytes) {
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

static uint16_t *zelAcquirePaletteScratch(const ZELContext *ctx, size_t neededEntries) {
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

static void zelConvertPaletteEncoding(const uint16_t *src,
                                      uint16_t *dst,
                                      uint16_t count,
                                      ZELColorEncoding srcEncoding,
                                      ZELColorEncoding dstEncoding) {
    if (srcEncoding == dstEncoding) {
        memcpy(dst, src, (size_t)count * sizeof(uint16_t));
        return;
    }

    for (uint16_t i = 0; i < count; ++i)
        dst[i] = zelSwapRgb565(src[i]);
}

static ZELColorEncoding zelSelectOutputEncoding(const ZELContext *ctx,
                                                ZELColorEncoding sourceEncoding) {
    if (ctx->hasCustomOutputEncoding)
        return ctx->outputColorEncoding;
    return sourceEncoding;
}

static void zelZoneIndexToCoordinates(const ZELZoneLayout *layout,
                                      uint32_t zoneIndex,
                                      uint32_t *outX,
                                      uint32_t *outY) {
    uint32_t zonesPerRow = layout->zonesPerRow;
    *outX = (zoneIndex % zonesPerRow) * layout->zoneWidth;
    *outY = (zoneIndex / zonesPerRow) * layout->zoneHeight;
}

static ZELResult zelComputeZoneLayout(const ZELContext *ctx, ZELZoneLayout *outLayout) {
    if (!ctx || !outLayout)
        return ZEL_ERR_INVALID_ARGUMENT;

    if (ctx->header.zoneWidth == 0 || ctx->header.zoneHeight == 0)
        return ZEL_ERR_CORRUPT_DATA;

    if ((ctx->header.width % ctx->header.zoneWidth) != 0
        || (ctx->header.height % ctx->header.zoneHeight) != 0) {
        return ZEL_ERR_CORRUPT_DATA;
    }

    uint32_t zonesPerRow = ctx->header.width / ctx->header.zoneWidth;
    uint32_t zonesPerCol = ctx->header.height / ctx->header.zoneHeight;
    uint32_t zoneCount = zonesPerRow * zonesPerCol;

    if (zonesPerRow == 0 || zonesPerCol == 0 || zoneCount == 0)
        return ZEL_ERR_CORRUPT_DATA;

    if (zoneCount > UINT16_MAX)
        return ZEL_ERR_UNSUPPORTED_FORMAT;

    size_t zonePixelBytes = (size_t)ctx->header.zoneWidth * (size_t)ctx->header.zoneHeight;
    if (zonePixelBytes == 0)
        return ZEL_ERR_CORRUPT_DATA;

    outLayout->zoneWidth = ctx->header.zoneWidth;
    outLayout->zoneHeight = ctx->header.zoneHeight;
    outLayout->zonesPerRow = zonesPerRow;
    outLayout->zonesPerCol = zonesPerCol;
    outLayout->zoneCount = zoneCount;
    outLayout->zonePixelBytes = zonePixelBytes;
    return ZEL_OK;
}

static ZELResult zelInitFrameZoneStream(const ZELContext *ctx,
                                        uint32_t frameIndex,
                                        ZELFrameZoneStream *outStream) {
    if (!ctx || !outStream)
        return ZEL_ERR_INVALID_ARGUMENT;

    if (frameIndex >= ctx->header.frameCount)
        return ZEL_ERR_OUT_OF_BOUNDS;

    const ZELFrameIndexEntry *fi = &ctx->frameIndexTable[frameIndex];
    size_t frameOffset = fi->frameOffset;
    size_t frameSize = fi->frameSize;

    if (frameSize == 0 || frameOffset + sizeof(ZELFrameHeader) > ctx->size
        || frameOffset + frameSize > ctx->size) {
        return ZEL_ERR_CORRUPT_DATA;
    }

    const ZELFrameHeader *fh = (const ZELFrameHeader *)(ctx->data + frameOffset);
    if (fh->headerSize < sizeof(ZELFrameHeader))
        return ZEL_ERR_CORRUPT_DATA;

    size_t frameEnd = frameOffset + frameSize;
    size_t offset = frameOffset + fh->headerSize;
    if (offset > frameEnd)
        return ZEL_ERR_CORRUPT_DATA;

    if (fh->flags.hasLocalPalette) {
        if (offset + sizeof(ZELPaletteHeader) > ctx->size
            || offset + sizeof(ZELPaletteHeader) > frameEnd)
            return ZEL_ERR_CORRUPT_DATA;

        const ZELPaletteHeader *ph = (const ZELPaletteHeader *)(ctx->data + offset);
        if (ph->headerSize < sizeof(ZELPaletteHeader))
            return ZEL_ERR_CORRUPT_DATA;

        size_t paletteDataOffset = offset + ph->headerSize;
        size_t paletteBytes = (size_t)ph->entryCount * sizeof(uint16_t);

        if (paletteDataOffset + paletteBytes > ctx->size
            || paletteDataOffset + paletteBytes > frameEnd) {
            return ZEL_ERR_CORRUPT_DATA;
        }

        offset = paletteDataOffset + paletteBytes;
    }

    if (offset > frameEnd)
        return ZEL_ERR_CORRUPT_DATA;

    ZELZoneLayout layout;
    ZELResult zr = zelComputeZoneLayout(ctx, &layout);
    if (zr != ZEL_OK)
        return zr;

    if (layout.zoneCount == 0 || fh->zoneCount != (uint16_t)layout.zoneCount)
        return ZEL_ERR_CORRUPT_DATA;

    outStream->header = fh;
    outStream->frameOffset = frameOffset;
    outStream->frameSize = frameSize;
    outStream->zoneDataOffset = offset;
    outStream->frameDataEnd = frameEnd;
    outStream->layout = layout;
    return ZEL_OK;
}

static ZELResult zelReadZoneChunkAtCursor(const ZELContext *ctx,
                                          const ZELFrameZoneStream *stream,
                                          size_t *cursor,
                                          const uint8_t **outData,
                                          uint32_t *outSize) {
    if (*cursor + sizeof(uint32_t) > stream->frameDataEnd)
        return ZEL_ERR_CORRUPT_DATA;

    uint32_t chunkSize = 0;
    memcpy(&chunkSize, ctx->data + *cursor, sizeof(uint32_t));
    *cursor += sizeof(uint32_t);

    if (chunkSize == 0)
        return ZEL_ERR_CORRUPT_DATA;

    if (*cursor + chunkSize > stream->frameDataEnd)
        return ZEL_ERR_CORRUPT_DATA;

    *outData = ctx->data + *cursor;
    *outSize = chunkSize;
    *cursor += chunkSize;
    return ZEL_OK;
}

static ZELResult zelLocateZoneChunk(const ZELContext *ctx,
                                    const ZELFrameZoneStream *stream,
                                    uint32_t targetZone,
                                    const uint8_t **outData,
                                    uint32_t *outSize) {
    size_t cursor = stream->zoneDataOffset;
    ZELResult result = ZEL_OK;
    const uint8_t *chunkData = NULL;
    uint32_t chunkSize = 0;

    for (uint32_t idx = 0; idx <= targetZone; ++idx) {
        result = zelReadZoneChunkAtCursor(ctx, stream, &cursor, &chunkData, &chunkSize);
        if (result != ZEL_OK)
            return result;
    }

    *outData = chunkData;
    *outSize = chunkSize;
    return ZEL_OK;
}

static ZELResult zelAccessZonePixels(const ZELContext *ctx,
                                     const ZELFrameZoneStream *stream,
                                     const uint8_t *chunkData,
                                     uint32_t chunkSize,
                                     uint8_t *scratch,
                                     const uint8_t **outPixels) {
    (void)ctx;
    size_t zoneBytes = stream->layout.zonePixelBytes;

    switch (stream->header->compressionType) {
        case ZEL_COMPRESSION_NONE:
            if ((size_t)chunkSize != zoneBytes)
                return ZEL_ERR_CORRUPT_DATA;
            *outPixels = chunkData;
            return ZEL_OK;
        case ZEL_COMPRESSION_LZ4:
            if (!scratch)
                return ZEL_ERR_INTERNAL;
            if (zoneBytes > (size_t)INT32_MAX)
                return ZEL_ERR_UNSUPPORTED_FORMAT;
            if (chunkSize > (uint32_t)INT32_MAX)
                return ZEL_ERR_CORRUPT_DATA;
            {
                int decodedBytes = LZ4_decompress_safe((const char *)chunkData,
                                                       (char *)scratch,
                                                       (int)chunkSize,
                                                       (int)zoneBytes);
                if (decodedBytes < 0 || (size_t)decodedBytes != zoneBytes)
                    return ZEL_ERR_CORRUPT_DATA;
                *outPixels = scratch;
                return ZEL_OK;
            }
        default:
            return ZEL_ERR_UNSUPPORTED_FORMAT;
    }
}

static void zelBlitZoneIndices(const ZELZoneLayout *layout,
                               uint32_t zoneIndex,
                               const uint8_t *zonePixels,
                               uint8_t *dst,
                               size_t dstStrideBytes) {
    uint32_t zoneX = 0;
    uint32_t zoneY = 0;
    zelZoneIndexToCoordinates(layout, zoneIndex, &zoneX, &zoneY);

    for (uint32_t row = 0; row < layout->zoneHeight; ++row) {
        uint8_t *dstRow = dst + (size_t)(zoneY + row) * dstStrideBytes + zoneX;
        const uint8_t *srcRow = zonePixels + (size_t)row * layout->zoneWidth;
        memcpy(dstRow, srcRow, layout->zoneWidth);
    }
}

static ZELResult zelBlitZoneRgb(const ZELZoneLayout *layout,
                                uint32_t zoneIndex,
                                const uint8_t *zonePixels,
                                const uint16_t *palette,
                                uint16_t paletteCount,
                                uint16_t *dst,
                                size_t dstStridePixels) {
    uint32_t zoneX = 0;
    uint32_t zoneY = 0;
    zelZoneIndexToCoordinates(layout, zoneIndex, &zoneX, &zoneY);

    for (uint32_t row = 0; row < layout->zoneHeight; ++row) {
        uint16_t *dstRow = dst + (size_t)(zoneY + row) * dstStridePixels + zoneX;
        const uint8_t *srcRow = zonePixels + (size_t)row * layout->zoneWidth;

        for (uint32_t col = 0; col < layout->zoneWidth; ++col) {
            uint8_t idx = srcRow[col];
            if (idx >= paletteCount)
                return ZEL_ERR_CORRUPT_DATA;
            dstRow[col] = palette[idx];
        }
    }

    return ZEL_OK;
}

static int zelValidateHeader(const ZELFileHeader *h) {
    if (memcmp(h->magic, "ZEL0", 4) != 0) {
        return 0;
    }

    if (h->version != 1) {
        return 0;
    }

    if (h->width == 0 || h->height == 0) {
        return 0;
    }

    if (h->zoneWidth == 0 || h->zoneHeight == 0) {
        return 0;
    }

    if ((h->width % h->zoneWidth) != 0 || (h->height % h->zoneHeight) != 0) {
        return 0;
    }

    uint32_t zonesPerRow = h->width / h->zoneWidth;
    uint32_t zonesPerCol = h->height / h->zoneHeight;
    uint32_t zoneCount = zonesPerRow * zonesPerCol;

    if (zonesPerRow == 0 || zonesPerCol == 0 || zoneCount == 0) {
        return 0;
    }

    if (zoneCount > UINT16_MAX) {
        return 0;
    }

    if (h->colorFormat != ZEL_COLOR_FORMAT_INDEXED8) {
        return 0;
    }

    return 1;
}

ZELContext *zelOpenMemory(const uint8_t *data, size_t size, ZELResult *outResult) {
    ZELResult result = ZEL_OK;
    ZELContext *ctx = NULL;
    ZELFileHeader tmpHeader;
    size_t offset;

    if (data == NULL || size < sizeof(ZELFileHeader)) {
        result = ZEL_ERR_INVALID_ARGUMENT;
        goto fail;
    }

    memcpy(&tmpHeader, data, sizeof(ZELFileHeader));

    if (!zelValidateHeader(&tmpHeader)) {
        result = ZEL_ERR_INVALID_MAGIC;
        goto fail;
    }

    if (tmpHeader.headerSize > size) {
        result = ZEL_ERR_CORRUPT_DATA;
        goto fail;
    }

    ctx = (ZELContext *)malloc(sizeof(ZELContext));
    if (!ctx) {
        result = ZEL_ERR_OUT_OF_MEMORY;
        goto fail;
    }

    memset(ctx, 0, sizeof(ZELContext));
    ctx->data = data;
    ctx->size = size;
    memcpy(&ctx->header, &tmpHeader, sizeof(ZELFileHeader));
    ctx->globalPaletteRaw = NULL;
    ctx->globalPaletteConverted = NULL;
    ctx->globalPaletteConvertedCapacity = 0;
    ctx->globalPaletteEncoding = ZEL_COLOR_RGB565_LE;
    ctx->globalPaletteConvertedEncoding = (ZELColorEncoding)255;
    ctx->hasCustomOutputEncoding = 0;
    ctx->outputColorEncoding = ZEL_COLOR_RGB565_LE;
    ctx->zoneScratch = NULL;
    ctx->zoneScratchCapacity = 0;
    ctx->paletteScratch = NULL;
    ctx->paletteScratchCapacity = 0;

    offset = ctx->header.headerSize;

    if (offset > size) {
        result = ZEL_ERR_CORRUPT_DATA;
        goto fail;
    }

    if (ctx->header.flags.hasGlobalPalette) {
        if (offset + sizeof(ZELPaletteHeader) > size) {
            result = ZEL_ERR_CORRUPT_DATA;
            goto fail;
        }

        ZELPaletteHeader ph;
        memcpy(&ph, data + offset, sizeof(ZELPaletteHeader));

        if (!zelIsValidColorEncoding(ph.colorEncoding)) {
            result = ZEL_ERR_UNSUPPORTED_FORMAT;
            goto fail;
        }

        if (ph.entryCount == 0) {
            result = ZEL_ERR_CORRUPT_DATA;
            goto fail;
        }

        size_t paletteDataOffset = offset + ph.headerSize;
        size_t paletteBytes = (size_t)ph.entryCount * sizeof(uint16_t);

        if (ph.headerSize < sizeof(ZELPaletteHeader)) {
            result = ZEL_ERR_CORRUPT_DATA;
            goto fail;
        }

        if (paletteDataOffset + paletteBytes > size) {
            result = ZEL_ERR_CORRUPT_DATA;
            goto fail;
        }

        ctx->globalPaletteRaw = (const uint16_t *)(data + paletteDataOffset);
        ctx->globalPaletteCount = ph.entryCount;
        ctx->globalPaletteEncoding = (ZELColorEncoding)ph.colorEncoding;

        offset = paletteDataOffset + paletteBytes;
    }

    if (!ctx->header.flags.hasFrameIndexTable) {
        result = ZEL_ERR_UNSUPPORTED_FORMAT;
        goto fail;
    }

    {
        size_t needed = (size_t)ctx->header.frameCount * sizeof(ZELFrameIndexEntry);
        if (offset + needed > size) {
            result = ZEL_ERR_CORRUPT_DATA;
            goto fail;
        }

        ctx->frameIndexTable = (const ZELFrameIndexEntry *)(data + offset);
    }

    if (outResult) {
        *outResult = ZEL_OK;
    }
    return ctx;

fail:
    if (ctx) {
        zelClose(ctx);
        ctx = NULL;
    }
    if (outResult) {
        *outResult = result;
    }
    return NULL;
}

void zelClose(ZELContext *ctx) {
    if (!ctx) {
        return;
    }

    if (ctx->globalPaletteConverted)
        free(ctx->globalPaletteConverted);

    if (ctx->zoneScratch)
        free(ctx->zoneScratch);

    if (ctx->paletteScratch)
        free(ctx->paletteScratch);

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

static ZELResult zelResolveGlobalPalette(const ZELContext *ctx,
                                         const uint16_t **outEntries,
                                         uint16_t *outCount) {
    if (!ctx->globalPaletteRaw)
        return ZEL_ERR_OUT_OF_BOUNDS;

    ZELColorEncoding desired = zelSelectOutputEncoding(ctx, ctx->globalPaletteEncoding);

    if (desired == ctx->globalPaletteEncoding) {
        *outEntries = ctx->globalPaletteRaw;
        *outCount = ctx->globalPaletteCount;
        return ZEL_OK;
    }

    ZELContext *mutableCtx = (ZELContext *)ctx;
    size_t requiredEntries = ctx->globalPaletteCount;
    size_t requiredBytes = requiredEntries * sizeof(uint16_t);

    if (mutableCtx->globalPaletteConvertedCapacity < requiredEntries) {
        uint16_t *converted =
                (uint16_t *)realloc(mutableCtx->globalPaletteConverted, requiredBytes);
        if (!converted)
            return ZEL_ERR_OUT_OF_MEMORY;
        mutableCtx->globalPaletteConverted = converted;
        mutableCtx->globalPaletteConvertedCapacity = requiredEntries;
    }

    if (mutableCtx->globalPaletteConvertedEncoding != desired) {
        zelConvertPaletteEncoding(ctx->globalPaletteRaw,
                                  mutableCtx->globalPaletteConverted,
                                  ctx->globalPaletteCount,
                                  ctx->globalPaletteEncoding,
                                  desired);
        mutableCtx->globalPaletteConvertedEncoding = desired;
    }

    *outEntries = mutableCtx->globalPaletteConverted;
    *outCount = ctx->globalPaletteCount;
    return ZEL_OK;
}

ZELResult zelGetGlobalPalette(const ZELContext *ctx,
                              const uint16_t **outEntries,
                              uint16_t *outCount) {
    if (!ctx || !outEntries || !outCount)
        return ZEL_ERR_INVALID_ARGUMENT;

    return zelResolveGlobalPalette(ctx, outEntries, outCount);
}

static ZELResult zelResolveLocalPalette(const ZELContext *ctx,
                                        const ZELPaletteHeader *ph,
                                        const uint16_t *paletteData,
                                        const uint16_t **outEntries,
                                        uint16_t *outCount) {
    ZELColorEncoding sourceEncoding = (ZELColorEncoding)ph->colorEncoding;
    ZELColorEncoding desired = zelSelectOutputEncoding(ctx, sourceEncoding);

    if (desired == sourceEncoding) {
        *outEntries = paletteData;
        *outCount = ph->entryCount;
        return ZEL_OK;
    }

    uint16_t *scratch = zelAcquirePaletteScratch(ctx, ph->entryCount);
    if (!scratch)
        return ZEL_ERR_OUT_OF_MEMORY;

    zelConvertPaletteEncoding(paletteData,
                              scratch,
                              ph->entryCount,
                              sourceEncoding,
                              desired);

    *outEntries = scratch;
    *outCount = ph->entryCount;
    return ZEL_OK;
}

ZELResult zelGetFramePalette(const ZELContext *ctx,
                             uint32_t frameIndex,
                             const uint16_t **outEntries,
                             uint16_t *outCount) {
    if (!ctx || !outEntries || !outCount)
        return ZEL_ERR_INVALID_ARGUMENT;

    if (frameIndex >= ctx->header.frameCount)
        return ZEL_ERR_OUT_OF_BOUNDS;

    const ZELFrameIndexEntry *fi = &ctx->frameIndexTable[frameIndex];

    if (!fi->flags.hasLocalPalette)
        return zelResolveGlobalPalette(ctx, outEntries, outCount);

    size_t offset = fi->frameOffset;

    if (offset + sizeof(ZELFrameHeader) > ctx->size)
        return ZEL_ERR_CORRUPT_DATA;

    const ZELFrameHeader *fh = (const ZELFrameHeader *)(ctx->data + offset);

    if (fh->localPaletteEntryCount == 0)
        return ZEL_ERR_CORRUPT_DATA;

    size_t phOffset = offset + fh->headerSize;

    if (phOffset + sizeof(ZELPaletteHeader) > ctx->size)
        return ZEL_ERR_CORRUPT_DATA;

    const ZELPaletteHeader *ph = (const ZELPaletteHeader *)(ctx->data + phOffset);

    if (ph->headerSize < sizeof(ZELPaletteHeader))
        return ZEL_ERR_CORRUPT_DATA;

    if (!zelIsValidColorEncoding(ph->colorEncoding))
        return ZEL_ERR_UNSUPPORTED_FORMAT;

    size_t paletteDataOffset = phOffset + ph->headerSize;
    size_t paletteBytes = (size_t)ph->entryCount * sizeof(uint16_t);

    if (paletteDataOffset + paletteBytes > ctx->size)
        return ZEL_ERR_CORRUPT_DATA;

    const uint16_t *paletteData = (const uint16_t *)(ctx->data + paletteDataOffset);

    return zelResolveLocalPalette(ctx, ph, paletteData, outEntries, outCount);
}

ZELResult zelGetFrameDurationMs(const ZELContext *ctx,
                                uint32_t frameIndex,
                                uint16_t *outDurationMs) {
    if (!ctx || !outDurationMs)
        return ZEL_ERR_INVALID_ARGUMENT;

    if (frameIndex >= ctx->header.frameCount)
        return ZEL_ERR_OUT_OF_BOUNDS;

    const ZELFrameIndexEntry *fi = &ctx->frameIndexTable[frameIndex];

    if (fi->frameDuration != 0) {
        *outDurationMs = fi->frameDuration;
    } else {
        *outDurationMs = ctx->header.defaultFrameDuration;
    }

    return ZEL_OK;
}

ZELResult zelGetFrameIsKeyframe(const ZELContext *ctx, uint32_t frameIndex, int *outIsKeyframe) {
    if (!ctx || !outIsKeyframe)
        return ZEL_ERR_INVALID_ARGUMENT;

    if (frameIndex >= ctx->header.frameCount)
        return ZEL_ERR_OUT_OF_BOUNDS;

    const ZELFrameIndexEntry *fi = &ctx->frameIndexTable[frameIndex];
    *outIsKeyframe = fi->flags.keyframe ? 1 : 0;

    return ZEL_OK;
}

ZELResult zelGetFrameUsesLocalPalette(const ZELContext *ctx,
                                      uint32_t frameIndex,
                                      int *outUsesLocalPalette) {
    if (!ctx || !outUsesLocalPalette)
        return ZEL_ERR_INVALID_ARGUMENT;

    if (frameIndex >= ctx->header.frameCount)
        return ZEL_ERR_OUT_OF_BOUNDS;

    const ZELFrameIndexEntry *fi = &ctx->frameIndexTable[frameIndex];
    *outUsesLocalPalette = fi->flags.hasLocalPalette ? 1 : 0;

    return ZEL_OK;
}

ZELResult zelDecodeFrameIndex8(const ZELContext *ctx,
                               uint32_t frameIndex,
                               uint8_t *dst,
                               size_t dstStrideBytes) {
    if (!ctx || !dst)
        return ZEL_ERR_INVALID_ARGUMENT;

    if (frameIndex >= ctx->header.frameCount)
        return ZEL_ERR_OUT_OF_BOUNDS;

    if (ctx->header.colorFormat != ZEL_COLOR_FORMAT_INDEXED8)
        return ZEL_ERR_UNSUPPORTED_FORMAT;

    uint16_t width = ctx->header.width;
    if (dstStrideBytes < width)
        return ZEL_ERR_INVALID_ARGUMENT;

    ZELFrameZoneStream stream;
    ZELResult result = zelInitFrameZoneStream(ctx, frameIndex, &stream);
    if (result != ZEL_OK)
        return result;

    uint8_t *scratch = NULL;
    if (stream.header->compressionType == ZEL_COMPRESSION_LZ4) {
        scratch = zelAcquireZoneScratch(ctx, stream.layout.zonePixelBytes);
        if (!scratch)
            return ZEL_ERR_OUT_OF_MEMORY;
    }

    size_t cursor = stream.zoneDataOffset;
    for (uint32_t zoneIndex = 0; zoneIndex < stream.layout.zoneCount; ++zoneIndex) {
        const uint8_t *chunkData = NULL;
        uint32_t chunkSize = 0;
        result = zelReadZoneChunkAtCursor(ctx, &stream, &cursor, &chunkData, &chunkSize);
        if (result != ZEL_OK)
            break;

        const uint8_t *zonePixels = NULL;
        result = zelAccessZonePixels(ctx, &stream, chunkData, chunkSize, scratch, &zonePixels);
        if (result != ZEL_OK)
            break;

        zelBlitZoneIndices(&stream.layout, zoneIndex, zonePixels, dst, dstStrideBytes);
    }

    if (result == ZEL_OK && cursor != stream.frameDataEnd)
        result = ZEL_ERR_CORRUPT_DATA;

    return result;
}

ZELResult zelDecodeFrameIndex8Zone(const ZELContext *ctx,
                                   uint32_t frameIndex,
                                   uint32_t zoneIndex,
                                   uint8_t *dst) {
    if (!ctx || !dst)
        return ZEL_ERR_INVALID_ARGUMENT;

    if (ctx->header.colorFormat != ZEL_COLOR_FORMAT_INDEXED8)
        return ZEL_ERR_UNSUPPORTED_FORMAT;

    ZELFrameZoneStream stream;
    ZELResult result = zelInitFrameZoneStream(ctx, frameIndex, &stream);
    if (result != ZEL_OK)
        return result;

    if (zoneIndex >= stream.layout.zoneCount)
        return ZEL_ERR_OUT_OF_BOUNDS;

    uint8_t *scratch = NULL;
    if (stream.header->compressionType == ZEL_COMPRESSION_LZ4) {
        scratch = zelAcquireZoneScratch(ctx, stream.layout.zonePixelBytes);
        if (!scratch)
            return ZEL_ERR_OUT_OF_MEMORY;
    }

    const uint8_t *chunkData = NULL;
    uint32_t chunkSize = 0;
    result = zelLocateZoneChunk(ctx, &stream, zoneIndex, &chunkData, &chunkSize);
    if (result == ZEL_OK) {
        const uint8_t *zonePixels = NULL;
        result = zelAccessZonePixels(ctx, &stream, chunkData, chunkSize, scratch, &zonePixels);
        if (result == ZEL_OK)
            zelBlitZoneIndices(&stream.layout,
                               0,
                               zonePixels,
                               dst,
                               stream.layout.zoneWidth); /* zoneIndex=0 writes a contiguous tile */
    }

    return result;
}

ZELResult zelDecodeFrameRgb565(const ZELContext *ctx,
                               uint32_t frameIndex,
                               uint16_t *dst,
                               size_t dstStridePixels) {
    if (!ctx || !dst)
        return ZEL_ERR_INVALID_ARGUMENT;

    if (ctx->header.colorFormat != ZEL_COLOR_FORMAT_INDEXED8)
        return ZEL_ERR_UNSUPPORTED_FORMAT;

    uint16_t width = ctx->header.width;
    if (dstStridePixels < width)
        return ZEL_ERR_INVALID_ARGUMENT;

    const uint16_t *palette = NULL;
    uint16_t paletteCount = 0;
    ZELResult result = zelGetFramePalette(ctx, frameIndex, &palette, &paletteCount);
    if (result != ZEL_OK)
        return result;

    ZELFrameZoneStream stream;
    result = zelInitFrameZoneStream(ctx, frameIndex, &stream);
    if (result != ZEL_OK)
        return result;

    uint8_t *scratch = NULL;
    if (stream.header->compressionType == ZEL_COMPRESSION_LZ4) {
        scratch = zelAcquireZoneScratch(ctx, stream.layout.zonePixelBytes);
        if (!scratch)
            return ZEL_ERR_OUT_OF_MEMORY;
    }

    size_t cursor = stream.zoneDataOffset;
    for (uint32_t zoneIndex = 0; zoneIndex < stream.layout.zoneCount; ++zoneIndex) {
        const uint8_t *chunkData = NULL;
        uint32_t chunkSize = 0;
        result = zelReadZoneChunkAtCursor(ctx, &stream, &cursor, &chunkData, &chunkSize);
        if (result != ZEL_OK)
            break;

        const uint8_t *zonePixels = NULL;
        result = zelAccessZonePixels(ctx, &stream, chunkData, chunkSize, scratch, &zonePixels);
        if (result != ZEL_OK)
            break;

        result = zelBlitZoneRgb(&stream.layout,
                                zoneIndex,
                                zonePixels,
                                palette,
                                paletteCount,
                                dst,
                                dstStridePixels);
        if (result != ZEL_OK)
            break;
    }

    if (result == ZEL_OK && cursor != stream.frameDataEnd)
        result = ZEL_ERR_CORRUPT_DATA;

    return result;
}

ZELResult zelDecodeFrameRgb565Zone(const ZELContext *ctx,
                                   uint32_t frameIndex,
                                   uint32_t zoneIndex,
                                   uint16_t *dst) {
    if (!ctx || !dst)
        return ZEL_ERR_INVALID_ARGUMENT;

    if (ctx->header.colorFormat != ZEL_COLOR_FORMAT_INDEXED8)
        return ZEL_ERR_UNSUPPORTED_FORMAT;

    const uint16_t *palette = NULL;
    uint16_t paletteCount = 0;
    ZELResult result = zelGetFramePalette(ctx, frameIndex, &palette, &paletteCount);
    if (result != ZEL_OK)
        return result;

    ZELFrameZoneStream stream;
    result = zelInitFrameZoneStream(ctx, frameIndex, &stream);
    if (result != ZEL_OK)
        return result;

    if (zoneIndex >= stream.layout.zoneCount)
        return ZEL_ERR_OUT_OF_BOUNDS;

    uint8_t *scratch = NULL;
    if (stream.header->compressionType == ZEL_COMPRESSION_LZ4) {
        scratch = zelAcquireZoneScratch(ctx, stream.layout.zonePixelBytes);
        if (!scratch)
            return ZEL_ERR_OUT_OF_MEMORY;
    }

    const uint8_t *chunkData = NULL;
    uint32_t chunkSize = 0;
    result = zelLocateZoneChunk(ctx, &stream, zoneIndex, &chunkData, &chunkSize);
    if (result == ZEL_OK) {
        const uint8_t *zonePixels = NULL;
        result = zelAccessZonePixels(ctx, &stream, chunkData, chunkSize, scratch, &zonePixels);
        if (result == ZEL_OK)
            result = zelBlitZoneRgb(&stream.layout,
                                    0,
                                    zonePixels,
                                    palette,
                                    paletteCount,
                                    dst,
                                    stream.layout.zoneWidth); /* contiguous tile buffer */
    }

    return result;
}

ZELResult zelGetTotalDurationMs(const ZELContext *ctx, uint32_t *outTotalDurationMs) {
    if (!ctx || !outTotalDurationMs) {
        return ZEL_ERR_INVALID_ARGUMENT;
    }

    uint32_t total = 0;
    uint32_t frameCount = ctx->header.frameCount;

    for (uint32_t i = 0; i < frameCount; ++i) {
        uint16_t duration = 0;
        ZELResult r = zelGetFrameDurationMs(ctx, i, &duration);
        if (r != ZEL_OK) {
            return r;
        }
        total += (uint32_t)duration;
    }

    *outTotalDurationMs = total;
    return ZEL_OK;
}

ZELResult zelFindFrameByTimeMs(const ZELContext *ctx,
                               uint32_t timeMs,
                               uint32_t *outFrameIndex,
                               uint32_t *outFrameStartMs) {
    if (!ctx || !outFrameIndex || !outFrameStartMs) {
        return ZEL_ERR_INVALID_ARGUMENT;
    }

    uint32_t totalDuration = 0;
    ZELResult r = zelGetTotalDurationMs(ctx, &totalDuration);
    if (r != ZEL_OK) {
        return r;
    }

    if (totalDuration == 0) {
        return ZEL_ERR_CORRUPT_DATA;
    }

    uint32_t t = timeMs % totalDuration;

    uint32_t frameCount = ctx->header.frameCount;
    uint32_t accum = 0;

    for (uint32_t i = 0; i < frameCount; ++i) {
        uint16_t duration = 0;
        r = zelGetFrameDurationMs(ctx, i, &duration);
        if (r != ZEL_OK) {
            return r;
        }

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
