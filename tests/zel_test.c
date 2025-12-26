#include "fixtures/simple_zel_file.h"
#include "zel/zel.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint16_t swap_u16(uint16_t v) {
    return (uint16_t)(((v & 0x00FFu) << 8) | ((v & 0xFF00u) >> 8));
}

static void write_le16(uint8_t *dst, uint16_t v) {
    dst[0] = (uint8_t)(v & 0xFFu);
    dst[1] = (uint8_t)(v >> 8);
}

static void write_le32(uint8_t *dst, uint32_t v) {
    dst[0] = (uint8_t)(v & 0xFFu);
    dst[1] = (uint8_t)((v >> 8) & 0xFFu);
    dst[2] = (uint8_t)((v >> 16) & 0xFFu);
    dst[3] = (uint8_t)((v >> 24) & 0xFFu);
}

typedef struct {
    const uint8_t *data;
    size_t size;
} TestMemoryStream;

static size_t test_memory_stream_read(void *userData, size_t offset, void *dst, size_t size) {
    TestMemoryStream *stream = (TestMemoryStream *)userData;
    if (!stream || !dst)
        return 0;
    if (offset > stream->size)
        return 0;
    if (size > stream->size - offset)
        return 0;
    memcpy(dst, stream->data + offset, size);
    return size;
}

static const uint8_t kSimpleFramePattern[8] = {0, 1, 0, 1, 1, 0, 1, 0};

static void build_expected_rgb_frame(uint16_t *dst, const uint16_t palette[2]) {
    for (size_t i = 0; i < 8; ++i)
        dst[i] = palette[kSimpleFramePattern[i]];
}

static void write_zone_chunks(uint8_t *dst,
                              size_t *offset,
                              const uint8_t *pixels,
                              uint16_t width,
                              uint16_t height,
                              uint16_t zoneWidth,
                              uint16_t zoneHeight,
                              uint8_t *scratch) {
    const uint32_t zonesPerRow = width / zoneWidth;
    const uint32_t zonesPerCol = height / zoneHeight;
    const uint32_t zoneCount = zonesPerRow * zonesPerCol;
    const size_t zoneBytes = (size_t)zoneWidth * zoneHeight;

    for (uint32_t zoneIndex = 0; zoneIndex < zoneCount; ++zoneIndex) {
        const uint32_t zoneX = (zoneIndex % zonesPerRow) * zoneWidth;
        const uint32_t zoneY = (zoneIndex / zonesPerRow) * zoneHeight;

        for (uint16_t row = 0; row < zoneHeight; ++row) {
            const uint8_t *srcRow = pixels + (size_t)(zoneY + row) * width + zoneX;
            memcpy(scratch + (size_t)row * zoneWidth, srcRow, zoneWidth);
        }

        uint32_t chunkSize = (uint32_t)zoneBytes;
        memcpy(dst + *offset, &chunkSize, sizeof(chunkSize));
        *offset += sizeof(chunkSize);
        memcpy(dst + *offset, scratch, zoneBytes);
        *offset += zoneBytes;
    }
}

static void blit_indices_zone_to_frame(uint32_t zoneIndex,
                                       uint16_t frameWidth,
                                       uint16_t zoneWidth,
                                       uint16_t zoneHeight,
                                       uint8_t *frameDst,
                                       const uint8_t *zonePixels) {
    const uint32_t zonesPerRow = frameWidth / zoneWidth;
    const uint32_t zoneX = (zoneIndex % zonesPerRow) * zoneWidth;
    const uint32_t zoneY = (zoneIndex / zonesPerRow) * zoneHeight;

    for (uint16_t row = 0; row < zoneHeight; ++row) {
        uint8_t *dstRow = frameDst + (size_t)(zoneY + row) * frameWidth + zoneX;
        const uint8_t *srcRow = zonePixels + (size_t)row * zoneWidth;
        memcpy(dstRow, srcRow, zoneWidth);
    }
}

