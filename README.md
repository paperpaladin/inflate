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

162 passed, 0 failed
```

```
$ make test-asan
gcc -std=c11 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
	-Wall -Wextra -Wpedantic -o inflate-asan inflate.c
ASAN_OPTIONS=exitcode=86 \
	 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1:exitcode=86 t/run.sh ./inflate-asan

162 passed, 0 failed
```

Usage is stdin to stdout and nothing else:

```
$ ./inflate < file > file.gz      # compress
$ ./inflate -d < file.gz > file   # decompress
$ gzip -d < file.gz               # also works, which is the whole point
```

## Why this exists

DEFLATE is the format you cannot avoid. It is inside gzip, zlib, PNG, zip,
HTTP `Content-Encoding` and git packfiles, and it is small enough that "I use
zlib" and "I understand DEFLATE" can stay separate facts indefinitely. This
closes that gap by writing both halves against the RFCs and then refusing to
believe any of it until an independent implementation agrees.

The decoder is the interesting half, and not because it is clever. It is a
parser for bytes chosen by somebody else, the shape of problem that produces
CVEs: a Huffman tree whose code lengths over-subscribe, a
back-reference pointing before the start of output, a code-length repeat that
runs off the end of the alphabet, a stored block claiming more bytes than
remain. Each of those is a one-line check, and each of those checks is a
one-line vulnerability if you skip it.

## The oracle

**`/usr/bin/gzip` is the authority. If it disagrees with this code, this code
is wrong.** There is not a single hand-written expected bitstream in the test
suite, on purpose: a suite written against my own reading of RFC 1951 would
pass exactly when my misreadings were self-consistent, the failure mode that
matters. Every assertion is a question put to an implementation I did
not write, ranked here by how little control I have over it:

1. `./inflate < X | gzip -d` must reproduce `X`, byte for byte.
2. `gzip -N -c < X | ./inflate -d` must reproduce `X`, for every N in 1..9.
3. `./inflate < X | ./inflate -d` must reproduce `X`. (Weakest: both sides are
   mine, so a shared misunderstanding passes.)

Over eleven fixtures chosen to hit code paths, not to look varied:
empty, one byte, two bytes, 70KB of a single repeated byte, 100KB of zeros,
1.3MB of repetitive text, 1.2MB of `/dev/urandom`, 348KB of real C source, a
text+random+text sandwich, a fixture containing every byte value 0-255, and two
identical 348KB halves so matches land past the 32K window limit. Three are over
1MB. The random one is load-bearing: it is the only input that forces the
stored-block path on both sides.

Then, because none of that catches a decoder that is merely memory-unsafe:
eight malformed streams must each exit exactly 1 (not "nonzero", a crash is
also nonzero). Nine hand-built streams that each violate exactly one rule must
be rejected by gzip *and* by us, and 300 randomly mutated valid streams must
each exit 0 or 1 and never on a signal. `make test-asan` reruns the identical
suite under `-fsanitize=address,undefined`.

The gzip CLI never emits `FEXTRA`, `FCOMMENT` or `FHCRC`, so those header-skip
branches would be dead code in every run above. One test builds such members by
hand and asserts that gzip and this code agree member by member.

## Status

Works, and is verified to work by something other than itself: 162 checks pass
clean under `-O2`, under `-fsanitize=address,undefined`, and under valgrind with
no leaks in either direction. 400 additional randomised oracle round-trips and
3100 fuzzed decoder inputs were run outside the suite without a failure.

The decoder is complete. Stored, fixed-Huffman and dynamic-Huffman blocks;
CRC-32 and ISIZE both verified and both rejected on mismatch; `FEXTRA`,
`FNAME`, `FCOMMENT` and `FHCRC` headers skipped; concatenated members handled,
because `cat a.gz b.gz | gzip -d` handles them.

The encoder emits all three block types and picks per block. Measured against
`gzip -6` on this machine, before and after it learned to build its own trees.
Five of these seven rows reproduce byte for byte today. The two that do not
are marked, and why is worth saying. That `fixed codes` column is a measurement of an encoder that no longer exists, so a row cannot simply be
re-run to refresh it, both columns have to come from the same input, which
is what dates the first row instead of making it wrong:

| input | bytes | fixed codes | own trees | `gzip -6` |
| --- | --- | --- | --- | --- |
| 20 copies of `inflate.c` *(when it was 25694 bytes; it is 28350 now, so this row no longer re-runs, 567000 in gives 15038 against gzip's 15431)* | 513880 | 18098 (1.30x) | **13508** (0.97x) | 13880 |
| 30000 repeated lines *(`the quick brown fox jumps over the lazy dog`, 44 bytes with its newline)* | 1320000 | 10984 (2.80x) | 4440 (1.13x) | 3916 |
| 100KB of zeros | 100000 | 656 (4.97x) | 161 (1.22x) | 132 |
| every byte value x400 | 102400 | 1235 (1.67x) | 805 (1.09x) | 739 |
| the deep-tree fixture | 22958 | 22978 (1.40x) | 16378 (1.00x) | 16377 |
| 10MB of docs and man pages *(a local corpus, so not reproducible elsewhere)* | 10000000 | 7887682 (1.02x) | **7715731** (1.00x) | 7731463 |
| 1.2MB of `/dev/urandom` | 1200000 | 1200203 | 1200203 | 1200203 |

The "own trees" column also carries lazy matching, the other half of the gap,
and it is what takes the first two rows below `gzip -6`. A greedy matcher
takes the match in front of it; this one checks whether starting one byte later
is strictly longer and spends the byte as a literal when it is. Worth 3.9% on
text: 14063 to 13508, and 0.3% on the 10MB corpus, and worth exactly
nothing on the three degenerate rows, because their matches all run to the
258-byte cap and there is no longer match to find.

Encoding costs 1.50x the time it did before either change: 405ms against 270ms
on the 10MB corpus, where `gzip -6` takes 281ms. The trees are close to free.
Lazy matching is not, because it runs the match search twice at every position
that found a match. Wiring `-1`..`-9` to turn it off is the next item in
`PLAN.md`, and that measurement is why.

### What does not work

- Match finding is still where the gap is. One lazy pass with a one-byte
  lookahead, a fixed 128-entry chain limit, and a block boundary every 32KB
  regardless of where the symbol distribution actually turns over. That is what
  the 1.13x on repetitive input and 1.22x on 100KB of zeros are made of. The
  trees there are already as tight as trees can get, and lazy matching cannot
  help, because every match in those already runs to the 258-byte cap.
- Over-long codes are corrected by halving frequencies, not by
  package-merge. A block whose natural Huffman tree runs deeper than the 15
  bits RFC 1951 allows has every frequency halved and the tree rebuilt until it
  fits. Correct, and coarse: one halving usually overshoots several levels at
  once, so those blocks pay slightly more than an optimal length-limited code
  would. Rare enough that no fixture reached it until one was built to.
- Nothing streams. Both the whole input and the whole output are held in
  memory, and the encoder's match-chain array adds 4 bytes per input byte, so
  compressing an N-byte file costs roughly 5N plus the output. A file larger
  than RAM does not work at all. This is the limitation most likely to bite a
  real user first.
- `-1`..`-9` move two knobs, not three. They set the hash-chain limit (4
  to 1024) and whether lazy matching runs at all, which is where the time
  goes. They do not change the block size or the Huffman strategy. `-6` is the
  default because it is gzip's. On this project's own source they span 26382
  bytes at `-1` to 13297 at `-9`, against 13880 for `gzip -6`.

  They did nothing until recently: they parsed and were discarded. A
  monotonicity check alone would not have caught that, since every level
  produced identical output and identical output is non-increasing, so the
  suite asserts `-1` is *strictly* larger than `-9`.
- Slower than gzip, by about 2.3x decoding and 1.4x encoding on 10MB of
  text. The decoder walks Huffman codes one bit at a time instead of using a
  lookup table.
- `FHCRC` is skipped, not verified. A corrupt header is caught later by the payload CRC-32 instead of up front.
- Incomplete Huffman trees are tolerated, not rejected. An under-subscribed
  tree builds, and an unused bit pattern fails at use time instead of build time. zlib is stricter about the literal/length tree specifically. No gzip
  output exercises the difference, so the oracle cannot tell me which of us is
  right, and I would rather say so than guess.
- No raw-DEFLATE or zlib (RFC 1950) mode, no file arguments, no `-k`/`-f`,
  no preserved filenames or timestamps. The encoder deliberately writes MTIME 0
  and omits `FNAME`.
- Three known-weak assertions. See "Where the tests are still weak" below.
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
  DEFLATE leans on constantly. There `memcpy` and `memmove` are both wrong, so
  the optimisation would be a correctness bug.
- The fixed distance tree is built with **32 codes, not 30**. RFC 1951 section 3.2.6
  defines the unusable 30 and 31 precisely so the tree comes out complete.
  Symbols 30 and 31 are then rejected at use.

And the deliberate corners, each marked in the source with a `shortcut:` comment
naming its ceiling and upgrade path:

| Where | Shortcut | Upgrade when |
| --- | --- | --- |
| `huf1` | O(n^2) scan for the two lightest nodes, no heap | blocks grow by orders of magnitude |
| `huflen` | Over-long codes halved into range, not package-merged | that fraction of a percent becomes measurable |
| `prev[]` in `deflate_gz` | One chain slot per input byte, 4N memory | a file stops fitting in RAM |
| `CHAIN` | One hard-coded chain limit, greedy matching | ratio matters |
| `main` | `-1`..`-9` parse and do nothing | there is more than one strategy |
| `gunzip` | `FHCRC` skipped, not verified | a caller must reject bad headers before inflating |
| `t/run.sh` | Scratch dir keyed on binary name only | two runs of the *same* binary ever race |

The one piece of compression intelligence that is *not* optional is the
per-block choice of block type, because without it random input grows ~5%
instead of ~0.02%. Each 32KB of input is tokenised once, and then all three
candidate costs are counted in bits before anything is written: stored, fixed
codes, and dynamic codes *including* the tree header that block would have to
carry. Cheapest wins, and dynamic has to win strictly, so a block with a flat
distribution never pays for a tree it cannot earn back. 32KB specifically
because one stored block holds 65535 bytes, so the chooser can never want a
size it cannot express.

On `/dev/urandom` that lands on exactly `gzip -9`'s size: 1200203 bytes from
1200000, because both fall back to stored blocks and pay the same 5 bytes per
block. Not the same *bytes*, though, and this README claimed otherwise until
the claim was checked: `gzip` writes a real `MTIME` where we write 0, and it
flushes its stored blocks at 32777 bytes, not 32768, so the two agree on
the total and on nothing else.

## Where the tests are still weak

The suite was itself tested, by deleting each of the decoder's ten defensive
checks one at a time and rerunning. Seven deletions are caught. Three are not,
and pretending otherwise would be worse than saying so:

- `lens[256] == 0` (no end-of-block code). Deleting it changes only the
  error message: the tree still builds, decoding still runs out of input, and
  the stream is still rejected. The check buys diagnosis, not safety.
- `b->n - b->i < len` (stored block longer than the input). Deleting it is a
  real heap overread, but ASan cannot see it, because the input buffer is
  allocated in 64KB steps and the bogus read lands inside that slack.
- `i == 0` before a repeat-previous code. Deleting it reads `lens[-1]`, one
  byte before a stack array, and ASan's stack redzones did not flag it at `-O1`.

Two more, found while adding the encoder's own Huffman trees:

- The deep-tree fixture can rot without failing. `t/run.sh` builds it from
  a seeded `random.Random(1)`, and its job is to make the natural tree 17 bits
  deep so the 15-bit limiter has to run. Nothing in the suite checks that it
  still does. If a future change to the matcher, or to CPython's
  `random.choices`, flattens that distribution below 16 bits, the fixture
  keeps passing while silently testing nothing. The two bits of headroom are
  deliberate, and are not a substitute for the assertion.
- `cnt[0] = 0` in `hufcode` cannot be killed, and should not be chased.
  Deleting it leaves the suite green, which looks exactly like a coverage gap
  and is not one: without it every `next[l]` comes out as the correct code plus
  `2^l * cnt[0]`, and `wcode` only ever extracts the low `l` bits, so the
  offset always masks off. It is kept because `code[]` holding real canonical
  codes is worth one line to anyone who reads it later without knowing that.

And one from the lazy matcher, the same shape as the 32-code case
below: a real difference the oracle cannot resolve.

- Changing the lazy test from `next > best` to `next >= best` survives.
  Preferring an equal-length match one byte later pays for a literal and buys
  nothing, so it is genuinely worse, and it costs 1.4% on the text fixture:
  0.973x of `gzip -6` becomes 0.987x. The bound is 1.00x, so it passes.
  Tightening to 0.98x would catch it with 0.7% of margin against *`gzip`'s*
  output, not against ours, so the next gzip that packs a little tighter
  would fail this suite for a reason that has nothing to do with this code. An
  absolute byte count would be the right assertion and is not available either:
  the text fixture is twenty copies of `inflate.c`, so it changes size every
  time this file does. Left as a known gap, not papered over with a
  brittle number.

The honest summary is that the suite proves interoperability thoroughly and
memory safety only partly.

## A note on being wrong

Three hand-written expectations turned out wrong, and in all three cases the
code or the tool was right.

The fixed distance tree with 30 codes "obviously" matched the 30 usable distance
codes. That RFC spells out 32, and building it with 30 leaves the tree
incomplete. Having fixed it, I then tried to write a test that distinguishes the
two, and could not: valid streams never use codes 30 or 31, and both versions
reject invalid ones, so the two are observationally equivalent and the oracle has
no opinion. The 32-code form is still correct. What I wanted to write does not exist.

I expected the 300-stream mutation fuzz to catch any missing bounds check,
because an out-of-bounds back-reference is *the* classic inflate bug. It did
not. Deleting `dist > o->n` left all 137 assertions green. That is why there is
now a section of hand-built streams that each violate exactly one rule.

Then the same shape of mistake again, on the encoder this time. Dynamic
Huffman went in, all 148 checks passed, and the ratio on the project's own
source went from 1.30x to 1.01x, so the obvious conclusion was that the
existing fixtures covered the new code. They did not cover the part most likely
to be wrong. Raising the 15-bit code-length cap to 16 left the suite green;
deleting the length limiter outright *also* left the suite green, because not
one of the eleven fixtures produced a Huffman tree deeper than 14 bits, and the
limiter had therefore never executed in any test run. The fixture that forces it
had to be constructed: Fibonacci frequencies, which are the deepest tree a given
total can produce, laid out so that every three-byte window in the file is
unique and LZ77 finds no match at all to blur them. With it in place, deleting
the limiter fails against `gzip -d`, as it always should have.

Worst of the three: the hand-built streams did not catch it either, and the
reason was in the Makefile, not the test. A sanitizer abort exits 1 by
default, and `t/run.sh` asserts "exit 1" to mean "cleanly rejected this stream",
so `make test-asan` reported a passing suite while ASan was printing a
heap-buffer-overflow to stderr. `test-asan` could not fail. Setting
`ASAN_OPTIONS=exitcode=86` fixed it, and only then did the deleted bounds check
show up as a failure. The bug was never in the codec. It was in the thing that
was supposed to find bugs in the codec.

## License

GPL-3, matching `gzip` itself: the oracle this is checked against is GPL, and a
reimplementation of its format has no business being less free than the thing it
interoperates with.
