/*
 * ospf.c - Link-state routing in the style of OSPFv2 (RFC 2328), single area,
 *          carried directly over IP protocol 89.
 *
 * What is real: Hello with neighbor discovery, two-way check and dead
 * interval; router-LSAs with sequence numbers; reliable flooding with
 * acknowledgements and retransmission; a link-state database identical on
 * every router; Dijkstra's shortest-path-first with a hold-down; costs
 * derived from the link bandwidth.
 *
 * What is simplified: the packet layouts are OSPF-like but not bit-exact;
 * there is no designated router (a LAN with several routers is treated as a
 * mesh of point-to-point adjacencies); a new adjacency synchronizes by
 * sending the whole database instead of the DBD/LSR exchange; LSAs are not
 * aged out.
 */
#include "nl_int.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OSPF_GROUP  NL_IP(224, 0, 0, 5)
#define OSPF_COLOR  43
#define MAX_NBR     24
#define MAX_LSA     64
#define MAX_LNK     24
#define MAX_INST    128
#define HDR         16
#define INF         0x3fffffff

enum { OP_HELLO = 1, OP_LSU = 4, OP_ACK = 5 };
enum { LT_P2P = 1, LT_STUB = 3 };
enum { NS_DOWN, NS_INIT, NS_FULL };

typedef struct { uint8_t type; uint16_t cost; nl_ip id, data; } olink;

typedef struct {
    int      used;
    nl_ip    rid;
    uint32_t seq;
    nl_time  rcvd;
    int      nl;
    olink    l[MAX_LNK];
} lsa;

typedef struct {
    int      state;
    nl_ip    rid, addr;
    int      ifx;
    nl_time  last;
    struct { nl_ip rid; uint32_t seq; } rx[MAX_LSA];   /* flooded, not yet acknowledged */
    int      nrx;
} nbr;

typedef struct {
    nl_node *n;
    nl_ip    rid;
    nl_time  hello, dead, rxmt, spf_delay;
    int      cost_all, cost_if[NL_MAX_IF], carrier;
    nbr      nb[MAX_NBR];
    lsa      db[MAX_LSA];
    uint32_t seq;
    nl_ev    hello_ev, rx_ev, spf_ev;
    struct { nl_ip dst; int plen; } inst[MAX_INST];
    int      ninst;
    unsigned spf_runs, flooded, rexmits;
    nl_time  last_spf;
} ostate;

static const nl_module ospf_mod;
static void originate(ostate *s, int force);

/* ---- helpers ---- */

static int if_cost(const ostate *s, int ifx)
{
    const nl_link *l = s->n->ifc[ifx].link;
    double c;

    if (s->cost_if[ifx] > 0)
        return s->cost_if[ifx];
    if (s->cost_all > 0)
        return s->cost_all;
    c = l && l->cfg.bw > 0 ? 1e8 / l->cfg.bw : 1;
    return c < 1 ? 1 : c > 65535 ? 65535 : (int)(c + 0.5);
}

static lsa *db_find(ostate *s, nl_ip rid, int create)
{
    lsa *slot = NULL;
    int i;

    for (i = 0; i < MAX_LSA; i++) {
        if (s->db[i].used && s->db[i].rid == rid)
            return &s->db[i];
        if (!s->db[i].used && !slot)
            slot = &s->db[i];
    }
    if (create && slot) {
        memset(slot, 0, sizeof(*slot));
        slot->used = 1;
        slot->rid = rid;
    }
    return create ? slot : NULL;
}

static nbr *nbr_find(ostate *s, nl_ip rid, int ifx, int create)
{
    nbr *slot = NULL;
    int i;

    for (i = 0; i < MAX_NBR; i++) {
        if (s->nb[i].state != NS_DOWN && s->nb[i].rid == rid && s->nb[i].ifx == ifx)
            return &s->nb[i];
        if (s->nb[i].state == NS_DOWN && !slot)
            slot = &s->nb[i];
    }
    if (create && slot) {
        memset(slot, 0, sizeof(*slot));
        slot->rid = rid;
        slot->ifx = ifx;
    }
    return create ? slot : NULL;
}

static int full_count(const ostate *s)
{
    int i, c = 0;

    for (i = 0; i < MAX_NBR; i++)
        c += s->nb[i].state == NS_FULL;
    return c;
}

