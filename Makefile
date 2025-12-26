# Zel build configuration
CC ?= gcc
AR ?= ar
ARFLAGS ?= rcs
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2
CPPFLAGS ?= -Iinclude -Isrc -Ilib
CLANG_FORMAT ?= clang-format
MKDIR_P ?= mkdir -p
RM := rm -rf

SRC := $(wildcard src/*.c) $(wildcard lib/lz4/*.c)
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

.PHONY: all clean test lint format dirs amalgamate single

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
