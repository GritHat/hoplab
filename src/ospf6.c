/*
 * ospf6.c - OSPF for IPv6 (OSPFv3, RFC 5340), single area, over IP protocol
 *           89 in IPv6.
 *
 * What is real: the 16-byte OSPFv3 packet header with the IPv6 pseudo-header
 * checksum, Hello packets (interface id, intervals in seconds, list of the
 * neighbors heard) sent to ff02::5 from the link-local address; the 20-byte
 * LSA header with the Fletcher checksum; Router-LSAs that describe topology
 * only (neighbor router id and interface ids, no addresses); Link-LSAs with
 * link-local flooding scope that announce the router's link-local address;
 * Intra-Area-Prefix-LSAs that attach the IPv6 prefixes to a router;
 * reliable flooding with LS Acknowledgment packets and retransmission;
 * Dijkstra on the database; next hops that are link-local addresses.
 *
 * What is simplified, as in the IPv4 module: no designated router (a link
 * with several routers is a mesh of point-to-point adjacencies, so there
 * are no Network-LSAs); a new adjacency synchronizes by sending the whole
 * database instead of the Database Description / LS Request exchange;
 * LSAs are not aged out; one area, no inter-area or external LSAs.
 *
 * The group ff02::5 is received with a packet hook placed before the
 * core's delivery decision (the core has no group membership).
 */
#include "nl_int.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define O6_COLOR  43
#define MAX_NBR   24
#define MAX_LSA   96
#define MAX_LNK   16
#define MAX_PFX   12
#define MAX_INST  128
#define HDR       16
#define LSA_HDR   20
#define INF       0x3fffffff
#define LSA_BUF   (LSA_HDR + 24 + MAX_PFX * 20 + MAX_LNK * 16)

static const nl_ip6 O6_GROUP = { { 0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 5 } };

enum { OP_HELLO = 1, OP_LSU = 4, OP_ACK = 5 };
enum { T_LINK = 0x0008, T_ROUTER = 0x2001, T_PREFIX = 0x2009 };
enum { NS_DOWN, NS_INIT, NS_FULL };

typedef struct { uint16_t metric; uint32_t ifid, nifid, nrid; } rlink;
typedef struct { nl_ip6 p; uint8_t plen; uint16_t metric; } pfx;

typedef struct {
    int      used;
    uint16_t type;
    uint32_t id, adv, seq;
    nl_time  rcvd;
    int      ifx;                       /* Link-LSA: the interface it belongs to */
    int      nl, np;
    rlink    l[MAX_LNK];                /* Router-LSA */
    pfx      p[MAX_PFX];                /* Intra-Area-Prefix-LSA, Link-LSA */
    nl_ip6   lladdr;                    /* Link-LSA */
} lsa;

typedef struct {
    int      state, ifx;
    uint32_t rid, ifid;
    nl_ip6   addr;                      /* link-local address: the next hop */
    nl_time  last;
    struct { uint16_t type; uint32_t id, adv, seq; nl_time sent; } rx[MAX_LSA];
    int      nrx;
} nbr;

typedef struct {
    nl_node *n;
    uint32_t rid;
    nl_time  hello, dead, rxmt, spf_delay;
    int      cost_all, cost_if[NL_MAX_IF], carrier;
    nbr      nb[MAX_NBR];
    lsa      db[MAX_LSA];
    nl_ev    hello_ev, rx_ev, spf_ev, flush_ev, carrier_ev;
    struct { nl_ip6 dst; int plen; } inst[MAX_INST];
    int      ninst;
    unsigned spf_runs, flooded, rexmits;
    nl_time  last_spf;
} ostate;

static const nl_module ospf6_mod;
static void originate(ostate *s);

/* ---- helpers ---- */

static const char *rid_str(uint32_t rid) { return nl_ip_str(rid); }

static const char *type_name(int t)
{
    return t == T_ROUTER ? "Router-LSA" : t == T_PREFIX ? "Intra-Area-Prefix-LSA" : t == T_LINK ? "Link-LSA" : "LSA";
}

static void prefix6(nl_ip6 *o, const nl_ip6 *a, int plen)
{
    int i;

    for (i = 0; i < 16; i++) {
        int bits = plen - i * 8;
        o->b[i] = bits >= 8 ? a->b[i] : bits <= 0 ? 0 : (uint8_t)(a->b[i] & (0xff << (8 - bits)));
    }
}

static const nl_ip6 *if_lladdr(const nl_node *n, int ifx)
{
    int k;

    if (!nl_if_usable(n, ifx))
        return NULL;
    for (k = 0; k < n->ifc[ifx].nip6; k++)
        if (nl_ip6_linklocal(&n->ifc[ifx].ip6[k].a) && n->ifc[ifx].ip6[k].state == NL_A6_OK)
            return &n->ifc[ifx].ip6[k].a;
    return NULL;
}

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

static lsa *db_find(ostate *s, int type, uint32_t id, uint32_t adv, int create)
{
    lsa *slot = NULL;
    int i;

    for (i = 0; i < MAX_LSA; i++) {
        if (s->db[i].used && s->db[i].type == type && s->db[i].id == id && s->db[i].adv == adv)
            return &s->db[i];
        if (!s->db[i].used && !slot)
            slot = &s->db[i];
    }
    if (create && slot) {
        memset(slot, 0, sizeof(*slot));
        slot->used = 1;
        slot->type = (uint16_t)type;
        slot->id = id;
        slot->adv = adv;
    }
    return create ? slot : NULL;
}

static nbr *nbr_find(ostate *s, uint32_t rid, int ifx, int create)
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

static void count_db(ostate *s)
{
    int i, k = 0;

    for (i = 0; i < MAX_LSA; i++)
        k += s->db[i].used;
    nl_count_set(s->n, "ospf6.lsdb", k);
}