static void send_pkt(ostate *s, int type, int ifx, nl_ip dst, uint8_t *b, int len)
{
    nl_iptx tx;

    b[0] = 2;
    b[1] = (uint8_t)type;
    nl_put16(b + 2, (uint16_t)len);
    nl_put32(b + 4, s->rid);
    nl_put32(b + 8, 0);                 /* area 0 */
    nl_put16(b + 12, 0);
    nl_put16(b + 14, 0);
    nl_put16(b + 12, nl_cksum(b, len, 0));
    memset(&tx, 0, sizeof(tx));
    tx.dst = dst;
    tx.proto = NL_PROTO_OSPF;
    tx.ttl = 1;
    tx.ifx = ifx;
    tx.use_if = 1;
    nl_ip_send(s->n, &tx, b, len);
}

static int lsa_put(const lsa *a, uint8_t *b)
{
    int i;

    nl_put32(b, a->rid);
    nl_put32(b + 4, a->seq);
    nl_put16(b + 8, (uint16_t)((nl_now() - a->rcvd) / NL_SEC(1)));
    nl_put16(b + 10, (uint16_t)a->nl);
    for (i = 0; i < a->nl; i++) {
        uint8_t *e = b + 12 + i * 12;
        e[0] = a->l[i].type;
        e[1] = 0;
        nl_put16(e + 2, a->l[i].cost);
        nl_put32(e + 4, a->l[i].id);
        nl_put32(e + 8, a->l[i].data);
    }
    return 12 + a->nl * 12;
}

/* Send one LSA to a neighbor and remember that it must acknowledge it */
static void send_lsa(ostate *s, nbr *nb, const lsa *a, int track)
{
    uint8_t b[HDR + 4 + 12 + MAX_LNK * 12];
    int len = HDR + 4, i;

    nl_put32(b + HDR, 1);
    len += lsa_put(a, b + len);
    send_pkt(s, OP_LSU, nb->ifx, nb->addr, b, len);
    if (!track)
        return;
    for (i = 0; i < nb->nrx; i++)
        if (nb->rx[i].rid == a->rid)
            break;
    if (i == nb->nrx) {
        if (nb->nrx >= MAX_LSA)
            return;
        nb->nrx++;
    }
    nb->rx[i].rid = a->rid;
    nb->rx[i].seq = a->seq;
}

static void flood(ostate *s, const lsa *a, const nbr *except)
{
    int i;

    for (i = 0; i < MAX_NBR; i++)
        if (s->nb[i].state == NS_FULL && &s->nb[i] != except) {
            send_lsa(s, &s->nb[i], a, 1);
            s->flooded++;
            nl_count(s->n, "ospf.lsa_flooded", 1);
        }
}

/* ---- shortest path first ---- */

static const olink *link_to(const lsa *a, nl_ip rid)
{
    int i;

    for (i = 0; i < a->nl; i++)
        if (a->l[i].type == LT_P2P && a->l[i].id == rid)
            return &a->l[i];
    return NULL;
}

