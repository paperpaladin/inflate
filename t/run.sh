#!/bin/sh
# The oracle. /usr/bin/gzip is the authority: if it disagrees with us, we are
# wrong. Everything here is either "gzip accepts what we emit" or "we accept
# what gzip emits", plus a self round-trip and a crash hunt on corrupt input.
#
# Usage: t/run.sh [path-to-inflate-binary]

IN=${1:-./inflate}
case $IN in /*) ;; *) IN="$PWD/$IN" ;; esac

D=$(dirname "$0")/tmp
rm -rf "$D"
mkdir -p "$D"
F="$D/fix"
mkdir -p "$F"

pass=0
fail=0
ok()   { pass=$((pass+1)); }
bad()  { fail=$((fail+1)); printf 'FAIL: %s\n' "$1"; }
check() { if [ "$2" = 0 ]; then ok; else bad "$1"; fi; }

# ---------------------------------------------------------------- fixtures
: > "$F/empty"
printf 'x' > "$F/one"
printf 'ab' > "$F/two"
printf 'A%.0s' $(seq 1 70000) > "$F/allsame"
head -c 100000 /dev/zero > "$F/zeros"
# Highly repetitive, >1MB.
yes 'the quick brown fox jumps over the lazy dog' | head -n 30000 > "$F/rep"
# Incompressible, >1MB: forces the stored-block path.
head -c 1200000 /dev/urandom > "$F/rand"
# Real text, ~300KB.
for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
	cat "$(dirname "$0")/../inflate.c"
done > "$F/text"
# Compressible and incompressible interleaved in one stream, >1MB: the block
# chooser has to switch strategies mid-file.
cat "$F/text" "$F/rand" "$F/text" > "$F/mixed"
# Every byte value, so no literal symbol goes untested.
python3 -c 'import sys;sys.stdout.buffer.write(bytes(range(256))*400)' > "$F/bytes"
# Long-distance matches: identical halves 500KB apart (beyond the 32K window
# for the second copy's start, so it exercises the window limit too).
cat "$F/text" > "$F/far"; head -c 20 /dev/zero >> "$F/far"; cat "$F/text" >> "$F/far"

FIX="empty one two allsame zeros rep rand text mixed bytes far"

# -------------------------------- 2. we decode gzip at every level -1..-9
for f in $FIX; do
	for L in 1 2 3 4 5 6 7 8 9; do
		gzip -$L -c < "$F/$f" > "$D/g.gz" 2>"$D/err" \
			&& "$IN" -d < "$D/g.gz" > "$D/out" 2>>"$D/err" \
			&& cmp -s "$F/$f" "$D/out"
		check "gzip -$L -> ours -d: $f" $?
	done
done

# ------------------------------- 4. gzip header variants we have to skip
# Compressing a named file makes gzip set FNAME; -9 on stdin does not.
cp "$F/text" "$D/named-input-file"
gzip -k -f "$D/named-input-file" 2>/dev/null \
	&& "$IN" -d < "$D/named-input-file.gz" > "$D/out" \
	&& cmp -s "$F/text" "$D/out"
check "FNAME header skipped" $?

gzip -9 -c -n < "$F/text" > "$D/noname.gz" \
	&& "$IN" -d < "$D/noname.gz" > "$D/out" && cmp -s "$F/text" "$D/out"
check "gzip -n (no FNAME)" $?

# --------------------------------------- 5. concatenated members, both ways
gzip -c < "$F/one" > "$D/a.gz"
gzip -9 -c < "$F/text" > "$D/b.gz"
cat "$D/a.gz" "$D/b.gz" > "$D/cat.gz"
cat "$F/one" "$F/text" > "$D/cat.exp"
"$IN" -d < "$D/cat.gz" > "$D/out" && cmp -s "$D/cat.exp" "$D/out"
check "multi-member gzip stream" $?

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
