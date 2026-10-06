CC      = gcc
CFLAGS  = -std=c11 -O2 -Wall -Wextra -Wpedantic

inflate: inflate.c
	$(CC) $(CFLAGS) -o $@ $<

test: inflate
	t/run.sh ./inflate

clean:
	rm -f inflate
	rm -rf t/tmp

.PHONY: test clean
