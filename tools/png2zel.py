#!/usr/bin/env python3
import argparse
import os
import struct
from PIL import Image
FILE_HEADER_STRUCT = struct.Struct("<4sHHHHHHBBIH10s")
PALETTE_HEADER_STRUCT = struct.Struct("<BBHB3s")
FRAME_INDEX_ENTRY_STRUCT = struct.Struct("<IIBH")
FRAME_HEADER_STRUCT = struct.Struct("<BBBHBHH4s")

try:
    import imagequant
except ImportError:
    imagequant = None

try:
    from lz4 import block as lz4_block
    from lz4.block import LZ4BlockError
except ImportError:
    lz4_block = None
    LZ4BlockError = None

HEADER_FLAG_GLOBAL_PALETTE = 0x01
HEADER_FLAG_FRAME_LOCAL_PALETTE = 0x02
HEADER_FLAG_FRAME_INDEX_TABLE = 0x04

FRAME_FLAG_KEYFRAME = 0x01
FRAME_FLAG_HAS_LOCAL_PALETTE = 0x02

ZEL_COLOR_FORMAT_INDEXED8 = 0
ZEL_COLOR_RGB565 = 0
ZEL_COMPRESSION_NONE = 0
ZEL_COMPRESSION_LZ4 = 1


def rgb_to_rgb565(r, g, b):
    r5 = (r & 0xF8) >> 3
    g6 = (g & 0xFC) >> 2
    b5 = (b & 0xF8) >> 3
    return (r5 << 11) | (g6 << 5) | b5


def rgb565_to_rgb888(value):
    r5 = (value >> 11) & 0x1F
    g6 = (value >> 5) & 0x3F
    b5 = value & 0x1F
    r = (r5 << 3) | (r5 >> 2)
    g = (g6 << 2) | (g6 >> 4)
    b = (b5 << 3) | (b5 >> 2)
    return r, g, b


def tweak_rgb565(base_value, variant, used_values):
    r = (base_value >> 11) & 0x1F
    g = (base_value >> 5) & 0x3F
    b = base_value & 0x1F

    adjustments = []
    for radius in range(1, 4):
        adjustments.extend([(radius, 0, 0), (-radius, 0, 0)])
        adjustments.extend([(0, radius, 0), (0, -radius, 0)])
        adjustments.extend([(0, 0, radius), (0, 0, -radius)])
        for dr in (-radius, radius):
            for dg in (-radius, radius):
                adjustments.append((dr, dg, 0))
                adjustments.append((dr, 0, dg))
                adjustments.append((0, dr, dg))
        for dr in (-radius, radius):
            for dg in (-radius, radius):
                for db in (-radius, radius):
                    adjustments.append((dr, dg, db))

    if not adjustments:
        adjustments = [(1, 0, 0)]

    length = len(adjustments)
    for offset in range(length):
        dr, dg, db = adjustments[(variant + offset) % length]
        nr = max(0, min(31, r + dr))
        ng = max(0, min(63, g + dg))
        nb = max(0, min(31, b + db))
        candidate = (nr << 11) | (ng << 5) | nb
        if candidate not in used_values:
            used_values.add(candidate)
            return candidate

    for fallback in range(0x10000):
        if fallback not in used_values:
            used_values.add(fallback)
            return fallback


def quantize_image(img):
    if img.mode != "RGB":
        img = img.convert("RGB")

    if imagequant is not None:
        try:
            pal_img = imagequant.quantize_pil_image(
                img,
                dithering_level=0.5,
                max_colors=256,
            )
            if pal_img.mode != "P":
                pal_img = pal_img.convert("P")
            return pal_img
        except RuntimeError:
            pass

    return img.quantize(
        colors=256,
    )


def _collect_frame_paths(input_path):
    if os.path.isdir(input_path):
        frames = []
        index = 0
        while True:
            candidate = os.path.join(input_path, f"{index}.png")
            if not os.path.isfile(candidate):
                break
            frames.append(candidate)
            index += 1
        if not frames:
            raise ValueError(
                "Input directory missing numbered PNG files starting at 0.png."
            )
        if len(frames) != index:
            raise ValueError(
                "Frame sequence has gaps; expected contiguous numbering."
            )
        return frames
    if os.path.isfile(input_path):
        return [input_path]
    raise ValueError("Input path does not exist.")


