#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#include "fixtures/simple_zel_file.h"
#include "zel/zel.h"

static void write_zone_chunks(uint8_t *dst, size_t *offset, const uint8_t *pixels,
                              uint16_t width, uint16_t height, uint16_t zoneWidth,
                              uint16_t zoneHeight, uint8_t *scratch) {
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


/* Simple helper function: builds a simple ZEL file in memory with configurable zone size:
    - 4x2 pixels
    - 1 frame
    - global palette with 2 entries (RGB565)
    - no compression
    Pixel pattern: 0,1,0,1 / 1,0,1,0
*/
static uint8_t *buildSimpleZelSingleFrameWithZones(uint16_t zoneWidth, uint16_t zoneHeight,
                                                                    size_t *outSize) {
     enum { WIDTH = 4, HEIGHT = 2, PIXEL_COUNT = WIDTH * HEIGHT };
     const uint16_t width = WIDTH;
     const uint16_t height = HEIGHT;

     assert(zoneWidth != 0 && zoneHeight != 0);
     assert(width % zoneWidth == 0);
     assert(height % zoneHeight == 0);

    ZELFileHeader fh;
    memset(&fh, 0, sizeof(fh));
    memcpy(fh.magic, "ZEL0", 4);
    fh.version = 1;
    fh.headerSize = sizeof(ZELFileHeader);
    fh.width = width;
    fh.height = height;
    fh.zoneWidth = zoneWidth;
    fh.zoneHeight = zoneHeight;
    fh.colorFormat = ZEL_COLOR_FORMAT_INDEXED8;
    fh.flags.hasGlobalPalette = 1;
    fh.flags.hasFrameLocalPalettes = 0;
    fh.flags.hasFrameIndexTable = 1;
    fh.frameCount = 1;
    fh.defaultFrameDuration = 16;

    ZELPaletteHeader ph;
    memset(&ph, 0, sizeof(ph));
    ph.type = ZEL_PALETTE_TYPE_GLOBAL;
    ph.headerSize = sizeof(ZELPaletteHeader);
    ph.entryCount = 2;
    ph.colorEncoding = ZEL_COLOR_RGB565;

    uint16_t palette[2];
    palette[0] = 0x0000; /* black */
    palette[1] = 0xFFFF; /* white */

    ZELFrameHeader frh;
    memset(&frh, 0, sizeof(frh));
    frh.blockType = 1;
    frh.headerSize = sizeof(ZELFrameHeader);
    frh.flags.keyframe = 1;
    frh.compressionType = ZEL_COMPRESSION_NONE;
    frh.localPaletteEntryCount = 0;

    uint8_t pixels[PIXEL_COUNT] = {0, 1, 0, 1, 1, 0, 1, 0};
    const uint32_t zoneCount = (width / fh.zoneWidth) * (height / fh.zoneHeight);
    frh.zoneCount = (uint16_t)zoneCount;

    const size_t zoneBytes = (size_t)fh.zoneWidth * fh.zoneHeight;
    const size_t frameBlockSize = sizeof(ZELFrameHeader) +
                                  zoneCount * (sizeof(uint32_t) + zoneBytes);

    ZELFrameIndexEntry fie;
    memset(&fie, 0, sizeof(fie));
    fie.flags.keyframe = 1;
    fie.frameDuration = 16;
    fie.frameSize = (uint32_t)frameBlockSize;

    size_t size = sizeof(ZELFileHeader) + sizeof(ZELPaletteHeader) + sizeof(palette) +
                  sizeof(ZELFrameIndexEntry) + frameBlockSize;

    uint8_t *buf = (uint8_t *)malloc(size);
    assert(buf);

    size_t off = 0;

    memcpy(buf + off, &fh, sizeof(fh));
    off += sizeof(fh);

    memcpy(buf + off, &ph, sizeof(ph));
    off += sizeof(ph);

    memcpy(buf + off, &palette[0], sizeof(palette));
    off += sizeof(palette);

    size_t frameIndexTableOffset = off;
    off += sizeof(fie);

    size_t frameOffset = off;
    memcpy(buf + off, &frh, sizeof(frh));
    off += sizeof(frh);

    uint8_t zoneScratch[PIXEL_COUNT];
    write_zone_chunks(buf, &off, pixels, width, height, fh.zoneWidth, fh.zoneHeight,
                      zoneScratch);

    fie.frameOffset = (uint32_t)frameOffset;

    memcpy(buf + frameIndexTableOffset, &fie, sizeof(fie));

    assert(off == size);

    if (outSize)
        *outSize = size;
    return buf;
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

    ZELFileHeader fh;
    memset(&fh, 0, sizeof(fh));
    memcpy(fh.magic, "ZEL0", 4);
    fh.version = 1;
    fh.headerSize = sizeof(ZELFileHeader);
    fh.width = width;
    fh.height = height;
    fh.zoneWidth = width;
    fh.zoneHeight = height;
    fh.colorFormat = ZEL_COLOR_FORMAT_INDEXED8;
    fh.flags.hasGlobalPalette = 1;
    fh.flags.hasFrameLocalPalettes = 0;
    fh.flags.hasFrameIndexTable = 1;
    fh.frameCount = 3;
    fh.defaultFrameDuration = 0;

    ZELPaletteHeader ph;
    memset(&ph, 0, sizeof(ph));
    ph.type = ZEL_PALETTE_TYPE_GLOBAL;
    ph.headerSize = sizeof(ZELPaletteHeader);
    ph.entryCount = 2;
    ph.colorEncoding = ZEL_COLOR_RGB565;

    uint16_t palette[2] = {0x0000, 0xFFFF};

    ZELFrameIndexEntry fie[3];
    memset(fie, 0, sizeof(fie));

    fie[0].flags.keyframe = 1;
    fie[0].frameDuration = 10;
    fie[1].flags.keyframe = 1;
    fie[1].frameDuration = 20;
    fie[2].flags.keyframe = 1;
    fie[2].frameDuration = 30;

    ZELFrameHeader frh;
    memset(&frh, 0, sizeof(frh));
    frh.blockType = 1;
    frh.headerSize = sizeof(ZELFrameHeader);
    frh.flags.keyframe = 1;
    frh.compressionType = ZEL_COMPRESSION_NONE;
    frh.localPaletteEntryCount = 0;

    uint8_t pixels[PIXEL_COUNT] = {0, 1};
    const uint32_t zoneCount = (width / fh.zoneWidth) * (height / fh.zoneHeight);
    frh.zoneCount = (uint16_t)zoneCount;

    const size_t zoneBytes = (size_t)fh.zoneWidth * fh.zoneHeight;
    size_t oneFrameBlockSize = sizeof(ZELFrameHeader) +
                               zoneCount * (sizeof(uint32_t) + zoneBytes);

    size_t size = sizeof(ZELFileHeader) + sizeof(ZELPaletteHeader) + sizeof(palette) + sizeof(fie) +
                  3 * oneFrameBlockSize;

    uint8_t *buf = (uint8_t *)malloc(size);
    assert(buf);

    size_t off = 0;

    memcpy(buf + off, &fh, sizeof(fh));
    off += sizeof(fh);

    memcpy(buf + off, &ph, sizeof(ph));
    off += sizeof(ph);

    memcpy(buf + off, palette, sizeof(palette));
    off += sizeof(palette);

    size_t frameIndexTableOffset = off;
    off += sizeof(fie);

    uint8_t zoneScratch[PIXEL_COUNT];

    size_t frame0Offset = off;
    memcpy(buf + off, &frh, sizeof(frh));
    off += sizeof(frh);
    write_zone_chunks(buf, &off, pixels, width, height, fh.zoneWidth, fh.zoneHeight,
                      zoneScratch);

    size_t frame1Offset = off;
    memcpy(buf + off, &frh, sizeof(frh));
    off += sizeof(frh);
    write_zone_chunks(buf, &off, pixels, width, height, fh.zoneWidth, fh.zoneHeight,
                      zoneScratch);

    size_t frame2Offset = off;
    memcpy(buf + off, &frh, sizeof(frh));
    off += sizeof(frh);
    write_zone_chunks(buf, &off, pixels, width, height, fh.zoneWidth, fh.zoneHeight,
                      zoneScratch);

    fie[0].frameOffset = (uint32_t)frame0Offset;
    fie[1].frameOffset = (uint32_t)frame1Offset;
    fie[2].frameOffset = (uint32_t)frame2Offset;

    fie[0].frameSize = (uint32_t)oneFrameBlockSize;
    fie[1].frameSize = (uint32_t)oneFrameBlockSize;
    fie[2].frameSize = (uint32_t)oneFrameBlockSize;

    memcpy(buf + frameIndexTableOffset, fie, sizeof(fie));

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

static void test_zone_decoders(void) {
    size_t size = 0;
    uint8_t *data = buildSimpleZelSingleFrameMultiZone(&size);

    ZELResult res;
    ZELContext *ctx = zelOpenMemory(data, size, &res);
    assert(ctx && res == ZEL_OK);

    static const uint8_t expectedIndices[8] = {0, 1, 0, 1, 1, 0, 1, 0};
    static const uint16_t expectedRgb[8] = {0x0000, 0xFFFF, 0x0000, 0xFFFF,
                                            0xFFFF, 0x0000, 0xFFFF, 0x0000};

    uint8_t indices[8];
    memset(indices, 0xCC, sizeof(indices));
    for (uint32_t zone = 0; zone < 4; ++zone) {
        res = zelDecodeFrameIndex8Zone(ctx, 0, zone, indices, 4);
        assert(res == ZEL_OK);
    }
    assert(memcmp(indices, expectedIndices, sizeof(expectedIndices)) == 0);

    uint16_t rgb[8];
    memset(rgb, 0x00, sizeof(rgb));
    for (uint32_t zone = 0; zone < 4; ++zone) {
        res = zelDecodeFrameRgb565Zone(ctx, 0, zone, rgb, 4);
        assert(res == ZEL_OK);
    }
    for (size_t i = 0; i < 8; ++i) {
        assert(rgb[i] == expectedRgb[i]);
    }

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
    test_decode_rgb565();
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
