/*
 * bgp.c - BGP-4 (RFC 4271): path-vector routing between autonomous systems.
 *
 * Real: sessions over TCP port 179 with the RFC 4271 message framing
 * (16-byte marker, length, type) reassembled from the byte stream; OPEN
 * with the four-octet AS capability (RFC 6793), UPDATE with withdrawn
 * routes, ORIGIN / AS_PATH / NEXT_HOP attributes and NLRI, KEEPALIVE and
 * NOTIFICATION with error codes; the finite state machine (Idle, Connect,
 * Active, OpenSent, OpenConfirm, Established) with ConnectRetry, Hold and
 * Keepalive timers, hold-time negotiation and connection collision
 * resolution; loop detection by AS path, the decision process (local
 * preference, AS-path length, router id), per-neighbor export with
 * Gao-Rexford (valley-free) policies, withdrawals, AS-path prepending and
 * RPKI-style origin validation.
 *
 * Not modelled: iBGP (one router per AS, neighbors directly connected), MED,
 * communities, aggregation, route-flap damping, graceful restart; one prefix
 * per UPDATE; local preference is local policy derived from the business
 * relation (it is never sent, as on real eBGP sessions).
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
#define BGP_MAXMSG 4096
#define AS_TRANS   23456

enum { M_OPEN = 1, M_UPDATE = 2, M_NOTIFICATION = 3, M_KEEPALIVE = 4 };
enum { REL_NONE, REL_CUSTOMER, REL_PEER, REL_PROVIDER };
enum { BS_IDLE, BS_CONNECT, BS_ACTIVE, BS_OPENSENT, BS_OPENCONFIRM, BS_ESTABLISHED };
/* NOTIFICATION error codes (RFC 4271 section 4.5) */
enum { E_HEADER = 1, E_OPEN = 2, E_UPDATE = 3, E_HOLD = 4, E_FSM = 5, E_CEASE = 6 };

static const char *const REL_NAME[] = { "-", "customer", "peer", "provider" };
static const char *const ST_NAME[] = { "Idle", "Connect", "Active", "OpenSent", "OpenConfirm", "Established" };

typedef struct {
    int      used;
    nl_ip    pfx;
    int      plen;
    uint32_t path[MAX_PATH];
    int      npath;
    nl_ip    nh;
} rib_in;

typedef struct { int used; nl_ip pfx; int plen; uint32_t sig; } rib_out;

struct bstate;

/* One TCP connection towards a neighbor. Both sides may open one at the
 * same time, so a neighbor has two slots until the collision is resolved. */
typedef struct bconn {
    struct bstate *s;
    int      peer, dir;         /* dir 0: we opened it; 1: the neighbor did */
    nl_tcp  *c;
    int      st;                /* 0 = unused, else BS_CONNECT .. BS_ESTABLISHED */
    int      as4;               /* the neighbor announced the four-octet AS capability */
    nl_time  last_rx, last_tx, t_open, hold;
    int      len;
    uint8_t  buf[BGP_MAXMSG];   /* bytes of the stream not yet forming a whole message */
} bconn;

typedef struct {
    char     name[16];
    nl_ip    addr, id;
    int      ifx, rel, prepend;
    int      est;               /* slot of the established connection, or -1 */
    int      tried;             /* a connection attempt failed: Active rather than Idle */
    uint32_t as;
    nl_time  next_try, since;
    bconn    conn[2];
    rib_in   in[MAX_RIB];
    rib_out  out[MAX_RIB];
    unsigned rx_updates, tx_updates;
} peer;

typedef struct {
    int      used;
    nl_ip    pfx;
    int      plen;
    int      src;               /* peer index, -1 = originated here */
    uint32_t path[MAX_PATH];
    int      npath, lp, ncand;
    nl_ip    nh;
    const char *why;
} loc_route;

typedef struct bstate {
    nl_node  *n;
    uint32_t  as;
    nl_ip     id;
    nl_time   keepalive, hold, mrai, retry;
    int       prepend, rov, policy, connected;
    peer      p[MAX_PEER];
    int       np;
    struct { nl_ip pfx; int plen; } net[MAX_NET], sup[MAX_NET];   /* originated / suppressed */
    int       nnet, nsup;
    struct { nl_ip pfx; int plen, maxlen; uint32_t as; } roa[MAX_ROA];
    int       nroa;
    loc_route loc[MAX_RIB];
    nl_ev     tick_ev, proc_ev;
} bstate;

static const nl_module bgp_mod;
static void bgp_schedule(struct bstate *s);

