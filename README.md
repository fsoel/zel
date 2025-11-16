# zel

ZEL is a C library that targets microcontrollers with a speed-optimised format for animated images. It provides functionality to decode and render animated images with low memory footprint and high performance, making it suitable for resource-constrained environments.

## Layout

- `include/` - public headers
- `src/` - library sources
- `tests/` - unit and integration tests
- `examples/` - sample applications
- `docs/` - documentation assets
- `tools/` - helper scripts and utilities

## Build

The project uses `make` together with `gcc` to build a static library. Run `make` to produce `build/libzel.a` once source files are implemented. Use `make clean` to remove build artefacts.

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
