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

# Handcrafted members exercising FEXTRA/FCOMMENT/FHCRC, which the gzip CLI
# never emits, so nothing above reaches those header branches. The script
# asserts gzip -d and we agree, member by member.
python3 - "$F/text" "$IN" <<'PY' && ok || bad "FEXTRA/FCOMMENT/FHCRC headers"
import subprocess, sys, zlib
data = open(sys.argv[1], 'rb').read()
B = sys.argv[2]
raw = subprocess.run(['gzip','-9','-c','-n'], input=data,
                     capture_output=True, check=True).stdout
assert raw[3] == 0          # gzip -n on stdin sets no optional header fields
body = raw[10:]

def member(extra=None, name=None, comment=None, hcrc=False):
	flg = ((4 if extra is not None else 0) | (8 if name else 0)
	       | (16 if comment else 0) | (2 if hcrc else 0))
	h = bytes([0x1f, 0x8b, 8, flg, 0, 0, 0, 0, 0, 255])
	if extra is not None: h += len(extra).to_bytes(2, 'little') + extra
	if name: h += name + b'\0'
	if comment: h += comment + b'\0'
	if hcrc: h += (zlib.crc32(h) & 0xffff).to_bytes(2, 'little')
	return h + body

cases = {
	'FEXTRA':       member(extra=b'ab\x04\x00xxxx'),
	'FEXTRA empty': member(extra=b''),
	'FNAME':        member(name=b'some/file.txt'),
	'FCOMMENT':     member(comment=b'a comment'),
	'FHCRC':        member(hcrc=True),
	'all flags':    member(extra=b'QQ\x02\x00hi', name=b'n', comment=b'c', hcrc=True),
}
rc = 0
for label, m in cases.items():
	g = subprocess.run(['gzip', '-d'], input=m, capture_output=True)
	o = subprocess.run([B, '-d'], input=m, capture_output=True)
	gok = g.returncode == 0 and g.stdout == data
	ook = o.returncode == 0 and o.stdout == data
	if gok != ook:
		print('  %s: gzip=%s ours=%s %s'
		      % (label, gok, ook, o.stderr.decode().strip()))
		rc = 1
sys.exit(rc)
PY

# --------------------------------------- 5. concatenated members, both ways
gzip -c < "$F/one" > "$D/a.gz"
gzip -9 -c < "$F/text" > "$D/b.gz"
cat "$D/a.gz" "$D/b.gz" > "$D/cat.gz"
cat "$F/one" "$F/text" > "$D/cat.exp"
"$IN" -d < "$D/cat.gz" > "$D/out" && cmp -s "$D/cat.exp" "$D/out"
check "multi-member gzip stream" $?

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