def _encode_frame(frame_path, expected_size):
    img = Image.open(frame_path).convert("RGBA")
    width, height = img.size

    if expected_size is not None and (width, height) != expected_size:
        raise ValueError(
            f"Frame '{frame_path}' has size {width}x{height}, "
            f"expected {expected_size[0]}x{expected_size[1]}."
        )

    pal_img = quantize_image(img)
    palette = pal_img.getpalette()
    if palette is None:
        raise ValueError(
            f"Palette missing after quantization for '{frame_path}'."
        )
    if len(palette) < 256 * 3:
        palette = palette + [0] * (256 * 3 - len(palette))

    indices = list(pal_img.getdata())
    pixel_data = bytes(indices)
    max_index = max(indices) if indices else 0
    used_entries = max_index + 1 if indices else 1
    if used_entries > 256:
        raise ValueError(
            f"Palette for '{frame_path}' has more than 256 colors."
        )

    palette_rgb565 = []
    for i in range(used_entries):
        base = 3 * i
        r = palette[base]
        g = palette[base + 1]
        b = palette[base + 2]
        palette_rgb565.append(rgb_to_rgb565(r, g, b))

    colors_used = len(set(palette_rgb565))

    return {
        "path": frame_path,
        "width": width,
        "height": height,
        "palette": palette_rgb565,
        "palette_count": used_entries,
        "colors_used": colors_used,
        "pixels": pixel_data,
    }