static void blit_rgb_zone_to_frame(uint32_t zoneIndex,
                                   uint16_t frameWidth,
                                   uint16_t zoneWidth,
                                   uint16_t zoneHeight,
                                   uint16_t *frameDst,
                                   const uint16_t *zonePixels) {
    const uint32_t zonesPerRow = frameWidth / zoneWidth;
    const uint32_t zoneX = (zoneIndex % zonesPerRow) * zoneWidth;
    const uint32_t zoneY = (zoneIndex / zonesPerRow) * zoneHeight;

    for (uint16_t row = 0; row < zoneHeight; ++row) {
        uint16_t *dstRow = frameDst + (size_t)(zoneY + row) * frameWidth + zoneX;
        const uint16_t *srcRow = zonePixels + (size_t)row * zoneWidth;
        memcpy(dstRow, srcRow, (size_t)zoneWidth * sizeof(uint16_t));
    }
}

/* Simple helper function: builds a simple ZEL file in memory with configurable zone size:
    - 4x2 pixels
    - 1 frame
    - global palette with 2 entries (RGB565)
    - no compression
    Pixel pattern: 0,1,0,1 / 1,0,1,0
*/
static void write_palette_bytes(uint8_t *dst,
                                const uint16_t *palette,
                                uint16_t count,
                                ZELColorEncoding encoding) {
    for (uint16_t i = 0; i < count; ++i) {
        uint16_t value = palette[i];
        if (encoding == ZEL_COLOR_RGB565_BE) {
            dst[2 * i] = (uint8_t)(value >> 8);
            dst[2 * i + 1] = (uint8_t)(value & 0xFF);
        } else {
            dst[2 * i] = (uint8_t)(value & 0xFF);
            dst[2 * i + 1] = (uint8_t)(value >> 8);
        }
    }
}

static uint8_t *buildSimpleZelSingleFrameWithZonesCustom(uint16_t zoneWidth,
                                                         uint16_t zoneHeight,
                                                         const uint16_t *paletteEntries,
                                                         uint16_t paletteEntryCount,
                                                         ZELColorEncoding paletteEncoding,
                                                         size_t *outSize) {
    enum { WIDTH = 4, HEIGHT = 2, PIXEL_COUNT = WIDTH * HEIGHT };
    const uint16_t width = WIDTH;
    const uint16_t height = HEIGHT;

    assert(zoneWidth != 0 && zoneHeight != 0);
    assert(width % zoneWidth == 0);
    assert(height % zoneHeight == 0);

    const size_t fileHeaderSize = ZEL_FILE_HEADER_DISK_SIZE;
    const size_t paletteHeaderSize = ZEL_PALETTE_HEADER_DISK_SIZE;
    const size_t frameHeaderSize = ZEL_FRAME_HEADER_DISK_SIZE;
    const size_t frameIndexEntrySize = ZEL_FRAME_INDEX_ENTRY_DISK_SIZE;

    uint8_t fh[ZEL_FILE_HEADER_DISK_SIZE];
    memset(fh, 0, sizeof(fh));
    memcpy(fh, "ZEL0", 4);
    write_le16(fh + 4, 1);
    write_le16(fh + 6, (uint16_t)fileHeaderSize);
    write_le16(fh + 8, width);
    write_le16(fh + 0x0A, height);
    write_le16(fh + 0x0C, zoneWidth);
    write_le16(fh + 0x0E, zoneHeight);
    fh[0x10] = ZEL_COLOR_FORMAT_INDEXED8;
    fh[0x11] = 0x01u /* hasGlobalPalette */ | 0x04u /* hasFrameIndexTable */;
    write_le32(fh + 0x12, 1);
    write_le16(fh + 0x16, 16);

    uint8_t ph[ZEL_PALETTE_HEADER_DISK_SIZE];
    memset(ph, 0, sizeof(ph));
    ph[0] = ZEL_PALETTE_TYPE_GLOBAL;
    ph[1] = (uint8_t)paletteHeaderSize;
    write_le16(ph + 2, paletteEntryCount);
    ph[4] = (uint8_t)paletteEncoding;

    uint8_t frh[ZEL_FRAME_HEADER_DISK_SIZE];
    memset(frh, 0, sizeof(frh));
    frh[0] = 1; /* blockType */
    frh[1] = (uint8_t)frameHeaderSize;
    frh[2] = 0x01u; /* keyframe */
    frh[5] = ZEL_COMPRESSION_NONE;
    write_le16(frh + 6, 0); /* referenceFrameIndex */
    write_le16(frh + 8, 0); /* localPaletteEntryCount */

    uint8_t pixels[PIXEL_COUNT] = {0, 1, 0, 1, 1, 0, 1, 0};
    const uint32_t zoneCount = (width / zoneWidth) * (height / zoneHeight);
    write_le16(frh + 3, (uint16_t)zoneCount);

    const size_t zoneBytes = (size_t)zoneWidth * zoneHeight;
    const size_t frameBlockSize = frameHeaderSize + zoneCount * (sizeof(uint32_t) + zoneBytes);

    const size_t paletteBytes = (size_t)paletteEntryCount * sizeof(uint16_t);

    size_t size = fileHeaderSize + paletteHeaderSize + paletteBytes + frameIndexEntrySize + frameBlockSize;

    uint8_t *buf = (uint8_t *)malloc(size);
    assert(buf);

    size_t off = 0;

    memcpy(buf + off, fh, fileHeaderSize);
    off += fileHeaderSize;

    memcpy(buf + off, ph, paletteHeaderSize);
    off += paletteHeaderSize;

    uint8_t *paletteBytesBuf = (uint8_t *)malloc(paletteBytes);
    assert(paletteBytesBuf);
    write_palette_bytes(paletteBytesBuf, paletteEntries, paletteEntryCount, paletteEncoding);
    memcpy(buf + off, paletteBytesBuf, paletteBytes);
    off += paletteBytes;
    free(paletteBytesBuf);

    size_t frameIndexTableOffset = off;
    off += frameIndexEntrySize;

    size_t frameOffset = off;
    memcpy(buf + off, frh, frameHeaderSize);
    off += frameHeaderSize;

    uint8_t zoneScratch[PIXEL_COUNT];
    write_zone_chunks(buf, &off, pixels, width, height, zoneWidth, zoneHeight, zoneScratch);

    uint8_t fie[ZEL_FRAME_INDEX_ENTRY_DISK_SIZE];
    memset(fie, 0, sizeof(fie));
    write_le32(fie + 0, (uint32_t)frameOffset);
    write_le32(fie + 4, (uint32_t)frameBlockSize);
    fie[8] = 0x01u; /* keyframe */
    write_le16(fie + 9, 16);

    memcpy(buf + frameIndexTableOffset, fie, frameIndexEntrySize);

    assert(off == size);

    if (outSize)
        *outSize = size;
    return buf;
}

