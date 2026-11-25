/*
 * link.c - Emulated links: queue, transmitter (bandwidth), propagation
 *          delay, loss (independent or in bursts), bit errors, link codes,
 *          duplication and reordering. A link whose far end lives in
 *          another process is carried by a UDP tunnel; the same datagrams
 *          feed the monitor.
 */
#include "nl_int.h"
#include "rdt.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Configuration                                                      */
/* ------------------------------------------------------------------ */

void nl_linkcfg_default(nl_linkcfg *c)
{
    memset(c, 0, sizeof(*c));
    c->bw = 10e6;
    c->delay = NL_MS(10);
    c->qcap = 50;
    c->mtu = NL_DEFAULT_MTU;
    strcpy(c->qdisc, "droptail");
    strcpy(c->codec, "fcs");
}

int nl_linkcfg_apply(nl_linkcfg *c, const nl_args *a, char *err, size_t errsz)
{
    int k;

    for (k = 0; k < a->argc; k++) {
        const char *tok = a->argv[k], *eq = strchr(tok, '=');
        char key[24];
        const char *val;
        int ok = 1;

        if (!eq)
            continue;
        snprintf(key, sizeof(key), "%.*s", (int)(eq - tok), tok);
        val = eq + 1;
        if (!strcmp(key, "bw") || !strcmp(key, "rate"))   ok = nl_parse_rate(val, &c->bw);
        else if (!strcmp(key, "delay"))    ok = nl_parse_time(val, &c->delay);
        else if (!strcmp(key, "jitter"))   ok = nl_parse_time(val, &c->jitter);
        else if (!strcmp(key, "loss"))     c->loss = nl_arg_dbl(a, key, 0);
        else if (!strcmp(key, "burst"))    c->burst = atof(val);
        else if (!strcmp(key, "ber"))      c->ber = atof(val);
        else if (!strcmp(key, "corrupt"))  c->corrupt = nl_arg_dbl(a, key, 0);
        else if (!strcmp(key, "dup"))      c->dup = nl_arg_dbl(a, key, 0);
        else if (!strcmp(key, "reorder"))  c->reorder = nl_arg_dbl(a, key, 0);
        else if (!strcmp(key, "queue"))    c->qcap = atoi(val);
        else if (!strcmp(key, "mtu"))      c->mtu = atoi(val);
        else if (!strcmp(key, "qdisc")) {
            if (strcmp(val, "droptail") && !nl_qdisc_find(val)) {
                snprintf(err, errsz, _("unknown queue discipline '%s'"), val);
                return -1;
            }
            snprintf(c->qdisc, sizeof(c->qdisc), "%s", val);
        } else if (!strcmp(key, "codec") || !strcmp(key, "fec")) {
            if (strcmp(val, "fcs") && strcmp(val, "none") && !nl_codec_find(val)) {
                snprintf(err, errsz, _("unknown link code '%s'"), val);
                return -1;
            }
            snprintf(c->codec, sizeof(c->codec), "%s", val);
        } else {
            /* anything else is for the queue discipline */
            size_t len = strlen(c->qargs);
            snprintf(c->qargs + len, sizeof(c->qargs) - len, "%s%s", len ? " " : "", tok);
        }
        if (!ok) {
            snprintf(err, errsz, _("bad value in '%s'"), tok);
            return -1;
        }
    }
    if (c->qcap < 1) c->qcap = 1;
    if (c->qcap > 10000) c->qcap = 10000;
    if (c->mtu < 68) c->mtu = 68;
    if (c->mtu > NL_FRAME_MAX - 64) c->mtu = NL_FRAME_MAX - 64;
    return 0;
}

/* (Re)build queues and queue-discipline state after a configuration change */
void nl_link_reconf(nl_link *l)
{
    const nl_qdisc *qd = nl_qdisc_find(l->cfg.qdisc);
    nl_args a;
    int d, i;

    nl_args_parse(&a, l->cfg.qargs);
    for (d = 0; d < 2; d++) {
        nl_linkdir *q = &l->d[d];

        if (q->qalloc != l->cfg.qcap) {
            nl_qitem *nq = calloc((size_t)l->cfg.qcap, sizeof(*nq));
            int keep = 0;

            if (!nq)
                exit(EXIT_FAILURE);
            for (i = 0; i < q->qlen; i++) {
                nl_qitem *it = &q->q[(q->qhead + i) % q->qalloc];
                if (keep < l->cfg.qcap) {
                    nq[keep++] = *it;
                } else {
                    q->qbytes -= it->p->len;
                    nl_pkt_free(it->p);
                }
            }
            free(q->q);
            q->q = nq;
            q->qalloc = l->cfg.qcap;
            q->qhead = 0;
            q->qlen = keep;
        }
        free(q->qstate);
        q->qstate = qd && qd->create ? qd->create(l, d, &a) : NULL;
    }
}