static void spf_run(void *ctx)
{
    ostate *s = ctx;
    nl_node *n = s->n;
    int dist[MAX_LSA], done[MAX_LSA], fif[MAX_LSA], i, k, u, root = -1, reach = 0;
    nl_ip fgw[MAX_LSA];
    struct { nl_ip dst; int plen, cost, ifx; nl_ip gw; } rt[MAX_INST];
    int nrt = 0;

    s->spf_ev = 0;
    for (i = 0; i < MAX_LSA; i++) {
        dist[i] = INF;
        done[i] = 0;
        if (s->db[i].used && s->db[i].rid == s->rid)
            root = i;
    }
    if (root < 0)
        return;
    dist[root] = 0;
    for (;;) {
        const lsa *a;

        u = -1;
        for (i = 0; i < MAX_LSA; i++)
            if (s->db[i].used && !done[i] && dist[i] < INF && (u < 0 || dist[i] < dist[u]))
                u = i;
        if (u < 0)
            break;
        done[u] = 1;
        reach++;
        a = &s->db[u];
        for (k = 0; k < a->nl; k++) {
            const olink *l = &a->l[k];
            lsa *v;
            int vi, alt, hif = -1;
            nl_ip hgw = 0;

            if (l->type != LT_P2P || !(v = db_find(s, l->id, 0)) || !link_to(v, a->rid))
                continue;               /* a link counts only if both ends report it */
            vi = (int)(v - s->db);
            alt = dist[u] + l->cost;
            if (done[vi] || alt >= dist[vi])
                continue;
            if (u == root) {
                for (i = 0; i < MAX_NBR; i++)
                    if (s->nb[i].state == NS_FULL && s->nb[i].rid == l->id &&
                        n->ifc[s->nb[i].ifx].ip == l->data) {
                        hif = s->nb[i].ifx;
                        hgw = s->nb[i].addr;
                    }
                if (hif < 0)
                    continue;
            } else {
                hif = fif[u];
                hgw = fgw[u];
            }
            dist[vi] = alt;
            fif[vi] = hif;
            fgw[vi] = hgw;
        }
    }
    /* networks attached to every reachable router */
    for (u = 0; u < MAX_LSA; u++) {
        const lsa *a = &s->db[u];

        if (!a->used || !done[u] || u == root)
            continue;
        for (k = 0; k < a->nl; k++) {
            const olink *l = &a->l[k];
            int plen = 0, cost = dist[u] + l->cost, mine = 0;
            nl_ip m = l->data;

            if (l->type != LT_STUB)
                continue;
            while (m & 0x80000000u) {
                plen++;
                m <<= 1;
            }
            for (i = 0; i < n->nif; i++)
                if (n->ifc[i].ip && nl_if_usable(n, i) && n->ifc[i].plen == plen &&
                    (n->ifc[i].ip & nl_mask(plen)) == l->id)
                    mine = 1;
            if (mine)
                continue;
            for (i = 0; i < nrt; i++)
                if (rt[i].dst == l->id && rt[i].plen == plen)
                    break;
            if (i == nrt) {
                if (nrt >= MAX_INST)
                    continue;
                nrt++;
            } else if (rt[i].cost <= cost) {
                continue;
            }
            rt[i].dst = l->id;
            rt[i].plen = plen;
            rt[i].cost = cost;
            rt[i].ifx = fif[u];
            rt[i].gw = fgw[u];
        }
    }
    for (i = 0; i < s->ninst; i++) {
        for (k = 0; k < nrt; k++)
            if (rt[k].dst == s->inst[i].dst && rt[k].plen == s->inst[i].plen)
                break;
        if (k == nrt)
            nl_route_del(n, s->inst[i].dst, s->inst[i].plen, NL_RT_OSPF);
    }
    for (i = 0; i < nrt; i++) {
        nl_route_set(n, rt[i].dst, rt[i].plen, rt[i].gw, rt[i].ifx, rt[i].cost, NL_RT_OSPF);
        s->inst[i].dst = rt[i].dst;
        s->inst[i].plen = rt[i].plen;
    }
    s->ninst = nrt;
    s->spf_runs++;
    s->last_spf = nl_now();
    nl_count(n, "ospf.spf_runs", 1);
    nl_count_set(n, "ospf.routes", nrt);
    nl_log(n, NL_L_NET, 2, 0, T_OK "OSPF" T_RESET " SPF #%u: %d %s, %d %s", s->spf_runs, reach,
           _("routers in the shortest-path tree"), nrt, _("routes"));
}

static void spf_schedule(ostate *s)
{
    if (!s->spf_ev)
        s->spf_ev = nl_after(s->n, s->spf_delay, spf_run, s);
}

/* ---- our own LSA ---- */

static void originate(ostate *s, int force)
{
    nl_node *n = s->n;
    lsa nu, *cur;
    int i;

    memset(&nu, 0, sizeof(nu));
    for (i = 0; i < MAX_NBR && nu.nl < MAX_LNK; i++)
        if (s->nb[i].state == NS_FULL) {
            olink *l = &nu.l[nu.nl++];
            l->type = LT_P2P;
            l->id = s->nb[i].rid;
            l->data = n->ifc[s->nb[i].ifx].ip;
            l->cost = (uint16_t)if_cost(s, s->nb[i].ifx);
        }
    for (i = 0; i < n->nif && nu.nl < MAX_LNK; i++)
        if (n->ifc[i].ip && nl_if_usable(n, i)) {
            olink *l = &nu.l[nu.nl++];
            l->type = LT_STUB;
            l->id = n->ifc[i].ip & nl_mask(n->ifc[i].plen);
            l->data = nl_mask(n->ifc[i].plen);
            l->cost = (uint16_t)if_cost(s, i);
        }
    cur = db_find(s, s->rid, 1);
    if (!cur)
        return;
    if (!force && cur->seq && cur->nl == nu.nl && memcmp(cur->l, nu.l, sizeof(olink) * (size_t)nu.nl) == 0)
        return;
    cur->nl = nu.nl;
    memcpy(cur->l, nu.l, sizeof(nu.l));
    cur->seq = ++s->seq;
    cur->rcvd = nl_now();
    nl_log(n, NL_L_NET, 2, 0, T_OK "OSPF" T_RESET " %s seq=%u: %d %s", _("originating router-LSA"), cur->seq,
           cur->nl, _("links"));
    flood(s, cur, NULL);
    spf_schedule(s);
}

/* ---- neighbors ---- */

static void nbr_down(ostate *s, nbr *nb, const char *why)
{
    int was_full = nb->state == NS_FULL;

    nl_log(s->n, NL_L_NET, 1, 0, T_WARN "OSPF" T_RESET " %s %s (%s): %s", _("neighbor"), nl_ip_str(nb->rid),
           s->n->ifc[nb->ifx].name, why);
    nb->state = NS_DOWN;
    nl_count_set(s->n, "ospf.neighbors", full_count(s));
    if (was_full)
        originate(s, 0);
}