static uint8_t *buildSimpleZelSingleFrameWithZones(uint16_t zoneWidth,
                                                   uint16_t zoneHeight,
                                                   size_t *outSize) {
    static const uint16_t palette[2] = {0x0000, 0xFFFF};
    return buildSimpleZelSingleFrameWithZonesCustom(zoneWidth,
                                                    zoneHeight,
                                                    palette,
                                                    2,
                                                    ZEL_COLOR_RGB565_LE,
                                                    outSize);
}

static uint8_t *buildSimpleZelSingleFrame(size_t *outSize) {
    return buildSimpleZelSingleFrameWithZones(4, 2, outSize);
}

static uint8_t *buildSimpleZelSingleFrameMultiZone(size_t *outSize) {
    return buildSimpleZelSingleFrameWithZones(2, 1, outSize);
}

/* Builds a simple ZEL file in memory with:
    - 2x1 pixels
    - 3 frames
    - global palette with 2 entries (RGB565)
    - no compression
    - single zone per frame
    Pixel pattern: 0,1
*/
static uint8_t *buildSimpleZelThreeFrames(size_t *outSize) {
    enum { WIDTH = 2, HEIGHT = 1, PIXEL_COUNT = WIDTH * HEIGHT };
    const uint16_t width = WIDTH;
    const uint16_t height = HEIGHT;

    const size_t fileHeaderSize = ZEL_FILE_HEADER_DISK_SIZE;
    const size_t paletteHeaderSize = ZEL_PALETTE_HEADER_DISK_SIZE;
    const size_t frameHeaderSize = ZEL_FRAME_HEADER_DISK_SIZE;
    const size_t frameIndexEntrySize = ZEL_FRAME_INDEX_ENTRY_DISK_SIZE;

    uint8_t fh[ZEL_FILE_HEADER_DISK_SIZE];
    memset(fh, 0, sizeof(fh));
    memcpy(fh, "ZEL0", 4);
    write_le16(fh + 4, 1);
    write_le16(fh + 6, (uint16_t)fileHeaderSize);
    write_le16(fh + 8, width);
    write_le16(fh + 0x0A, height);
    write_le16(fh + 0x0C, width);
    write_le16(fh + 0x0E, height);
    fh[0x10] = ZEL_COLOR_FORMAT_INDEXED8;
    fh[0x11] = 0x01u /* hasGlobalPalette */ | 0x04u /* hasFrameIndexTable */;
    write_le32(fh + 0x12, 3);
    write_le16(fh + 0x16, 0);

    uint8_t ph[ZEL_PALETTE_HEADER_DISK_SIZE];
    memset(ph, 0, sizeof(ph));
    ph[0] = ZEL_PALETTE_TYPE_GLOBAL;
    ph[1] = (uint8_t)paletteHeaderSize;
    write_le16(ph + 2, 2);
    ph[4] = ZEL_COLOR_RGB565_LE;

    uint16_t palette[2] = {0x0000, 0xFFFF};

    uint8_t fie[3][ZEL_FRAME_INDEX_ENTRY_DISK_SIZE];
    memset(fie, 0, sizeof(fie));

    fie[0][8] = 0x01u;
    write_le16(fie[0] + 9, 10);
    fie[1][8] = 0x01u;
    write_le16(fie[1] + 9, 20);
    fie[2][8] = 0x01u;
    write_le16(fie[2] + 9, 30);

    uint8_t frh[ZEL_FRAME_HEADER_DISK_SIZE];
    memset(frh, 0, sizeof(frh));
    frh[0] = 1; /* blockType */
    frh[1] = (uint8_t)frameHeaderSize;
    frh[2] = 0x01u; /* keyframe */
    frh[5] = ZEL_COMPRESSION_NONE;
    write_le16(frh + 6, 0);
    write_le16(frh + 8, 0);

    uint8_t pixels[PIXEL_COUNT] = {0, 1};
    const uint32_t zoneCount = (width / width) * (height / height);
    write_le16(frh + 3, (uint16_t)zoneCount);

    const size_t zoneBytes = (size_t)width * height;
    size_t oneFrameBlockSize = frameHeaderSize + zoneCount * (sizeof(uint32_t) + zoneBytes);

    size_t size = fileHeaderSize + paletteHeaderSize + sizeof(palette) + (frameIndexEntrySize * 3)
                  + 3 * oneFrameBlockSize;

    uint8_t *buf = (uint8_t *)malloc(size);
    assert(buf);

    size_t off = 0;

    memcpy(buf + off, fh, fileHeaderSize);
    off += fileHeaderSize;

    memcpy(buf + off, ph, paletteHeaderSize);
    off += paletteHeaderSize;

    memcpy(buf + off, palette, sizeof(palette));
    off += sizeof(palette);

    size_t frameIndexTableOffset = off;
    off += frameIndexEntrySize * 3;

    uint8_t zoneScratch[PIXEL_COUNT];

    size_t frame0Offset = off;
    memcpy(buf + off, frh, frameHeaderSize);
    off += frameHeaderSize;
    write_zone_chunks(buf, &off, pixels, width, height, width, height, zoneScratch);

    size_t frame1Offset = off;
    memcpy(buf + off, frh, frameHeaderSize);
    off += frameHeaderSize;
    write_zone_chunks(buf, &off, pixels, width, height, width, height, zoneScratch);

    size_t frame2Offset = off;
    memcpy(buf + off, frh, frameHeaderSize);
    off += frameHeaderSize;
    write_zone_chunks(buf, &off, pixels, width, height, width, height, zoneScratch);

    write_le32(fie[0] + 0, (uint32_t)frame0Offset);
    write_le32(fie[1] + 0, (uint32_t)frame1Offset);
    write_le32(fie[2] + 0, (uint32_t)frame2Offset);

    write_le32(fie[0] + 4, (uint32_t)oneFrameBlockSize);
    write_le32(fie[1] + 4, (uint32_t)oneFrameBlockSize);
    write_le32(fie[2] + 4, (uint32_t)oneFrameBlockSize);

    memcpy(buf + frameIndexTableOffset, fie, frameIndexEntrySize * 3);

    assert(off == size);
    if (outSize)
        *outSize = size;
    return buf;
}

