/* From-scratch DEFLATE (RFC 1951) / gzip (RFC 1952) decoder, adapted
 * from the well-known "puff" reference-decoder approach (bit-at-a-time
 * canonical Huffman decoding, no lookup-table construction needed) --
 * small and easy to verify by hand rather than fast. Good enough to
 * unwrap Content-Encoding: gzip/deflate on HTTP responses, which is
 * all this OS's browser needs it for. */
#include <net/gzip.h>
#include <string.h>

#define MAXBITS    15
#define MAXLCODES  286
#define MAXDCODES  30
#define MAXCODES   (MAXLCODES + MAXDCODES)
#define FIXLCODES  288
#define HUFFSYMS   288 /* big enough for either a literal/length or distance table */

struct bitstate {
    const uint8_t *in;
    uint32_t inlen;
    uint32_t incnt;
    uint32_t bitbuf;
    int bitcnt;

    uint8_t *out;
    uint32_t outcap;
    uint32_t outcnt;
    int error;
};

struct huffman {
    short count[MAXBITS + 1];
    short symbol[HUFFSYMS];
};

/* LSB-first bit reader -- every DEFLATE field except the Huffman codes
 * themselves (see decode() below) is packed this way. */
static int bits(struct bitstate *s, int need) {
    uint32_t val = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->incnt >= s->inlen) { s->error = 1; return 0; }
        val |= (uint32_t)(s->in[s->incnt++]) << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = val >> need;
    s->bitcnt -= need;
    return (int)(val & ((1u << need) - 1));
}

/* Returns 1 (and writes nothing) once the output cap is reached -- the
 * caller treats that as "stop cleanly, keep what we have" rather than
 * an error, same convention http.c's decode_chunked() uses. */
static int out_byte(struct bitstate *s, uint8_t b) {
    if (s->outcnt >= s->outcap) return 1;
    s->out[s->outcnt++] = b;
    return 0;
}

/* Canonical Huffman decode, one bit at a time: rebuilds the MSB-first
 * code value directly from bits read off the (LSB-first-packed) stream
 * -- the first bit taken IS the code's high-order bit per RFC 1951
 * 3.1.1, so growing `code` by shifting in each new bit reconstructs it
 * correctly without ever materializing a lookup table. */
