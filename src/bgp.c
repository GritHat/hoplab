/*
 * bgp.c - BGP-4 (RFC 4271) in miniature: path-vector routing between
 *         autonomous systems.
 *
 * Real: the message formats (16-byte marker, OPEN, UPDATE with withdrawn
 * routes, ORIGIN / AS_PATH / NEXT_HOP attributes and NLRI, KEEPALIVE,
 * NOTIFICATION), keepalive and hold timers, loop detection by AS path, the
 * decision process (local preference, AS-path length, router id), per-peer
 * export with Gao-Rexford (valley-free) policies, withdrawals, AS-path
 * prepending and RPKI-style origin validation.
 *
 * Simplified: sessions run over UDP port 179 instead of TCP (so use
 * loss-free links between BGP routers), only eBGP between directly
 * connected routers, one router per AS, 16-bit AS numbers, one prefix per
 * UPDATE, no MED, communities, aggregation or route-flap damping.
 */
#include "nl_int.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BGP_PORT   179
#define BGP_COLOR  204
#define MAX_PEER   8
#define MAX_NET    12
#define MAX_PATH   16
#define MAX_RIB    48
#define MAX_ROA    8
#define BHDR       19

enum { M_OPEN = 1, M_UPDATE = 2, M_NOTIFICATION = 3, M_KEEPALIVE = 4 };
enum { REL_NONE, REL_CUSTOMER, REL_PEER, REL_PROVIDER };
enum { BS_IDLE, BS_OPENSENT, BS_OPENCONFIRM, BS_ESTABLISHED };

static const char *const REL_NAME[] = { "-", "customer", "peer", "provider" };
static const char *const ST_NAME[] = { "Idle", "OpenSent", "OpenConfirm", "Established" };

typedef struct {
    int      used;
    nl_ip    pfx;
    int      plen;
    uint16_t path[MAX_PATH];
    int      npath;
    nl_ip    nh;
} rib_in;

typedef struct { int used; nl_ip pfx; int plen; uint32_t sig; } rib_out;

typedef struct {
    char     name[16];
    nl_ip    addr, id;
    int      ifx, rel, state;
    uint16_t as;
    nl_time  last_rx, last_tx, last_open, since;
    rib_in   in[MAX_RIB];
    rib_out  out[MAX_RIB];
    unsigned rx_updates, tx_updates;
} peer;

typedef struct {
    int      used;
    nl_ip    pfx;
    int      plen;
    int      src;               /* peer index, -1 = originated here */
    uint16_t path[MAX_PATH];
    int      npath, lp, ncand;
    nl_ip    nh;
    const char *why;
} loc_route;

typedef struct {
    nl_node  *n;
    uint16_t  as;
    nl_ip     id;
    nl_time   keepalive, hold, mrai;
    int       prepend, rov, policy, connected;
    peer      p[MAX_PEER];
    int       np;
    struct { nl_ip pfx; int plen; } net[MAX_NET];
    int       nnet;
    struct { nl_ip pfx; int plen, maxlen; uint16_t as; } roa[MAX_ROA];
    int       nroa;
    loc_route loc[MAX_RIB];
    nl_ev     tick_ev, proc_ev;
} bstate;

static const nl_module bgp_mod;

static const char *path_str(const uint16_t *path, int n)
{
    static char bufs[4][MAX_PATH * 6 + 8];
    static int k;
    char *b = bufs[k = (k + 1) % 4];
    size_t len = 0;
    int i;

    b[0] = '\0';
    for (i = 0; i < n; i++)
        len += (size_t)snprintf(b + len, sizeof(bufs[0]) - len, "%s%u", i ? " " : "", path[i]);
    if (!n)
        snprintf(b, sizeof(bufs[0]), "(local)");
    return b;
}

/* ---- wire format ---- */

static int put_prefix(uint8_t *b, nl_ip pfx, int plen)
{
    int nb = (plen + 7) / 8, i;

    b[0] = (uint8_t)plen;
    for (i = 0; i < nb; i++)
        b[1 + i] = (uint8_t)(pfx >> (24 - 8 * i));
    return 1 + nb;
}

static int get_prefix(const uint8_t *b, int len, nl_ip *pfx, int *plen)
{
    int nb, i;

    if (len < 1 || b[0] > 32)
        return -1;
    nb = (b[0] + 7) / 8;
    if (len < 1 + nb)
        return -1;
    *plen = b[0];
    *pfx = 0;
    for (i = 0; i < nb; i++)
        *pfx |= (nl_ip)b[1 + i] << (24 - 8 * i);
    *pfx &= nl_mask(*plen);
    return 1 + nb;
}

static void bgp_send(bstate *s, peer *p, int type, uint8_t *b, int len)
{
    nl_iptx tx;

    memset(b, 0xff, 16);
    nl_put16(b + 16, (uint16_t)len);
    b[18] = (uint8_t)type;
    memset(&tx, 0, sizeof(tx));
    tx.dst = p->addr;
    tx.ttl = 1;                         /* eBGP: the neighbor is one hop away */
    tx.ifx = p->ifx;
    tx.use_if = 1;
    nl_udp_send_ex(s->n, &tx, BGP_PORT, BGP_PORT, b, len);
    p->last_tx = nl_now();
}

static void send_open(bstate *s, peer *p)
{
    uint8_t b[BHDR + 10];

    b[BHDR] = 4;
    nl_put16(b + BHDR + 1, s->as);
    nl_put16(b + BHDR + 3, (uint16_t)(s->hold / NL_SEC(1)));
    nl_put32(b + BHDR + 5, s->id);
    b[BHDR + 9] = 0;
    p->last_open = nl_now();
    bgp_send(s, p, M_OPEN, b, sizeof(b));
}

