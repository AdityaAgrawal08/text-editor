CC := gcc
CSTD := -std=c11
WARN := -Wall -Wextra
SANITIZE ?=
OPT ?= -O2 -g

SDL_CFLAGS := $(shell pkg-config --cflags sdl2)
SDL_LIBS   := $(shell pkg-config --libs sdl2)
FT_CFLAGS  := $(shell pkg-config --cflags freetype2)
FT_LIBS    := $(shell pkg-config --libs freetype2)

INCLUDE := -Iinclude
DEPFLAGS := -MMD -MP
# Recursively expanded so target-specific overrides (debug/test) reach the
# recipes; immediate (:=) expansion here is what silently disabled sanitizers.
CFLAGS  = $(CSTD) $(WARN) $(OPT) $(INCLUDE) $(SDL_CFLAGS) $(FT_CFLAGS) $(SANITIZE)
LDFLAGS = $(SDL_LIBS) $(FT_LIBS) -lm $(SANITIZE)

BUILD_DIR := build
SRC_DIR   := src

EDITOR_BIN       := $(BUILD_DIR)/editor
TEST_STORAGE_BIN := $(BUILD_DIR)/test_storage
FUZZ_BIN         := $(BUILD_DIR)/fuzz_harness
EDOC_BIN         := $(BUILD_DIR)/edoc

# Editor sources (all .c files except the test harness)
EDITOR_SRCS := \
	$(SRC_DIR)/storage.c      \
	$(SRC_DIR)/language.c     \
	$(SRC_DIR)/formatter.c    \
	$(SRC_DIR)/save_pipeline.c \
	$(SRC_DIR)/editor.c

EDITOR_OBJS := $(EDITOR_SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)

TEST_STORAGE_SRCS := $(SRC_DIR)/storage.c $(SRC_DIR)/test_storage.c
TEST_STORAGE_OBJS := $(TEST_STORAGE_SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/test_%.o)

# Fuzz harness: storage.c compiled with the -DSTORAGE_FUZZING shim
FUZZ_SRCS := $(SRC_DIR)/storage.c $(SRC_DIR)/fuzz_harness.c
FUZZ_OBJS := $(FUZZ_SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/fuzz_%.o)

# edoc CLI: storage.o + cli front-end only (no SDL/FreeType)
EDOC_OBJS := $(BUILD_DIR)/storage.o $(BUILD_DIR)/edoc_cli.o

# libFuzzer variant (clang only): coverage-guided, same shim + entry macro
LF_CFLAGS := $(CSTD) $(WARN) -g -O1 $(INCLUDE) -DSTORAGE_FUZZING -DLIBFUZZER \
             -fsanitize=fuzzer-no-link,fuzzer
LF_OBJS   := $(BUILD_DIR)/lf_storage.o $(BUILD_DIR)/lf_fuzz_harness.o

ifneq (,$(findstring clang,$(CC)))
LF_ENABLED := 1
endif

.PHONY: all clean test run debug fuzz fuzz-libfuzzer

all: $(EDITOR_BIN) $(EDOC_BIN)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD_DIR)/test_%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CSTD) $(WARN) -g -O0 $(INCLUDE) $(SANITIZE) $(DEPFLAGS) \
		-DSTORAGE_FUZZING -c $< -o $@

$(BUILD_DIR)/fuzz_%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CSTD) $(WARN) -g -O1 $(INCLUDE) $(SANITIZE) $(DEPFLAGS) \
		-DSTORAGE_FUZZING -c $< -o $@

$(FUZZ_BIN): $(FUZZ_OBJS) | $(BUILD_DIR)
	$(CC) $(CSTD) $(WARN) -g -O1 $(SANITIZE) $(FUZZ_OBJS) -o $@ -lm

$(EDOC_BIN): $(EDOC_OBJS) | $(BUILD_DIR)
	$(CC) $(CSTD) $(WARN) $(OPT) $(SANITIZE) $(EDOC_OBJS) -o $@ $(LDFLAGS)

# N and SEED are overridable: make fuzz N=5000000 SEED=7
N ?= 1000000
SEED ?= 42
fuzz: SANITIZE := -fsanitize=address,undefined
fuzz: $(FUZZ_BIN)
	./$(FUZZ_BIN) all $(N) $(SEED)

# Coverage-guided variant; requires clang (-fsanitize=fuzzer is clang-only).
ifneq (,$(findstring clang,$(CC)))
.PHONY: fuzz-libfuzzer
fuzz-libfuzzer: $(BUILD_DIR)/fuzz_libfuzzer

$(BUILD_DIR)/fuzz_libfuzzer: $(LF_OBJS)
	$(CC) $(LF_CFLAGS) $(LF_OBJS) -o $@

$(BUILD_DIR)/lf_%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(LF_CFLAGS) $(DEPFLAGS) -c $< -o $@
else
.PHONY: fuzz-libfuzzer
fuzz-libfuzzer:
	$(error fuzz-libfuzzer requires clang: run 'make fuzz-libfuzzer CC=clang')
endif

$(EDITOR_BIN): $(EDITOR_OBJS) | $(BUILD_DIR)
	$(CC) $(EDITOR_OBJS) -o $@ $(LDFLAGS)

$(TEST_STORAGE_BIN): $(TEST_STORAGE_OBJS) | $(BUILD_DIR)
	$(CC) $(CSTD) $(WARN) -g -O0 $(SANITIZE) $(TEST_STORAGE_OBJS) -o $@ -lm $(SANITIZE)

test: SANITIZE := -fsanitize=address,undefined
test: $(TEST_STORAGE_BIN)
	./$(TEST_STORAGE_BIN)

debug: SANITIZE := -fsanitize=address,undefined
debug: OPT := -O0 -g
debug: clean $(EDITOR_BIN)

run: $(EDITOR_BIN)
	./$(EDITOR_BIN)

clean:
	rm -rf $(BUILD_DIR)

DEPS := $(EDITOR_OBJS:.o=.d) $(TEST_STORAGE_OBJS:.o=.d) \
        $(FUZZ_OBJS:.o=.d) $(LF_OBJS:.o=.d) \
        $(BUILD_DIR)/edoc_cli.d
-include $(DEPS)