static void send_pkt(ostate *s, int type, int ifx, const nl_ip6 *dst, uint8_t *b, int len)
{
    const nl_ip6 *src = if_lladdr(s->n, ifx);
    nl_ip6tx tx;

    if (!src)
        return;
    b[0] = 3;                           /* version */
    b[1] = (uint8_t)type;
    nl_put16(b + 2, (uint16_t)len);
    nl_put32(b + 4, s->rid);
    nl_put32(b + 8, 0);                 /* area 0 */
    nl_put16(b + 12, 0);
    b[14] = 0;                          /* instance id */
    b[15] = 0;
    nl_put16(b + 12, nl_l4_cksum6(src, dst, NL_PROTO_OSPF, b, len));
    memset(&tx, 0, sizeof(tx));
    tx.src = *src;
    tx.dst = *dst;
    tx.proto = NL_PROTO_OSPF;
    tx.hlim = 1;
    tx.ifx = ifx;
    tx.use_if = 1;
    nl_ip6_send(s->n, &tx, b, len);
}

/* ---- LSA encoding ---- */

/* Fletcher checksum of an LSA, age field excluded (RFC 2328 12.1.7) */
static uint16_t fletcher(const uint8_t *lsa_bytes, int len)
{
    const uint8_t *d = lsa_bytes + 2;
    int n = len - 2, i, c0 = 0, c1 = 0, x, y;

    for (i = 0; i < n; i++) {
        c0 = (c0 + (i == 14 || i == 15 ? 0 : d[i])) % 255;
        c1 = (c1 + c0) % 255;
    }
    x = ((n - 15) * c0 - c1) % 255;
    if (x <= 0)
        x += 255;
    y = 510 - c0 - x;
    if (y > 255)
        y -= 255;
    return (uint16_t)(x << 8 | y);
}

static int put_prefix(uint8_t *b, const pfx *p)
{
    int words = (p->plen + 31) / 32;

    b[0] = p->plen;
    b[1] = 0;                           /* prefix options */
    nl_put16(b + 2, p->metric);
    memcpy(b + 4, p->p.b, (size_t)words * 4);
    return 4 + words * 4;
}

static int get_prefix(const uint8_t *b, int left, pfx *p)
{
    int words;

    if (left < 4 || b[0] > 128)
        return -1;
    words = (b[0] + 31) / 32;
    if (left < 4 + words * 4)
        return -1;
    memset(p, 0, sizeof(*p));
    p->plen = b[0];
    p->metric = nl_get16(b + 2);
    memcpy(p->p.b, b + 4, (size_t)words * 4);
    return 4 + words * 4;
}

static int lsa_put(const lsa *a, uint8_t *b)
{
    int len = LSA_HDR, i;
    nl_time age = (nl_now() - a->rcvd) / NL_SEC(1);

    nl_put16(b, (uint16_t)(age > 3599 ? 3599 : age));
    nl_put16(b + 2, a->type);
    nl_put32(b + 4, a->id);
    nl_put32(b + 8, a->adv);
    nl_put32(b + 12, a->seq);
    nl_put16(b + 16, 0);
    if (a->type == T_ROUTER) {
        b[len] = 0;                     /* flags */
        b[len + 1] = 0;                 /* options: V6 and R bits */
        b[len + 2] = 0;
        b[len + 3] = 0x13;
        len += 4;
        for (i = 0; i < a->nl; i++) {
            b[len] = 1;                 /* point-to-point */
            b[len + 1] = 0;
            nl_put16(b + len + 2, a->l[i].metric);
            nl_put32(b + len + 4, a->l[i].ifid);
            nl_put32(b + len + 8, a->l[i].nifid);
            nl_put32(b + len + 12, a->l[i].nrid);
            len += 16;
        }
    } else if (a->type == T_PREFIX) {
        nl_put16(b + len, (uint16_t)a->np);
        nl_put16(b + len + 2, T_ROUTER);        /* the prefixes belong to this router */
        nl_put32(b + len + 4, 0);
        nl_put32(b + len + 8, a->adv);
        len += 12;
        for (i = 0; i < a->np; i++)
            len += put_prefix(b + len, &a->p[i]);
    } else {
        b[len] = 1;                     /* router priority */
        b[len + 1] = 0;
        b[len + 2] = 0;
        b[len + 3] = 0x13;
        memcpy(b + len + 4, a->lladdr.b, 16);
        nl_put32(b + len + 20, (uint32_t)a->np);
        len += 24;
        for (i = 0; i < a->np; i++)
            len += put_prefix(b + len, &a->p[i]);
    }
    nl_put16(b + 18, (uint16_t)len);
    nl_put16(b + 16, fletcher(b, len));
    return len;
}

/* Parse the body of a received LSA into a; 0 = malformed */
static int lsa_get(lsa *a, const uint8_t *b, int len)
{
    int off = LSA_HDR, i, np, used;

    a->nl = a->np = 0;
    if (a->type == T_ROUTER) {
        for (off += 4; off + 16 <= len && a->nl < MAX_LNK; off += 16) {
            if (b[off] != 1)
                continue;
            a->l[a->nl].metric = nl_get16(b + off + 2);
            a->l[a->nl].ifid = nl_get32(b + off + 4);
            a->l[a->nl].nifid = nl_get32(b + off + 8);
            a->l[a->nl].nrid = nl_get32(b + off + 12);
            a->nl++;
        }
        return 1;
    }
    if (a->type == T_PREFIX) {
        if (len < off + 12)
            return 0;
        np = nl_get16(b + off);
        off += 12;
    } else if (a->type == T_LINK) {
        if (len < off + 24)
            return 0;
        memcpy(a->lladdr.b, b + off + 4, 16);
        np = (int)nl_get32(b + off + 20);
        off += 24;
    } else {
        return 0;
    }
    for (i = 0; i < np && a->np < MAX_PFX; i++) {
        used = get_prefix(b + off, len - off, &a->p[a->np]);
        if (used < 0)
            return 0;
        off += used;
        a->np++;
    }
    return 1;
}

/* LSAs are not sent one packet each: they are queued per neighbor and a
 * flush, scheduled for the same instant, packs what is pending into as few
 * LS Update packets as possible (as real routers do). */