def png_to_zel(
    input_path,
    output_path,
    default_frame_duration_ms=16,
    zone_width_override=None,
    zone_height_override=None,
    compression="lz4",
):
    frame_paths = _collect_frame_paths(input_path)
    expected_size = None
    frame_infos = []

    for frame_path in frame_paths:
        info = _encode_frame(frame_path, expected_size)
        if expected_size is None:
            expected_size = (info["width"], info["height"])
        frame_infos.append(info)

    width, height = expected_size
    frame_count = len(frame_infos)

    compression_choice = compression.lower()
    compression_map = {
        "none": ZEL_COMPRESSION_NONE,
        "lz4": ZEL_COMPRESSION_LZ4,
    }
    if compression_choice not in compression_map:
        raise ValueError(f"Unsupported compression '{compression}'.")
    compression_type = compression_map[compression_choice]

    if compression_type == ZEL_COMPRESSION_LZ4 and lz4_block is None:
        raise RuntimeError(
            "LZ4 compression requested but the 'lz4' package is not "
            "installed. Install it via 'pip install lz4'."
        )

    if zone_width_override is None:
        zone_width = min(width, 0xFFFF)
    else:
        if not (1 <= zone_width_override <= 0xFFFF):
            raise ValueError("zone width must be between 1 and 65535")
        zone_width = zone_width_override

    if zone_height_override is None:
        zone_height = min(height, 0xFFFF)
    else:
        if not (1 <= zone_height_override <= 0xFFFF):
            raise ValueError("zone height must be between 1 and 65535")
        zone_height = zone_height_override

    if width % zone_width != 0:
        raise ValueError("Image width must be divisible by zone width")
    if height % zone_height != 0:
        raise ValueError("Image height must be divisible by zone height")

    zones_per_row = width // zone_width
    zones_per_col = height // zone_height
    zone_count = zones_per_row * zones_per_col
    if zone_count == 0 or zone_count > 0xFFFF:
        raise ValueError("Zone count must be between 1 and 65535")

    zone_pixel_count = zone_width * zone_height

    magic = b"ZEL0"
    version = 1
    header_size = FILE_HEADER_STRUCT.size
    header_flags = (
        HEADER_FLAG_FRAME_INDEX_TABLE | HEADER_FLAG_FRAME_LOCAL_PALETTE
    )
    frame_count_value = frame_count
    default_frame_duration = default_frame_duration_ms
    file_reserved = b"\x00" * 10

    file_header = FILE_HEADER_STRUCT.pack(
        magic,
        version,
        header_size,
        width,
        height,
        zone_width,
        zone_height,
        ZEL_COLOR_FORMAT_INDEXED8,
        header_flags,
        frame_count_value,
        default_frame_duration,
        file_reserved,
    )

    frame_index_entries = []
    frame_blocks = []
    current_offset = header_size + frame_count * FRAME_INDEX_ENTRY_STRUCT.size

    for index, info in enumerate(frame_infos):
        palette_entries = info["palette"]
        palette_count = info["palette_count"]

        palette_header = PALETTE_HEADER_STRUCT.pack(
            1,
            PALETTE_HEADER_STRUCT.size,
            palette_count,
            ZEL_COLOR_RGB565,
            b"\x00" * 3,
        )
        palette_data = b"".join(
            struct.pack("<H", value) for value in palette_entries
        )

        full_indices = info["pixels"]
        zone_chunks = []
        compressed_total = 0

        for zone_index in range(zone_count):
            zone_x = (zone_index % zones_per_row) * zone_width
            zone_y = (zone_index // zones_per_row) * zone_height
            zone_raw = bytearray(zone_pixel_count)

            for row in range(zone_height):
                src_start = (zone_y + row) * width + zone_x
                src_end = src_start + zone_width
                dst_start = row * zone_width
                zone_raw[dst_start:dst_start + zone_width] = (
                    full_indices[src_start:src_end]
                )

            chunk_payload = bytes(zone_raw)
            if compression_type == ZEL_COMPRESSION_LZ4:
                try:
                    chunk_payload = lz4_block.compress(
                        chunk_payload,
                        store_size=False,
                    )
                except LZ4BlockError as exc:
                    raise RuntimeError(
                        f"Failed to compress frame {index} with LZ4"
                    ) from exc

            zone_chunks.append(
                struct.pack("<I", len(chunk_payload)) + chunk_payload
            )
            compressed_total += len(chunk_payload)

        frame_flags = FRAME_FLAG_KEYFRAME | FRAME_FLAG_HAS_LOCAL_PALETTE
        frame_header = FRAME_HEADER_STRUCT.pack(
            1,
            FRAME_HEADER_STRUCT.size,
            frame_flags,
            zone_count,
            compression_type,
            0,
            palette_count,
            b"\x00" * 4,
        )

        frame_bytes = (
            frame_header
            + palette_header
            + palette_data
            + b"".join(zone_chunks)
        )

        frame_size = len(frame_bytes)
        frame_offset = current_offset
        info["compressed_size"] = compressed_total
        info["compression_choice"] = compression_choice
        frame_index_entries.append(
            FRAME_INDEX_ENTRY_STRUCT.pack(
                frame_offset,
                frame_size,
                frame_flags,
                0,
            )
        )
        frame_blocks.append(frame_bytes)
        current_offset += frame_size

    zel_bytes = (
        file_header + b"".join(frame_index_entries) + b"".join(frame_blocks)
    )

    with open(output_path, "wb") as output_file:
        output_file.write(zel_bytes)

    print(f"Wrote ZEL file: {output_path}")
    print(f"  Size: {len(zel_bytes)} bytes")
    print(f"  Compression: {compression_choice}")
    print(
        "  Frames: "
        f"{frame_count}, default duration: {default_frame_duration_ms} ms"
    )
    print(
        "  Image: "
        f"{width}x{height}, zone {zone_width}x{zone_height}"
    )
    for index, info in enumerate(frame_infos):
        print(
            "    Frame "
            f"{index}: colors used {info['colors_used']}, "
            f"palette entries {info['palette_count']}, "
            f"payload {info.get('compressed_size', len(info['pixels']))} bytes"
            f" (raw {len(info['pixels'])} bytes)"
        )


def zel_to_png(input_path, output_path, frame_index=0):
    with open(input_path, "rb") as input_file:
        data = input_file.read()

    if len(data) < FILE_HEADER_STRUCT.size:
        raise ValueError("File too small for ZEL header")

    (
        magic,
        version,
        header_size,
        width,
        height,
        zone_width,
        zone_height,
        color_format,
        header_flags,
        frame_count,
        default_frame_duration,
        _reserved,
    ) = FILE_HEADER_STRUCT.unpack_from(data, 0)

    if magic != b"ZEL0":
        raise ValueError("Invalid signature; not a ZEL file")
    if version != 1:
        raise ValueError(f"Unsupported version {version}")
    if color_format != ZEL_COLOR_FORMAT_INDEXED8:
        raise ValueError("Only INDEXED8 files are supported")
    if not (header_flags & HEADER_FLAG_FRAME_INDEX_TABLE):
        raise ValueError("Frame index table missing; not supported")
    if frame_count == 0:
        raise ValueError("ZEL file does not contain frames")
    if frame_index < 0 or frame_index >= frame_count:
        raise ValueError("Frame index out of range")

    offset = header_size
    global_palette_rgb565 = None

    if header_flags & HEADER_FLAG_GLOBAL_PALETTE:
        if offset + PALETTE_HEADER_STRUCT.size > len(data):
            raise ValueError("File truncated: palette header")
        (
            pal_type,
            pal_header_size,
            pal_entry_count,
            pal_color_encoding,
            _pal_reserved,
        ) = PALETTE_HEADER_STRUCT.unpack_from(data, offset)
        if pal_type != 0:
            raise ValueError("Only global palettes are supported")
        if pal_color_encoding != ZEL_COLOR_RGB565:
            raise ValueError("Only RGB565 palettes are supported")
        offset += pal_header_size
        palette_bytes = pal_entry_count * 2
        if offset + palette_bytes > len(data):
            raise ValueError("File truncated: palette data")
        global_palette_rgb565 = list(
            struct.unpack_from(f"<{pal_entry_count}H", data, offset)
        )
        offset += palette_bytes
    elif not (header_flags & HEADER_FLAG_FRAME_LOCAL_PALETTE):
        raise ValueError(
            "File must provide either a global palette or frame-local palettes"
        )

    index_table_size = frame_count * FRAME_INDEX_ENTRY_STRUCT.size
    if offset + index_table_size > len(data):
        raise ValueError("File truncated: frame index table")

    entry_offset = offset + frame_index * FRAME_INDEX_ENTRY_STRUCT.size
    frame_offset, frame_size, frame_flags, frame_duration = (
        FRAME_INDEX_ENTRY_STRUCT.unpack_from(data, entry_offset)
    )

    if frame_offset + frame_size > len(data):
        raise ValueError("File truncated: frame data")

    (
        block_type,
        frame_header_size,
        frame_block_flags,
        zone_count,
        compression_type,
        reference_frame_index,
        local_palette_entry_count,
        _frame_reserved,
    ) = FRAME_HEADER_STRUCT.unpack_from(data, frame_offset)

    pixel_count = width * height
    pixel_data_offset = frame_offset + frame_header_size
    palette_rgb565_for_frame = global_palette_rgb565

    if frame_block_flags & FRAME_FLAG_HAS_LOCAL_PALETTE:
        if pixel_data_offset + PALETTE_HEADER_STRUCT.size > len(data):
            raise ValueError("File truncated: frame palette header")
        (
            pal_type,
            pal_header_size,
            pal_entry_count,
            pal_color_encoding,
            _pal_reserved,
        ) = PALETTE_HEADER_STRUCT.unpack_from(data, pixel_data_offset)
        if pal_type != 1:
            raise ValueError("Frame palette type must be local (1)")
        if pal_color_encoding != ZEL_COLOR_RGB565:
            raise ValueError("Only RGB565 palettes are supported")
        pixel_data_offset += pal_header_size
        palette_bytes = pal_entry_count * 2
        if pixel_data_offset + palette_bytes > len(data):
            raise ValueError("File truncated: frame palette data")
        palette_rgb565_for_frame = list(
            struct.unpack_from(f"<{pal_entry_count}H", data, pixel_data_offset)
        )
        pixel_data_offset += palette_bytes
        if (
            local_palette_entry_count
            and pal_entry_count != local_palette_entry_count
        ):
            raise ValueError("Frame palette count mismatch")
    elif palette_rgb565_for_frame is None:
        raise ValueError("Frame does not provide a palette")

    if pixel_data_offset > frame_offset + frame_size:
        raise ValueError("File truncated: pixel payload is missing")

    pixel_data_end = frame_offset + frame_size

    if zone_width == 0 or zone_height == 0:
        raise ValueError("Zone dimensions must be non-zero")
    if width % zone_width != 0 or height % zone_height != 0:
        raise ValueError("Image dimensions not divisible by zone size")

    zones_per_row = width // zone_width
    zones_per_col = height // zone_height
    expected_zone_count = zones_per_row * zones_per_col
    if expected_zone_count == 0:
        raise ValueError("Zone grid is empty")
    if zone_count != expected_zone_count:
        raise ValueError("Zone count mismatch in frame header")

    zone_pixel_count = zone_width * zone_height
    zone_offset = pixel_data_offset
    indices = bytearray(pixel_count)

    for zone_index in range(zone_count):
        if zone_offset + 4 > pixel_data_end:
            raise ValueError("File truncated: zone header")
        (chunk_size,) = struct.unpack_from("<I", data, zone_offset)
        zone_offset += 4
        if chunk_size == 0:
            raise ValueError("Zone chunk size is zero")
        if zone_offset + chunk_size > pixel_data_end:
            raise ValueError("File truncated: zone payload")
        chunk_payload = data[zone_offset:zone_offset + chunk_size]
        zone_offset += chunk_size

        if compression_type == ZEL_COMPRESSION_NONE:
            if chunk_size != zone_pixel_count:
                raise ValueError("Zone payload size mismatch")
            zone_pixels = chunk_payload
        elif compression_type == ZEL_COMPRESSION_LZ4:
            if lz4_block is None:
                raise ValueError(
                    "Cannot decode LZ4-compressed frame without the 'lz4' "
                    "package."
                )
            try:
                zone_pixels = lz4_block.decompress(
                    chunk_payload,
                    uncompressed_size=zone_pixel_count,
                    return_bytearray=False,
                )
            except LZ4BlockError as exc:
                raise ValueError("Failed to decompress LZ4 zone") from exc
            if len(zone_pixels) != zone_pixel_count:
                raise ValueError("Zone decompression size mismatch")
        else:
            raise ValueError(
                f"Unsupported compression type {compression_type} in frame"
            )

        zone_x = (zone_index % zones_per_row) * zone_width
        zone_y = (zone_index // zones_per_row) * zone_height
        for row in range(zone_height):
            dest_start = (zone_y + row) * width + zone_x
            dest_end = dest_start + zone_width
            src_start = row * zone_width
            indices[dest_start:dest_end] = zone_pixels[
                src_start:src_start + zone_width
            ]

    if zone_offset != pixel_data_end:
        raise ValueError("Extra data after zone payloads")

    pixel_data = bytes(indices)

    palette_rgb = [
        rgb565_to_rgb888(value) for value in palette_rgb565_for_frame
    ]
    if not palette_rgb:
        raise ValueError("Palette is empty")

    pixels = []
    for idx in pixel_data:
        if idx >= len(palette_rgb):
            raise ValueError("Pixel index outside palette range")
        pixels.append(palette_rgb[idx])

    img = Image.new("RGB", (width, height))
    img.putdata(pixels)
    img.save(output_path, format="PNG")

    print(f"Wrote PNG file: {output_path}")
    print(
        "  Size: "
        f"{width}x{height} pixels, frame {frame_index}, "
        f"duration {default_frame_duration} ms"
    )


def main():
    parser = argparse.ArgumentParser(
        description="Convert between PNG and single-frame ZEL."
    )
    parser.add_argument("input", help="Input file")
    parser.add_argument("output", help="Output file")
    parser.add_argument(
        "--mode",
        choices=["encode", "decode"],
        default="encode",
        help=(
            "Operation mode. 'encode' converts PNG -> ZEL, "
            "'decode' converts ZEL -> PNG."
        ),
    )
    parser.add_argument(
        "--duration",
        type=int,
        default=16,
        help="Frame duration in ms (encode mode, default: 16)",
    )
    parser.add_argument(
        "--zone-width",
        type=int,
        help=(
            "Zone width written to the header (encode mode, 1-65535). "
            "Default: min(image width, 65535)"
        ),
    )
    parser.add_argument(
        "--zone-height",
        type=int,
        help=(
            "Zone height written to the header (encode mode, 1-65535). "
            "Default: min(image height, 65535)"
        ),
    )
    parser.add_argument(
        "--compression",
        choices=["none", "lz4"],
        default="lz4",
        help=(
            "Compression format for frame data in encode mode. "
            "Default: lz4"
        ),
    )
    parser.add_argument(
        "--frame",
        type=int,
        default=0,
        help="Frame index to decode (decode mode only).",
    )
    args = parser.parse_args()

    if args.mode == "encode":
        png_to_zel(
            args.input,
            args.output,
            args.duration,
            zone_width_override=args.zone_width,
            zone_height_override=args.zone_height,
            compression=args.compression,
        )
    else:
        zel_to_png(args.input, args.output, frame_index=args.frame)


if __name__ == "__main__":
    main()
