# Zel build configuration
CC ?= gcc
AR ?= ar
ARFLAGS ?= rcs
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2
CPPFLAGS ?= -Iinclude -Isrc -Ilib
CLANG_FORMAT ?= clang-format
SCAN_BUILD ?= scan-build
SCAN_FLAGS ?= --status-bugs
SCAN_EXCLUDES ?= --exclude 'lib/*'
MSVC_OUTDIR ?= build/msvc
MSVC_RSP ?= $(MSVC_OUTDIR)/sources.rsp
MSVC_CL ?= cl
MSVC_LIB ?= lib
MSVC_CLFLAGS ?= /nologo /std:c11 /W4 /O2 /Iinclude
MKDIR_P ?= mkdir -p
RM := rm -rf

SRC := $(wildcard src/*.c) $(wildcard lib/lz4/*.c)
SRC_WIN := $(subst /,\\,$(SRC))
AMALG := build/zel.c
AMALG_PARTS := $(wildcard src/*.c) $(wildcard lib/lz4/*.c)
AMALG_HEADERS := src/zel_internal.h include/lz4/lz4.h
OBJ := $(patsubst %.c,build/%.o,$(SRC))
LIB := build/libzel.a

rwildcard = $(foreach d,$(wildcard $1*/),$(call rwildcard,$d,$2)) $(wildcard $1$2)
TEST_SRC := $(wildcard tests/*.c)
TEST_OBJ := $(patsubst tests/%.c,build/tests/%.o,$(TEST_SRC))
TEST_BIN := $(patsubst tests/%.c,build/tests/%,$(TEST_SRC))
HEADERS := $(call rwildcard,include/,*.h) $(call rwildcard,tests/,*.h) src/zel_internal.h
FMT_FILES := $(sort $(SRC) $(HEADERS) $(TEST_SRC))

.PHONY: all clean test lint format scan msvc dirs amalgamate single

all: $(LIB)

$(LIB): $(OBJ)
	@$(MKDIR_P) $(dir $@)
	$(AR) $(ARFLAGS) $@ $^

build/%.o: %.c | dirs
	@$(MKDIR_P) $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

build/tests/%.o: tests/%.c | dirs
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

build/tests/%: build/tests/%.o $(LIB)
	$(CC) $^ -o $@

dirs:
	@$(MKDIR_P) build build/tests

clean:
	$(RM) build

test:
ifeq ($(strip $(TEST_BIN)),)
	@echo "No tests have been defined yet."
else
	@$(MAKE) $(TEST_BIN)
	@for t in $(TEST_BIN); do \
		echo "Running $$t"; \
		$$t || exit $$?; \
	done
endif

lint:
ifeq ($(strip $(FMT_FILES)),)
	@echo "No files to lint."
else
	@$(CLANG_FORMAT) --version >/dev/null 2>&1 || (echo "clang-format not found." && exit 1)
	@$(CLANG_FORMAT) --dry-run -Werror $(FMT_FILES)
endif

format:
ifeq ($(strip $(FMT_FILES)),)
	@echo "No files to format."
else
	@$(CLANG_FORMAT) -i $(FMT_FILES)
endif

scan:
	@$(SCAN_BUILD) $(SCAN_FLAGS) $(SCAN_EXCLUDES) $(MAKE) clean all

msvc:
	@$(MKDIR_P) $(MSVC_OUTDIR)
	@printf "%s\n" $(SRC_WIN) > $(MSVC_RSP)
	@powershell -NoLogo -NoProfile -Command \
		"\$$vsPath = & \"C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer\\vswhere.exe\" -latest -property installationPath; \
		if (\$$vsPath) { \
			Import-Module \"\$$vsPath\\Common7\\Tools\\Microsoft.VisualStudio.DevShell.dll\"; \
			Enter-VsDevShell -VsInstallPath \$$vsPath -SkipAutomaticLocation -Arch amd64 -ErrorAction SilentlyContinue; \
		} \
		$(MSVC_CL) $(MSVC_CLFLAGS) /Fo$(MSVC_OUTDIR)\\ /c @$(MSVC_RSP); \
		$(MSVC_LIB) /nologo /OUT:$(MSVC_OUTDIR)\\zel.lib $(MSVC_OUTDIR)\\*.obj;"

amalgamate: $(AMALG)
single: $(AMALG)

$(AMALG): $(AMALG_PARTS) $(AMALG_HEADERS) | dirs
	@$(MKDIR_P) $(dir $@)
	@printf "/* Auto-generated single-file amalgamation. Do not edit directly. */\n" > $@
	@printf "/* Source files: $(AMALG_PARTS) */\n" >> $@
	@printf "/* Internal header */\n" >> $@
	@cat src/zel_internal.h >> $@
	@printf "\n/* LZ4 public header (dependency) */\n" >> $@
	@cat include/lz4/lz4.h >> $@
	@for f in $(AMALG_PARTS); do \
		printf "\n/* BEGIN %s */\n" "$$f" >> $@; \
		sed -e '/^#include[[:space:]]*"zel_internal.h"/d' \
		    -e '/^#include[[:space:]]*".*lz4.h"/d' "$$f" >> $@; \
		printf "\n/* END %s */\n" "$$f" >> $@; \
	done
