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
CFLAGS  := $(CSTD) $(WARN) $(OPT) $(INCLUDE) $(SDL_CFLAGS) $(FT_CFLAGS) $(SANITIZE)
LDFLAGS := $(SDL_LIBS) $(FT_LIBS) -lm $(SANITIZE)

BUILD_DIR := build
SRC_DIR   := src

EDITOR_BIN       := $(BUILD_DIR)/editor
TEST_STORAGE_BIN := $(BUILD_DIR)/test_storage

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

.PHONY: all clean test run debug

all: $(EDITOR_BIN)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/test_%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CSTD) $(WARN) -g -O0 $(INCLUDE) $(SANITIZE) -c $< -o $@

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
