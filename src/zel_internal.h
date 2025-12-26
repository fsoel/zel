#ifndef ZEL_INTERNAL_H
#define ZEL_INTERNAL_H

#include "zel/zel.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint16_t zoneWidth;
    uint16_t zoneHeight;
    uint32_t zonesPerRow;
    uint32_t zonesPerCol;
    uint32_t zoneCount;
    size_t zonePixelBytes;
} ZELZoneLayout;

typedef struct {
    ZELFrameHeader header;
    size_t frameOffset;
    size_t frameSize;
    size_t zoneDataOffset;
    size_t frameDataEnd;
    ZELZoneLayout layout;
    const uint8_t *frameData;
} ZELFrameZoneStream;

struct ZELContext {
    const uint8_t *data;
    size_t size;

    ZELInputStream stream;

    ZELFileHeader header;

    const ZELFrameIndexEntry *frameIndexTable;
    ZELFrameIndexEntry *frameIndexOwned;
    const uint16_t *globalPaletteRaw;
    uint16_t *globalPaletteOwned;
    uint16_t *globalPaletteConverted;
    size_t globalPaletteConvertedCapacity;
    uint16_t globalPaletteCount;
    ZELColorEncoding globalPaletteEncoding;
    ZELColorEncoding globalPaletteConvertedEncoding;

    int hasCustomOutputEncoding;
    ZELColorEncoding outputColorEncoding;

    uint8_t *zoneScratch;
    size_t zoneScratchCapacity;
    uint8_t *frameDataScratch;
    size_t frameDataScratchCapacity;
    uint16_t *paletteScratch;
    size_t paletteScratchCapacity;
};

int zelIsValidColorEncoding(uint8_t encoding);
uint16_t zelSwapRgb565(uint16_t value);
int zelRangeFits(size_t offset, size_t length, size_t limit);
ZELResult zelReadAt(const ZELContext *ctx, size_t offset, void *dst, size_t length);
uint8_t *zelAcquireZoneScratch(const ZELContext *ctx, size_t neededBytes);
uint16_t *zelAcquirePaletteScratch(const ZELContext *ctx, size_t neededEntries);
ZELColorEncoding zelSelectOutputEncoding(const ZELContext *ctx, ZELColorEncoding sourceEncoding);

#endif /* ZEL_INTERNAL_H */