static void flush_lsas(void *ctx)
{
    ostate *s = ctx;
    uint8_t b[HDR + 4 + 1200 + LSA_BUF];
    int i, k;

    s->flush_ev = 0;
    for (i = 0; i < MAX_NBR; i++) {
        nbr *nb = &s->nb[i];
        int len = HDR + 4, count = 0;

        if (nb->state != NS_FULL)
            continue;
        for (k = 0; k <= nb->nrx; k++) {
            lsa *a = k < nb->nrx && nb->rx[k].sent < 0
                     ? db_find(s, nb->rx[k].type, nb->rx[k].id, nb->rx[k].adv, 0) : NULL;

            if (count && (k == nb->nrx || len > HDR + 4 + 1200)) {
                nl_put32(b + HDR, (uint32_t)count);
                send_pkt(s, OP_LSU, nb->ifx, &nb->addr, b, len);
                len = HDR + 4;
                count = 0;
            }
            if (!a)
                continue;
            len += lsa_put(a, b + len);
            count++;
            nb->rx[k].seq = a->seq;
            nb->rx[k].sent = nl_now();
        }
    }
}

/* Queue one LSA for a neighbor, which must then acknowledge it */
static void send_lsa(ostate *s, nbr *nb, const lsa *a)
{
    int i;

    for (i = 0; i < nb->nrx; i++)
        if (nb->rx[i].type == a->type && nb->rx[i].id == a->id && nb->rx[i].adv == a->adv)
            break;
    if (i == nb->nrx) {
        if (nb->nrx >= MAX_LSA)
            return;
        nb->nrx++;
    }
    nb->rx[i].type = a->type;
    nb->rx[i].id = a->id;
    nb->rx[i].adv = a->adv;
    nb->rx[i].seq = a->seq;
    nb->rx[i].sent = -1;                /* pending */
    if (!s->flush_ev)
        s->flush_ev = nl_after(s->n, 0, flush_lsas, s);
}

/* Area scope goes to every adjacency; link-local scope stays on its link */
static void flood(ostate *s, const lsa *a, const nbr *except)
{
    int i;

    for (i = 0; i < MAX_NBR; i++) {
        nbr *nb = &s->nb[i];
        if (nb->state != NS_FULL || nb == except || (a->type == T_LINK && nb->ifx != a->ifx))
            continue;
        send_lsa(s, nb, a);
        s->flooded++;
        nl_count(s->n, "ospf6.lsa_flooded", 1);
    }
}

/* ---- shortest path first ---- */

static const rlink *link_back(const lsa *a, uint32_t rid, uint32_t my_ifid_there)
{
    int i;

    for (i = 0; i < a->nl; i++)
        if (a->l[i].nrid == rid && a->l[i].ifid == my_ifid_there)
            return &a->l[i];
    return NULL;
}