static void send_keepalive(bstate *s, peer *p)
{
    uint8_t b[BHDR];

    bgp_send(s, p, M_KEEPALIVE, b, BHDR);
}

static void send_notification(bstate *s, peer *p, int code, int sub)
{
    uint8_t b[BHDR + 2];

    b[BHDR] = (uint8_t)code;
    b[BHDR + 1] = (uint8_t)sub;
    bgp_send(s, p, M_NOTIFICATION, b, sizeof(b));
}

static void send_update(bstate *s, peer *p, nl_ip pfx, int plen, const uint16_t *path, int npath)
{
    uint8_t b[BHDR + 2 + 5 + 2 + 4 + 3 + 2 + MAX_PATH * 4 + 7 + 5];
    int len = BHDR, alen, i;
    uint8_t *a;

    if (!path) {                        /* withdrawal */
        int w = put_prefix(b + len + 2, pfx, plen);
        nl_put16(b + len, (uint16_t)w);
        len += 2 + w;
        nl_put16(b + len, 0);
        len += 2;
    } else {
        nl_put16(b + len, 0);
        len += 2;
        a = b + len + 2;
        alen = 0;
        a[alen++] = 0x40; a[alen++] = 1; a[alen++] = 1; a[alen++] = 0;      /* ORIGIN: IGP */
        a[alen++] = 0x40; a[alen++] = 2; a[alen++] = (uint8_t)(2 + npath * 2);
        a[alen++] = 2;                                                       /* AS_SEQUENCE */
        a[alen++] = (uint8_t)npath;
        for (i = 0; i < npath; i++) {
            nl_put16(a + alen, path[i]);
            alen += 2;
        }
        a[alen++] = 0x40; a[alen++] = 3; a[alen++] = 4;                     /* NEXT_HOP */
        nl_put32(a + alen, s->n->ifc[p->ifx].ip);
        alen += 4;
        nl_put16(b + len, (uint16_t)alen);
        len += 2 + alen;
        len += put_prefix(b + len, pfx, plen);
    }
    p->tx_updates++;
    nl_count(s->n, "bgp.updates_sent", 1);
    bgp_send(s, p, M_UPDATE, b, len);
}

/* ---- decision process and export ---- */

/* "AS 65002" or "AS 65002, our customer" */
static const char *peer_tag(const bstate *s, const peer *p)
{
    static char b[48];

    if (s->policy && p->rel != REL_NONE)
        snprintf(b, sizeof(b), "AS %u, %s", p->as, REL_NAME[p->rel]);
    else
        snprintf(b, sizeof(b), "AS %u", p->as);
    return b;
}

static int rel_pref(const bstate *s, const peer *p)
{
    if (!s->policy)
        return 100;
    return p->rel == REL_CUSTOMER ? 200 : p->rel == REL_PROVIDER ? 50 : 100;
}

static int may_export(const bstate *s, const loc_route *l, const peer *to)
{
    int from;

    if (l->src < 0 || !s->policy)
        return 1;
    from = s->p[l->src].rel;
    /* valley-free: what came from a peer or a provider goes only to customers */
    return from == REL_CUSTOMER || from == REL_NONE || to->rel == REL_CUSTOMER || to->rel == REL_NONE;
}

static loc_route *loc_find(loc_route *t, nl_ip pfx, int plen, int create)
{
    loc_route *slot = NULL;
    int i;

    for (i = 0; i < MAX_RIB; i++) {
        if (t[i].used && t[i].pfx == pfx && t[i].plen == plen)
            return &t[i];
        if (!t[i].used && !slot)
            slot = &t[i];
    }
    if (create && slot) {
        memset(slot, 0, sizeof(*slot));
        slot->used = 1;
        slot->pfx = pfx;
        slot->plen = plen;
        slot->src = -2;
    }
    return create ? slot : NULL;
}

