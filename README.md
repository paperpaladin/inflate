# inflate

A from-scratch DEFLATE (RFC 1951) and gzip (RFC 1952) codec in one C11 file,
both directions, with `/usr/bin/gzip` as the only test oracle.

## make test

```
$ make clean && make test
rm -f inflate inflate-asan
rm -rf t/tmp
gcc -std=c11 -O2 -Wall -Wextra -Wpedantic -o inflate inflate.c
t/run.sh ./inflate

146 passed, 0 failed
```

```
$ make test-asan
gcc -std=c11 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
	-Wall -Wextra -Wpedantic -o inflate-asan inflate.c
ASAN_OPTIONS=exitcode=86 \
	 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1:exitcode=86 t/run.sh ./inflate-asan

146 passed, 0 failed
```

Usage is stdin to stdout and nothing else:

```
$ ./inflate < file > file.gz      # compress
$ ./inflate -d < file.gz > file   # decompress
$ gzip -d < file.gz               # also works, which is the whole point
```

## Why this exists

DEFLATE is the format you cannot avoid -- it is inside gzip, zlib, PNG, zip,
HTTP `Content-Encoding` and git packfiles -- and it is small enough that "I use
zlib" and "I understand DEFLATE" can stay separate facts indefinitely. This
closes that gap by writing both halves against the RFCs and then refusing to
believe any of it until an independent implementation agrees.

The decoder is the interesting half, and not because it is clever. It is a
parser for bytes chosen by somebody else, which is the shape of problem that
produces CVEs: a Huffman tree whose code lengths over-subscribe, a
back-reference pointing before the start of output, a code-length repeat that
runs off the end of the alphabet, a stored block claiming more bytes than
remain. Each of those is a one-line check, and each of those checks is a
one-line vulnerability if you skip it.

## The oracle

**`/usr/bin/gzip` is the authority. If it disagrees with this code, this code
is wrong.** There is not a single hand-written expected bitstream in the test
suite, on purpose: a suite written against my own reading of RFC 1951 would
pass exactly when my misreadings were self-consistent, which is the failure
mode that matters. Every assertion is a question put to an implementation I did
not write, ranked here by how little control I have over it:

1. `./inflate < X | gzip -d` must reproduce `X`, byte for byte.
2. `gzip -N -c < X | ./inflate -d` must reproduce `X`, for every N in 1..9.
3. `./inflate < X | ./inflate -d` must reproduce `X`. (Weakest: both sides are
   mine, so a shared misunderstanding passes.)

Over eleven fixtures chosen to hit code paths rather than to look varied:
empty, one byte, two bytes, 70KB of a single repeated byte, 100KB of zeros,
1.3MB of repetitive text, 1.2MB of `/dev/urandom`, 348KB of real C source, a
text+random+text sandwich, a fixture containing every byte value 0-255, and two
identical 348KB halves so matches land past the 32K window limit. Three are over
1MB. The random one is load-bearing: it is the only input that forces the
stored-block path on both sides.

Then, because none of that catches a decoder that is merely memory-unsafe:
eight malformed streams must each exit exactly 1 (not "nonzero" -- a crash is
also nonzero); nine hand-built streams that each violate exactly one rule must
be rejected by gzip *and* by us; and 300 randomly mutated valid streams must
each exit 0 or 1 and never on a signal. `make test-asan` reruns the identical
suite under `-fsanitize=address,undefined`.

The gzip CLI never emits `FEXTRA`, `FCOMMENT` or `FHCRC`, so those header-skip
branches would be dead code in every run above. One test builds such members by
hand and asserts that gzip and this code agree member by member.

## Status

Works, and is verified to work by something other than itself: 146 checks pass
clean under `-O2`, under `-fsanitize=address,undefined`, and under valgrind with
no leaks in either direction. 400 additional randomised oracle round-trips and
3100 fuzzed decoder inputs were run outside the suite without a failure.

The decoder is complete. Stored, fixed-Huffman and dynamic-Huffman blocks;
CRC-32 and ISIZE both verified and both rejected on mismatch; `FEXTRA`,
`FNAME`, `FCOMMENT` and `FHCRC` headers skipped; concatenated members handled,
because `cat a.gz b.gz | gzip -d` handles them.

### What does not work

- **The encoder has no dynamic Huffman.** Fixed codes only, so output is about
  1.35x larger than `gzip -6`: 12497 bytes vs 9342 on 348KB of C source, 6.70MB
  vs 4.97MB on 10MB of hard-to-compress text. On *highly* repetitive input it is
  worse -- 10984 bytes vs 3916, about 2.8x -- because fixed codes cannot cheapen
  the handful of symbols such a file actually uses. This is the biggest thing
  missing.
- **Nothing streams.** Both the whole input and the whole output are held in
  memory, and the encoder's match-chain array adds 4 bytes per input byte, so
  compressing an N-byte file costs roughly 5N plus the output. A file larger
  than RAM does not work at all. This is the limitation most likely to bite a
  real user first.
- **Match finding is greedy with a fixed 128-entry chain limit.** No lazy
  matching, no `-1`..`-9`. Those flags parse and are then ignored, which is a
  small lie told for drop-in convenience.
- **Slower than gzip**, by about 2.3x decoding and 1.4x encoding on 10MB of
  text. The decoder walks Huffman codes one bit at a time instead of using a
  lookup table.
