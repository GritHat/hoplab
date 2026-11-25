/*
 * tcp.c - TCP: connections (three-way handshake, sliding window, cumulative
 *         ACKs, out-of-order reassembly, flow control with window scaling and
 *         zero-window probes, RFC 6298 retransmission timer with Karn's rule,
 *         fast retransmit / fast recovery, FIN and RST), pluggable congestion
 *         control (tahoe, reno, newreno, cubic, simplified bbr), ECN, the
 *         dissector, and the `tcp` tool (bulk sender and sink).
 *
 * Simplifications: no SACK, no delayed ACKs (every segment is acknowledged),
 * no Nagle, no timestamps (one RTT measurement at a time), no path-MTU
 * discovery, TIME_WAIT lasts one second.
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

enum { BBR_STARTUP, BBR_DRAIN, BBR_PROBE_BW };

#define OOO_MAX   64
#define MAX_LIS   8
#define RTO_MAX   NL_SEC(60)
#define TICK      NL_MS(200)

typedef struct { uint32_t seq; int len; uint8_t *data; } ooo_seg;

struct nl_tcp {
    nl_node *n;
    struct nl_tcp *next;
    nl_ip    lip, rip;
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
    int      rtt_valid, timing, backoff, tries;
    uint32_t rtt_seq;
    nl_time  rtt_time;
    nl_ev    rto_timer, persist_timer, wait_timer, tick_timer, pace_timer, drain_timer;
    int      persist_backoff;

    /* congestion control */
    int      cc, iw, init_ssthresh;
    double   cwnd, ssthresh;            /* bytes */
    int      dupacks, in_recovery;
    uint32_t recover;
    double   w_max, cub_k, cub_origin, w_est;     /* cubic, in segments */
    nl_time  epoch;
    int      ecn_ok, ecn_want, ece_latch, cwr_pending;
    uint32_t ecn_recover;
    /* bbr (simplified) */
    int      bbr_state, bbr_phase, bbr_full_cnt;
    double   bbr_bw, bbr_full_bw, bbr_bws[10], pace_rate;     /* bytes/s */
    int      bbr_nbw;
    nl_time  bbr_rtprop, bbr_rtprop_t, bbr_round_t, bbr_phase_t, next_tx;
    uint32_t bbr_round_end;
    uint64_t delivered, bbr_round_deliv;

    /* receiver */
    uint32_t irs, rcv_nxt;
    int      rwnd_max, last_adv;
    ooo_seg  ooo[OOO_MAX];
    int      nooo;
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
static void tcp_input(nl_node *n, void *ctx, const nl_ipmeta *m);
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
    nl_ip_bind(n, NL_PROTO_TCP, tcp_input, tn);
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
    c->rwnd_max = (int)nl_arg_int(&a, "rwnd", 1 << 20);
    if (c->rwnd_max < 256) c->rwnd_max = 256;
    if (c->rwnd_max > (1 << 24)) c->rwnd_max = 1 << 24;
    c->scap = (int)nl_arg_int(&a, "sndbuf", 256 * 1024);
    if (c->scap < 2048) c->scap = 2048;
    c->ecn_want = (int)nl_arg_int(&a, "ecn", 0);
    c->iw = (int)nl_arg_int(&a, "iw", 10);
    if (c->iw < 1) c->iw = 1;
    c->init_ssthresh = (int)nl_arg_int(&a, "ssthresh", 0);
    c->rto_min = nl_arg_time(&a, "rto_min", NL_MS(200));
    c->read_rate = nl_arg_rate(&a, "readrate", 0);
    while ((c->rwnd_max >> c->rcv_wscale) > 65535)
        c->rcv_wscale++;
}

static nl_tcp *conn_new(tcp_node *tn, nl_ip lip, uint16_t lport, nl_ip rip, uint16_t rport, const char *opts)
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
    pmtu = nl_ip_mtu(tn->n, rip) - 40;
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

