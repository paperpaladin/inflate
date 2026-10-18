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

169 passed, 0 failed
```

```
$ make test-asan
gcc -std=c11 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
	-Wall -Wextra -Wpedantic -o inflate-asan inflate.c
ASAN_OPTIONS=exitcode=86 \
	 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1:exitcode=86 t/run.sh ./inflate-asan

169 passed, 0 failed
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

Works, and is verified to work by something other than itself: 169 checks pass
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
that found a match. Wiring `-1`..`-9` to turn it off is why those flags exist.

Blocks then stopped ending every 32768 input bytes and started ending where
the symbol distribution changes. Tokens accumulate in chunks of 4096 tokens or
8192 input bytes, whichever comes first, and each chunk is priced three ways:
as its own block, as part of the block in progress, and the block in progress
alone. When two blocks come out cheaper than one, the boundary goes there. On
uniform input one header always beats two, so a block grows until the token
buffer forces a boundary at a million tokens.

Measured on the suite's own fixtures, which `t/run.sh` builds, against the
same files before the change. The two text rows are 20 copies of `inflate.c`
at 32973 bytes, so they move every time this file does:

| fixture | bytes in | 32KB blocks | by cost | `gzip -6` |
| --- | --- | --- | --- | --- |
| 20 copies of `inflate.c` | 659460 | 214648 | **213172** | 217870 |
| the same, twice, 659KB apart | 1318940 | 427763 | **424737** | 434443 |
| 30000 repeated lines | 1320000 | 4440 | **3915** | 3916 |
| 70000 `A`s | 70000 | 129 | **104** | 104 |
| 100KB of zeros | 100000 | 161 | **132** | 132 |
| every byte value x400 | 102400 | 805 | **743** | 739 |
| text, random, text | 2518920 | 1630731 | **1627075** | 1640550 |
| 1.2MB of `/dev/urandom` | 1200000 | 1200203 | **1200118** | 1200203 |
| the deep-tree fixture | 22958 | 16378 | 16378 | 16377 |

Two other corpora, local and so not reproducible elsewhere. A `tar` of
`/usr/include/linux` and `/usr/include/x86_64-linux-gnu`, 7311360 bytes, goes
from 1631836 to 1626345 against `gzip -6`'s 1632111. And 400 files sampled
from `/usr/include`, `/usr/share/doc` and `/usr/bin`, 24095673 bytes of our
output before and 24005928 after, against 24144768 for `gzip -6`.

Ten of those 400 files came out **bigger**, by 1 to 58 bytes, the worst being
0.44% on a 13KB HTML file. The boundary search is greedy: it decides a cut
against the next chunk only, so a cut that pays for itself there can leave the
chunks after it in the wrong block. A margin that required the cut to save
more than it costs was tried and made every corpus worse, so it is not there.

Encoding time went up 4%, 362ms to 376ms on the 7.3MB tar, where `gzip -6`
takes 199ms. Pricing two or three candidate blocks per chunk means building
Huffman trees for each of them, which is the cost.

### What does not work

- Match finding is where the remaining gap is. One lazy pass with a one-byte
  lookahead and a chain limit of 4 to 1024 depending on the level. The two
  degenerate rows that used to sit at 1.13x and 1.22x of `gzip -6` are now
  level with it, because those were block-boundary costs rather than matching
  costs, but nothing here looks for a better match than the first long one it
  finds a byte later.
- The block boundary search is greedy and one chunk deep. It asks whether
  this chunk belongs with the block in front of it, never whether the three
  chunks after it would have changed the answer, and it cannot move a boundary
  once it is placed. That is what makes 10 files in 400 come out a few bytes
  larger than the fixed 32KB grid did. An optimal split is a shortest-path
  problem over chunk boundaries, which is what zopfli does and what this does
  not.
