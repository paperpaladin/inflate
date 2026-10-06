CC      = gcc
CFLAGS  = -std=c11 -O2 -Wall -Wextra -Wpedantic

inflate: inflate.c
	$(CC) $(CFLAGS) -o $@ $<

test: inflate
	t/run.sh ./inflate

# A decompressor parses untrusted bytes, so the same tests have to pass with
# the sanitizers on. Separate binary so `make test` stays fast.
test-asan: inflate.c
	$(CC) -std=c11 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
		-Wall -Wextra -Wpedantic -o inflate-asan inflate.c
	t/run.sh ./inflate-asan

clean:
	rm -f inflate inflate-asan
	rm -rf t/tmp

.PHONY: test test-asan clean
