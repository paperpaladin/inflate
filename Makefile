CC      = gcc
CFLAGS  = -std=c11 -O2 -Wall -Wextra -Wpedantic

inflate: inflate.c
	$(CC) $(CFLAGS) -o $@ $<

test: inflate
	t/run.sh ./inflate

# A decompressor parses untrusted bytes, so the same tests have to pass with
# the sanitizers on. Separate binary so `make test` stays fast.
#
# exitcode=86 is load-bearing, not decoration. By default a sanitizer abort
# exits 1, and t/run.sh cannot tell that apart from the clean "exit 1, rejected
# this stream" it is asserting - so test-asan silently could not fail. UBSan
# needs halt_on_error too, because it only prints and keeps going otherwise.
SANOPT = ASAN_OPTIONS=exitcode=86 \
	 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1:exitcode=86

test-asan: inflate.c
	$(CC) -std=c11 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
		-Wall -Wextra -Wpedantic -o inflate-asan inflate.c
	$(SANOPT) t/run.sh ./inflate-asan

clean:
	rm -f inflate inflate-asan
	rm -rf t/tmp

.PHONY: test test-asan clean
