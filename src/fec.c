/*
 * fec.c - Link codes: error detection (parity, Internet checksum, CRC-8,
 *         CRC-16, CRC-32) and error correction (triple repetition,
 *         Hamming(7,4), Hamming(8,4) SECDED, interleaved Hamming,
 *         Reed-Solomon), selectable per link with codec=NAME; plus the
 *         `coding` bench that shows one code at work bit by bit and
 *         measures it against the bit error rate.
 */
#include "nl_int.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Bit helpers (most significant bit first)                           */
/* ------------------------------------------------------------------ */

static int  bit_get(const uint8_t *b, long i) { return (b[i / 8] >> (7 - i % 8)) & 1; }
static void bit_set(uint8_t *b, long i, int v)
{
    if (v) b[i / 8] |= (uint8_t)(0x80 >> (i % 8));
    else   b[i / 8] &= (uint8_t)~(0x80 >> (i % 8));
}
static void bit_flip(uint8_t *b, long i) { b[i / 8] ^= (uint8_t)(0x80 >> (i % 8)); }

/* ------------------------------------------------------------------ */
/* Detection: trailers                                                */
/* ------------------------------------------------------------------ */

/* One even-parity bit for the whole frame, carried in a trailer byte */
static int parity_of(const uint8_t *d, int n)
{
    uint8_t x = 0;
    int i;

    for (i = 0; i < n; i++)
        x ^= d[i];
    x ^= x >> 4;
    x ^= x >> 2;
    x ^= x >> 1;
    return x & 1;
}

static int parity_enc(const uint8_t *in, int n, uint8_t *out, int cap)
{
    if (n + 1 > cap)
        return -1;
    memcpy(out, in, (size_t)n);
    out[n] = (uint8_t)parity_of(in, n);
    return n + 1;
}

static int parity_dec(const uint8_t *in, int n, uint8_t *out, int cap, int *fixed)
{
    *fixed = 0;
    if (n < 1 || n - 1 > cap || in[n - 1] > 1 || parity_of(in, n - 1) != in[n - 1])
        return -1;
    memcpy(out, in, (size_t)(n - 1));
    return n - 1;
}

static int cksum_enc(const uint8_t *in, int n, uint8_t *out, int cap)
{
    if (n + 2 > cap)
        return -1;
    memcpy(out, in, (size_t)n);
    nl_put16(out + n, nl_cksum(in, n, 0));
    return n + 2;
}

static int cksum_dec(const uint8_t *in, int n, uint8_t *out, int cap, int *fixed)
{
    *fixed = 0;
    if (n < 2 || n - 2 > cap || nl_cksum(in, n - 2, 0) != nl_get16(in + n - 2))
        return -1;
    memcpy(out, in, (size_t)(n - 2));
    return n - 2;
}

static uint32_t crc_bits(const uint8_t *d, int n, int width)
{
    uint32_t crc;
    int i, k;

    if (width == 32) {                  /* IEEE 802.3, reflected */
        crc = 0xFFFFFFFFu;
        for (i = 0; i < n; i++) {
            crc ^= d[i];
            for (k = 0; k < 8; k++)
                crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
        }
        return ~crc;
    }
    if (width == 16) {                  /* CCITT, x^16 + x^12 + x^5 + 1 */
        crc = 0xFFFF;
        for (i = 0; i < n; i++) {
            crc ^= (uint32_t)d[i] << 8;
            for (k = 0; k < 8; k++)
                crc = (crc & 0x8000) ? ((crc << 1) ^ 0x1021) & 0xFFFF : (crc << 1) & 0xFFFF;
        }
        return crc;
    }
    crc = 0;                            /* CRC-8, x^8 + x^2 + x + 1 */
    for (i = 0; i < n; i++) {
        crc ^= d[i];
        for (k = 0; k < 8; k++)
            crc = (crc & 0x80) ? ((crc << 1) ^ 0x07) & 0xFF : (crc << 1) & 0xFF;
    }
    return crc;
}

static int crc_enc(const uint8_t *in, int n, uint8_t *out, int cap, int width)
{
    uint32_t c = crc_bits(in, n, width);
    int t = width / 8, i;

    if (n + t > cap)
        return -1;
    memcpy(out, in, (size_t)n);
    for (i = 0; i < t; i++)
        out[n + i] = (uint8_t)(c >> (8 * (t - 1 - i)));
    return n + t;
}

static int crc_dec(const uint8_t *in, int n, uint8_t *out, int cap, int *fixed, int width)
{
    int t = width / 8, i;
    uint32_t c = 0;

    *fixed = 0;
    if (n < t || n - t > cap)
        return -1;
    for (i = 0; i < t; i++)
        c = c << 8 | in[n - t + i];
    if (crc_bits(in, n - t, width) != c)
        return -1;
    memcpy(out, in, (size_t)(n - t));
    return n - t;
}

static int crc8_enc(const uint8_t *i, int n, uint8_t *o, int c)             { return crc_enc(i, n, o, c, 8); }
static int crc16_enc(const uint8_t *i, int n, uint8_t *o, int c)            { return crc_enc(i, n, o, c, 16); }
static int crc32_enc(const uint8_t *i, int n, uint8_t *o, int c)            { return crc_enc(i, n, o, c, 32); }
static int crc8_dec(const uint8_t *i, int n, uint8_t *o, int c, int *f)     { return crc_dec(i, n, o, c, f, 8); }
static int crc16_dec(const uint8_t *i, int n, uint8_t *o, int c, int *f)    { return crc_dec(i, n, o, c, f, 16); }
static int crc32_dec(const uint8_t *i, int n, uint8_t *o, int c, int *f)    { return crc_dec(i, n, o, c, f, 32); }

