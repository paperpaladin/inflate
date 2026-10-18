/* inflate - DEFLATE (RFC 1951) and gzip (RFC 1952), decoder and encoder.
 *
 * One file on purpose: the two directions share the gzip framing, the CRC-32
 * table and the RFC 1951 length/distance tables. Splitting them would buy a
 * header file whose only job is to re-join what the RFCs defined together.
 *
 * The authority on correctness here is /usr/bin/gzip, not this code. See
 * t/run.sh.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* shortcut: die() exits without freeing, so LeakSanitizer flags it. Only
 * out-of-memory and write-error paths reach it, neither of which has a
 * useful recovery; wire the buffers to atexit if that ever matters. */
static void die(const char *m) { fprintf(stderr, "inflate: %s\n", m); exit(1); }

/* ---------------------------------------------------------------- buffer */

/* Holds indices, never interior pointers: inflate copies from its own output
 * while that output is being realloc'd, so a cached pointer would dangle. */
typedef struct { uint8_t *p; size_t n, cap; } Buf;

static void bneed(Buf *b, size_t extra) {
	if (b->n + extra <= b->cap) return;
	size_t c = b->cap ? b->cap : 1u << 16;
	while (c < b->n + extra) c <<= 1;
	uint8_t *q = realloc(b->p, c);
	if (!q) die("out of memory");
	b->p = q;
	b->cap = c;
}
static void bput(Buf *b, unsigned v) { bneed(b, 1); b->p[b->n++] = (uint8_t)v; }
static void bwrite(Buf *b, const void *s, size_t n) {
	bneed(b, n);
	memcpy(b->p + b->n, s, n);
	b->n += n;
}

/* ------------------------------------------------------- RFC 1951 tables */

static const uint16_t lbase[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,
	35,43,51,59,67,83,99,115,131,163,195,227,258};
static const uint8_t lext[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,
	4,4,5,5,5,5,0};
