CC = gcc

CFLAGS = -Wall -Wextra -Iinclude

LDFLAGS = -lSDL2

SRC = $(wildcard src/*.c)

OUT = build/editor

all:
	mkdir -p build
	$(CC) $(CFLAGS) $(SRC) -o $(OUT) $(LDFLAGS)

run: all
	./$(OUT)

clean:
	rm -rf build