nl_link *nl_link_add(nl_node *a, int ifa, nl_node *b, int ifb, const nl_linkcfg *cfg)
{
    nl_link *l;

    if (W.nlinks >= NL_MAX_LINKS)
        return NULL;
    l = &W.links[W.nlinks];
    memset(l, 0, sizeof(*l));
    l->idx = W.nlinks++;
    l->n[0] = a;
    l->n[1] = b;
    l->ifx[0] = ifa;
    l->ifx[1] = ifb;
    l->up = 1;
    l->cfg = *cfg;
    a->ifc[ifa].link = l;
    a->ifc[ifa].end = 0;
    b->ifc[ifb].link = l;
    b->ifc[ifb].end = 1;
    nl_link_reconf(l);
    return l;
}

nl_link *nl_link_find(const nl_node *a, const nl_node *b)
{
    int i;

    for (i = 0; i < W.nlinks; i++) {
        nl_link *l = &W.links[i];
        if ((l->n[0] == a && l->n[1] == b) || (l->n[0] == b && l->n[1] == a))
            return l;
    }
    return NULL;
}

void nl_link_set_up(nl_link *l, int up)
{
    int d;

    if (l->up == !!up)
        return;
    l->up = !!up;
    nl_log(NULL, NL_L_SIM, 1, 0, up ? T_OK "● %s %s — %s" : T_ERR "✗ %s %s — %s",
           up ? _("link up:") : _("link down:"), l->n[0]->name, l->n[1]->name);
    if (!up)
        for (d = 0; d < 2; d++) {
            nl_pkt *p;
            while ((p = nl_q_pop(l, d, NULL)) != NULL)
                nl_pkt_free(p);
        }
}

/* ------------------------------------------------------------------ */
/* Queue helpers                                                      */
/* ------------------------------------------------------------------ */

nl_pkt *nl_q_pop(nl_link *l, int dir, nl_time *sojourn)
{
    nl_linkdir *q = &l->d[dir];
    nl_qitem it;

    if (q->qlen == 0)
        return NULL;
    it = q->q[q->qhead];
    q->qhead = (q->qhead + 1) % q->qalloc;
    q->qlen--;
    q->qbytes -= it.p->len;
    if (sojourn)
        *sojourn = W.now - it.t_in;
    return it.p;
}

const nl_qitem *nl_q_peek(const nl_link *l, int dir, int i)
{
    const nl_linkdir *q = &l->d[dir];
    return i >= 0 && i < q->qlen ? &q->q[(q->qhead + i) % q->qalloc] : NULL;
}

static nl_cap *cap_make(nl_link *l, int dir, const nl_pkt *p, int fate, nl_time t0, nl_time t1)
{
    nl_cap *c = nl_cap_new();
    nl_dissect *d = malloc(sizeof(*d));

    c->t0 = t0;
    c->t1 = t1;
    c->link = (short)l->idx;
    c->dir = (short)dir;
    c->from = (short)l->n[dir]->idx;
    c->to = (short)l->n[!dir]->idx;
    c->fate = (uint8_t)fate;
    c->fate_pos = (float)(0.25 + 0.5 * nl_rand01());
    memcpy(&c->pkt, p, offsetof(nl_pkt, data) + p->len);
    if (d) {
        nl_dissect_frame(p->data, p->len, d);
        c->color = d->color;
        snprintf(c->proto, sizeof(c->proto), "%s", d->proto);
        snprintf(c->summary, sizeof(c->summary), "%s", d->summary);
        snprintf(c->addr, sizeof(c->addr), "%s", d->addr);
        free(d);
    }
    return c;
}