static void bgp_process(void *ctx)
{
    bstate *s = ctx;
    nl_node *n = s->n;
    static loc_route nu[MAX_RIB];
    int i, k, j, routes = 0, est = 0;

    s->proc_ev = 0;
    memset(nu, 0, sizeof(nu));
    for (i = 0; i < s->nnet; i++) {
        loc_route *l = loc_find(nu, s->net[i].pfx, s->net[i].plen, 1);
        if (l) {
            l->src = -1;
            l->lp = 1000;
            l->why = N_("originated here");
        }
    }
    if (s->connected)
        for (i = 0; i < n->nif; i++)
            if (n->ifc[i].ip && nl_if_usable(n, i)) {
                loc_route *l = loc_find(nu, n->ifc[i].ip & nl_mask(n->ifc[i].plen), n->ifc[i].plen, 1);
                if (l) {
                    l->src = -1;
                    l->lp = 1000;
                    l->why = N_("connected network");
                }
            }
    for (k = 0; k < s->np; k++) {
        peer *p = &s->p[k];

        if (p->state != BS_ESTABLISHED)
            continue;
        est++;
        for (i = 0; i < MAX_RIB; i++) {
            rib_in *r = &p->in[i];
            loc_route *l;
            int lp = rel_pref(s, p);
            const char *why = NULL;

            if (!r->used || !(l = loc_find(nu, r->pfx, r->plen, 1)))
                continue;
            l->ncand++;
            if (l->src == -1)
                continue;
            if (l->src == -2)                                   why = N_("only path");
            else if (lp != l->lp)                               why = lp > l->lp ? N_("higher local preference") : NULL;
            else if (r->npath != l->npath)                      why = r->npath < l->npath ? N_("shorter AS path") : NULL;
            else if (p->id < s->p[l->src].id)                   why = N_("lowest router id");
            if (!why) {
                if (l->ncand == 2 && l->why && strcmp(l->why, "only path") == 0)
                    l->why = lp != l->lp ? N_("higher local preference")
                           : r->npath != l->npath ? N_("shorter AS path") : N_("lowest router id");
                continue;
            }
            l->src = k;
            l->lp = lp;
            l->npath = r->npath;
            memcpy(l->path, r->path, sizeof(l->path));
            l->nh = p->addr;
            l->why = why;
        }
    }
    /* forwarding table: follow the changes of the best paths */
    for (i = 0; i < MAX_RIB; i++) {
        loc_route *o = &s->loc[i], *l;

        if (!o->used)
            continue;
        l = loc_find(nu, o->pfx, o->plen, 0);
        if (o->src >= 0 && (!l || l->src < 0))
            nl_route_del(n, o->pfx, o->plen, NL_RT_BGP);
        if (!l)
            nl_log(n, NL_L_NET, 1, 0, T_WARN "BGP" T_RESET " %s/%d: %s", nl_ip_str(o->pfx), o->plen,
                   _("no path left, route withdrawn"));
    }
    for (i = 0; i < MAX_RIB; i++) {
        loc_route *l = &nu[i], *o;

        if (!l->used)
            continue;
        o = loc_find(s->loc, l->pfx, l->plen, 0);
        if (l->src >= 0) {
            routes++;
            nl_route_set(n, l->pfx, l->plen, l->nh, s->p[l->src].ifx, l->npath, NL_RT_BGP);
        }
        if (!o || o->src != l->src || o->npath != l->npath ||
            memcmp(o->path, l->path, sizeof(uint16_t) * (size_t)l->npath) != 0) {
            if (l->src >= 0)
                nl_log(n, NL_L_NET, 1, 0, T_OK "BGP" T_RESET " %s/%d: %s %s (%s) — AS path %s — %s",
                       nl_ip_str(l->pfx), l->plen, _("best path via"), s->p[l->src].name,
                       peer_tag(s, &s->p[l->src]), path_str(l->path, l->npath), _(l->why));
            else if (o)
                nl_log(n, NL_L_NET, 1, 0, T_OK "BGP" T_RESET " %s/%d: %s", nl_ip_str(l->pfx), l->plen, _(l->why));
        }
    }
    memcpy(s->loc, nu, sizeof(nu));
    nl_count_set(n, "bgp.routes", routes);
    nl_count_set(n, "bgp.established", est);

    /* tell every neighbor what changed for it */
    for (k = 0; k < s->np; k++) {
        peer *p = &s->p[k];

        if (p->state != BS_ESTABLISHED)
            continue;
        for (i = 0; i < MAX_RIB; i++) {
            rib_out *o = &p->out[i];
            loc_route *l;

            if (!o->used)
                continue;
            l = loc_find(s->loc, o->pfx, o->plen, 0);
            if (!l || l->src == k || !may_export(s, l, p)) {
                nl_log(n, NL_L_NET, 2, 0, T_WARN "BGP" T_RESET " → %s: withdraw %s/%d", p->name, nl_ip_str(o->pfx),
                       o->plen);
                send_update(s, p, o->pfx, o->plen, NULL, 0);
                o->used = 0;
            }
        }
        for (i = 0; i < MAX_RIB; i++) {
            loc_route *l = &s->loc[i];
            uint16_t path[MAX_PATH];
            rib_out *o = NULL, *slot = NULL;
            uint32_t sig = 2166136261u;
            int np = 0;

            if (!l->used || l->src == k)
                continue;
            if (!may_export(s, l, p)) {
                if (l->src >= 0)
                    nl_log(n, NL_L_NET, 3, 0, T_DIM "BGP %s/%d %s %s: %s", nl_ip_str(l->pfx), l->plen,
                           _("not exported to"), p->name, _("valley-free policy"));
                continue;
            }
            for (j = 0; j <= s->prepend && np < MAX_PATH; j++)
                path[np++] = s->as;
            for (j = 0; j < l->npath && np < MAX_PATH; j++)
                path[np++] = l->path[j];
            for (j = 0; j < np; j++)
                sig = (sig ^ path[j]) * 16777619u;
            sig |= 1;
            for (j = 0; j < MAX_RIB; j++) {
                if (p->out[j].used && p->out[j].pfx == l->pfx && p->out[j].plen == l->plen)
                    o = &p->out[j];
                if (!p->out[j].used && !slot)
                    slot = &p->out[j];
            }
            if (o && o->sig == sig)
                continue;
            if (!o && !(o = slot))
                continue;
            o->used = 1;
            o->pfx = l->pfx;
            o->plen = l->plen;
            o->sig = sig;
            nl_log(n, NL_L_NET, 2, 0, T_OK "BGP" T_RESET " → %s: announce %s/%d AS path %s", p->name,
                   nl_ip_str(l->pfx), l->plen, path_str(path, np));
            send_update(s, p, l->pfx, l->plen, path, np);
        }
    }
}

static void bgp_schedule(bstate *s)
{
    if (!s->proc_ev)
        s->proc_ev = nl_after(s->n, s->mrai, bgp_process, s);
}

/* ---- sessions ---- */