static const uint16_t dbase[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,
	257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
static const uint8_t dext[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,
	10,10,11,11,12,12,13,13};

/* RFC 1951 3.2.7: the order the code-length code lengths travel in, arranged so
 * the ones most often nonzero come first and the tail can be dropped. The
 * decoder reads it and the encoder writes it. */
static const uint8_t clord[19] =
	{16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};

/* Extra bits carried by code-length symbols 16, 17 and 18. */
static int clextra(int s) { return s == 16 ? 2 : s == 17 ? 3 : s == 18 ? 7 : 0; }

/* ----------------------------------------------------------------- crc32 */

static uint32_t crctab[256];
static void crcinit(void) {
	for (uint32_t i = 0; i < 256; i++) {
		uint32_t c = i;
		for (int k = 0; k < 8; k++)
			c = (c >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(c & 1));
		crctab[i] = c;
	}
}
static uint32_t crc32b(const uint8_t *p, size_t n) {
	uint32_t c = 0xFFFFFFFFu;
	for (size_t i = 0; i < n; i++) c = crctab[(c ^ p[i]) & 0xff] ^ (c >> 8);
	return c ^ 0xFFFFFFFFu;
}

/* ============================================================== DECODER */

typedef struct { const uint8_t *p; size_t n, i; uint32_t acc; int cnt; } Br;

/* DEFLATE packs bits LSB-first within each byte. need <= 16, so acc never
 * holds more than 23 bits and cannot overflow. */
static int rbits(Br *b, int need, uint32_t *out) {
	while (b->cnt < need) {
		if (b->i >= b->n) return -1;
		b->acc |= (uint32_t)b->p[b->i++] << b->cnt;
		b->cnt += 8;
	}
	*out = b->acc & ((1u << need) - 1);
	b->acc >>= need;
	b->cnt -= need;
	return 0;
}

/* Canonical Huffman as counts-plus-sorted-symbols. Decoding walks one bit at
 * a time instead of using a lookup table: ~15 lines, no table build, and the
 * oracle says it is fast enough. */
typedef struct { uint16_t count[16]; uint16_t sym[288]; } Huf;

/* Returns <0 on over-subscription, 0 complete, >0 slack (incomplete tree). */
static int hbuild(Huf *h, const uint8_t *len, int n) {
	memset(h->count, 0, sizeof h->count);
	for (int i = 0; i < n; i++) h->count[len[i]]++;
	if (h->count[0] == n) return 0; /* empty tree: legal while never used */
	int left = 1;
	for (int l = 1; l < 16; l++) {
		left <<= 1;
		left -= h->count[l];
		if (left < 0) return -1;
	}
	uint16_t off[16];
	off[1] = 0;
	for (int l = 1; l < 15; l++) off[l + 1] = off[l] + h->count[l];
	for (int i = 0; i < n; i++) if (len[i]) h->sym[off[len[i]]++] = (uint16_t)i;
	return left;
}

static int hdec(Br *b, const Huf *h) {
	int code = 0, first = 0, index = 0;
	for (int l = 1; l < 16; l++) {
		uint32_t bit;
		if (rbits(b, 1, &bit)) return -1;
		code |= (int)bit;
		int c = h->count[l];
		if (code - first < c) return h->sym[index + (code - first)];
		index += c;
		first = (first + c) << 1;
		code <<= 1;
	}
	return -1; /* no code matched in 15 bits: corrupt, or an incomplete tree */
}

static Huf fixlit, fixdist;
static void fixinit(void) {
	uint8_t l[288];
	for (int i = 0; i < 144; i++) l[i] = 8;
	for (int i = 144; i < 256; i++) l[i] = 9;
	for (int i = 256; i < 280; i++) l[i] = 7;
	for (int i = 280; i < 288; i++) l[i] = 8;
	hbuild(&fixlit, l, 288);
	/* 32 codes, not 30: RFC 1951 3.2.6 defines the unused 30 and 31 so that
	 * the tree is complete. Dropping them would make it incomplete and change
	 * which bit patterns decode. */
	for (int i = 0; i < 32; i++) l[i] = 5;
	hbuild(&fixdist, l, 32);
}

/* Appends one raw DEFLATE stream to o. Returns NULL on success, else why. */
static const char *inflate_raw(Br *b, Buf *o) {
	uint32_t v, final, type;

	for (;;) {
		if (rbits(b, 1, &final) || rbits(b, 2, &type))
			return "truncated: block header";

		if (type == 3) return "invalid block type 3";

		if (type == 0) {
			b->acc = 0; /* stored blocks resume at the next byte boundary */
			b->cnt = 0;
			if (b->n - b->i < 4) return "truncated: stored header";
			unsigned len = b->p[b->i] | (unsigned)b->p[b->i + 1] << 8;
			unsigned nlen = b->p[b->i + 2] | (unsigned)b->p[b->i + 3] << 8;
			b->i += 4;
			if ((len ^ 0xffffu) != nlen) return "stored block LEN/NLEN mismatch";
			if (b->n - b->i < len) return "truncated: stored data";
			bwrite(o, b->p + b->i, len);
			b->i += len;
		} else {
			Huf dynl, dynd;
			const Huf *lt = &fixlit, *dt = &fixdist;

			if (type == 2) {
				uint32_t hlit, hdist, hclen;
				if (rbits(b, 5, &hlit) || rbits(b, 5, &hdist) ||
				    rbits(b, 4, &hclen))
					return "truncated: dynamic header";
				hlit += 257; hdist += 1; hclen += 4;

				uint8_t cl[19];
				memset(cl, 0, sizeof cl);
				for (uint32_t i = 0; i < hclen; i++) {
					if (rbits(b, 3, &v)) return "truncated: code lengths";
					cl[clord[i]] = (uint8_t)v;
				}
				Huf clh;
				if (hbuild(&clh, cl, 19) < 0)
					return "over-subscribed code-length tree";

				uint8_t lens[288 + 32];
				memset(lens, 0, sizeof lens);
				uint32_t i = 0;
				while (i < hlit + hdist) {
					int s = hdec(b, &clh);
					if (s < 0) return "bad code-length symbol";
					if (s < 16) { lens[i++] = (uint8_t)s; continue; }
					uint8_t val = 0;
					uint32_t rep;
					if (s == 16) {
						if (i == 0) return "repeat with no previous length";
						val = lens[i - 1];
						if (rbits(b, 2, &rep)) return "truncated: repeat";
						rep += 3;
					} else if (s == 17) {
						if (rbits(b, 3, &rep)) return "truncated: repeat";
						rep += 3;
					} else {
						if (rbits(b, 7, &rep)) return "truncated: repeat";
						rep += 11;
					}
					if (rep > hlit + hdist - i)
						return "code-length repeat overruns the alphabet";
					while (rep--) lens[i++] = val;
				}
				if (!lens[256]) return "no end-of-block code";
				if (hbuild(&dynl, lens, (int)hlit) < 0)
					return "over-subscribed literal/length tree";
				if (hbuild(&dynd, lens + hlit, (int)hdist) < 0)
					return "over-subscribed distance tree";
				lt = &dynl;
				dt = &dynd;
			}

			for (;;) {
				int s = hdec(b, lt);
				if (s < 0) return "bad literal/length code";
				if (s < 256) { bput(o, (unsigned)s); continue; }
				if (s == 256) break;
				s -= 257;
				if (s >= 29) return "invalid length code (286/287)";
				if (rbits(b, lext[s], &v)) return "truncated: length extra bits";
				unsigned length = lbase[s] + v;

				int d = hdec(b, dt);
				if (d < 0) return "bad distance code";
				if (d >= 30) return "invalid distance code (30/31)";
				if (rbits(b, dext[d], &v))
					return "truncated: distance extra bits";
				size_t dist = dbase[d] + v;
				if (dist > o->n) return "distance reaches before output start";

				bneed(o, length);
				size_t from = o->n - dist;
				/* Byte at a time, deliberately: dist < length is the RLE case
				 * DEFLATE relies on, where source and destination overlap and
				 * both memcpy and memmove give the wrong answer. */
				for (unsigned k = 0; k < length; k++)
					o->p[o->n + k] = o->p[from + k];
				o->n += length;
			}
		}
		if (final) return NULL;
	}
}

static uint32_t le32(const uint8_t *p) {
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	       (uint32_t)p[3] << 24;
}

/* gzip framing: header, deflate payload, CRC-32 and ISIZE. Handles the
 * concatenated-member streams `cat a.gz b.gz` produces, because gzip -d does. */
static const char *gunzip(const uint8_t *p, size_t n, Buf *o) {
	Br b = {p, n, 0, 0, 0};
	int members = 0;

	for (;;) {
		if (b.i == b.n)
			return members ? NULL : "not gzip: empty input";
		/* Shortest legal member is 10 header + 1 deflate + 8 footer. */
		if (b.n - b.i < 19)
			return members ? "trailing garbage after gzip member"
			               : "truncated gzip header";
		if (b.p[b.i] != 0x1f || b.p[b.i + 1] != 0x8b)
			return members ? "trailing garbage after gzip member"
			               : "not gzip: bad magic";
		if (b.p[b.i + 2] != 8) return "unsupported compression method";
		uint8_t flg = b.p[b.i + 3];
		if (flg & 0xe0) return "reserved gzip flag bits set";
		b.i += 10;

		if (flg & 4) { /* FEXTRA */
			if (b.n - b.i < 2) return "truncated FEXTRA";
			size_t xl = b.p[b.i] | (size_t)b.p[b.i + 1] << 8;
			b.i += 2;
			if (b.n - b.i < xl) return "truncated FEXTRA";
			b.i += xl;
		}
		if (flg & 8) { /* FNAME: gzip sets this whenever it compresses a file */
			while (b.i < b.n && b.p[b.i]) b.i++;
			if (b.i >= b.n) return "truncated FNAME";
			b.i++;
		}
		if (flg & 16) { /* FCOMMENT */
			while (b.i < b.n && b.p[b.i]) b.i++;
			if (b.i >= b.n) return "truncated FCOMMENT";
			b.i++;
		}
		/* shortcut: FHCRC is skipped, not verified. The payload CRC-32 already
		 * catches corruption; add the header CRC16 if a caller needs to reject
		 * a bad header before spending time inflating. */
		if (flg & 2) {
			if (b.n - b.i < 2) return "truncated FHCRC";
			b.i += 2;
		}

		size_t start = o->n;
		b.acc = 0;
		b.cnt = 0;
		const char *e = inflate_raw(&b, o);
		if (e) return e;

		/* b.i already points past the last byte the bit reader touched, and
		 * the footer is byte-aligned, so leftover bits are simply padding. */
		b.acc = 0;
		b.cnt = 0;
		if (b.n - b.i < 8) return "truncated gzip footer";
		uint32_t want = le32(b.p + b.i), isize = le32(b.p + b.i + 4);
		b.i += 8;
		if (crc32b(o->p + start, o->n - start) != want) return "CRC-32 mismatch";
		if ((uint32_t)(o->n - start) != isize) return "ISIZE mismatch";
		members++;
	}
}

/* ============================================================== ENCODER */

#define HBITS 15
#define HSIZE (1u << HBITS)
#define NIL 0xffffffffu
#define MINM 3
#define MAXM 258
#define WIN 32768
/*
 * None of these three is a block size: blocks end where the cost says they
 * should. A chunk is the unit a boundary can fall on, and a block is any run
 * of chunks the cost said belonged together, which on uniform input is the
 * whole buffer and at a change of distribution is one chunk.
 *
 * A chunk ends at CHUNK tokens or CHUNKIN input bytes, whichever comes first,
 * and the byte bound is not redundant. Tokens alone are the wrong unit when
 * matches are long: 4096 tokens of 258-byte matches cover a megabyte, and the
 * nearest boundary to a change of distribution is then half a megabyte away.
 * Dropping CHUNKIN costs 3.5% on 20 copies of this file. Both values were
 * swept, and the spread across 1024..32768 was under 0.5% on three corpora,
 * so neither is a tuned constant so much as a resolution.
 *
 * shortcut: MAXTOK bounds how far a block can grow, so a perfectly uniform
 * file pays a tree header every million tokens whether or not it is worth one.
 * The buffer is what forces it; streaming the encoder (item 24) is where that
 * gets revisited.
 */
#define CHUNK 4096
#define CHUNKIN 8192
#define MAXTOK (1u << 20)
#define STOREDMAX 65535 /* a stored block's LEN field is 16 bits */
/*
 * -1..-9, which until now parsed and did nothing.
 *
 * Two knobs, both measured rather than guessed. `chain` is how far back the
 * hash chain is walked before giving up, and `lazy` is whether a match is
 * checked against the one starting a byte later. Lazy matching costs 1.18x
 * the encode time for 3.9% on text and nothing at all on input whose matches
 * already run to the 258-byte cap, so turning it off is exactly what the low
 * levels are for.
 *
 * Level 6 is the default because it is gzip's, and this is a drop-in.
 */
static const struct { uint16_t chain; uint8_t lazy; } level[10] = {
	{0, 0},      /* unused: there is no -0 */
	{4, 0}, {8, 0}, {16, 0},          /* 1-3: no lookahead, short chains */
	{32, 1}, {64, 1}, {128, 1},       /* 4-6: 6 is what the old code did */
	{256, 1}, {512, 1}, {1024, 1},    /* 7-9: deeper search, same strategy */
};

static unsigned chain_limit = 128;
static int lazy_match = 1;

typedef struct { Buf *o; uint32_t acc; int nbits; size_t total; } Bw;

static void wbits(Bw *w, uint32_t v, int n) {
	w->acc |= (v & ((1u << n) - 1)) << w->nbits;
	w->nbits += n;
	w->total += (size_t)n; /* what the cost model is checked against */
	while (w->nbits >= 8) {
		bput(w->o, w->acc & 0xff);
		w->acc >>= 8;
		w->nbits -= 8;
	}
}
static void walign(Bw *w) {
	if (w->nbits) { bput(w->o, w->acc & 0xff); w->acc = 0; w->nbits = 0; }
}
/* Huffman codes are defined MSB-first but the stream is LSB-first, so every
 * code goes out reversed. Extra bits are not codes and go out as-is. */
static void wcode(Bw *w, uint32_t code, int n) {
	uint32_t r = 0;
	for (int i = 0; i < n; i++) { r = (r << 1) | (code & 1); code >>= 1; }
	wbits(w, r, n);
}

/* RFC 1951 3.2.6 fixed literal/length code. */
static void wfix(Bw *w, int sym) {
	if (sym < 144) wcode(w, 0x30u + (uint32_t)sym, 8);
	else if (sym < 256) wcode(w, 0x190u + (uint32_t)sym - 144, 9);
	else if (sym < 280) wcode(w, (uint32_t)sym - 256, 7);
	else wcode(w, 0xC0u + (uint32_t)sym - 280, 8);
}
static int fixbits(int sym) {
	return sym < 144 ? 8 : sym < 256 ? 9 : sym < 280 ? 7 : 8;
}
static int lcode(unsigned len) { int i = 28; while (lbase[i] > len) i--; return i; }
static int dcode(unsigned d) { int i = 29; while (dbase[i] > d) i--; return i; }

typedef struct { uint16_t v, dist; } Tok; /* dist==0 => v is a literal byte */

/* ------------------------------------------- encoder-side Huffman trees */

#define NLIT 286 /* literal/length symbols we can emit: 0..285 */
#define NDIST 30
#define NCL 19
#define MAXSYM 288

/* One Huffman build: merge the two lightest live nodes until one is left, then
 * read each leaf's depth off the parent chain. Returns the longest length, so
 * the caller can tell whether the result is legal.
 *
 * shortcut: linear scan for the two lightest rather than a heap. n is 286 at
 * worst, and the boundary search runs it three times -- literal/length,
 * distance, code-length -- for each of the two or three candidate blocks it
 * prices per chunk, plus once more per halving. That is the hot path now, and
 * it still does not reach a profile: 4% more encode time than the fixed 32KB
 * boundary it replaced. Swap in a heap if the chunk ever gets smaller. */
static int huf1(const uint32_t *freq, int n, uint8_t *len) {
	uint32_t w[2 * MAXSYM];
	int dad[2 * MAXSYM], live[MAXSYM], nlive = 0, next = n;

	for (int i = 0; i < n; i++) {
		len[i] = 0;
		dad[i] = -2; /* -2: unused symbol, not a leaf of this tree at all */
		if (freq[i]) { w[i] = freq[i]; dad[i] = -1; live[nlive++] = i; }
	}
	/* RFC 1951 has no zero-bit code, so an alphabet used once or not at all
	 * still needs two leaves to make a complete tree. A block of pure literals
	 * reaches here: it carries a distance tree it never consults. */
	for (int i = 0; i < n && nlive < 2; i++)
		if (dad[i] == -2) { w[i] = 1; dad[i] = -1; live[nlive++] = i; }

	while (nlive > 1) {
		int a = 0, b = 1;
		if (w[live[1]] < w[live[0]]) { a = 1; b = 0; }
		for (int i = 2; i < nlive; i++) {
			if (w[live[i]] < w[live[a]]) { b = a; a = i; }
			else if (w[live[i]] < w[live[b]]) b = i;
		}
		int x = live[a], y = live[b];
		w[next] = w[x] + w[y];
		dad[x] = dad[y] = next;
		dad[next] = -1; /* overwritten unless this turns out to be the root */
		live[a] = next++;
		live[b] = live[--nlive];
	}

	int longest = 0;
	for (int i = 0; i < n; i++) {
		if (dad[i] == -2) continue;
		int d = 0;
		for (int p = dad[i]; p >= 0; p = dad[p]) d++;
		len[i] = (uint8_t)d;
		if (d > longest) longest = d;
	}
	return longest;
}

/* Code lengths for freq[0..n), none longer than maxlen bits.
 *
 * shortcut: over-long codes are corrected by halving every frequency and
 * rebuilding, not by package-merge. Halving strictly shrinks every weight above
 * 1, so the loop ends at worst with a flat distribution, and 288 equally likely
 * symbols is a 9-bit tree -- comfortably inside 15. The cost is a slightly
 * suboptimal code on the rare block whose natural tree is deeper than the
 * format allows. Package-merge is the upgrade if that ever shows up in a ratio
 * measurement rather than in theory. */
static void huflen(const uint32_t *freq, int n, int maxlen, uint8_t *len) {
	uint32_t f[MAXSYM];
	memcpy(f, freq, (size_t)n * sizeof *f);
	while (huf1(f, n, len) > maxlen)
		for (int i = 0; i < n; i++) f[i] = (f[i] + 1) / 2; /* 0 stays 0, 1 stays 1 */
}

/* Canonical codes from lengths, RFC 1951 3.2.2: shorter codes numerically
 * first, and within one length, lower symbol numbers first. */
static void hufcode(const uint8_t *len, int n, uint16_t *code) {
	uint16_t cnt[16] = {0}, next[16];
	for (int i = 0; i < n; i++) cnt[len[i]]++;
	cnt[0] = 0; /* unused symbols get no code and must not shift the others */
	uint16_t c = 0;
	for (int l = 1; l < 16; l++) next[l] = c = (uint16_t)((c + cnt[l - 1]) << 1);
	for (int i = 0; i < n; i++) if (len[i]) code[i] = next[len[i]]++;
}

/* Run-length codes the concatenated literal/length and distance lengths into
 * the 19-symbol code-length alphabet (RFC 1951 3.2.7), writing one symbol per
 * slot of sym[] with its extra-bit payload in extra[]. Returns how many.
 *
 * Repeats are allowed to cross from the last literal/length code into the first
 * distance code; the RFC says so explicitly, and the decoder above reads both
 * halves out of one array for exactly that reason. */
static int clpack(const uint8_t *len, int n, uint8_t *sym, uint8_t *extra) {
	int m = 0;
	for (int i = 0; i < n; ) {
		int v = len[i], run = 1;
		while (i + run < n && len[i + run] == v) run++;
		i += run;
		if (v == 0) {
			while (run >= 11) {
				int r = run > 138 ? 138 : run;
				sym[m] = 18; extra[m++] = (uint8_t)(r - 11); run -= r;
			}
			if (run >= 3) { sym[m] = 17; extra[m++] = (uint8_t)(run - 3); run = 0; }
		} else {
			/* Symbol 16 repeats the previous length, so the first one is
			 * always spelled out. */
			sym[m] = (uint8_t)v; extra[m++] = 0;
			run--;
			while (run >= 3) {
				int r = run > 6 ? 6 : run;
				sym[m] = 16; extra[m++] = (uint8_t)(r - 3); run -= r;
			}
		}
		while (run-- > 0) { sym[m] = (uint8_t)v; extra[m++] = 0; }
	}
	return m;
}

static uint32_t hash3(const uint8_t *p) {
	uint32_t k = (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
	return (k * 2654435761u) >> (32 - HBITS);
}

/* Longest match for the bytes at `pos`, searching the hash chain back at most
 * CHAIN links and WIN bytes. Returns 0 if nothing reaches MINM, otherwise the
 * length, with the distance in *distp.
 *
 * It reads head[] and never writes it, which is what lets the caller run it at
 * pos and again at pos+1 without touching the chains in between. Inserting a
 * position before searching it would find the position itself at distance 0,
 * and inserting one twice leaves prev[p] == p, which is a chain that never
 * ends. */
static unsigned match_at(const uint8_t *in, size_t n, size_t pos,
                         const uint32_t *head, const uint32_t *prev,
                         size_t *distp)
{
	unsigned best = 0;
	size_t bestd = 0;

	if (n - pos >= MINM) {
		size_t max = n - pos < MAXM ? n - pos : MAXM;
		uint32_t c = head[hash3(in + pos)];
		for (unsigned tries = chain_limit; c != NIL && tries; tries--) {
			if (pos - c > WIN) break;
			if (in[c + best] == in[pos + best]) { /* cheap reject */
				unsigned k = 0;
				while (k < max && in[c + k] == in[pos + k]) k++;
				if (k > best) {
					best = k;
					bestd = pos - c;
					if (best == max) break;
				}
			}
			c = prev[c];
		}
	}
	*distp = bestd;
	return best < MINM ? 0 : best;
}

/* ---------------------------------------- cost-chosen block boundaries */

/*
 * Symbol counts for a span of tokens, plus what that span covers. A block's
 * cost depends only on these counts, never on the tokens themselves, which is
 * what makes a boundary search affordable: merging a candidate chunk into the
 * running block is 316 additions rather than a second pass over a million
 * tokens.
 */
typedef struct {
	uint32_t l[NLIT], d[NDIST];
	size_t bytes, ntok;
} Fr;

static void fr_zero(Fr *f) { memset(f, 0, sizeof *f); }

static void fr_add(Fr *a, const Fr *b) {
	for (int i = 0; i < NLIT; i++) a->l[i] += b->l[i];
	for (int i = 0; i < NDIST; i++) a->d[i] += b->d[i];
	a->bytes += b->bytes;
	a->ntok += b->ntok;
}

static void fr_tok(Fr *f, Tok t) {
	if (!t.dist) {
		f->l[t.v]++;
		f->bytes += 1;
	} else {
		f->l[257 + lcode(t.v)]++;
		f->d[dcode(t.dist)]++;
		f->bytes += t.v;
	}
	f->ntok++;
}

/* Everything the dynamic branch puts on the wire, and which block type won. */
typedef struct {
	uint8_t llen[NLIT], dlen[NDIST], cllen[NCL];
	uint16_t lcod[NLIT], dcod[NDIST], clcod[NCL];
	uint8_t cls[NLIT + NDIST], clx[NLIT + NDIST];
	int ncls, nlit, ndist, hclen;
	int type; /* 0 stored, 1 fixed, 2 dynamic */
} Trees;

/* Trees fitted to these counts, plus the exact bit cost of the dynamic block
 * that would carry them, header included. The fixed codes are a guess, made
 * once in RFC 1951, about what a block looks like; on this project's own
 * source they cost 35% over gzip -6 and no amount of better matching recovers
 * it, because the frequencies never reach a tree at all.
 *
 * The cost is summed per symbol rather than per token, which is the same
 * number and is what lets a candidate boundary be priced without walking the
 * tokens again. */
static long dyn_trees(const Fr *f, Trees *t) {
	uint32_t lfreq[NLIT];
	memcpy(lfreq, f->l, sizeof lfreq);
	lfreq[256] = 1; /* end of block, emitted exactly once */

	huflen(lfreq, NLIT, 15, t->llen);
	huflen(f->d, NDIST, 15, t->dlen);
	hufcode(t->llen, NLIT, t->lcod);
	hufcode(t->dlen, NDIST, t->dcod);

	/* Trailing unused lengths are not sent; RFC 1951 floors the counts at
	 * 257 literal/length codes and 1 distance code. */
	t->nlit = NLIT;
	while (t->nlit > 257 && !t->llen[t->nlit - 1]) t->nlit--;
	t->ndist = NDIST;
	while (t->ndist > 1 && !t->dlen[t->ndist - 1]) t->ndist--;

	uint8_t both[NLIT + NDIST];
	memcpy(both, t->llen, (size_t)t->nlit);
	memcpy(both + t->nlit, t->dlen, (size_t)t->ndist);
	t->ncls = clpack(both, t->nlit + t->ndist, t->cls, t->clx);

	uint32_t cfreq[NCL] = {0};
	for (int i = 0; i < t->ncls; i++) cfreq[t->cls[i]]++;
	huflen(cfreq, NCL, 7, t->cllen); /* 3 bits each on the wire, so 7 is the cap */
	hufcode(t->cllen, NCL, t->clcod);
	t->hclen = NCL;
	while (t->hclen > 4 && !t->cllen[clord[t->hclen - 1]]) t->hclen--;

	long bits = 3 + 5 + 5 + 4 + 3 * (long)t->hclen;
	for (int i = 0; i < t->ncls; i++)
		bits += t->cllen[t->cls[i]] + clextra(t->cls[i]);
	for (int s = 0; s < NLIT; s++) bits += (long)lfreq[s] * t->llen[s];
	for (int lc = 0; lc < 29; lc++) bits += (long)lfreq[257 + lc] * lext[lc];
	for (int dc = 0; dc < NDIST; dc++)
		bits += (long)f->d[dc] * (t->dlen[dc] + dext[dc]);
	return bits;
}

static long fixed_cost(const Fr *f) {
	long bits = 3 + 7; /* block header, plus the 7-bit fixed code for EOB */
	for (int s = 0; s < 256; s++) bits += (long)f->l[s] * fixbits(s);
	for (int lc = 0; lc < 29; lc++)
		bits += (long)f->l[257 + lc] * (fixbits(257 + lc) + lext[lc]);
	for (int dc = 0; dc < NDIST; dc++)
		bits += (long)f->d[dc] * (5 + dext[dc]);
	return bits;
}

/* A stored block's LEN is 16 bits, so a long one goes out in pieces and each
 * piece pays its own header, byte alignment and LEN/NLEN pair. Before blocks
 * could outgrow 32KB this could not happen. */
static long stored_cost(size_t bytes) {
	size_t parts = bytes ? (bytes + STOREDMAX - 1) / STOREDMAX : 1;
	return (long)parts * (3 + 7 + 32) + 8 * (long)bytes; /* 7 = worst-case padding */
}

/* What this span would actually cost, by the cheapest of the three block
 * types, with the dynamic trees built as a side effect. */
static long block_cost(const Fr *f, Trees *t) {
	long best = dyn_trees(f, t), fix = fixed_cost(f), st = stored_cost(f->bytes);
	t->type = 2;
	if (fix < best) { best = fix; t->type = 1; }
	if (st < best) { best = st; t->type = 0; }
	return best;
}

static void emit_block(Bw *w, const uint8_t *in, size_t bstart, const Fr *f,
                       const Tok *tok, size_t lo, size_t hi, int final)
{
	Trees t;
	long cost = block_cost(f, &t);
	size_t before = w->total;

	if (t.type == 2) {
		wbits(w, (uint32_t)final, 1);
		wbits(w, 2, 2); /* BTYPE 10: dynamic Huffman */
		wbits(w, (uint32_t)(t.nlit - 257), 5);
		wbits(w, (uint32_t)(t.ndist - 1), 5);
		wbits(w, (uint32_t)(t.hclen - 4), 4);
		for (int i = 0; i < t.hclen; i++) wbits(w, t.cllen[clord[i]], 3);
		for (int i = 0; i < t.ncls; i++) {
			wcode(w, t.clcod[t.cls[i]], t.cllen[t.cls[i]]);
			wbits(w, t.clx[i], clextra(t.cls[i]));
		}
		for (size_t i = lo; i < hi; i++) {
			if (!tok[i].dist) {
				wcode(w, t.lcod[tok[i].v], t.llen[tok[i].v]);
				continue;
			}
			int lc = lcode(tok[i].v), dc = dcode(tok[i].dist);
			wcode(w, t.lcod[257 + lc], t.llen[257 + lc]);
			wbits(w, (uint32_t)(tok[i].v - lbase[lc]), lext[lc]);
			wcode(w, t.dcod[dc], t.dlen[dc]);
			wbits(w, (uint32_t)(tok[i].dist - dbase[dc]), dext[dc]);
		}
		wcode(w, t.lcod[256], t.llen[256]);
	} else if (t.type == 1) {
		wbits(w, (uint32_t)final, 1);
		wbits(w, 1, 2); /* BTYPE 01: fixed Huffman */
		for (size_t i = lo; i < hi; i++) {
			if (!tok[i].dist) { wfix(w, tok[i].v); continue; }
			int lc = lcode(tok[i].v), dc = dcode(tok[i].dist);
			wfix(w, 257 + lc);
			wbits(w, (uint32_t)(tok[i].v - lbase[lc]), lext[lc]);
			wcode(w, (uint32_t)dc, 5);
			wbits(w, (uint32_t)(tok[i].dist - dbase[dc]), dext[dc]);
		}
		wfix(w, 256);
	} else {
		/* Incompressible run: a stored block is 5 bytes of overhead instead
		 * of the ~5% a Huffman pass costs on random data. */
		size_t left = f->bytes, off = bstart;
		do {
			unsigned L = left > STOREDMAX ? STOREDMAX : (unsigned)left;
			wbits(w, (uint32_t)(final && left == L), 1);
			wbits(w, 0, 2);
			walign(w);
			unsigned N = ~L & 0xffffu;
			bput(w->o, L & 0xff);
			bput(w->o, L >> 8);
			bput(w->o, N & 0xff);
			bput(w->o, N >> 8);
			bwrite(w->o, in + off, L);
			off += L;
			left -= L;
		} while (left);
	}

	/*
	 * The boundary chooser spent its whole run comparing predicted costs, so
	 * a prediction that does not match what the writer emitted means every
	 * boundary in the file was picked on a fiction. Checked for the two coded
	 * types, which are accounted bit for bit; a stored block's byte alignment
	 * is a worst case, not a count.
	 */
	if (t.type && w->total - before != (size_t)cost)
		die("internal: block cost mispredicted");
}

static void deflate_gz(const uint8_t *in, size_t n, Buf *o) {
	/* MTIME 0 and OS 255 ("unknown"): no clock read, so output is
	 * byte-for-byte reproducible, which the round-trip test depends on. */
	static const uint8_t hdr[10] = {0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 255};
	bwrite(o, hdr, 10);

	uint32_t *head = malloc(HSIZE * sizeof *head);
	/* shortcut: prev[] is one slot per input byte rather than one per window
	 * slot, so peak memory is 4n. Switch to a 32K ring and stream the input
	 * when a file stops fitting in RAM. */
	uint32_t *prev = malloc((n ? n : 1) * sizeof *prev);
	/* One token per input byte is the worst case, so a small input never
	 * pays for the whole buffer. */
	size_t cap = n + 1 < MAXTOK ? n + 1 : MAXTOK;
	Tok *tok = malloc(cap * sizeof *tok);
	if (!head || !prev || !tok) die("out of memory");
	for (uint32_t i = 0; i < HSIZE; i++) head[i] = NIL;

	Bw w = {o, 0, 0, 0};
	size_t pos = 0;    /* next input byte to tokenize */
	size_t ntok = 0;   /* tokens buffered in this span */
	size_t btok = 0;   /* first token of the block being accumulated */
	size_t bstart = 0; /* input offset of that block */
	Fr a, c, b;        /* block so far, candidate chunk, the two merged */
	long ca = 0;       /* cost of `a` as its own block */
	fr_zero(&a);

	for (;;) { /* one pass per chunk, so a zero-byte input still emits a block */
		size_t cstart = ntok;
		fr_zero(&c);

		while (pos < n && ntok - cstart < CHUNK && c.bytes < CHUNKIN && ntok < cap) {
			size_t bestd;
			unsigned best = match_at(in, n, pos, head, prev, &bestd);

			/*
			 * Lazy evaluation. A greedy matcher takes this match; starting
			 * one byte later can give a longer one, and when it does, the
			 * byte at pos is cheaper spent as a literal. Only a strictly
			 * longer match justifies it -- equal length would cost the
			 * literal for nothing.
			 *
			 * Nothing is inserted into the chains before the lookahead, so
			 * the search at pos+1 cannot see pos itself. The only match that
			 * loses is a distance-1 run, which the search at pos has already
			 * found and which therefore wins the comparison anyway.
			 */
			if (lazy_match && best && best < MAXM && pos + 1 < n) {
				size_t nextd;
				unsigned next = match_at(in, n, pos + 1, head, prev, &nextd);
				if (next > best) {
					tok[ntok].v = in[pos];
					tok[ntok].dist = 0;
					fr_tok(&c, tok[ntok]);
					ntok++;
					if (n - pos >= MINM) {
						uint32_t h = hash3(in + pos);
						prev[pos] = head[h];
						head[h] = (uint32_t)pos;
					}
					pos++;
					continue;
				}
			}

			unsigned adv = best ? best : 1;
			if (best) {
				tok[ntok].v = (uint16_t)best;
				tok[ntok].dist = (uint16_t)bestd;
			} else {
				tok[ntok].v = in[pos];
				tok[ntok].dist = 0;
			}
			fr_tok(&c, tok[ntok]);
			ntok++;

			/* Insert every covered position, not just block starts: a match
			 * that skips bytes still has to leave them findable. */
			for (unsigned k = 0; k < adv; k++) {
				size_t p = pos + k;
				if (n - p < MINM) break;
				uint32_t h = hash3(in + p);
				prev[p] = head[h];
				head[h] = (uint32_t)p;
			}
			pos += adv;
		}

		int at_end = pos >= n, full = ntok >= cap;

		/*
		 * The boundary decision, and the whole point of this shape. A block
		 * ends where the symbol distribution changes, not on a round number:
		 * if this chunk is cheaper carrying its own tree than folded into the
		 * block in progress, the block ends here. On uniform input the merged
		 * cost always wins, because one header is cheaper than two, and the
		 * block grows until the buffer forces a boundary.
		 */
		if (c.ntok) {
			Trees t;
			if (!a.ntok) {
				a = c;
				ca = block_cost(&a, &t);
			} else {
				long cc = block_cost(&c, &t);
				b = a;
				fr_add(&b, &c);
				long cb = block_cost(&b, &t);
				if (ca + cc < cb) {
					emit_block(&w, in, bstart, &a, tok, btok, cstart, 0);
					bstart += a.bytes;
					btok = cstart;
					a = c;
					ca = cc;
				} else {
					a = b;
					ca = cb;
				}
			}
		}

		if (at_end || full) {
			emit_block(&w, in, bstart, &a, tok, btok, ntok, at_end);
			if (at_end) break;
			/* Buffer full: the span restarts, which forces a boundary here. */
			bstart = pos;
			btok = ntok = 0;
			fr_zero(&a);
			ca = 0;
		}
	}

	walign(&w);
	uint32_t crc = crc32b(in, n);
	for (int i = 0; i < 4; i++) bput(o, (crc >> (8 * i)) & 0xff);
	uint32_t sz = (uint32_t)n; /* RFC 1952: input size modulo 2^32 */
	for (int i = 0; i < 4; i++) bput(o, (sz >> (8 * i)) & 0xff);

	free(head);
	free(prev);
	free(tok);
}

/* ================================================================== main */

static const char usage[] =
	"usage: inflate [-d] < in > out\n"
	"  (no flag)  compress stdin to a gzip stream on stdout\n"
	"  -d         decompress a gzip stream from stdin to stdout\n";

int main(int argc, char **argv) {
	int dec = 0;
	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		if (!strcmp(a, "-d")) dec = 1;
		else if (!strcmp(a, "-c")) ; /* accepted, ignored: stdout is the only sink */
		else if (a[0] == '-' && a[1] >= '1' && a[1] <= '9' && !a[2]) {
			chain_limit = level[a[1] - '0'].chain;
			lazy_match = level[a[1] - '0'].lazy;
		}
		else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { fputs(usage, stdout); return 0; }
		else { fprintf(stderr, "inflate: unknown option %s\n%s", a, usage); return 2; }
	}

	crcinit();
	fixinit();

	Buf in = {0};
	for (;;) {
		bneed(&in, 1u << 16);
		size_t got = fread(in.p + in.n, 1, in.cap - in.n, stdin);
		in.n += got;
		if (got == 0) break;
	}
	if (ferror(stdin)) die("read error on stdin");

	Buf out = {0};
	bneed(&out, 1); /* so out.p is never NULL and pointer arithmetic is defined */

	/* Single exit, and it frees: returning straight out of the error branch
	 * leaked both buffers on every rejected stream, and LeakSanitizer had been
	 * reporting it unseen because its exit code is also 1. */
	int rc = 0;
	if (dec) {
		const char *e = gunzip(in.p, in.n, &out);
		if (e) { fprintf(stderr, "inflate: %s\n", e); rc = 1; }
	} else {
		deflate_gz(in.p, in.n, &out);
	}

	if (!rc) {
		if (out.n && fwrite(out.p, 1, out.n, stdout) != out.n) die("write error");
		if (fflush(stdout)) die("write error");
	}
	free(in.p);
	free(out.p);
	return rc;
}