void nl_q_drop(nl_link *l, int dir, nl_pkt *p, const char *why)
{
    nl_cap *c = cap_make(l, dir, p, NL_FATE_QDROP, W.now, W.now);

    l->d[dir].qdrops++;
    nl_log(l->n[dir], NL_L_LINK, 2, p->id, T_ERR "✗ %s" T_RESET " %s → %s: %s  " T_DIM "%s",
           why ? why : _("queue full"), l->n[dir]->name, l->n[!dir]->name, c->proto, c->summary);
    if (W.mon_sock >= 0 && W.mode == NL_MODE_NODE)
        nl_mon_send_cap(c);
    nl_pkt_free(p);
}

int nl_ecn_capable(const nl_pkt *p)
{
    return p->len >= NL_ETH_HLEN + NL_IP_HLEN && nl_get16(p->data + 12) == NL_ETH_IP &&
           (p->data[NL_ETH_HLEN + 1] & 3) != 0;
}

int nl_ecn_mark(nl_pkt *p)
{
    uint8_t *ip = p->data + NL_ETH_HLEN;

    if (!nl_ecn_capable(p))
        return 0;
    ip[1] |= 3;
    nl_put16(ip + 10, 0);
    nl_put16(ip + 10, nl_cksum(ip, (ip[0] & 15) * 4, 0));
    return 1;
}

/* ------------------------------------------------------------------ */
/* Tunnels and monitor datagrams                                      */
/* ------------------------------------------------------------------ */

enum { DG_FRAME = 1, DG_LOG = 2, DG_CAP = 3, DG_HELLO = 4, DG_START = 5 };
#define DG_MAGIC 0x4E4C5431u            /* "NLT1" */
#define DG_HDR   24

static struct sockaddr_in g_peer[NL_MAX_NODES];
static int                g_peer_ok[NL_MAX_NODES];

static const struct sockaddr_in *peer_addr(const nl_node *n)
{
    if (!g_peer_ok[n->idx]) {
        if (rdt_resolve(n->under_host[0] ? n->under_host : "127.0.0.1", n->under_port, &g_peer[n->idx]) != 0)
            return NULL;
        g_peer_ok[n->idx] = 1;
    }
    return &g_peer[n->idx];
}

static int any_local_sock(void)
{
    int i;

    for (i = 0; i < W.nnodes; i++)
        if (W.nodes[i].local && W.nodes[i].sock >= 0)
            return W.nodes[i].sock;
    return -1;
}

static size_t dg_head(uint8_t *b, int type, int link, int dir, uint32_t id, int hops, int fate,
                      nl_time dur, int len, int aux)
{
    nl_put32(b, DG_MAGIC);
    b[4] = (uint8_t)type;
    b[5] = (uint8_t)dir;
    nl_put16(b + 6, (uint16_t)link);
    nl_put32(b + 8, id);
    b[12] = (uint8_t)hops;
    b[13] = (uint8_t)fate;
    nl_put16(b + 14, (uint16_t)len);
    nl_put32(b + 16, (uint32_t)dur);
    nl_put32(b + 20, (uint32_t)aux);
    return DG_HDR;
}

void nl_mon_send_log(const nl_logrec *r)
{
    uint8_t b[DG_HDR + sizeof(r->text)];
    size_t tl = strlen(r->text), n;

    if (W.mon_sock < 0)
        return;
    n = dg_head(b, DG_LOG, r->node, r->layer, r->pkt_id, r->level, 0, 0, (int)tl, 0);
    memcpy(b + n, r->text, tl);
    sendto(W.mon_sock, b, n + tl, 0, (const struct sockaddr *)&W.mon_addr, sizeof(W.mon_addr));
}

void nl_mon_send_cap(const nl_cap *c)
{
    uint8_t b[DG_HDR + NL_FRAME_MAX];
    size_t n;

    if (W.mon_sock < 0 || c->inbound)
        return;
    n = dg_head(b, DG_CAP, c->link, c->dir, c->pkt.id, c->pkt.hops, c->fate, c->t1 - c->t0, c->pkt.len,
                (int)(c->fate_pos * 1000));
    memcpy(b + n, c->pkt.data, c->pkt.len);
    sendto(W.mon_sock, b, n + c->pkt.len, 0, (const struct sockaddr *)&W.mon_addr, sizeof(W.mon_addr));
}