static void session_down(bstate *s, peer *p, const char *why)
{
    if (p->state == BS_ESTABLISHED) {
        nl_log(s->n, NL_L_NET, 1, 0, T_ERR "BGP" T_RESET " %s %s (AS %u) %s: %s", _("session with"), p->name, p->as,
               _("is down"), why);
        nl_count(s->n, "bgp.session_drops", 1);
    }
    p->state = BS_IDLE;
    memset(p->in, 0, sizeof(p->in));
    memset(p->out, 0, sizeof(p->out));
    bgp_schedule(s);
}

static void bgp_tick(void *ctx)
{
    bstate *s = ctx;
    nl_time now = nl_now();
    int k;

    for (k = 0; k < s->np; k++) {
        peer *p = &s->p[k];

        if (!nl_if_usable(s->n, p->ifx)) {
            if (p->state != BS_IDLE)
                session_down(s, p, _("interface down"));
            continue;
        }
        if (p->state == BS_IDLE || p->state == BS_OPENSENT) {
            if (!p->last_open || now - p->last_open >= s->keepalive) {
                nl_log(s->n, NL_L_NET, 2, 0, T_OK "BGP" T_RESET " → %s: OPEN (AS %u, hold %s)", p->name, s->as,
                       nl_time_str(s->hold));
                send_open(s, p);
                p->state = BS_OPENSENT;
            }
            continue;
        }
        if (now - p->last_rx > s->hold) {
            send_notification(s, p, 4, 0);
            session_down(s, p, _("hold timer expired: nothing heard"));
            continue;
        }
        if (now - p->last_tx >= s->keepalive)
            send_keepalive(s, p);
    }
    if (s->connected)
        bgp_schedule(s);                /* connected networks may have changed */
    s->tick_ev = nl_after(s->n, s->keepalive / 3 > NL_SEC(1) ? NL_SEC(1) : s->keepalive / 3, bgp_tick, s);
}

/* RPKI-style origin validation: 0 = unknown, 1 = valid, -1 = invalid */
static int rov_check(const bstate *s, nl_ip pfx, int plen, uint16_t origin)
{
    int i, covered = 0;

    for (i = 0; i < s->nroa; i++) {
        if (plen < s->roa[i].plen || (pfx & nl_mask(s->roa[i].plen)) != s->roa[i].pfx)
            continue;
        covered = 1;
        if (s->roa[i].as == origin && plen <= s->roa[i].maxlen)
            return 1;
    }
    return covered ? -1 : 0;
}

static void rx_update(bstate *s, peer *p, const nl_udpmsg *m, const uint8_t *b, int len)
{
    uint16_t path[MAX_PATH];
    int wlen, alen, off, npath = 0, i, used, plen;
    nl_ip pfx, nh = p->addr;

    if (len < 4)
        return;
    p->rx_updates++;
    nl_count(s->n, "bgp.updates_rcvd", 1);
    wlen = nl_get16(b);
    if (2 + wlen + 2 > len)
        return;
    for (off = 2; off < 2 + wlen; off += used) {
        used = get_prefix(b + off, 2 + wlen - off, &pfx, &plen);
        if (used < 0)
            return;
        nl_log(s->n, NL_L_NET, 2, m->pkt_id, T_WARN "BGP" T_RESET " ← %s: withdraw %s/%d", p->name, nl_ip_str(pfx),
               plen);
        for (i = 0; i < MAX_RIB; i++)
            if (p->in[i].used && p->in[i].pfx == pfx && p->in[i].plen == plen)
                p->in[i].used = 0;
    }
    alen = nl_get16(b + off);
    off += 2;
    if (off + alen > len)
        return;
    for (i = off; i + 3 <= off + alen; ) {
        int type = b[i + 1], l = b[i + 2], k;
        const uint8_t *v = b + i + 3;

        if (i + 3 + l > off + alen)
            break;
        if (type == 2 && l >= 2) {
            for (k = 0; k < v[1] && k < MAX_PATH && 2 + (k + 1) * 2 <= l; k++)
                path[npath++] = nl_get16(v + 2 + k * 2);
        } else if (type == 3 && l == 4) {
            nh = nl_get32(v);
        }
        i += 3 + l;
    }
    for (off += alen; off < len; off += used) {
        rib_in *r = NULL, *slot = NULL;
        int rv;

        used = get_prefix(b + off, len - off, &pfx, &plen);
        if (used < 0)
            return;
        for (i = 0; i < MAX_RIB; i++) {
            if (p->in[i].used && p->in[i].pfx == pfx && p->in[i].plen == plen)
                r = &p->in[i];
            if (!p->in[i].used && !slot)
                slot = &p->in[i];
        }
        nl_log(s->n, NL_L_NET, 2, m->pkt_id, T_OK "BGP" T_RESET " ← %s: %s/%d AS path %s", p->name, nl_ip_str(pfx),
               plen, path_str(path, npath));
        for (i = 0; i < npath; i++)
            if (path[i] == s->as)
                break;
        if (i < npath) {
            nl_log(s->n, NL_L_NET, 2, m->pkt_id, T_WARN "BGP" T_RESET " %s/%d: %s", nl_ip_str(pfx), plen,
                   _("our own AS is in the path: loop, rejected"));
            nl_count(s->n, "bgp.loop_rejected", 1);
            if (r)
                r->used = 0;
            continue;
        }
        rv = s->rov && npath ? rov_check(s, pfx, plen, path[npath - 1]) : 0;
        if (rv < 0) {
            nl_log(s->n, NL_L_NET, 1, m->pkt_id, T_ERR "BGP" T_RESET " %s/%d %s AS %u: %s", nl_ip_str(pfx), plen,
                   _("originated by"), path[npath - 1],
                   _("origin validation says INVALID (wrong origin AS or too specific): rejected"));
            nl_count(s->n, "bgp.rov_rejected", 1);
            if (r)
                r->used = 0;
            continue;
        }
        if (!r && !(r = slot))
            continue;
        r->used = 1;
        r->pfx = pfx;
        r->plen = plen;
        r->npath = npath;
        memcpy(r->path, path, sizeof(path));
        r->nh = nh;
    }
    bgp_schedule(s);
}