/* ------------------------------------------------------------------ */
/* Correction: repetition                                             */
/* ------------------------------------------------------------------ */

/* Three whole copies of the frame, one after the other */
static int rep3_enc(const uint8_t *in, int n, uint8_t *out, int cap)
{
    if (3 * n > cap)
        return -1;
    memcpy(out, in, (size_t)n);
    memcpy(out + n, in, (size_t)n);
    memcpy(out + 2 * n, in, (size_t)n);
    return 3 * n;
}

static int popcount8(uint8_t x)
{
    int c = 0;

    for (; x; x &= (uint8_t)(x - 1))
        c++;
    return c;
}

static int rep3_dec(const uint8_t *in, int n, uint8_t *out, int cap, int *fixed)
{
    int k = n / 3, i;

    *fixed = 0;
    if (n % 3 || k > cap)
        return -1;
    for (i = 0; i < k; i++) {
        uint8_t a = in[i], b = in[k + i], c = in[2 * k + i];
        out[i] = (uint8_t)((a & b) | (a & c) | (b & c));        /* majority, bit by bit */
        *fixed += popcount8((uint8_t)((a ^ out[i]) | (b ^ out[i]) | (c ^ out[i])));
    }
    return k;
}

/* ------------------------------------------------------------------ */
/* Correction: Hamming                                                */
/* ------------------------------------------------------------------ */

/* Codeword positions 1..7 = p1 p2 d1 p3 d2 d3 d4; position i is bit 7-i */
static uint8_t h74_enc(int nib)
{
    int d1 = (nib >> 3) & 1, d2 = (nib >> 2) & 1, d3 = (nib >> 1) & 1, d4 = nib & 1;
    int p1 = d1 ^ d2 ^ d4, p2 = d1 ^ d3 ^ d4, p3 = d2 ^ d3 ^ d4;

    return (uint8_t)(p1 << 6 | p2 << 5 | d1 << 4 | p3 << 3 | d2 << 2 | d3 << 1 | d4);
}

static int h74_syndrome(uint8_t cw)
{
    int b[8], i;

    for (i = 1; i <= 7; i++)
        b[i] = (cw >> (7 - i)) & 1;
    return (b[1] ^ b[3] ^ b[5] ^ b[7]) | (b[2] ^ b[3] ^ b[6] ^ b[7]) << 1 | (b[4] ^ b[5] ^ b[6] ^ b[7]) << 2;
}

static int h74_data(uint8_t cw)
{
    return ((cw >> 4) & 1) << 3 | (cw & 7);
}

static int h74_dec(uint8_t cw, int *fixed)
{
    int s = h74_syndrome(cw);

    if (s) {
        cw ^= (uint8_t)(1 << (7 - s));
        (*fixed)++;
    }
    return h74_data(cw);
}

/* Where the j-th bit (0..6) of codeword c travels: plain = one codeword
 * after the other; interleaved = all first bits, then all second bits... */
static long h74_pos(long c, int j, long ncw, int inter)
{
    return inter ? (long)j * ncw + c : c * 7 + j;
}

static int h74x_enc(const uint8_t *in, int n, uint8_t *out, int cap, int inter)
{
    long ncw = 2L * n, c;
    int bytes = (int)((ncw * 7 + 7) / 8), j;

    if (bytes > cap)
        return -1;
    memset(out, 0, (size_t)bytes);
    for (c = 0; c < ncw; c++) {
        uint8_t cw = h74_enc(c & 1 ? in[c / 2] & 15 : in[c / 2] >> 4);
        for (j = 0; j < 7; j++)
            bit_set(out, h74_pos(c, j, ncw, inter), (cw >> (6 - j)) & 1);
    }
    return bytes;
}

static int h74x_dec(const uint8_t *in, int n, uint8_t *out, int cap, int *fixed, int inter)
{
    long ncw = ((long)n * 8 / 7) & ~1L, c;
    int j;

    *fixed = 0;
    if (ncw / 2 > cap)
        return -1;
    for (c = 0; c < ncw; c++) {
        uint8_t cw = 0;
        int nib;

        for (j = 0; j < 7; j++)
            cw = (uint8_t)(cw << 1 | bit_get(in, h74_pos(c, j, ncw, inter)));
        nib = h74_dec(cw, fixed);
        if (c & 1)
            out[c / 2] |= (uint8_t)nib;
        else
            out[c / 2] = (uint8_t)(nib << 4);
    }
    return (int)(ncw / 2);
}

static int h74_encf(const uint8_t *i, int n, uint8_t *o, int c)            { return h74x_enc(i, n, o, c, 0); }
static int h74_decf(const uint8_t *i, int n, uint8_t *o, int c, int *f)    { return h74x_dec(i, n, o, c, f, 0); }
static int h74i_encf(const uint8_t *i, int n, uint8_t *o, int c)           { return h74x_enc(i, n, o, c, 1); }
static int h74i_decf(const uint8_t *i, int n, uint8_t *o, int c, int *f)   { return h74x_dec(i, n, o, c, f, 1); }