static nl_tcp *conn_find(tcp_node *tn, nl_ip lip, uint16_t lport, nl_ip rip, uint16_t rport)
{
    nl_tcp *c;

    for (c = tn->conns; c; c = c->next)
        if (c->lport == lport && c->rport == rport && c->rip == rip && c->lip == lip)
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

static void raw_xmit(nl_node *n, nl_ip src, nl_ip dst, uint16_t sport, uint16_t dport, uint32_t seq,
                     uint32_t ack, int flags, int win, const uint8_t *opt, int optlen,
                     const uint8_t *data, int len, uint8_t tos)
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
    nl_put16(b + 16, nl_l4_cksum(src, dst, NL_PROTO_TCP, b, hl + len));
    memset(&tx, 0, sizeof(tx));
    tx.src = src;
    tx.dst = dst;
    tx.proto = NL_PROTO_TCP;
    tx.tos = tos;
    nl_ip_send(n, &tx, b, hl + len);
}

static void seg_xmit(nl_tcp *c, uint32_t seq, int flags, const uint8_t *data, int len)
{
    uint8_t opt[8] = { 0 };
    int optlen = 0, win = rcv_window(c);

    if (flags & TH_SYN) {
        opt[0] = 2; opt[1] = 4;                         /* MSS */
        nl_put16(opt + 2, (uint16_t)c->mss);
        opt[4] = 1;                                     /* NOP */
        opt[5] = 3; opt[6] = 3; opt[7] = (uint8_t)c->rcv_wscale;   /* window scale */
        optlen = 8;
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
    }
    raw_xmit(c->n, c->lip, c->rip, c->lport, c->rport, seq, c->rcv_nxt, flags, win, opt, optlen, data, len,
             (uint8_t)(c->ecn_ok && len > 0 ? 2 : 0));
}

static void send_ack(nl_tcp *c)
{
    seg_xmit(c, c->snd_nxt, TH_ACK, NULL, 0);
}

static void rto_arm(nl_tcp *c)
{
    nl_time t = c->rto << c->backoff;

    nl_cancel(&c->rto_timer);
    c->rto_timer = nl_after(c->n, t > RTO_MAX ? RTO_MAX : t, rto_fire, c);
}