static void bgp_rx(nl_node *n, void *ctx, const nl_udpmsg *m)
{
    bstate *s = ctx;
    peer *p = NULL;
    const uint8_t *b = m->data;
    int k, len;

    for (k = 0; k < s->np; k++)
        if (s->p[k].addr == m->src)
            p = &s->p[k];
    if (!p || m->len < BHDR || (len = nl_get16(b + 16)) > m->len || len < BHDR)
        return;
    p->last_rx = nl_now();
    switch (b[18]) {
    case M_OPEN:
        if (len < BHDR + 10)
            return;
        if (nl_get16(b + BHDR + 1) != p->as) {
            nl_log(n, NL_L_NET, 1, m->pkt_id, T_ERR "BGP" T_RESET " OPEN %s %s: AS %u, %s %u — %s", _("from"),
                   p->name, nl_get16(b + BHDR + 1), _("expected"), p->as, _("session refused"));
            send_notification(s, p, 2, 2);          /* OPEN error: bad peer AS */
            return;
        }
        if (p->state == BS_ESTABLISHED) {
            if (nl_now() - p->since < s->keepalive * 2)
                return;                             /* a late copy of the OPEN we already answered */
            session_down(s, p, _("the neighbor restarted"));
        }
        p->id = nl_get32(b + BHDR + 5);
        if (p->state == BS_IDLE)
            send_open(s, p);
        nl_log(n, NL_L_NET, 2, m->pkt_id, T_OK "BGP" T_RESET " ← %s: OPEN (AS %u, id %s) → KEEPALIVE", p->name,
               p->as, nl_ip_str(p->id));
        send_keepalive(s, p);
        p->state = BS_OPENCONFIRM;
        break;
    case M_KEEPALIVE:
        if (p->state == BS_OPENCONFIRM) {
            p->state = BS_ESTABLISHED;
            p->since = nl_now();
            nl_log(n, NL_L_NET, 1, m->pkt_id, T_OK "BGP" T_RESET " %s %s (%s) %s", _("session with"), p->name,
                   peer_tag(s, p), _("Established: exchanging routes"));
            bgp_schedule(s);
        }
        break;
    case M_UPDATE:
        if (p->state == BS_ESTABLISHED)
            rx_update(s, p, m, b + BHDR, len - BHDR);
        break;
    case M_NOTIFICATION:
        nl_log(n, NL_L_NET, 2, m->pkt_id, T_ERR "BGP" T_RESET " ← %s: NOTIFICATION %d/%d", p->name,
               len > BHDR ? b[BHDR] : 0, len > BHDR + 1 ? b[BHDR + 1] : 0);
        if (p->state != BS_IDLE)
            session_down(s, p, _("the neighbor closed it (NOTIFICATION)"));
        break;
    default:
        break;
    }
}

/* ---- configuration ---- */

/* The address of node `other` on a network we share with it */
static int shared_addr(const nl_node *n, const nl_node *other, nl_ip *addr)
{
    int i, k;

    for (i = 0; i < n->nif; i++)
        for (k = 0; k < other->nif; k++)
            if (n->ifc[i].ip && other->ifc[k].ip && n->ifc[i].plen == other->ifc[k].plen &&
                ((n->ifc[i].ip ^ other->ifc[k].ip) & nl_mask(n->ifc[i].plen)) == 0) {
                *addr = other->ifc[k].ip;
                return 1;
            }
    return 0;
}

static int add_peer(bstate *s, const char *spec)
{
    char buf[64], *as, *rel;
    nl_node *other;
    peer *p;
    int i;

    snprintf(buf, sizeof(buf), "%s", spec);
    as = strchr(buf, ':');
    if (!as || s->np >= MAX_PEER)
        return -1;
    *as++ = '\0';
    rel = strchr(as, ':');
    if (rel)
        *rel++ = '\0';
    p = &s->p[s->np];
    memset(p, 0, sizeof(*p));
    snprintf(p->name, sizeof(p->name), "%.15s", buf);
    p->as = (uint16_t)atoi(as);
    other = nl_node_find(buf);
    if (other ? !shared_addr(s->n, other, &p->addr) : !nl_parse_ip(buf, &p->addr))
        return -1;
    p->ifx = -1;
    for (i = 0; i < s->n->nif; i++)
        if (s->n->ifc[i].ip && ((s->n->ifc[i].ip ^ p->addr) & nl_mask(s->n->ifc[i].plen)) == 0)
            p->ifx = i;
    if (p->ifx < 0 || !p->as || p->as == s->as)
        return -1;
    if (rel) {
        if (!strcmp(rel, "customer"))      p->rel = REL_CUSTOMER;
        else if (!strcmp(rel, "peer"))     p->rel = REL_PEER;
        else if (!strcmp(rel, "provider")) p->rel = REL_PROVIDER;
        else return -1;
    }
    s->np++;
    return 0;
}

static int net_index(const bstate *s, nl_ip pfx, int plen)
{
    int i;

    for (i = 0; i < s->nnet; i++)
        if (s->net[i].pfx == pfx && s->net[i].plen == plen)
            return i;
    return -1;
}

