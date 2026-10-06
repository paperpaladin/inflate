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

# ------------------------------------------- 1. gzip -d accepts our output
for f in $FIX; do
	"$IN" < "$F/$f" > "$D/c.gz" 2>"$D/err" \
		&& gzip -d < "$D/c.gz" > "$D/out" 2>>"$D/err" \
		&& cmp -s "$F/$f" "$D/out"
	check "ours -> gzip -d: $f" $?
done

# -------------------------------- 2. we decode gzip at every level -1..-9
for f in $FIX; do
	for L in 1 2 3 4 5 6 7 8 9; do
		gzip -$L -c < "$F/$f" > "$D/g.gz" 2>"$D/err" \
			&& "$IN" -d < "$D/g.gz" > "$D/out" 2>>"$D/err" \
			&& cmp -s "$F/$f" "$D/out"
		check "gzip -$L -> ours -d: $f" $?
	done
done

# --------------------------------------------------- 3. our own round trip
for f in $FIX; do
	"$IN" < "$F/$f" | "$IN" -d > "$D/out" 2>"$D/err" && cmp -s "$F/$f" "$D/out"
	check "round trip: $f" $?
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

"$IN" < "$F/one" > "$D/oa.gz"; "$IN" < "$F/text" > "$D/ob.gz"
cat "$D/oa.gz" "$D/ob.gz" | gzip -d > "$D/out" 2>/dev/null \
	&& cmp -s "$D/cat.exp" "$D/out"
check "gzip -d accepts our members concatenated" $?

# ------------------------------------------ 6. corrupt input is rejected
reject() { # name, file
	"$IN" -d < "$2" > /dev/null 2>/dev/null
	rc=$?
	if [ $rc -eq 1 ]; then ok; else bad "$1 (expected exit 1, got $rc)"; fi
}
: > "$D/z"; reject "empty input rejected" "$D/z"
printf 'not a gzip file at all' > "$D/z"; reject "bad magic rejected" "$D/z"
gzip -c < "$F/text" > "$D/v.gz"
head -c 40 "$D/v.gz" > "$D/z"; reject "truncated payload rejected" "$D/z"
head -c 9 "$D/v.gz" > "$D/z"; reject "truncated header rejected" "$D/z"
sz=$(wc -c < "$D/v.gz"); head -c $((sz-4)) "$D/v.gz" > "$D/z"
reject "missing footer rejected" "$D/z"
# Flip one bit inside the stored CRC so only the checksum disagrees.
python3 - "$D/v.gz" "$D/z" <<'PY'
import sys
b = bytearray(open(sys.argv[1],'rb').read())
b[-8] ^= 0x01
open(sys.argv[2],'wb').write(b)
PY
reject "CRC-32 mismatch rejected" "$D/z"
python3 - "$D/v.gz" "$D/z" <<'PY'
import sys
b = bytearray(open(sys.argv[1],'rb').read())
b[-4] ^= 0x10          # ISIZE only
open(sys.argv[2],'wb').write(b)
PY
reject "ISIZE mismatch rejected" "$D/z"
cat "$D/v.gz" > "$D/z"; printf 'junk' >> "$D/z"
reject "trailing garbage rejected" "$D/z"

# ------------- 6b. hand-built streams that each violate exactly one rule
# Section 7's mutations are random, so they miss narrow checks: deleting the
# `dist > o->n` bounds test left all 137 other assertions passing, which is how
# this section came to exist. Each stream below breaks one rule on purpose, and
# each assertion still belongs to the oracle - gzip -d has to reject it too, so
# "invalid" is never just my opinion.
python3 - "$D/evil" <<'PY'
import sys, zlib
out = sys.argv[1]

class BW:
	def __init__(s): s.b = bytearray(); s.a = 0; s.n = 0
	def bits(s, v, n):
		s.a |= (v & ((1 << n) - 1)) << s.n
		s.n += n
		while s.n >= 8:
			s.b.append(s.a & 0xff); s.a >>= 8; s.n -= 8
	def code(s, c, n):      # Huffman codes are MSB-first, the stream is LSB-first
		r = 0
		for _ in range(n): r = (r << 1) | (c & 1); c >>= 1
		s.bits(r, n)
	def align(s):
		if s.n: s.b.append(s.a & 0xff); s.a = 0; s.n = 0

def flit(w, sym):           # RFC 1951 3.2.6 fixed literal/length code
	if sym < 144:   w.code(0x30 + sym, 8)
	elif sym < 256: w.code(0x190 + sym - 144, 9)
	elif sym < 280: w.code(sym - 256, 7)
	else:           w.code(0xC0 + sym - 280, 8)

def wrap(body, payload=b'A'):
	return (bytes([0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 255]) + bytes(body)
	        + zlib.crc32(payload).to_bytes(4, 'little')
	        + len(payload).to_bytes(4, 'little'))

def fixed_one_literal(w):
	w.bits(1, 1); w.bits(1, 2)      # BFINAL, BTYPE=01 fixed
	flit(w, 65)                     # one literal, so output length is exactly 1

def dist_before_start():
	w = BW(); fixed_one_literal(w)
	flit(w, 257); w.code(6, 5); w.bits(1, 2)   # len 3, dist 9+1=10 > 1 byte out
	flit(w, 256); w.align(); return w.b

def bad_length_code():
	w = BW(); fixed_one_literal(w); flit(w, 286); w.align(); return w.b

def bad_dist_code():
	w = BW(); fixed_one_literal(w)
	flit(w, 257); w.code(30, 5)     # distance codes 30 and 31 are undefined
	w.align(); return w.b

