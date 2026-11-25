/*
 * tcp.c - TCP: connections (three-way handshake, sliding window, out-of-order
 *         reassembly, flow control with window scaling and zero-window
 *         probes, delayed ACKs, Nagle, RFC 6298 retransmission timer with
 *         Karn's rule, FIN and RST), selective acknowledgements (RFC 2018,
 *         D-SACK RFC 2883) with the loss recovery of RFC 6675, NewReno
 *         recovery (RFC 6582) when the peer has no SACK, pluggable congestion
 *         control (tahoe, reno, newreno, cubic, bbr v1), ECN, the dissector,
 *         and the `tcp` tool (bulk sender and sink).
 *
 * The sender keeps a scoreboard with one record per segment in flight
 * (SACKed, lost, retransmitted, when it was sent and what had been
 * delivered by then). The same records drive loss recovery, RTT sampling
 * and the delivery-rate samples that BBR needs.
 *
 * Not implemented: timestamps (so no PAWS and no RTT from retransmissions),
 * path-MTU discovery, tail-loss probes, SACK reneging; TIME_WAIT lasts one
 * second; BBR has no long-term (policer) sampling.
 */
#include "nl_int.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TH_FIN 0x01
#define TH_SYN 0x02
#define TH_RST 0x04
#define TH_PSH 0x08
#define TH_ACK 0x10
#define TH_ECE 0x40
#define TH_CWR 0x80

#define SEQ_LT(a, b)  ((int32_t)((a) - (b)) < 0)
#define SEQ_LEQ(a, b) ((int32_t)((a) - (b)) <= 0)
#define SEQ_GT(a, b)  ((int32_t)((a) - (b)) > 0)
#define SEQ_GEQ(a, b) ((int32_t)((a) - (b)) >= 0)

enum { ST_CLOSED, ST_SYN_SENT, ST_SYN_RCVD, ST_ESTABLISHED, ST_FIN_WAIT_1, ST_FIN_WAIT_2,
       ST_CLOSE_WAIT, ST_CLOSING, ST_LAST_ACK, ST_TIME_WAIT };
static const char *const ST_NAME[] = { "CLOSED", "SYN_SENT", "SYN_RCVD", "ESTABLISHED", "FIN_WAIT_1",
                                       "FIN_WAIT_2", "CLOSE_WAIT", "CLOSING", "LAST_ACK", "TIME_WAIT" };

enum { CC_TAHOE, CC_RENO, CC_NEWRENO, CC_CUBIC, CC_BBR };
static const char *const CC_NAME[] = { "tahoe", "reno", "newreno", "cubic", "bbr" };

enum { BBR_STARTUP, BBR_DRAIN, BBR_PROBE_BW, BBR_PROBE_RTT };

#define OOO_MAX   64
#define MAX_LIS   8
#define RTO_MAX   NL_SEC(60)
#define TICK      NL_MS(200)

typedef struct { uint32_t seq; int len; uint8_t *data; } ooo_seg;

/* Scoreboard record: one transmitted segment not yet cumulatively acknowledged */
typedef struct {
    uint32_t seq;
    int      len;                       /* a FIN takes one sequence number */
    uint8_t  sacked, lost, retx, ever_retx, fin, app_limited;
    nl_time  xmit;                      /* last (re)transmission */
    uint64_t delivered;                 /* connection state when it was sent: */
    nl_time  delivered_time, first_tx;  /* for the delivery-rate sample */
} txseg;

/* What one ACK tells: the delivery-rate sample and the RTT */
typedef struct {
    int      have, app_limited, newly_acked;
    uint64_t prior_delivered;
    nl_time  send_elapsed, ack_elapsed, rack;
    nl_time  rtt;                       /* of the first segment acknowledged: for the RTO */
    nl_time  rtt_min;                   /* smallest over the segments delivered: for RTprop */
} rsamp;

/* An endpoint address of either family. The whole of TCP is the same over
 * IPv4 and IPv6: only the pseudo-header of the checksum, the room left for
 * the MSS and the network-layer send call differ. */
typedef struct { int v6; nl_ip v4; nl_ip6 a6; } taddr;

static taddr A4(nl_ip ip)
{
    taddr a;
    memset(&a, 0, sizeof(a));
    a.v4 = ip;
    return a;
}

static taddr A6(const nl_ip6 *ip)
{
    taddr a;
    memset(&a, 0, sizeof(a));
    a.v6 = 1;
    a.a6 = *ip;
    return a;
}

static int taddr_eq(const taddr *a, const taddr *b)
{
    return a->v6 == b->v6 && (a->v6 ? nl_ip6_eq(&a->a6, &b->a6) : a->v4 == b->v4);
}

/* "10.0.0.1" or "[2001:db8::1]", ready to be followed by :port */
static const char *taddr_str(const taddr *a)
{
    static char buf[4][64];
    static int k;

    if (!a->v6)
        return nl_ip_str(a->v4);
    k = (k + 1) % 4;
    snprintf(buf[k], sizeof(buf[k]), "[%s]", nl_ip6_str(&a->a6));
    return buf[k];
}

/* A received segment, whichever network layer delivered it */
typedef struct {
    taddr    src, dst;
    uint8_t  tos;
    uint32_t pkt_id;
    const uint8_t *l4;
    int      l4len;
} seg_in;

struct nl_tcp {
    nl_node *n;
    struct nl_tcp *next;
    taddr    lip, rip;
    int      pmtud;                     /* path MTU discovery: DF on IPv4, always on IPv6 */
    uint16_t lport, rport;
    int      state, active;             /* active = we sent the first SYN */
    const nl_tcp_ops *ops;
    void    *ctx;
    const nl_tcp_ops *lops;             /* passive side before accept */
    void    *lctx;

    /* sender */
    uint32_t iss, snd_una, snd_nxt, snd_max, fin_seq;
    uint32_t snd_wnd;                   /* window offered by the peer, bytes */
    uint8_t *sbuf;                      /* bytes from snd_una on */
    int      scap, slen;
    int      fin_pending, fin_sent, fin_acked;
    int      mss, snd_wscale, rcv_wscale, ws_ok;

    /* retransmission */
    nl_time  srtt, rttvar, rto, rto_min;
    int      rtt_valid, backoff, tries;
    nl_time  rtt_time;                  /* when the SYN left */
    nl_ev    rto_timer, persist_timer, wait_timer, tick_timer, pace_timer, drain_timer, delack_timer;
    int      persist_backoff;
    txseg   *sq;                        /* scoreboard, in sequence order */
    int      sq_n, sq_cap;
    int      sack_want, sack_peer, sack_ok, nagle;
    int      pseudo;                    /* without SACK: duplicate ACKs = segments that left the network */
    int      rto_recovery;              /* retransmitting after a timeout */
    int      force_retx;                /* the retransmission that starts recovery ignores the window */
    int      partial_acks;              /* in this NewReno recovery */

    /* congestion control */
    int      cc, iw, init_ssthresh;
    double   cwnd, ssthresh;            /* bytes */
    int      dupacks, in_recovery;
    uint32_t recover;
    double   w_max, cub_k, cub_origin, w_est;     /* cubic, in segments */
    nl_time  epoch;
    int      ecn_ok, ecn_want, ece_latch, cwr_pending;
    uint32_t ecn_recover;
    /* delivery-rate estimation */
    uint64_t delivered, app_limited;
    nl_time  delivered_time, first_tx_time;
    /* bbr */
    int      bbr_state, bbr_cycle, bbr_full_cnt, bbr_filled, bbr_pc, bbr_probe_rtt_round, idle_restart;
    double   bbr_pacing_gain, bbr_cwnd_gain, bbr_full_bw, bbr_prior_cwnd, pace_rate;   /* rates in bytes/s */
    double   bbr_bw_r[10];
    uint64_t bbr_bw_round[10], bbr_round, bbr_next_round, bbr_pc_end;
    nl_time  bbr_rtprop, bbr_rtprop_stamp, bbr_cycle_stamp, bbr_probe_rtt_done, next_tx;

    /* receiver */
    uint32_t irs, rcv_nxt;
    int      rwnd_max, last_adv;
    ooo_seg  ooo[OOO_MAX];
    int      nooo;
    uint32_t sack_recent[4];            /* out-of-order arrivals, newest first (RFC 2018 block order) */
    int      nsack_recent, dsack_pending;
    uint32_t dsack_lo, dsack_hi;
    uint32_t rx_blk[8];                 /* SACK blocks of the segment being processed */
    int      rx_nblk;
    nl_time  delack;                    /* delayed-ACK timer, 0 = acknowledge every segment */
    int      ack_pending;
    uint8_t *rbuf;                      /* only with a slow reader (readrate) */
    int      rlen, eof_pending;
    double   read_rate;                 /* bytes/s, 0 = the application reads at once */

    /* statistics */
    uint64_t bytes_acked, bytes_rcvd, tick_acked;
    unsigned retx, fast_retx, timeouts;
};

typedef struct {
    uint16_t port;
    const nl_tcp_ops *ops;
    void    *ctx;
    char     opts[120];
} listener;

typedef struct {
    nl_node *n;
    nl_tcp  *conns;
    listener lis[MAX_LIS];
    int      nlis;
    uint16_t eph;
    nl_tcp  *sampler;                   /* connection whose cwnd is plotted */
    /* the `tcp` tool */
    int      sink_port;
    char     sink_opts[200];
    uint64_t sink_bytes;
} tcp_node;

static const nl_module tcp_mod;
static void tcp_input4(nl_node *n, void *ctx, const nl_ipmeta *m);
static void tcp_input6(nl_node *n, void *ctx, const nl_ip6meta *m);
static void tcp_icmp4(nl_node *n, void *ctx, const nl_icmpmeta *m);
static void tcp_icmp6(nl_node *n, void *ctx, const nl_icmp6meta *m);
static void tcp_output(nl_tcp *c);
static void rto_fire(void *ctx);

static tcp_node *tcp_get(nl_node *n)
{
    tcp_node *tn = nl_mod_get(n, &tcp_mod);

    if (tn)
        return tn;
    tn = calloc(1, sizeof(*tn));
    if (!tn)
        return NULL;
    tn->n = n;
    tn->eph = (uint16_t)(49152 + n->idx * 16);
    nl_mod_set(n, &tcp_mod, tn);
    nl_ip_bind(n, NL_PROTO_TCP, tcp_input4, tn);
    nl_ip6_bind(n, NL_PROTO_TCP, tcp_input6, tn);
    nl_icmp_watch(n, tcp_icmp4, tn);
    nl_icmp6_watch(n, tcp_icmp6, tn);
    return tn;
}

/* ------------------------------------------------------------------ */
/* Connection objects                                                 */
/* ------------------------------------------------------------------ */

static void free_later(void *ctx) { free(ctx); }

static void conn_destroy(nl_tcp *c)
{
    tcp_node *tn = nl_mod_get(c->n, &tcp_mod);
    nl_tcp **pp;
    int i;

    if (c->state == ST_CLOSED && !c->sbuf)
        return;
    for (pp = tn ? &tn->conns : NULL; pp && *pp; pp = &(*pp)->next)
        if (*pp == c) {
            *pp = c->next;
            break;
        }
    if (tn && tn->sampler == c)
        tn->sampler = NULL;
    nl_cancel(&c->rto_timer);
    nl_cancel(&c->persist_timer);
    nl_cancel(&c->wait_timer);
    nl_cancel(&c->tick_timer);
    nl_cancel(&c->pace_timer);
    nl_cancel(&c->drain_timer);
    nl_cancel(&c->delack_timer);
    free(c->sq);
    c->sq = NULL;
    c->sq_n = c->sq_cap = 0;
    for (i = 0; i < c->nooo; i++)
        free(c->ooo[i].data);
    c->nooo = 0;
    free(c->sbuf);
    free(c->rbuf);
    c->sbuf = c->rbuf = NULL;
    c->state = ST_CLOSED;
    c->ops = NULL;
    nl_after(NULL, 0, free_later, c);   /* callers up the stack may still look at it */
}

/* Tell the application the connection is gone (once) */
static void conn_gone(nl_tcp *c, int reason)
{
    const nl_tcp_ops *ops = c->ops;
    void *ctx = c->ctx;

    c->ops = NULL;
    if (ops && ops->on_close)
        ops->on_close(c, ctx, reason);
}

static void conn_abort(nl_tcp *c, int reason)
{
    conn_gone(c, reason);
    conn_destroy(c);
}

static void conn_opts(nl_tcp *c, const char *opts)
{
    nl_args a;
    const char *cc;
    int i;

    nl_args_parse(&a, opts ? opts : "");
    cc = nl_arg_str(&a, "cc", "reno");
    c->cc = CC_RENO;
    for (i = 0; i < 5; i++)
        if (strcmp(cc, CC_NAME[i]) == 0)
            c->cc = i;
    c->mss = (int)nl_arg_int(&a, "mss", 1460);
    c->pmtud = (int)nl_arg_int(&a, "pmtud", 1);
    c->rwnd_max = (int)nl_arg_rate(&a, "rwnd", 1 << 20);     /* sizes accept k and M */
    if (c->rwnd_max < 256) c->rwnd_max = 256;
    if (c->rwnd_max > (1 << 24)) c->rwnd_max = 1 << 24;
    c->scap = (int)nl_arg_rate(&a, "sndbuf", 256 * 1024);
    if (c->scap < 2048) c->scap = 2048;
    if (c->scap > (1 << 24)) c->scap = 1 << 24;
    c->ecn_want = (int)nl_arg_int(&a, "ecn", 0);
    c->iw = (int)nl_arg_int(&a, "iw", 10);
    if (c->iw < 1) c->iw = 1;
    c->init_ssthresh = (int)nl_arg_int(&a, "ssthresh", 0);
    c->rto_min = nl_arg_time(&a, "rto_min", NL_MS(200));
    c->read_rate = nl_arg_rate(&a, "readrate", 0);
    c->sack_want = (int)nl_arg_int(&a, "sack", 1) && c->cc != CC_TAHOE;    /* Tahoe predates SACK */
    c->nagle = (int)nl_arg_int(&a, "nagle", 1);
    c->delack = nl_arg_time(&a, "delack", NL_MS(40));
    while ((c->rwnd_max >> c->rcv_wscale) > 65535)
        c->rcv_wscale++;
}