int nl_net_open(void)
{
    struct sockaddr_in a;
    int i;

    /* the monitor, or a node process that also collects the reports (hub) */
    if (W.mode == NL_MODE_MONITOR || (W.mode == NL_MODE_NODE && W.hub)) {
        if (rdt_resolve("0.0.0.0", W.baseport - 1, &a) != 0 || (W.mon_listen = rdt_udp_socket(&a)) < 0) {
            fprintf(stderr, "hoplab: %s %d: %s\n", _("cannot listen on UDP port"), W.baseport - 1, strerror(errno));
            return -1;
        }
        W.waiting = 1;
    }
    if (W.mode != NL_MODE_NODE)
        return 0;
    for (i = 0; i < W.nnodes; i++) {
        nl_node *n = &W.nodes[i];
        if (!n->local)
            continue;
        if (rdt_resolve("0.0.0.0", n->under_port, &a) != 0 || (n->sock = rdt_udp_socket(&a)) < 0) {
            fprintf(stderr, "hoplab: %s: %s %d: %s\n", n->name, _("cannot listen on UDP port"),
                    n->under_port, strerror(errno));
            return -1;
        }
    }
    if (W.hub)
        W.mon_sock = -1;                /* we are the collector */
    else if (W.mon_sock == -2)
        W.mon_sock = any_local_sock();
    W.waiting = W.hub || W.mon_sock >= 0;
    return 0;
}

int nl_net_fds(int *fds, int max)
{
    int i, k = 0;

    if (W.mon_listen >= 0 && k < max)
        fds[k++] = W.mon_listen;
    for (i = 0; i < W.nnodes && k < max; i++)
        if (W.nodes[i].local && W.nodes[i].sock >= 0)
            fds[k++] = W.nodes[i].sock;
    return k;
}

/* ------------------------------------------------------------------ */
/* Transmission                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    nl_link *l;
    int      dir;
    nl_pkt  *p;
} arrival;

static void on_arrival(void *ctx)
{
    arrival *a = ctx;
    nl_node *dst = a->l->n[!a->dir];

    if (a->l->up && dst->up && dst->ifc[a->l->ifx[!a->dir]].up) {
        a->p->hops++;
        nl_stack_rx(dst, a->l->ifx[!a->dir], a->p);
    } else {
        nl_pkt_free(a->p);
    }
    free(a);
}

static void schedule_arrival(nl_link *l, int dir, nl_pkt *p, nl_time delay)
{
    arrival *a = malloc(sizeof(*a));

    if (!a) {
        nl_pkt_free(p);
        return;
    }
    a->l = l;
    a->dir = dir;
    a->p = p;
    nl_after(NULL, delay, on_arrival, a);
}

/* Flip bits of buf according to the link configuration; returns how many */
static int flip_bits(const nl_linkcfg *c, uint8_t *buf, int n)
{
    int flips = 0;
    long nbits = (long)n * 8, pos;

    if (n <= 0)
        return 0;
    if (c->corrupt > 0 && nl_rand01() < c->corrupt) {
        pos = (long)nl_rand((unsigned)nbits);
        buf[pos / 8] ^= (uint8_t)(0x80 >> (pos % 8));
        flips++;
    }
    if (c->ber > 0) {
        /* jump from one error to the next: gaps are geometric */
        pos = (long)nl_rand_exp(1.0 / c->ber);
        while (pos < nbits) {
            buf[pos / 8] ^= (uint8_t)(0x80 >> (pos % 8));
            flips++;
            pos += 1 + (long)nl_rand_exp(1.0 / c->ber);
        }
    }
    return flips;
}

static int lose(nl_link *l, int dir)
{
    const nl_linkcfg *c = &l->cfg;
    nl_linkdir *q = &l->d[dir];

    if (c->loss <= 0)
        return 0;
    if (c->burst <= 1.0)
        return nl_rand01() < c->loss;
    /* Gilbert-Elliott: every frame is lost in the bad state; the mean
     * sojourn there is `burst` frames and the long-run loss is `loss` */
    if (q->ge_bad) {
        if (nl_rand01() < 1.0 / c->burst)
            q->ge_bad = 0;
    } else if (c->loss < 1.0 && nl_rand01() < c->loss / (c->burst * (1.0 - c->loss))) {
        q->ge_bad = 1;
    }
    return q->ge_bad;
}

static void start_tx(nl_link *l, int dir);

static void on_tx_done(void *ctx)
{
    nl_linkdir *q = ctx;
    nl_link *l = NULL;
    int i, dir = 0;

    q->sending = 0;
    for (i = 0; i < W.nlinks && !l; i++)
        for (dir = 0; dir < 2; dir++)
            if (&W.links[i].d[dir] == q) {
                l = &W.links[i];
                break;
            }
    if (l)
        start_tx(l, dir);
}

