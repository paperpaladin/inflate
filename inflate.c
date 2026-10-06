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

	/* Shortest legal member is 10 header + 1 deflate byte + 8 footer. */
	if (!n) return "not gzip: empty input";
	if (n < 19) return "truncated gzip header";
	if (b.p[0] != 0x1f || b.p[1] != 0x8b) return "not gzip: bad magic";
	if (b.p[2] != 8) return "unsupported compression method";
	/* shortcut: optional header fields and concatenated members come later.
	 * Rejecting beats skipping bytes we have not learned to parse. */
	if (b.p[3]) return "gzip header option fields not supported yet";
	b.i = 10;

	const char *e = inflate_raw(&b, o);
	if (e) return e;

	/* b.i already points past the last byte the bit reader touched and the
	 * footer is byte-aligned, so leftover bits are simply padding. */
	b.acc = 0;
	b.cnt = 0;
	if (b.n - b.i < 8) return "truncated gzip footer";
	uint32_t want = le32(b.p + b.i), isize = le32(b.p + b.i + 4);
	b.i += 8;
	if (crc32b(o->p, o->n) != want) return "CRC-32 mismatch";
	if ((uint32_t)o->n != isize) return "ISIZE mismatch";
	if (b.i != b.n) return "trailing garbage after gzip member";
	return NULL;
}

/* ================================================================== main */

static const char usage[] =
	"usage: inflate -d < in.gz > out\n"
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

	if (dec) {
		const char *e = gunzip(in.p, in.n, &out);
		if (e) { fprintf(stderr, "inflate: %s\n", e); return 1; }
	} else {
		/* shortcut: the encoder lands later. Refusing beats silently copying -
		 * a caller who forgot -d would otherwise get plain text named .gz. */
		die("compression not implemented yet; use -d to decompress");
	}

	if (out.n && fwrite(out.p, 1, out.n, stdout) != out.n) die("write error");
	if (fflush(stdout)) die("write error");
	free(in.p);
	free(out.p);
	return 0;
}
