# ZEL File Format (v0.1)

This document defines the on-disk layout of ZEL. All integer fields are little-endian and structures are byte-packed.

## Conventions
- Offsets are relative to the start of the file unless stated otherwise.
- Sizes are in bytes. All multi-byte integer fields are unsigned.
- `zoneIndex` increases row-major: `zoneIndex = y * zonesPerRow + x`.

## Top-Level Layout
1. FileHeader
2. Optional Global Palette block (only if FileHeader.flags.hasGlobalPalette)
3. Frame Index Table (required; FileHeader.flags.hasFrameIndexTable)
4. Frame Blocks (one per frame), each located at its frameOffset

## FileHeader (headerSize bytes; typically 34)
| Offset | Size | Field | Description |
| --- | --- | --- | --- |
| 0x00 | 4 | magic | ASCII "ZEL0" |
| 0x04 | 2 | version | Format version (must be 1) |
| 0x06 | 2 | headerSize | Bytes from file start to the first block after the header |
| 0x08 | 2 | width | Frame width in pixels |
| 0x0A | 2 | height | Frame height in pixels |
| 0x0C | 2 | zoneWidth | Zone width; must divide width |
| 0x0E | 2 | zoneHeight | Zone height; must divide height |
| 0x10 | 1 | colorFormat | `ZELColorFormat` |
| 0x11 | 1 | flags | `ZELHeaderFlags` |
| 0x12 | 4 | frameCount | Number of frames |
| 0x16 | 2 | defaultFrameDuration | Milliseconds; used when per-frame duration is 0 |
| 0x18 | 10 | reserved | Must be zero |

### ZELColorFormat
- INDEXED8 (0)

### ZELHeaderFlags
- bit0: hasGlobalPalette
- bit1: hasFrameLocalPalettes
- bit2: hasFrameIndexTable (required by current decoder)
- bits3–7: reserved (zero)

## PaletteHeader (headerSize bytes; typically 8)
Used for both global and local palettes; immediately followed by `entryCount` uint16 entries in the declared `colorEncoding`.

| Offset | Size | Field | Description |
| --- | --- | --- | --- |
| 0x00 | 1 | type | `ZELPaletteType` |
| 0x01 | 1 | headerSize | Bytes for this header (>= 8) |
| 0x02 | 2 | entryCount | Number of palette entries |
| 0x04 | 1 | colorEncoding | `ZELColorEncoding` |
| 0x05 | 3 | reserved | Must be zero |

### ZELPaletteType
- global (0)
- local (1)

### ZELColorEncoding
- RGB565 LE (0)
- RGB565 BE (1)

## Global Palette Block (optional)
Present only if FileHeader.flags.hasGlobalPalette is set. Layout:
1. PaletteHeader (type = GLOBAL)
2. `entryCount` entries in `colorEncoding` (with `entryCount * 2` bytes)

## Frame Index Table
Immediately follows the header and optional global palette. Contains `frameCount` entries.

### FrameIndexEntry (11 bytes)
| Offset | Size | Field | Description |
| --- | --- | --- | --- |
| 0x00 | 4 | frameOffset | Absolute byte offset to the frame block |
| 0x04 | 4 | frameSize | Total bytes of the frame block |
| 0x08 | 1 | flags | `ZELFrameFlags` |
| 0x09 | 2 | frameDuration | Milliseconds; 0 uses FileHeader.defaultFrameDuration |

#### ZELFrameFlags
- bit0: keyframe
- bit1: hasLocalPalette
- bit2: usePreviousFrameAsBase
- bits3–7: reserved (zero)

## Frame Block
Located at frameOffset and spans frameSize bytes. Layout:
1. FrameHeader
2. Optional Local Palette block (if FrameHeader.flags.hasLocalPalette)
3. Zone chunk stream (exactly `zoneCount` chunks)

### FrameHeader (headerSize bytes; typically 14)
| Offset | Size | Field | Description |
| --- | --- | --- | --- |
| 0x00 | 1 | blockType | Must be 1 |
| 0x01 | 1 | headerSize | Bytes for this header (>= 14) |
| 0x02 | 1 | flags | `ZELFrameFlags` |
| 0x03 | 2 | zoneCount | Must equal (width/zoneWidth) × (height/zoneHeight) |
| 0x05 | 1 | compressionType | `ZELCompressionType` |
| 0x06 | 2 | referenceFrameIndex | For differential use; unused by current encoder/decoder |
| 0x08 | 2 | localPaletteEntryCount | Mirrors the following local palette entryCount when present |
| 0x0A | 4 | reserved | Must be zero |

#### ZELFrameFlags
- bit0: keyframe
- bit1: hasLocalPalette
- bit2: usePreviousFrameAsBase
- bits3–7: reserved (zero)

#### ZELCompressionType
- NONE (0)
- LZ4 (1)
- RLE (2) (reserved; not currently used)

### Local Palette Block (optional)
Present only when FrameHeader.flags.hasLocalPalette is set. Layout:
1. PaletteHeader (type = LOCAL)
2. `entryCount` entries in `colorEncoding` (with `entryCount * 2` bytes)

### Zone Chunk Stream
Appears after the optional local palette. Contains exactly `zoneCount` chunks in zone index order (row-major).

Each zone chunk
- 4 bytes: chunkSize (uint32, must be > 0)
- chunkSize bytes: payload

Payload interpretation
- If compressionType == NONE: payload is exactly `zoneWidth × zoneHeight` bytes of 8-bit indices.
- If compressionType == LZ4: payload is an LZ4 block (no embedded length) that inflates to `zoneWidth × zoneHeight` bytes.

After decoding all chunks, the cursor must equal frameOffset + frameSize; extra bytes indicate corruption.

Coordinate Mapping
------------------
- zonesPerRow = width / zoneWidth; zonesPerCol = height / zoneHeight.
- For a given zoneIndex: x = (zoneIndex % zonesPerRow) * zoneWidth; y = (zoneIndex / zonesPerRow) * zoneHeight.
- Within each zone, rows are stored top-to-bottom, left-to-right.

Validation Summary
------------------
- magic == "ZEL0"; version == 1.
- zone dimensions are non-zero and evenly divide width/height; zoneCount between 1 and 65535.
- frameCount > 0; frameSize > 0; chunkSize > 0.
- Palette entryCount > 0; reserved bytes are zero; only supported colorFormat is INDEXED8.