static int bgp_announce(bstate *s, const char *spec, int on)
{
    nl_ip pfx;
    int plen, i;

    if (!spec || !nl_parse_prefix(spec, &pfx, &plen))
        return -1;
    pfx &= nl_mask(plen);
    i = net_index(s, pfx, plen);
    if (on && i < 0 && s->nnet < MAX_NET) {
        s->net[s->nnet].pfx = pfx;
        s->net[s->nnet].plen = plen;
        s->nnet++;
    } else if (!on && i >= 0) {
        s->net[i] = s->net[--s->nnet];
    }
    return 0;
}

static int bgp_run(nl_node *n, const nl_args *a)
{
    bstate *s = nl_mod_get(n, &bgp_mod);
    const char *sub = nl_arg_pos(a, 0);
    int i;

    if (s) {
        if (sub && (!strcmp(sub, "announce") || !strcmp(sub, "withdraw"))) {
            int on = sub[0] == 'a';
            if (bgp_announce(s, nl_arg_pos(a, 1), on) != 0) {
                nl_log(n, NL_L_NET, 1, 0, T_ERR "bgp: %s", _("usage: bgp announce|withdraw PREFIX/LEN"));
                return -1;
            }
            nl_log(n, NL_L_NET, 1, 0, T_INFO "BGP" T_RESET " AS %u %s %s", s->as,
                   on ? _("now originates") : _("stops originating"), nl_arg_pos(a, 1));
            bgp_schedule(s);
        }
        return 0;
    }
    s = calloc(1, sizeof(*s));
    if (!s)
        return -1;
    s->n = n;
    s->as = (uint16_t)nl_arg_int(a, "as", 0);
    s->keepalive = nl_arg_time(a, "keepalive", NL_SEC(30));
    if (s->keepalive < NL_MS(300))
        s->keepalive = NL_MS(300);
    s->hold = nl_arg_time(a, "hold", s->keepalive * 3);
    s->mrai = nl_arg_time(a, "mrai", NL_MS(500));
    s->prepend = (int)nl_arg_int(a, "prepend", 0);
    s->rov = (int)nl_arg_int(a, "rov", 0);
    s->policy = (int)nl_arg_int(a, "policy", 1);
    s->connected = (int)nl_arg_int(a, "connected", 0);
    for (i = 0; i < n->nif; i++)
        if (n->ifc[i].ip > s->id)
            s->id = n->ifc[i].ip;
    if (!s->as) {
        nl_log(n, NL_L_NET, 1, 0, T_ERR "bgp: %s", _("as=NUMBER is required"));
        free(s);
        return -1;
    }
    for (i = 0; i < a->argc; i++) {
        const char *t = a->argv[i];

        if (!strncmp(t, "neighbor=", 9) && add_peer(s, t + 9) != 0) {
            nl_log(n, NL_L_NET, 1, 0, T_ERR "bgp: %s '%s' %s", _("bad neighbor"), t + 9,
                   _("(NODE:AS[:customer|peer|provider], on a shared network, different AS)"));
            free(s);
            return -1;
        }
        if (!strncmp(t, "network=", 8) && bgp_announce(s, t + 8, 1) != 0) {
            nl_log(n, NL_L_NET, 1, 0, T_ERR "bgp: %s '%s'", _("bad network"), t + 8);
            free(s);
            return -1;
        }
        if (!strncmp(t, "roa=", 4) && s->nroa < MAX_ROA) {
            char buf[64], *as, *ml;

            snprintf(buf, sizeof(buf), "%s", t + 4);
            as = strchr(buf, ':');
            if (as) {
                *as++ = '\0';
                ml = strchr(as, ':');
                if (ml)
                    *ml++ = '\0';
                if (nl_parse_prefix(buf, &s->roa[s->nroa].pfx, &s->roa[s->nroa].plen)) {
                    s->roa[s->nroa].as = (uint16_t)atoi(as);
                    s->roa[s->nroa].maxlen = ml ? atoi(ml) : s->roa[s->nroa].plen;
                    s->nroa++;
                }
            }
        }
    }
    n->forward = 1;
    nl_mod_set(n, &bgp_mod, s);
    nl_udp_bind(n, BGP_PORT, bgp_rx, s);
    nl_log(n, NL_L_NET, 2, 0, T_OK "BGP" T_RESET " AS %u, id %s: %d %s, %d %s", s->as, nl_ip_str(s->id), s->np,
           _("neighbors"), s->nnet, _("networks originated"));
    s->tick_ev = nl_after(n, (nl_time)nl_rand(100000), bgp_tick, s);
    bgp_schedule(s);
    return 0;
}

static void bgp_reset(bstate *s)
{
    int k;

    nl_cancel(&s->tick_ev);
    nl_cancel(&s->proc_ev);
    for (k = 0; k < s->np; k++) {
        s->p[k].state = BS_IDLE;
        s->p[k].last_open = 0;
        memset(s->p[k].in, 0, sizeof(s->p[k].in));
        memset(s->p[k].out, 0, sizeof(s->p[k].out));
    }
    memset(s->loc, 0, sizeof(s->loc));
    nl_route_flush(s->n, NL_RT_BGP);
}

static void bgp_stop(nl_node *n)
{
    bstate *s = nl_mod_get(n, &bgp_mod);

    if (!s)
        return;
    bgp_reset(s);
    nl_udp_unbind(n, BGP_PORT);
    nl_mod_set(n, &bgp_mod, NULL);
    free(s);
}

static void bgp_crash(nl_node *n) { bgp_reset(nl_mod_get(n, &bgp_mod)); }

static void bgp_recover(nl_node *n)
{
    bstate *s = nl_mod_get(n, &bgp_mod);

    s->tick_ev = nl_after(n, NL_MS(100), bgp_tick, s);
    bgp_schedule(s);
}