/* Hamming(8,4): the (7,4) codeword plus an overall parity bit. One byte
 * per nibble. Returns the nibble, or -1 for a double error. */
static uint8_t h84_enc(int nib)
{
    uint8_t cw = h74_enc(nib);
    return (uint8_t)(cw << 1 | (popcount8(cw) & 1));
}

static int h84_dec(uint8_t b, int *fixed)
{
    int s = h74_syndrome(b >> 1), odd = popcount8(b) & 1;

    if (s && !odd)
        return -1;                      /* two errors: detected, not correctable */
    if (s || odd)
        (*fixed)++;
    if (s)
        b ^= (uint8_t)(1 << (8 - s));
    return h74_data(b >> 1);
}

static int h84_encf(const uint8_t *in, int n, uint8_t *out, int cap)
{
    int i;

    if (2 * n > cap)
        return -1;
    for (i = 0; i < n; i++) {
        out[2 * i] = h84_enc(in[i] >> 4);
        out[2 * i + 1] = h84_enc(in[i] & 15);
    }
    return 2 * n;
}

static int h84_decf(const uint8_t *in, int n, uint8_t *out, int cap, int *fixed)
{
    int i;

    *fixed = 0;
    if (n % 2 || n / 2 > cap)
        return -1;
    for (i = 0; i < n / 2; i++) {
        int hi = h84_dec(in[2 * i], fixed), lo = h84_dec(in[2 * i + 1], fixed);
        if (hi < 0 || lo < 0)
            return -1;
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return n / 2;
}

/* ------------------------------------------------------------------ */
/* Correction: Reed-Solomon over GF(2^8)                              */
/* ------------------------------------------------------------------ */

#define RS_NSYM 32                      /* parity bytes per block: corrects 16 byte errors */
#define RS_K    223                     /* data bytes per full block */

static uint8_t gf_exp[512], gf_log[256], rs_gen[RS_NSYM + 1];

static uint8_t gf_mul(uint8_t a, uint8_t b) { return a && b ? gf_exp[gf_log[a] + gf_log[b]] : 0; }
static uint8_t gf_div(uint8_t a, uint8_t b) { return a ? gf_exp[gf_log[a] + 255 - gf_log[b]] : 0; }

static void rs_init(void)
{
    int i, j, x = 1;

    for (i = 0; i < 255; i++) {
        gf_exp[i] = (uint8_t)x;
        gf_log[x] = (uint8_t)i;
        x <<= 1;
        if (x & 0x100)
            x ^= 0x11d;
    }
    for (i = 255; i < 512; i++)
        gf_exp[i] = gf_exp[i - 255];
    /* generator g(x) = (x + a^0)(x + a^1)...(x + a^31), highest degree first */
    memset(rs_gen, 0, sizeof(rs_gen));
    rs_gen[0] = 1;
    for (i = 0; i < RS_NSYM; i++)
        for (j = i + 1; j > 0; j--)
            rs_gen[j] ^= gf_mul(rs_gen[j - 1], gf_exp[i]);
}

static void rs_enc_block(const uint8_t *d, int k, uint8_t *par)
{
    int i, j;

    memset(par, 0, RS_NSYM);
    for (i = 0; i < k; i++) {
        uint8_t fb = d[i] ^ par[0];
        memmove(par, par + 1, RS_NSYM - 1);
        par[RS_NSYM - 1] = 0;
        if (fb)
            for (j = 0; j < RS_NSYM; j++)
                par[j] ^= gf_mul(rs_gen[j + 1], fb);
    }
}

static int rs_syndromes(const uint8_t *cw, int n, uint8_t *s)
{
    int i, j, any = 0;

    for (j = 0; j < RS_NSYM; j++) {
        uint8_t v = 0;
        for (i = 0; i < n; i++)
            v = gf_mul(v, gf_exp[j]) ^ cw[i];
        s[j] = v;
        any |= v;
    }
    return any;
}

/* Correct cw (n bytes, highest-degree coefficient first) in place.
 * Returns the number of corrected bytes, or -1 when there are too many. */
static int rs_dec_block(uint8_t *cw, int n)
{
    uint8_t s[RS_NSYM], C[RS_NSYM + 2], B[RS_NSYM + 2], T[RS_NSYM + 2], om[RS_NSYM];
    int pos[RS_NSYM], L = 0, m = 1, i, j, r, nerr = 0;
    uint8_t b = 1;

    if (!rs_syndromes(cw, n, s))
        return 0;
    /* Berlekamp-Massey: error locator C(x), lowest degree first */
    memset(C, 0, sizeof(C));
    memset(B, 0, sizeof(B));
    C[0] = B[0] = 1;
    for (r = 0; r < RS_NSYM; r++) {
        uint8_t d = s[r], coef;

        for (i = 1; i <= L; i++)
            d ^= gf_mul(C[i], s[r - i]);
        if (!d) {
            m++;
            continue;
        }
        coef = gf_div(d, b);
        memcpy(T, C, sizeof(T));
        for (i = 0; i + m < RS_NSYM + 2; i++)
            C[i + m] ^= gf_mul(coef, B[i]);
        if (2 * L <= r) {
            L = r + 1 - L;
            memcpy(B, T, sizeof(B));
            b = d;
            m = 1;
        } else {
            m++;
        }
    }
    if (L > RS_NSYM / 2)
        return -1;
    /* Chien search: C(a^-p) = 0 means an error at the coefficient of x^p */
    for (i = 0; i < n; i++) {
        uint8_t xi = gf_exp[(255 - i) % 255], v = 0;
        for (j = L; j >= 0; j--)
            v = gf_mul(v, xi) ^ C[j];
        if (!v) {
            if (nerr >= L)
                return -1;
            pos[nerr++] = i;
        }
    }
    if (nerr != L)
        return -1;
    /* Forney: error values from the evaluator S(x)C(x) mod x^NSYM */
    for (j = 0; j < RS_NSYM; j++) {
        om[j] = 0;
        for (i = 0; i <= L && i <= j; i++)
            om[j] ^= gf_mul(C[i], s[j - i]);
    }
    for (r = 0; r < nerr; r++) {
        uint8_t xi = gf_exp[(255 - pos[r]) % 255], num = 0, den = 0, xp = 1, x2 = gf_mul(xi, xi);

        for (j = RS_NSYM - 1; j >= 0; j--)
            num = gf_mul(num, xi) ^ om[j];
        for (i = 1; i <= L; i += 2) {
            den ^= gf_mul(C[i], xp);
            xp = gf_mul(xp, x2);
        }
        if (!den)
            return -1;
        cw[n - 1 - pos[r]] ^= gf_mul(gf_exp[pos[r]], gf_div(num, den));
    }
    return rs_syndromes(cw, n, s) ? -1 : nerr;
}

static int rs_encf(const uint8_t *in, int n, uint8_t *out, int cap)
{
    int off = 0, o = 0;

    if (n <= 0)
        return -1;
    while (off < n) {
        int k = n - off < RS_K ? n - off : RS_K;
        if (o + k + RS_NSYM > cap)
            return -1;
        memcpy(out + o, in + off, (size_t)k);
        rs_enc_block(in + off, k, out + o + k);
        o += k + RS_NSYM;
        off += k;
    }
    return o;
}

static int rs_decf(const uint8_t *in, int n, uint8_t *out, int cap, int *fixed)
{
    uint8_t cw[255];
    int off = 0, o = 0;

    *fixed = 0;
    while (off < n) {
        int bl = n - off < 255 ? n - off : 255, e;
        if (bl <= RS_NSYM || o + bl - RS_NSYM > cap)
            return -1;
        memcpy(cw, in + off, (size_t)bl);
        e = rs_dec_block(cw, bl);
        if (e < 0)
            return -1;
        *fixed += e;
        memcpy(out + o, cw, (size_t)(bl - RS_NSYM));
        o += bl - RS_NSYM;
        off += bl;
    }
    return o;
}

/* ------------------------------------------------------------------ */
/* Registration                                                       */
/* ------------------------------------------------------------------ */

static const nl_codec CODECS[] = {
    { "parity",     N_("one even-parity bit per frame: detects an odd number of flipped bits"),
      parity_enc, parity_dec },
    { "checksum",   N_("16-bit Internet checksum trailer: detection, weak against some patterns"),
      cksum_enc, cksum_dec },
    { "crc8",       N_("CRC-8 trailer: detects every burst up to 8 bits"), crc8_enc, crc8_dec },
    { "crc16",      N_("CRC-16 (CCITT) trailer: detects every burst up to 16 bits"), crc16_enc, crc16_dec },
    { "crc32",      N_("CRC-32 (IEEE 802.3, as in Ethernet) trailer: detection"), crc32_enc, crc32_dec },
    { "rep3",       N_("three copies and a majority vote: corrects, at three times the airtime"),
      rep3_enc, rep3_dec },
    { "hamming74",  N_("Hamming(7,4): corrects one flipped bit in every 7"), h74_encf, h74_decf },
    { "hamming84",  N_("Hamming(8,4) SECDED: corrects one bit per byte, detects two"), h84_encf, h84_decf },
    { "hamming74i", N_("Hamming(7,4) with a block interleaver: also survives bursts"), h74i_encf, h74i_decf },
    { "rs",         N_("Reed-Solomon (255,223) over GF(256): corrects 16 bytes per block, bursts included"),
      rs_encf, rs_decf },
};
#define NCODECS ((int)(sizeof(CODECS) / sizeof(CODECS[0])))

/* Is bit i of an encoded frame redundancy (1) or data (0)? */
static int redundant_bit(const char *code, long i, int enc_len, int data_len)
{
    if (!strcmp(code, "rep3"))
        return i >= (long)data_len * 8;
    if (!strcmp(code, "hamming74")) {
        int j = (int)(i % 7);
        return i >= (long)data_len * 14 || j == 0 || j == 1 || j == 3;
    }
    if (!strcmp(code, "hamming74i")) {
        long ncw = 2L * data_len;
        int j = (int)(i / (ncw ? ncw : 1));
        return j >= 7 || j == 0 || j == 1 || j == 3;
    }
    if (!strcmp(code, "hamming84")) {
        int j = (int)(i % 8);
        return j == 0 || j == 1 || j == 3 || j == 7;
    }
    if (!strcmp(code, "rs")) {
        long byte = i / 8, block = byte / 255, in_block = byte % 255;
        long bl = enc_len - block * 255 < 255 ? enc_len - block * 255 : 255;
        return in_block >= bl - RS_NSYM;
    }
    return i >= (long)data_len * 8;     /* trailers */
}

/* ------------------------------------------------------------------ */
/* The coding bench                                                   */
/* ------------------------------------------------------------------ */

#define SHOW_MAX   48                   /* bytes of text */
#define SHOW_ENC   320
#define SWEEP_ROWS 8
#define NOTE_MAX   10

enum { OUT_INTACT, OUT_FIXED, OUT_DETECTED, OUT_UNDETECTED };

typedef struct {
    /* last `show` */
    int     have_show;
    char    code[16];
    uint8_t text[SHOW_MAX], sent[SHOW_ENC], rcvd[SHOW_ENC], back[SHOW_ENC];
    int     text_len, enc_len, back_len, nflips, fixed, outcome;
    char    notes[NOTE_MAX][130];
    int     nnotes;
    /* last `sweep` */
    int     have_sweep, sweep_burst, sweep_size, sweep_frames, nrows;
    char    sweep_code[16];
    struct { double ber; int n[4]; } row[SWEEP_ROWS];
    double  overhead;                   /* encoded bits per data bit */
} coding_state;

static const nl_module coding_mod;

static const char *outcome_name(int o)
{
    switch (o) {
    case OUT_INTACT:   return _("arrived intact");
    case OUT_FIXED:    return _("errors corrected: the data is right");
    case OUT_DETECTED: return _("errors detected: frame discarded");
    default:           return _("errors NOT detected: wrong data delivered");
    }
}

static coding_state *coding_get(nl_node *n)
{
    coding_state *c = nl_mod_get(n, &coding_mod);

    if (!c) {
        c = calloc(1, sizeof(*c));
        if (c)
            nl_mod_set(n, &coding_mod, c);
    }
    return c;
}

static void show_notes(coding_state *c)
{
    int i, b;

    c->nnotes = 0;
    if (!strcmp(c->code, "hamming74") || !strcmp(c->code, "hamming74i")) {
        int inter = c->code[9] == 'i';
        long ncw = 2L * c->text_len, k;

        for (k = 0; k < ncw && c->nnotes < NOTE_MAX; k++) {
            uint8_t s = 0, r = 0;
            int j, syn;

            for (j = 0; j < 7; j++) {
                s = (uint8_t)(s << 1 | bit_get(c->sent, h74_pos(k, j, ncw, inter)));
                r = (uint8_t)(r << 1 | bit_get(c->rcvd, h74_pos(k, j, ncw, inter)));
            }
            if (s == r)
                continue;
            syn = h74_syndrome(r);
            b = popcount8(s ^ r);
            snprintf(c->notes[c->nnotes++], sizeof(c->notes[0]), "%s %ld: %d %s, %s %d → %s", _("block"),
                     k + 1, b, _("bit(s) flipped"), _("syndrome"), syn,
                     b == 1 ? _("that bit is flipped back") : _("the wrong bit is \"corrected\""));
        }
    } else if (!strcmp(c->code, "hamming84")) {
        for (i = 0; i < c->enc_len && c->nnotes < NOTE_MAX; i++) {
            int syn, odd;

            if (c->sent[i] == c->rcvd[i])
                continue;
            syn = h74_syndrome(c->rcvd[i] >> 1);
            odd = popcount8(c->rcvd[i]) & 1;
            b = popcount8(c->sent[i] ^ c->rcvd[i]);
            snprintf(c->notes[c->nnotes++], sizeof(c->notes[0]), "%s %d: %d %s, %s %d, %s %s → %s", _("block"),
                     i + 1, b, _("bit(s) flipped"), _("syndrome"), syn, _("overall parity"),
                     odd ? _("wrong") : _("right"),
                     syn && !odd ? _("double error: give up") : _("single error: corrected"));
        }
    } else if (!strcmp(c->code, "rep3")) {
        snprintf(c->notes[c->nnotes++], sizeof(c->notes[0]), "%d %s", c->fixed,
                 _("bit position(s) where the three copies disagree: the majority wins"));
    } else if (!strcmp(c->code, "rs")) {
        if (c->outcome != OUT_DETECTED)
            snprintf(c->notes[c->nnotes++], sizeof(c->notes[0]), "%d %s", c->fixed,
                     _("byte(s) located and repaired from the 32 parity bytes (up to 16 per block)"));
    } else if (c->nflips) {
        snprintf(c->notes[c->nnotes++], sizeof(c->notes[0]), "%s",
                 c->outcome == OUT_DETECTED ? _("the check recomputed by the receiver does not match the trailer")
                 : _("the damaged frame still passes the check: this code cannot see this pattern"));
    }
}

static int coding_show(nl_node *n, coding_state *c, const nl_args *a)
{
    const char *code = nl_arg_str(a, "code", "hamming74"), *text = nl_arg_str(a, "text", "HELLO");
    const char *posl = nl_arg_str(a, "pos", NULL);
    const nl_codec *cd = nl_codec_find(code);
    int flips = (int)nl_arg_int(a, "flips", 1), burst = (int)nl_arg_int(a, "burst", 0), i, fixed = 0;
    long nbits;
    char shown[SHOW_MAX + 1];

    if (!cd) {
        nl_log(n, NL_L_LINK, 1, 0, T_ERR "coding: %s '%s'", _("unknown code"), code);
        return -1;
    }
    snprintf(c->code, sizeof(c->code), "%s", code);
    c->text_len = (int)strlen(text) < SHOW_MAX ? (int)strlen(text) : SHOW_MAX;
    if (c->text_len < 1)
        return -1;
    memcpy(c->text, text, (size_t)c->text_len);
    c->enc_len = cd->encode(c->text, c->text_len, c->sent, SHOW_ENC);
    if (c->enc_len <= 0)
        return -1;
    memcpy(c->rcvd, c->sent, (size_t)c->enc_len);
    nbits = (long)c->enc_len * 8;
    c->nflips = 0;
    if (posl) {                         /* explicit bit positions: pos=3,17 */
        const char *s = posl;
        while (*s) {
            long p = strtol(s, (char **)&s, 10);
            if (p >= 0 && p < nbits) {
                bit_flip(c->rcvd, p);
                c->nflips++;
            }
            while (*s == ',' || *s == ' ')
                s++;
        }
    } else if (burst) {
        long start = (long)nl_rand((unsigned)(nbits - flips > 0 ? nbits - flips + 1 : 1));
        for (i = 0; i < flips && start + i < nbits; i++, c->nflips++)
            bit_flip(c->rcvd, start + i);
    } else {
        for (i = 0; i < flips && i < nbits; i++) {
            long p;
            do
                p = (long)nl_rand((unsigned)nbits);
            while (bit_get(c->rcvd, p) != bit_get(c->sent, p));
            bit_flip(c->rcvd, p);
            c->nflips++;
        }
    }
    c->back_len = cd->decode(c->rcvd, c->enc_len, c->back, SHOW_ENC, &fixed);
    c->fixed = fixed;
    if (c->back_len < 0)
        c->outcome = OUT_DETECTED;
    else if (c->back_len == c->text_len && memcmp(c->back, c->text, (size_t)c->text_len) == 0)
        c->outcome = c->nflips ? OUT_FIXED : OUT_INTACT;
    else
        c->outcome = OUT_UNDETECTED;
    c->have_show = 1;
    show_notes(c);

    tui_payload(c->text, (size_t)c->text_len, shown, sizeof(shown), 32);
    nl_log(n, NL_L_LINK, 1, 0, T_INFO "coding" T_RESET " %s «%s»: %d → %d bit, %d %s%s", code, shown,
           c->text_len * 8, c->enc_len * 8, c->nflips, _("bit(s) flipped on the channel"),
           burst && !posl ? _(" in one burst") : "");
    nl_log(n, NL_L_LINK, 1, 0, "%s  → %s", c->outcome == OUT_UNDETECTED ? T_ERR "✗" : c->outcome == OUT_DETECTED
           ? T_WARN "↯" : T_OK "✓", outcome_name(c->outcome));
    for (i = 0; i < c->nnotes; i++)
        nl_log(n, NL_L_LINK, 2, 0, T_DIM "     %s", c->notes[i]);
    if (c->outcome == OUT_UNDETECTED) {
        tui_payload(c->back, (size_t)c->back_len, shown, sizeof(shown), 32);
        nl_log(n, NL_L_LINK, 1, 0, T_ERR "     %s «%s»", _("the receiver believes the text was"), shown);
    }
    nl_count_set(n, "coding.show_ok", c->outcome == OUT_INTACT || c->outcome == OUT_FIXED);
    nl_count_set(n, "coding.show_detected", c->outcome == OUT_DETECTED);
    nl_count_set(n, "coding.show_undetected", c->outcome == OUT_UNDETECTED);
    nl_count_set(n, "coding.show_fixed", fixed);
    return 0;
}

static int coding_sweep(nl_node *n, coding_state *c, const nl_args *a)
{
    static const double BERS[] = { 1e-5, 1e-4, 3e-4, 1e-3, 3e-3, 1e-2, 3e-2 };
    static uint8_t data[1024], enc[4200], rx[4200], dec[2200];
    const char *code = nl_arg_str(a, "code", "hamming74");
    const nl_codec *cd = nl_codec_find(code);
    int frames = (int)nl_arg_int(a, "frames", 2000), size = (int)nl_arg_int(a, "size", 100);
    int burst = (int)nl_arg_int(a, "burst", 1), r, f, i, tot[4] = { 0, 0, 0, 0 }, all = 0;
    double one = nl_arg_dbl(a, "ber", 0);
    char name[28];

    if (!cd) {
        nl_log(n, NL_L_LINK, 1, 0, T_ERR "coding: %s '%s'", _("unknown code"), code);
        return -1;
    }
    if (size < 1) size = 1;
    if (size > 1000) size = 1000;
    if (frames < 10) frames = 10;
    if (frames > 100000) frames = 100000;
    if (burst < 1) burst = 1;
    snprintf(c->sweep_code, sizeof(c->sweep_code), "%s", code);
    c->sweep_burst = burst;
    c->sweep_size = size;
    c->sweep_frames = frames;
    c->nrows = one > 0 ? 1 : (int)(sizeof(BERS) / sizeof(BERS[0]));
    c->have_sweep = 1;
    nl_log(n, NL_L_LINK, 1, 0, T_INFO "coding sweep" T_RESET " %s: %d %s, %d B%s", code, frames,
           _("frames per point"), size, burst > 1 ? _(", errors in bursts") : "");
    for (r = 0; r < c->nrows; r++) {
        double ber = one > 0 ? one : BERS[r], gap = burst / ber;

        c->row[r].ber = ber;
        memset(c->row[r].n, 0, sizeof(c->row[r].n));
        for (f = 0; f < frames; f++) {
            int en, dn, fixed = 0, flips = 0, o;
            long nbits, pos;

            for (i = 0; i < size; i++)
                data[i] = (uint8_t)nl_rand(256);
            en = cd->encode(data, size, enc, (int)sizeof(enc));
            if (en <= 0)
                return -1;
            memcpy(rx, enc, (size_t)en);
            nbits = (long)en * 8;
            c->overhead = (double)en / size;
            /* error events arrive with geometric gaps; each flips `burst` bits */
            for (pos = (long)nl_rand_exp(gap); pos < nbits; pos += burst + (long)nl_rand_exp(gap))
                for (i = 0; i < burst && pos + i < nbits; i++, flips++)
                    bit_flip(rx, pos + i);
            dn = cd->decode(rx, en, dec, (int)sizeof(dec), &fixed);
            o = dn < 0 ? OUT_DETECTED : dn == size && memcmp(dec, data, (size_t)size) == 0
                ? (flips ? OUT_FIXED : OUT_INTACT) : OUT_UNDETECTED;
            c->row[r].n[o]++;
        }
        for (i = 0; i < 4; i++)
            tot[i] += c->row[r].n[i];
        all += frames;
        nl_log(n, NL_L_LINK, 1, 0, "  BER %-7.0e %s %5.1f%% " T_DIM "(%s %5.1f%%)" T_RESET "  %s %5.1f%%  %s%s %5.1f%%",
               ber, _("delivered"), 100.0 * (c->row[r].n[OUT_INTACT] + c->row[r].n[OUT_FIXED]) / frames,
               _("repaired"), 100.0 * c->row[r].n[OUT_FIXED] / frames, _("discarded"),
               100.0 * c->row[r].n[OUT_DETECTED] / frames, c->row[r].n[OUT_UNDETECTED] ? T_ERR : "",
               _("silently wrong"), 100.0 * c->row[r].n[OUT_UNDETECTED] / frames);
    }
    nl_log(n, NL_L_LINK, 1, 0, T_DIM "  %s: %.2f %s", _("cost"), c->overhead, _("bits sent per data bit"));
    snprintf(name, sizeof(name), "coding.%s.good", code);
    nl_count_set(n, name, 100.0 * (tot[OUT_INTACT] + tot[OUT_FIXED]) / all);
    snprintf(name, sizeof(name), "coding.%s.dropped", code);
    nl_count_set(n, name, 100.0 * tot[OUT_DETECTED] / all);
    snprintf(name, sizeof(name), "coding.%s.bad", code);
    nl_count_set(n, name, 100.0 * tot[OUT_UNDETECTED] / all);
    nl_count_set(n, "coding.undetected", tot[OUT_UNDETECTED]);
    nl_count(n, "coding.sweeps", 1);
    return 0;
}

static int coding_run(nl_node *n, const nl_args *a)
{
    coding_state *c = coding_get(n);
    const char *what = nl_arg_pos(a, 0);

    if (!c)
        return -1;
    if (what && strcmp(what, "sweep") == 0)
        return coding_sweep(n, c, a);
    if (what && strcmp(what, "list") == 0) {
        int i;
        for (i = 0; i < NCODECS; i++)
            nl_log(n, NL_L_LINK, 1, 0, "  " T_BOLD "%-11s" T_RESET " %s", CODECS[i].name, _(CODECS[i].title));
        return 0;
    }
    return coding_show(n, c, a);
}

static void coding_stop(nl_node *n)
{
    coding_state *c = nl_mod_get(n, &coding_mod);

    nl_mod_set(n, &coding_mod, NULL);
    free(c);
}

/* Bits of one buffer, bytes separated by a space; returns rows used */
static int draw_bits(int x, int y, int w, int maxrows, const coding_state *c, int received)
{
    int per_row = (w - 6) / 9, rows = 0, i, k;

    if (per_row < 1)
        per_row = 1;
    for (i = 0; i < c->enc_len && rows < maxrows; i += per_row, rows++) {
        tui_print(x, y + rows, 6, TS(C_DIM, C_DEFAULT, 0), "%s", i ? "" : received ? _("recv") : _("sent"));
        for (k = 0; k < per_row * 8 && (long)i * 8 + k < (long)c->enc_len * 8; k++) {
            long bit = (long)i * 8 + k;
            int v = bit_get(received ? c->rcvd : c->sent, bit);
            int red = redundant_bit(c->code, bit, c->enc_len, c->text_len);
            int flipped = received && v != bit_get(c->sent, bit);
            tstyle st = flipped ? TS(231, C_ERR_BG, TA_BOLD) : red ? TS(C_WARN, C_DEFAULT, 0)
                      : TS(C_DATA, C_DEFAULT, received ? 0 : TA_BOLD);

            tui_put(x + 6 + k + k / 8, y + rows, st, v ? "1" : "0");
        }
    }
    return rows;
}

static int coding_panel(nl_node *n, int x, int y, int w, int h, int detail)
{
    coding_state *c = nl_mod_get(n, &coding_mod);
    tstyle txt = TS(C_TEXT, C_DEFAULT, 0), dim = TS(C_DIM, C_DEFAULT, 0);
    int row = 0, i;
    char shown[SHOW_MAX + 1];

    if (c->have_show && row + 5 <= h) {
        int room = (h - 5 - (c->have_sweep ? c->nrows + 2 : 0)) / 2;

        tui_payload(c->text, (size_t)c->text_len, shown, sizeof(shown), 24);
        tui_print(x, y + row++, w, txt, T_BOLD "%s" T_RESET "  «%s»  %d → %d bit  " T_DATA "■" T_RESET " %s  "
                  T_WARN "■" T_RESET " %s  " T_CHIP_ERR "1" T_RESET " %s", c->code, shown, c->text_len * 8,
                  c->enc_len * 8, _("data"), _("redundancy"), _("flipped"));
        if (room < 1)
            room = 1;
        row += draw_bits(x, y + row, w, room, c, 0);
        row += draw_bits(x, y + row, w, room, c, 1);
        tui_print(x, y + row++, w, txt, "%s %s", c->outcome == OUT_UNDETECTED ? T_ERR "✗" : c->outcome == OUT_DETECTED
                  ? T_WARN "↯" : T_OK "✓", outcome_name(c->outcome));
        if (c->back_len >= 0 && row < h) {
            tui_payload(c->back, (size_t)c->back_len, shown, sizeof(shown), 24);
            tui_print(x, y + row++, w, dim, "%s «%s»", _("decoded text"), shown);
        }
        for (i = 0; detail >= 2 && i < c->nnotes && row < h - (c->have_sweep ? c->nrows + 2 : 0); i++)
            tui_print(x, y + row++, w, dim, "%s", c->notes[i]);
    }
    if (c->have_sweep && row + 2 <= h) {
        tui_print(x, y + row++, w, txt, T_BOLD "%s" T_RESET " %s  " T_DIM "%d B, ×%.2f %s%s", _("sweep"),
                  c->sweep_code, c->sweep_size, c->overhead, _("airtime"),
                  c->sweep_burst > 1 ? _(", bursts") : "");
        tui_print(x, y + row++, w, dim, "%-8s %9s %9s %9s %9s", "BER", _("intact"), _("repaired"), _("dropped"),
                  _("wrong"));
        for (i = 0; i < c->nrows && row < h; i++) {
            double f = c->sweep_frames / 100.0;
            tui_print(x, y + row++, w, txt, "%-8.0e %8.1f%% " T_OK "%8.1f%%" T_RESET " " T_WARN "%8.1f%%" T_RESET
                      " %s%8.1f%%", c->row[i].ber, c->row[i].n[0] / f, c->row[i].n[1] / f, c->row[i].n[2] / f,
                      c->row[i].n[3] ? T_ERR : T_DIM, c->row[i].n[3] / f);
        }
    }
    if (!row)
        tui_print(x, y + row++, w, dim, "%s", _("run NODE coding show text=HELLO code=hamming74 flips=1"));
    return row;
}

static const nl_module coding_mod = {
    .name = "coding", .title = N_("coding bench — error detecting and correcting codes, bit by bit"),
    .layer = NL_L_LINK,
    .usage = N_("run NODE coding show [text=HELLO] [code=hamming74] [flips=1] [burst=1] [pos=3,17]\n"
                "  encode the text, flip bits (random, one burst, or the listed positions), decode.\n"
                "run NODE coding sweep [code=hamming74] [frames=2000] [size=100] [burst=N] [ber=X]\n"
                "  frames delivered / discarded / silently wrong over a range of bit error rates.\n"
                "run NODE coding list\n"
                "  Codes: parity checksum crc8 crc16 crc32 rep3 hamming74 hamming84 hamming74i rs.\n"
                "  The same names work on links: link A B ber=1e-4 codec=hamming74"),
    .theory = N_("A channel flips bits. The sender can only fight back by sending more bits than the data: "
                 "redundancy computed from the data, which the receiver recomputes and compares. Detection "
                 "codes add little: a parity bit catches any odd number of flips and misses every even "
                 "one; the Internet checksum adds 16 bits but is blind to errors that cancel in the sum; "
                 "a CRC treats the frame as a polynomial and sends the remainder of a division, which "
                 "catches every burst shorter than the CRC and all but one in 2^n of the others. A frame "
                 "found damaged is discarded, and someone above must send it again.\n\n"
                 "Correction codes add enough redundancy to find WHERE the error is. Repetition sends "
                 "three copies and votes. Hamming(7,4) protects 4 data bits with 3 parity bits placed so "
                 "that the pattern of failed checks (the syndrome) spells the position of the flipped "
                 "bit; with two flips it confidently repairs the wrong bit, which is why the (8,4) "
                 "variant adds an overall parity bit to tell one error from two (SECDED). Reed-Solomon "
                 "works on bytes instead of bits: 32 parity bytes per block of 255 let it repair any 16 "
                 "damaged bytes, however many bits are wrong inside them, so it shrugs off bursts. An "
                 "interleaver spreads a burst over many codewords, turning it into isolated errors a "
                 "weak code can fix.\n\n"
                 "The trade-off is rate against residual errors: every redundant bit costs airtime, and "
                 "which code wins depends on the error rate and on whether errors come alone or in "
                 "bursts. Simplifications here: frames are coded whole, the interleaver spans one frame, "
                 "and Reed-Solomon uses fixed (255,223) blocks, the last one shortened."),
    .run = coding_run, .stop = coding_stop, .panel = coding_panel,
};
NL_MODULE(coding_mod)

NL_INIT(fec)
{
    int i;

    rs_init();
    for (i = 0; i < NCODECS; i++)
        nl_codec_register(&CODECS[i]);
}
