# Zel build configuration
CC ?= gcc
AR ?= ar
ARFLAGS ?= rcs
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2
CPPFLAGS ?= -Iinclude
CLANG_FORMAT ?= clang-format
MKDIR_P ?= mkdir -p
RM ?= rm -rf

SRC := $(wildcard src/*.c) $(wildcard lib/lz4/*.c)
OBJ := $(patsubst %.c,build/%.o,$(SRC))
LIB := build/libzel.a

rwildcard = $(foreach d,$(wildcard $1*/),$(call rwildcard,$d,$2)) $(wildcard $1$2)
HEADERS := $(call rwildcard,include/,*.h)
FMT_FILES := $(sort $(SRC) $(HEADERS))
TEST_SRC := $(wildcard tests/*.c)
TEST_OBJ := $(patsubst tests/%.c,build/tests/%.o,$(TEST_SRC))
TEST_BIN := $(patsubst tests/%.c,build/tests/%,$(TEST_SRC))

.PHONY: all clean test lint format dirs

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
