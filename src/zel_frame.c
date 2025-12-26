#include "lz4/lz4.h"
#include "zel_internal.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

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

    if (frameSize == 0)
        return ZEL_ERR_CORRUPT_DATA;

    if (!zelRangeFits(frameOffset, sizeof(ZELFrameHeader), ctx->size)
        || !zelRangeFits(frameOffset, frameSize, ctx->size)) {
        return ZEL_ERR_CORRUPT_DATA;
    }

    const uint8_t *frameBytes = NULL;
    if (ctx->data) {
        frameBytes = ctx->data + frameOffset;
    } else {
        ZELContext *mutableCtx = (ZELContext *)ctx;
        if (mutableCtx->frameDataScratchCapacity < frameSize) {
            uint8_t *newBuf = (uint8_t *)realloc(mutableCtx->frameDataScratch, frameSize);
            if (!newBuf)
                return ZEL_ERR_OUT_OF_MEMORY;
            mutableCtx->frameDataScratch = newBuf;
            mutableCtx->frameDataScratchCapacity = frameSize;
        }

        ZELResult result = zelReadAt(ctx, frameOffset, mutableCtx->frameDataScratch, frameSize);
        if (result != ZEL_OK)
            return result;

        frameBytes = mutableCtx->frameDataScratch;
    }

    if (frameSize < sizeof(ZELFrameHeader))
        return ZEL_ERR_CORRUPT_DATA;

    ZELFrameHeader fh;
    memcpy(&fh, frameBytes, sizeof(ZELFrameHeader));

    if (fh.headerSize < sizeof(ZELFrameHeader) || fh.headerSize > frameSize)
        return ZEL_ERR_CORRUPT_DATA;

    size_t relOffset = fh.headerSize;

    if (fh.flags.hasLocalPalette) {
        if (frameSize - relOffset < sizeof(ZELPaletteHeader))
            return ZEL_ERR_CORRUPT_DATA;

        const ZELPaletteHeader *ph = (const ZELPaletteHeader *)(frameBytes + relOffset);
        if (ph->headerSize < sizeof(ZELPaletteHeader) || ph->entryCount == 0)
            return ZEL_ERR_CORRUPT_DATA;

        if (ph->headerSize > frameSize - relOffset)
            return ZEL_ERR_CORRUPT_DATA;

        size_t paletteDataRel = relOffset + ph->headerSize;
        size_t paletteBytes = (size_t)ph->entryCount * sizeof(uint16_t);

        if (paletteBytes > frameSize - paletteDataRel)
            return ZEL_ERR_CORRUPT_DATA;

        relOffset = paletteDataRel + paletteBytes;
    }

    if (relOffset > frameSize)
        return ZEL_ERR_CORRUPT_DATA;

    size_t frameEnd = frameOffset + frameSize;
    size_t offset = frameOffset + relOffset;

    ZELZoneLayout layout;
    ZELResult zr = zelComputeZoneLayout(ctx, &layout);
    if (zr != ZEL_OK)
        return zr;

    if (layout.zoneCount == 0 || fh.zoneCount != (uint16_t)layout.zoneCount)
        return ZEL_ERR_CORRUPT_DATA;

    outStream->header = fh;
    outStream->frameOffset = frameOffset;
    outStream->frameSize = frameSize;
    outStream->zoneDataOffset = offset;
    outStream->frameDataEnd = frameEnd;
    outStream->layout = layout;
    outStream->frameData = frameBytes;
    return ZEL_OK;
}

static ZELResult zelReadZoneChunkAtCursor(const ZELContext *ctx,
                                          const ZELFrameZoneStream *stream,
                                          size_t *cursor,
                                          const uint8_t **outData,
                                          uint32_t *outSize) {
    if (!ctx || !stream || !cursor || !outData || !outSize)
        return ZEL_ERR_INVALID_ARGUMENT;

    if (!stream->frameData)
        return ZEL_ERR_INTERNAL;

    if (*cursor < stream->frameOffset || *cursor > stream->frameDataEnd)
        return ZEL_ERR_CORRUPT_DATA;

    size_t relOffset = *cursor - stream->frameOffset;
    size_t frameBytesRemaining = stream->frameSize - relOffset;
    if (frameBytesRemaining < sizeof(uint32_t))
        return ZEL_ERR_CORRUPT_DATA;

    const uint8_t *frameBytes = stream->frameData;
    uint32_t chunkSize = 0;
    memcpy(&chunkSize, frameBytes + relOffset, sizeof(uint32_t));

    relOffset += sizeof(uint32_t);
    *cursor += sizeof(uint32_t);

    if (chunkSize == 0)
        return ZEL_ERR_CORRUPT_DATA;

    if (relOffset > stream->frameSize || (size_t)chunkSize > stream->frameSize - relOffset)
        return ZEL_ERR_CORRUPT_DATA;

    const uint8_t *chunkData = frameBytes + relOffset;
    *cursor += chunkSize;
    *outData = chunkData;
    *outSize = chunkSize;
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

    switch (stream->header.compressionType) {
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
    if (stream.header.compressionType == ZEL_COMPRESSION_LZ4) {
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
    if (stream.header.compressionType == ZEL_COMPRESSION_LZ4) {
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
            zelBlitZoneIndices(&stream.layout, 0, zonePixels, dst, stream.layout.zoneWidth);
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
    if (stream.header.compressionType == ZEL_COMPRESSION_LZ4) {
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
    if (stream.header.compressionType == ZEL_COMPRESSION_LZ4) {
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
                                    stream.layout.zoneWidth);
    }

    return result;
}