/* === Tests === */

static void test_open_and_basic_getters(void) {
    size_t size = 0;
    uint8_t *data = buildSimpleZelSingleFrame(&size);

    ZELResult res;
    ZELContext *ctx = zelOpenMemory(data, size, &res);
    assert(ctx != NULL);
    assert(res == ZEL_OK);

    assert(zelGetWidth(ctx) == 4);
    assert(zelGetHeight(ctx) == 2);
    assert(zelGetFrameCount(ctx) == 1);
    assert(zelGetDefaultFrameDurationMs(ctx) == 16);

    zelClose(ctx);
    free(data);
}

static void test_palette_and_decode_index8(void) {
    size_t size = 0;
    uint8_t *data = buildSimpleZelSingleFrame(&size);

    ZELResult res;
    ZELContext *ctx = zelOpenMemory(data, size, &res);
    assert(ctx && res == ZEL_OK);

    assert(zelHasGlobalPalette(ctx) == 1);

    const uint16_t *pal = NULL;
    uint16_t palCount = 0;
    res = zelGetGlobalPalette(ctx, &pal, &palCount);
    assert(res == ZEL_OK);
    assert(pal != NULL);
    assert(palCount == 2);
    assert(pal[0] == 0x0000);
    assert(pal[1] == 0xFFFF);

    uint8_t buf[2 * 4];
    memset(buf, 0xCD, sizeof(buf));

    res = zelDecodeFrameIndex8(ctx, 0, buf, 4);
    assert(res == ZEL_OK);

    assert(buf[0] == 0);
    assert(buf[1] == 1);
    assert(buf[2] == 0);
    assert(buf[3] == 1);
    assert(buf[4] == 1);
    assert(buf[5] == 0);
    assert(buf[6] == 1);
    assert(buf[7] == 0);

    zelClose(ctx);
    free(data);
}