static void send_hello(ostate *s, int ifx)
{
    uint8_t b[HDR + 12 + MAX_NBR * 4];
    int len = HDR + 12, i;

    nl_put32(b + HDR, nl_mask(s->n->ifc[ifx].plen));
    nl_put32(b + HDR + 4, (uint32_t)(s->hello / 1000));     /* milliseconds here, seconds in real OSPF */
    nl_put32(b + HDR + 8, (uint32_t)(s->dead / 1000));
    for (i = 0; i < MAX_NBR; i++)
        if (s->nb[i].state != NS_DOWN && s->nb[i].ifx == ifx) {
            nl_put32(b + len, s->nb[i].rid);
            len += 4;
        }
    send_pkt(s, OP_HELLO, ifx, OSPF_GROUP, b, len);
}

static void hello_tick(void *ctx)
{
    ostate *s = ctx;
    nl_node *n = s->n;
    int i;

    for (i = 0; i < MAX_NBR; i++) {
        nbr *nb = &s->nb[i];
        if (nb->state == NS_DOWN)
            continue;
        if (s->carrier && !nl_if_usable(n, nb->ifx))
            nbr_down(s, nb, _("interface down"));
        else if (nl_now() - nb->last > s->dead)
            nbr_down(s, nb, _("dead interval expired: no Hello heard"));
    }
    for (i = 0; i < n->nif; i++)
        if (n->ifc[i].ip && nl_if_usable(n, i))
            send_hello(s, i);
    originate(s, 0);                    /* interfaces may have come or gone */
    s->hello_ev = nl_after(n, s->hello, hello_tick, s);
}

static void rxmt_tick(void *ctx)
{
    ostate *s = ctx;
    int i, k;

    for (i = 0; i < MAX_NBR; i++) {
        nbr *nb = &s->nb[i];
        if (nb->state != NS_FULL)
            continue;
        for (k = 0; k < nb->nrx; ) {
            lsa *a = db_find(s, nb->rx[k].rid, 0);
            if (!a || a->seq != nb->rx[k].seq) {
                nb->rx[k] = nb->rx[--nb->nrx];
                continue;
            }
            nl_log(s->n, NL_L_NET, 2, 0, T_WARN "OSPF" T_RESET " %s %s seq=%u → %s", _("no ack: retransmitting LSA"),
                   nl_ip_str(a->rid), a->seq, nl_ip_str(nb->rid));
            s->rexmits++;
            nl_count(s->n, "ospf.retransmits", 1);
            send_lsa(s, nb, a, 0);
            k++;
        }
    }
    s->rx_ev = nl_after(s->n, s->rxmt, rxmt_tick, s);
}

/* ---- input ---- */

static void rx_hello(ostate *s, const nl_ipmeta *m, nl_ip rid, const uint8_t *b, int len)
{
    nl_node *n = s->n;
    nbr *nb;
    int i, sees_me = 0, fresh;

    if (len < 12)
        return;
    if ((nl_time)nl_get32(b + 4) * 1000 != s->hello || (nl_time)nl_get32(b + 8) * 1000 != s->dead) {
        nl_log(n, NL_L_NET, 2, m->pkt_id, T_ERR "OSPF" T_RESET " Hello %s %s: %s", _("from"), nl_ip_str(rid),
               _("hello/dead intervals differ from ours: ignored, no adjacency"));
        nl_count(n, "ospf.hello_mismatch", 1);
        return;
    }
    for (i = 12; i + 4 <= len; i += 4)
        if (nl_get32(b + i) == s->rid)
            sees_me = 1;
    nb = nbr_find(s, rid, m->in_if, 1);
    if (!nb)
        return;
    fresh = nb->state == NS_DOWN;
    nb->addr = m->src;
    nb->last = nl_now();
    if (fresh) {
        nb->state = NS_INIT;
        nb->nrx = 0;
        nl_log(n, NL_L_NET, 2, m->pkt_id, T_OK "OSPF" T_RESET " Hello %s %s (%s): %s", _("from"), nl_ip_str(rid),
               n->ifc[m->in_if].name, _("new neighbor, state Init"));
        send_hello(s, m->in_if);        /* let it see us quickly */
    }
    if (sees_me && nb->state == NS_INIT) {
        nb->state = NS_FULL;
        nl_count_set(n, "ospf.neighbors", full_count(s));
        nl_log(n, NL_L_NET, 1, m->pkt_id, T_OK "OSPF" T_RESET " %s %s (%s): %s", _("neighbor"), nl_ip_str(rid),
               n->ifc[m->in_if].name, _("two-way, adjacency Full: exchanging databases"));
        send_hello(s, m->in_if);        /* so that it sees itself listed before our LSAs arrive */
        for (i = 0; i < MAX_LSA; i++)
            if (s->db[i].used)
                send_lsa(s, nb, &s->db[i], 1);
        originate(s, 0);
    } else if (!sees_me && nb->state == NS_FULL) {
        nbr_down(s, nb, _("it no longer lists us: restarted"));
        nb->state = NS_INIT;
        nb->nrx = 0;
    }
}