- **`FHCRC` is skipped, not verified.** A corrupt header is caught later by the
  payload CRC-32 rather than up front.
- **Incomplete Huffman trees are tolerated, not rejected.** An under-subscribed
  tree builds, and an unused bit pattern fails at use time rather than at build
  time. zlib is stricter about the literal/length tree specifically. No gzip
  output exercises the difference, so the oracle cannot tell me which of us is
  right, and I would rather say so than guess.
- **No raw-DEFLATE or zlib (RFC 1950) mode**, no file arguments, no `-k`/`-f`,
  no preserved filenames or timestamps. The encoder deliberately writes MTIME 0
  and omits `FNAME`.
- **Three known-weak assertions.** See "Where the tests are still weak" below.
- ISIZE is modulo 2^32, as RFC 1952 specifies, so a member over 4GB cannot be
  length-validated. Moot while nothing streams.

## Design

One file, because the two directions share the gzip framing, the CRC-32 table
and the RFC 1951 length/distance tables; splitting them buys a header whose only
job is to re-join what the RFCs defined together.

Three decisions that look like shortcuts but are not:

- The output buffer stores **indices, never interior pointers**. Back-references
  copy out of the output while that output is being `realloc`'d, so a cached
  pointer dangles at the worst possible moment.
- Match copies go **one byte at a time**. `dist < length` is the run-length case
  DEFLATE leans on constantly; there `memcpy` and `memmove` are both wrong, so
  the optimisation would be a correctness bug.
- The fixed distance tree is built with **32 codes, not 30**. RFC 1951 §3.2.6
  defines the unusable 30 and 31 precisely so the tree comes out complete.
  Symbols 30 and 31 are then rejected at use.

And the deliberate corners, each marked in the source with a `shortcut:` comment
naming its ceiling and upgrade path:

| Where | Shortcut | Upgrade when |
| --- | --- | --- |
| `fixbits`/`wfix` | Fixed Huffman only in the encoder | ratio matters |
| `prev[]` in `deflate_gz` | One chain slot per input byte, 4N memory | a file stops fitting in RAM |
| `CHAIN` | One hard-coded chain limit, greedy matching | ratio matters |
| `main` | `-1`..`-9` parse and do nothing | there is more than one strategy |
| `gunzip` | `FHCRC` skipped, not verified | a caller must reject bad headers before inflating |
| `t/run.sh` | Scratch dir keyed on binary name only | two runs of the *same* binary ever race |

The one piece of compression intelligence that is *not* optional is the
per-block choice between a fixed-Huffman block and a stored block, because
without it random input grows ~5% instead of ~0.02%. Each 32KB of input is
tokenised, the fixed-Huffman cost is totted up as tokens are produced, and the
cheaper block type wins. 32KB specifically because one stored block holds 65535
bytes, so the chooser can never want a size it cannot express. On `/dev/urandom`
this lands on byte-identical output to `gzip -9` -- 300068 bytes from 300000 --
because both fall back to stored blocks with the same 5-byte overhead.

## Where the tests are still weak

The suite was itself tested, by deleting each of the decoder's ten defensive
checks one at a time and rerunning. Seven deletions are caught. Three are not,
and pretending otherwise would be worse than saying so:

- **`lens[256] == 0` (no end-of-block code).** Deleting it changes only the
  error message: the tree still builds, decoding still runs out of input, and
  the stream is still rejected. The check buys diagnosis, not safety.
- **`b->n - b->i < len` (stored block longer than the input).** Deleting it is a
  real heap overread, but ASan cannot see it, because the input buffer is
  allocated in 64KB steps and the bogus read lands inside that slack.
- **`i == 0` before a repeat-previous code.** Deleting it reads `lens[-1]`, one
  byte before a stack array, and ASan's stack redzones did not flag it at `-O1`.

The honest summary is that the suite proves interoperability thoroughly and
memory safety only partly.

## A note on being wrong

Three hand-written expectations turned out wrong, and in all three cases the
code or the tool was right.

The fixed distance tree with 30 codes "obviously" matched the 30 usable distance
codes. The RFC spells out 32, and building it with 30 leaves the tree
incomplete. Having fixed it, I then tried to write a test that distinguishes the
two -- and could not: valid streams never use codes 30 or 31, and both versions
reject invalid ones, so the two are observationally equivalent and the oracle has
no opinion. The 32-code form is still correct; the test I wanted does not exist.

I expected the 300-stream mutation fuzz to catch any missing bounds check,
because an out-of-bounds back-reference is *the* classic inflate bug. It did
not: deleting `dist > o->n` left all 137 assertions green. That is why there is
now a section of hand-built streams that each violate exactly one rule.

Worst of the three: the hand-built streams did not catch it either, and the
reason was in the Makefile rather than the test. A sanitizer abort exits 1 by
default, and `t/run.sh` asserts "exit 1" to mean "cleanly rejected this stream" --
so `make test-asan` reported a passing suite while ASan was printing a
heap-buffer-overflow to stderr. `test-asan` could not fail. Setting
`ASAN_OPTIONS=exitcode=86` fixed it, and only then did the deleted bounds check
show up as a failure. The bug was never in the codec; it was in the thing that
was supposed to find bugs in the codec.

## License

GPL-3, matching `gzip` itself: the oracle this is checked against is GPL, and a
reimplementation of its format has no business being less free than the thing it
interoperates with.