static void start_tx(nl_link *l, int dir)
{
    const nl_linkcfg *c = &l->cfg;
    const nl_qdisc *qd = nl_qdisc_find(c->qdisc);
    const nl_codec *codec = nl_codec_find(c->codec);
    nl_linkdir *q = &l->d[dir];
    nl_node *dst = l->n[!dir];
    nl_pkt *p, *orig;
    nl_time ser, prop;
    int fate = NL_FATE_OK, wire_len, flips = 0;
    nl_cap *cap;

    if (q->sending)
        return;
    p = qd && qd->dequeue ? qd->dequeue(q->qstate, l, dir) : nl_q_pop(l, dir, NULL);
    if (!p)
        return;

    orig = nl_pkt_clone(p);
    wire_len = p->len + 4;                              /* frame check sequence */
    if (lose(l, dir)) {
        fate = NL_FATE_LOST;
    } else if (codec) {
        static uint8_t enc[NL_FRAME_MAX * 4], dec[NL_FRAME_MAX * 2];
        int en = codec->encode(p->data, p->len, enc, (int)sizeof(enc)), dn, fixed = 0;

        if (en > 0) {
            wire_len = en;
            flips = flip_bits(c, enc, en);
            dn = codec->decode(enc, en, dec, (int)sizeof(dec), &fixed);
            if (dn < 0 || dn > NL_FRAME_MAX) {
                fate = NL_FATE_FCS;
            } else {
                int same = dn == p->len && memcmp(dec, orig->data, (size_t)dn) == 0;
                memcpy(p->data, dec, (size_t)dn);
                p->len = (uint16_t)dn;
                fate = same ? (flips ? NL_FATE_FIXED : NL_FATE_OK) : NL_FATE_CORRUPT;
            }
        }
    } else {
        flips = flip_bits(c, p->data, p->len);
        if (flips)
            fate = strcmp(c->codec, "none") == 0 ? NL_FATE_CORRUPT : NL_FATE_FCS;
    }

    ser = c->bw > 0 ? (nl_time)(wire_len * 8.0 * 1e6 / c->bw) : 0;
    prop = c->delay + (c->jitter > 0 ? (nl_time)(nl_rand01() * (double)c->jitter) : 0);
    if (c->reorder > 0 && nl_rand01() < c->reorder)
        prop += c->delay + ser * 3;

    q->tx++;
    q->tx_bytes += (uint64_t)wire_len;
    l->n[dir]->ifc[l->ifx[dir]].tx++;
    l->n[dir]->ifc[l->ifx[dir]].tx_bytes += (uint64_t)wire_len;
    cap = cap_make(l, dir, orig, fate, W.now, W.now + ser + prop);
    switch (fate) {
    case NL_FATE_LOST:
        q->lost++;
        nl_log(l->n[dir], NL_L_PHY, 2, p->id, T_ERR "✗ %s" T_RESET " %s → %s: %s  " T_DIM "%s",
               _("lost on the link"), l->n[dir]->name, dst->name, cap->proto, cap->summary);
        break;
    case NL_FATE_FCS:
        q->corrupted++;
        q->fcs_drops++;
        nl_log(dst, NL_L_LINK, 2, p->id, T_WARN "↯ %s" T_RESET " (%d bit) %s → %s: %s — %s",
               _("bit errors"), flips, l->n[dir]->name, dst->name, cap->proto,
               _("detected, frame discarded"));
        break;
    case NL_FATE_FIXED:
        q->corrupted++;
        q->fixed++;
        nl_log(dst, NL_L_LINK, 2, p->id, T_OK "✓ %s" T_RESET " (%d bit) %s → %s: %s — %s (%s)",
               _("bit errors"), flips, l->n[dir]->name, dst->name, cap->proto,
               _("corrected by the link code"), c->codec);
        break;
    case NL_FATE_CORRUPT:
        q->corrupted++;
        q->undetected++;
        nl_log(dst, NL_L_LINK, 2, p->id, T_ERR "↯ %s" T_RESET " (%d bit) %s → %s: %s — %s",
               _("bit errors"), flips, l->n[dir]->name, dst->name, cap->proto,
               _("NOT detected by the link: delivered damaged"));
        break;
    default:
        break;
    }
    if (W.mon_sock >= 0 && W.mode == NL_MODE_NODE)
        nl_mon_send_cap(cap);
    nl_pkt_free(orig);

    if (fate == NL_FATE_LOST || fate == NL_FATE_FCS) {
        nl_pkt_free(p);
    } else if (dst->local) {
        if (c->dup > 0 && nl_rand01() < c->dup)
            schedule_arrival(l, dir, nl_pkt_clone(p), ser + prop + ser / 2 + 1);
        schedule_arrival(l, dir, p, ser + prop);
    } else {
        const struct sockaddr_in *to = peer_addr(dst);
        int sock = l->n[dir]->sock >= 0 ? l->n[dir]->sock : any_local_sock();
        uint8_t b[DG_HDR + NL_FRAME_MAX];
        size_t n = dg_head(b, DG_FRAME, l->idx, dir, p->id, p->hops, fate, ser + prop, p->len, 0);

        memcpy(b + n, p->data, p->len);
        if (to && sock >= 0)
            sendto(sock, b, n + p->len, 0, (const struct sockaddr *)to, sizeof(*to));
        nl_pkt_free(p);
    }

    q->sending = 1;
    q->busy_until = W.now + ser;
    nl_after(NULL, ser, on_tx_done, q);
}

