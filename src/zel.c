#include "zel/zel.h"

#include "lz4/lz4.h"
#include <stdlib.h>
#include <string.h>

struct ZELContext {
    const uint8_t *data;
    size_t size;

    ZELFileHeader header;

    const ZELFrameIndexEntry *frameIndexTable;
    const uint16_t *globalPalette;
    uint16_t globalPaletteCount;
};

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

    if (h->tileWidth == 0 || h->tileHeight == 0) {
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

        if (ph.colorEncoding != ZEL_COLOR_RGB565) {
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

        ctx->globalPalette = (const uint16_t *)(data + paletteDataOffset);
        ctx->globalPaletteCount = ph.entryCount;

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
        free(ctx);
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
    free(ctx);
}

uint16_t zelGetWidth(const ZELContext *ctx) { return ctx ? ctx->header.width : 0; }

uint16_t zelGetHeight(const ZELContext *ctx) { return ctx ? ctx->header.height : 0; }

uint32_t zelGetFrameCount(const ZELContext *ctx) { return ctx ? ctx->header.frameCount : 0; }

uint16_t zelGetDefaultFrameDurationMs(const ZELContext *ctx) {
    return ctx ? ctx->header.defaultFrameDuration : 0;
}

int zelHasGlobalPalette(const ZELContext *ctx) {
    return (ctx && ctx->globalPalette && ctx->globalPaletteCount > 0);
}

ZELResult zelGetGlobalPalette(const ZELContext *ctx, const uint16_t **outEntries,
                              uint16_t *outCount) {
    if (!ctx || !outEntries || !outCount)
        return ZEL_ERR_INVALID_ARGUMENT;

    if (!ctx->globalPalette)
        return ZEL_ERR_OUT_OF_BOUNDS;

    *outEntries = ctx->globalPalette;
    *outCount = ctx->globalPaletteCount;

    return ZEL_OK;
}

ZELResult zelGetFramePalette(const ZELContext *ctx, uint32_t frameIndex,
                             const uint16_t **outEntries, uint16_t *outCount) {
    if (!ctx || !outEntries || !outCount)
        return ZEL_ERR_INVALID_ARGUMENT;

    if (frameIndex >= ctx->header.frameCount)
        return ZEL_ERR_OUT_OF_BOUNDS;

    const ZELFrameIndexEntry *fi = &ctx->frameIndexTable[frameIndex];

    if (!fi->flags.hasLocalPalette) {
        if (ctx->globalPalette) {
            *outEntries = ctx->globalPalette;
            *outCount = ctx->globalPaletteCount;
            return ZEL_OK;
        } else {
            return ZEL_ERR_OUT_OF_BOUNDS;
        }
    }

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

    if (ph->colorEncoding != ZEL_COLOR_RGB565)
        return ZEL_ERR_UNSUPPORTED_FORMAT;

    size_t paletteDataOffset = phOffset + ph->headerSize;
    size_t paletteBytes = ph->entryCount * sizeof(uint16_t);

    if (paletteDataOffset + paletteBytes > ctx->size)
        return ZEL_ERR_CORRUPT_DATA;

    *outEntries = (const uint16_t *)(ctx->data + paletteDataOffset);
    *outCount = ph->entryCount;

    return ZEL_OK;
}

ZELResult zelGetFrameDurationMs(const ZELContext *ctx, uint32_t frameIndex,
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

ZELResult zelGetFrameUsesLocalPalette(const ZELContext *ctx, uint32_t frameIndex,
                                      int *outUsesLocalPalette) {
    if (!ctx || !outUsesLocalPalette)
        return ZEL_ERR_INVALID_ARGUMENT;

    if (frameIndex >= ctx->header.frameCount)
        return ZEL_ERR_OUT_OF_BOUNDS;

    const ZELFrameIndexEntry *fi = &ctx->frameIndexTable[frameIndex];
    *outUsesLocalPalette = fi->flags.hasLocalPalette ? 1 : 0;

    return ZEL_OK;
}

ZELResult zelDecodeFrameIndex8(const ZELContext *ctx, uint32_t frameIndex, uint8_t *dst,
                               size_t dstStrideBytes) {
    if (!ctx || !dst) {
        return ZEL_ERR_INVALID_ARGUMENT;
    }

    if (frameIndex >= ctx->header.frameCount) {
        return ZEL_ERR_OUT_OF_BOUNDS;
    }

    if (ctx->header.colorFormat != ZEL_COLOR_FORMAT_INDEXED8) {
        return ZEL_ERR_UNSUPPORTED_FORMAT;
    }

    uint16_t width = ctx->header.width;
    uint16_t height = ctx->header.height;

    if (dstStrideBytes < width) {
        return ZEL_ERR_INVALID_ARGUMENT;
    }

    const ZELFrameIndexEntry *fi = &ctx->frameIndexTable[frameIndex];
    size_t frameOffset = fi->frameOffset;
    size_t frameSize = fi->frameSize;

    if (frameOffset + sizeof(ZELFrameHeader) > ctx->size) {
        return ZEL_ERR_CORRUPT_DATA;
    }
    if (frameOffset + frameSize > ctx->size) {
        return ZEL_ERR_CORRUPT_DATA;
    }
    if (frameSize == 0) {
        return ZEL_ERR_CORRUPT_DATA;
    }

    const ZELFrameHeader *fh = (const ZELFrameHeader *)(ctx->data + frameOffset);
    size_t offset = frameOffset + fh->headerSize;

    if (offset > frameOffset + frameSize) {
        return ZEL_ERR_CORRUPT_DATA;
    }

    if (fh->flags.hasLocalPalette) {
        if (offset + sizeof(ZELPaletteHeader) > ctx->size ||
            offset + sizeof(ZELPaletteHeader) > frameOffset + frameSize) {
            return ZEL_ERR_CORRUPT_DATA;
        }

        const ZELPaletteHeader *ph = (const ZELPaletteHeader *)(ctx->data + offset);

        if (ph->headerSize < sizeof(ZELPaletteHeader)) {
            return ZEL_ERR_CORRUPT_DATA;
        }

        size_t paletteDataOffset = offset + ph->headerSize;
        size_t paletteBytes = (size_t)ph->entryCount * sizeof(uint16_t);

        if (paletteDataOffset + paletteBytes > ctx->size ||
            paletteDataOffset + paletteBytes > frameOffset + frameSize) {
            return ZEL_ERR_CORRUPT_DATA;
        }

        offset = paletteDataOffset + paletteBytes;
    }

    size_t pixelBytes = (size_t)width * (size_t)height;
    if (pixelBytes == 0) {
        return ZEL_ERR_CORRUPT_DATA;
    }

    /* Check if pixelBytes fits in int for LZ4 API */
    if (pixelBytes > (size_t)INT32_MAX) {
        return ZEL_ERR_UNSUPPORTED_FORMAT;
    }

    if (fh->compressionType == ZEL_COMPRESSION_NONE) {
        /* Handle uncompressed indices */
        if (offset + pixelBytes > ctx->size || offset + pixelBytes > frameOffset + frameSize) {
            return ZEL_ERR_CORRUPT_DATA;
        }

        const uint8_t *src = ctx->data + offset;

        for (uint16_t y = 0; y < height; ++y) {
            const uint8_t *srcRow = src + (size_t)y * width;
            uint8_t *dstRow = dst + (size_t)y * dstStrideBytes;
            memcpy(dstRow, srcRow, width);
        }

        return ZEL_OK;
    } else if (fh->compressionType == ZEL_COMPRESSION_LZ4) {
        /* LZ4 compressed indices */
        if (offset > frameOffset + frameSize) {
            return ZEL_ERR_CORRUPT_DATA;
        }

        size_t compressedSize = (frameOffset + frameSize) - offset;
        if (compressedSize == 0) {
            return ZEL_ERR_CORRUPT_DATA;
        }

        const char *src = (const char *)(ctx->data + offset);
        int srcSize = (int)compressedSize;
        int dstCapacity = (int)pixelBytes;

        int decodedBytes = 0;

        if (dstStrideBytes == (size_t)width) {
            /* Decompress directly into the destination buffer */
            decodedBytes = LZ4_decompress_safe(src, (char *)dst, srcSize, dstCapacity);
            if (decodedBytes < 0 || (size_t)decodedBytes != pixelBytes) {
                return ZEL_ERR_CORRUPT_DATA;
            }
        } else {
            /* Decompress into a temporary, tightly packed buffer, then copy row by row into dst */
            uint8_t *tmp = (uint8_t *)malloc(pixelBytes);
            if (!tmp) {
                return ZEL_ERR_OUT_OF_MEMORY;
            }

            decodedBytes = LZ4_decompress_safe(src, (char *)tmp, srcSize, dstCapacity);
            if (decodedBytes < 0 || (size_t)decodedBytes != pixelBytes) {
                free(tmp);
                return ZEL_ERR_CORRUPT_DATA;
            }

            for (uint16_t y = 0; y < height; ++y) {
                const uint8_t *srcRow = tmp + (size_t)y * width;
                uint8_t *dstRow = dst + (size_t)y * dstStrideBytes;
                memcpy(dstRow, srcRow, width);
            }

            free(tmp);
        }

        return ZEL_OK;
    } else {
        /* TODO: Add other compression types (e.g., RLE) */
        return ZEL_ERR_UNSUPPORTED_FORMAT;
    }
}

ZELResult zelDecodeFrameRgb565(const ZELContext *ctx, uint32_t frameIndex, uint16_t *dst,
                               size_t dstStridePixels) {
    if (!ctx || !dst) {
        return ZEL_ERR_INVALID_ARGUMENT;
    }

    if (frameIndex >= ctx->header.frameCount) {
        return ZEL_ERR_OUT_OF_BOUNDS;
    }

    uint16_t width = ctx->header.width;
    uint16_t height = ctx->header.height;

    if (dstStridePixels < width) {
        return ZEL_ERR_INVALID_ARGUMENT;
    }

    const uint16_t *palette = NULL;
    uint16_t paletteCount = 0;
    ZELResult r = zelGetFramePalette(ctx, frameIndex, &palette, &paletteCount);
    if (r != ZEL_OK) {
        return r;
    }

    const ZELFrameIndexEntry *fi = &ctx->frameIndexTable[frameIndex];
    size_t frameOffset = fi->frameOffset;
    size_t frameSize = fi->frameSize;

    if (frameOffset + sizeof(ZELFrameHeader) > ctx->size) {
        return ZEL_ERR_CORRUPT_DATA;
    }
    if (frameOffset + frameSize > ctx->size) {
        return ZEL_ERR_CORRUPT_DATA;
    }
    if (frameSize == 0) {
        return ZEL_ERR_CORRUPT_DATA;
    }

    const ZELFrameHeader *fh = (const ZELFrameHeader *)(ctx->data + frameOffset);
    size_t offset = frameOffset + fh->headerSize;

    if (offset > frameOffset + frameSize) {
        return ZEL_ERR_CORRUPT_DATA;
    }

    if (fh->flags.hasLocalPalette) {
        if (offset + sizeof(ZELPaletteHeader) > ctx->size ||
            offset + sizeof(ZELPaletteHeader) > frameOffset + frameSize) {
            return ZEL_ERR_CORRUPT_DATA;
        }

        const ZELPaletteHeader *ph = (const ZELPaletteHeader *)(ctx->data + offset);

        if (ph->headerSize < sizeof(ZELPaletteHeader)) {
            return ZEL_ERR_CORRUPT_DATA;
        }

        size_t paletteDataOffset = offset + ph->headerSize;
        size_t paletteBytes = (size_t)ph->entryCount * sizeof(uint16_t);

        if (paletteDataOffset + paletteBytes > ctx->size ||
            paletteDataOffset + paletteBytes > frameOffset + frameSize) {
            return ZEL_ERR_CORRUPT_DATA;
        }

        offset = paletteDataOffset + paletteBytes;
    }

    size_t pixelBytes = (size_t)width * (size_t)height;
    if (pixelBytes == 0) {
        return ZEL_ERR_CORRUPT_DATA;
    }

    /* Check if pixelBytes fits in int for LZ4 API */
    if (pixelBytes > (size_t)INT32_MAX) {
        return ZEL_ERR_UNSUPPORTED_FORMAT;
    }

    if (fh->compressionType == ZEL_COMPRESSION_NONE) {
        /* Handle uncompressed indices */
        if (offset + pixelBytes > ctx->size || offset + pixelBytes > frameOffset + frameSize) {
            return ZEL_ERR_CORRUPT_DATA;
        }

        const uint8_t *src = ctx->data + offset;

        for (uint16_t y = 0; y < height; ++y) {
            const uint8_t *srcRow = src + (size_t)y * width;
            uint16_t *dstRow = dst + (size_t)y * dstStridePixels;

            for (uint16_t x = 0; x < width; ++x) {
                uint8_t idx = srcRow[x];
                if (idx >= paletteCount) {
                    return ZEL_ERR_CORRUPT_DATA;
                }
                dstRow[x] = palette[idx];
            }
        }

        return ZEL_OK;
    } else if (fh->compressionType == ZEL_COMPRESSION_LZ4) {
        /* LZ4 compressed indices */
        if (offset > frameOffset + frameSize) {
            return ZEL_ERR_CORRUPT_DATA;
        }

        size_t compressedSize = (frameOffset + frameSize) - offset;
        if (compressedSize == 0) {
            return ZEL_ERR_CORRUPT_DATA;
        }

        const char *src = (const char *)(ctx->data + offset);
        int srcSize = (int)compressedSize;
        int dstCapacity = (int)pixelBytes;

        /* Decompress into a temporary, tightly packed buffer */
        uint8_t *tmp = (uint8_t *)malloc(pixelBytes);
        if (!tmp) {
            return ZEL_ERR_OUT_OF_MEMORY;
        }

        int decodedBytes = LZ4_decompress_safe(src, (char *)tmp, srcSize, dstCapacity);
        if (decodedBytes < 0 || (size_t)decodedBytes != pixelBytes) {
            free(tmp);
            return ZEL_ERR_CORRUPT_DATA;
        }

        /* Map palette indices to RGB565 */
        for (uint16_t y = 0; y < height; ++y) {
            const uint8_t *srcRow = tmp + (size_t)y * width;
            uint16_t *dstRow = dst + (size_t)y * dstStridePixels;

            for (uint16_t x = 0; x < width; ++x) {
                uint8_t idx = srcRow[x];
                if (idx >= paletteCount) {
                    free(tmp);
                    return ZEL_ERR_CORRUPT_DATA;
                }
                dstRow[x] = palette[idx];
            }
        }

        free(tmp);
        return ZEL_OK;
    } else {
        /* TODO: Add other compression types (e.g., RLE) */
        return ZEL_ERR_UNSUPPORTED_FORMAT;
    }
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

ZELResult zelFindFrameByTimeMs(const ZELContext *ctx, uint32_t timeMs, uint32_t *outFrameIndex,
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
