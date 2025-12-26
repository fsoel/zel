# Streaming from Files or SD Cards

If you cannot load the entire ZEL file into memory, create a `ZELInputStream` that exposes
random-access reads. The library caches the global palette and frame index table once and,
for each decoded frame, issues a single read that copies the compressed frame block into RAM
before processing its zones.

```c
typedef struct {
	FILE *file;
} FileStreamCtx;

static size_t file_stream_read(void *userData, size_t offset, void *dst, size_t size) {
	FileStreamCtx *ctx = (FileStreamCtx *)userData;
	if (fseek(ctx->file, (long)offset, SEEK_SET) != 0)
		return 0;
	return fread(dst, 1, size, ctx->file);
}

static void file_stream_close(void *userData) {
	FileStreamCtx *ctx = (FileStreamCtx *)userData;
	fclose(ctx->file);
}

FileStreamCtx fs = {sdCardFile};
ZELInputStream stream = {
	.read = file_stream_read,
	.close = file_stream_close, /* optional */
	.userData = &fs,
	.size = totalZelFileBytes
};

ZELResult res = ZEL_OK;
ZELContext *ctx = zelOpenStream(&stream, &res);
```

The `read` callback must return exactly the number of bytes requested or zero on error, and the
`size` field must describe the total accessible byte count in the stream. Set `close` to `NULL` if
you prefer to manage the underlying handle yourself.