void nl_link_kick(nl_link *l, int dir)
{
    if (l->up)
        start_tx(l, dir);
}

void nl_link_tx(nl_node *n, int ifx, nl_pkt *p)
{
    nl_iface *f = &n->ifc[ifx];
    nl_link *l = f->link;
    const nl_qdisc *qd;
    nl_linkdir *q;
    int dir;

    if (!l || !nl_if_usable(n, ifx)) {
        if (l) {
            nl_cap *c = cap_make(l, f->end, p, NL_FATE_DOWN, W.now, W.now);
            if (W.mon_sock >= 0 && W.mode == NL_MODE_NODE)
                nl_mon_send_cap(c);
        }
        nl_pkt_free(p);
        return;
    }
    dir = f->end;
    q = &l->d[dir];
    qd = nl_qdisc_find(l->cfg.qdisc);
    if (qd && qd->enqueue && qd->enqueue(q->qstate, l, dir, p) == NL_Q_DROP) {
        nl_q_drop(l, dir, p, qd->name);
        return;
    }
    if (q->qlen >= q->qalloc) {
        nl_q_drop(l, dir, p, NULL);
        return;
    }
    q->q[(q->qhead + q->qlen) % q->qalloc] = (nl_qitem){ p, W.now };
    q->qlen++;
    q->qbytes += p->len;
    start_tx(l, dir);
}

/* ------------------------------------------------------------------ */
/* Start-up rendezvous                                                */
/* ------------------------------------------------------------------ */

/* The processes of a lab are started one after the other, but virtual time
 * must begin together or the first frames are sent to nobody. A process
 * that reports to a monitor holds its clock and says HELLO (listing its
 * nodes) until the monitor answers START; the monitor (or hub) answers
 * when it has heard every node it does not run itself. */

static int                g_seen[NL_MAX_NODES];
static struct sockaddr_in g_hello_from[NL_MAX_NODES];
static long long          g_last_hello;

int nl_net_missing(char *buf, size_t n)
{
    int i, missing = 0;
    size_t len = 0;

    if (buf && n)
        buf[0] = '\0';
    for (i = 0; i < W.nnodes; i++)
        if (!W.nodes[i].local && !g_seen[i]) {
            missing++;
            if (buf && len < n)
                len += (size_t)snprintf(buf + len, n - len, "%s%s", len ? " " : "", W.nodes[i].name);
        }
    return missing;
}

static void send_start(const struct sockaddr_in *to)
{
    uint8_t b[DG_HDR];
    int sock = W.mon_listen >= 0 ? W.mon_listen : any_local_sock();

    dg_head(b, DG_START, 0, 0, 0, 0, 0, 0, 0, 0);
    if (sock >= 0)
        sendto(sock, b, DG_HDR, 0, (const struct sockaddr *)to, sizeof(*to));
}

void nl_net_start(void)
{
    int i;

    if (!W.waiting)
        return;
    W.waiting = 0;
    if (W.mon_listen >= 0)
        for (i = 0; i < W.nnodes; i++)
            if (g_seen[i])
                send_start(&g_hello_from[i]);
}

