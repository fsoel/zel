#ifndef ZEL_ZEL_H
#define ZEL_ZEL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#pragma pack(push, 1)

/* Enums */

typedef enum { ZEL_COLOR_FORMAT_INDEXED8 = 0 } ZELColorFormat;

typedef enum {
    ZEL_COMPRESSION_NONE = 0,
    ZEL_COMPRESSION_LZ4 = 1,
    ZEL_COMPRESSION_RLE = 2
} ZELCompressionType;

typedef enum { ZEL_COLOR_RGB565_LE = 0, ZEL_COLOR_RGB565_BE = 1 } ZELColorEncoding;

typedef enum { ZEL_PALETTE_TYPE_GLOBAL = 0, ZEL_PALETTE_TYPE_LOCAL = 1 } ZELPaletteType;

typedef enum {
    ZEL_OK = 0,
    ZEL_ERR_INVALID_ARGUMENT,
    ZEL_ERR_INVALID_MAGIC,
    ZEL_ERR_UNSUPPORTED_VERSION,
    ZEL_ERR_UNSUPPORTED_FORMAT,
    ZEL_ERR_CORRUPT_DATA,
    ZEL_ERR_OUT_OF_MEMORY,
    ZEL_ERR_OUT_OF_BOUNDS,
    ZEL_ERR_IO,
    ZEL_ERR_INTERNAL
} ZELResult;

/* On-disk structs */

typedef struct {
    uint8_t hasGlobalPalette : 1;
    uint8_t hasFrameLocalPalettes : 1;
    uint8_t hasFrameIndexTable : 1;
    uint8_t reserved : 5;
} ZELHeaderFlags;

typedef struct {
    uint8_t keyframe : 1;
    uint8_t hasLocalPalette : 1;
    uint8_t usePreviousFrameAsBase : 1;
    uint8_t reserved : 5;
} ZELFrameFlags;

typedef struct {
    char magic[4];       /* "ZEL0" */
    uint16_t version;    /* e.g. 0x0001 */
    uint16_t headerSize; /* sizeof(ZELFileHeader) */
    uint16_t width;
    uint16_t height;
    uint16_t zoneWidth;
    uint16_t zoneHeight;
    uint8_t colorFormat; /* ZELColorFormat */
    ZELHeaderFlags flags;
    uint32_t frameCount;
    uint16_t defaultFrameDuration;
    uint8_t reserved[10];
} ZELFileHeader;

typedef struct {
    uint32_t frameOffset;
    uint32_t frameSize;
    ZELFrameFlags flags;
    uint16_t frameDuration;
} ZELFrameIndexEntry;

typedef struct {
    uint8_t blockType;
    uint8_t headerSize;
    ZELFrameFlags flags;
    uint16_t zoneCount;
    uint8_t compressionType; /* ZELCompressionType */
    uint16_t referenceFrameIndex;
    uint16_t localPaletteEntryCount;
    uint8_t reserved[4];
} ZELFrameHeader;

typedef struct {
    uint8_t type; /* ZELPaletteType */
    uint8_t headerSize;
    uint16_t entryCount;
    uint8_t colorEncoding; /* ZELColorEncoding */
    uint8_t reserved[3];
} ZELPaletteHeader;

#pragma pack(pop)

typedef struct ZELContext ZELContext;

typedef size_t (*ZELStreamReadFunc)(void *userData, size_t offset, void *dst, size_t size);
typedef void (*ZELStreamCloseFunc)(void *userData);

typedef struct {
    ZELStreamReadFunc read;
    ZELStreamCloseFunc close;
    void *userData;
    size_t size;
} ZELInputStream;

ZELContext *zelOpenMemory(const uint8_t *data, size_t size, ZELResult *outResult);
ZELContext *zelOpenStream(const ZELInputStream *stream, ZELResult *outResult);

void zelClose(ZELContext *ctx);

uint16_t zelGetWidth(const ZELContext *ctx);
uint16_t zelGetHeight(const ZELContext *ctx);
uint32_t zelGetFrameCount(const ZELContext *ctx);
uint16_t zelGetDefaultFrameDurationMs(const ZELContext *ctx);
uint16_t zelGetZoneWidth(const ZELContext *ctx);
uint16_t zelGetZoneHeight(const ZELContext *ctx);
ZELColorFormat zelGetColorFormat(const ZELContext *ctx);

void zelSetOutputColorEncoding(ZELContext *ctx, ZELColorEncoding encoding);
ZELColorEncoding zelGetOutputColorEncoding(const ZELContext *ctx);

int zelHasGlobalPalette(const ZELContext *ctx);

ZELResult zelGetGlobalPalette(const ZELContext *ctx,
                              const uint16_t **outEntries,
                              uint16_t *outCount);

ZELResult zelGetFramePalette(const ZELContext *ctx,
                             uint32_t frameIndex,
                             const uint16_t **outEntries,
                             uint16_t *outCount);

ZELResult zelGetFrameDurationMs(const ZELContext *ctx,
                                uint32_t frameIndex,
                                uint16_t *outDurationMs);

ZELResult zelGetFrameIsKeyframe(const ZELContext *ctx, uint32_t frameIndex, int *outIsKeyframe);

ZELResult zelGetFrameUsesLocalPalette(const ZELContext *ctx,
                                      uint32_t frameIndex,
                                      int *outUsesLocalPalette);

ZELResult zelDecodeFrameIndex8(const ZELContext *ctx,
                               uint32_t frameIndex,
                               uint8_t *dst,
                               size_t dstStrideBytes);

ZELResult zelDecodeFrameIndex8Zone(const ZELContext *ctx,
                                   uint32_t frameIndex,
                                   uint32_t zoneIndex,
                                   uint8_t *dst);

ZELResult zelDecodeFrameRgb565(const ZELContext *ctx,
                               uint32_t frameIndex,
                               uint16_t *dst,
                               size_t dstStridePixels);

ZELResult zelDecodeFrameRgb565Zone(const ZELContext *ctx,
                                   uint32_t frameIndex,
                                   uint32_t zoneIndex,
                                   uint16_t *dst);

ZELResult zelGetTotalDurationMs(const ZELContext *ctx, uint32_t *outTotalDurationMs);

ZELResult zelFindFrameByTimeMs(const ZELContext *ctx,
                               uint32_t timeMs,
                               uint32_t *outFrameIndex,
                               uint32_t *outFrameStartMs);

const char *zelResultToString(ZELResult result);

#ifdef __cplusplus
}
#endif

#endif /* ZEL_ZEL_H */