static void rx_lsu(ostate *s, const nl_ipmeta *m, nbr *nb, const uint8_t *b, int len)
{
    uint8_t ack[HDR + 4 + MAX_LSA * 8];
    int count, off = 4, nack = 0, i, k;

    if (len < 4)
        return;
    count = (int)nl_get32(b);
    for (i = 0; i < count && off + 12 <= len; i++) {
        nl_ip rid = nl_get32(b + off);
        uint32_t seq = nl_get32(b + off + 4);
        int nlk = nl_get16(b + off + 10);
        const uint8_t *lk = b + off + 12;
        lsa *cur;

        if (nlk > MAX_LNK || off + 12 + nlk * 12 > len)
            break;
        off += 12 + nlk * 12;
        if (nack < MAX_LSA) {
            nl_put32(ack + HDR + 4 + nack * 8, rid);
            nl_put32(ack + HDR + 8 + nack * 8, seq);
            nack++;
        }
        if (rid == s->rid) {
            /* an old incarnation of ours is still around: outbid it */
            if (seq >= s->seq) {
                s->seq = seq;
                originate(s, 1);
            }
            continue;
        }
        cur = db_find(s, rid, 0);
        if (cur && seq == cur->seq) {
            for (k = 0; k < nb->nrx; k++)       /* same copy back: as good as an ack */
                if (nb->rx[k].rid == rid && nb->rx[k].seq == seq)
                    nb->rx[k--] = nb->rx[--nb->nrx];
            continue;
        }
        if (cur && seq < cur->seq) {
            send_lsa(s, nb, cur, 1);            /* we know better: tell the sender */
            nack--;
            continue;
        }
        cur = db_find(s, rid, 1);
        if (!cur)
            continue;
        cur->seq = seq;
        cur->rcvd = nl_now();
        cur->nl = nlk;
        for (k = 0; k < nlk; k++) {
            cur->l[k].type = lk[k * 12];
            cur->l[k].cost = nl_get16(lk + k * 12 + 2);
            cur->l[k].id = nl_get32(lk + k * 12 + 4);
            cur->l[k].data = nl_get32(lk + k * 12 + 8);
        }
        nl_log(s->n, NL_L_NET, 2, m->pkt_id, T_OK "OSPF" T_RESET " LSA %s seq=%u (%d %s) %s %s: %s",
               nl_ip_str(rid), seq, nlk, _("links"), _("from"), nl_ip_str(nb->rid),
               _("newer: stored and flooded on"));
        flood(s, cur, nb);
        spf_schedule(s);
    }
    if (nack > 0) {
        nl_put32(ack + HDR, (uint32_t)nack);
        send_pkt(s, OP_ACK, nb->ifx, nb->addr, ack, HDR + 4 + nack * 8);
    }
    nl_count_set(s->n, "ospf.lsdb", 0);
    for (i = 0, k = 0; i < MAX_LSA; i++)
        k += s->db[i].used;
    nl_count_set(s->n, "ospf.lsdb", k);
}

static void ospf_rx(nl_node *n, void *ctx, const nl_ipmeta *m)
{
    ostate *s = ctx;
    const uint8_t *b = m->l4;
    nl_ip rid;
    nbr *nb;
    int len, i, k;

    if (m->in_if < 0 || m->l4len < HDR || b[0] != 2)
        return;
    len = nl_get16(b + 2);
    if (len < HDR || len > m->l4len || nl_cksum(b, len, 0) != 0)
        return;
    rid = nl_get32(b + 4);
    if (rid == s->rid)
        return;
    if (b[1] == OP_HELLO) {
        rx_hello(s, m, rid, b + HDR, len - HDR);
        return;
    }
    nb = nbr_find(s, rid, m->in_if, 0);
    if (!nb || nb->state != NS_FULL)
        return;
    if (b[1] == OP_LSU) {
        rx_lsu(s, m, nb, b + HDR, len - HDR);
    } else if (b[1] == OP_ACK && len >= HDR + 4) {
        int count = (int)nl_get32(b + HDR);
        for (i = 0; i < count && HDR + 4 + (i + 1) * 8 <= len; i++) {
            nl_ip arid = nl_get32(b + HDR + 4 + i * 8);
            uint32_t aseq = nl_get32(b + HDR + 8 + i * 8);
            for (k = 0; k < nb->nrx; k++)
                if (nb->rx[k].rid == arid && nb->rx[k].seq == aseq) {
                    nb->rx[k] = nb->rx[--nb->nrx];
                    break;
                }
        }
    }
    (void)n;
}