static const char *path_str(const uint32_t *path, int n)
{
    static char bufs[4][MAX_PATH * 11 + 8];
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

static int peer_state(const peer *p)
{
    int st = p->conn[0].st > p->conn[1].st ? p->conn[0].st : p->conn[1].st;

    return st ? st : p->tried ? BS_ACTIVE : BS_IDLE;
}

/* Write one message on a connection: the header goes in the first 19 bytes of b */
static void bgp_send(bconn *bc, int type, uint8_t *b, int len)
{
    if (!bc->c)
        return;
    memset(b, 0xff, 16);
    nl_put16(b + 16, (uint16_t)len);
    b[18] = (uint8_t)type;
    nl_tcp_write(bc->c, b, len);
    bc->last_tx = nl_now();
}

static void send_open(bconn *bc)
{
    bstate *s = bc->s;
    uint8_t b[BHDR + 10 + 8];

    b[BHDR] = 4;
    nl_put16(b + BHDR + 1, (uint16_t)(s->as > 65535 ? AS_TRANS : s->as));
    nl_put16(b + BHDR + 3, (uint16_t)(s->hold / NL_SEC(1)));
    nl_put32(b + BHDR + 5, s->id);
    b[BHDR + 9] = 8;                    /* optional parameters: one, the capabilities */
    b[BHDR + 10] = 2;
    b[BHDR + 11] = 6;
    b[BHDR + 12] = 65;                  /* four-octet AS numbers (RFC 6793) */
    b[BHDR + 13] = 4;
    nl_put32(b + BHDR + 14, s->as);
    bgp_send(bc, M_OPEN, b, sizeof(b));
}

static void send_keepalive(bconn *bc)
{
    uint8_t b[BHDR];

    bgp_send(bc, M_KEEPALIVE, b, BHDR);
}

static const char *notif_name(int code, int sub)
{
    switch (code) {
    case E_HEADER: return sub == 1 ? "connection not synchronized" : sub == 2 ? "bad message length" : "bad message type";
    case E_OPEN:   return sub == 1 ? "unsupported version" : sub == 2 ? "bad peer AS" : sub == 6 ? "unacceptable hold time" : "OPEN error";
    case E_UPDATE: return "UPDATE error";
    case E_HOLD:   return "hold timer expired";
    case E_FSM:    return "finite state machine error";
    case E_CEASE:  return sub == 7 ? "cease: connection collision resolution" : sub == 2 ? "cease: administrative shutdown" : "cease";
    default:       return "?";
    }
}

/* Give a connection up. code != 0: say why with a NOTIFICATION and close in
 * an orderly way (the FIN follows the message); hard: reset it at once. */
static void conn_drop(bconn *bc, int code, int sub, int hard)
{
    if (bc->c) {
        nl_tcp *c = bc->c;

        if (code && bc->st >= BS_OPENSENT) {
            uint8_t b[BHDR + 2];

            b[BHDR] = (uint8_t)code;
            b[BHDR + 1] = (uint8_t)sub;
            nl_log(bc->s->n, NL_L_NET, 2, 0, T_ERR "BGP" T_RESET " → %s: NOTIFICATION %d/%d (%s)",
                   bc->s->p[bc->peer].name, code, sub, notif_name(code, sub));
            bgp_send(bc, M_NOTIFICATION, b, sizeof(b));
        }
        bc->c = NULL;                   /* callbacks still to come for it are ignored */
        if (hard)
            nl_tcp_abort(c);
        else
            nl_tcp_close(c);
    }
    bc->st = 0;
    bc->len = 0;
    bc->as4 = 0;
}

static void send_update(bstate *s, peer *p, nl_ip pfx, int plen, const uint32_t *path, int npath)
{
    uint8_t b[BHDR + 2 + 5 + 2 + 4 + 3 + 2 + MAX_PATH * 4 + 7 + 5];
    bconn *bc = p->est >= 0 ? &p->conn[p->est] : NULL;
    int len = BHDR, alen, i, w = bc && bc->as4 ? 4 : 2;
    uint8_t *a;

    if (!bc)
        return;
    if (!path) {                        /* withdrawal */
        int wl = put_prefix(b + len + 2, pfx, plen);
        nl_put16(b + len, (uint16_t)wl);
        len += 2 + wl;
        nl_put16(b + len, 0);
        len += 2;
    } else {
        nl_put16(b + len, 0);
        len += 2;
        a = b + len + 2;
        alen = 0;
        a[alen++] = 0x40; a[alen++] = 1; a[alen++] = 1; a[alen++] = 0;      /* ORIGIN: IGP */
        a[alen++] = 0x40; a[alen++] = 2; a[alen++] = (uint8_t)(2 + npath * w);
        a[alen++] = 2;                                                       /* AS_SEQUENCE */
        a[alen++] = (uint8_t)npath;
        for (i = 0; i < npath; i++) {
            if (w == 4)
                nl_put32(a + alen, path[i]);
            else
                nl_put16(a + alen, (uint16_t)(path[i] > 65535 ? AS_TRANS : path[i]));
            alen += w;
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
    bgp_send(bc, M_UPDATE, b, len);
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
                nl_ip net = n->ifc[i].ip & nl_mask(n->ifc[i].plen);
                loc_route *l;

                for (j = 0; j < s->nsup; j++)
                    if (s->sup[j].pfx == net && s->sup[j].plen == n->ifc[i].plen)
                        break;
                if (j < s->nsup)
                    continue;           /* withdrawn by the operator */
                l = loc_find(nu, net, n->ifc[i].plen, 1);
                if (l) {
                    l->src = -1;
                    l->lp = 1000;
                    l->why = N_("connected network");
                }
            }
    for (k = 0; k < s->np; k++) {
        peer *p = &s->p[k];

        if (p->est < 0)
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
            memcmp(o->path, l->path, sizeof(uint32_t) * (size_t)l->npath) != 0) {
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

        if (p->est < 0)
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
            uint32_t path[MAX_PATH];
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
            for (j = 0; j <= p->prepend && np < MAX_PATH; j++)
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

static void bgp_schedule(struct bstate *s)
{
    if (!s->proc_ev)
        s->proc_ev = nl_after(s->n, s->mrai, bgp_process, s);
}

/* ---- sessions ---- */

/* The session with a neighbor is over: forget what it told us */
static void peer_down(bstate *s, peer *p, const char *why)
{
    if (p->est >= 0) {
        nl_log(s->n, NL_L_NET, 1, 0, T_ERR "BGP" T_RESET " %s %s (AS %u) %s: %s", _("session with"), p->name, p->as,
               _("is down"), why);
        nl_count(s->n, "bgp.session_drops", 1);
    }
    p->est = -1;
    p->tried = 0;
    p->next_try = nl_now() + s->retry;  /* stay Idle for a while before trying again */
    memset(p->in, 0, sizeof(p->in));
    memset(p->out, 0, sizeof(p->out));
    bgp_schedule(s);
}

/* A connection is over: closed or reset by the other side, timed out, or
 * given up by us (code != 0: after telling the neighbor why). */
static void conn_end(bconn *bc, int code, int sub, const char *why, int hard)
{
    bstate *s = bc->s;
    peer *p = &s->p[bc->peer];
    int was_session = p->est == bc->dir, was = bc->st;

    conn_drop(bc, code, sub, hard);
    if (was_session) {
        peer_down(s, p, why);
    } else if (!p->conn[!bc->dir].c) {
        if (was >= BS_OPENSENT)
            nl_log(s->n, NL_L_NET, 2, 0, T_WARN "BGP" T_RESET " %s %s: %s", _("connection with"), p->name, why);
        p->tried = 1;
        p->next_try = nl_now() + s->retry;
    }
}

static void conn_lost(bconn *bc, const char *why, int hard)
{
    conn_end(bc, 0, 0, why, hard);
}

static void conn_error(bconn *bc, int code, int sub, const char *why)
{
    conn_end(bc, code, sub, why, 0);
}

/* RPKI-style origin validation: 0 = unknown, 1 = valid, -1 = invalid */
static int rov_check(const bstate *s, nl_ip pfx, int plen, uint32_t origin)
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

/* Returns 0, or the UPDATE error subcode */
static int rx_update(bstate *s, peer *p, int as4, const uint8_t *b, int len)
{
    uint32_t path[MAX_PATH];
    int wlen, alen, off, npath = 0, i, used, plen, w = as4 ? 4 : 2;
    nl_ip pfx, nh = p->addr;

    if (len < 4)
        return 1;
    p->rx_updates++;
    nl_count(s->n, "bgp.updates_rcvd", 1);
    wlen = nl_get16(b);
    if (2 + wlen + 2 > len)
        return 1;                       /* malformed attribute list */
    for (off = 2; off < 2 + wlen; off += used) {
        used = get_prefix(b + off, 2 + wlen - off, &pfx, &plen);
        if (used < 0)
            return 10;                  /* invalid network field */
        nl_log(s->n, NL_L_NET, 2, 0, T_WARN "BGP" T_RESET " ← %s: withdraw %s/%d", p->name, nl_ip_str(pfx), plen);
        for (i = 0; i < MAX_RIB; i++)
            if (p->in[i].used && p->in[i].pfx == pfx && p->in[i].plen == plen)
                p->in[i].used = 0;
    }
    alen = nl_get16(b + off);
    off += 2;
    if (off + alen > len)
        return 1;
    for (i = off; i + 3 <= off + alen; ) {
        int flags = b[i], type = b[i + 1], hl = flags & 0x10 ? 4 : 3, l, k;
        const uint8_t *v;

        if (i + hl > off + alen)
            return 5;                   /* attribute length error */
        l = hl == 4 ? nl_get16(b + i + 2) : b[i + 2];
        v = b + i + hl;
        if (i + hl + l > off + alen)
            return 5;
        if (type == 2) {                /* AS_PATH: one or more segments */
            int q = 0;
            while (q + 2 <= l) {
                int cnt = v[q + 1];
                if (q + 2 + cnt * w > l)
                    return 11;          /* malformed AS_PATH */
                for (k = 0; k < cnt && npath < MAX_PATH; k++)
                    path[npath++] = w == 4 ? nl_get32(v + q + 2 + k * 4) : nl_get16(v + q + 2 + k * 2);
                q += 2 + cnt * w;
            }
        } else if (type == 3 && l == 4) {
            nh = nl_get32(v);
        }
        i += hl + l;
    }
    for (off += alen; off < len; off += used) {
        rib_in *r = NULL, *slot = NULL;
        int rv;

        used = get_prefix(b + off, len - off, &pfx, &plen);
        if (used < 0)
            return 10;
        for (i = 0; i < MAX_RIB; i++) {
            if (p->in[i].used && p->in[i].pfx == pfx && p->in[i].plen == plen)
                r = &p->in[i];
            if (!p->in[i].used && !slot)
                slot = &p->in[i];
        }
        nl_log(s->n, NL_L_NET, 2, 0, T_OK "BGP" T_RESET " ← %s: %s/%d AS path %s", p->name, nl_ip_str(pfx), plen,
               path_str(path, npath));
        if (!npath || path[0] != p->as)
            return 11;                  /* an eBGP neighbor puts its own AS first (RFC 4271 6.3) */
        for (i = 0; i < npath; i++)
            if (path[i] == s->as)
                break;
        if (i < npath) {
            nl_log(s->n, NL_L_NET, 2, 0, T_WARN "BGP" T_RESET " %s/%d: %s", nl_ip_str(pfx), plen,
                   _("our own AS is in the path: loop, rejected"));
            nl_count(s->n, "bgp.loop_rejected", 1);
            if (r)
                r->used = 0;
            continue;
        }
        rv = s->rov ? rov_check(s, pfx, plen, path[npath - 1]) : 0;
        if (rv < 0) {
            nl_log(s->n, NL_L_NET, 1, 0, T_ERR "BGP" T_RESET " %s/%d %s AS %u: %s", nl_ip_str(pfx), plen,
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
    return 0;
}

static void rx_open(bconn *bc, const uint8_t *b, int len)
{
    bstate *s = bc->s;
    peer *p = &s->p[bc->peer];
    bconn *other = &p->conn[!bc->dir];
    uint32_t as;
    nl_time hold;
    int optlen, q, as4 = 0;

    if (bc->st != BS_OPENSENT) {
        conn_error(bc, E_FSM, 0, _("OPEN received in the wrong state"));
        return;
    }
    if (len < BHDR + 10 || BHDR + 10 + b[BHDR + 9] > len) {
        conn_error(bc, E_HEADER, 2, _("malformed OPEN"));
        return;
    }
    if (b[BHDR] != 4) {
        conn_error(bc, E_OPEN, 1, _("unsupported BGP version"));
        return;
    }
    as = nl_get16(b + BHDR + 1);
    optlen = b[BHDR + 9];
    for (q = BHDR + 10; q + 2 <= BHDR + 10 + optlen; q += 2 + b[q + 1]) {
        int k, end = q + 2 + b[q + 1];

        if (b[q] != 2 || end > BHDR + 10 + optlen)
            continue;
        for (k = q + 2; k + 2 <= end; k += 2 + b[k + 1])
            if (b[k] == 65 && b[k + 1] == 4 && k + 6 <= end) {
                as = nl_get32(b + k + 2);
                as4 = 1;
            }
    }
    if (as != p->as) {
        nl_log(s->n, NL_L_NET, 1, 0, T_ERR "BGP" T_RESET " OPEN %s %s: AS %u, %s %u — %s", _("from"), p->name, as,
               _("expected"), p->as, _("session refused"));
        nl_count(s->n, "bgp.open_errors", 1);
        conn_error(bc, E_OPEN, 2, _("wrong AS number in the OPEN"));
        return;
    }
    hold = NL_SEC(nl_get16(b + BHDR + 3));
    if (hold && hold < NL_SEC(3)) {
        conn_error(bc, E_OPEN, 6, _("unacceptable hold time"));
        return;
    }
    p->id = nl_get32(b + BHDR + 5);
    bc->as4 = as4;
    bc->hold = hold < s->hold ? hold : s->hold;         /* the smaller of the two proposals */
    nl_log(s->n, NL_L_NET, 2, 0, T_OK "BGP" T_RESET " ← %s: OPEN (AS %u, id %s, hold %s)", p->name, as,
           nl_ip_str(p->id), nl_time_str(hold));

    /* Connection collision (RFC 4271 6.8): two connections with the same
     * neighbor. The one opened by the router with the higher identifier
     * survives; both ends apply the same rule and keep the same one. */
    if (other->c && other->st >= BS_OPENSENT) {
        bconn *lose = other->st == BS_ESTABLISHED ? bc : &p->conn[s->id > p->id ? 1 : 0];

        nl_log(s->n, NL_L_NET, 2, 0, T_WARN "BGP" T_RESET " %s %s: %s %s", _("connection collision with"), p->name,
               _("keeping the connection opened by"), lose->dir == 0 ? p->name : s->n->name);
        nl_count(s->n, "bgp.collisions", 1);
        conn_drop(lose, E_CEASE, 7, 0);
        if (lose == bc)
            return;
    }
    send_keepalive(bc);
    bc->st = BS_OPENCONFIRM;
}

/* One whole message from the stream */
static void bgp_message(bconn *bc, const uint8_t *b, int len)
{
    bstate *s = bc->s;
    peer *p = &s->p[bc->peer];
    int err;

    bc->last_rx = nl_now();
    switch (b[18]) {
    case M_OPEN:
        rx_open(bc, b, len);
        break;
    case M_KEEPALIVE:
        if (len != BHDR) {
            conn_error(bc, E_HEADER, 2, _("malformed KEEPALIVE"));
        } else if (bc->st == BS_OPENCONFIRM) {
            bconn *other = &p->conn[!bc->dir];

            if (other->c)
                conn_drop(other, E_CEASE, 7, 0);
            bc->st = BS_ESTABLISHED;
            p->est = bc->dir;
            p->tried = 0;
            p->since = nl_now();
            nl_log(s->n, NL_L_NET, 1, 0, T_OK "BGP" T_RESET " %s %s (%s) %s", _("session with"), p->name,
                   peer_tag(s, p), _("Established: exchanging routes"));
            bgp_schedule(s);
        } else if (bc->st != BS_ESTABLISHED) {
            conn_error(bc, E_FSM, 0, _("KEEPALIVE received in the wrong state"));
        }
        break;
    case M_UPDATE:
        if (bc->st != BS_ESTABLISHED)
            conn_error(bc, E_FSM, 0, _("UPDATE received before the session was established"));
        else if ((err = rx_update(s, p, bc->as4, b + BHDR, len - BHDR)) != 0)
            conn_error(bc, E_UPDATE, err, _("malformed UPDATE"));
        break;
    case M_NOTIFICATION: {
        int code = len > BHDR ? b[BHDR] : 0, sub = len > BHDR + 1 ? b[BHDR + 1] : 0;
        char why[120];

        nl_log(s->n, NL_L_NET, 2, 0, T_ERR "BGP" T_RESET " ← %s: NOTIFICATION %d/%d (%s)", p->name, code, sub,
               notif_name(code, sub));
        snprintf(why, sizeof(why), "%s (%s)", _("the neighbor closed it"), notif_name(code, sub));
        conn_lost(bc, why, 0);
        break;
    }
    default:
        conn_error(bc, E_HEADER, 3, _("unknown message type"));
        break;
    }
}

/* ---- TCP callbacks ---- */

static void on_tcp_connect(nl_tcp *c, void *ctx)
{
    bconn *bc = ctx;
    bstate *s;
    peer *p;

    if (!bc || bc->c != c)
        return;
    s = bc->s;
    p = &s->p[bc->peer];
    if (p->est >= 0) {                  /* the other connection made it first */
        conn_drop(bc, 0, 0, 0);
        return;
    }
    bc->st = BS_OPENSENT;
    bc->last_rx = nl_now();
    nl_log(s->n, NL_L_NET, 2, 0, T_OK "BGP" T_RESET " → %s: OPEN (AS %u, id %s, hold %s) — %s", p->name, s->as,
           nl_ip_str(s->id), nl_time_str(s->hold),
           bc->dir ? _("on the connection the neighbor opened") : _("on the connection we opened"));
    send_open(bc);
}

static void *on_tcp_accept(nl_tcp *c, void *lctx)
{
    bstate *s = lctx;
    nl_ip rip;
    uint16_t rport;
    bconn *bc;
    int k;

    nl_tcp_peer(c, &rip, &rport);
    for (k = 0; k < s->np; k++)
        if (s->p[k].addr == rip)
            break;
    if (k == s->np) {
        nl_log(s->n, NL_L_NET, 2, 0, T_WARN "BGP" T_RESET " %s %s: %s", _("connection from"), nl_ip_str(rip),
               _("not a configured neighbor, refused"));
        nl_tcp_abort(c);
        return NULL;
    }
    bc = &s->p[k].conn[1];
    if (bc->c) {
        if (bc->st == BS_ESTABLISHED) { /* a session is already up on the previous one */
            nl_tcp_abort(c);
            return NULL;
        }
        conn_drop(bc, 0, 0, 1);
    }
    bc->c = c;
    bc->st = BS_CONNECT;
    bc->t_open = nl_now();
    return bc;
}

static void on_tcp_data(nl_tcp *c, void *ctx, const uint8_t *d, int len)
{
    bconn *bc = ctx;

    while (bc && bc->c == c && len > 0) {
        int take = BGP_MAXMSG - bc->len < len ? BGP_MAXMSG - bc->len : len, mlen, i;

        memcpy(bc->buf + bc->len, d, (size_t)take);
        bc->len += take;
        d += take;
        len -= take;
        /* messages are delimited only by their length field */
        while (bc->c == c && bc->len >= BHDR) {
            uint8_t msg[BGP_MAXMSG];

            for (i = 0; i < 16 && bc->buf[i] == 0xff; i++)
                ;
            mlen = nl_get16(bc->buf + 16);
            if (i < 16 || mlen < BHDR || mlen > BGP_MAXMSG) {
                conn_error(bc, E_HEADER, i < 16 ? 1 : 2, _("the byte stream is not a sequence of BGP messages"));
                return;
            }
            if (bc->len < mlen)
                break;
            memcpy(msg, bc->buf, (size_t)mlen);
            memmove(bc->buf, bc->buf + mlen, (size_t)(bc->len - mlen));
            bc->len -= mlen;
            bgp_message(bc, msg, mlen);
        }
    }
}

static void on_tcp_eof(nl_tcp *c, void *ctx)
{
    bconn *bc = ctx;

    if (bc && bc->c == c)
        conn_lost(bc, _("the neighbor closed the TCP connection"), 0);
}

static void on_tcp_close(nl_tcp *c, void *ctx, int reason)
{
    bconn *bc = ctx;

    if (!bc || bc->c != c)
        return;
    bc->c = NULL;                       /* already gone: nothing to close */
    conn_lost(bc, reason == NL_TCP_CLOSED_RST ? _("TCP connection reset")
              : reason == NL_TCP_CLOSED_REFUSED ? _("TCP connection refused")
              : reason == NL_TCP_CLOSED_TIMEOUT ? _("TCP connection timed out") : _("TCP connection closed"), 0);
}

static const nl_tcp_ops bgp_tcp_ops = {
    .on_connect = on_tcp_connect, .on_accept = on_tcp_accept, .on_data = on_tcp_data, .on_eof = on_tcp_eof,
    .on_close = on_tcp_close,
};

/* ---- timers ---- */

static void bgp_tick(void *ctx)
{
    bstate *s = ctx;
    nl_time now = nl_now();
    int k, d;

    for (k = 0; k < s->np; k++) {
        peer *p = &s->p[k];

        if (!nl_if_usable(s->n, p->ifx)) {
            /* the neighbor is directly connected: no interface, no session */
            if (p->conn[0].c || p->conn[1].c || p->est >= 0) {
                conn_drop(&p->conn[0], 0, 0, 1);
                conn_drop(&p->conn[1], 0, 0, 1);
                peer_down(s, p, _("interface down"));
            }
            p->tried = 0;
            p->next_try = 0;
            continue;
        }
        for (d = 0; d < 2; d++) {
            bconn *bc = &p->conn[d];
            nl_time hold = bc->st >= BS_OPENCONFIRM ? bc->hold : s->hold, ka = hold / 3;

            if (!bc->c)
                continue;
            if (ka > s->keepalive || !hold)
                ka = s->keepalive;
            if (bc->st == BS_CONNECT) {
                if (now - bc->t_open >= s->retry) {     /* ConnectRetry timer */
                    nl_log(s->n, NL_L_NET, 2, 0, T_WARN "BGP" T_RESET " %s: %s", p->name,
                           _("no TCP connection yet: ConnectRetry timer expired, trying again"));
                    conn_lost(bc, "", 1);
                    p->next_try = now;
                }
            } else if (hold && now - bc->last_rx > hold) {
                conn_error(bc, E_HOLD, 0, _("hold timer expired: nothing heard"));
            } else if (bc->st >= BS_OPENCONFIRM && hold && now - bc->last_tx >= ka) {
                send_keepalive(bc);
            }
        }
        /* Idle or Active with nothing in progress: open a connection */
        if (p->est < 0 && !p->conn[0].c && !p->conn[1].c && now >= p->next_try) {
            bconn *bc = &p->conn[0];

            bc->c = nl_tcp_connect(s->n, p->addr, BGP_PORT, &bgp_tcp_ops, bc, NULL);
            bc->t_open = now;
            if (bc->c) {
                bc->st = BS_CONNECT;
            } else {
                p->tried = 1;
                p->next_try = now + s->retry;
            }
        }
    }
    if (s->connected)
        bgp_schedule(s);                /* connected networks may have changed */
    s->tick_ev = nl_after(s->n, s->keepalive / 3 > NL_MS(500) ? NL_MS(500) : s->keepalive / 3, bgp_tick, s);
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
    p->as = (uint32_t)strtoul(as, NULL, 10);
    p->est = -1;
    p->prepend = s->prepend;
    for (i = 0; i < 2; i++) {
        p->conn[i].s = s;
        p->conn[i].peer = s->np;
        p->conn[i].dir = i;
    }
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
    /* `withdraw` also silences a network that connected=1 would originate */
    for (i = 0; i < s->nsup; i++)
        if (s->sup[i].pfx == pfx && s->sup[i].plen == plen)
            break;
    if (on && i < s->nsup) {
        s->sup[i] = s->sup[--s->nsup];
    } else if (!on && i == s->nsup && s->nsup < MAX_NET) {
        s->sup[s->nsup].pfx = pfx;
        s->sup[s->nsup].plen = plen;
        s->nsup++;
    }
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
    s->as = (uint32_t)strtoul(nl_arg_str(a, "as", "0"), NULL, 10);
    s->keepalive = nl_arg_time(a, "keepalive", NL_SEC(30));
    if (s->keepalive < NL_SEC(1))
        s->keepalive = NL_SEC(1);
    s->hold = nl_arg_time(a, "hold", s->keepalive * 3);
    s->hold -= s->hold % NL_SEC(1);     /* the OPEN carries whole seconds... */
    if (s->hold < NL_SEC(3))
        s->hold = NL_SEC(3);            /* ...and at least three (RFC 4271 4.2) */
    if (s->hold > NL_SEC(65535))
        s->hold = NL_SEC(65535);
    s->retry = nl_arg_time(a, "retry", s->keepalive);
    if (s->retry < NL_MS(500))
        s->retry = NL_MS(500);
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
                    s->roa[s->nroa].as = (uint32_t)strtoul(as, NULL, 10);
                    s->roa[s->nroa].maxlen = ml ? atoi(ml) : s->roa[s->nroa].plen;
                    s->nroa++;
                }
            }
        }
    }
    for (i = 0; i < s->np; i++) {       /* prepend.NEIGHBOR=N: only towards that neighbor */
        char key[32];

        snprintf(key, sizeof(key), "prepend.%s", s->p[i].name);
        s->p[i].prepend = (int)nl_arg_int(a, key, s->prepend);
        if (s->p[i].prepend < 0)
            s->p[i].prepend = 0;
    }
    n->forward = 1;
    nl_mod_set(n, &bgp_mod, s);
    if (nl_tcp_listen(n, BGP_PORT, &bgp_tcp_ops, s, NULL) != 0) {
        nl_log(n, NL_L_NET, 1, 0, T_ERR "bgp: %s", _("cannot listen on TCP port 179"));
        nl_mod_set(n, &bgp_mod, NULL);
        free(s);
        return -1;
    }
    nl_log(n, NL_L_NET, 2, 0, T_OK "BGP" T_RESET " AS %u, id %s: %d %s, %d %s", s->as, nl_ip_str(s->id), s->np,
           _("neighbors"), s->nnet, _("networks originated"));
    s->tick_ev = nl_after(n, (nl_time)nl_rand(100000), bgp_tick, s);
    bgp_schedule(s);
    return 0;
}

/* Forget every session. close: 0 = the TCP connections are already gone
 * (the node crashed), 1 = tell the neighbors and close them. */
static void bgp_reset(bstate *s, int close)
{
    int k, d;

    nl_cancel(&s->tick_ev);
    nl_cancel(&s->proc_ev);
    for (k = 0; k < s->np; k++) {
        peer *p = &s->p[k];

        for (d = 0; d < 2; d++) {
            if (close)
                conn_drop(&p->conn[d], E_CEASE, 2, 0);
            p->conn[d].c = NULL;
            p->conn[d].st = 0;
            p->conn[d].len = 0;
        }
        p->est = -1;
        p->tried = 0;
        p->next_try = 0;
        memset(p->in, 0, sizeof(p->in));
        memset(p->out, 0, sizeof(p->out));
    }
    memset(s->loc, 0, sizeof(s->loc));
    nl_route_flush(s->n, NL_RT_BGP);
}

static void bgp_stop(nl_node *n)
{
    bstate *s = nl_mod_get(n, &bgp_mod);

    if (!s)
        return;
    bgp_reset(s, 1);
    nl_tcp_unlisten(n, BGP_PORT);
    nl_mod_set(n, &bgp_mod, NULL);
    free(s);
}

/* A crash takes the TCP connections with it, silently: the neighbors find
 * out when their hold timer expires or when a keepalive is answered by a
 * reset from the restarted router. */
static void bgp_crash(nl_node *n) { bgp_reset(nl_mod_get(n, &bgp_mod), 0); }

static void bgp_recover(nl_node *n)
{
    bstate *s = nl_mod_get(n, &bgp_mod);

    nl_tcp_listen(n, BGP_PORT, &bgp_tcp_ops, s, NULL);
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
        tui_print(x, y + row++, w, TS(p->est >= 0 ? C_OK : C_WARN, C_DEFAULT, 0),
                  "%-8s %-6u %-9s %-12s %u/%u", p->name, p->as, REL_NAME[p->rel], ST_NAME[peer_state(p)],
                  p->rx_updates, p->tx_updates);
        if (detail >= 2 && p->est >= 0 && row < h) {
            const bconn *bc = &p->conn[p->est];
            tui_print(x, y + row++, w, TS(C_DIM, C_DEFAULT, 0), "         TCP %s · %s %s",
                      bc->dir ? _("opened by the neighbor") : _("opened by us"), _("hold"), nl_time_str(bc->hold));
        }
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
                "               [connected=1] [keepalive=30s] [hold=3×keepalive] [retry=keepalive]\n"
                "               [mrai=500ms] [prepend=N] [prepend.NEIGHBOR=N] [policy=1]\n"
                "               [rov=1 roa=PREFIX/LEN:AS[:MAXLEN] ...]\n"
                "run ROUTER bgp announce|withdraw PREFIX/LEN\n"
                "  network= and neighbor= can be repeated. RELATION says what the neighbor is for\n"
                "  this AS: customer, peer or provider (none: no policy on that session).\n"
                "  connected=1 also originates every connected network; `withdraw` silences one.\n"
                "  policy=0 ignores the relations. prepend=N lengthens the AS path we export to\n"
                "  everybody, prepend.NEIGHBOR=N only towards that neighbor.\n"
                "  Nothing checks that an AS owns what it announces: that is the hijack lab;\n"
                "  rov=1 with roa= entries rejects announcements with the wrong origin.\n"
                "  Sessions run over TCP port 179. keepalive is at least 1 s and hold at least 3 s;\n"
                "  retry is the ConnectRetry timer. AS numbers may be four octets long."),
    .theory = N_("The Internet is a network of autonomous systems (AS): networks under one administration, "
                 "each with its own interior routing. BGP is the protocol they use to tell each other which "
                 "address blocks (prefixes) they can reach. It is a path-vector protocol: an announcement "
                 "carries the whole list of ASes it has crossed (the AS path). When a router passes a route "
                 "on, it prepends its own AS number; a router that finds its own number in a path discards "
                 "it, which prevents loops without any count-to-infinity. When a route disappears an explicit "
                 "withdrawal is sent.\n\n"
                 "Two BGP routers talk over a TCP connection to port 179, so BGP itself needs no "
                 "acknowledgements or retransmissions: a route is sent once and stays valid until it is "
                 "withdrawn or the session ends. Either router may open the connection (if both do, the one "
                 "opened by the router with the higher identifier is kept). Each sends an OPEN with its AS "
                 "number, identifier and proposed hold time, confirms the other's with a KEEPALIVE, and the "
                 "session is Established; then the whole table is sent as UPDATE messages, and afterwards "
                 "only the changes. KEEPALIVEs every third of the hold time prove the neighbor is alive; "
                 "when the hold timer expires, or the connection breaks, everything learned over the session "
                 "is withdrawn. Errors are reported with a NOTIFICATION, which also closes the session.\n\n"
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
                 "Not modelled here: iBGP (there is one router per AS and neighbors are directly connected), "
                 "MED, communities, aggregation and route-flap damping; each UPDATE carries one prefix."),
    .run = bgp_run, .stop = bgp_stop, .crash = bgp_crash, .recover = bgp_recover, .panel = bgp_panel,
};
NL_MODULE(bgp_mod)

/* One message; returns its length. brief: fields only, the summary is the caller's */
static int dis_bgp_msg(nl_dissect *o, const uint8_t *d, int len, int off, char *sum, size_t sumsz)
{
    int type, mlen, wlen, alen, p, used, plen, i;
    nl_ip pfx;
    char path[160] = "", first[40] = "";
    size_t pl = 0;

    mlen = nl_get16(d + 16);
    type = d[18];
    if (mlen < BHDR)
        mlen = BHDR;
    if (mlen > len) {
        snprintf(sum, sumsz, "%s (%d of %d bytes: continues in the next segment)",
                 type == M_UPDATE ? "UPDATE" : type == M_OPEN ? "OPEN" : "message", len, mlen);
        mlen = len;
        nl_dis_field(o, "BGP", "Marker", off, 16, "16 × 0xff");
        return mlen;
    }
    nl_dis_field(o, "BGP", "Marker", off, 16, "16 × 0xff");
    nl_dis_field(o, "BGP", "Length", off + 16, 2, "%d", mlen);
    nl_dis_field(o, "BGP", "Type", off + 18, 1, "%d (%s)", type,
                 type == M_OPEN ? "OPEN" : type == M_UPDATE ? "UPDATE" : type == M_KEEPALIVE ? "KEEPALIVE"
                 : type == M_NOTIFICATION ? "NOTIFICATION" : "?");
    if (type == M_OPEN && mlen >= BHDR + 10) {
        uint32_t as = nl_get16(d + BHDR + 1);
        int optlen = d[BHDR + 9], q, k;

        nl_dis_field(o, "BGP", "Version", off + BHDR, 1, "%d", d[BHDR]);
        nl_dis_field(o, "BGP", "My AS", off + BHDR + 1, 2, "%u%s", as, as == AS_TRANS ? " (AS_TRANS)" : "");
        nl_dis_field(o, "BGP", "Hold time", off + BHDR + 3, 2, "%u s", nl_get16(d + BHDR + 3));
        nl_dis_field(o, "BGP", "BGP identifier", off + BHDR + 5, 4, "%s", nl_ip_str(nl_get32(d + BHDR + 5)));
        nl_dis_field(o, "BGP", "Optional parameters", off + BHDR + 9, 1, "%d bytes", optlen);
        for (q = BHDR + 10; q + 2 <= BHDR + 10 + optlen && q + 2 + d[q + 1] <= mlen; q += 2 + d[q + 1])
            for (k = q + 2; d[q] == 2 && k + 2 <= q + 2 + d[q + 1]; k += 2 + d[k + 1])
                if (d[k] == 65 && d[k + 1] == 4 && k + 6 <= mlen) {
                    as = nl_get32(d + k + 2);
                    nl_dis_field(o, "BGP", "Capability", off + k, 6, "four-octet AS numbers: AS %u", as);
                } else {
                    nl_dis_field(o, "BGP", "Capability", off + k, 2 + d[k + 1], "code %d", d[k]);
                }
        snprintf(sum, sumsz, "OPEN AS %u hold %us id %s", as, nl_get16(d + BHDR + 3),
                 nl_ip_str(nl_get32(d + BHDR + 5)));
    } else if (type == M_KEEPALIVE) {
        snprintf(sum, sumsz, "KEEPALIVE");
    } else if (type == M_NOTIFICATION && mlen >= BHDR + 2) {
        nl_dis_field(o, "BGP", "Error code / subcode", off + BHDR, 2, "%d / %d (%s)", d[BHDR], d[BHDR + 1],
                     notif_name(d[BHDR], d[BHDR + 1]));
        snprintf(sum, sumsz, "NOTIFICATION %d/%d (%s)", d[BHDR], d[BHDR + 1], notif_name(d[BHDR], d[BHDR + 1]));
    } else if (type == M_UPDATE && mlen >= BHDR + 4) {
        int nw = 0, nn = 0;

        wlen = nl_get16(d + BHDR);
        nl_dis_field(o, "BGP", "Withdrawn length", off + BHDR, 2, "%d", wlen);
        for (p = BHDR + 2; p < BHDR + 2 + wlen && p < mlen; p += used) {
            used = get_prefix(d + p, mlen - p, &pfx, &plen);
            if (used < 0)
                return mlen;
            nl_dis_field(o, "BGP", "Withdrawn route", off + p, used, "%s/%d", nl_ip_str(pfx), plen);
            if (!nw++)
                snprintf(first, sizeof(first), "%s/%d", nl_ip_str(pfx), plen);
        }
        if (p + 2 > mlen)
            return mlen;
        alen = nl_get16(d + p);
        nl_dis_field(o, "BGP", "Attributes length", off + p, 2, "%d", alen);
        p += 2;
        for (i = p; i + 3 <= p + alen && i + 3 <= mlen; ) {
            int hl = d[i] & 0x10 ? 4 : 3, l = hl == 4 ? nl_get16(d + i + 2) : d[i + 2], k;
            const uint8_t *v = d + i + hl;

            if (i + hl + l > mlen)
                break;
            if (d[i + 1] == 1 && l >= 1) {
                nl_dis_field(o, "BGP", "ORIGIN", off + i, hl + l, "%s", v[0] == 0 ? "IGP" : v[0] == 1 ? "EGP" : "incomplete");
            } else if (d[i + 1] == 2 && l >= 2) {
                /* two or four octets per AS: tell from the segment length */
                int w = v[1] && (l - 2) / v[1] >= 4 ? 4 : 2;

                for (k = 0; k < v[1] && 2 + (k + 1) * w <= l && pl < sizeof(path) - 12; k++)
                    pl += (size_t)snprintf(path + pl, sizeof(path) - pl, "%s%u", k ? " " : "",
                                           w == 4 ? nl_get32(v + 2 + k * 4) : nl_get16(v + 2 + k * 2));
                nl_dis_field(o, "BGP", "AS_PATH", off + i, hl + l, "AS_SEQUENCE: %s", path);
            } else if (d[i + 1] == 3 && l == 4) {
                nl_dis_field(o, "BGP", "NEXT_HOP", off + i, hl + l, "%s", nl_ip_str(nl_get32(v)));
            } else {
                nl_dis_field(o, "BGP", "Attribute", off + i, hl + l, "type %d", d[i + 1]);
            }
            i += hl + l;
        }
        for (p += alen; p < mlen; p += used) {
            used = get_prefix(d + p, mlen - p, &pfx, &plen);
            if (used < 0)
                return mlen;
            nl_dis_field(o, "BGP", "NLRI (announced)", off + p, used, "%s/%d", nl_ip_str(pfx), plen);
            if (!nn++)
                snprintf(first, sizeof(first), "%s/%d", nl_ip_str(pfx), plen);
        }
        if (nn)
            snprintf(sum, sumsz, "UPDATE %s AS path %s", first, path);
        else
            snprintf(sum, sumsz, "UPDATE withdraw %s", first);
    } else {
        snprintf(sum, sumsz, "message type %d", type);
    }
    return mlen;
}

/* The payload of one TCP segment: usually one message, sometimes several,
 * or the middle of one that started in an earlier segment */
static void dis_bgp(nl_dissect *o, const uint8_t *d, int len, int off)
{
    char sum[200] = "", one[150];
    int pos = 0, n = 0, i;
    size_t sl = 0;

    while (len - pos >= BHDR) {
        for (i = 0; i < 16 && d[pos + i] == 0xff; i++)
            ;
        if (i < 16)
            break;
        one[0] = '\0';
        pos += dis_bgp_msg(o, d + pos, len - pos, off + pos, one, sizeof(one));
        if (n++ < 3 && sl < sizeof(sum) - 1)
            sl += (size_t)snprintf(sum + sl, sizeof(sum) - sl, "%s%s", sl ? " | " : "", one);
    }
    if (!n) {
        nl_dis_field(o, "BGP", "Data", off, len, "%d bytes of a message begun in an earlier segment", len);
        nl_dis_summary(o, "BGP", BGP_COLOR, "BGP continuation (%d bytes)", len);
    } else if (n > 3) {
        nl_dis_summary(o, "BGP", BGP_COLOR, "%s | +%d more", sum, n - 3);
    } else {
        nl_dis_summary(o, "BGP", BGP_COLOR, "%s", sum);
    }
}

NL_INIT(bgp)
{
    nl_dissector_tcp(BGP_PORT, dis_bgp);
}