static void test_stream_decode_index8(void) {
    size_t size = 0;
    uint8_t *data = buildSimpleZelSingleFrame(&size);

    TestMemoryStream memStream = {data, size};
    ZELInputStream stream;
    stream.read = test_memory_stream_read;
    stream.close = NULL;
    stream.userData = &memStream;
    stream.size = size;

    ZELResult res = ZEL_OK;
    ZELContext *ctx = zelOpenStream(&stream, &res);
    assert(ctx && res == ZEL_OK);

    uint8_t buf[8];
    memset(buf, 0xEF, sizeof(buf));

    res = zelDecodeFrameIndex8(ctx, 0, buf, 4);
    assert(res == ZEL_OK);

    for (size_t i = 0; i < sizeof(kSimpleFramePattern); ++i)
        assert(buf[i] == kSimpleFramePattern[i]);

    zelClose(ctx);
    free(data);
}

static void test_decode_rgb565(void) {
    size_t size = 0;
    uint8_t *data = buildSimpleZelSingleFrame(&size);

    ZELResult res;
    ZELContext *ctx = zelOpenMemory(data, size, &res);
    assert(ctx && res == ZEL_OK);

    uint16_t buf[2 * 4];
    memset(buf, 0x00, sizeof(buf));

    res = zelDecodeFrameRgb565(ctx, 0, buf, 4);
    assert(res == ZEL_OK);

    assert(buf[0] == 0x0000);
    assert(buf[1] == 0xFFFF);
    assert(buf[2] == 0x0000);
    assert(buf[3] == 0xFFFF);
    assert(buf[4] == 0xFFFF);
    assert(buf[5] == 0x0000);
    assert(buf[6] == 0xFFFF);
    assert(buf[7] == 0x0000);

    zelClose(ctx);
    free(data);
}