static nl_tcp *conn_new(tcp_node *tn, taddr lip, uint16_t lport, taddr rip, uint16_t rport, const char *opts)
{
    nl_tcp *c = calloc(1, sizeof(*c));
    int pmtu;

    if (!c)
        return NULL;
    c->n = tn->n;
    c->lip = lip;
    c->lport = lport;
    c->rip = rip;
    c->rport = rport;
    conn_opts(c, opts);
    /* room for the segment: the MTU less the IP and TCP headers (40 or 60 bytes) */
    pmtu = rip.v6 ? nl_ip6_mtu(tn->n, &rip.a6) - 60 : nl_ip_mtu(tn->n, rip.v4) - 40;
    if (c->mss > pmtu) c->mss = pmtu;
    if (c->mss < 64) c->mss = 64;
    c->sbuf = malloc((size_t)c->scap);
    if (c->read_rate > 0)
        c->rbuf = malloc((size_t)c->rwnd_max);
    if (!c->sbuf || (c->read_rate > 0 && !c->rbuf)) {
        free(c->sbuf);
        free(c->rbuf);
        free(c);
        return NULL;
    }
    c->iss = 1000 + nl_rand(0x3fffffff);
    c->snd_una = c->snd_nxt = c->snd_max = c->iss;
    c->recover = c->iss;
    c->ecn_recover = c->iss;
    c->rto = NL_SEC(1);
    c->ssthresh = 1e12;
    c->snd_wnd = 65535;
    c->last_adv = c->rwnd_max;
    c->next = tn->conns;
    tn->conns = c;
    return c;
}

