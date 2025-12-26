#include "zel_internal.h"

#include <stdlib.h>
#include <string.h>

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

    zelConvertPaletteEncoding(paletteData, scratch, ph->entryCount, sourceEncoding, desired);

    *outEntries = scratch;
    *outCount = ph->entryCount;
    return ZEL_OK;
}

ZELResult zelGetGlobalPalette(const ZELContext *ctx,
                              const uint16_t **outEntries,
                              uint16_t *outCount) {
    if (!ctx || !outEntries || !outCount)
        return ZEL_ERR_INVALID_ARGUMENT;

    return zelResolveGlobalPalette(ctx, outEntries, outCount);
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

    size_t frameOffset = fi->frameOffset;
    size_t frameSize = fi->frameSize;

    if (frameSize == 0)
        return ZEL_ERR_CORRUPT_DATA;

    if (!zelRangeFits(frameOffset, frameSize, ctx->size))
        return ZEL_ERR_CORRUPT_DATA;

    size_t frameEnd = frameOffset + frameSize;

    if (!zelRangeFits(frameOffset, sizeof(ZELFrameHeader), ctx->size))
        return ZEL_ERR_CORRUPT_DATA;

    ZELFrameHeader fh;
    ZELResult result = zelReadAt(ctx, frameOffset, &fh, sizeof(ZELFrameHeader));
    if (result != ZEL_OK)
        return result;

    if (fh.localPaletteEntryCount == 0)
        return ZEL_ERR_CORRUPT_DATA;

    size_t phOffset = frameOffset + fh.headerSize;
    if (phOffset > frameEnd || !zelRangeFits(phOffset, sizeof(ZELPaletteHeader), ctx->size)
        || sizeof(ZELPaletteHeader) > frameEnd - phOffset) {
        return ZEL_ERR_CORRUPT_DATA;
    }

    ZELPaletteHeader ph;
    result = zelReadAt(ctx, phOffset, &ph, sizeof(ZELPaletteHeader));
    if (result != ZEL_OK)
        return result;

    if (ph.headerSize < sizeof(ZELPaletteHeader))
        return ZEL_ERR_CORRUPT_DATA;

    if (!zelIsValidColorEncoding(ph.colorEncoding))
        return ZEL_ERR_UNSUPPORTED_FORMAT;

    if (ph.entryCount == 0)
        return ZEL_ERR_CORRUPT_DATA;

    size_t paletteDataOffset = phOffset + ph.headerSize;
    size_t paletteBytes = (size_t)ph.entryCount * sizeof(uint16_t);

    if (!zelRangeFits(paletteDataOffset, paletteBytes, ctx->size))
        return ZEL_ERR_CORRUPT_DATA;
    if (paletteDataOffset > frameEnd || paletteBytes > frameEnd - paletteDataOffset)
        return ZEL_ERR_CORRUPT_DATA;

    const uint16_t *paletteData = NULL;
    if (ctx->data) {
        paletteData = (const uint16_t *)(ctx->data + paletteDataOffset);
    } else {
        uint16_t *scratch = zelAcquirePaletteScratch(ctx, ph.entryCount);
        if (!scratch)
            return ZEL_ERR_OUT_OF_MEMORY;
        result = zelReadAt(ctx, paletteDataOffset, scratch, paletteBytes);
        if (result != ZEL_OK)
            return result;
        paletteData = scratch;
    }

    return zelResolveLocalPalette(ctx, &ph, paletteData, outEntries, outCount);
}
