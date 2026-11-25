CC      = gcc
CFLAGS  = -std=c11 -Wall -Wextra -Wpedantic -O2 -D_DEFAULT_SOURCE

COMMON  = rdt.c channel.c tui.c i18n.c
HEADERS = rdt.h channel.h tui.h i18n.h rdt_i18n.h
PROGS   = stop_and_wait_send stop_and_wait_recv \
          go_back_n_send go_back_n_recv \
          selective_repeat_send selective_repeat_recv

# hoplab: every .c file in src/ is part of the program (modules register
# themselves, so adding a protocol means adding a file)
NL_SRC  = $(wildcard src/*.c)
NL_OBJ  = $(patsubst src/%.c,build/%.o,$(NL_SRC)) $(patsubst %.c,build/common-%.o,$(COMMON))
NL_HDR  = $(wildcard src/*.h) $(HEADERS)

.PHONY: all clean test regress
all: $(PROGS) hoplab

# each demo program is one .c file plus the common modules
%: %.c $(COMMON) $(HEADERS)
	$(CC) $(CFLAGS) $< $(COMMON) -o $@

hoplab: $(NL_OBJ)
	$(CC) $(CFLAGS) $(NL_OBJ) -lm -o $@

build/%.o: src/%.c $(NL_HDR) | build
	$(CC) $(CFLAGS) -I. -Isrc -c $< -o $@

build/common-%.o: %.c $(HEADERS) | build
	$(CC) $(CFLAGS) -c $< -o $@

build:
	mkdir -p build

# headless run of every scenario in the catalog, checking its `expect` lines
test: hoplab
	./hoplab check

# the full regression suite: seeds, parameter variants, sanitizers, tunnels,
# classic demos, translations (tests/regress.sh quick for a fast pass)
regress: all
	tests/regress.sh

clean:
	rm -rf $(PROGS) hoplab build