static nl_tcp *conn_find(tcp_node *tn, taddr lip, uint16_t lport, taddr rip, uint16_t rport)
{
    nl_tcp *c;

    for (c = tn->conns; c; c = c->next)
        if (c->lport == lport && c->rport == rport && taddr_eq(&c->rip, &rip) && taddr_eq(&c->lip, &lip))
            return c;
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Sending segments                                                   */
/* ------------------------------------------------------------------ */

static int rcv_window(const nl_tcp *c)
{
    return c->rwnd_max - c->rlen;
}

static void raw_xmit(nl_node *n, taddr src, taddr dst, uint16_t sport, uint16_t dport, uint32_t seq,
                     uint32_t ack, int flags, int win, const uint8_t *opt, int optlen,
                     const uint8_t *data, int len, uint8_t tos, int df)
{
    static uint8_t b[60 + NL_FRAME_MAX];
    nl_iptx tx;
    int hl = 20 + optlen;

    nl_put16(b, sport);
    nl_put16(b + 2, dport);
    nl_put32(b + 4, seq);
    nl_put32(b + 8, ack);
    b[12] = (uint8_t)((hl / 4) << 4);
    b[13] = (uint8_t)flags;
    nl_put16(b + 14, (uint16_t)(win > 65535 ? 65535 : win));
    nl_put16(b + 16, 0);
    nl_put16(b + 18, 0);
    if (optlen)
        memcpy(b + 20, opt, (size_t)optlen);
    if (len)
        memcpy(b + hl, data, (size_t)len);
    if (dst.v6) {
        nl_ip6tx tx6;

        nl_put16(b + 16, nl_l4_cksum6(&src.a6, &dst.a6, NL_PROTO_TCP, b, hl + len));
        memset(&tx6, 0, sizeof(tx6));
        tx6.src = src.a6;
        tx6.dst = dst.a6;
        tx6.proto = NL_PROTO_TCP;
        tx6.tclass = tos;
        nl_ip6_send(n, &tx6, b, hl + len);
        return;
    }
    nl_put16(b + 16, nl_l4_cksum(src.v4, dst.v4, NL_PROTO_TCP, b, hl + len));
    memset(&tx, 0, sizeof(tx));
    tx.src = src.v4;
    tx.dst = dst.v4;
    tx.proto = NL_PROTO_TCP;
    tx.tos = tos;
    tx.df = df;
    nl_ip_send(n, &tx, b, hl + len);
}

/* SACK option for an ACK: the D-SACK block if any, then the block holding
 * the most recent arrival, then the others, newest first (RFC 2018). */
static int sack_option(nl_tcp *c, uint8_t *opt)
{
    uint32_t lo[OOO_MAX], hi[OOO_MAX], out[8];
    int n = 0, nout = 0, first = 0, i, k, r;

    for (i = 0; i < c->nooo; i++) {                     /* sorted intervals... */
        uint32_t a = c->ooo[i].seq, b = a + (uint32_t)c->ooo[i].len;
        for (k = n; k > 0 && SEQ_LT(a, lo[k - 1]); k--) {
            lo[k] = lo[k - 1];
            hi[k] = hi[k - 1];
        }
        lo[k] = a;
        hi[k] = b;
        n++;
    }
    for (i = 0, k = 0; i < n; i++) {                    /* ...merged */
        if (k > 0 && SEQ_LEQ(lo[i], hi[k - 1])) {
            if (SEQ_GT(hi[i], hi[k - 1]))
                hi[k - 1] = hi[i];
        } else {
            lo[k] = lo[i];
            hi[k] = hi[i];
            k++;
        }
    }
    n = k;
    if (c->dsack_pending) {
        out[0] = c->dsack_lo;
        out[1] = c->dsack_hi;
        nout = first = 1;
        c->dsack_pending = 0;
    }
    for (r = 0; r <= c->nsack_recent && nout < 4; r++)
        for (i = 0; i < n && nout < 4; i++) {
            /* pass r < nsack_recent: the block of the r-th newest arrival; last pass: all the rest */
            if (r < c->nsack_recent && !(SEQ_GEQ(c->sack_recent[r], lo[i]) && SEQ_LT(c->sack_recent[r], hi[i])))
                continue;
            for (k = first; k < nout; k++)
                if (out[2 * k] == lo[i])
                    break;
            if (k < nout)
                continue;
            out[2 * nout] = lo[i];
            out[2 * nout + 1] = hi[i];
            nout++;
        }
    if (!nout)
        return 0;
    opt[0] = opt[1] = 1;                                /* NOP NOP */
    opt[2] = 5;
    opt[3] = (uint8_t)(2 + 8 * nout);
    for (i = 0; i < 2 * nout; i++)
        nl_put32(opt + 4 + 4 * i, out[i]);
    return 4 + 8 * nout;
}

static void seg_xmit(nl_tcp *c, uint32_t seq, int flags, const uint8_t *data, int len)
{
    uint8_t opt[40] = { 0 };
    int optlen = 0, win = rcv_window(c);

    if (flags & TH_SYN) {
        opt[0] = 2; opt[1] = 4;                         /* MSS */
        nl_put16(opt + 2, (uint16_t)c->mss);
        opt[4] = 1;                                     /* NOP */
        opt[5] = 3; opt[6] = 3; opt[7] = (uint8_t)c->rcv_wscale;   /* window scale */
        optlen = 8;
        if ((flags & TH_ACK) ? c->sack_ok : c->sack_want) {
            opt[8] = opt[9] = 1;
            opt[10] = 4; opt[11] = 2;                   /* SACK permitted */
            optlen = 12;
        }
        if (win > 65535)
            win = 65535;
        if (c->ecn_want)
            flags |= (flags & TH_ACK) ? (c->ecn_ok ? TH_ECE : 0) : (TH_ECE | TH_CWR);
    } else {
        c->last_adv = win;
        win >>= c->rcv_wscale;
        if (c->ece_latch)
            flags |= TH_ECE;
        if (c->cwr_pending && len > 0) {
            flags |= TH_CWR;
            c->cwr_pending = 0;
        }
        if (c->sack_ok && (c->nooo || c->dsack_pending))
            optlen = sack_option(c, opt);
        c->ack_pending = 0;                             /* every segment carries the ACK */
        nl_cancel(&c->delack_timer);
    }
    raw_xmit(c->n, c->lip, c->rip, c->lport, c->rport, seq, c->rcv_nxt, flags, win, opt, optlen, data, len,
             (uint8_t)(c->ecn_ok && len > 0 ? 2 : 0), c->pmtud);
}

static void send_ack(nl_tcp *c)
{
    seg_xmit(c, c->snd_nxt, TH_ACK, NULL, 0);
}

static void delack_fire(void *ctx)
{
    nl_tcp *c = ctx;

    c->delack_timer = 0;
    if (c->state != ST_CLOSED && c->ack_pending)
        send_ack(c);
}

static void rto_arm(nl_tcp *c)
{
    nl_time t = c->rto << c->backoff;

    nl_cancel(&c->rto_timer);
    c->rto_timer = nl_after(c->n, t > RTO_MAX ? RTO_MAX : t, rto_fire, c);
}

/* ------------------------------------------------------------------ */
/* Scoreboard: one record per segment in flight                       */
/* ------------------------------------------------------------------ */

static txseg *sq_push(nl_tcp *c, uint32_t seq, int len)
{
    txseg *s;

    if (c->sq_n == c->sq_cap) {
        int ncap = c->sq_cap ? c->sq_cap * 2 : 64;
        txseg *nq = realloc(c->sq, (size_t)ncap * sizeof(*nq));
        if (!nq)
            return NULL;
        c->sq = nq;
        c->sq_cap = ncap;
    }
    s = &c->sq[c->sq_n++];
    memset(s, 0, sizeof(*s));
    s->seq = seq;
    s->len = len;
    return s;
}

/* RFC 6675 "pipe": bytes we believe are still in the network. Without
 * SACK every duplicate ACK stands for one segment that left it. */
static int pipe_bytes(const nl_tcp *c)
{
    long pipe = 0;
    int i;

    for (i = 0; i < c->sq_n; i++) {
        const txseg *s = &c->sq[i];
        if (s->sacked)
            continue;
        if (!s->lost)
            pipe += s->len;
        if (s->retx)
            pipe += s->len;
    }
    pipe -= (long)c->pseudo * c->mss;
    return pipe < 0 ? 0 : (int)pipe;
}

/* A segment is going out: remember when, and what had been delivered by
 * then (delivery-rate estimation, draft-cheng-iccrg-delivery-rate-estimation).
 * idle = nothing else was in flight. */
static void seg_stamp(nl_tcp *c, txseg *s, int idle)
{
    if (idle || !c->delivered_time)
        c->first_tx_time = c->delivered_time = nl_now();
    s->xmit = nl_now();
    s->delivered = c->delivered;
    s->delivered_time = c->delivered_time;
    s->first_tx = c->first_tx_time;
    s->app_limited = c->app_limited != 0;
}

static void seg_send(nl_tcp *c, const txseg *s)
{
    int off = (int)(s->seq - c->snd_una);

    if (s->fin) {
        seg_xmit(c, s->seq, TH_FIN | TH_ACK, NULL, 0);
        return;
    }
    nl_log(c->n, NL_L_TRANSPORT, 3, 0, T_DATA "TCP" T_RESET " :%u → seq=%u len=%d%s%s", c->lport, s->seq - c->iss,
           s->len, s->retx ? "  " : "", s->retx ? _("retransmission") : "");
    seg_xmit(c, s->seq, TH_ACK | (off + s->len >= c->slen ? TH_PSH : 0), c->sbuf + off, s->len);
}

static void persist_fire(void *ctx)
{
    nl_tcp *c = ctx;

    c->persist_timer = 0;
    if (c->state == ST_CLOSED || c->snd_wnd > 0 || c->slen == 0)
        return;
    /* one byte past the closed window: the answer carries the current window */
    nl_log(c->n, NL_L_TRANSPORT, 2, 0, T_DATA "TCP" T_RESET " :%u %s", c->lport, _("zero-window probe"));
    nl_count(c->n, "tcp.zero_window_probes", 1);
    if (c->sq_n == 0) {
        txseg *s = sq_push(c, c->snd_una, 1);
        if (s) {
            seg_stamp(c, s, 1);
            c->snd_nxt = c->snd_max = c->snd_una + 1;
        }
    }
    seg_xmit(c, c->snd_una, TH_ACK, c->sbuf, 1);
    if (c->persist_backoff < 5)
        c->persist_backoff++;
    c->persist_timer = nl_after(c->n, NL_MS(500) << c->persist_backoff, persist_fire, c);
}

static void pace_fire(void *ctx)
{
    nl_tcp *c = ctx;

    c->pace_timer = 0;
    if (c->state != ST_CLOSED)
        tcp_output(c);
}

/* Pacing (bbr): may a segment of len bytes leave now? */
static int pace_ok(nl_tcp *c, int len)
{
    if (c->pace_rate <= 0)
        return 1;
    if (nl_now() < c->next_tx) {
        if (!c->pace_timer)
            c->pace_timer = nl_after(c->n, c->next_tx - nl_now(), pace_fire, c);
        return 0;
    }
    c->next_tx = (nl_now() > c->next_tx ? nl_now() : c->next_tx) + (nl_time)(len * 1e6 / c->pace_rate);
    return 1;
}

/* Send what the windows allow, in the order of RFC 6675 NextSeg(): first
 * the segments known to be lost, then new data. */
static void tcp_output(nl_tcp *c)
{
    int i, pipe = 0, lost_waiting = 0, unsent;

    if (c->state < ST_ESTABLISHED || c->state == ST_TIME_WAIT)
        return;
    for (;;) {
        txseg *s = NULL;
        int sent = (int)(c->snd_nxt - c->snd_una) - (c->fin_sent ? 1 : 0), avail = c->slen - sent, len;
        long wroom;

        pipe = pipe_bytes(c);
        for (i = 0; i < c->sq_n; i++)
            if (c->sq[i].lost && !c->sq[i].retx && !c->sq[i].sacked) {
                s = &c->sq[i];
                break;
            }
        lost_waiting = s != NULL;
        if (s) {
            if (pipe > 0 && pipe + s->len > (int)c->cwnd && !c->force_retx)
                break;
            if (!c->force_retx && !pace_ok(c, s->len))
                break;
            c->force_retx = 0;
            seg_stamp(c, s, pipe == 0);
            s->retx = s->ever_retx = 1;
            c->retx++;
            nl_count(c->n, "tcp.retransmits", 1);
            if (c->sack_ok && c->in_recovery)
                nl_count(c->n, "tcp.sack_retransmits", 1);
            seg_send(c, s);
            if (!c->rto_timer)
                rto_arm(c);
            continue;
        }
        if (avail <= 0 || c->fin_sent)
            break;
        len = avail < c->mss ? avail : c->mss;
        wroom = (long)c->snd_wnd - sent;
        if (wroom <= 0)
            break;
        if (pipe > 0 && pipe + len > (int)c->cwnd)
            break;
        if (len > wroom) {
            /* avoid silly small segments unless nothing is in flight */
            if (sent > 0 && wroom < c->mss)
                break;
            len = (int)wroom;
        }
        /* Nagle: a small segment waits while earlier data is unacknowledged */
        if (c->nagle && len < c->mss && sent > 0 && !c->fin_pending)
            break;
        if (!pace_ok(c, len))
            break;
        if (pipe == 0 && c->app_limited)
            c->idle_restart = 1;
        s = sq_push(c, c->snd_nxt, len);
        if (!s)
            break;
        seg_stamp(c, s, pipe == 0);
        seg_send(c, s);
        c->snd_nxt += (uint32_t)len;
        c->snd_max = c->snd_nxt;
        if (!c->rto_timer)
            rto_arm(c);
    }
    /* nothing left to send although the window would allow more: the
     * delivery-rate samples taken from now on are application-limited */
    unsent = c->slen - ((int)(c->snd_nxt - c->snd_una) - (c->fin_sent ? 1 : 0));
    if (!lost_waiting && unsent <= 0 && pipe < (int)c->cwnd) {
        c->app_limited = c->delivered + (uint64_t)pipe;
        if (!c->app_limited)
            c->app_limited = 1;
    }

    if (c->fin_pending && !c->fin_sent && (int)(c->snd_nxt - c->snd_una) == c->slen) {
        txseg *s = sq_push(c, c->snd_nxt, 1);

        if (s) {
            s->fin = 1;
            seg_stamp(c, s, pipe == 0);
            c->fin_seq = c->snd_nxt;
            c->fin_sent = 1;
            seg_send(c, s);
            c->snd_nxt++;
            c->snd_max = c->snd_nxt;
            if (c->state == ST_ESTABLISHED)
                c->state = ST_FIN_WAIT_1;
            else if (c->state == ST_CLOSE_WAIT)
                c->state = ST_LAST_ACK;
            nl_log(c->n, NL_L_TRANSPORT, 2, 0, T_DATA "TCP" T_RESET " :%u → %s:%u FIN (%s)", c->lport,
                   taddr_str(&c->rip), c->rport, ST_NAME[c->state]);
            if (!c->rto_timer)
                rto_arm(c);
        }
    }
    if (c->snd_wnd == 0 && c->slen > 0 && c->snd_una == c->snd_max && !c->persist_timer) {
        c->persist_backoff = 0;
        c->persist_timer = nl_after(c->n, NL_MS(500), persist_fire, c);
    }
}

/* ------------------------------------------------------------------ */
/* Congestion control                                                 */
/* ------------------------------------------------------------------ */

#define BBR_HIGH_GAIN   2.885390                /* 2 / ln 2 */
#define BBR_BW_ROUNDS   10
#define BBR_RTPROP_WIN  NL_SEC(10)
#define BBR_PROBE_RTT_T NL_MS(200)
#define BBR_MIN_PIPE    4                       /* segments */

static const char *const BBR_NAME[] = { "STARTUP", "DRAIN", "PROBE_BW", "PROBE_RTT" };

static double bbr_btlbw(const nl_tcp *c)
{
    double bw = 0;
    int i;

    for (i = 0; i < BBR_BW_ROUNDS; i++)
        if (c->bbr_bw_r[i] > bw && c->bbr_bw_round[i] + BBR_BW_ROUNDS > c->bbr_round)
            bw = c->bbr_bw_r[i];
    return bw;
}

/* The amount of data the model wants in flight at a given gain */
static double bbr_inflight(const nl_tcp *c, double gain)
{
    double bw = bbr_btlbw(c);

    if (!c->bbr_rtprop || bw <= 0)
        return (double)c->iw * c->mss;
    return gain * bw * (double)c->bbr_rtprop / 1e6 + 3.0 * c->mss;
}

static void bbr_save_cwnd(nl_tcp *c)
{
    if (!c->in_recovery && !c->rto_recovery && c->bbr_state != BBR_PROBE_RTT)
        c->bbr_prior_cwnd = c->cwnd;
    else if (c->cwnd > c->bbr_prior_cwnd)
        c->bbr_prior_cwnd = c->cwnd;
}

static void bbr_advance_cycle(nl_tcp *c)
{
    static const double cycle[8] = { 1.25, 0.75, 1, 1, 1, 1, 1, 1 };

    c->bbr_cycle_stamp = nl_now();
    c->bbr_cycle = (c->bbr_cycle + 1) % 8;
    c->bbr_pacing_gain = cycle[c->bbr_cycle];
}

static void bbr_enter_probe_bw(nl_tcp *c)
{
    c->bbr_state = BBR_PROBE_BW;
    c->bbr_pacing_gain = 1.0;
    c->bbr_cwnd_gain = 2.0;
    c->bbr_cycle = 7 - (int)nl_rand(7);         /* any phase but the 0.75 one */
    bbr_advance_cycle(c);
}

static void bbr_enter_startup(nl_tcp *c)
{
    c->bbr_state = BBR_STARTUP;
    c->bbr_pacing_gain = c->bbr_cwnd_gain = BBR_HIGH_GAIN;
}

static void bbr_log(nl_tcp *c, const char *why)
{
    char name[24];

    snprintf(name, sizeof(name), "tcp.bbr_%s", c->bbr_state == BBR_STARTUP ? "startup"
             : c->bbr_state == BBR_DRAIN ? "drain" : c->bbr_state == BBR_PROBE_BW ? "probe_bw" : "probe_rtt");
    nl_count(c->n, name, 1);
    nl_log(c->n, NL_L_TRANSPORT, 2, 0, T_DATA "TCP" T_RESET " :%u bbr → " T_BOLD "%s" T_RESET "  %s  " T_DIM
           "BtlBw %s  RTprop %s", c->lport, BBR_NAME[c->bbr_state], why, nl_rate_str(bbr_btlbw(c) * 8),
           nl_time_str(c->bbr_rtprop));
}

/* BBR v1 (Cardwell, Cheng, Gunn, Yeganeh, Jacobson, "BBR: Congestion-Based
 * Congestion Control", 2016; draft-cardwell-iccrg-bbr-congestion-control-00).
 * Called for every ACK with the delivery-rate sample it produced. */
static void bbr_on_ack(nl_tcp *c, const rsamp *rs, int prior_inflight, int newly_lost)
{
    double rate = -1, bw, target;
    int pipe = pipe_bytes(c), expired, round_start = 0;

    if (c->app_limited && c->delivered > c->app_limited)
        c->app_limited = 0;
    if (rs->have) {
        nl_time interval = rs->send_elapsed > rs->ack_elapsed ? rs->send_elapsed : rs->ack_elapsed;

        /* an interval shorter than the minimum RTT would overestimate */
        if (interval > 0 && interval >= c->bbr_rtprop && c->delivered > rs->prior_delivered)
            rate = (double)(c->delivered - rs->prior_delivered) * 1e6 / (double)interval;
        if (rs->prior_delivered >= c->bbr_next_round) {         /* a round trip has passed */
            c->bbr_next_round = c->delivered;
            c->bbr_round++;
            round_start = 1;
            if (c->bbr_pc && rs->prior_delivered >= c->bbr_pc_end)
                c->bbr_pc = 0;
        }
    }
    bw = bbr_btlbw(c);
    if (rate > 0 && (rate >= bw || !rs->app_limited)) {         /* windowed max over 10 rounds */
        int k = (int)(c->bbr_round % BBR_BW_ROUNDS);
        if (c->bbr_bw_round[k] != c->bbr_round) {
            c->bbr_bw_round[k] = c->bbr_round;
            c->bbr_bw_r[k] = 0;
        }
        if (rate > c->bbr_bw_r[k])
            c->bbr_bw_r[k] = rate;
        bw = bbr_btlbw(c);
    }

    if (c->bbr_state == BBR_PROBE_BW) {                         /* gain cycling */
        int full = nl_now() - c->bbr_cycle_stamp > c->bbr_rtprop, next;

        if (c->bbr_pacing_gain == 1.0)
            next = full;
        else if (c->bbr_pacing_gain > 1.0)
            next = full && (newly_lost > 0 || prior_inflight >= (int)bbr_inflight(c, c->bbr_pacing_gain));
        else
            next = full || prior_inflight <= (int)bbr_inflight(c, 1.0);
        if (next)
            bbr_advance_cycle(c);
    }
    if (!c->bbr_filled && round_start && !rs->app_limited) {    /* has the pipe been filled? */
        if (bw >= c->bbr_full_bw * 1.25) {
            c->bbr_full_bw = bw;
            c->bbr_full_cnt = 0;
        } else if (++c->bbr_full_cnt >= 3) {
            c->bbr_filled = 1;
        }
    }
    if (c->bbr_state == BBR_STARTUP && c->bbr_filled) {
        c->bbr_state = BBR_DRAIN;
        c->bbr_pacing_gain = 1.0 / BBR_HIGH_GAIN;
        c->bbr_cwnd_gain = BBR_HIGH_GAIN;
        bbr_log(c, _("the delivery rate stopped growing: the pipe is full, drain the queue"));
    }
    if (c->bbr_state == BBR_DRAIN && pipe <= (int)bbr_inflight(c, 1.0)) {
        bbr_enter_probe_bw(c);
        bbr_log(c, _("queue drained: cruise and probe for more bandwidth"));
    }

    expired = c->bbr_rtprop && nl_now() > c->bbr_rtprop_stamp + BBR_RTPROP_WIN;
    if (rs->rtt_min >= 0 && (!c->bbr_rtprop || rs->rtt_min <= c->bbr_rtprop || expired)) {
        c->bbr_rtprop = rs->rtt_min;
        c->bbr_rtprop_stamp = nl_now();
    }
    if (c->bbr_state != BBR_PROBE_RTT && expired && !c->idle_restart) {
        bbr_save_cwnd(c);
        c->bbr_state = BBR_PROBE_RTT;
        c->bbr_pacing_gain = c->bbr_cwnd_gain = 1.0;
        c->bbr_probe_rtt_done = 0;
        bbr_log(c, _("no new minimum RTT for 10 s: empty the pipe to measure it"));
    }
    if (c->bbr_state == BBR_PROBE_RTT) {
        c->app_limited = c->delivered + (uint64_t)pipe;
        if (!c->app_limited)
            c->app_limited = 1;
        if (!c->bbr_probe_rtt_done && pipe <= BBR_MIN_PIPE * c->mss) {
            c->bbr_probe_rtt_done = nl_now() + BBR_PROBE_RTT_T;
            c->bbr_probe_rtt_round = 0;
            c->bbr_next_round = c->delivered;
        } else if (c->bbr_probe_rtt_done) {
            if (round_start)
                c->bbr_probe_rtt_round = 1;
            if (c->bbr_probe_rtt_round && nl_now() > c->bbr_probe_rtt_done) {
                c->bbr_rtprop_stamp = nl_now();
                if (c->cwnd < c->bbr_prior_cwnd)
                    c->cwnd = c->bbr_prior_cwnd;
                if (c->bbr_filled)
                    bbr_enter_probe_bw(c);
                else
                    bbr_enter_startup(c);
                bbr_log(c, _("minimum RTT refreshed"));
            }
        }
    }
    c->idle_restart = 0;

    if (bw > 0 && (c->bbr_filled || c->bbr_pacing_gain * bw > c->pace_rate))
        c->pace_rate = c->bbr_pacing_gain * bw;

    target = bbr_inflight(c, c->bbr_cwnd_gain);
    if (newly_lost > 0) {
        c->cwnd -= newly_lost;
        if (c->cwnd < c->mss)
            c->cwnd = c->mss;
    }
    if (c->bbr_pc) {                                            /* packet conservation */
        if (c->cwnd < (double)pipe + rs->newly_acked)
            c->cwnd = (double)pipe + rs->newly_acked;
    } else if (c->bbr_filled) {
        c->cwnd = c->cwnd + rs->newly_acked < target ? c->cwnd + rs->newly_acked : target;
    } else if (c->cwnd < target || c->delivered < (uint64_t)c->iw * (uint64_t)c->mss) {
        c->cwnd += rs->newly_acked;
    }
    if (c->cwnd < (double)BBR_MIN_PIPE * c->mss)
        c->cwnd = (double)BBR_MIN_PIPE * c->mss;
    if (c->bbr_state == BBR_PROBE_RTT && c->cwnd > (double)BBR_MIN_PIPE * c->mss)
        c->cwnd = (double)BBR_MIN_PIPE * c->mss;
}

static void cc_init(nl_tcp *c)
{
    c->cwnd = (double)c->iw * c->mss;
    c->ssthresh = c->init_ssthresh > 0 ? (double)c->init_ssthresh * c->mss : 1e12;
    if (c->cc == CC_BBR) {
        bbr_enter_startup(c);
        nl_count(c->n, "tcp.bbr_startup", 1);
        c->pace_rate = BBR_HIGH_GAIN * c->cwnd * 1e6 / (double)(c->rtt_valid && c->srtt > 0 ? c->srtt : NL_MS(1));
    }
}

static void cubic_ack(nl_tcp *c, int acked)
{
    double segs = c->cwnd / c->mss, t, target, a = (double)acked / c->mss;

    if (!c->epoch) {
        c->epoch = nl_now();
        if (segs < c->w_max) {
            c->cub_k = cbrt((c->w_max - segs) / 0.4);
            c->cub_origin = c->w_max;
        } else {
            c->cub_k = 0;
            c->cub_origin = segs;
        }
        c->w_est = segs;
    }
    t = (double)(nl_now() - c->epoch + (c->rtt_valid ? c->srtt : 0)) / 1e6;
    target = c->cub_origin + 0.4 * (t - c->cub_k) * (t - c->cub_k) * (t - c->cub_k);
    c->w_est += 3.0 * 0.3 / 1.7 * a / segs;             /* what Reno would have */
    if (target < c->w_est)
        target = c->w_est;
    if (target > segs)
        segs += (target - segs) / segs * a;
    else
        segs += a / (100.0 * segs);
    c->cwnd = segs * c->mss;
}

/* New data acknowledged outside recovery (loss-based algorithms) */
static void cc_ack(nl_tcp *c, int acked)
{
    if (c->cwnd < c->ssthresh) {                        /* slow start, byte counting (RFC 3465, L = 2) */
        c->cwnd += acked < 2 * c->mss ? acked : 2 * c->mss;
        if (c->cwnd > c->ssthresh)
            c->cwnd = c->ssthresh;
    } else if (c->cc == CC_CUBIC) {
        cubic_ack(c, acked);
    } else {                                            /* congestion avoidance */
        c->cwnd += (double)c->mss * acked / c->cwnd;
    }
}

/* Multiplicative decrease: sets ssthresh */
static void cc_reduce(nl_tcp *c, double flight)
{
    if (c->cc == CC_BBR)
        return;                                         /* loss is not a congestion signal for bbr */
    if (c->cc == CC_CUBIC) {
        double segs = c->cwnd / c->mss;
        c->w_max = segs < c->w_max ? segs * 0.85 : segs;        /* fast convergence */
        c->epoch = 0;
        c->ssthresh = c->cwnd * 0.7;
    } else {
        c->ssthresh = (flight < c->cwnd ? flight : c->cwnd) / 2;
    }
    if (c->ssthresh < 2.0 * c->mss)
        c->ssthresh = 2.0 * c->mss;
}

static void cc_sample(nl_tcp *c)
{
    nl_sample(c->n, "cwnd (segments)", c->cwnd / c->mss);
    if (c->ssthresh < 1e11)
        nl_sample(c->n, "ssthresh", c->ssthresh / c->mss);
}

/* Loss detected by duplicate ACKs or SACK: enter fast recovery (Tahoe:
 * start again from one segment). flight = bytes outstanding before the
 * loss was noticed. */
static void enter_recovery(nl_tcp *c, int newly_acked, double flight)
{
    double before = c->cwnd;
    int i;

    c->fast_retx++;
    nl_count(c->n, "tcp.fast_retransmits", 1);
    cc_reduce(c, flight);
    c->recover = c->snd_max;
    if (c->cc == CC_TAHOE) {
        for (i = 0; i < c->sq_n; i++)
            if (!c->sq[i].sacked) {
                c->sq[i].lost = 1;
                c->sq[i].retx = 0;
            }
        c->cwnd = c->mss;
        c->dupacks = c->pseudo = 0;
    } else {
        if (c->cc == CC_BBR) {
            bbr_save_cwnd(c);
            c->cwnd = (double)pipe_bytes(c) + (newly_acked > c->mss ? newly_acked : c->mss);
            c->bbr_pc = 1;
            c->bbr_pc_end = c->delivered;
        } else {
            c->cwnd = c->ssthresh;
        }
        c->in_recovery = 1;
        c->force_retx = 1;
        c->partial_acks = 0;
    }
    nl_log(c->n, NL_L_TRANSPORT, 2, 0, T_WARN "TCP" T_RESET " :%u %s seq=%u (%s%s, cwnd %.1f → %.1f)", c->lport,
           c->sack_ok ? _("SACK shows a hole: fast retransmit") : _("3 duplicate ACKs: fast retransmit"),
           c->snd_una - c->iss, CC_NAME[c->cc], c->sack_ok ? "+sack" : "", before / c->mss, c->cwnd / c->mss);
    rto_arm(c);
    cc_sample(c);
}

static void rto_fire(void *ctx)
{
    nl_tcp *c = ctx;
    int i;

    c->rto_timer = 0;
    if (c->state == ST_CLOSED)
        return;
    if (c->state == ST_SYN_SENT || c->state == ST_SYN_RCVD) {
        if (++c->tries > 5) {
            nl_log(c->n, NL_L_TRANSPORT, 1, 0, T_ERR "TCP" T_RESET " %s:%u: %s", taddr_str(&c->rip), c->rport,
                   _("no answer to SYN: giving up"));
            conn_abort(c, NL_TCP_CLOSED_TIMEOUT);
            return;
        }
        c->backoff++;
        nl_count(c->n, "tcp.retransmits", 1);
        seg_xmit(c, c->iss, c->state == ST_SYN_SENT ? TH_SYN : TH_SYN | TH_ACK, NULL, 0);
        rto_arm(c);
        return;
    }
    if (c->snd_una == c->snd_max)
        return;
    if (++c->tries > 9) {
        nl_log(c->n, NL_L_TRANSPORT, 1, 0, T_ERR "TCP" T_RESET " %s:%u: %s", taddr_str(&c->rip), c->rport,
               _("too many retransmissions: connection dropped"));
        conn_abort(c, NL_TCP_CLOSED_TIMEOUT);
        return;
    }
    c->timeouts++;
    nl_count(c->n, "tcp.timeouts", 1);
    cc_reduce(c, (double)pipe_bytes(c));
    if (c->cc == CC_BBR)
        bbr_save_cwnd(c);
    nl_log(c->n, NL_L_TRANSPORT, 2, 0, T_ERR "TCP" T_RESET " :%u %s seq=%u (RTO %s, cwnd %.1f → 1)", c->lport,
           _("retransmission timeout"), c->snd_una - c->iss, nl_time_str(c->rto << c->backoff),
           c->cwnd / c->mss);
    /* everything not SACKed is presumed lost and goes out again, in order,
     * as the window reopens from one segment */
    for (i = 0; i < c->sq_n; i++)
        if (!c->sq[i].sacked) {
            c->sq[i].lost = 1;
            c->sq[i].retx = 0;
        }
    c->cwnd = c->mss;
    c->dupacks = c->pseudo = 0;
    c->in_recovery = 0;
    c->rto_recovery = 1;
    c->bbr_pc = 0;
    c->recover = c->snd_max;
    c->epoch = 0;
    if (c->backoff < 6)
        c->backoff++;
    cc_sample(c);
    rto_arm(c);
    tcp_output(c);
}

static void rtt_sample(nl_tcp *c, nl_time r)
{
    if (!c->rtt_valid) {
        c->srtt = r;
        c->rttvar = r / 2;
        c->rtt_valid = 1;
    } else {
        nl_time d = c->srtt > r ? c->srtt - r : r - c->srtt;
        c->rttvar = (3 * c->rttvar + d) / 4;
        c->srtt = (7 * c->srtt + r) / 8;
    }
    c->rto = c->srtt + (4 * c->rttvar > 1000 ? 4 * c->rttvar : 1000);
    if (c->rto < c->rto_min)
        c->rto = c->rto_min;
    if (c->rto > RTO_MAX)
        c->rto = RTO_MAX;
}

static void tick_fire(void *ctx)
{
    nl_tcp *c = ctx;
    tcp_node *tn = nl_mod_get(c->n, &tcp_mod);

    c->tick_timer = 0;
    if (c->state == ST_CLOSED || !tn || tn->sampler != c)
        return;
    cc_sample(c);
    if (c->rtt_valid)
        nl_sample(c->n, "rtt (ms)", c->srtt / 1000.0);
    nl_sample(c->n, "throughput (kbit/s)", (double)c->tick_acked * 8.0 / ((double)TICK / 1e6) / 1000.0);
    if (c->cc == CC_BBR) {
        nl_sample(c->n, "bbr btlbw (kbit/s)", bbr_btlbw(c) * 8.0 / 1000.0);
        nl_sample(c->n, "bbr rtprop (ms)", c->bbr_rtprop / 1000.0);
        nl_sample(c->n, "pacing (kbit/s)", c->pace_rate * 8.0 / 1000.0);
    }
    c->tick_acked = 0;
    if (c->slen > 0 || c->snd_una != c->snd_max)
        c->tick_timer = nl_after(c->n, TICK, tick_fire, c);
}

/* ------------------------------------------------------------------ */
/* Receiving                                                          */
/* ------------------------------------------------------------------ */

static void time_wait_done(void *ctx)
{
    nl_tcp *c = ctx;

    c->wait_timer = 0;
    conn_destroy(c);
}

static void enter_time_wait(nl_tcp *c)
{
    c->state = ST_TIME_WAIT;
    nl_cancel(&c->rto_timer);
    nl_cancel(&c->persist_timer);
    nl_log(c->n, NL_L_TRANSPORT, 2, 0, T_DATA "TCP" T_RESET " :%u ⇄ %s:%u %s", c->lport, taddr_str(&c->rip),
           c->rport, _("connection closed (TIME_WAIT)"));
    conn_gone(c, NL_TCP_CLOSED_FIN);
    if (c->state != ST_CLOSED && !c->wait_timer)
        c->wait_timer = nl_after(c->n, NL_SEC(1), time_wait_done, c);
}

static void drain_fire(void *ctx)
{
    nl_tcp *c = ctx;
    int n = (int)(c->read_rate / 8.0 * 0.05), win_before = rcv_window(c);

    c->drain_timer = 0;
    if (c->state == ST_CLOSED)
        return;
    if (n < 1) n = 1;
    if (n > c->rlen) n = c->rlen;
    if (n > 0) {
        const nl_tcp_ops *ops = c->ops;
        static uint8_t tmp[1 << 16];

        if (n > (int)sizeof(tmp))
            n = (int)sizeof(tmp);
        memcpy(tmp, c->rbuf, (size_t)n);
        memmove(c->rbuf, c->rbuf + n, (size_t)(c->rlen - n));
        c->rlen -= n;
        if (ops && ops->on_data)
            ops->on_data(c, c->ctx, tmp, n);
        if (c->state == ST_CLOSED)
            return;
        /* the window re-opened enough to matter: tell the sender */
        if ((c->last_adv < c->mss && rcv_window(c) >= c->mss) ||
            (win_before < c->rwnd_max / 2 && rcv_window(c) >= c->rwnd_max / 2 && c->last_adv < c->rwnd_max / 2)) {
            nl_log(c->n, NL_L_TRANSPORT, 2, 0, T_ACK "TCP" T_RESET " :%u %s (%d B)", c->lport,
                   _("window update"), rcv_window(c));
            send_ack(c);
        }
    }
    if (c->rlen > 0) {
        c->drain_timer = nl_after(c->n, NL_MS(50), drain_fire, c);
    } else if (c->eof_pending) {
        const nl_tcp_ops *ops = c->ops;
        c->eof_pending = 0;
        if (ops && ops->on_eof)
            ops->on_eof(c, c->ctx);
    }
}

/* In-order bytes for the application */
static void rx_deliver(nl_tcp *c, const uint8_t *d, int len)
{
    c->rcv_nxt += (uint32_t)len;
    c->bytes_rcvd += (uint64_t)len;
    nl_count(c->n, "tcp.bytes_received", len);
    if (c->rbuf) {
        memcpy(c->rbuf + c->rlen, d, (size_t)len);
        c->rlen += len;
        if (!c->drain_timer)
            c->drain_timer = nl_after(c->n, NL_MS(50), drain_fire, c);
    } else if (c->ops && c->ops->on_data) {
        c->ops->on_data(c, c->ctx, d, len);
    }
}

/* Data received twice: report it in the next ACK (D-SACK, RFC 2883) */
static void dsack(nl_tcp *c, uint32_t lo, uint32_t hi)
{
    if (c->sack_ok) {
        c->dsack_lo = lo;
        c->dsack_hi = hi;
        c->dsack_pending = 1;
    }
}

static void rx_data(nl_tcp *c, uint32_t seq, const uint8_t *d, int len)
{
    int win = rcv_window(c), i, again;

    if (SEQ_LT(seq, c->rcv_nxt)) {                      /* trim what we already have */
        int skip = (int)(c->rcv_nxt - seq);
        dsack(c, seq, skip >= len ? seq + (uint32_t)len : c->rcv_nxt);
        if (skip >= len)
            return;
        seq += (uint32_t)skip;
        d += skip;
        len -= skip;
    }
    if ((int)(seq - c->rcv_nxt) + len > win)            /* beyond the window we offered */
        len = win - (int)(seq - c->rcv_nxt);
    if (len <= 0)
        return;
    if (seq != c->rcv_nxt) {                            /* a hole before it: keep for later */
        for (i = 0; i < c->nooo; i++)
            if (c->ooo[i].seq == seq) {
                dsack(c, seq, seq + (uint32_t)len);
                return;
            }
        memmove(c->sack_recent + 1, c->sack_recent, 3 * sizeof(c->sack_recent[0]));
        c->sack_recent[0] = seq;
        if (c->nsack_recent < 4)
            c->nsack_recent++;
        if (c->nooo < OOO_MAX) {
            ooo_seg *s = &c->ooo[c->nooo];
            s->data = malloc((size_t)len);
            if (s->data) {
                memcpy(s->data, d, (size_t)len);
                s->seq = seq;
                s->len = len;
                c->nooo++;
                nl_count(c->n, "tcp.out_of_order", 1);
            }
        }
        return;
    }
    rx_deliver(c, d, len);
    do {                                                /* did it fill a hole? */
        again = 0;
        for (i = 0; i < c->nooo && c->state != ST_CLOSED; i++) {
            ooo_seg s = c->ooo[i];
            if (SEQ_GT(s.seq, c->rcv_nxt))
                continue;
            c->ooo[i] = c->ooo[--c->nooo];
            if (SEQ_GT(s.seq + (uint32_t)s.len, c->rcv_nxt)) {
                int skip = (int)(c->rcv_nxt - s.seq), take = s.len - skip;
                if (take > rcv_window(c))
                    take = rcv_window(c);
                if (take > 0)
                    rx_deliver(c, s.data + skip, take);
            }
            free(s.data);
            again = 1;
            break;
        }
    } while (again && c->state != ST_CLOSED);
}

static void parse_options(nl_tcp *c, const uint8_t *opt, int len)
{
    int i = 0;

    while (i < len) {
        int kind = opt[i], l;

        if (kind == 0)
            break;
        if (kind == 1) {
            i++;
            continue;
        }
        if (i + 1 >= len || (l = opt[i + 1]) < 2 || i + l > len)
            break;
        if (kind == 2 && l == 4) {
            int m = nl_get16(opt + i + 2);
            if (m >= 64 && m < c->mss)
                c->mss = m;
        } else if (kind == 3 && l == 3) {
            c->snd_wscale = opt[i + 2] > 14 ? 14 : opt[i + 2];
            c->ws_ok = 1;
        } else if (kind == 4 && l == 2) {
            c->sack_peer = 1;
        }
        i += l;
    }
}

/* SACK blocks of an ACK (absolute sequence numbers) into c->rx_blk */
static void parse_sack(nl_tcp *c, const uint8_t *opt, int len)
{
    int i = 0, k;

    c->rx_nblk = 0;
    while (i < len) {
        int kind = opt[i], l;

        if (kind == 0)
            break;
        if (kind == 1) {
            i++;
            continue;
        }
        if (i + 1 >= len || (l = opt[i + 1]) < 2 || i + l > len)
            break;
        if (kind == 5)
            for (k = 0; k + 8 <= l - 2 && c->rx_nblk < 4; k += 8) {
                c->rx_blk[2 * c->rx_nblk] = nl_get32(opt + i + 2 + k);
                c->rx_blk[2 * c->rx_nblk + 1] = nl_get32(opt + i + 6 + k);
                c->rx_nblk++;
            }
        i += l;
    }
}

static void established(nl_tcp *c)
{
    tcp_node *tn = nl_mod_get(c->n, &tcp_mod);

    c->state = ST_ESTABLISHED;
    c->backoff = c->tries = 0;
    nl_cancel(&c->rto_timer);
    if (!c->ws_ok)
        c->snd_wscale = c->rcv_wscale = 0;
    if (!c->ws_ok && c->rwnd_max > 65535)
        c->rwnd_max = 65535;
    cc_init(c);
    nl_count(c->n, "tcp.connections", 1);
    nl_log(c->n, NL_L_TRANSPORT, 2, 0, T_OK "TCP" T_RESET " :%u ⇄ %s:%u %s (MSS %d, %s%s%s)", c->lport,
           taddr_str(&c->rip), c->rport, _("connection established"), c->mss, CC_NAME[c->cc],
           c->sack_ok ? ", SACK" : "", c->ecn_ok ? ", ECN" : "");
    if (!c->active) {
        c->ops = c->lops;
        c->ctx = c->lops && c->lops->on_accept ? c->lops->on_accept(c, c->lctx) : c->lctx;
        if (c->state == ST_CLOSED)
            return;
    }
    (void)tn;
    if (c->ops && c->ops->on_connect)
        c->ops->on_connect(c, c->ctx);
}

/* A segment has been delivered (cumulatively acknowledged or SACKed):
 * account for it and let it contribute to the rate and RTT samples */
static void deliver(nl_tcp *c, const txseg *s, rsamp *rs, int rtt_ok)
{
    nl_time now = nl_now();

    c->delivered += (uint64_t)s->len;
    c->delivered_time = now;
    rs->newly_acked += s->len;
    if (!rs->have || s->delivered > rs->prior_delivered) {
        rs->have = 1;
        rs->prior_delivered = s->delivered;
        rs->app_limited = s->app_limited;
        rs->send_elapsed = s->xmit - s->first_tx;
        rs->ack_elapsed = now - s->delivered_time;
        c->first_tx_time = s->xmit;
    }
    if (!s->ever_retx) {                /* Karn: never time a retransmitted segment */
        if (rtt_ok && (rs->rtt < 0 || now - s->xmit < rs->rtt))
            rs->rtt = now - s->xmit;
        if (rs->rtt_min < 0 || now - s->xmit < rs->rtt_min)
            rs->rtt_min = now - s->xmit;
        if (s->xmit > rs->rack)
            rs->rack = s->xmit;
    }
}

/* RFC 6675 IsLost(): a hole is lost when enough data above it has been
 * SACKed (DupThresh = 3). Returns the bytes newly marked. */
static int sack_mark_lost(nl_tcp *c)
{
    int i, cnt = 0, newly = 0;
    long bytes = 0;

    for (i = c->sq_n - 1; i >= 0; i--) {
        txseg *s = &c->sq[i];
        if (s->sacked) {
            cnt++;
            bytes += s->len;
        } else if (!s->lost && (cnt >= 3 || bytes > 2L * c->mss)) {
            s->lost = 1;
            s->retx = 0;
            newly += s->len;
        }
    }
    return newly;
}

static void ack_input(nl_tcp *c, uint32_t ack, int flags, int len, uint32_t wnd, const uint32_t *blk, int nblk)
{
    int window_changed = wnd != c->snd_wnd, advanced = 0, data_acked = 0, removed = 0, new_sack = 0;
    int newly_lost = 0, is_dup, prior_inflight = pipe_bytes(c), ecn_cut = 0, i, b;
    rsamp rs;

    memset(&rs, 0, sizeof(rs));
    rs.rtt = rs.rtt_min = -1;
    if (SEQ_GT(ack, c->snd_max))
        return;
    if (wnd == 0 && c->snd_wnd != 0 && c->slen > 0) {
        nl_count(c->n, "tcp.zero_windows", 1);
        nl_log(c->n, NL_L_TRANSPORT, 2, 0, T_WARN "TCP" T_RESET " :%u %s", c->lport,
               _("the receiver advertises window 0: sender blocked by flow control"));
    }
    c->snd_wnd = wnd;
    if (wnd > 0)
        nl_cancel(&c->persist_timer);

    if (SEQ_GT(ack, c->snd_una)) {                      /* the cumulative ACK moves */
        advanced = 1;
        data_acked = (int)(ack - c->snd_una);
        while (removed < c->sq_n && SEQ_LEQ(c->sq[removed].seq + (uint32_t)c->sq[removed].len, ack)) {
            const txseg *s = &c->sq[removed];
            if (s->fin) {
                c->fin_acked = 1;
                data_acked--;
            }
            if (!s->sacked)
                deliver(c, s, &rs, removed == 0);
            removed++;
        }
        memmove(c->sq, c->sq + removed, (size_t)(c->sq_n - removed) * sizeof(c->sq[0]));
        c->sq_n -= removed;
        if (c->pseudo > 0 && removed > 1) {             /* already counted when their duplicate ACKs came */
            uint64_t twice = (uint64_t)(c->pseudo < removed - 1 ? c->pseudo : removed - 1) * (uint64_t)c->mss;
            c->delivered -= twice < c->delivered ? twice : 0;
            rs.newly_acked = rs.newly_acked > (int)twice ? rs.newly_acked - (int)twice : 0;
        }
        if (c->sq_n && SEQ_LT(c->sq[0].seq, ack)) {     /* the ACK falls inside a segment */
            int cut = (int)(ack - c->sq[0].seq);
            c->sq[0].seq = ack;
            c->sq[0].len -= cut;
            if (!c->sq[0].sacked) {
                c->delivered += (uint64_t)cut;
                rs.newly_acked += cut;
            }
        }
        if (data_acked > c->slen)
            data_acked = c->slen;
        if (data_acked > 0) {
            memmove(c->sbuf, c->sbuf + data_acked, (size_t)(c->slen - data_acked));
            c->slen -= data_acked;
        }
        c->snd_una = ack;
        nl_log(c->n, NL_L_TRANSPORT, 3, 0, T_ACK "TCP" T_RESET " :%u ← ack=%u (%d B %s, %d %s)", c->lport,
               ack - c->iss, data_acked, _("acknowledged"), c->sq_n, _("segments still in flight"));
        c->bytes_acked += (uint64_t)data_acked;
        c->tick_acked += (uint64_t)data_acked;
        nl_count(c->n, "tcp.bytes_acked", data_acked);
        c->backoff = c->tries = 0;
    }

    for (b = 0; c->sack_ok && b < nblk; b++) {          /* selective acknowledgements */
        uint32_t lo = blk[2 * b], hi = blk[2 * b + 1];

        if (!SEQ_GT(hi, lo))
            continue;
        /* D-SACK (RFC 2883): the first block reports data received twice */
        if (b == 0 && (SEQ_LEQ(hi, ack) || (nblk > 1 && SEQ_GEQ(lo, blk[2]) && SEQ_LEQ(hi, blk[3])))) {
            nl_count(c->n, "tcp.dsack_rcvd", 1);
            nl_log(c->n, NL_L_TRANSPORT, 3, 0, T_DATA "TCP" T_RESET " :%u D-SACK %u-%u: %s", c->lport,
                   lo - c->iss, hi - c->iss, _("the receiver got this twice (a retransmission was not needed)"));
            continue;
        }
        if (SEQ_GT(hi, c->snd_max) || SEQ_LEQ(hi, ack))
            continue;
        nl_count(c->n, "tcp.sack_blocks_rcvd", 1);
        for (i = 0; i < c->sq_n; i++) {
            txseg *s = &c->sq[i];
            if (!s->sacked && SEQ_GEQ(s->seq, lo) && SEQ_LEQ(s->seq + (uint32_t)s->len, hi)) {
                s->sacked = 1;
                deliver(c, s, &rs, 1);
                s->lost = s->retx = 0;
                new_sack = 1;
            }
        }
    }
    if (rs.rtt >= 0)
        rtt_sample(c, rs.rtt);

    /* a duplicate ACK says that a segment arrived while an earlier one is missing */
    is_dup = !advanced && c->snd_una != c->snd_max &&
             (c->sack_ok ? new_sack : (len == 0 && !(flags & (TH_SYN | TH_FIN)) && !window_changed));
    if (is_dup) {
        nl_count(c->n, "tcp.dup_acks", 1);
        c->dupacks++;
        /* some segment arrived, we cannot tell which; after a timeout the
         * duplicate ACKs answer our own duplicates and say nothing */
        if (!c->sack_ok && !c->rto_recovery) {
            c->pseudo++;
            c->delivered += (uint64_t)c->mss;
            c->delivered_time = nl_now();
            rs.newly_acked += c->mss;
        }
    }
    if (advanced) {
        c->dupacks = 0;
        /* segments that duplicate ACKs had announced are now acknowledged by
         * number; those sent later and already announced stay counted */
        if (c->pseudo > 0 && removed > 1)
            c->pseudo = c->pseudo > removed - 1 ? c->pseudo - (removed - 1) : 0;
        if (c->pseudo > c->sq_n)
            c->pseudo = c->sq_n;
        if ((c->in_recovery || c->rto_recovery) && SEQ_GEQ(ack, c->recover)) {
            if (c->in_recovery)
                nl_log(c->n, NL_L_TRANSPORT, 3, 0, T_OK "TCP" T_RESET " :%u %s", c->lport,
                       _("every hole is repaired: fast recovery ends"));
            c->in_recovery = c->rto_recovery = 0;
            c->bbr_pc = 0;
            if (c->cc == CC_BBR && c->cwnd < c->bbr_prior_cwnd)
                c->cwnd = c->bbr_prior_cwnd;
            cc_sample(c);
        }
    }

    /* which segments are lost? */
    if (c->sack_ok) {
        nl_time reo = c->rtt_valid ? c->srtt / 4 : NL_MS(1);

        newly_lost = sack_mark_lost(c);
        /* a retransmission is lost again when segments sent well after it
         * have been delivered (the idea of RACK, RFC 8985) */
        for (i = 0; rs.rack && i < c->sq_n; i++) {
            txseg *s = &c->sq[i];
            if (s->retx && !s->sacked && s->xmit + reo < rs.rack) {
                s->retx = 0;
                nl_count(c->n, "tcp.lost_retransmits", 1);
            }
        }
    } else if (c->sq_n) {
        txseg *s = &c->sq[0];

        if (is_dup && c->dupacks == 3 && !c->in_recovery && SEQ_GT(c->snd_una, c->recover) && !s->lost) {
            s->lost = 1;
            s->retx = 0;
            newly_lost = s->len;
        } else if (advanced && c->in_recovery) {        /* partial ACK */
            if (c->cc == CC_RENO) {
                c->in_recovery = 0;                     /* classic Reno: any new ACK ends recovery */
                c->pseudo = 0;
            } else if (!s->lost) {                      /* NewReno: the next hole */
                s->lost = 1;
                s->retx = 0;
                newly_lost = s->len;
                c->force_retx = 1;
                c->partial_acks++;
            }
        }
    }
    if (newly_lost && !c->in_recovery && (c->cc != CC_RENO || c->sack_ok || is_dup))
        enter_recovery(c, rs.newly_acked, (double)prior_inflight);

    if (advanced && (flags & TH_ECE) && c->ecn_ok && SEQ_GT(ack, c->ecn_recover) && !c->in_recovery) {
        cc_reduce(c, (double)prior_inflight);
        if (c->cc != CC_BBR)
            c->cwnd = c->ssthresh;
        c->ecn_recover = c->snd_max;
        c->cwr_pending = 1;
        ecn_cut = 1;
        nl_count(c->n, "tcp.ecn_reductions", 1);
        nl_log(c->n, NL_L_TRANSPORT, 2, 0, T_WARN "TCP" T_RESET " :%u %s (cwnd → %.1f)", c->lport,
               _("ECN echo: congestion signalled without loss"), c->cwnd / c->mss);
        cc_sample(c);
    }
    if (c->cc == CC_BBR) {
        if (rs.newly_acked > 0 || newly_lost > 0)
            bbr_on_ack(c, &rs, prior_inflight, newly_lost);
    } else if (advanced && data_acked > 0 && !c->in_recovery && !ecn_cut) {
        cc_ack(c, data_acked);
    }

    /* NewReno, "impatient" variant (RFC 6582): only the first partial ACK
     * restarts the timer, so that a recovery with many holes ends in a
     * timeout instead of dragging on for one round trip per hole */
    if (c->snd_una == c->snd_max)
        nl_cancel(&c->rto_timer);
    else if (advanced && (c->sack_ok || !c->in_recovery || c->partial_acks <= 1))
        rto_arm(c);
    if (data_acked > 0 && c->ops && c->ops->on_sent)
        c->ops->on_sent(c, c->ctx, data_acked);
}

static void tcp_input(nl_node *n, tcp_node *tn, const seg_in *m)
{
    const uint8_t *h = m->l4;
    int hl, len, flags, i, in_order = 0;
    uint16_t sport, dport;
    uint32_t seq, ack, wnd;
    nl_tcp *c;

    if (m->l4len < 20)
        return;
    hl = (h[12] >> 4) * 4;
    if (hl < 20 || hl > m->l4len)
        return;
    if ((m->src.v6 ? nl_l4_cksum6(&m->src.a6, &m->dst.a6, NL_PROTO_TCP, h, m->l4len)
                   : nl_l4_cksum(m->src.v4, m->dst.v4, NL_PROTO_TCP, h, m->l4len)) != 0) {
        nl_log(n, NL_L_TRANSPORT, 2, m->pkt_id, T_ERR "TCP" T_RESET " %s", _("wrong checksum: segment discarded"));
        nl_count(n, "tcp.bad_checksum", 1);
        return;
    }
    sport = nl_get16(h);
    dport = nl_get16(h + 2);
    seq = nl_get32(h + 4);
    ack = nl_get32(h + 8);
    flags = h[13];
    len = m->l4len - hl;
    c = conn_find(tn, m->dst, dport, m->src, sport);

    if (!c) {
        const listener *l = NULL;

        for (i = 0; i < tn->nlis; i++)
            if (tn->lis[i].port == dport)
                l = &tn->lis[i];
        if (flags & TH_RST)
            return;
        if (!l || (flags & (TH_SYN | TH_ACK)) != TH_SYN) {
            /* nobody here: reset */
            nl_count(n, "tcp.resets_sent", 1);
            if (!l && (flags & TH_SYN))
                nl_log(n, NL_L_TRANSPORT, 2, m->pkt_id, T_ERR "TCP" T_RESET " %s %u: RST", _("no listener on port"),
                       dport);
            if (flags & TH_ACK)
                raw_xmit(n, m->dst, m->src, dport, sport, ack, 0, TH_RST, 0, NULL, 0, NULL, 0, 0, 0);
            else
                raw_xmit(n, m->dst, m->src, dport, sport, 0,
                         seq + (uint32_t)len + ((flags & TH_SYN) ? 1 : 0) + ((flags & TH_FIN) ? 1 : 0),
                         TH_RST | TH_ACK, 0, NULL, 0, NULL, 0, 0, 0);
            return;
        }
        c = conn_new(tn, m->dst, dport, m->src, sport, l->opts);
        if (!c)
            return;
        c->lops = l->ops;
        c->lctx = l->ctx;
        c->state = ST_SYN_RCVD;
        c->irs = seq;
        c->rcv_nxt = seq + 1;
        c->snd_wnd = nl_get16(h + 14);
        parse_options(c, h + 20, hl - 20);
        c->sack_ok = c->sack_want && c->sack_peer;
        if (!c->ws_ok)
            c->rcv_wscale = 0;
        if ((flags & (TH_ECE | TH_CWR)) == (TH_ECE | TH_CWR) && c->ecn_want)
            c->ecn_ok = 1;
        c->snd_nxt = c->snd_max = c->iss + 1;
        nl_log(n, NL_L_TRANSPORT, 2, m->pkt_id, T_DATA "TCP" T_RESET " :%u ← %s:%u SYN → SYN+ACK", dport,
               taddr_str(&m->src), sport);
        seg_xmit(c, c->iss, TH_SYN | TH_ACK, NULL, 0);
        rto_arm(c);
        return;
    }

    if (flags & TH_RST) {
        int refused = c->state == ST_SYN_SENT;

        if (c->state == ST_TIME_WAIT) {
            conn_destroy(c);
            return;
        }
        nl_count(n, "tcp.resets", 1);
        nl_log(n, NL_L_TRANSPORT, refused ? 1 : 2, m->pkt_id, T_ERR "TCP" T_RESET " %s:%u: %s", taddr_str(&c->rip),
               c->rport, refused ? _("connection refused (RST)") : _("connection reset by peer"));
        conn_abort(c, refused ? NL_TCP_CLOSED_REFUSED : NL_TCP_CLOSED_RST);
        return;
    }

    if (c->state == ST_SYN_SENT) {
        if ((flags & (TH_SYN | TH_ACK)) != (TH_SYN | TH_ACK) || ack != c->iss + 1)
            return;
        c->irs = seq;
        c->rcv_nxt = seq + 1;
        c->snd_una = ack;
        c->snd_wnd = nl_get16(h + 14);
        parse_options(c, h + 20, hl - 20);
        c->sack_ok = c->sack_want && c->sack_peer;
        if ((flags & TH_ECE) && c->ecn_want)
            c->ecn_ok = 1;
        if (c->tries == 0)
            rtt_sample(c, nl_now() - c->rtt_time);
        established(c);
        if (c->state == ST_CLOSED)
            return;
        send_ack(c);
        tcp_output(c);
        return;
    }
    if (c->state == ST_SYN_RCVD) {
        if (flags & TH_SYN) {                           /* our SYN+ACK was lost */
            seg_xmit(c, c->iss, TH_SYN | TH_ACK, NULL, 0);
            return;
        }
        if (!(flags & TH_ACK) || ack != c->iss + 1)
            return;
        c->snd_una = ack;
        c->snd_wnd = (uint32_t)nl_get16(h + 14) << c->snd_wscale;
        established(c);
        if (c->state == ST_CLOSED)
            return;
    } else if (flags & TH_SYN) {
        send_ack(c);                                    /* duplicate SYN+ACK: our ACK was lost */
        return;
    }

    wnd = (uint32_t)nl_get16(h + 14) << c->snd_wscale;
    if (flags & TH_CWR)
        c->ece_latch = 0;
    if (c->ecn_ok && (m->tos & 3) == 3 && len > 0) {
        if (!c->ece_latch)
            nl_log(n, NL_L_TRANSPORT, 2, m->pkt_id, T_WARN "TCP" T_RESET " :%u %s", c->lport,
                   _("packet marked CE by a router: echoing ECE"));
        c->ece_latch = 1;
        nl_count(n, "tcp.ce_received", 1);
    }
    if (flags & TH_ACK) {
        parse_sack(c, h + 20, hl - 20);
        ack_input(c, ack, flags, len, wnd, c->rx_blk, c->rx_nblk);
        if (c->state == ST_CLOSED)
            return;
        if (c->fin_acked) {
            if (c->state == ST_FIN_WAIT_1) {
                c->state = ST_FIN_WAIT_2;
            } else if (c->state == ST_CLOSING) {
                enter_time_wait(c);
                return;
            } else if (c->state == ST_LAST_ACK) {
                nl_log(n, NL_L_TRANSPORT, 2, 0, T_DATA "TCP" T_RESET " :%u ⇄ %s:%u %s", c->lport,
                       taddr_str(&c->rip), c->rport, _("connection closed"));
                conn_abort(c, NL_TCP_CLOSED_FIN);
                return;
            }
        }
    }
    if (c->state == ST_TIME_WAIT) {
        if (flags & TH_FIN)
            send_ack(c);
        return;
    }

    if (len > 0 && c->state != ST_CLOSE_WAIT && c->state != ST_LAST_ACK && c->state != ST_CLOSING) {
        uint32_t before = c->rcv_nxt;
        int holes = c->nooo;

        rx_data(c, seq, h + hl, len);
        if (c->state == ST_CLOSED)
            return;
        /* plain in-order data, nothing missing before or after: its ACK may wait */
        in_order = seq == before && c->rcv_nxt != before && !holes && !c->nooo && !c->dsack_pending;
        if (c->rcv_nxt == before && seq != before && SEQ_GT(seq, before))
            nl_log(n, NL_L_TRANSPORT, 3, m->pkt_id, T_ACK "TCP" T_RESET " :%u %s seq=%u, %s %u → dup ACK",
                   c->lport, _("out of order"), seq - c->irs, _("expected"), before - c->irs);
    }
    if ((flags & TH_FIN) && seq + (uint32_t)len == c->rcv_nxt &&
        (c->state == ST_ESTABLISHED || c->state == ST_FIN_WAIT_1 || c->state == ST_FIN_WAIT_2)) {
        c->rcv_nxt++;
        nl_log(n, NL_L_TRANSPORT, 2, m->pkt_id, T_DATA "TCP" T_RESET " :%u ← %s:%u FIN", c->lport,
               taddr_str(&c->rip), c->rport);
        send_ack(c);
        if (c->state == ST_ESTABLISHED) {
            c->state = ST_CLOSE_WAIT;
        } else if (c->state == ST_FIN_WAIT_1) {
            c->state = ST_CLOSING;
        } else {
            enter_time_wait(c);
            return;
        }
        if (c->rlen > 0)
            c->eof_pending = 1;
        else if (c->ops && c->ops->on_eof)
            c->ops->on_eof(c, c->ctx);
        if (c->state == ST_CLOSED)
            return;
    } else if (len > 0 || (flags & TH_FIN)) {
        /* delayed ACK (RFC 1122, RFC 5681): at least every second segment,
         * and at once for anything out of the ordinary */
        if ((flags & TH_FIN) || !in_order || c->delack <= 0 || (c->ecn_ok && (m->tos & 3) == 3) ||
            rcv_window(c) < 2 * c->mss || ++c->ack_pending >= 2)
            send_ack(c);
        else if (!c->delack_timer)
            c->delack_timer = nl_after(c->n, c->delack, delack_fire, c);
    }
    tcp_output(c);
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

static void tcp_input4(nl_node *n, void *ctx, const nl_ipmeta *m)
{
    seg_in in;

    in.src = A4(m->src);
    in.dst = A4(m->dst);
    in.tos = m->tos;
    in.pkt_id = m->pkt_id;
    in.l4 = m->l4;
    in.l4len = m->l4len;
    tcp_input(n, ctx, &in);
}

static void tcp_input6(nl_node *n, void *ctx, const nl_ip6meta *m)
{
    seg_in in;

    in.src = A6(&m->src);
    in.dst = A6(&m->dst);
    in.tos = m->tclass;
    in.pkt_id = m->pkt_id;
    in.l4 = m->l4;
    in.l4len = m->l4len;
    tcp_input(n, ctx, &in);
}

/* ------------------------------------------------------------------ */
/* Path MTU discovery (RFC 1191, RFC 8201)                            */
/* ------------------------------------------------------------------ */

/* A router on the path could not forward a segment of ours (IPv4 with DF,
 * or any IPv6 packet: IPv6 routers never fragment) and reported the MTU of
 * its next hop. Lower the MSS, cut the segments in flight that no longer
 * fit and send them again at once: they were discarded, not lost to
 * congestion, so the window is left alone. */
static void pmtu_shrink(nl_tcp *c, int mss, nl_node *n, uint32_t pkt_id, int mtu)
{
    int i;

    if (mss < 64)
        mss = 64;
    if (mss >= c->mss || c->state == ST_CLOSED)
        return;
    nl_log(n, NL_L_TRANSPORT, 1, pkt_id, T_WARN "TCP" T_RESET " %s:%u: %s %d → MSS %d → %d", taddr_str(&c->rip),
           c->rport, _("path MTU is"), mtu, c->mss, mss);
    nl_count(n, "tcp.pmtu_reductions", 1);
    nl_count_set(n, "tcp.mss", mss);
    c->mss = mss;
    for (i = 0; i < c->sq_n; i++) {
        txseg rest;

        if (c->sq[i].fin || c->sq[i].sacked || c->sq[i].len <= mss)
            continue;
        rest = c->sq[i];
        rest.seq += (uint32_t)mss;
        rest.len -= mss;
        if (!sq_push(c, 0, 0))
            break;
        memmove(&c->sq[i + 2], &c->sq[i + 1], (size_t)(c->sq_n - i - 2) * sizeof(c->sq[0]));
        c->sq[i + 1] = rest;
        c->sq[i].len = mss;
        c->sq[i].lost = c->sq[i + 1].lost = 1;
        c->sq[i].retx = c->sq[i + 1].retx = 0;
    }
    c->force_retx = 1;
    tcp_output(c);
}

static void tcp_icmp4(nl_node *n, void *ctx, const nl_icmpmeta *m)
{
    tcp_node *tn = ctx;
    nl_tcp *c;

    if (m->type != NL_ICMP_UNREACH || m->code != 4 || m->orig_proto != NL_PROTO_TCP || m->mtu < 68)
        return;
    c = conn_find(tn, A4(m->orig_src), m->orig_sport, A4(m->orig_dst), m->orig_dport);
    if (c && c->pmtud)
        pmtu_shrink(c, m->mtu - 40, n, m->pkt_id, m->mtu);
}

static void tcp_icmp6(nl_node *n, void *ctx, const nl_icmp6meta *m)
{
    tcp_node *tn = ctx;
    nl_tcp *c;

    if (m->type != NL_ICMP6_TOO_BIG || m->orig_proto != NL_PROTO_TCP || m->mtu < 1280)
        return;
    c = conn_find(tn, A6(&m->orig_src), m->orig_sport, A6(&m->orig_dst), m->orig_dport);
    if (c)
        pmtu_shrink(c, m->mtu - 60, n, m->pkt_id, m->mtu);
}

/* ------------------------------------------------------------------ */
/* The nl_tcp_* API                                                   */
/* ------------------------------------------------------------------ */

static nl_tcp *tcp_open(nl_node *n, taddr lip, taddr dst, uint16_t dport, const nl_tcp_ops *ops, void *ctx,
                        const char *opts)
{
    tcp_node *tn = tcp_get(n);
    nl_tcp *c;
    uint16_t lport;

    if (!tn)
        return NULL;
    do {
        lport = tn->eph++;
        if (tn->eph < 49152)
            tn->eph = 49152;
    } while (conn_find(tn, lip, lport, dst, dport));
    c = conn_new(tn, lip, lport, dst, dport, opts);
    if (!c)
        return NULL;
    c->active = 1;
    c->ops = ops;
    c->ctx = ctx;
    c->state = ST_SYN_SENT;
    c->snd_nxt = c->snd_max = c->iss + 1;
    c->rtt_time = nl_now();
    nl_log(n, NL_L_TRANSPORT, 2, 0, T_DATA "TCP" T_RESET " :%u → %s:%u SYN (%s)", lport, taddr_str(&dst), dport,
           _("opening a connection"));
    seg_xmit(c, c->iss, TH_SYN, NULL, 0);
    rto_arm(c);
    return c;
}

nl_tcp *nl_tcp_connect(nl_node *n, nl_ip dst, uint16_t dport, const nl_tcp_ops *ops, void *ctx, const char *opts)
{
    const nl_route *r;
    nl_ip lip = dst;

    if (!n->up)
        return NULL;
    if (!nl_ip_is_local(n, dst)) {
        r = nl_route_lookup(n, dst);
        if (!r) {
            nl_log(n, NL_L_TRANSPORT, 1, 0, T_ERR "TCP" T_RESET " %s %s", _("no route to"), nl_ip_str(dst));
            return NULL;
        }
        lip = n->ifc[r->ifx].ip;
    }
    return tcp_open(n, A4(lip), A4(dst), dport, ops, ctx, opts);
}

nl_tcp *nl_tcp_connect6(nl_node *n, const nl_ip6 *dst, uint16_t dport, const nl_tcp_ops *ops, void *ctx,
                        const char *opts)
{
    nl_ip6 lip;

    if (!n->up)
        return NULL;
    if (!nl_ip6_src(n, dst, &lip)) {
        nl_log(n, NL_L_TRANSPORT, 1, 0, T_ERR "TCP" T_RESET " %s %s", _("no route to"), nl_ip6_str(dst));
        return NULL;
    }
    return tcp_open(n, A6(&lip), A6(dst), dport, ops, ctx, opts);
}

int nl_tcp_listen(nl_node *n, uint16_t port, const nl_tcp_ops *ops, void *listen_ctx, const char *opts)
{
    tcp_node *tn = tcp_get(n);
    listener *l = NULL;
    int i;

    if (!tn)
        return -1;
    for (i = 0; i < tn->nlis; i++)
        if (tn->lis[i].port == port)
            l = &tn->lis[i];
    if (!l) {
        if (tn->nlis >= MAX_LIS)
            return -1;
        l = &tn->lis[tn->nlis++];
    }
    l->port = port;
    l->ops = ops;
    l->ctx = listen_ctx;
    snprintf(l->opts, sizeof(l->opts), "%s", opts ? opts : "");
    return 0;
}

void nl_tcp_unlisten(nl_node *n, uint16_t port)
{
    tcp_node *tn = nl_mod_get(n, &tcp_mod);
    int i;

    for (i = 0; tn && i < tn->nlis; i++)
        if (tn->lis[i].port == port) {
            tn->lis[i] = tn->lis[--tn->nlis];
            return;
        }
}

int nl_tcp_writable(const nl_tcp *c)
{
    if (c->state == ST_CLOSED || c->fin_pending || !c->sbuf)
        return 0;
    return c->scap - c->slen;
}

int nl_tcp_write(nl_tcp *c, const void *data, int len)
{
    tcp_node *tn;
    int room = nl_tcp_writable(c);

    if (len > room)
        len = room;
    if (len <= 0)
        return 0;
    memcpy(c->sbuf + c->slen, data, (size_t)len);
    c->slen += len;
    tn = nl_mod_get(c->n, &tcp_mod);
    if (tn && tn->sampler != c && c->slen >= c->mss) {
        tn->sampler = c;
        c->tick_acked = 0;
    }
    if (tn && tn->sampler == c && !c->tick_timer)
        c->tick_timer = nl_after(c->n, TICK, tick_fire, c);
    tcp_output(c);
    return len;
}

void nl_tcp_close(nl_tcp *c)
{
    if (c->state == ST_CLOSED || c->fin_pending)
        return;
    if (c->state == ST_SYN_SENT || c->state == ST_SYN_RCVD) {
        c->ops = NULL;
        conn_destroy(c);
        return;
    }
    c->fin_pending = 1;
    tcp_output(c);
}

void nl_tcp_abort(nl_tcp *c)
{
    if (c->state == ST_CLOSED)
        return;
    if (c->state >= ST_SYN_RCVD && c->state != ST_TIME_WAIT)
        raw_xmit(c->n, c->lip, c->rip, c->lport, c->rport, c->snd_nxt, c->rcv_nxt, TH_RST | TH_ACK, 0,
                 NULL, 0, NULL, 0, 0, 0);
    c->ops = NULL;
    conn_destroy(c);
}

int nl_tcp_family(const nl_tcp *c) { return c->rip.v6 ? 6 : 4; }

void nl_tcp_peer6(const nl_tcp *c, nl_ip6 *ip, uint16_t *port)
{
    if (ip) *ip = c->rip.a6;
    if (port) *port = c->rport;
}

void nl_tcp_local6(const nl_tcp *c, nl_ip6 *ip, uint16_t *port)
{
    if (ip) *ip = c->lip.a6;
    if (port) *port = c->lport;
}

void nl_tcp_peer(const nl_tcp *c, nl_ip *ip, uint16_t *port)
{
    if (ip) *ip = c->rip.v4;
    if (port) *port = c->rport;
}

void nl_tcp_local(const nl_tcp *c, nl_ip *ip, uint16_t *port)
{
    if (ip) *ip = c->lip.v4;
    if (port) *port = c->lport;
}

nl_node *nl_tcp_node(const nl_tcp *c) { return c->n; }

/* ------------------------------------------------------------------ */
/* The `tcp` tool: bulk sender and sink                               */
/* ------------------------------------------------------------------ */

typedef struct {
    nl_node *n;
    nl_tcp  *c;
    long long total, queued;            /* total < 0: send until the deadline */
    nl_time  t0, t_conn, duration, warmup;
    nl_time  t_mark;                    /* start of the steady-state measurement */
    uint64_t acked_mark;
    nl_ev    deadline, start;
    taddr    dst;
    int      port;
    char     dest[48], cc[12], opts[200];
} xfer;
typedef struct { nl_node *n; unsigned long long bytes; nl_time t0; } sink_conn;

/* Steady-state rates of the transfers that ended, for Jain's fairness index */
static struct { nl_node *n; int port; double kbps; } g_flows[32];
static int g_nflows;

static void fairness(nl_node *n, int port, double kbps)
{
    double sum = 0, sq = 0, j;
    int i, k = 0;

    if (g_nflows < (int)(sizeof(g_flows) / sizeof(g_flows[0]))) {
        g_flows[g_nflows].n = n;
        g_flows[g_nflows].port = port;
        g_flows[g_nflows].kbps = kbps;
        g_nflows++;
    }
    for (i = 0; i < g_nflows; i++)
        if (g_flows[i].port == port) {
            sum += g_flows[i].kbps;
            sq += g_flows[i].kbps * g_flows[i].kbps;
            k++;
        }
    if (k < 2 || sq <= 0)
        return;
    j = sum * sum / (k * sq);                           /* 1 = equal shares, 1/k = one flow has it all */
    for (i = 0; i < g_nflows; i++)
        if (g_flows[i].port == port) {
            nl_count_set(g_flows[i].n, "tcp.jain_pct", j * 100.0);
            nl_count_set(g_flows[i].n, "tcp.share_pct", sum > 0 ? g_flows[i].kbps * 100.0 / sum : 0);
        }
    nl_log(n, NL_L_APP, 1, 0, T_INFO "tcp:" T_RESET " %s %d %s = " T_BOLD "%.3f" T_RESET " (%s)", _("Jain's fairness index over"),
           k, _("flows"), j, _("1 = equal shares"));
}

static void xfer_fill(nl_tcp *c, xfer *x)
{
    static uint8_t pat[8192];
    int i, w;

    if (!pat[1])
        for (i = 0; i < (int)sizeof(pat); i++)
            pat[i] = (uint8_t)('A' + i % 26);
    /* the first part of a transfer (handshake, slow start) is left out of the steady-state rate */
    if (!x->t_mark && (x->total < 0 ? nl_now() - x->t_conn >= x->warmup
                                    : (long long)c->bytes_acked * 4 >= x->total)) {
        x->t_mark = nl_now();
        x->acked_mark = c->bytes_acked;
    }
    while (x->total < 0 || x->queued < x->total) {
        long long left = x->total < 0 ? (long long)sizeof(pat) : x->total - x->queued;
        w = nl_tcp_write(c, pat, left < (long long)sizeof(pat) ? (int)left : (int)sizeof(pat));
        if (w <= 0)
            break;
        x->queued += w;
    }
    if (x->total >= 0 && x->queued >= x->total)
        nl_tcp_close(c);
}

static void xfer_deadline(void *ctx)
{
    xfer *x = ctx;

    tcp_node *tn = nl_mod_get(x->n, &tcp_mod);
    nl_tcp *c;

    x->deadline = 0;
    for (c = tn ? tn->conns : NULL; c && c != x->c; c = c->next)
        ;
    if (!c)
        return;                                         /* the connection died with a crash */
    x->total = x->queued;                               /* what is queued is still delivered */
    nl_tcp_close(c);
}

static void xfer_connect(nl_tcp *c, void *ctx)
{
    xfer *x = ctx;

    x->t_conn = nl_now();
    if (x->total < 0) {
        nl_log(x->n, NL_L_APP, 1, 0, T_OK "tcp send %s:" T_RESET " %s (%s), %s %s", x->dest, _("connected"),
               nl_time_str(x->t_conn - x->t0), _("sending for"), nl_time_str(x->duration));
        x->deadline = nl_after(x->n, x->duration, xfer_deadline, x);
    } else {
        nl_log(x->n, NL_L_APP, 1, 0, T_OK "tcp send %s:" T_RESET " %s (%s), %lld B", x->dest,
               _("connected"), nl_time_str(x->t_conn - x->t0), x->total);
    }
    xfer_fill(c, x);
}

static void xfer_sent(nl_tcp *c, void *ctx, int acked)
{
    (void)acked;
    xfer_fill(c, ctx);
}

static void xfer_close(nl_tcp *c, void *ctx, int reason)
{
    xfer *x = ctx;
    double secs = (double)(nl_now() - x->t0) / 1e6;

    nl_cancel(&x->deadline);
    if (reason == NL_TCP_CLOSED_FIN && x->total >= 0 && c->bytes_acked >= (uint64_t)x->total) {
        double kbps = secs > 0 ? (double)x->total * 8.0 / secs / 1000.0 : 0, steady = kbps;

        if (x->t_mark && nl_now() > x->t_mark)
            steady = (double)(c->bytes_acked - x->acked_mark) * 8.0 / ((double)(nl_now() - x->t_mark) / 1e6) / 1000.0;
        nl_log(x->n, NL_L_APP, 1, 0, T_OK "tcp send %s: %s" T_RESET " %lld B, %.3f s, %s, %s %s  " T_DIM
               "(%s%s, %u %s, %u fast, %u %s)", x->dest, _("done"), x->total, secs, nl_rate_str(kbps * 1000.0),
               _("steady"), nl_rate_str(steady * 1000.0), x->cc, c->sack_ok ? "+sack" : "", c->retx,
               _("retransmissions"), c->fast_retx, c->timeouts, _("timeouts"));
        nl_count(x->n, "tcp.send_done", 1);
        nl_count_set(x->n, "tcp.goodput_kbps", kbps);
        nl_count_set(x->n, "tcp.steady_kbps", steady);
        nl_count_set(x->n, "tcp.time_ms", secs * 1000.0);
        fairness(x->n, x->port, steady);
    } else {
        nl_log(x->n, NL_L_APP, 1, 0, T_ERR "tcp send %s: %s" T_RESET " (%s), %llu / %lld B", x->dest, _("failed"),
               reason == NL_TCP_CLOSED_REFUSED ? _("connection refused")
               : reason == NL_TCP_CLOSED_TIMEOUT ? _("timeout") : _("connection reset"),
               (unsigned long long)c->bytes_acked, x->total < 0 ? x->queued : x->total);
        nl_count(x->n, "tcp.send_failed", 1);
    }
    free(x);
}

static const nl_tcp_ops xfer_ops = {
    .on_connect = xfer_connect, .on_sent = xfer_sent, .on_close = xfer_close,
};

static void xfer_start(void *ctx)
{
    xfer *x = ctx;

    x->start = 0;
    x->t0 = nl_now();
    if (x->total < 0)
        nl_log(x->n, NL_L_APP, 1, 0, T_INFO "tcp send %s" T_RESET " (%s:%d): %s, %s", x->dest, taddr_str(&x->dst),
               x->port, nl_time_str(x->duration), x->cc);
    else
        nl_log(x->n, NL_L_APP, 1, 0, T_INFO "tcp send %s" T_RESET " (%s:%d): %lld B, %s", x->dest,
               taddr_str(&x->dst), x->port, x->total, x->cc);
    x->c = x->dst.v6 ? nl_tcp_connect6(x->n, &x->dst.a6, (uint16_t)x->port, &xfer_ops, x, x->opts)
                     : nl_tcp_connect(x->n, x->dst.v4, (uint16_t)x->port, &xfer_ops, x, x->opts);
    if (!x->c) {
        nl_count(x->n, "tcp.send_failed", 1);
        free(x);
    }
}

static void *sink_accept(nl_tcp *c, void *lctx)
{
    sink_conn *s = calloc(1, sizeof(*s));

    (void)lctx;
    if (!s) {
        nl_tcp_abort(c);
        return NULL;
    }
    s->n = nl_tcp_node(c);
    s->t0 = nl_now();
    return s;
}

static void sink_data(nl_tcp *c, void *ctx, const uint8_t *d, int len)
{
    sink_conn *s = ctx;
    tcp_node *tn = nl_mod_get(c->n, &tcp_mod);

    (void)d;
    if (!s)
        return;
    s->bytes += (unsigned long long)len;
    if (tn)
        tn->sink_bytes += (uint64_t)len;
    nl_count(c->n, "tcp.sink_bytes", len);
}

static void sink_eof(nl_tcp *c, void *ctx)
{
    sink_conn *s = ctx;
    double secs = s ? (double)(nl_now() - s->t0) / 1e6 : 0;

    if (s) {
        nl_log(c->n, NL_L_APP, 1, 0, T_OK "tcp sink:" T_RESET " %llu B %s %s:%u, %.3f s, %s", s->bytes, _("from"),
               taddr_str(&c->rip), c->rport, secs, nl_rate_str(secs > 0 ? (double)s->bytes * 8.0 / secs : 0));
        nl_count(c->n, "tcp.sink_done", 1);
    }
    nl_tcp_close(c);
}

static void sink_close(nl_tcp *c, void *ctx, int reason)
{
    (void)c;
    (void)reason;
    free(ctx);
}

static const nl_tcp_ops sink_ops = {
    .on_accept = sink_accept, .on_data = sink_data, .on_eof = sink_eof, .on_close = sink_close,
};

/* Everything after the first two words, as an option string */
static void join_opts(const nl_args *a, int skip, char *out, size_t n)
{
    size_t len = 0;
    int i, pos = 0;

    out[0] = '\0';
    for (i = 0; i < a->argc; i++) {
        if (!strchr(a->argv[i], '=')) {
            if (pos++ < skip)
                continue;
        }
        len += (size_t)snprintf(out + len, n - len, "%s%s", len ? " " : "", a->argv[i]);
        if (len >= n)
            break;
    }
}

static int tcp_run(nl_node *n, const nl_args *a)
{
    tcp_node *tn = tcp_get(n);
    const char *what = nl_arg_pos(a, 0);
    char opts[200];

    if (!tn)
        return -1;
    if (!what)
        return 0;                                       /* just make the stack visible */
    if (strcmp(what, "sink") == 0) {
        join_opts(a, 1, opts, sizeof(opts));
        tn->sink_port = (int)nl_arg_int(a, "port", 5001);
        snprintf(tn->sink_opts, sizeof(tn->sink_opts), "%s", opts);
        nl_tcp_listen(n, (uint16_t)tn->sink_port, &sink_ops, NULL, opts);
        nl_log(n, NL_L_APP, 2, 0, T_INFO "tcp sink" T_RESET " %s %d", _("listening on TCP port"), tn->sink_port);
        return 0;
    }
    if (strcmp(what, "send") == 0) {
        const char *target = nl_arg_pos(a, 1), *b = nl_arg_str(a, "bytes", "1M");
        double total = 0;
        nl_time jitter;
        taddr dst;
        nl_ip6 d6;
        xfer *x;

        /* IPv6 for an IPv6 literal, for v6=1, or when the name only has an IPv6 address */
        memset(&dst, 0, sizeof(dst));
        if (target && (nl_parse_ip6(target, &d6) || (nl_arg_int(a, "v6", 0) && nl_resolve6(target, &d6)) ||
                       (!nl_arg_int(a, "v6", 0) && !nl_resolve(target, &dst.v4) && nl_ip6_enabled() &&
                        nl_resolve6(target, &d6))))
            dst = A6(&d6);
        if (!target || (!dst.v6 && (nl_arg_int(a, "v6", 0) || !nl_resolve(target, &dst.v4)))) {
            nl_log(n, NL_L_APP, 1, 0, T_ERR "tcp send: %s '%s'", _("unknown destination"), target ? target : "");
            return -1;
        }
        if (!nl_parse_rate(b, &total) || total < 1)
            total = 1e6;
        x = calloc(1, sizeof(*x));
        if (!x)
            return -1;
        x->n = n;
        x->total = (long long)total;
        x->duration = nl_arg_time(a, "duration", 0);
        if (x->duration > 0) {                          /* send for a time instead of a size */
            x->total = -1;
            x->warmup = nl_arg_time(a, "warmup", x->duration / 4);
        }
        x->dst = dst;
        x->port = (int)nl_arg_int(a, "port", 5001);
        snprintf(x->dest, sizeof(x->dest), "%s", target);
        snprintf(x->cc, sizeof(x->cc), "%s", nl_arg_str(a, "cc", "reno"));
        join_opts(a, 2, x->opts, sizeof(x->opts));
        jitter = nl_arg_time(a, "jitter", 0);
        /* a random start offset: two flows started "together" do not stay in lockstep */
        x->start = nl_after(n, jitter > 0 ? (nl_time)(nl_rand01() * (double)jitter) : 0, xfer_start, x);
        return 0;
    }
    nl_log(n, NL_L_APP, 1, 0, T_ERR "tcp: %s '%s' (sink, send)", _("unknown command"), what);
    return -1;
}

/* Drop every connection without a word on the wire (crash, stop) */
static void tcp_drop_all(tcp_node *tn, int with_listeners)
{
    while (tn->conns) {
        nl_tcp *c = tn->conns;
        c->ops = NULL;
        conn_destroy(c);
    }
    if (with_listeners)
        tn->nlis = 0;
}

static void tcp_stop(nl_node *n)
{
    tcp_node *tn = nl_mod_get(n, &tcp_mod);

    if (!tn)
        return;
    /* the stack stays (other modules may hold listeners): only the tool goes */
    if (tn->sink_port) {
        nl_tcp_unlisten(n, (uint16_t)tn->sink_port);
        tn->sink_port = 0;
    }
}

static void tcp_crash(nl_node *n)
{
    tcp_node *tn = nl_mod_get(n, &tcp_mod);

    if (tn)
        tcp_drop_all(tn, 1);
}

static void tcp_recover(nl_node *n)
{
    tcp_node *tn = nl_mod_get(n, &tcp_mod);

    if (tn && tn->sink_port)
        nl_tcp_listen(n, (uint16_t)tn->sink_port, &sink_ops, NULL, tn->sink_opts);
}

static int tcp_panel(nl_node *n, int x, int y, int w, int h, int detail)
{
    tcp_node *tn = nl_mod_get(n, &tcp_mod);
    nl_tcp *c;
    int row = 0, i;

    for (i = 0; i < tn->nlis && row < h; i++)
        tui_print(x, y + row++, w, TS(C_TEXT, C_DEFAULT, 0), T_DIM "LISTEN" T_RESET " :%u", tn->lis[i].port);
    for (c = tn->conns; c && row < h; c = c->next) {
        tui_print(x, y + row++, w, TS(C_TEXT, C_DEFAULT, 0), "%s:%u → %s:%u  %s%s",
                  T_DATA, c->lport, taddr_str(&c->rip), c->rport,
                  c->state == ST_ESTABLISHED ? T_OK : T_WARN, ST_NAME[c->state]);
        if (row < h)
            tui_print(x + 2, y + row++, w - 2, TS(C_TEXT, C_DEFAULT, 0),
                      T_DIM "%s%s" T_RESET " cwnd %.1f  ssthresh %s%.1f" T_RESET "  %s",
                      CC_NAME[c->cc], c->sack_ok ? "+sack" : "", c->cwnd / c->mss, c->ssthresh < 1e11 ? "" : T_DIM,
                      c->ssthresh < 1e11 ? c->ssthresh / c->mss : 0.0,
                      c->in_recovery ? T_WARN "fast recovery" : c->rto_recovery ? T_ERR "after timeout"
                      : c->cc == CC_BBR ? BBR_NAME[c->bbr_state] : c->cwnd < c->ssthresh ? "slow start"
                      : "congestion avoidance");
        if (c->cc == CC_BBR && row < h)
            tui_print(x + 2, y + row++, w - 2, TS(C_TEXT, C_DEFAULT, 0),
                      T_DIM "BtlBw" T_RESET " %s  " T_DIM "RTprop" T_RESET " %s  " T_DIM "pacing" T_RESET " %s (×%.2f)",
                      nl_rate_str(bbr_btlbw(c) * 8), nl_time_str(c->bbr_rtprop), nl_rate_str(c->pace_rate * 8),
                      c->bbr_pacing_gain);
        if (detail >= 2 && c->sq_n && row < h) {
            int i2, sacked = 0, lost = 0;
            for (i2 = 0; i2 < c->sq_n; i2++) {
                sacked += c->sq[i2].sacked;
                lost += c->sq[i2].lost && !c->sq[i2].sacked;
            }
            tui_print(x + 2, y + row++, w - 2, TS(C_TEXT, C_DEFAULT, 0),
                      T_DIM "in flight" T_RESET " %d seg  " T_DIM "pipe" T_RESET " %.1f  " T_ACK "sacked" T_RESET
                      " %d  " T_ERR "lost" T_RESET " %d", c->sq_n, (double)pipe_bytes(c) / c->mss, sacked, lost);
        }
        if (detail >= 2 && row < h)
            tui_print(x + 2, y + row++, w - 2, TS(C_TEXT, C_DEFAULT, 0),
                      T_DIM "srtt" T_RESET " %.1f ms  " T_DIM "rto" T_RESET " %.0f ms  " T_DIM "retx" T_RESET
                      " %u (%u fast, %u RTO)", c->srtt / 1000.0, (double)(c->rto << c->backoff) / 1000.0,
                      c->retx, c->fast_retx, c->timeouts);
        if (detail >= 3 && row < h)
            tui_print(x + 2, y + row++, w - 2, TS(C_DIM, C_DEFAULT, 0),
                      "una %u nxt %u max %u  wnd %u  rcv_nxt %u rwnd %d  mss %d%s",
                      c->snd_una - c->iss, c->snd_nxt - c->iss, c->snd_max - c->iss, c->snd_wnd,
                      c->rcv_nxt - c->irs, rcv_window(c), c->mss, c->ecn_ok ? " ecn" : "");
    }
    if (!tn->conns && !tn->nlis && row < h)
        tui_print(x, y + row++, w, TS(C_DIM, C_DEFAULT, 0), "%s", _("no connections"));
    if (tn->sink_port && row < h)
        row += nl_ui_kv(x, y + row, w, _("sink received"), "%llu B", (unsigned long long)tn->sink_bytes);
    if (h - row >= 6) {
        nl_ui_graph(x, y + row, w, h - row, nl_series_find(n, "cwnd (segments)"), C_DATA, "cwnd (segments)");
        row = h;
    }
    return row;
}

static const nl_module tcp_mod = {
    .name = "tcp", .title = N_("TCP — reliable byte streams, flow and congestion control"),
    .layer = NL_L_TRANSPORT,
    .usage = N_("run NODE tcp sink [port=5001] [rwnd=BYTES] [readrate=RATE] [sack=1] [delack=40ms]\n"
                "run NODE tcp send DEST [port=5001] [bytes=1M | duration=TIME] [cc=reno|tahoe|newreno|cubic|bbr]\n"
                "             [sack=1] [ecn=1] [nagle=1] [delack=40ms] [mss=1460] [iw=10] [sndbuf=256k]\n"
                "             [ssthresh=SEGMENTS] [rto_min=200ms] [jitter=TIME] [warmup=TIME]\n"
                "             [v6=1] [pmtud=1]\n"
                "  sink accepts connections and discards the data; readrate makes it a slow\n"
                "  reader (flow control). send transfers `bytes`, or sends for `duration`, and\n"
                "  reports time, goodput and the steady-state rate (after the first quarter);\n"
                "  when several transfers to the same port end, Jain's fairness index too.\n"
                "  sack=0 falls back to NewReno recovery; delack=0 acknowledges every segment;\n"
                "  jitter delays the start by a random time up to TIME.\n"
                "  DEST may be an IPv6 address, or a node name with v6=1: the transfer then runs\n"
                "  over IPv6 (the sink accepts both). pmtud=0 clears DF on IPv4 and lets routers\n"
                "  fragment instead of discovering the path MTU.\n"
                "  Not implemented: timestamps, tail-loss probes; bbr is\n"
                "  version 1 without long-term (policer) sampling; tahoe never uses SACK."),
    .theory = N_("TCP turns the unreliable, unordered packet service of IP into a reliable byte stream "
                 "between two ports. A connection starts with the three-way handshake (SYN, SYN+ACK, ACK) "
                 "in which the two ends choose initial sequence numbers and exchange options: the "
                 "maximum segment size, the window scale, and whether selective acknowledgements are "
                 "permitted. Every byte has a sequence number; the receiver answers with cumulative "
                 "acknowledgements (\"I have everything before N\"), delayed so that one ACK covers two "
                 "segments, and buffers segments that arrive out of order. A segment that is not "
                 "acknowledged before the retransmission timeout (RTO, computed from the smoothed "
                 "round-trip time and its variance, doubled at each expiry) is sent again. The connection "
                 "ends when each side has sent a FIN and seen it acknowledged.\n\n"
                 "TCP is the same over IPv4 and IPv6: only the pseudo-header that the checksum covers "
                 "and the room left for data differ (an MSS of 1460 bytes on a 1500-byte MTU over IPv4, "
                 "1440 over IPv6, whose header is 20 bytes longer). The MSS the ends announce reflects "
                 "their own links only, so the sender also discovers the smallest MTU of the path: its "
                 "IPv4 segments carry the don't-fragment flag, and IPv6 routers never fragment, so a "
                 "router that cannot forward a segment discards it and reports the MTU of the next hop "
                 "(ICMP \"fragmentation needed\", ICMPv6 \"packet too big\"). The sender lowers its MSS, "
                 "cuts the segments in flight that no longer fit and sends them again.\n\n"
                 "Waiting for the timer is slow, so the sender reads the ACKs for signs of loss. With "
                 "selective acknowledgements (SACK) every ACK also lists the blocks received beyond the "
                 "hole; the sender keeps a scoreboard of what is SACKed, declares a segment lost when "
                 "three segments' worth of data above it have arrived, and retransmits every hole of the "
                 "window in one round trip, sending whenever its estimate of the data still in the "
                 "network (the pipe) is below the congestion window. Without SACK it only has duplicate "
                 "ACKs: three of them trigger a fast retransmit of the first hole, and NewReno repairs "
                 "the following ones one per round trip, each revealed by a partial ACK.\n\n"
                 "Two windows limit the sender. The receive window, advertised in every segment, is "
                 "flow control: it keeps a fast sender from overrunning a slow reader, and when it drops "
                 "to zero the sender probes periodically until it reopens. The congestion window (cwnd) "
                 "is the sender's own estimate of what the network can carry. It grows exponentially in "
                 "slow start up to ssthresh, then linearly in congestion avoidance (additive increase); a "
                 "loss halves it (multiplicative decrease), giving the classic sawtooth. Tahoe restarts "
                 "from one segment after every loss; Reno adds fast recovery; CUBIC grows the window as a "
                 "cubic function of the time since the last loss, which suits fast long-distance paths. "
                 "With ECN, routers mark packets instead of dropping them and the receiver echoes the "
                 "mark, so the sender slows down without losing data.\n\n"
                 "BBR does not treat loss as the signal. From every ACK it computes a delivery-rate "
                 "sample and keeps two estimates of the path: the bottleneck bandwidth (the largest rate "
                 "seen in the last ten round trips) and the round-trip propagation time (the smallest RTT "
                 "seen in the last ten seconds). It paces packets at gain x bandwidth and caps the data "
                 "in flight at a multiple of bandwidth x RTT. STARTUP doubles the rate every round until "
                 "the bandwidth stops growing, DRAIN empties the queue that this created, PROBE_BW "
                 "cruises with an eight-phase gain cycle (1.25 to look for more bandwidth, 0.75 to drain "
                 "what that left, then six phases at 1), and every ten seconds without a new minimum, "
                 "PROBE_RTT cuts the window to four segments for 200 ms to measure the RTT again."),
    .run = tcp_run, .stop = tcp_stop, .crash = tcp_crash, .recover = tcp_recover, .panel = tcp_panel,
};
NL_MODULE(tcp_mod)

/* ------------------------------------------------------------------ */
/* Dissector                                                          */
/* ------------------------------------------------------------------ */

static void dis_tcp(nl_dissect *o, const uint8_t *d, int len, int off)
{
    static const struct { int bit; const char *name; } F[] = {
        { TH_SYN, "SYN" }, { TH_FIN, "FIN" }, { TH_RST, "RST" }, { TH_PSH, "PSH" }, { TH_ACK, "ACK" },
        { TH_ECE, "ECE" }, { TH_CWR, "CWR" },
    };
    char fl[48] = "", optbuf[200] = "";
    uint16_t sp, dp;
    int hl, flags, plen, i;
    size_t k = 0;

    if (len < 20) {
        nl_dis_summary(o, "TCP", 75, "truncated TCP segment");
        return;
    }
    sp = nl_get16(d);
    dp = nl_get16(d + 2);
    hl = (d[12] >> 4) * 4;
    flags = d[13];
    if (hl < 20 || hl > len)
        hl = 20;
    plen = len - hl;
    for (i = 0; i < (int)(sizeof(F) / sizeof(F[0])); i++)
        if (flags & F[i].bit)
            k += (size_t)snprintf(fl + k, sizeof(fl) - k, "%s%s", k ? ", " : "", F[i].name);
    nl_dis_field(o, "TCP", "Source port", off, 2, "%u", sp);
    nl_dis_field(o, "TCP", "Destination port", off + 2, 2, "%u", dp);
    nl_dis_field(o, "TCP", "Sequence number", off + 4, 4, "%u", nl_get32(d + 4));
    nl_dis_field(o, "TCP", "Acknowledgment", off + 8, 4, "%u%s", nl_get32(d + 8), flags & TH_ACK ? "" : " (unused)");
    nl_dis_field(o, "TCP", "Header length", off + 12, 1, "%d bytes", hl);
    nl_dis_field(o, "TCP", "Flags", off + 13, 1, "0x%02x [%s]", flags, fl);
    nl_dis_field(o, "TCP", "Window", off + 14, 2, "%u", nl_get16(d + 14));
    nl_dis_field(o, "TCP", "Checksum", off + 16, 2, "0x%04x", nl_get16(d + 16));
    k = 0;
    for (i = 20; i < hl; ) {
        int kind = d[i], l = kind <= 1 ? 1 : (i + 1 < hl ? d[i + 1] : 0);

        if (kind == 0 || l < 1 || i + l > hl)
            break;
        if (kind == 2 && l == 4)
            k += (size_t)snprintf(optbuf + k, sizeof(optbuf) - k, "MSS=%u ", nl_get16(d + i + 2));
        else if (kind == 3 && l == 3)
            k += (size_t)snprintf(optbuf + k, sizeof(optbuf) - k, "WS=%u ", d[i + 2]);
        else if (kind == 4 && l == 2)
            k += (size_t)snprintf(optbuf + k, sizeof(optbuf) - k, "SACK_PERM ");
        else if (kind == 5 && l >= 10) {
            int j;
            k += (size_t)snprintf(optbuf + k, sizeof(optbuf) - k, "SACK=");
            for (j = 2; j + 8 <= l && k < sizeof(optbuf); j += 8)
                k += (size_t)snprintf(optbuf + k, sizeof(optbuf) - k, "%s%u-%u", j > 2 ? "," : "",
                                      nl_get32(d + i + j), nl_get32(d + i + j + 4));
            if (k < sizeof(optbuf))
                k += (size_t)snprintf(optbuf + k, sizeof(optbuf) - k, " ");
        }
        else if (kind != 1)
            k += (size_t)snprintf(optbuf + k, sizeof(optbuf) - k, "opt%d ", kind);
        if (k >= sizeof(optbuf))
            break;
        i += l;
    }
    if (hl > 20)
        nl_dis_field(o, "TCP", "Options", off + 20, hl - 20, "%s", optbuf);
    if (plen > 0)
        nl_dis_field(o, "TCP", "Payload", off + hl, plen, "%d bytes", plen);
    if (flags & TH_ACK)
        nl_dis_summary(o, "TCP", 75, "%u → %u [%s] Seq=%u Ack=%u Win=%u Len=%d %s", sp, dp, fl, nl_get32(d + 4),
                       nl_get32(d + 8), nl_get16(d + 14), plen, optbuf);
    else
        nl_dis_summary(o, "TCP", 75, "%u → %u [%s] Seq=%u Win=%u Len=%d %s", sp, dp, fl, nl_get32(d + 4),
                       nl_get16(d + 14), plen, optbuf);
    if (plen > 0)
        nl_dissect_tcp_payload(o, sp, dp, d + hl, plen, off + hl);
}

NL_INIT(tcp_dissector)
{
    nl_dissector_ip(NL_PROTO_TCP, dis_tcp);
}
