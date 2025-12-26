# ZEL

ZEL is a C library that targets microcontrollers with a speed-optimised format for animated images. It provides functionality to decode and render animated images with low memory footprint and high performance, making it suitable for resource-constrained environments.

## Premise

Traditional image formats like GIF and APNG are often not optimized for the constraints of microcontrollers, leading to high memory usage and slow rendering times. ZEL addresses these issues by providing a format and library specifically designed for efficient animation handling on such devices. It prioritizes decoding speed over file size, making it ideal for applications where performance is critical.

The format is flexible and allows for various compression methods, including uncompressed frames and LZ4 compression. To reduce memory usage during decoding, frame data can be tiled into zones, allowing only parts of the frame to be decompressed at a time.

## Build

The project uses `make` together with `gcc` to build a static library. Run `make` to produce `build/libzel.a`.

```
make
make clean
make lint
make format
```

## Usage

Include the main header in your source files:
```c
#include "zel/zel.h"
```
Link against the static library `libzel.a` when compiling your application.

## Tools

The `tools/` directory contains helper scripts for working with ZEL files. For example, `png2zel.py` can convert PNG images (or directories of numbered PNG files) to ZEL format (and vice versa).

## Testing

```
make test
```

On success, the output will be:
```
All ZEL tests passed.
```