- Blocks cannot grow past a million tokens. The buffer that holds them is
  what forces a boundary, so a uniform file pays a tree header every million
  tokens whether or not the distribution gave any reason for one. Streaming the
  encoder is the item that has to revisit it.
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

  They are also not monotone, which the suite claimed they were. A deeper
  chain finds different matches, which changes where the cost puts a block
  boundary, so a level can land a few bytes above the level below it. Over 60
  real files, 6 of 480 level pairs grow, the worst by 0.18%; the encoder before
  cost-chosen boundaries had 2 of 480 growing by up to 0.029%, so the strict
  claim was already false when it was written. It survived because the only
  fixture it runs on happened not to be one of those cases, and it failed the
  first time that fixture changed by a few bytes, that fixture being 20 copies
  of `inflate.c`. The assertion now allows 0.5%.
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
| `MAXTOK` | A block cannot outgrow the token buffer | the encoder streams |
| `gunzip` | `FHCRC` skipped, not verified | a caller must reject bad headers before inflating |
| `t/run.sh` | Scratch dir keyed on binary name only | two runs of the *same* binary ever race |

The one piece of compression intelligence that is *not* optional is the
per-block choice of block type, because without it random input grows ~5%
instead of ~0.02%. All three candidate costs are counted in bits before
anything is written: stored, fixed codes, and dynamic codes *including* the
tree header that block would have to carry. Cheapest wins, and dynamic wins
ties, so a block with a flat distribution never pays for a tree it cannot earn
back.

The same cost function then chooses the boundaries, which is why it is
computed from symbol counts rather than from the tokens. Merging a candidate
chunk into the block in progress is 316 additions, so pricing a boundary does
not mean walking a million tokens again.

A block's cost has to be the number of bits the writer then emits, or every
boundary in the file was chosen on a fiction, so the encoder checks: it counts
the bits it wrote and dies if they differ from the price. That check is the
only thing standing behind the two dynamic-header terms. Dropping the `3 *
hclen` term, or the extra bits on code-length symbol 16, leaves every other
assertion in the suite green, and both are caught the moment the writer and
the price disagree. Stored blocks are exempt: their byte alignment is a
worst case, not a count.

A stored block holds 65535 bytes, which a block could not reach while blocks
were cut at 32768; a long stored run is now written in pieces, each with its
own LEN/NLEN pair, and only the last of them carries `BFINAL`.

On `/dev/urandom` this used to land on exactly `gzip -9`'s size: 1200203 bytes
from 1200000, because both fell back to stored blocks and paid the same 5 bytes
per block. It is 1200118 now, 85 bytes smaller, because a stored run is cut at
65535 rather than 32768 and so pays that overhead half as often. The bytes were
never the same anyway, and this README claimed otherwise until the claim was
checked: `gzip` writes a real `MTIME` where we write 0, and it flushes its
stored blocks at 32777 bytes, not 32768.

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

Seventeen mutations of the block-boundary code were planted and rebuilt.
Thirteen are caught, four are not:

- `stored_cost` returning `parts = 1` for a run of any length. It
  underprices a long stored block by 42 bits per 65535 bytes, which is 0.008%
  of what the block costs, and no decision this feeds turns on a margin that
  thin. Equivalent in every measurable sense; killing it would mean asserting
  on a cost the encoder never prints.
- `CHUNKIN` raised from 8192 to a megabyte, which is the same as removing
  it. This is a tuning constant: it costs 3.5% on 20 copies of a 28KB file,
  where long matches make 4096 tokens cover half a megabyte, and it *gains*
  0.02% on the suite's own fixtures. Pinning it would mean a size bound tight
  enough to fail for the next matcher improvement, and a tuning constant is
  exactly the thing a brittle bound should not be spent on.
- Deleting the cost self-check. An assertion that never fires in a correct
  program cannot be observed on its own, which makes this survivor expected
  rather than informative. What it is worth was measured by planting it
  *together* with a cost defect: with the check deleted, dropping the
  `3 * hclen` term or the `clextra` term from the dynamic price leaves all
  169 assertions green, and with it in place both die immediately. So the
  check is load-bearing and the survivor is the hole it fills, not a hole
  in it.
- `fix < best` becoming `fix <= best` in the block-type chooser. It changes
  which type wins when a fixed-code block and a dynamic one cost the same
  number of bits, and a tie means the output is the same size either way. No
  size assertion can see it and none should be written to.

The thirteen that are caught are worth listing, because they are the shape of
defect this item could plausibly have shipped: never cutting a block, always
cutting one, cutting on a fixed grid (the previous encoder fails 5
assertions), a stored run not split at 65535, `BFINAL` set on every piece of
a split stored run, counting a match as one byte instead of its length, a
million-token buffer shrunk to 16384, and six separate terms removed from the
cost model.

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
