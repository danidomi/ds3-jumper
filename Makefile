# ds3-jumper — builds on Linux, macOS and other Unix systems with SDL2.
CC      ?= cc
SDL_CFLAGS := $(shell pkg-config --cflags sdl2 2>/dev/null || sdl2-config --cflags)
SDL_LIBS   := $(shell pkg-config --libs sdl2 2>/dev/null || sdl2-config --libs)
CFLAGS  ?= -O2
CFLAGS  += -std=c99 -Wall -Wextra -D_DEFAULT_SOURCE $(SDL_CFLAGS)
LDLIBS  += $(SDL_LIBS) -lm

BIN := ds3-jumper

all: $(BIN)

SRC := src/main.c src/audio.c
HDR := src/audio.h

$(BIN): $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS) $(LDLIBS)

start: $(BIN)
	./$(BIN)

run: start

windowed: $(BIN)
	./$(BIN) --windowed

stats: $(BIN)
	@./$(BIN) --stats

clean:
	rm -f $(BIN) *.bmp

.PHONY: all start run windowed stats clean