/* ---- module ---- */

static void ospf_arm(ostate *s)
{
    s->hello_ev = nl_after(s->n, (nl_time)nl_rand(100000), hello_tick, s);
    s->rx_ev = nl_after(s->n, s->rxmt, rxmt_tick, s);
}

static int ospf_run(nl_node *n, const nl_args *a)
{
    ostate *s = nl_mod_get(n, &ospf_mod);
    const char *sub = nl_arg_pos(a, 0), *id;
    char key[24];
    int i;

    if (s && sub && strcmp(sub, "cost") == 0) {
        const char *ifn = nl_arg_pos(a, 1), *val = nl_arg_pos(a, 2);
        int ifx = ifn ? nl_if_find(n, ifn) : -1;

        if (ifx < 0 || !val || atoi(val) < 1) {
            nl_log(n, NL_L_NET, 1, 0, T_ERR "ospf: %s", _("usage: ospf cost IFACE N"));
            return -1;
        }
        s->cost_if[ifx] = atoi(val);
        nl_log(n, NL_L_NET, 1, 0, T_OK "OSPF" T_RESET " %s %s = %d: %s", _("cost of"), ifn, s->cost_if[ifx],
               _("a new LSA tells everyone"));
        originate(s, 0);
        return 0;
    }
    if (s)
        return 0;
    s = calloc(1, sizeof(*s));
    if (!s)
        return -1;
    s->n = n;
    s->hello = nl_arg_time(a, "hello", NL_SEC(10));
    if (s->hello < NL_MS(50))
        s->hello = NL_MS(50);
    s->dead = nl_arg_time(a, "dead", s->hello * 4);
    s->rxmt = nl_arg_time(a, "rxmt", s->hello / 2 > NL_SEC(5) ? NL_SEC(5) : s->hello / 2);
    s->spf_delay = nl_arg_time(a, "spf", NL_MS(200));
    s->cost_all = (int)nl_arg_int(a, "cost", 0);
    s->carrier = (int)nl_arg_int(a, "carrier", 1);
    for (i = 0; i < n->nif; i++) {
        snprintf(key, sizeof(key), "cost.%s", n->ifc[i].name);
        s->cost_if[i] = (int)nl_arg_int(a, key, 0);
        if (n->ifc[i].ip > s->rid)
            s->rid = n->ifc[i].ip;      /* router id: the highest address */
    }
    id = nl_arg_str(a, "id", NULL);
    if (id)
        nl_parse_ip(id, &s->rid);
    n->forward = 1;
    nl_mod_set(n, &ospf_mod, s);
    nl_ip_bind(n, NL_PROTO_OSPF, ospf_rx, s);
    nl_log(n, NL_L_NET, 2, 0, T_OK "OSPF" T_RESET " %s %s, hello %s, dead %s", _("started: router id"),
           nl_ip_str(s->rid), nl_time_str(s->hello), nl_time_str(s->dead));
    ospf_arm(s);
    return 0;
}

static void ospf_reset(ostate *s)
{
    nl_cancel(&s->hello_ev);
    nl_cancel(&s->rx_ev);
    nl_cancel(&s->spf_ev);
    memset(s->nb, 0, sizeof(s->nb));
    memset(s->db, 0, sizeof(s->db));
    s->seq = 0;
    s->ninst = 0;
    nl_route_flush(s->n, NL_RT_OSPF);
}

static void ospf_stop(nl_node *n)
{
    ostate *s = nl_mod_get(n, &ospf_mod);

    if (!s)
        return;
    ospf_reset(s);
    nl_mod_set(n, &ospf_mod, NULL);
    free(s);
}

static void ospf_crash(nl_node *n)   { ospf_reset(nl_mod_get(n, &ospf_mod)); }
static void ospf_recover(nl_node *n) { ospf_arm(nl_mod_get(n, &ospf_mod)); }