ORD = [16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15]

def dyn(cl, emit, hlit=257, hdist=1):
	"""cl maps code-length symbol -> bit length; emit writes the length stream."""
	w = BW(); w.bits(1, 1); w.bits(2, 2)       # BFINAL, BTYPE=10 dynamic
	hclen = max(ORD.index(s) for s in cl) + 1
	w.bits(hlit - 257, 5); w.bits(hdist - 1, 5); w.bits(hclen - 4, 4)
	for i in range(hclen): w.bits(cl.get(ORD[i], 0), 3)
	code = 0; prev = None; codes = {}
	for s in sorted(cl, key=lambda s: (cl[s], s)):
		if prev is not None: code = (code + 1) << (cl[s] - prev)
		codes[s] = code; prev = cl[s]
	emit(w, lambda s: w.code(codes[s], cl[s]))
	w.align(); return w.b

def oversubscribed_cl():
	return dyn({16: 1, 17: 1, 18: 1, 0: 1}, lambda w, c: c(16))   # 4 codes of len 1

def no_eob():
	def emit(w, c):
		c(1); c(1)                            # lens[0] = lens[1] = 1
		c(18); w.bits(138 - 11, 7)            # 138 zeros
		c(18); w.bits(117 - 11, 7)            # 117 more, so lens[256] stays 0
		c(1)                                  # the single distance length
	return dyn({18: 1, 1: 1}, emit)

def repeat_overrun():
	def emit(w, c):
		c(1); c(1)
		for _ in range(4): c(18); w.bits(138 - 11, 7)   # 552 lengths, alphabet is 258
	return dyn({18: 1, 1: 1}, emit)

def repeat_no_prev():
	return dyn({16: 1, 1: 1}, lambda w, c: (c(16), w.bits(0, 2)))

def stored_nlen():
	w = BW(); w.bits(1, 1); w.bits(0, 2); w.align()
	w.b += b'\x01\x00\xff\xff' + b'A'         # NLEN should be 0xfffe
	return w.b

def stored_truncated():
	w = BW(); w.bits(1, 1); w.bits(0, 2); w.align()
	w.b += (9999).to_bytes(2, 'little') + (~9999 & 0xffff).to_bytes(2, 'little') + b'A'
	return w.b

cases = [
	('distance before output start', dist_before_start()),
	('invalid length code 286',      bad_length_code()),
	('invalid distance code 30',     bad_dist_code()),
	('over-subscribed CL tree',      oversubscribed_cl()),
	('dynamic block with no EOB',    no_eob()),
	('code-length repeat overrun',   repeat_overrun()),
	('repeat 16 with no previous',   repeat_no_prev()),
	('stored LEN/NLEN mismatch',     stored_nlen()),
	('stored block truncated',       stored_truncated()),
]
with open(out + '.list', 'w') as mf:
	for i, (label, body) in enumerate(cases):
		open('%s.%02d' % (out, i), 'wb').write(wrap(body))
		mf.write(label + '\n')
PY

i=0
while read -r label; do
	f=$(printf '%s.%02d' "$D/evil" "$i")
	gzip -d < "$f" > /dev/null 2>&1; grc=$?
	"$IN" -d < "$f" > /dev/null 2>&1; orc=$?
	if [ "$grc" -ne 0 ] && [ "$orc" -eq 1 ]; then ok
	else bad "hand-built: $label (gzip rc=$grc, ours rc=$orc)"; fi
	i=$((i+1))
done < "$D/evil.list"

# --------------------- 7. no mutation of a valid stream may crash the decoder
# This is the test test-asan exists for: a decompressor parses hostile bytes.
gzip -6 -c < "$F/bytes" | head -c 3000 > "$D/seed.gz"
python3 - "$D/seed.gz" "$D/mut" <<'PY'
import random, sys
random.seed(1951)
b = open(sys.argv[1],'rb').read()
for i in range(300):
	m = bytearray(b)
	for _ in range(random.randint(1,6)):
		m[random.randrange(len(m))] = random.randrange(256)
	open('%s.%03d' % (sys.argv[2], i),'wb').write(m)
PY
crashes=0
for m in "$D"/mut.*; do
	"$IN" -d < "$m" > /dev/null 2>/dev/null
	rc=$?
	# 0 (the mutation happened to stay valid) and 1 (rejected) are both fine.
	# Anything else is a signal, i.e. a bug.
	[ $rc -le 1 ] || { crashes=$((crashes+1)); printf '  crash rc=%s on %s\n' "$rc" "$m"; }
done
check "300 mutated streams: no crash (saw $crashes)" $([ $crashes -eq 0 ]; echo $?)

# ------------------------------------------------------- 8. ratio sanity
# Not a correctness property, but a silent regression to "store everything"
# would still pass every test above, and that is worth catching.
ours=$("$IN" < "$F/text" | wc -c)
theirs=$(gzip -6 -c < "$F/text" | wc -c)
if [ "$ours" -lt $((theirs * 2)) ]; then ok
else bad "ratio: ours $ours vs gzip -6 $theirs on text (want < 2x)"; fi
raw=$(wc -c < "$F/rand")
ours=$("$IN" < "$F/rand" | wc -c)
# Random data must go through stored blocks: at most 0.2% overhead.
if [ "$ours" -lt $((raw + raw / 500)) ]; then ok
else bad "ratio: random data expanded $raw -> $ours (stored path not taken?)"; fi

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
