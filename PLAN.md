# PLAN

## Contract

One C11 file and a CLI. `inflate < X > X.gz` writes a gzip (RFC 1952) stream
wrapping DEFLATE (RFC 1951). `inflate -d < X.gz > X` reads one back. stdin to
stdout only, no file arguments, no zlib, no third-party code of any kind. Both
directions must handle every input shape: empty, one byte, incompressible,
highly repetitive, and larger than a megabyte.

## Hard invariant

**`gzip -d` accepts everything we emit, and we decode everything `gzip` emits,
at every level, byte for byte.**

Nothing else in this project is worth anything if that is false. Ratio is a
preference; speed is a preference; interoperability is the product. A violation
is a failed project instead of something to triage. Secondary but non-negotiable. No
input, however malformed, may make the decoder read or write out of bounds:
`exit 1` with a message is the only acceptable response to garbage.

## Oracle

Ranked by how little control I have over them, most independent first:

1. `/usr/bin/gzip`: a GPL implementation written by other people against
   the same RFCs, with no shared code and no shared assumptions. Both
   directions are checked against it. This is the only judge whose agreement
   means anything, and where it disagrees with me, I am wrong.
2. The RFCs themselves: 1951 and 1952. Authoritative but inert: they cannot
   tell me I misread them, exactly the failure that matters.
3. AddressSanitizer / UBSan / valgrind: independent of my intent but not of
   my test inputs; they only see the paths something else drove them down.
4. Mutation testing: delete one defensive check, confirm the suite goes
   red. Judges the tests, not the code, and catches the suite agreeing
   with itself.
5. Our own round trip: weakest, and listed only for completeness. Both
   sides are mine, so a shared misunderstanding passes silently. It exists to
   catch regressions, never to establish correctness.

## Non-goals

- zlib/RFC 1950 framing, raw headerless DEFLATE, or the zip container.
- Streaming. Whole input and whole output live in memory, deliberately.
- Beating gzip on ratio or speed. Being *accepted by* gzip is the goal.
- File arguments, `-k`, `-f`, timestamp or filename preservation, `-1`..`-9`
  actually selecting anything.
- Optimal length-limited Huffman (package-merge). Over-long codes are brought
  into range by halving frequencies, which is correct but not optimal.
- Threads, SIMD, or any platform beyond x86-64 Linux.

## Commit sequence

- [x] 01 decoder skeleton: buffer, CRC-32, bit reader, gzip framing, stored blocks
      why: the smallest thing that decodes a whole member end to end, so the
      framing and checksum are under test before Huffman can hide a bug in them.
- [x] 02 decoder: RFC 1951 tables, canonical Huffman, fixed-Huffman blocks
      why: fixed blocks exercise the tree decoder and both extra-bit tables with
      no tree-reading code in the way.
- [x] 03 decoder: dynamic Huffman blocks
      why: this is what gzip actually emits at every level, so until it works
      the oracle cannot be consulted at all.
- [x] 04 decoder: optional gzip header fields and concatenated members
      why: gzip sets FNAME whenever it compresses a file, and `cat a.gz b.gz`
      is a legal stream; refusing either would fail interoperability.
- [x] 05 tests: fixtures and the gzip -1..-9 oracle, plus `make test`
      why: half the hard invariant becomes checkable the moment the decoder is
      complete; no reason to wait for the encoder.
- [x] 06 tests: handcrafted FEXTRA/FCOMMENT/FHCRC members
      why: the gzip CLI never emits those, so without hand-built members three
      header branches are dead code in every run.
- [x] 07 tests: eight malformed streams must exit exactly 1
      why: "nonzero" is not good enough, because a segfault is also nonzero.
- [x] 08 tests: nine hand-built streams that each violate exactly one rule
      why: random mutation missed the `dist > o->n` check entirely; narrow
      checks need narrow tests.
- [x] 09 Makefile: test-asan target
      why: a decompressor parses hostile bytes, so the suite has to run again
      with the sanitizers on.
- [x] 10 tests: 300 mutated valid streams must not crash
      why: cheap breadth over the paths no hand-written case thought of.
- [x] 11 encoder: gzip framing with stored blocks only
      why: proves the framing, CRC and ISIZE we *write* against gzip -d before
      any Huffman coding exists to be blamed.
- [x] 12 tests: ours -> gzip -d, round trip, concatenated members
      why: completes the hard invariant; passes with a stored-only encoder,
      which is the point - correctness first, ratio second.
- [x] 13 encoder: LZ77 hash chains, fixed Huffman, per-block type chooser
      why: the first commit that actually compresses, and the stored-block
      escape is what keeps random data from growing 5%.
- [x] 14 tests: ratio sanity
      why: a regression to "store everything" would pass all 144 correctness
      assertions, so one test has to care about size.
- [x] 15 fix: give each suite run its own scratch directory
      why: two concurrent runs wiped each other's fixtures and reported 39
      bogus failures; `make -j test test-asan` could never have worked.
- [x] 16 fix: free both buffers on the error paths
      why: every rejected stream leaked 192KB, and LeakSanitizer had been
      reporting it unseen for the entire project.