static int ospf_panel(nl_node *n, int x, int y, int w, int h, int detail)
{
    static const char *const st[] = { "Down", "Init", "Full" };
    ostate *s = nl_mod_get(n, &ospf_mod);
    int i, k, row = 0;

    row += nl_ui_kv(x, y + row, w, _("router id"), "%s", nl_ip_str(s->rid));
    row += nl_ui_kv(x, y + row, w, _("hello / dead"), "%s / %s", nl_time_str(s->hello), nl_time_str(s->dead));
    row += nl_ui_kv(x, y + row, w, _("SPF runs"), "%u  (%s %s)  %d %s", s->spf_runs, _("last"),
                    s->spf_runs ? nl_time_str(nl_now() - s->last_spf) : "-", s->ninst, _("routes"));
    if (detail >= 2)
        row += nl_ui_kv(x, y + row, w, _("LSAs flooded / retransmitted"), "%u / %u", s->flooded, s->rexmits);
    if (row < h)
        tui_print(x, y + row++, w, TS(C_DIM, C_DEFAULT, 0), "%-15s %-15s %-5s %-5s %s", _("neighbor"), _("address"),
                  _("dev"), _("state"), _("cost"));
    for (i = 0; i < MAX_NBR && row < h; i++) {
        nbr *nb = &s->nb[i];
        if (nb->state == NS_DOWN)
            continue;
        tui_print(x, y + row++, w, TS(nb->state == NS_FULL ? C_OK : C_WARN, C_DEFAULT, 0),
                  "%-15s %-15s %-5s %-5s %d", nl_ip_str(nb->rid), nl_ip_str(nb->addr), n->ifc[nb->ifx].name,
                  st[nb->state], if_cost(s, nb->ifx));
    }
    if (row < h)
        tui_print(x, y + row++, w, TS(C_DIM, C_DEFAULT, 0), "%-15s %-6s %-5s %s", _("LSDB: router"), "seq",
                  _("links"), detail >= 3 ? _("(neighbor:cost, network/cost)") : _("age"));
    for (i = 0; i < MAX_LSA && row < h; i++) {
        lsa *a = &s->db[i];
        char links[160] = "";
        size_t len = 0;

        if (!a->used)
            continue;
        if (detail >= 3) {
            for (k = 0; k < a->nl && len < sizeof(links) - 30; k++)
                len += (size_t)snprintf(links + len, sizeof(links) - len, a->l[k].type == LT_P2P ? "%s:%d " : "%s/%d ",
                                        nl_ip_str(a->l[k].id), a->l[k].cost);
        } else {
            snprintf(links, sizeof(links), "%s", nl_time_str(nl_now() - a->rcvd));
        }
        tui_print(x, y + row++, w, TS(a->rid == s->rid ? C_INFO : C_TEXT, C_DEFAULT, 0), "%-15s %-6u %-5d %s",
                  nl_ip_str(a->rid), a->seq, a->nl, links);
    }
    return row;
}

static const nl_module ospf_mod = {
    .name = "ospf", .title = N_("OSPF — link-state routing with shortest-path-first"), .layer = NL_L_NET,
    .usage = N_("run ROUTER ospf [hello=10s] [dead=4×hello] [cost=N] [cost.eth1=N] [id=A.B.C.D]\n"
                "                [rxmt=TIME] [spf=200ms] [carrier=1]\n"
                "run ROUTER ospf cost IFACE N     change the cost of an interface while running\n"
                "  Without cost= the cost of an interface is 100 Mbit/s divided by its bandwidth.\n"
                "  carrier=0 ignores link-down signals: failures are found only by the dead interval.\n"
                "  Neighbors must use the same hello and dead intervals."),
    .theory = N_("A link-state protocol makes every router learn the whole map. Each router does three "
                 "things. First, it discovers its neighbors by sending Hello packets on every interface; "
                 "two routers become adjacent when each sees itself listed in the other's Hello, and a "
                 "neighbor that stays silent for the dead interval is declared down. Second, it describes "
                 "itself in a link-state advertisement (LSA): the list of its neighbors and attached "
                 "networks with a cost for each. The LSA carries a sequence number and is flooded: a "
                 "router that receives a newer LSA stores it, acknowledges it and passes it on to all its "
                 "other neighbors, retransmitting until acknowledged. Soon every router holds the same "
                 "link-state database. Third, each router runs Dijkstra's algorithm on that database with "
                 "itself as the root and installs, for every network, the first hop of the cheapest path.\n\n"
                 "Compared with distance vector, bad news travel as fast as good news (a change is "
                 "flooded at once, and everyone recomputes from facts rather than from hearsay), there is "
                 "no count-to-infinity, and costs can express bandwidth instead of hop count. The price is "
                 "memory and computation, which real OSPF contains by splitting large networks into "
                 "areas.\n\n"
                 "Simplified here: single area, no designated router (routers on a shared LAN form a full "
                 "mesh of adjacencies), a new adjacency sends its whole database instead of the database "
                 "description exchange, packet layouts are OSPF-like but not bit-exact, and LSAs do not "
                 "age out."),
    .run = ospf_run, .stop = ospf_stop, .crash = ospf_crash, .recover = ospf_recover, .panel = ospf_panel,
};
NL_MODULE(ospf_mod)

