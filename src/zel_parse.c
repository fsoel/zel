#include "zel_internal.h"

#include <string.h>

void zelParseFileHeader(const uint8_t *src, ZELFileHeader *out) {
    if (!src || !out)
        return;
    memset(out, 0, sizeof(*out));
    memcpy(out->magic, src, 4);
    out->version = zelLe16(src + 4);
    out->headerSize = zelLe16(src + 6);
    out->width = zelLe16(src + 8);
    out->height = zelLe16(src + 0x0A);
    out->zoneWidth = zelLe16(src + 0x0C);
    out->zoneHeight = zelLe16(src + 0x0E);
    out->colorFormat = src[0x10];
    uint8_t f = src[0x11];
    out->flags.hasGlobalPalette = (f & 0x01u) != 0;
    out->flags.hasFrameLocalPalettes = (f & 0x02u) != 0;
    out->flags.hasFrameIndexTable = (f & 0x04u) != 0;
    out->flags.reserved = (uint8_t)((f >> 3) & 0x1Fu);
    out->frameCount = zelLe32(src + 0x12);
    out->defaultFrameDuration = zelLe16(src + 0x16);
    memcpy(out->reserved, src + 0x18, sizeof(out->reserved));
}

void zelParsePaletteHeader(const uint8_t *src, ZELPaletteHeader *out) {
    if (!src || !out)
        return;
    memset(out, 0, sizeof(*out));
    out->type = src[0];
    out->headerSize = src[1];
    out->entryCount = zelLe16(src + 2);
    out->colorEncoding = src[4];
    memcpy(out->reserved, src + 5, sizeof(out->reserved));
}

void zelParseFrameHeader(const uint8_t *src, ZELFrameHeader *out) {
    if (!src || !out)
        return;
    memset(out, 0, sizeof(*out));
    out->blockType = src[0];
    out->headerSize = src[1];
    uint8_t f = src[2];
    out->flags.keyframe = (f & 0x01u) != 0;
    out->flags.hasLocalPalette = (f & 0x02u) != 0;
    out->flags.usePreviousFrameAsBase = (f & 0x04u) != 0;
    out->flags.reserved = (uint8_t)((f >> 3) & 0x1Fu);
    out->zoneCount = zelLe16(src + 3);
    out->compressionType = src[5];
    out->referenceFrameIndex = zelLe16(src + 6);
    out->localPaletteEntryCount = zelLe16(src + 8);
    memcpy(out->reserved, src + 0x0A, sizeof(out->reserved));
}

void zelParseFrameIndexEntry(const uint8_t *src, ZELFrameIndexEntry *out) {
    if (!src || !out)
        return;
    memset(out, 0, sizeof(*out));
    out->frameOffset = zelLe32(src + 0);
    out->frameSize = zelLe32(src + 4);
    uint8_t f = src[8];
    out->flags.keyframe = (f & 0x01u) != 0;
    out->flags.hasLocalPalette = (f & 0x02u) != 0;
    out->flags.usePreviousFrameAsBase = (f & 0x04u) != 0;
    out->flags.reserved = (uint8_t)((f >> 3) & 0x1Fu);
    out->frameDuration = zelLe16(src + 9);
}