- [x] 17 fix: a sanitizer abort must not look like a clean rejection
      why: ASan exits 1, exactly what the reject tests assert, so
      test-asan could not fail. It silently had not been failing.
- [x] 18 docs: README, PLAN, LICENSE
      why: last, so they describe what was built, not what was hoped.

### Milestone 1 - make the encoder competitive

- [x] 19 dynamic Huffman in the encoder
      why: the single largest gap. Fixed codes cost ~1.35x gzip -6 on text and
      ~2.8x on highly repetitive input, and no amount of better matching fixes
      that; the symbol frequencies have to reach the tree.
      done: 1.30x -> 1.01x on text, 2.80x -> 1.13x on repetitive input. Found
      that no fixture reached a tree deeper than 14 bits, so deleting the
      15-bit limiter left the suite green; the fixture that forces it is now
      in t/run.sh. Also found the README claiming byte-identical output to
      gzip -9 on random input, which was only ever identical in size.
- [x] 20 lazy match evaluation
      why: greedy matching takes the first long match even when starting one
      byte later would be longer. Classic ~3-5% for about fifteen lines, and
      with the trees now fitted per block the match search is what the whole
      remaining gap is made of: 1.13x on repetitive input, 1.22x on zeros.
      done: 3.9% on text, which takes it from 1.013x of gzip -6 to 0.973x,
      smaller than gzip's own output. 0.3% on the 10MB corpus, and nothing at
      all on the repetitive, zeros and every-byte fixtures, whose matches all
      run to the 258-byte cap so there is no longer match to look for. Costs
      1.18x the encode time, because the match search now runs twice at every
      position that found one. The ratio bound on text went from 120% to 100%,
      which is the only value that separates lazy from greedy; one mutant
      survives (>= instead of >) and is documented in the README,
      not chased with a brittle bound.
- [x] 21 wire `-1`..`-9` to CHAIN and the lazy threshold
      why: those flags currently parse and lie. Either make them mean
      something or remove them. Raised in value by item 20: lazy matching
      costs 1.18x the encode time for 3.9% on text and 0% on anything whose
      matches saturate, so there is now a real reason to want it off, and
      -1..-3 is where that belongs.
      done: chain limits 4 to 1024, lazy matching off for -1..-3 and on from
      -4 up, default 6 because that is gzip's and this is a drop-in. 26382
      bytes at -1 to 13297 at -9 on the project's own source, against 13880
      for gzip -6. Four assertions: gzip -d accepts all nine levels, size
      never grows as the level rises, -1 is STRICTLY larger than -9, and no
      level equals -6.
      The strict comparison is the one that matters. While the flags were
      ignored every level produced identical output, and identical output is
      non-increasing, so a monotonicity check on its own would have been
      green for a program where the flags did nothing, exactly the
      state this item existed to fix. Reverting the wiring now fails that one
      assertion by name.
- [ ] 22 choose block boundaries by cost, not at a fixed 32KB
      why: with dynamic trees in place, a block should end where the symbol
      distribution changes, not on a round number.

### Milestone 2 - stop holding whole files in memory

- [ ] 23 stream the decoder through a 32K ring buffer
      why: output currently grows without bound, so a 10GB member needs 10GB.
      The window is 32K; nothing older is reachable.
- [ ] 24 stream the encoder, replacing prev[] with a window-sized ring
      why: `prev[]` is one slot per input byte, so compressing costs 4N on top
      of the input. A ring makes it constant.
- [ ] 25 incremental CRC-32 and ISIZE instead of one pass at the end
      why: both currently require the whole buffer to still be present, which
      is the last thing pinning the design to whole-file-in-memory.

### Milestone 3 - close the testing gaps the mutation sweep found

- [ ] 26 make the three surviving mutants fail, starting by sizing the input
      buffer exactly
      why: deleting the stored-length check, the EOB check, or the
      repeat-with-no-previous check still leaves the suite green. The 64KB
      allocation granularity is what hides the first one from ASan, so exact
      sizing turns a documented weakness into a caught bug. Documented in the
      README is not the same as fixed.
- [ ] 27 assert on stderr content, not just exit status
      why: several checks only improve the error message, and the suite cannot
      currently tell a good diagnosis from a vague one.
- [ ] 28 run the mutation sweep from `make`
      why: it found three real problems and currently lives in my shell
      history, which means it will rot. Raised in priority by item 19: the
      deep-tree fixture exists only to drive the length limiter, and nothing
      asserts it still does, so if that distribution ever flattens the suite
      goes on passing while testing nothing. A sweep from `make` is what
      notices.

### Milestone 4 - formats the shortcut corners rule out today

- [ ] 29 raw DEFLATE and zlib (RFC 1950) modes behind a flag
      why: the inflate core already handles both; only framing differs, and
      PNG and git packfiles need exactly that.
- [ ] 30 verify FHCRC instead of skipping it
      why: a corrupt header is currently caught only after inflating the whole
      payload, the wrong order for a hostile input.