/* Retransmit the first unacknowledged segment */
static void retx_one(nl_tcp *c)
{
    int len = c->slen < c->mss ? c->slen : c->mss;
    int out = (int)(c->snd_max - c->snd_una);

    if (len > out)
        len = out;
    c->timing = 0;                      /* Karn: never time a retransmitted segment */
    c->retx++;
    nl_count(c->n, "tcp.retransmits", 1);
    if (len > 0)
        seg_xmit(c, c->snd_una, TH_ACK | TH_PSH, c->sbuf, len);
    else if (c->fin_sent && !c->fin_acked)
        seg_xmit(c, c->fin_seq, TH_FIN | TH_ACK, NULL, 0);
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
    seg_xmit(c, c->snd_una, TH_ACK, c->sbuf, 1);
    if (SEQ_LT(c->snd_max, c->snd_una + 1))
        c->snd_max = c->snd_una + 1;
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

static void tcp_output(nl_tcp *c)
{
    if (c->state < ST_ESTABLISHED || c->state == ST_TIME_WAIT)
        return;
    for (;;) {
        int off = (int)(c->snd_nxt - c->snd_una), avail = c->slen - off;
        double wnd = c->cwnd < (double)c->snd_wnd ? c->cwnd : (double)c->snd_wnd;
        int can = (int)wnd - off, len;

        if (avail <= 0)
            break;
        if (can <= 0)
            break;
        len = avail < c->mss ? avail : c->mss;
        if (len > can) {
            /* avoid silly small segments unless nothing is in flight */
            if (off > 0 && can < c->mss)
                break;
            len = can;
        }
        if (c->pace_rate > 0) {
            if (nl_now() < c->next_tx) {
                if (!c->pace_timer)
                    c->pace_timer = nl_after(c->n, c->next_tx - nl_now(), pace_fire, c);
                break;
            }
            c->next_tx = (nl_now() > c->next_tx ? nl_now() : c->next_tx) +
                         (nl_time)(len * 1e6 / c->pace_rate);
        }
        if (SEQ_LT(c->snd_nxt, c->snd_max)) {
            c->retx++;
            nl_count(c->n, "tcp.retransmits", 1);
        } else if (!c->timing) {
            c->timing = 1;
            c->rtt_seq = c->snd_nxt + (uint32_t)len;
            c->rtt_time = nl_now();
        }
        seg_xmit(c, c->snd_nxt, TH_ACK | (len == avail ? TH_PSH : 0), c->sbuf + off, len);
        c->snd_nxt += (uint32_t)len;
        if (SEQ_GT(c->snd_nxt, c->snd_max))
            c->snd_max = c->snd_nxt;
        if (!c->rto_timer)
            rto_arm(c);
    }
    if (c->fin_pending && !c->fin_sent && (int)(c->snd_nxt - c->snd_una) == c->slen) {
        c->fin_seq = c->snd_nxt;
        c->fin_sent = 1;
        seg_xmit(c, c->snd_nxt, TH_FIN | TH_ACK, NULL, 0);
        c->snd_nxt++;
        if (SEQ_GT(c->snd_nxt, c->snd_max))
            c->snd_max = c->snd_nxt;
        if (c->state == ST_ESTABLISHED)
            c->state = ST_FIN_WAIT_1;
        else if (c->state == ST_CLOSE_WAIT)
            c->state = ST_LAST_ACK;
        nl_log(c->n, NL_L_TRANSPORT, 2, 0, T_DATA "TCP" T_RESET " :%u → %s:%u FIN (%s)", c->lport,
               nl_ip_str(c->rip), c->rport, ST_NAME[c->state]);
        if (!c->rto_timer)
            rto_arm(c);
    }
    if (c->snd_wnd == 0 && c->slen > 0 && c->snd_una == c->snd_max && !c->persist_timer) {
        c->persist_backoff = 0;
        c->persist_timer = nl_after(c->n, NL_MS(500), persist_fire, c);
    }
}

/* ------------------------------------------------------------------ */
/* Congestion control                                                 */
/* ------------------------------------------------------------------ */

static void cc_init(nl_tcp *c)
{
    c->cwnd = (double)c->iw * c->mss;
    c->ssthresh = c->init_ssthresh > 0 ? (double)c->init_ssthresh * c->mss : 1e12;
    c->bbr_state = BBR_STARTUP;
    c->bbr_round_end = c->snd_nxt;
    c->bbr_round_t = nl_now();
}

static void bbr_update(nl_tcp *c, uint32_t ack)
{
    static const double cycle[8] = { 1.25, 0.75, 1, 1, 1, 1, 1, 1 };
    double pg = 2.89, cg = 2.89, bdp;
    int i;

    if (SEQ_GEQ(ack, c->bbr_round_end)) {               /* one round trip completed */
        nl_time dt = nl_now() - c->bbr_round_t;

        if (dt > 0 && c->delivered > c->bbr_round_deliv) {
            double rate = (double)(c->delivered - c->bbr_round_deliv) * 1e6 / (double)dt;

            c->bbr_bws[c->bbr_nbw++ % 10] = rate;
            c->bbr_bw = 0;
            for (i = 0; i < 10 && i < c->bbr_nbw; i++)
                if (c->bbr_bws[i] > c->bbr_bw)
                    c->bbr_bw = c->bbr_bws[i];
            if (c->bbr_state == BBR_STARTUP) {
                if (c->bbr_bw >= c->bbr_full_bw * 1.25) {
                    c->bbr_full_bw = c->bbr_bw;
                    c->bbr_full_cnt = 0;
                } else if (++c->bbr_full_cnt >= 3) {
                    c->bbr_state = BBR_DRAIN;
                    nl_log(c->n, NL_L_TRANSPORT, 2, 0, T_DATA "TCP" T_RESET " bbr: %s (%s) → DRAIN",
                           _("bottleneck bandwidth found"), nl_rate_str(c->bbr_bw * 8));
                }
            }
        }
        c->bbr_round_end = c->snd_max;
        c->bbr_round_t = nl_now();
        c->bbr_round_deliv = c->delivered;
    }
    if (!c->bbr_bw || !c->bbr_rtprop)
        return;                                         /* no model yet: keep the initial window */
    bdp = c->bbr_bw * (double)c->bbr_rtprop / 1e6;
    if (c->bbr_state == BBR_DRAIN) {
        pg = 1 / 2.89;
        cg = 2.89;
        if ((double)(c->snd_max - c->snd_una) <= bdp) {
            c->bbr_state = BBR_PROBE_BW;
            c->bbr_phase = (int)nl_rand(6) + 2;
            c->bbr_phase_t = nl_now();
        }
    }
    if (c->bbr_state == BBR_PROBE_BW) {
        if (nl_now() - c->bbr_phase_t >= c->bbr_rtprop) {
            c->bbr_phase = (c->bbr_phase + 1) % 8;
            c->bbr_phase_t = nl_now();
        }
        pg = cycle[c->bbr_phase];
        cg = 2.0;
    }
    c->pace_rate = pg * c->bbr_bw;
    c->cwnd = cg * bdp;
    if (c->cwnd < 4.0 * c->mss)
        c->cwnd = 4.0 * c->mss;
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

/* New data acknowledged outside recovery */
static void cc_ack(nl_tcp *c, int acked, uint32_t ack)
{
    if (c->cc == CC_BBR) {
        bbr_update(c, ack);
        return;
    }
    if (c->cwnd < c->ssthresh) {                        /* slow start */
        c->cwnd += acked < 2 * c->mss ? acked : 2 * c->mss;
        if (c->cwnd > c->ssthresh)
            c->cwnd = c->ssthresh;
    } else if (c->cc == CC_CUBIC) {
        cubic_ack(c, acked);
    } else {                                            /* congestion avoidance */
        c->cwnd += (double)c->mss * acked / c->cwnd;
    }
}

/* Multiplicative decrease; returns the new ssthresh */
static void cc_reduce(nl_tcp *c)
{
    double flight = (double)(c->snd_max - c->snd_una);

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

static void fast_retransmit(nl_tcp *c)
{
    c->fast_retx++;
    nl_count(c->n, "tcp.fast_retransmits", 1);
    cc_reduce(c);
    nl_log(c->n, NL_L_TRANSPORT, 2, 0, T_WARN "TCP" T_RESET " :%u %s seq=%u (%s, cwnd %.1f → ssthresh %.1f)",
           c->lport, _("3 duplicate ACKs: fast retransmit"), c->snd_una - c->iss, CC_NAME[c->cc],
           c->cwnd / c->mss, c->ssthresh < 1e11 ? c->ssthresh / c->mss : 0.0);
    c->recover = c->snd_max;
    if (c->cc == CC_TAHOE) {
        c->cwnd = c->mss;
        c->dupacks = 0;
        c->snd_nxt = c->snd_una;        /* back to slow start from the hole */
        if (c->fin_sent && !c->fin_acked)
            c->fin_sent = 0;
        c->timing = 0;
        rto_arm(c);
        cc_sample(c);
        tcp_output(c);
        return;
    }
    c->in_recovery = 1;
    if (c->cc != CC_BBR)
        c->cwnd = c->ssthresh + 3.0 * c->mss;
    retx_one(c);
    rto_arm(c);
    cc_sample(c);
}

static void rto_fire(void *ctx)
{
    nl_tcp *c = ctx;

    c->rto_timer = 0;
    if (c->state == ST_CLOSED)
        return;
    if (c->state == ST_SYN_SENT || c->state == ST_SYN_RCVD) {
        if (++c->tries > 5) {
            nl_log(c->n, NL_L_TRANSPORT, 1, 0, T_ERR "TCP" T_RESET " %s:%u: %s", nl_ip_str(c->rip), c->rport,
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
        nl_log(c->n, NL_L_TRANSPORT, 1, 0, T_ERR "TCP" T_RESET " %s:%u: %s", nl_ip_str(c->rip), c->rport,
               _("too many retransmissions: connection dropped"));
        conn_abort(c, NL_TCP_CLOSED_TIMEOUT);
        return;
    }
    c->timeouts++;
    nl_count(c->n, "tcp.timeouts", 1);
    cc_reduce(c);
    nl_log(c->n, NL_L_TRANSPORT, 2, 0, T_ERR "TCP" T_RESET " :%u %s seq=%u (RTO %s, cwnd %.1f → 1)", c->lport,
           _("retransmission timeout"), c->snd_una - c->iss, nl_time_str(c->rto << c->backoff),
           c->cwnd / c->mss);
    c->cwnd = c->mss;
    c->dupacks = 0;
    c->in_recovery = 0;
    c->recover = c->snd_max;
    c->epoch = 0;
    c->pace_rate = 0;
    c->timing = 0;
    if (c->backoff < 6)
        c->backoff++;
    /* go back: everything from snd_una is sent again as the window allows */
    c->snd_nxt = c->snd_una;
    if (c->fin_sent && !c->fin_acked)
        c->fin_sent = 0;
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
    if (!c->bbr_rtprop || r <= c->bbr_rtprop || nl_now() - c->bbr_rtprop_t > NL_SEC(10)) {
        c->bbr_rtprop = r;
        c->bbr_rtprop_t = nl_now();
    }
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
    nl_log(c->n, NL_L_TRANSPORT, 2, 0, T_DATA "TCP" T_RESET " :%u ⇄ %s:%u %s", c->lport, nl_ip_str(c->rip),
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

static void rx_data(nl_tcp *c, uint32_t seq, const uint8_t *d, int len)
{
    int win = rcv_window(c), i, again;

    if (SEQ_LT(seq, c->rcv_nxt)) {                      /* trim what we already have */
        int skip = (int)(c->rcv_nxt - seq);
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
            if (c->ooo[i].seq == seq)
                return;
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
    nl_log(c->n, NL_L_TRANSPORT, 2, 0, T_OK "TCP" T_RESET " :%u ⇄ %s:%u %s (MSS %d, %s%s)", c->lport,
           nl_ip_str(c->rip), c->rport, _("connection established"), c->mss, CC_NAME[c->cc],
           c->ecn_ok ? ", ECN" : "");
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

static void ack_input(nl_tcp *c, uint32_t ack, int flags, int len, uint32_t wnd)
{
    int window_changed = wnd != c->snd_wnd;

    if (SEQ_GT(ack, c->snd_max))
        return;
    if (SEQ_GT(ack, c->snd_una)) {
        int acked = (int)(ack - c->snd_una), data_acked = acked;

        if (c->fin_sent && ack == c->fin_seq + 1) {
            c->fin_acked = 1;
            data_acked--;
        }
        if (data_acked > c->slen)
            data_acked = c->slen;
        if (data_acked > 0) {
            memmove(c->sbuf, c->sbuf + data_acked, (size_t)(c->slen - data_acked));
            c->slen -= data_acked;
        }
        c->snd_una = ack;
        if (SEQ_LT(c->snd_nxt, c->snd_una))
            c->snd_nxt = c->snd_una;
        if (wnd == 0 && c->snd_wnd != 0 && c->slen > 0) {
            nl_count(c->n, "tcp.zero_windows", 1);
            nl_log(c->n, NL_L_TRANSPORT, 2, 0, T_WARN "TCP" T_RESET " :%u %s", c->lport,
                   _("the receiver advertises window 0: sender blocked by flow control"));
        }
        c->snd_wnd = wnd;
        if (wnd > 0)
            nl_cancel(&c->persist_timer);
        c->bytes_acked += (uint64_t)data_acked;
        c->tick_acked += (uint64_t)data_acked;
        c->delivered += (uint64_t)data_acked;
        nl_count(c->n, "tcp.bytes_acked", data_acked);
        if (c->timing && SEQ_GEQ(ack, c->rtt_seq)) {
            c->timing = 0;
            rtt_sample(c, nl_now() - c->rtt_time);
        }
        c->backoff = c->tries = 0;

        if ((flags & TH_ECE) && c->ecn_ok && SEQ_GT(ack, c->ecn_recover) && !c->in_recovery) {
            cc_reduce(c);
            if (c->cc != CC_BBR)
                c->cwnd = c->ssthresh;
            c->ecn_recover = c->snd_max;
            c->cwr_pending = 1;
            nl_count(c->n, "tcp.ecn_reductions", 1);
            nl_log(c->n, NL_L_TRANSPORT, 2, 0, T_WARN "TCP" T_RESET " :%u %s (cwnd → %.1f)", c->lport,
                   _("ECN echo: congestion signalled without loss"), c->cwnd / c->mss);
            cc_sample(c);
        } else if (c->in_recovery) {
            if (SEQ_GEQ(ack, c->recover) || c->cc == CC_RENO) {
                c->in_recovery = 0;                     /* full ACK (classic Reno: any new ACK) */
                if (c->cc != CC_BBR)
                    c->cwnd = c->ssthresh;
                cc_sample(c);
            } else {                                    /* partial ACK: the next hole */
                retx_one(c);
                if (c->cc != CC_BBR) {
                    c->cwnd -= data_acked;
                    c->cwnd += c->mss;
                    if (c->cwnd < c->mss)
                        c->cwnd = c->mss;
                }
            }
            if (c->cc == CC_BBR)
                bbr_update(c, ack);
        } else if (data_acked > 0) {
            cc_ack(c, data_acked, ack);
        }
        c->dupacks = 0;
        if (c->snd_una == c->snd_max)
            nl_cancel(&c->rto_timer);
        else
            rto_arm(c);
        if (data_acked > 0 && c->ops && c->ops->on_sent)
            c->ops->on_sent(c, c->ctx, data_acked);
        return;
    }
    if (wnd == 0 && c->snd_wnd != 0 && c->slen > 0) {
        nl_count(c->n, "tcp.zero_windows", 1);
        nl_log(c->n, NL_L_TRANSPORT, 2, 0, T_WARN "TCP" T_RESET " :%u %s", c->lport,
               _("the receiver advertises window 0: sender blocked by flow control"));
    }
    c->snd_wnd = wnd;
    if (wnd > 0)
        nl_cancel(&c->persist_timer);
    if (ack == c->snd_una && len == 0 && !(flags & (TH_SYN | TH_FIN)) && c->snd_una != c->snd_max &&
        !window_changed) {
        nl_count(c->n, "tcp.dup_acks", 1);
        c->dupacks++;
        if (c->in_recovery) {
            if (c->cc != CC_BBR)
                c->cwnd += c->mss;                      /* a segment left the network */
        } else if (c->dupacks == 3 && SEQ_GT(c->snd_una, c->recover)) {
            fast_retransmit(c);
        }
    }
}

static void tcp_input(nl_node *n, void *ctx, const nl_ipmeta *m)
{
    tcp_node *tn = ctx;
    const uint8_t *h = m->l4;
    int hl, len, flags, i;
    uint16_t sport, dport;
    uint32_t seq, ack, wnd;
    nl_tcp *c;

    if (m->l4len < 20)
        return;
    hl = (h[12] >> 4) * 4;
    if (hl < 20 || hl > m->l4len)
        return;
    if (nl_l4_cksum(m->src, m->dst, NL_PROTO_TCP, h, m->l4len) != 0) {
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
                raw_xmit(n, m->dst, m->src, dport, sport, ack, 0, TH_RST, 0, NULL, 0, NULL, 0, 0);
            else
                raw_xmit(n, m->dst, m->src, dport, sport, 0,
                         seq + (uint32_t)len + ((flags & TH_SYN) ? 1 : 0) + ((flags & TH_FIN) ? 1 : 0),
                         TH_RST | TH_ACK, 0, NULL, 0, NULL, 0, 0);
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
        if (!c->ws_ok)
            c->rcv_wscale = 0;
        if ((flags & (TH_ECE | TH_CWR)) == (TH_ECE | TH_CWR) && c->ecn_want)
            c->ecn_ok = 1;
        c->snd_nxt = c->snd_max = c->iss + 1;
        nl_log(n, NL_L_TRANSPORT, 2, m->pkt_id, T_DATA "TCP" T_RESET " :%u ← %s:%u SYN → SYN+ACK", dport,
               nl_ip_str(m->src), sport);
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
        nl_log(n, NL_L_TRANSPORT, refused ? 1 : 2, m->pkt_id, T_ERR "TCP" T_RESET " %s:%u: %s", nl_ip_str(c->rip),
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
        ack_input(c, ack, flags, len, wnd);
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
                       nl_ip_str(c->rip), c->rport, _("connection closed"));
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

        rx_data(c, seq, h + hl, len);
        if (c->state == ST_CLOSED)
            return;
        if (c->rcv_nxt == before && seq != before && SEQ_GT(seq, before))
            nl_log(n, NL_L_TRANSPORT, 3, m->pkt_id, T_ACK "TCP" T_RESET " :%u %s seq=%u, %s %u → dup ACK",
                   c->lport, _("out of order"), seq - c->irs, _("expected"), before - c->irs);
    }
    if ((flags & TH_FIN) && seq + (uint32_t)len == c->rcv_nxt &&
        (c->state == ST_ESTABLISHED || c->state == ST_FIN_WAIT_1 || c->state == ST_FIN_WAIT_2)) {
        c->rcv_nxt++;
        nl_log(n, NL_L_TRANSPORT, 2, m->pkt_id, T_DATA "TCP" T_RESET " :%u ← %s:%u FIN", c->lport,
               nl_ip_str(c->rip), c->rport);
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
        send_ack(c);
    }
    tcp_output(c);
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

nl_tcp *nl_tcp_connect(nl_node *n, nl_ip dst, uint16_t dport, const nl_tcp_ops *ops, void *ctx, const char *opts)
{
    tcp_node *tn = tcp_get(n);
    const nl_route *r;
    nl_ip lip = dst;
    nl_tcp *c;
    uint16_t lport;

    if (!tn || !n->up)
        return NULL;
    if (!nl_ip_is_local(n, dst)) {
        r = nl_route_lookup(n, dst);
        if (!r) {
            nl_log(n, NL_L_TRANSPORT, 1, 0, T_ERR "TCP" T_RESET " %s %s", _("no route to"), nl_ip_str(dst));
            return NULL;
        }
        lip = n->ifc[r->ifx].ip;
    }
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
    nl_log(n, NL_L_TRANSPORT, 2, 0, T_DATA "TCP" T_RESET " :%u → %s:%u SYN (%s)", lport, nl_ip_str(dst), dport,
           _("opening a connection"));
    seg_xmit(c, c->iss, TH_SYN, NULL, 0);
    rto_arm(c);
    return c;
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
                 NULL, 0, NULL, 0, 0);
    c->ops = NULL;
    conn_destroy(c);
}

void nl_tcp_peer(const nl_tcp *c, nl_ip *ip, uint16_t *port)
{
    if (ip) *ip = c->rip;
    if (port) *port = c->rport;
}

void nl_tcp_local(const nl_tcp *c, nl_ip *ip, uint16_t *port)
{
    if (ip) *ip = c->lip;
    if (port) *port = c->lport;
}

nl_node *nl_tcp_node(const nl_tcp *c) { return c->n; }

/* ------------------------------------------------------------------ */
/* The `tcp` tool: bulk sender and sink                               */
/* ------------------------------------------------------------------ */

typedef struct { nl_node *n; long long total, queued; nl_time t0, t_conn; char dest[24]; char cc[12]; } xfer;
typedef struct { nl_node *n; unsigned long long bytes; nl_time t0; } sink_conn;

static void xfer_fill(nl_tcp *c, xfer *x)
{
    static uint8_t pat[8192];
    int i, w;

    if (!pat[1])
        for (i = 0; i < (int)sizeof(pat); i++)
            pat[i] = (uint8_t)('A' + i % 26);
    while (x->queued < x->total) {
        long long left = x->total - x->queued;
        w = nl_tcp_write(c, pat, left < (long long)sizeof(pat) ? (int)left : (int)sizeof(pat));
        if (w <= 0)
            break;
        x->queued += w;
    }
    if (x->queued >= x->total)
        nl_tcp_close(c);
}

static void xfer_connect(nl_tcp *c, void *ctx)
{
    xfer *x = ctx;

    x->t_conn = nl_now();
    nl_log(x->n, NL_L_APP, 1, 0, T_OK "tcp send %s:" T_RESET " %s (%s), %lld B", x->dest,
           _("connected"), nl_time_str(x->t_conn - x->t0), x->total);
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

    if (reason == NL_TCP_CLOSED_FIN && c->bytes_acked >= (uint64_t)x->total) {
        double kbps = secs > 0 ? (double)x->total * 8.0 / secs / 1000.0 : 0;

        nl_log(x->n, NL_L_APP, 1, 0, T_OK "tcp send %s: %s" T_RESET " %lld B, %.3f s, %s  " T_DIM
               "(%s, %u %s, %u fast, %u %s)", x->dest, _("done"), x->total, secs, nl_rate_str(kbps * 1000.0),
               x->cc, c->retx, _("retransmissions"), c->fast_retx, c->timeouts, _("timeouts"));
        nl_count(x->n, "tcp.send_done", 1);
        nl_count_set(x->n, "tcp.goodput_kbps", kbps);
        nl_count_set(x->n, "tcp.time_ms", secs * 1000.0);
    } else {
        nl_log(x->n, NL_L_APP, 1, 0, T_ERR "tcp send %s: %s" T_RESET " (%s), %llu / %lld B", x->dest, _("failed"),
               reason == NL_TCP_CLOSED_REFUSED ? _("connection refused")
               : reason == NL_TCP_CLOSED_TIMEOUT ? _("timeout") : _("connection reset"),
               (unsigned long long)c->bytes_acked, x->total);
        nl_count(x->n, "tcp.send_failed", 1);
    }
    free(x);
}

static const nl_tcp_ops xfer_ops = {
    .on_connect = xfer_connect, .on_sent = xfer_sent, .on_close = xfer_close,
};

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
               nl_ip_str(c->rip), c->rport, secs, nl_rate_str(secs > 0 ? (double)s->bytes * 8.0 / secs : 0));
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
        nl_ip dst;
        xfer *x;

        if (!target || !nl_resolve(target, &dst)) {
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
        x->t0 = nl_now();
        snprintf(x->dest, sizeof(x->dest), "%s", target);
        snprintf(x->cc, sizeof(x->cc), "%s", nl_arg_str(a, "cc", "reno"));
        join_opts(a, 2, opts, sizeof(opts));
        nl_log(n, NL_L_APP, 1, 0, T_INFO "tcp send %s" T_RESET " (%s:%d): %lld B, %s", target, nl_ip_str(dst),
               (int)nl_arg_int(a, "port", 5001), x->total, x->cc);
        if (!nl_tcp_connect(n, dst, (uint16_t)nl_arg_int(a, "port", 5001), &xfer_ops, x, opts)) {
            free(x);
            return -1;
        }
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
                  T_DATA, c->lport, nl_ip_str(c->rip), c->rport,
                  c->state == ST_ESTABLISHED ? T_OK : T_WARN, ST_NAME[c->state]);
        if (row < h)
            tui_print(x + 2, y + row++, w - 2, TS(C_TEXT, C_DEFAULT, 0),
                      T_DIM "%s" T_RESET " cwnd %.1f  ssthresh %s%.1f" T_RESET "  %s",
                      CC_NAME[c->cc], c->cwnd / c->mss, c->ssthresh < 1e11 ? "" : T_DIM,
                      c->ssthresh < 1e11 ? c->ssthresh / c->mss : 0.0,
                      c->in_recovery ? T_WARN "fast recovery" : c->cwnd < c->ssthresh ? "slow start"
                      : "congestion avoidance");
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
    .usage = N_("run NODE tcp sink [port=5001] [rwnd=BYTES] [readrate=RATE]\n"
                "run NODE tcp send DEST [port=5001] [bytes=1M] [cc=reno|tahoe|newreno|cubic|bbr]\n"
                "             [ecn=1] [mss=1460] [iw=10] [ssthresh=SEGMENTS] [rto_min=200ms]\n"
                "  sink accepts connections and discards the data; readrate makes it a slow\n"
                "  reader (flow control). send transfers `bytes` and reports time and goodput.\n"
                "  Not implemented: SACK, delayed ACKs, timestamps; bbr is a simplified model."),
    .theory = N_("TCP turns the unreliable, unordered packet service of IP into a reliable byte stream "
                 "between two ports. A connection starts with the three-way handshake (SYN, SYN+ACK, ACK) "
                 "in which the two ends choose initial sequence numbers and exchange options such as the "
                 "maximum segment size. Every byte has a sequence number; the receiver answers with "
                 "cumulative acknowledgements (\"I have everything before N\") and buffers segments that "
                 "arrive out of order. A segment that is not acknowledged before the retransmission "
                 "timeout (RTO, computed from the smoothed round-trip time and its variance, doubled at "
                 "each expiry) is sent again; three duplicate ACKs reveal a hole earlier and trigger a "
                 "fast retransmit. The connection ends when each side has sent a FIN and seen it "
                 "acknowledged.\n\n"
                 "Two windows limit the sender. The receive window, advertised in every segment, is "
                 "flow control: it keeps a fast sender from overrunning a slow reader, and when it drops "
                 "to zero the sender probes periodically until it reopens. The congestion window (cwnd) "
                 "is the sender's own estimate of what the network can carry. It grows exponentially in "
                 "slow start up to ssthresh, then linearly in congestion avoidance (additive increase); a "
                 "loss halves it (multiplicative decrease), giving the classic sawtooth. Tahoe restarts "
                 "from one segment after every loss; Reno adds fast recovery; NewReno repairs several "
                 "losses in one window using partial ACKs; CUBIC grows the window as a cubic function of "
                 "the time since the last loss, which suits fast long-distance paths; BBR ignores loss "
                 "and instead models the path, pacing packets at the measured bottleneck rate and probing "
                 "periodically for more. With ECN, routers mark packets instead of dropping them and the "
                 "receiver echoes the mark, so the sender slows down without losing data."),
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
    char fl[48] = "", optbuf[96] = "";
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