static void test_palette_endianness_controls(void) {
    size_t sizeLE = 0;
    static const uint16_t paletteLE[2] = {0x00F8, 0x1234};
    uint8_t *dataLE = buildSimpleZelSingleFrameWithZonesCustom(4,
                                                               2,
                                                               paletteLE,
                                                               2,
                                                               ZEL_COLOR_RGB565_LE,
                                                               &sizeLE);
    ZELResult res;
    ZELContext *ctx = zelOpenMemory(dataLE, sizeLE, &res);
    assert(ctx && res == ZEL_OK);

    const uint16_t *pal = NULL;
    uint16_t palCount = 0;
    res = zelGetGlobalPalette(ctx, &pal, &palCount);
    assert(res == ZEL_OK && palCount == 2);
    assert(pal[0] == paletteLE[0]);
    assert(pal[1] == paletteLE[1]);

    uint16_t expected[8];
    build_expected_rgb_frame(expected, paletteLE);

    uint16_t frame[8];
    memset(frame, 0, sizeof(frame));
    res = zelDecodeFrameRgb565(ctx, 0, frame, 4);
    assert(res == ZEL_OK);
    assert(memcmp(frame, expected, sizeof(expected)) == 0);

    uint16_t swappedPalette[2] = {swap_u16(paletteLE[0]), swap_u16(paletteLE[1])};
    zelSetOutputColorEncoding(ctx, ZEL_COLOR_RGB565_BE);
    res = zelGetGlobalPalette(ctx, &pal, &palCount);
    assert(res == ZEL_OK && palCount == 2);
    assert(pal[0] == swappedPalette[0]);
    assert(pal[1] == swappedPalette[1]);

    build_expected_rgb_frame(expected, swappedPalette);
    memset(frame, 0, sizeof(frame));
    res = zelDecodeFrameRgb565(ctx, 0, frame, 4);
    assert(res == ZEL_OK);
    assert(memcmp(frame, expected, sizeof(expected)) == 0);

    zelClose(ctx);
    free(dataLE);

    size_t sizeBE = 0;
    static const uint16_t paletteBE[2] = {0x0F1E, 0x00D1};
    uint8_t *dataBE = buildSimpleZelSingleFrameWithZonesCustom(4,
                                                               2,
                                                               paletteBE,
                                                               2,
                                                               ZEL_COLOR_RGB565_BE,
                                                               &sizeBE);
    ctx = zelOpenMemory(dataBE, sizeBE, &res);
    assert(ctx && res == ZEL_OK);

    uint16_t swappedBE[2] = {swap_u16(paletteBE[0]), swap_u16(paletteBE[1])};
    res = zelGetGlobalPalette(ctx, &pal, &palCount);
    assert(res == ZEL_OK && palCount == 2);
    assert(pal[0] == swappedBE[0]);
    assert(pal[1] == swappedBE[1]);

    build_expected_rgb_frame(expected, swappedBE);
    memset(frame, 0, sizeof(frame));
    res = zelDecodeFrameRgb565(ctx, 0, frame, 4);
    assert(res == ZEL_OK);
    assert(memcmp(frame, expected, sizeof(expected)) == 0);

    zelSetOutputColorEncoding(ctx, ZEL_COLOR_RGB565_LE);
    res = zelGetGlobalPalette(ctx, &pal, &palCount);
    assert(res == ZEL_OK && palCount == 2);
    assert(pal[0] == paletteBE[0]);
    assert(pal[1] == paletteBE[1]);

    build_expected_rgb_frame(expected, paletteBE);
    memset(frame, 0, sizeof(frame));
    res = zelDecodeFrameRgb565(ctx, 0, frame, 4);
    assert(res == ZEL_OK);
    assert(memcmp(frame, expected, sizeof(expected)) == 0);

    zelClose(ctx);
    free(dataBE);
}