static void dis_ospf(nl_dissect *o, const uint8_t *d, int len, int off)
{
    int type, i, count, p;

    if (len < HDR)
        return;
    type = d[1];
    nl_dis_field(o, "OSPF", "Version / Type", off, 2, "%d / %d (%s)", d[0], type,
                 type == OP_HELLO ? "Hello" : type == OP_LSU ? "LS Update" : type == OP_ACK ? "LS Ack" : "?");
    nl_dis_field(o, "OSPF", "Length", off + 2, 2, "%d", nl_get16(d + 2));
    nl_dis_field(o, "OSPF", "Router ID", off + 4, 4, "%s", nl_ip_str(nl_get32(d + 4)));
    nl_dis_field(o, "OSPF", "Area", off + 8, 4, "%s", nl_ip_str(nl_get32(d + 8)));
    nl_dis_field(o, "OSPF", "Checksum", off + 12, 2, "0x%04x %s", nl_get16(d + 12),
                 nl_cksum(d, len, 0) == 0 ? "✓" : "✗ WRONG");
    if (type == OP_HELLO && len >= HDR + 12) {
        int nn = (len - HDR - 12) / 4;
        nl_dis_field(o, "OSPF", "Network mask", off + HDR, 4, "%s", nl_ip_str(nl_get32(d + HDR)));
        nl_dis_field(o, "OSPF", "Hello / Dead (ms)", off + HDR + 4, 8, "%u / %u", nl_get32(d + HDR + 4),
                     nl_get32(d + HDR + 8));
        for (i = 0; i < nn; i++)
            nl_dis_field(o, "OSPF", "Neighbor seen", off + HDR + 12 + i * 4, 4, "%s",
                         nl_ip_str(nl_get32(d + HDR + 12 + i * 4)));
        nl_dis_summary(o, "OSPF", OSPF_COLOR, "Hello from %s, sees %d neighbor%s", nl_ip_str(nl_get32(d + 4)), nn,
                       nn == 1 ? "" : "s");
    } else if (type == OP_LSU && len >= HDR + 4) {
        nl_ip first = 0;
        uint32_t fseq = 0;

        count = (int)nl_get32(d + HDR);
        p = HDR + 4;
        for (i = 0; i < count && p + 12 <= len; i++) {
            int nlk = nl_get16(d + p + 10), k;

            if (!i) {
                first = nl_get32(d + p);
                fseq = nl_get32(d + p + 4);
            }
            nl_dis_field(o, "OSPF", "Router-LSA", off + p, 12, "%s seq=%u age=%us links=%d",
                         nl_ip_str(nl_get32(d + p)), nl_get32(d + p + 4), nl_get16(d + p + 8), nlk);
            for (k = 0; k < nlk && p + 12 + (k + 1) * 12 <= len; k++) {
                const uint8_t *e = d + p + 12 + k * 12;
                nl_dis_field(o, "OSPF", e[0] == LT_P2P ? "  link to router" : "  stub network",
                             off + p + 12 + k * 12, 12, "%s %s%s cost %d", nl_ip_str(nl_get32(e + 4)),
                             e[0] == LT_P2P ? "via " : "mask ", nl_ip_str(nl_get32(e + 8)), nl_get16(e + 2));
            }
            p += 12 + nlk * 12;
        }
        if (count == 1)
            nl_dis_summary(o, "OSPF", OSPF_COLOR, "LS Update: LSA of %s seq=%u", nl_ip_str(first), fseq);
        else
            nl_dis_summary(o, "OSPF", OSPF_COLOR, "LS Update: %d LSAs", count);
    } else if (type == OP_ACK && len >= HDR + 4) {
        count = (int)nl_get32(d + HDR);
        for (i = 0; i < count && HDR + 4 + (i + 1) * 8 <= len; i++)
            nl_dis_field(o, "OSPF", "Acknowledged", off + HDR + 4 + i * 8, 8, "%s seq=%u",
                         nl_ip_str(nl_get32(d + HDR + 4 + i * 8)), nl_get32(d + HDR + 8 + i * 8));
        nl_dis_summary(o, "OSPF", OSPF_COLOR, "LS Ack: %d LSA%s", count, count == 1 ? "" : "s");
    } else {
        nl_dis_summary(o, "OSPF", OSPF_COLOR, "type %d", type);
    }
}

NL_INIT(ospf)
{
    nl_dissector_ip(NL_PROTO_OSPF, dis_ospf);
}