static int bgp_panel(nl_node *n, int x, int y, int w, int h, int detail)
{
    bstate *s = nl_mod_get(n, &bgp_mod);
    int i, k, row = 0;

    row += nl_ui_kv(x, y + row, w, _("autonomous system"), "%u  (id %s)%s%s", s->as, nl_ip_str(s->id),
                    s->policy ? "" : _("  policies off"), s->rov ? _("  origin validation on") : "");
    if (row < h)
        tui_print(x, y + row++, w, TS(C_DIM, C_DEFAULT, 0), "%-8s %-6s %-9s %-12s %s", _("neighbor"), "AS",
                  _("relation"), _("state"), _("updates in/out"));
    for (k = 0; k < s->np && row < h; k++) {
        peer *p = &s->p[k];
        tui_print(x, y + row++, w, TS(p->state == BS_ESTABLISHED ? C_OK : C_WARN, C_DEFAULT, 0),
                  "%-8s %-6u %-9s %-12s %u/%u", p->name, p->as, REL_NAME[p->rel], ST_NAME[p->state], p->rx_updates,
                  p->tx_updates);
    }
    if (row < h)
        tui_print(x, y + row++, w, TS(C_DIM, C_DEFAULT, 0), "%-18s %-8s %-18s %s", _("prefix"), _("via"),
                  _("AS path"), _("why it is the best"));
    for (i = 0; i < MAX_RIB && row < h; i++) {
        loc_route *l = &s->loc[i];
        char net[24];

        if (!l->used)
            continue;
        snprintf(net, sizeof(net), "%s/%d", nl_ip_str(l->pfx), l->plen);
        tui_print(x, y + row++, w, TS(l->src < 0 ? C_INFO : C_TEXT, C_DEFAULT, 0), "%-18s %-8s %-18s %s", net,
                  l->src < 0 ? "-" : s->p[l->src].name, path_str(l->path, l->npath), l->why ? _(l->why) : "");
        if (detail < 3)
            continue;
        for (k = 0; k < s->np && row < h; k++) {       /* the alternatives heard (Adj-RIB-In) */
            int j;
            for (j = 0; j < MAX_RIB; j++)
                if (s->p[k].in[j].used && s->p[k].in[j].pfx == l->pfx && s->p[k].in[j].plen == l->plen && k != l->src)
                    tui_print(x, y + row++, w, TS(C_DIM, C_DEFAULT, 0), "%-18s %-8s %s", "", s->p[k].name,
                              path_str(s->p[k].in[j].path, s->p[k].in[j].npath));
        }
    }
    return row;
}

static const nl_module bgp_mod = {
    .name = "bgp", .title = N_("BGP — path-vector routing between autonomous systems"), .layer = NL_L_NET,
    .usage = N_("run ROUTER bgp as=N [network=PREFIX/LEN ...] [neighbor=NODE:AS[:RELATION] ...]\n"
                "               [connected=1] [keepalive=30s] [hold=3×keepalive] [mrai=500ms]\n"
                "               [prepend=N] [policy=1] [rov=1 roa=PREFIX/LEN:AS[:MAXLEN] ...]\n"
                "run ROUTER bgp announce|withdraw PREFIX/LEN\n"
                "  network= and neighbor= can be repeated. RELATION says what the neighbor is for\n"
                "  this AS: customer, peer or provider (none: no policy on that session).\n"
                "  connected=1 also originates every connected network.\n"
                "  policy=0 ignores the relations; prepend=N lengthens the AS path we export.\n"
                "  Nothing checks that an AS owns what it announces: that is the hijack lab;\n"
                "  rov=1 with roa= entries rejects announcements with the wrong origin.\n"
                "  Sessions use UDP port 179 here (TCP in real BGP): use loss-free links."),
    .theory = N_("The Internet is a network of autonomous systems (AS): networks under one administration, "
                 "each with its own interior routing. BGP is the protocol they use to tell each other which "
                 "address blocks (prefixes) they can reach. It is a path-vector protocol: an announcement "
                 "carries the whole list of ASes it has crossed (the AS path). When a router passes a route "
                 "on, it prepends its own AS number; a router that finds its own number in a path discards "
                 "it, which prevents loops without any count-to-infinity. When a route disappears an explicit "
                 "withdrawal is sent. Two BGP routers keep a session alive with KEEPALIVE messages and tear "
                 "it down, withdrawing everything learned over it, when the hold timer expires.\n\n"
                 "Unlike interior protocols, BGP does not look for the shortest path: it applies policy. "
                 "Each AS first ranks the routes it hears by local preference, which encodes business "
                 "relations (a route through a customer earns money, through a peer is free, through a "
                 "provider costs), and only then by AS-path length. Export follows the same logic "
                 "(Gao-Rexford rules): routes learned from customers are announced to everyone, routes "
                 "learned from peers and providers only to customers. The result is that paths are "
                 "valley-free: they go up through providers, cross at most one peering link, and go down "
                 "through customers; nobody carries traffic for free between two of its providers.\n\n"
                 "BGP trusts what it is told. An AS that announces a prefix it does not own attracts the "
                 "traffic of everyone for whom its path looks better (prefix hijack), and announcing a more "
                 "specific prefix attracts everyone, because forwarding always follows the longest match. "
                 "Route origin validation (RPKI) lets routers reject announcements whose origin AS or prefix "
                 "length does not match a signed authorization.\n\n"
                 "Simplified here: sessions over UDP instead of TCP, eBGP only, one router per AS, no MED, "
                 "communities or aggregation."),
    .run = bgp_run, .stop = bgp_stop, .crash = bgp_crash, .recover = bgp_recover, .panel = bgp_panel,
};
NL_MODULE(bgp_mod)

