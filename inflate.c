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
static void bwrite(Buf *b, const void *s, size_t n) {
	bneed(b, n);
	memcpy(b->p + b->n, s, n);
	b->n += n;
}

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

/* Appends one raw DEFLATE stream to o. Returns NULL on success, else why. */
static const char *inflate_raw(Br *b, Buf *o) {
	uint32_t final, type;

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
			/* shortcut: Huffman blocks land in the next commit. Stored blocks
			 * alone are enough to put the gzip framing, the CRC-32 and ISIZE
			 * under the oracle. */
			return "Huffman blocks not supported yet";
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