void nl_net_sync(void)
{
    long long now = rdt_now_ms();

    if (!W.waiting)
        return;
    if (W.mon_listen >= 0) {            /* collector: start when everybody is here */
        if (nl_net_missing(NULL, 0) == 0)
            nl_net_start();
        return;
    }
    if (W.mon_sock < 0) {               /* nobody to wait for */
        W.waiting = 0;
        return;
    }
    if (now - g_last_hello >= 200) {
        uint8_t b[DG_HDR + NL_MAX_NODES];
        int i, k = 0;

        g_last_hello = now;
        for (i = 0; i < W.nnodes; i++)
            if (W.nodes[i].local)
                b[DG_HDR + k++] = (uint8_t)i;
        dg_head(b, DG_HELLO, 0, 0, 0, 0, 0, 0, k, 0);
        sendto(W.mon_sock, b, (size_t)(DG_HDR + k), 0, (const struct sockaddr *)&W.mon_addr, sizeof(W.mon_addr));
    }
}

/* ------------------------------------------------------------------ */
/* Receiving datagrams                                                */
/* ------------------------------------------------------------------ */

static void dg_input(const uint8_t *b, ssize_t n, const struct sockaddr_in *from)
{
    int type, dir, link, hops, fate, len, aux;
    uint32_t id;
    nl_time dur;

    if (n < DG_HDR || nl_get32(b) != DG_MAGIC)
        return;
    type = b[4];
    dir = b[5];
    link = (int16_t)nl_get16(b + 6);
    id = nl_get32(b + 8);
    hops = b[12];
    fate = b[13];
    len = nl_get16(b + 14);
    dur = nl_get32(b + 16);
    aux = (int)nl_get32(b + 20);
    if (len > n - DG_HDR)
        return;
    if (type == DG_START) {
        W.waiting = 0;
        return;
    }
    if (type == DG_HELLO) {
        int k;
        if (W.mon_listen < 0)
            return;
        for (k = 0; k < len; k++)
            if (b[DG_HDR + k] < W.nnodes) {
                g_seen[b[DG_HDR + k]] = 1;
                g_hello_from[b[DG_HDR + k]] = *from;
            }
        if (!W.waiting)
            send_start(from);           /* a late or restarted process */
        return;
    }

    if (type == DG_LOG && W.mon_listen >= 0) {
        nl_logrec *r = &W.logs[W.log_seq % NL_LOG_RING];

        r->seq = W.log_seq++;
        r->t = W.now;
        r->node = (short)(link >= 0 && link < W.nnodes ? link : -1);
        r->layer = (uint8_t)dir;
        r->level = (uint8_t)hops;
        r->pkt_id = id;
        snprintf(r->text, sizeof(r->text), "%.*s", len, (const char *)b + DG_HDR);
        return;
    }
    if (link < 0 || link >= W.nlinks || dir > 1 || len > NL_FRAME_MAX)
        return;
    if (type == DG_CAP && W.mon_listen >= 0) {
        nl_pkt *p = nl_pkt_new(id);
        nl_cap *c;

        p->len = (uint16_t)len;
        p->hops = (uint8_t)hops;
        memcpy(p->data, b + DG_HDR, (size_t)len);
        c = cap_make(&W.links[link], dir, p, fate, W.now, W.now + dur);
        c->fate_pos = (float)(aux / 1000.0);
        nl_pkt_free(p);
    } else if (type == DG_FRAME && W.mode == NL_MODE_NODE) {
        nl_link *l = &W.links[link];
        nl_pkt *p;
        nl_cap *c;

        if (!l->n[!dir]->local)
            return;
        p = nl_pkt_new(id);
        p->len = (uint16_t)len;
        p->hops = (uint8_t)hops;
        memcpy(p->data, b + DG_HDR, (size_t)len);
        c = cap_make(l, dir, p, fate, W.now, W.now + dur);
        c->inbound = 1;
        l->d[dir].tx++;
        schedule_arrival(l, dir, p, dur);
    }
}

void nl_net_poll(void)
{
    int fds[NL_MAX_NODES + 1], nf = nl_net_fds(fds, NL_MAX_NODES + 1), i;
    uint8_t b[DG_HDR + NL_FRAME_MAX + 64];

    for (i = 0; i < nf; i++)
        for (;;) {
            struct sockaddr_in from;
            socklen_t fl = sizeof(from);
            ssize_t n = recvfrom(fds[i], b, sizeof(b), 0, (struct sockaddr *)&from, &fl);
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                break;
            }
            dg_input(b, n, &from);
        }
}
