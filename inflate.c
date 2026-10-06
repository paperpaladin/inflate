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
	static const uint8_t ord[19] =
		{16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
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
					cl[ord[i]] = (uint8_t)v;
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
#define BLKIN 32768 /* input bytes per block; one stored block can hold 65535 */
#define CHAIN 128   /* shortcut: one fixed chain limit, no -1..-9 levels */

typedef struct { Buf *o; uint32_t acc; int nbits; } Bw;

static void wbits(Bw *w, uint32_t v, int n) {
	w->acc |= (v & ((1u << n) - 1)) << w->nbits;
	w->nbits += n;
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

static uint32_t hash3(const uint8_t *p) {
	uint32_t k = (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
	return (k * 2654435761u) >> (32 - HBITS);
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
	Tok *tok = malloc((BLKIN + 1) * sizeof *tok);
	if (!head || !prev || !tok) die("out of memory");
	for (uint32_t i = 0; i < HSIZE; i++) head[i] = NIL;

	Bw w = {o, 0, 0};
	size_t pos = 0;

	do { /* do/while so a zero-byte input still emits one final empty block */
		size_t bstart = pos, ntok = 0;
		long bits = 3 + 7; /* block header + end-of-block symbol */

		while (pos < n && pos - bstart < BLKIN) {
			unsigned best = 0;
			size_t bestd = 0;
			if (n - pos >= MINM) {
				size_t max = n - pos < MAXM ? n - pos : MAXM;
				uint32_t c = head[hash3(in + pos)];
				for (int tries = CHAIN; c != NIL && tries; tries--) {
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
			if (best < MINM) best = 0;

			unsigned adv = best ? best : 1;
			if (best) {
				int lc = lcode(best), dc = dcode((unsigned)bestd);
				tok[ntok].v = (uint16_t)best;
				tok[ntok].dist = (uint16_t)bestd;
				bits += fixbits(257 + lc) + lext[lc] + 5 + dext[dc];
			} else {
				tok[ntok].v = in[pos];
				tok[ntok].dist = 0;
				bits += fixbits(in[pos]);
			}
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

		size_t blen = pos - bstart;
		long stored = 3 + 7 + 32 + 8 * (long)blen; /* 7 = worst-case padding */
		unsigned final = pos >= n;

		if (bits <= stored) {
			wbits(&w, final, 1);
			wbits(&w, 1, 2); /* BTYPE 01: fixed Huffman */
			for (size_t i = 0; i < ntok; i++) {
				if (!tok[i].dist) { wfix(&w, tok[i].v); continue; }
				int lc = lcode(tok[i].v), dc = dcode(tok[i].dist);
				wfix(&w, 257 + lc);
				wbits(&w, (uint32_t)(tok[i].v - lbase[lc]), lext[lc]);
				wcode(&w, (uint32_t)dc, 5);
				wbits(&w, (uint32_t)(tok[i].dist - dbase[dc]), dext[dc]);
			}
			wfix(&w, 256);
		} else {
			/* Incompressible run: a stored block is 5 bytes of overhead
			 * instead of the ~5% a Huffman pass costs on random data. */
			wbits(&w, final, 1);
			wbits(&w, 0, 2);
			walign(&w);
			unsigned L = (unsigned)blen, N = ~L & 0xffffu;
			bput(o, L & 0xff);
			bput(o, L >> 8);
			bput(o, N & 0xff);
			bput(o, N >> 8);
			bwrite(o, in + bstart, blen);
		}
	} while (pos < n);

	walign(&w);
	uint32_t c = crc32b(in, n);
	for (int i = 0; i < 4; i++) bput(o, (c >> (8 * i)) & 0xff);
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
		else if (a[0] == '-' && a[1] >= '1' && a[1] <= '9' && !a[2]) ;
			/* shortcut: -1..-9 parse but do nothing, there is one strategy.
			 * Wire them to CHAIN and a lazy-match threshold if ratio matters. */
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
