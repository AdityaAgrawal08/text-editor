CC = gcc

CFLAGS = -Wall -Wextra -Iinclude $(shell pkg-config --cflags sdl2 freetype2)

LDFLAGS = $(shell pkg-config --libs sdl2 freetype2)

SRC = $(wildcard src/*.c)

OUT = build/editor

all:
	mkdir -p build
	$(CC) $(CFLAGS) $(SRC) -o $(OUT) $(LDFLAGS)

run: all
	./$(OUT)

clean:
	rm -rf build