static int decode(struct bitstate *s, const struct huffman *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= MAXBITS; len++) {
        code |= bits(s, 1);
        if (s->error) return -1;
        int count = h->count[len];
        if (code - first < count) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

/* Builds count[]/symbol[] (codes grouped by length, each group sorted
 * by symbol) from a per-symbol code-length array. Returns 0 for a
 * complete code, >0 for incomplete (some bit patterns unused --
 * tolerated; decode() just fails on an unused pattern), <0 if
 * over-subscribed (a genuinely malformed table). */
static int construct(struct huffman *h, const short *length, int n) {
    for (int len = 0; len <= MAXBITS; len++) h->count[len] = 0;
    for (int sym = 0; sym < n; sym++) h->count[length[sym]]++;
    if (h->count[0] == n) return 0;

    int left = 1;
    for (int len = 1; len <= MAXBITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return left;
    }

    short offs[MAXBITS + 1];
    offs[1] = 0;
    for (int len = 1; len < MAXBITS; len++) offs[len + 1] = (short)(offs[len] + h->count[len]);

    for (int sym = 0; sym < n; sym++) {
        if (length[sym] != 0) h->symbol[offs[length[sym]]++] = (short)sym;
    }
    return left;
}

static int stored_block(struct bitstate *s) {
    s->bitbuf = 0;
    s->bitcnt = 0;
    if (s->incnt + 4 > s->inlen) { s->error = 1; return -2; }
    uint32_t len = (uint32_t)s->in[s->incnt] | ((uint32_t)s->in[s->incnt + 1] << 8);
    s->incnt += 4; /* LEN, then its one's-complement check (unverified) */
    if (s->incnt + len > s->inlen) { s->error = 1; return -2; }
    for (uint32_t i = 0; i < len; i++) {
        if (out_byte(s, s->in[s->incnt + i])) { s->incnt += len; return 1; }
    }
    s->incnt += len;
    return 0;
}

static const short lbase[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
static const short lext[29]  = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
static const short dbase[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
static const short dext[30]  = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

/* Decodes one block's worth of literal/match symbols. Return convention
 * (shared with stored_block() above): 0 = block finished normally, 1 =
 * output cap reached (stop everything, not an error), <0 = malformed
 * stream. */
static int codes(struct bitstate *s, const struct huffman *lencode, const struct huffman *distcode) {
    for (;;) {
        int symbol = decode(s, lencode);
        if (symbol < 0) return symbol;

        if (symbol < 256) {
            if (out_byte(s, (uint8_t)symbol)) return 1;
        } else if (symbol == 256) {
            return 0;
        } else {
            symbol -= 257;
            if (symbol >= 29) { s->error = 1; return -10; }
            int len = lbase[symbol] + bits(s, lext[symbol]);
            if (s->error) return -10;

            int dsym = decode(s, distcode);
            if (dsym < 0) return dsym;
            if (dsym >= 30) { s->error = 1; return -10; }
            uint32_t dist = (uint32_t)dbase[dsym] + (uint32_t)bits(s, dext[dsym]);
            if (s->error) return -10;
            if (dist > s->outcnt) { s->error = 1; return -11; }

            while (len-- > 0) {
                uint8_t b = s->out[s->outcnt - dist];
                if (out_byte(s, b)) return 1;
            }
        }
    }
}

static int fixed_block(struct bitstate *s) {
    struct huffman lencode, distcode;
    short lengths[FIXLCODES];
    int symbol = 0;
    while (symbol < 144) lengths[symbol++] = 8;
    while (symbol < 256) lengths[symbol++] = 9;
    while (symbol < 280) lengths[symbol++] = 7;
    while (symbol < FIXLCODES) lengths[symbol++] = 8;
    construct(&lencode, lengths, FIXLCODES);

    short dlengths[MAXDCODES];
    for (symbol = 0; symbol < MAXDCODES; symbol++) dlengths[symbol] = 5;
    construct(&distcode, dlengths, MAXDCODES);

    return codes(s, &lencode, &distcode);
}

static int dynamic_block(struct bitstate *s) {
    static const short order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
    short lengths[MAXCODES];
    short lenlen[19];

    int hlit = bits(s, 5) + 257;
    int hdist = bits(s, 5) + 1;
    int hclen = bits(s, 4) + 4;
    if (s->error) return -3;
    if (hlit > MAXLCODES || hdist > MAXDCODES) { s->error = 1; return -3; }

    for (int i = 0; i < 19; i++) lenlen[i] = 0;
    for (int i = 0; i < hclen; i++) lenlen[order[i]] = (short)bits(s, 3);
    if (s->error) return -3;

    struct huffman lench;
    if (construct(&lench, lenlen, 19) < 0) { s->error = 1; return -4; }

    int index = 0;
    while (index < hlit + hdist) {
        int symbol = decode(s, &lench);
        if (symbol < 0) return symbol;

        if (symbol < 16) {
            lengths[index++] = (short)symbol;
            continue;
        }

        int len, rep;
        if (symbol == 16) {
            if (index == 0) { s->error = 1; return -5; }
            len = lengths[index - 1];
            rep = 3 + bits(s, 2);
        } else if (symbol == 17) {
            len = 0;
            rep = 3 + bits(s, 3);
        } else {
            len = 0;
            rep = 11 + bits(s, 7);
        }
        if (s->error) return -3;
        if (index + rep > hlit + hdist) { s->error = 1; return -6; }
        while (rep-- > 0) lengths[index++] = (short)len;
    }

    struct huffman lencode, distcode;
    if (construct(&lencode, lengths, hlit) < 0) { s->error = 1; return -7; }
    if (construct(&distcode, lengths + hlit, hdist) < 0) { s->error = 1; return -8; }

    return codes(s, &lencode, &distcode);
}

uint32_t inflate_raw(const uint8_t *src, uint32_t src_len, uint8_t *out, uint32_t out_cap) {
    struct bitstate s;
    s.in = src; s.inlen = src_len; s.incnt = 0;
    s.bitbuf = 0; s.bitcnt = 0;
    s.out = out; s.outcap = out_cap; s.outcnt = 0;
    s.error = 0;

    int last;
    do {
        last = bits(&s, 1);
        if (s.error) return s.outcnt;
        int type = bits(&s, 2);
        if (s.error) return s.outcnt;

        int ret;
        if (type == 0) ret = stored_block(&s);
        else if (type == 1) ret = fixed_block(&s);
        else if (type == 2) ret = dynamic_block(&s);
        else return s.outcnt; /* type 3: reserved/invalid -- stop, keep what decoded so far */

        if (ret < 0) return s.outcnt;
        if (ret == 1) break;
    } while (!last);

    return s.outcnt;
}

int gzip_decompress(const uint8_t *src, uint32_t src_len, uint8_t *out, uint32_t out_cap, uint32_t *out_len) {
    if (src_len < 10 || src[0] != 0x1f || src[1] != 0x8b || src[2] != 8) return 0;
    uint8_t flags = src[3];
    uint32_t pos = 10;

    if (flags & 0x04) { /* FEXTRA */
        if (pos + 2 > src_len) return 0;
        uint32_t xlen = (uint32_t)src[pos] | ((uint32_t)src[pos + 1] << 8);
        pos += 2 + xlen;
    }
    if (flags & 0x08) { /* FNAME */
        while (pos < src_len && src[pos] != 0) pos++;
        pos++;
    }
    if (flags & 0x10) { /* FCOMMENT */
        while (pos < src_len && src[pos] != 0) pos++;
        pos++;
    }
    if (flags & 0x02) pos += 2; /* FHCRC */
    if (pos > src_len) return 0;

    /* Trailing 8 bytes are CRC32+ISIZE -- excluded rather than verified. */
    uint32_t payload_len = (src_len >= pos + 8) ? (src_len - pos - 8) : (src_len - pos);

    *out_len = inflate_raw(src + pos, payload_len, out, out_cap);
    return 1;
}

int deflate_decompress(const uint8_t *src, uint32_t src_len, uint8_t *out, uint32_t out_cap, uint32_t *out_len) {
    /* "Content-Encoding: deflate" is inconsistently either a raw DEFLATE
     * stream or one with a 2-byte zlib header -- detect the header via
     * its checksum property (RFC 1950: the 16-bit big-endian value must
     * be a multiple of 31) rather than assuming either way. */
    uint32_t start = 0;
    if (src_len >= 2 && (src[0] & 0x0f) == 8 &&
        (((uint32_t)src[0] << 8) | src[1]) % 31 == 0) {
        start = 2;
    }
    *out_len = inflate_raw(src + start, src_len - start, out, out_cap);
    return 1;
}
