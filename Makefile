CC      = gcc
CFLAGS  = -std=c11 -Wall -Wextra -Wpedantic -O2 -D_DEFAULT_SOURCE

COMMON  = rdt.c channel.c tui.c i18n.c
HEADERS = rdt.h channel.h tui.h i18n.h rdt_i18n.h
PROGS   = stop_and_wait_send stop_and_wait_recv \
          go_back_n_send go_back_n_recv \
          selective_repeat_send selective_repeat_recv

.PHONY: all clean
all: $(PROGS)

# each program is one .c file plus the common modules
%: %.c $(COMMON) $(HEADERS)
	$(CC) $(CFLAGS) $< $(COMMON) -o $@

clean:
	rm -f $(PROGS)