static void test_zone_decoders(void) {
    size_t size = 0;
    uint8_t *data = buildSimpleZelSingleFrameMultiZone(&size);

    ZELResult res;
    ZELContext *ctx = zelOpenMemory(data, size, &res);
    assert(ctx && res == ZEL_OK);

    static const uint8_t expectedIndices[8] = {0, 1, 0, 1, 1, 0, 1, 0};
    static const uint16_t expectedRgb[8] =
            {0x0000, 0xFFFF, 0x0000, 0xFFFF, 0xFFFF, 0x0000, 0xFFFF, 0x0000};

    const uint16_t width = zelGetWidth(ctx);
    const uint16_t height = zelGetHeight(ctx);
    const uint16_t zoneWidth = zelGetZoneWidth(ctx);
    const uint16_t zoneHeight = zelGetZoneHeight(ctx);
    assert(width != 0 && height != 0);
    assert(zoneWidth != 0 && zoneHeight != 0);

    const uint32_t zonesPerRow = width / zoneWidth;
    const uint32_t zonesPerCol = height / zoneHeight;
    const uint32_t zoneCount = zonesPerRow * zonesPerCol;
    const size_t framePixelCount = (size_t)width * height;
    const size_t zonePixelCount = (size_t)zoneWidth * zoneHeight;

    assert(framePixelCount == sizeof(expectedIndices));
    assert(framePixelCount == (sizeof(expectedRgb) / sizeof(expectedRgb[0])));

    uint8_t *indices = (uint8_t *)malloc(framePixelCount);
    uint16_t *rgb = (uint16_t *)malloc(framePixelCount * sizeof(uint16_t));
    uint8_t *zoneIndexBuf = (uint8_t *)malloc(zonePixelCount);
    uint16_t *zoneRgbBuf = (uint16_t *)malloc(zonePixelCount * sizeof(uint16_t));
    assert(indices && rgb && zoneIndexBuf && zoneRgbBuf);

    memset(indices, 0xCC, framePixelCount);
    memset(rgb, 0x00, framePixelCount * sizeof(uint16_t));

    for (uint32_t zone = 0; zone < zoneCount; ++zone) {
        res = zelDecodeFrameIndex8Zone(ctx, 0, zone, zoneIndexBuf);
        assert(res == ZEL_OK);
        blit_indices_zone_to_frame(zone, width, zoneWidth, zoneHeight, indices, zoneIndexBuf);
    }
    assert(memcmp(indices, expectedIndices, sizeof(expectedIndices)) == 0);

    for (uint32_t zone = 0; zone < zoneCount; ++zone) {
        res = zelDecodeFrameRgb565Zone(ctx, 0, zone, zoneRgbBuf);
        assert(res == ZEL_OK);
        blit_rgb_zone_to_frame(zone, width, zoneWidth, zoneHeight, rgb, zoneRgbBuf);
    }
    for (size_t i = 0; i < framePixelCount; ++i) {
        assert(rgb[i] == expectedRgb[i]);
    }

    free(zoneRgbBuf);
    free(zoneIndexBuf);
    free(rgb);
    free(indices);

    zelClose(ctx);
    free(data);
}

static void test_timeline_helpers(void) {
    size_t size = 0;
    uint8_t *data = buildSimpleZelThreeFrames(&size);

    ZELResult res;
    ZELContext *ctx = zelOpenMemory(data, size, &res);
    assert(ctx && res == ZEL_OK);

    uint32_t total = 0;
    res = zelGetTotalDurationMs(ctx, &total);
    assert(res == ZEL_OK);
    assert(total == (10 + 20 + 30));

    uint32_t fi, start;

    res = zelFindFrameByTimeMs(ctx, 0, &fi, &start);
    assert(res == ZEL_OK);
    assert(fi == 0);
    assert(start == 0);

    res = zelFindFrameByTimeMs(ctx, 9, &fi, &start);
    assert(res == ZEL_OK);
    assert(fi == 0);
    assert(start == 0);

    res = zelFindFrameByTimeMs(ctx, 10, &fi, &start);
    assert(res == ZEL_OK);
    assert(fi == 1);
    assert(start == 10);

    res = zelFindFrameByTimeMs(ctx, 29, &fi, &start);
    assert(res == ZEL_OK);
    assert(fi == 1);
    assert(start == 10);

    res = zelFindFrameByTimeMs(ctx, 30, &fi, &start);
    assert(res == ZEL_OK);
    assert(fi == 2);
    assert(start == 30);

    res = zelFindFrameByTimeMs(ctx, 59, &fi, &start);
    assert(res == ZEL_OK);
    assert(fi == 2);
    assert(start == 30);

    res = zelFindFrameByTimeMs(ctx, 60, &fi, &start);
    assert(res == ZEL_OK);
    assert(fi == 0);
    assert(start == 0);

    zelClose(ctx);
    free(data);
}

static void test_result_to_string(void) {
    const char *s = zelResultToString(ZEL_OK);
    assert(s && strcmp(s, "ZEL_OK") == 0);

    s = zelResultToString(ZEL_ERR_INVALID_MAGIC);
    assert(s && strstr(s, "INVALID_MAGIC") != NULL);
}