static void spf_run(void *ctx)
{
    ostate *s = ctx;
    nl_node *n = s->n;
    static int dist[MAX_LSA], done[MAX_LSA], fif[MAX_LSA];
    static nl_ip6 fgw[MAX_LSA];
    struct { nl_ip6 dst; int plen, cost, ifx; nl_ip6 gw; } rt[MAX_INST];
    int nrt = 0, i, k, u, root = -1, reach = 0;

    s->spf_ev = 0;
    for (i = 0; i < MAX_LSA; i++) {
        dist[i] = INF;
        done[i] = 0;
        if (s->db[i].used && s->db[i].type == T_ROUTER && s->db[i].adv == s->rid)
            root = i;
    }
    if (root < 0)
        return;
    dist[root] = 0;
    for (;;) {
        const lsa *a;

        u = -1;
        for (i = 0; i < MAX_LSA; i++)
            if (s->db[i].used && s->db[i].type == T_ROUTER && !done[i] && dist[i] < INF &&
                (u < 0 || dist[i] < dist[u]))
                u = i;
        if (u < 0)
            break;
        done[u] = 1;
        reach++;
        a = &s->db[u];
        for (k = 0; k < a->nl; k++) {
            const rlink *l = &a->l[k];
            lsa *v = db_find(s, T_ROUTER, 0, l->nrid, 0);
            int vi, alt, hif = -1;
            nl_ip6 hgw;

            /* a link counts only if both ends describe it */
            if (!v || !link_back(v, a->adv, l->nifid))
                continue;
            vi = (int)(v - s->db);
            alt = dist[u] + l->metric;
            if (done[vi] || alt >= dist[vi])
                continue;
            memset(&hgw, 0, sizeof(hgw));
            if (u == root) {
                for (i = 0; i < MAX_NBR; i++)
                    if (s->nb[i].state == NS_FULL && s->nb[i].rid == l->nrid &&
                        (uint32_t)(s->nb[i].ifx + 1) == l->ifid) {
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
    /* the prefixes attached to every reachable router */
    for (u = 0; u < MAX_LSA; u++) {
        const lsa *a = &s->db[u];
        lsa *r;
        int ri;

        if (!a->used || a->type != T_PREFIX || a->adv == s->rid || !(r = db_find(s, T_ROUTER, 0, a->adv, 0)))
            continue;
        ri = (int)(r - s->db);
        if (!done[ri])
            continue;
        for (k = 0; k < a->np; k++) {
            int cost = dist[ri] + a->p[k].metric, mine = 0, j;

            for (i = 0; i < n->nif; i++)
                for (j = 0; j < n->ifc[i].nip6; j++)
                    if (nl_if_usable(n, i) && n->ifc[i].ip6[j].plen == a->p[k].plen &&
                        nl_ip6_match(&n->ifc[i].ip6[j].a, &a->p[k].p, a->p[k].plen))
                        mine = 1;
            if (mine)
                continue;
            for (i = 0; i < nrt; i++)
                if (rt[i].plen == a->p[k].plen && nl_ip6_eq(&rt[i].dst, &a->p[k].p))
                    break;
            if (i == nrt) {
                if (nrt >= MAX_INST)
                    continue;
                nrt++;
            } else if (rt[i].cost <= cost) {
                continue;
            }
            rt[i].dst = a->p[k].p;
            rt[i].plen = a->p[k].plen;
            rt[i].cost = cost;
            rt[i].ifx = fif[ri];
            rt[i].gw = fgw[ri];
        }
    }
    for (i = 0; i < s->ninst; i++) {
        for (k = 0; k < nrt; k++)
            if (rt[k].plen == s->inst[i].plen && nl_ip6_eq(&rt[k].dst, &s->inst[i].dst))
                break;
        if (k == nrt)
            nl_route6_del(n, &s->inst[i].dst, s->inst[i].plen, NL_RT_OSPF);
    }
    for (i = 0; i < nrt; i++) {
        nl_route6_set(n, &rt[i].dst, rt[i].plen, &rt[i].gw, rt[i].ifx, rt[i].cost, NL_RT_OSPF);
        s->inst[i].dst = rt[i].dst;
        s->inst[i].plen = rt[i].plen;
    }
    s->ninst = nrt;
    s->spf_runs++;
    s->last_spf = nl_now();
    nl_count(n, "ospf6.spf_runs", 1);
    nl_count_set(n, "ospf6.routes", nrt);
    nl_log(n, NL_L_NET, 2, 0, T_OK "OSPFv3" T_RESET " SPF #%u: %d %s, %d %s", s->spf_runs, reach,
           _("routers in the shortest-path tree"), nrt, _("routes"));
}

static void spf_schedule(ostate *s)
{
    if (!s->spf_ev)
        s->spf_ev = nl_after(s->n, s->spf_delay, spf_run, s);
}

/* ---- our own LSAs ---- */

/* Install a new version of one of our LSAs if its content changed */
static void own(ostate *s, lsa *nu, uint32_t min_seq)
{
    lsa *cur = db_find(s, nu->type, nu->id, s->rid, 1);

    if (!cur)
        return;
    if (!min_seq && cur->seq && cur->nl == nu->nl && cur->np == nu->np &&
        memcmp(cur->l, nu->l, sizeof(rlink) * (size_t)nu->nl) == 0 &&
        memcmp(cur->p, nu->p, sizeof(pfx) * (size_t)nu->np) == 0 && nl_ip6_eq(&cur->lladdr, &nu->lladdr))
        return;
    nu->used = 1;
    nu->adv = s->rid;
    nu->seq = cur->seq ? cur->seq + 1 : 0x80000001u;
    if (min_seq && nu->seq <= min_seq)
        nu->seq = min_seq + 1;
    nu->rcvd = nl_now();
    *cur = *nu;
    nl_log(s->n, NL_L_NET, 2, 0, T_OK "OSPFv3" T_RESET " %s %s seq=%08x", _("originating"), type_name(cur->type),
           cur->seq);
    flood(s, cur, NULL);
    spf_schedule(s);
}

static void originate(ostate *s)
{
    nl_node *n = s->n;
    lsa rl, pl;
    int i, k;

    memset(&rl, 0, sizeof(rl));
    memset(&pl, 0, sizeof(pl));
    rl.type = T_ROUTER;
    pl.type = T_PREFIX;
    for (i = 0; i < MAX_NBR && rl.nl < MAX_LNK; i++)
        if (s->nb[i].state == NS_FULL) {
            rlink *l = &rl.l[rl.nl++];
            l->metric = (uint16_t)if_cost(s, s->nb[i].ifx);
            l->ifid = (uint32_t)(s->nb[i].ifx + 1);
            l->nifid = s->nb[i].ifid;
            l->nrid = s->nb[i].rid;
        }
    for (i = 0; i < n->nif; i++) {
        const nl_ip6 *ll = if_lladdr(n, i);
        lsa ll_lsa;

        if (!ll)
            continue;
        memset(&ll_lsa, 0, sizeof(ll_lsa));
        ll_lsa.type = T_LINK;
        ll_lsa.id = (uint32_t)(i + 1);
        ll_lsa.ifx = i;
        ll_lsa.lladdr = *ll;
        for (k = 0; k < n->ifc[i].nip6; k++) {
            pfx p;

            if (nl_ip6_linklocal(&n->ifc[i].ip6[k].a) || n->ifc[i].ip6[k].state != NL_A6_OK)
                continue;
            memset(&p, 0, sizeof(p));
            prefix6(&p.p, &n->ifc[i].ip6[k].a, n->ifc[i].ip6[k].plen);
            p.plen = n->ifc[i].ip6[k].plen;
            if (ll_lsa.np < MAX_PFX)
                ll_lsa.p[ll_lsa.np++] = p;
            p.metric = (uint16_t)if_cost(s, i);
            if (pl.np < MAX_PFX)
                pl.p[pl.np++] = p;
        }
        own(s, &ll_lsa, 0);
    }
    own(s, &rl, 0);
    own(s, &pl, 0);
    count_db(s);
}

/* ---- neighbors ---- */

static void nbr_down(ostate *s, nbr *nb, const char *why)
{
    int was_full = nb->state == NS_FULL;

    nl_log(s->n, NL_L_NET, 1, 0, T_WARN "OSPFv3" T_RESET " %s %s (%s): %s", _("neighbor"), rid_str(nb->rid),
           s->n->ifc[nb->ifx].name, why);
    nb->state = NS_DOWN;
    nl_count_set(s->n, "ospf6.neighbors", full_count(s));
    if (was_full)
        originate(s);
}

static void send_hello(ostate *s, int ifx)
{
    uint8_t b[HDR + 20 + MAX_NBR * 4];
    int len = HDR + 20, i;

    nl_put32(b + HDR, (uint32_t)(ifx + 1));             /* interface id */
    b[HDR + 4] = 1;                                     /* router priority */
    b[HDR + 5] = 0;
    b[HDR + 6] = 0;
    b[HDR + 7] = 0x13;                                  /* options: V6, E, R */
    nl_put16(b + HDR + 8, (uint16_t)(s->hello / NL_SEC(1)));
    nl_put16(b + HDR + 10, (uint16_t)(s->dead / NL_SEC(1)));
    nl_put32(b + HDR + 12, 0);                          /* no designated router */
    nl_put32(b + HDR + 16, 0);
    for (i = 0; i < MAX_NBR; i++)
        if (s->nb[i].state != NS_DOWN && s->nb[i].ifx == ifx) {
            nl_put32(b + len, s->nb[i].rid);
            len += 4;
        }
    send_pkt(s, OP_HELLO, ifx, &O6_GROUP, b, len);
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
        if (if_lladdr(n, i))
            send_hello(s, i);
    originate(s);                       /* interfaces and addresses may have come or gone */
    s->hello_ev = nl_after(n, s->hello, hello_tick, s);
}

/* Loss of carrier is noticed at once, without waiting for the next Hello */
static void carrier_tick(void *ctx)
{
    ostate *s = ctx;
    int i;

    for (i = 0; i < MAX_NBR; i++)
        if (s->nb[i].state != NS_DOWN && !nl_if_usable(s->n, s->nb[i].ifx))
            nbr_down(s, &s->nb[i], _("interface down"));
    s->carrier_ev = nl_after(s->n, NL_MS(100), carrier_tick, s);
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
            lsa *a = db_find(s, nb->rx[k].type, nb->rx[k].id, nb->rx[k].adv, 0);
            if (!a || a->seq != nb->rx[k].seq) {
                nb->rx[k] = nb->rx[--nb->nrx];
                continue;
            }
            if (nb->rx[k].sent < 0 || nl_now() - nb->rx[k].sent < s->rxmt) {
                k++;
                continue;
            }
            nl_log(s->n, NL_L_NET, 2, 0, T_WARN "OSPFv3" T_RESET " %s %s %s seq=%08x → %s",
                   _("no ack: retransmitting"), type_name(a->type), rid_str(a->adv), a->seq, rid_str(nb->rid));
            s->rexmits++;
            nl_count(s->n, "ospf6.retransmits", 1);
            send_lsa(s, nb, a);
            k++;
        }
    }
    s->rx_ev = nl_after(s->n, s->rxmt / 4 > 0 ? s->rxmt / 4 : 1, rxmt_tick, s);
}

/* ---- input ---- */

static void rx_hello(ostate *s, const nl_ip6 *src, int ifx, uint32_t pkt_id, uint32_t rid, const uint8_t *b, int len)
{
    nl_node *n = s->n;
    nbr *nb;
    int i, sees_me = 0, fresh;

    if (len < 20)
        return;
    if ((nl_time)nl_get16(b + 8) * NL_SEC(1) != s->hello || (nl_time)nl_get16(b + 10) * NL_SEC(1) != s->dead) {
        nl_log(n, NL_L_NET, 2, pkt_id, T_ERR "OSPFv3" T_RESET " Hello %s %s: %s", _("from"), rid_str(rid),
               _("hello/dead intervals differ from ours: ignored, no adjacency"));
        nl_count(n, "ospf6.hello_mismatch", 1);
        return;
    }
    for (i = 20; i + 4 <= len; i += 4)
        if (nl_get32(b + i) == s->rid)
            sees_me = 1;
    nb = nbr_find(s, rid, ifx, 1);
    if (!nb)
        return;
    fresh = nb->state == NS_DOWN;
    nb->addr = *src;
    nb->ifid = nl_get32(b);
    nb->last = nl_now();
    if (fresh) {
        nb->state = NS_INIT;
        nb->nrx = 0;
        nl_log(n, NL_L_NET, 2, pkt_id, T_OK "OSPFv3" T_RESET " Hello %s %s (%s, %s): %s", _("from"), rid_str(rid),
               n->ifc[ifx].name, nl_ip6_str(src), _("new neighbor, state Init"));
        send_hello(s, ifx);
    }
    if (sees_me && nb->state == NS_INIT) {
        nb->state = NS_FULL;
        nl_count_set(n, "ospf6.neighbors", full_count(s));
        nl_log(n, NL_L_NET, 1, pkt_id, T_OK "OSPFv3" T_RESET " %s %s (%s): %s", _("neighbor"), rid_str(rid),
               n->ifc[ifx].name, _("two-way, adjacency Full: exchanging databases"));
        send_hello(s, ifx);
        for (i = 0; i < MAX_LSA; i++)
            if (s->db[i].used && (s->db[i].type != T_LINK || (s->db[i].ifx == ifx && s->db[i].adv == s->rid)))
                send_lsa(s, nb, &s->db[i]);
        originate(s);
    } else if (!sees_me && nb->state == NS_FULL) {
        nbr_down(s, nb, _("it no longer lists us: restarted"));
        nb->state = NS_INIT;
        nb->nrx = 0;
    }
}

static void unack(nbr *nb, int type, uint32_t id, uint32_t adv, uint32_t seq)
{
    int k;

    for (k = 0; k < nb->nrx; k++)
        if (nb->rx[k].type == type && nb->rx[k].id == id && nb->rx[k].adv == adv && nb->rx[k].seq == seq) {
            nb->rx[k] = nb->rx[--nb->nrx];
            return;
        }
}

static void rx_lsu(ostate *s, nbr *nb, uint32_t pkt_id, const uint8_t *b, int len)
{
    uint8_t ack[HDR + MAX_LSA * LSA_HDR];
    int count, off = 4, nack = 0, i;

    if (len < 4)
        return;
    count = (int)nl_get32(b);
    for (i = 0; i < count && off + LSA_HDR <= len; i++) {
        const uint8_t *lb = b + off;
        int type = nl_get16(lb + 2), llen = nl_get16(lb + 18);
        uint32_t id = nl_get32(lb + 4), adv = nl_get32(lb + 8), seq = nl_get32(lb + 12);
        lsa *cur, tmp;

        if (llen < LSA_HDR || off + llen > len)
            break;
        off += llen;
        if (fletcher(lb, llen) != nl_get16(lb + 16)) {
            nl_count(s->n, "ospf6.bad_lsa", 1);
            continue;
        }
        if (nack < MAX_LSA) {
            memcpy(ack + HDR + nack * LSA_HDR, lb, LSA_HDR);
            nack++;
        }
        if (adv == s->rid) {
            /* a copy of ours from before a restart: outbid it with a newer one */
            cur = db_find(s, type, id, adv, 0);
            if (cur && seq == cur->seq) {
                unack(nb, type, id, adv, seq);
            } else if (!cur || (int32_t)(seq - cur->seq) > 0) {
                memset(&tmp, 0, sizeof(tmp));
                if (cur)
                    tmp = *cur;
                else {
                    tmp.type = (uint16_t)type;
                    tmp.id = id;
                }
                own(s, &tmp, seq);
            }
            continue;
        }
        cur = db_find(s, type, id, adv, 0);
        if (cur && seq == cur->seq) {
            unack(nb, type, id, adv, seq);      /* the same copy back: as good as an ack */
            continue;
        }
        if (cur && (int32_t)(seq - cur->seq) < 0) {
            send_lsa(s, nb, cur);               /* we know better: tell the sender */
            nack--;
            continue;
        }
        memset(&tmp, 0, sizeof(tmp));
        tmp.used = 1;
        tmp.type = (uint16_t)type;
        tmp.id = id;
        tmp.adv = adv;
        tmp.seq = seq;
        tmp.rcvd = nl_now();
        tmp.ifx = nb->ifx;
        if (!lsa_get(&tmp, lb, llen) || !(cur = db_find(s, type, id, adv, 1)))
            continue;
        *cur = tmp;
        if (type == T_LINK && !nl_ip6_zero(&cur->lladdr))
            nb->addr = cur->lladdr;             /* the next hop this neighbor asks us to use */
        nl_log(s->n, NL_L_NET, 2, pkt_id, T_OK "OSPFv3" T_RESET " %s %s seq=%08x %s %s: %s", type_name(type),
               rid_str(adv), seq, _("from"), rid_str(nb->rid),
               type == T_LINK ? _("stored (link scope: not flooded further)") : _("newer: stored and flooded on"));
        if (type != T_LINK)
            flood(s, cur, nb);
        spf_schedule(s);
    }
    if (nack > 0)
        send_pkt(s, OP_ACK, nb->ifx, &nb->addr, ack, HDR + nack * LSA_HDR);
    count_db(s);
}

static void ospf6_input(ostate *s, const nl_ip6 *src, const nl_ip6 *dst, int ifx, uint32_t pkt_id, const uint8_t *b,
                        int l4len)
{
    uint32_t rid;
    nbr *nb;
    int len, off;

    if (ifx < 0 || l4len < HDR || b[0] != 3 || !nl_ip6_linklocal(src))
        return;
    len = nl_get16(b + 2);
    if (len < HDR || len > l4len || nl_l4_cksum6(src, dst, NL_PROTO_OSPF, b, len) != 0) {
        nl_count(s->n, "ospf6.bad_packets", 1);
        return;
    }
    rid = nl_get32(b + 4);
    if (rid == s->rid)
        return;
    if (b[1] == OP_HELLO) {
        rx_hello(s, src, ifx, pkt_id, rid, b + HDR, len - HDR);
        return;
    }
    nb = nbr_find(s, rid, ifx, 0);
    if (!nb || nb->state != NS_FULL)
        return;
    if (b[1] == OP_LSU) {
        rx_lsu(s, nb, pkt_id, b + HDR, len - HDR);
    } else if (b[1] == OP_ACK) {
        for (off = HDR; off + LSA_HDR <= len; off += LSA_HDR)
            unack(nb, nl_get16(b + off + 2), nl_get32(b + off + 4), nl_get32(b + off + 8), nl_get32(b + off + 12));
    }
}

static void ospf6_rx(nl_node *n, void *ctx, const nl_ip6meta *m)
{
    (void)n;
    ospf6_input(ctx, &m->src, &m->dst, m->in_if, m->pkt_id, m->l4, m->l4len);
}

/* Packets for ff02::5 (AllSPFRouters), taken before the core's delivery decision */
static int ospf6_hook(nl_node *n, void *ctx, nl_pkt *p, int in_if, int out_if)
{
    const uint8_t *h = p->data + NL_ETH_HLEN;
    nl_ip6 src, dst;

    (void)n;
    (void)out_if;
    if (p->len < NL_ETH_HLEN + NL_IP6_HLEN + HDR || h[6] != NL_PROTO_OSPF || memcmp(h + 24, O6_GROUP.b, 16) != 0)
        return NL_HOOK_OK;
    memcpy(src.b, h + 8, 16);
    memcpy(dst.b, h + 24, 16);
    ospf6_input(ctx, &src, &dst, in_if, p->id, h + NL_IP6_HLEN, nl_get16(h + 4));
    return NL_HOOK_DROP;
}

/* ---- module ---- */

static void ospf6_arm(ostate *s)
{
    s->hello_ev = nl_after(s->n, (nl_time)nl_rand(100000), hello_tick, s);
    s->rx_ev = nl_after(s->n, s->rxmt, rxmt_tick, s);
    if (s->carrier)
        s->carrier_ev = nl_after(s->n, NL_MS(100), carrier_tick, s);
}

static int ospf6_run(nl_node *n, const nl_args *a)
{
    ostate *s = nl_mod_get(n, &ospf6_mod);
    const char *sub = nl_arg_pos(a, 0), *id;
    char key[24];
    int i;

    if (s && sub && strcmp(sub, "cost") == 0) {
        const char *ifn = nl_arg_pos(a, 1), *val = nl_arg_pos(a, 2);
        int ifx = ifn ? nl_if_find(n, ifn) : -1;

        if (ifx < 0 || !val || atoi(val) < 1) {
            nl_log(n, NL_L_NET, 1, 0, T_ERR "ospf6: %s", _("usage: ospf6 cost IFACE N"));
            return -1;
        }
        s->cost_if[ifx] = atoi(val);
        nl_log(n, NL_L_NET, 1, 0, T_OK "OSPFv3" T_RESET " %s %s = %d: %s", _("cost of"), ifn, s->cost_if[ifx],
               _("new LSAs tell everyone"));
        originate(s);
        return 0;
    }
    if (s)
        return 0;
    if (!nl_ip6_enabled()) {
        nl_log(n, NL_L_NET, 1, 0, T_ERR "OSPFv3" T_RESET " %s",
               _("this lab does not use IPv6 (add `ipv6 on` or IPv6 addresses to the scenario)"));
        return -1;
    }
    s = calloc(1, sizeof(*s));
    if (!s)
        return -1;
    s->n = n;
    /* the Hello carries whole seconds, as the real packet does */
    s->hello = (nl_arg_time(a, "hello", NL_SEC(10)) + NL_SEC(1) / 2) / NL_SEC(1) * NL_SEC(1);
    if (s->hello < NL_SEC(1))
        s->hello = NL_SEC(1);
    s->dead = (nl_arg_time(a, "dead", s->hello * 4) + NL_SEC(1) / 2) / NL_SEC(1) * NL_SEC(1);
    if (s->dead <= s->hello)
        s->dead = s->hello * 2;
    s->rxmt = nl_arg_time(a, "rxmt", s->hello / 2 > NL_SEC(5) ? NL_SEC(5) : s->hello / 2);
    s->spf_delay = nl_arg_time(a, "spf", NL_MS(200));
    s->cost_all = (int)nl_arg_int(a, "cost", 0);
    s->carrier = (int)nl_arg_int(a, "carrier", 1);
    /* the router id is still a 32-bit number: the highest IPv4 address when
     * there is one, else a number made from the node's position */
    s->rid = NL_IP(0, 0, 0, n->idx + 1);
    for (i = 0; i < n->nif; i++) {
        snprintf(key, sizeof(key), "cost.%s", n->ifc[i].name);
        s->cost_if[i] = (int)nl_arg_int(a, key, 0);
        if (n->ifc[i].ip > s->rid)
            s->rid = n->ifc[i].ip;
    }
    id = nl_arg_str(a, "id", NULL);
    if (id)
        nl_parse_ip(id, &s->rid);
    n->forward = 1;
    nl_mod_set(n, &ospf6_mod, s);
    nl_ip6_bind(n, NL_PROTO_OSPF, ospf6_rx, s);
    nl_hook6_add(n, NL_HOOK_PRE, ospf6_hook, s);
    nl_log(n, NL_L_NET, 2, 0, T_OK "OSPFv3" T_RESET " %s %s, hello %s, dead %s", _("started: router id"),
           rid_str(s->rid), nl_time_str(s->hello), nl_time_str(s->dead));
    ospf6_arm(s);
    return 0;
}

static void ospf6_reset(ostate *s)
{
    nl_cancel(&s->hello_ev);
    nl_cancel(&s->rx_ev);
    nl_cancel(&s->spf_ev);
    nl_cancel(&s->flush_ev);
    nl_cancel(&s->carrier_ev);
    memset(s->nb, 0, sizeof(s->nb));
    memset(s->db, 0, sizeof(s->db));
    s->ninst = 0;
    nl_route6_flush(s->n, NL_RT_OSPF);
    nl_count_set(s->n, "ospf6.neighbors", 0);
}

static void ospf6_stop(nl_node *n)
{
    ostate *s = nl_mod_get(n, &ospf6_mod);

    if (!s)
        return;
    ospf6_reset(s);
    nl_hook6_del(n, NL_HOOK_PRE, ospf6_hook, s);
    nl_mod_set(n, &ospf6_mod, NULL);
    free(s);
}

static void ospf6_crash(nl_node *n)
{
    ospf6_reset(nl_mod_get(n, &ospf6_mod));
}

static void ospf6_recover(nl_node *n)
{
    ospf6_arm(nl_mod_get(n, &ospf6_mod));
}

static int ospf6_panel(nl_node *n, int x, int y, int w, int h, int detail)
{
    ostate *s = nl_mod_get(n, &ospf6_mod);
    int i, k, row = 0;

    row += nl_ui_kv(x, y + row, w, _("router id"), "%s   hello %s  dead %s", rid_str(s->rid), nl_time_str(s->hello),
                    nl_time_str(s->dead));
    row += nl_ui_kv(x, y + row, w, _("SPF runs"), "%u   %s %u   %s %u", s->spf_runs, _("LSAs flooded"), s->flooded,
                    _("retransmitted"), s->rexmits);
    if (row < h)
        tui_print(x, y + row++, w, TS(C_DIM, C_DEFAULT, 0), "%-15s %-5s %-5s %s", _("neighbor"), _("dev"),
                  _("state"), _("link-local next hop"));
    for (i = 0; i < MAX_NBR && row < h; i++) {
        const nbr *nb = &s->nb[i];
        if (nb->state == NS_DOWN)
            continue;
        tui_print(x, y + row++, w, TS(nb->state == NS_FULL ? C_TEXT : C_WARN, C_DEFAULT, 0), "%-15s %-5s %-5s %s",
                  rid_str(nb->rid), n->ifc[nb->ifx].name, nb->state == NS_FULL ? "Full" : "Init",
                  nl_ip6_str(&nb->addr));
    }
    if (row < h)
        tui_print(x, y + row++, w, TS(C_DIM, C_DEFAULT, 0), "%s: LSA  %-12s %-8s %s", _("link-state database"),
                  _("router"), "seq", _("content"));
    for (i = 0; i < MAX_LSA && row < h; i++) {
        const lsa *a = &s->db[i];
        char what[80] = "";
        size_t len = 0;

        if (!a->used || (a->type == T_LINK && detail < 3))
            continue;
        if (a->type == T_ROUTER)
            for (k = 0; k < a->nl && len < sizeof(what) - 24; k++)
                len += (size_t)snprintf(what + len, sizeof(what) - len, "%s→%s(%d)", k ? " " : "",
                                        rid_str(a->l[k].nrid), a->l[k].metric);
        else
            for (k = 0; k < a->np && len < sizeof(what) - 50; k++)
                len += (size_t)snprintf(what + len, sizeof(what) - len, "%s%s/%d", k ? " " : "",
                                        nl_ip6_str(&a->p[k].p), a->p[k].plen);
        tui_print(x, y + row++, w, TS(a->adv == s->rid ? C_INFO : C_TEXT, C_DEFAULT, 0), " %-9s %-12s %08x %s",
                  a->type == T_ROUTER ? "Router" : a->type == T_PREFIX ? "IA-Prefix" : "Link", rid_str(a->adv),
                  a->seq, what);
    }
    return row;
}

static const nl_module ospf6_mod = {
    .name = "ospf6", .title = N_("OSPFv3 — link-state routing for IPv6 (RFC 5340)"), .layer = NL_L_NET,
    .usage = N_("run ROUTER ospf6 [hello=10s] [dead=4×hello] [cost=N] [cost.eth1=N] [id=A.B.C.D]\n"
                "                 [rxmt=TIME] [spf=200ms] [carrier=1]\n"
                "run ROUTER ospf6 cost IFACE N     change a cost while running\n"
                "  hello and dead are whole seconds, as in the packet (minimum 1s).\n"
                "  carrier=0 ignores link-down signals: failures are found only by the dead interval.\n"
                "  The router id is a 32-bit number written like an IPv4 address: the highest\n"
                "  IPv4 address of the router, or 0.0.0.N when it has none, or id=.\n"
                "  The lab must use IPv6. ospf and ospf6 can run side by side."),
    .theory = N_("OSPFv3 is the link-state protocol of OSPFv2 rebuilt around IPv6, and the rebuild is "
                 "instructive because it separates what the old protocol mixed. Topology and addressing "
                 "are now different things. A Router-LSA says only \"I am router R and I have a link of "
                 "cost c to router S\", identifying the two ends by interface numbers: it contains no "
                 "address at all. The IPv6 prefixes travel in a separate Intra-Area-Prefix-LSA that says "
                 "\"these prefixes are attached to router R\". Renumbering a network therefore changes "
                 "one small LSA and does not disturb the shortest-path tree.\n\n"
                 "The protocol runs per link, not per subnet, and uses link-local addresses for "
                 "everything that stays on a link: Hellos go to ff02::5 from fe80::, and the next hop "
                 "placed in the routing table is the neighbor's link-local address, which each router "
                 "announces to its neighbors in a Link-LSA whose flooding scope is that link only. "
                 "LSAs thus have three scopes (link, area, AS) encoded in their type. Router ids, area "
                 "ids and link-state ids remain 32-bit numbers, conventionally written like IPv4 "
                 "addresses even on a network with no IPv4. Authentication is delegated to IPsec.\n\n"
                 "The algorithm is unchanged: discover neighbors with Hello, flood LSAs reliably until "
                 "every router holds the same database, run Dijkstra, install routes.\n\n"
                 "Here: real packet and LSA headers and bodies (checksums included), single area. As in "
                 "the IPv4 module there is no designated router (several routers on a LAN form a mesh "
                 "of adjacencies and there are no Network-LSAs), a new adjacency sends its whole "
                 "database instead of the Database Description exchange, and LSAs do not age out."),
    .run = ospf6_run, .stop = ospf6_stop, .crash = ospf6_crash, .recover = ospf6_recover, .panel = ospf6_panel,
};
NL_MODULE(ospf6_mod)

/* Called by the protocol-89 dissector of ospf.c for version 3 packets */
void nl_dissect_ospf6(nl_dissect *o, const uint8_t *d, int len, int off);

void nl_dissect_ospf6(nl_dissect *o, const uint8_t *d, int len, int off)
{
    static const char *const names[] = { "?", "Hello", "Database Description", "LS Request", "LS Update", "LS Ack" };
    int type, i, n;

    if (len < HDR || d[0] != 3)
        return;                         /* version 2 belongs to the IPv4 dissector */
    type = d[1];
    nl_dis_field(o, "OSPFv3", "Version / Type", off, 2, "%d / %d (%s)", d[0], type, type <= 5 ? names[type] : "?");
    nl_dis_field(o, "OSPFv3", "Length", off + 2, 2, "%d", nl_get16(d + 2));
    nl_dis_field(o, "OSPFv3", "Router ID", off + 4, 4, "%s", nl_ip_str(nl_get32(d + 4)));
    nl_dis_field(o, "OSPFv3", "Area ID", off + 8, 4, "%s", nl_ip_str(nl_get32(d + 8)));
    nl_dis_field(o, "OSPFv3", "Checksum", off + 12, 2, "0x%04x", nl_get16(d + 12));
    nl_dis_field(o, "OSPFv3", "Instance ID", off + 14, 1, "%d", d[14]);
    if (type == OP_HELLO && len >= HDR + 20) {
        n = (len - HDR - 20) / 4;
        nl_dis_field(o, "OSPFv3", "Interface ID", off + HDR, 4, "%u", nl_get32(d + HDR));
        nl_dis_field(o, "OSPFv3", "Hello / Dead interval", off + HDR + 8, 4, "%d s / %d s", nl_get16(d + HDR + 8),
                     nl_get16(d + HDR + 10));
        for (i = 0; i < n; i++)
            nl_dis_field(o, "OSPFv3", "Neighbor", off + HDR + 20 + i * 4, 4, "%s",
                         nl_ip_str(nl_get32(d + HDR + 20 + i * 4)));
        nl_dis_summary(o, "OSPFv3", O6_COLOR, "Hello from %s, %d neighbor%s heard", nl_ip_str(nl_get32(d + 4)), n,
                       n == 1 ? "" : "s");
    } else if (type == OP_LSU && len >= HDR + 4) {
        int p = HDR + 4, t = 0;
        uint32_t adv = 0, seq = 0;

        n = (int)nl_get32(d + HDR);
        for (i = 0; i < n && p + LSA_HDR <= len; i++) {
            int ll = nl_get16(d + p + 18);
            t = nl_get16(d + p + 2);
            adv = nl_get32(d + p + 8);
            seq = nl_get32(d + p + 12);
            nl_dis_field(o, "OSPFv3", type_name(t), off + p, ll, "adv %s id %u seq %08x age %d", nl_ip_str(adv),
                         nl_get32(d + p + 4), seq, nl_get16(d + p));
            if (ll < LSA_HDR)
                break;
            p += ll;
        }
        if (n == 1)
            nl_dis_summary(o, "OSPFv3", O6_COLOR, "LS Update: %s of %s seq=%08x", type_name(t), nl_ip_str(adv), seq);
        else
            nl_dis_summary(o, "OSPFv3", O6_COLOR, "LS Update: %d LSAs", n);
    } else if (type == OP_ACK) {
        n = (len - HDR) / LSA_HDR;
        for (i = 0; i < n; i++)
            nl_dis_field(o, "OSPFv3", "Acknowledged", off + HDR + i * LSA_HDR, LSA_HDR, "%s adv %s seq %08x",
                         type_name(nl_get16(d + HDR + i * LSA_HDR + 2)),
                         nl_ip_str(nl_get32(d + HDR + i * LSA_HDR + 8)), nl_get32(d + HDR + i * LSA_HDR + 12));
        nl_dis_summary(o, "OSPFv3", O6_COLOR, "LS Ack: %d LSA%s", n, n == 1 ? "" : "s");
    } else {
        nl_dis_summary(o, "OSPFv3", O6_COLOR, "%s", type <= 5 ? names[type] : "packet");
    }
}
