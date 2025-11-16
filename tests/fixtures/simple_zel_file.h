#ifndef ZEL_TEST_FIXTURE_SIMPLE_ZEL_FILE_H
#define ZEL_TEST_FIXTURE_SIMPLE_ZEL_FILE_H

#include <stddef.h>
#include <stdint.h>

/* Minimal Zel file: 4x2 pixels, 1 frame, global palette with two entries. */
static const uint8_t g_zelFixtureSimpleFile[] = {
    /* ZELFileHeader (32 bytes) */
    0x5A, 0x45, 0x4C, 0x30, /* magic "ZEL0" */
    0x01, 0x00,             /* version = 1 */
    0x20, 0x00,             /* headerSize = 32 */
    0x04, 0x00,             /* width = 4 */
    0x02, 0x00,             /* height = 2 */
    0x04,                   /* tileWidth */
    0x02,                   /* tileHeight */
    0x00,                   /* colorFormat = INDEXED8 */
    0x05,                   /* flags: hasGlobalPalette | hasFrameIndexTable */
    0x01, 0x00, 0x00, 0x00, /* frameCount = 1 */
    0x10, 0x00,             /* defaultFrameDuration = 16 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* reserved[10] */

    /* ZELPaletteHeader (8 bytes) */
    0x00, /* type = global */
    0x08, /* headerSize = 8 */
    0x02, 0x00, /* entryCount = 2 */
    0x00, /* colorEncoding = RGB565 */
    0x00, 0x00, 0x00, /* reserved[3] */

    /* Palette entries (2 × RGB565) */
    0x00, 0x00, /* black */
    0xFF, 0xFF, /* white */

    /* ZELFrameIndexEntry (11 bytes) */
    0x37, 0x00, 0x00, 0x00, /* frameOffset = 55 */
    0x16, 0x00, 0x00, 0x00, /* frameSize = 22 */
    0x01, /* flags.keyframe */
    0x10, 0x00, /* frameDuration = 16 */

    /* ZELFrameHeader (14 bytes) */
    0x01, /* blockType = 1 */
    0x0E, /* headerSize = 14 */
    0x01, /* flags.keyframe */
    0x00, 0x00, /* tileCount = 0 */
    0x00, /* compressionType = NONE */
    0x00, 0x00, /* referenceFrameIndex = 0 */
    0x00, 0x00, /* localPaletteEntryCount = 0 */
    0x00, 0x00, 0x00, 0x00, /* reserved[4] */

    /* Pixel data (8 bytes) */
    0x00, 0x01, 0x00, 0x01,
    0x01, 0x00, 0x01, 0x00
};

static const size_t g_zelFixtureSimpleFileSize = sizeof(g_zelFixtureSimpleFile);

#endif /* ZEL_TEST_FIXTURE_SIMPLE_ZEL_FILE_H */
