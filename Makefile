CC      = gcc
CFLAGS  = -std=c11 -O2 -Wall -Wextra -Wpedantic

inflate: inflate.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f inflate

.PHONY: clean
