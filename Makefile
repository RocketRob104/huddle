# HUDDLE (C edition)
#   make            build ./huddle (downloads the 2025 season first if data/ is empty)
#   make run        build and launch
#   make refresh    re-download the embedded season from ESPN and rebuild
#
# Requires: clang/gcc, libcurl (ships with macOS), and `brew install sdl3 yyjson`.

CC       ?= clang
SEASON   ?= 2025
DATA     := data/season_$(SEASON).json

PKGS     := sdl3 yyjson
CFLAGS   ?= -O2 -g
CFLAGS   += -std=c11 -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE $(shell pkg-config --cflags $(PKGS))
LDLIBS   := $(shell pkg-config --libs $(PKGS)) -lcurl -lpthread

CORE_OBJ := build/util.o build/model.o build/http.o build/espn.o build/store.o
APP_OBJ  := build/main.o build/views.o build/embedded.o

all: huddle build/huddle-fetch

huddle: $(CORE_OBJ) $(APP_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

build/huddle-fetch: tools/fetch_season.c $(CORE_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

# The season snapshot is downloaded once and then committed/kept. It's an
# order-only dependency on the fetch tool so rebuilding the tool never re-downloads.
$(DATA): | build/huddle-fetch
	build/huddle-fetch $(SEASON) $@

build/embedded.o: src/embedded.c $(DATA)
	$(CC) $(CFLAGS) -DSEASON_FILE='"$(CURDIR)/$(DATA)"' -c $< -o $@

build/%.o: src/%.c $(wildcard src/*.h) | build
	$(CC) $(CFLAGS) -c $< -o $@

build:
	mkdir -p build

run: huddle
	./huddle

refresh:
	rm -f $(DATA)
	$(MAKE) huddle

clean:
	rm -rf build huddle

.PHONY: all run refresh clean