static void dis_bgp(nl_dissect *o, const uint8_t *d, int len, int off)
{
    int type, mlen, wlen, alen, p, used, plen, i;
    nl_ip pfx;
    char path[120] = "", first[40] = "";
    size_t pl = 0;

    if (len < BHDR)
        return;
    mlen = nl_get16(d + 16);
    type = d[18];
    if (mlen > len)
        mlen = len;
    nl_dis_field(o, "BGP", "Marker", off, 16, "16 × 0xff");
    nl_dis_field(o, "BGP", "Length", off + 16, 2, "%d", mlen);
    nl_dis_field(o, "BGP", "Type", off + 18, 1, "%d (%s)", type,
                 type == M_OPEN ? "OPEN" : type == M_UPDATE ? "UPDATE" : type == M_KEEPALIVE ? "KEEPALIVE"
                 : type == M_NOTIFICATION ? "NOTIFICATION" : "?");
    if (type == M_OPEN && mlen >= BHDR + 10) {
        nl_dis_field(o, "BGP", "Version", off + BHDR, 1, "%d", d[BHDR]);
        nl_dis_field(o, "BGP", "My AS", off + BHDR + 1, 2, "%u", nl_get16(d + BHDR + 1));
        nl_dis_field(o, "BGP", "Hold time", off + BHDR + 3, 2, "%u s", nl_get16(d + BHDR + 3));
        nl_dis_field(o, "BGP", "BGP identifier", off + BHDR + 5, 4, "%s", nl_ip_str(nl_get32(d + BHDR + 5)));
        nl_dis_summary(o, "BGP", BGP_COLOR, "OPEN AS %u hold %us", nl_get16(d + BHDR + 1), nl_get16(d + BHDR + 3));
    } else if (type == M_KEEPALIVE) {
        nl_dis_summary(o, "BGP", BGP_COLOR, "KEEPALIVE");
    } else if (type == M_NOTIFICATION && mlen >= BHDR + 2) {
        nl_dis_field(o, "BGP", "Error code / subcode", off + BHDR, 2, "%d / %d", d[BHDR], d[BHDR + 1]);
        nl_dis_summary(o, "BGP", BGP_COLOR, "NOTIFICATION error %d/%d%s", d[BHDR], d[BHDR + 1],
                       d[BHDR] == 4 ? " (hold timer expired)" : d[BHDR] == 2 ? " (OPEN error)" : "");
    } else if (type == M_UPDATE && mlen >= BHDR + 4) {
        int nw = 0, nn = 0;

        wlen = nl_get16(d + BHDR);
        nl_dis_field(o, "BGP", "Withdrawn length", off + BHDR, 2, "%d", wlen);
        for (p = BHDR + 2; p < BHDR + 2 + wlen && p < mlen; p += used) {
            used = get_prefix(d + p, mlen - p, &pfx, &plen);
            if (used < 0)
                return;
            nl_dis_field(o, "BGP", "Withdrawn route", off + p, used, "%s/%d", nl_ip_str(pfx), plen);
            if (!nw++)
                snprintf(first, sizeof(first), "%s/%d", nl_ip_str(pfx), plen);
        }
        if (p + 2 > mlen)
            return;
        alen = nl_get16(d + p);
        nl_dis_field(o, "BGP", "Attributes length", off + p, 2, "%d", alen);
        p += 2;
        for (i = p; i + 3 <= p + alen && i + 3 <= mlen; i += 3 + d[i + 2]) {
            const uint8_t *v = d + i + 3;
            int l = d[i + 2], k;

            if (i + 3 + l > mlen)
                break;
            if (d[i + 1] == 1) {
                nl_dis_field(o, "BGP", "ORIGIN", off + i, 3 + l, "%s", v[0] == 0 ? "IGP" : v[0] == 1 ? "EGP" : "incomplete");
            } else if (d[i + 1] == 2 && l >= 2) {
                for (k = 0; k < v[1] && 2 + (k + 1) * 2 <= l && pl < sizeof(path) - 8; k++)
                    pl += (size_t)snprintf(path + pl, sizeof(path) - pl, "%s%u", k ? " " : "", nl_get16(v + 2 + k * 2));
                nl_dis_field(o, "BGP", "AS_PATH", off + i, 3 + l, "%s", path);
            } else if (d[i + 1] == 3 && l == 4) {
                nl_dis_field(o, "BGP", "NEXT_HOP", off + i, 3 + l, "%s", nl_ip_str(nl_get32(v)));
            } else {
                nl_dis_field(o, "BGP", "Attribute", off + i, 3 + l, "type %d", d[i + 1]);
            }
        }
        for (p += alen; p < mlen; p += used) {
            used = get_prefix(d + p, mlen - p, &pfx, &plen);
            if (used < 0)
                return;
            nl_dis_field(o, "BGP", "NLRI (announced)", off + p, used, "%s/%d", nl_ip_str(pfx), plen);
            if (!nn++)
                snprintf(first, sizeof(first), "%s/%d", nl_ip_str(pfx), plen);
        }
        if (nn)
            nl_dis_summary(o, "BGP", BGP_COLOR, "UPDATE %s AS path %s", first, path);
        else
            nl_dis_summary(o, "BGP", BGP_COLOR, "UPDATE withdraw %s", first);
    } else {
        nl_dis_summary(o, "BGP", BGP_COLOR, "message type %d", type);
    }
}

NL_INIT(bgp)
{
    nl_dissector_udp(BGP_PORT, dis_bgp);
}