/* === Tests with binary data === */

static void test_open_and_basic_getters_binary(void) {
    ZELResult res;
    ZELContext *ctx = zelOpenMemory(g_zelFixtureSimpleFile, g_zelFixtureSimpleFileSize, &res);
    assert(ctx != NULL);
    assert(res == ZEL_OK);

    assert(zelGetWidth(ctx) == 4);
    assert(zelGetHeight(ctx) == 2);
    assert(zelGetFrameCount(ctx) == 1);
    assert(zelGetDefaultFrameDurationMs(ctx) == 16);

    zelClose(ctx);
}

static void test_palette_and_decode_index8_binary(void) {
    ZELResult res;
    ZELContext *ctx = zelOpenMemory(g_zelFixtureSimpleFile, g_zelFixtureSimpleFileSize, &res);
    assert(ctx && res == ZEL_OK);

    assert(zelHasGlobalPalette(ctx) == 1);

    const uint16_t *pal = NULL;
    uint16_t palCount = 0;
    res = zelGetGlobalPalette(ctx, &pal, &palCount);
    assert(res == ZEL_OK);
    assert(pal != NULL);
    assert(palCount == 2);
    assert(pal[0] == 0x0000);
    assert(pal[1] == 0xFFFF);

    uint8_t buf[2 * 4];
    memset(buf, 0xCD, sizeof(buf));

    res = zelDecodeFrameIndex8(ctx, 0, buf, 4);
    assert(res == ZEL_OK);

    assert(buf[0] == 0);
    assert(buf[1] == 1);
    assert(buf[2] == 0);
    assert(buf[3] == 1);
    assert(buf[4] == 1);
    assert(buf[5] == 0);
    assert(buf[6] == 1);
    assert(buf[7] == 0);

    zelClose(ctx);
}

static void test_decode_rgb565_binary(void) {
    ZELResult res;
    ZELContext *ctx = zelOpenMemory(g_zelFixtureSimpleFile, g_zelFixtureSimpleFileSize, &res);
    assert(ctx && res == ZEL_OK);

    uint16_t buf[2 * 4];
    memset(buf, 0x00, sizeof(buf));

    res = zelDecodeFrameRgb565(ctx, 0, buf, 4);
    assert(res == ZEL_OK);

    assert(buf[0] == 0x0000);
    assert(buf[1] == 0xFFFF);
    assert(buf[2] == 0x0000);
    assert(buf[3] == 0xFFFF);
    assert(buf[4] == 0xFFFF);
    assert(buf[5] == 0x0000);
    assert(buf[6] == 0xFFFF);
    assert(buf[7] == 0x0000);

    zelClose(ctx);
}

static void test_timeline_helpers_binary(void) {
    ZELResult res;
    ZELContext *ctx = zelOpenMemory(g_zelFixtureSimpleFile, g_zelFixtureSimpleFileSize, &res);
    assert(ctx && res == ZEL_OK);

    uint32_t total = 0;
    res = zelGetTotalDurationMs(ctx, &total);
    assert(res == ZEL_OK);
    assert(total == 16);

    uint32_t fi = 0;
    uint32_t start = 0;

    res = zelFindFrameByTimeMs(ctx, 0, &fi, &start);
    assert(res == ZEL_OK);
    assert(fi == 0);
    assert(start == 0);

    res = zelFindFrameByTimeMs(ctx, 15, &fi, &start);
    assert(res == ZEL_OK);
    assert(fi == 0);
    assert(start == 0);

    res = zelFindFrameByTimeMs(ctx, 16, &fi, &start);
    assert(res == ZEL_OK);
    assert(fi == 0);
    assert(start == 0);

    zelClose(ctx);
}

int main(void) {
    test_open_and_basic_getters();
    test_palette_and_decode_index8();
    test_stream_decode_index8();
    test_decode_rgb565();
    test_palette_endianness_controls();
    test_zone_decoders();
    test_timeline_helpers();
    test_open_and_basic_getters_binary();
    test_palette_and_decode_index8_binary();
    test_decode_rgb565_binary();
    test_timeline_helpers_binary();
    test_result_to_string();

    printf("All ZEL tests passed.\n");
    return 0;
}
