/*
 * ripng.c - RIPng (RFC 2080): distance-vector routing for IPv6 over UDP 521.
 *
 * The same protocol as RIP version 2 with IPv6 in the entries: 20-byte
 * route entries (prefix, route tag, prefix length, metric), responses sent
 * to the link-scope multicast group ff02::9 from the link-local address
 * with hop limit 255, next hops that are the neighbor's link-local address.
 * Periodic and triggered updates, timeout and garbage collection, split
 * horizon and poisoned reverse, each switchable as in the IPv4 module.
 *
 * The core delivers only the multicast groups it knows; the group ff02::9 is
 * therefore taken with a packet hook placed before the delivery decision,
 * which is what joining the group amounts to. Unicast requests and
 * responses arrive through the ordinary UDP socket.
 */
#include "nl_int.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RIPNG_PORT   521
#define RIPNG_INF    16
#define RIPNG_MAX    128
#define RIPNG_COLOR  150
#define RIPNG_PER_PKT 60            /* entries that fit comfortably in 1280 bytes */

static const nl_ip6 RIPNG_GROUP = { { 0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 9 } };

enum { RS_FREE, RS_VALID, RS_GARBAGE };

typedef struct {
    int     state;
    nl_ip6  dst;
    int     plen;
    nl_ip6  nh;                 /* link-local address of the neighbor; zero for local routes */
    int     ifx;
    int     metric;             /* hops as stored: 0 = attached here, 16 = unreachable */
    int     local;
    int     changed;
    nl_time t;
} rng_route;

typedef struct {
    nl_node  *n;
    nl_time   update, timeout, gc;
    int       split, poison, triggered, redist, jitter;
    nl_time   phase;
    rng_route r[RIPNG_MAX];
    int       if_up[NL_MAX_IF];
    nl_ev     upd_ev, tick_ev, trig_ev;
    unsigned  sent, rcvd, ntrig, counting;
} rng_state;

static const nl_module ripng_mod;

static void prefix6(nl_ip6 *o, const nl_ip6 *a, int plen)
{
    int i;

    for (i = 0; i < 16; i++) {
        int bits = plen - i * 8;
        o->b[i] = bits >= 8 ? a->b[i] : bits <= 0 ? 0 : (uint8_t)(a->b[i] & (0xff << (8 - bits)));
    }
}

/* The interface speaks IPv6: it has a link-local address that passed DAD */
static int if_ready(const nl_node *n, int ifx)
{
    int k;

    if (!nl_if_usable(n, ifx))
        return 0;
    for (k = 0; k < n->ifc[ifx].nip6; k++)
        if (nl_ip6_linklocal(&n->ifc[ifx].ip6[k].a) && n->ifc[ifx].ip6[k].state == NL_A6_OK)
            return 1;
    return 0;
}

static rng_route *rng_find(rng_state *s, const nl_ip6 *dst, int plen, int create)
{
    rng_route *slot = NULL;
    int i;

    for (i = 0; i < RIPNG_MAX; i++) {
        if (s->r[i].state != RS_FREE && s->r[i].plen == plen && nl_ip6_eq(&s->r[i].dst, dst))
            return &s->r[i];
        if (s->r[i].state == RS_FREE && !slot)
            slot = &s->r[i];
    }
    if (!create || !slot)
        return NULL;
    memset(slot, 0, sizeof(*slot));
    slot->dst = *dst;
    slot->plen = plen;
    return slot;
}

static const char *pfx_str(const nl_ip6 *dst, int plen)
{
    static char buf[4][56];
    static int k;

    k = (k + 1) % 4;
    snprintf(buf[k], sizeof(buf[k]), "%s/%d", nl_ip6_str(dst), plen);
    return buf[k];
}

/* ---- sending ---- */

static void rng_emit(rng_state *s, int ifx, const nl_ip6 *to, uint16_t dport, uint8_t *b, int ne)
{
    nl_ip6tx tx;

    b[0] = 2;                           /* response */
    b[1] = 1;                           /* version */
    b[2] = b[3] = 0;
    memset(&tx, 0, sizeof(tx));
    tx.dst = *to;
    tx.hlim = 255;
    tx.ifx = ifx;
    tx.use_if = 1;
    if (nl_udp6_send_ex(s->n, &tx, RIPNG_PORT, dport, b, 4 + ne * 20)) {
        s->sent++;
        nl_count(s->n, "ripng.updates_sent", 1);
    }
}

static void rng_send_table(rng_state *s, int ifx, int only_changed, const nl_ip6 *to, uint16_t dport)
{
    uint8_t b[4 + RIPNG_PER_PKT * 20];
    int i, ne = 0;

    for (i = 0; i <= RIPNG_MAX; i++) {
        rng_route *r = i < RIPNG_MAX ? &s->r[i] : NULL;
        int metric;
        uint8_t *e;

        if (!r || ne == RIPNG_PER_PKT) {
            if (ne)
                rng_emit(s, ifx, to, dport, b, ne);
            ne = 0;
            if (!r)
                break;
        }
        if (r->state == RS_FREE || (only_changed && !r->changed))
            continue;
        metric = r->metric + 1 > RIPNG_INF ? RIPNG_INF : r->metric + 1;
        if (r->ifx == ifx) {
            if (r->local)
                continue;               /* the neighbors on that link are attached to it too */
            if (s->split) {
                if (!s->poison)
                    continue;
                metric = RIPNG_INF;
            }
        }
        e = b + 4 + ne++ * 20;
        memcpy(e, r->dst.b, 16);
        nl_put16(e + 16, 0);            /* route tag */
        e[18] = (uint8_t)r->plen;
        e[19] = (uint8_t)metric;
    }
}

static void rng_send_all(rng_state *s, int only_changed)
{
    int i;

    for (i = 0; i < s->n->nif; i++)
        if (if_ready(s->n, i))
            rng_send_table(s, i, only_changed, &RIPNG_GROUP, RIPNG_PORT);
    for (i = 0; i < RIPNG_MAX; i++)
        s->r[i].changed = 0;
}

static void rng_request(rng_state *s, int ifx)
{
    uint8_t b[24];
    nl_ip6tx tx;

    memset(b, 0, sizeof(b));
    b[0] = 1;                           /* request: one entry, prefix ::/0, metric 16 = the whole table */
    b[1] = 1;
    b[23] = RIPNG_INF;
    memset(&tx, 0, sizeof(tx));
    tx.dst = RIPNG_GROUP;
    tx.hlim = 255;
    tx.ifx = ifx;
    tx.use_if = 1;
    nl_udp6_send_ex(s->n, &tx, RIPNG_PORT, RIPNG_PORT, b, sizeof(b));
}

static void rng_trig_fire(void *ctx)
{
    rng_state *s = ctx;

    s->trig_ev = 0;
    s->ntrig++;
    nl_count(s->n, "ripng.triggered", 1);
    nl_log(s->n, NL_L_NET, 2, 0, T_OK "RIPng" T_RESET " %s", _("triggered update: only the routes that changed"));
    rng_send_all(s, 1);
}

static void rng_changed(rng_state *s)
{
    if (s->triggered && !s->trig_ev)
        s->trig_ev = nl_after(s->n, NL_MS(100) + (nl_time)nl_rand(400000), rng_trig_fire, s);
}

static void rng_periodic(void *ctx)
{
    rng_state *s = ctx;

    nl_log(s->n, NL_L_NET, 3, 0, T_OK "RIPng" T_RESET " %s", _("periodic update: the whole table to every neighbor"));
    rng_send_all(s, 0);
    s->upd_ev = nl_after(s->n, s->jitter ? s->update - s->update / 10 +
                         (nl_time)(nl_rand01() * (double)s->update / 5) : s->update, rng_periodic, s);
}

/* ---- table maintenance ---- */

static void rng_unreachable(rng_state *s, rng_route *r, const char *why)
{
    if (r->state != RS_VALID)
        return;
    nl_log(s->n, NL_L_NET, 1, 0, T_WARN "RIPng" T_RESET " %s %s (%s)", pfx_str(&r->dst, r->plen),
           _("is now unreachable: metric 16"), why);
    if (!r->local)
        nl_route6_del(s->n, &r->dst, r->plen, NL_RT_RIP);
    r->state = RS_GARBAGE;
    r->metric = RIPNG_INF;
    r->t = nl_now();
    r->changed = 1;
    rng_changed(s);
}

static void rng_local(rng_state *s, const nl_ip6 *net, int plen, int ifx)
{
    rng_route *r = rng_find(s, net, plen, 1);

    if (!r || (r->state == RS_VALID && r->local && r->ifx == ifx))
        return;
    if (r->state == RS_VALID && !r->local)
        nl_route6_del(s->n, net, plen, NL_RT_RIP);
    r->state = RS_VALID;
    r->local = 1;
    memset(&r->nh, 0, sizeof(r->nh));
    r->ifx = ifx;
    r->metric = 0;
    r->t = nl_now();
    r->changed = 1;
    rng_changed(s);
}

static void rng_tick(void *ctx)
{
    rng_state *s = ctx;
    nl_node *n = s->n;
    nl_time now = nl_now();
    int i, k, valid = 0;

    for (i = 0; i < n->nif; i++) {
        int up = if_ready(n, i);

        if (up) {
            for (k = 0; k < n->ifc[i].nip6; k++) {
                nl_ip6 net;
                if (nl_ip6_linklocal(&n->ifc[i].ip6[k].a) || n->ifc[i].ip6[k].state != NL_A6_OK)
                    continue;           /* link-local prefixes are never advertised */
                prefix6(&net, &n->ifc[i].ip6[k].a, n->ifc[i].ip6[k].plen);
                rng_local(s, &net, n->ifc[i].ip6[k].plen, i);
            }
            if (!s->if_up[i])
                rng_request(s, i);
        } else if (s->if_up[i]) {
            for (k = 0; k < RIPNG_MAX; k++)
                if (s->r[k].state == RS_VALID && s->r[k].ifx == i)
                    rng_unreachable(s, &s->r[k], _("interface down"));
        }
        s->if_up[i] = up;
    }
    if (s->redist)
        for (i = 0; i < n->nrt6; i++)
            if (n->rt6[i].proto == NL_RT_STATIC && nl_if_usable(n, n->rt6[i].ifx))
                rng_local(s, &n->rt6[i].dst, n->rt6[i].plen, n->rt6[i].ifx);
    for (i = 0; i < RIPNG_MAX; i++) {
        rng_route *r = &s->r[i];

        if (r->state == RS_VALID && !r->local && now - r->t > s->timeout)
            rng_unreachable(s, r, _("no update heard: timeout"));
        else if (r->state == RS_GARBAGE && now - r->t > s->gc)
            r->state = RS_FREE;
        if (r->state == RS_VALID)
            valid++;
    }
    nl_count_set(n, "ripng.routes", valid);
    s->tick_ev = nl_after(n, NL_SEC(1), rng_tick, s);
}

/* ---- receiving ---- */

static void rng_entry(rng_state *s, const nl_udp6msg *m, const nl_ip6 *nh, const nl_ip6 *dst, int plen, int metric)
{
    rng_route *r = rng_find(s, dst, plen, 0);

    if (!r) {
        if (metric >= RIPNG_INF || !(r = rng_find(s, dst, plen, 1)))
            return;
    } else if (r->local && r->state == RS_VALID) {
        return;
    } else if (nl_ip6_eq(&r->nh, nh) && r->ifx == m->in_if && !r->local) {
        if (metric >= RIPNG_INF) {
            rng_unreachable(s, r, _("the next hop says so"));
            return;
        }
        if (r->state == RS_VALID && metric == r->metric) {
            r->t = nl_now();
            return;
        }
        if (r->state == RS_VALID && metric > r->metric) {
            s->counting++;
            nl_count(s->n, "ripng.counting", 1);
            nl_log(s->n, NL_L_NET, 1, m->pkt_id, T_ERR "RIPng" T_RESET " %s: metric %d → %d %s %s",
                   pfx_str(dst, plen), r->metric, metric, _("via"), nl_ip6_str(nh));
        }
    } else if (metric >= r->metric) {
        return;
    }
    if (r->state != RS_VALID || !nl_ip6_eq(&r->nh, nh))
        nl_log(s->n, NL_L_NET, 1, m->pkt_id, T_OK "RIPng" T_RESET " %s %s %s (%s), %d %s", pfx_str(dst, plen),
               _("reachable via"), nl_ip6_str(nh), s->n->ifc[m->in_if].name, metric, _("hops"));
    r->state = RS_VALID;
    r->local = 0;
    r->nh = *nh;
    r->ifx = m->in_if;
    r->metric = metric;
    r->t = nl_now();
    r->changed = 1;
    nl_route6_set(s->n, dst, plen, nh, m->in_if, metric, NL_RT_RIP);
    rng_changed(s);
}

static void rng_rx(nl_node *n, void *ctx, const nl_udp6msg *m)
{
    rng_state *s = ctx;
    nl_ip6 nh;
    int i, ne;

    if (m->in_if < 0 || m->len < 4 || m->data[1] != 1 || nl_ip6_is_local(n, &m->src))
        return;
    ne = (m->len - 4) / 20;
    if (m->data[0] == 1) {
        nl_log(n, NL_L_NET, 2, m->pkt_id, T_OK "RIPng" T_RESET " %s %s", _("table requested by"),
               nl_ip6_str(&m->src));
        rng_send_table(s, m->in_if, 0, &m->src, m->sport);
        return;
    }
    /* a response is believed only from a neighbor: link-local source, port 521, hop limit 255 */
    if (m->data[0] != 2 || !nl_ip6_linklocal(&m->src) || m->sport != RIPNG_PORT || m->hlim != 255)
        return;
    s->rcvd++;
    nl_count(n, "ripng.updates_rcvd", 1);
    nl_log(n, NL_L_NET, 3, m->pkt_id, T_OK "RIPng" T_RESET " response %s %s: %d %s", _("from"),
           nl_ip6_str(&m->src), ne, _("routes"));
    nh = m->src;
    for (i = 0; i < ne; i++) {
        const uint8_t *e = m->data + 4 + i * 20;
        nl_ip6 dst, raw;
        int plen = e[18], metric = e[19];

        memcpy(raw.b, e, 16);
        if (metric == 0xff) {           /* next-hop entry: applies to the entries that follow */
            nh = nl_ip6_linklocal(&raw) ? raw : m->src;
            continue;
        }
        if (plen > 128 || metric < 1 || metric > RIPNG_INF || nl_ip6_linklocal(&raw) || nl_ip6_multicast(&raw))
            continue;
        prefix6(&dst, &raw, plen);
        rng_entry(s, m, &nh, &dst, plen, metric);
    }
}

/* Packets for ff02::9, taken before the core decides they are not ours */
static int rng_hook(nl_node *n, void *ctx, nl_pkt *p, int in_if, int out_if)
{
    const uint8_t *h = p->data + NL_ETH_HLEN, *u = h + NL_IP6_HLEN;
    nl_udp6msg m;
    int plen, ulen;

    (void)out_if;
    if (p->len < NL_ETH_HLEN + NL_IP6_HLEN + 8 || h[6] != NL_PROTO_UDP || memcmp(h + 24, RIPNG_GROUP.b, 16) != 0)
        return NL_HOOK_OK;
    plen = nl_get16(h + 4);
    ulen = nl_get16(u + 4);
    memcpy(m.src.b, h + 8, 16);
    memcpy(m.dst.b, h + 24, 16);
    if (nl_get16(u + 2) != RIPNG_PORT || ulen < 8 || ulen > plen ||
        nl_l4_cksum6(&m.src, &m.dst, NL_PROTO_UDP, u, ulen) != 0) {
        nl_count(n, "ripng.bad_packets", 1);
        return NL_HOOK_DROP;
    }
    m.sport = nl_get16(u);
    m.dport = RIPNG_PORT;
    m.data = u + 8;
    m.len = ulen - 8;
    m.in_if = in_if;
    m.hlim = h[7];
    m.pkt_id = p->id;
    rng_rx(n, ctx, &m);
    return NL_HOOK_DROP;                /* consumed: link-scope multicast is never forwarded */
}

/* ---- module ---- */

static void rng_arm(rng_state *s)
{
    s->tick_ev = nl_after(s->n, 0, rng_tick, s);
    s->upd_ev = nl_after(s->n, s->phase >= 0 ? s->phase : NL_MS(50) + (nl_time)nl_rand(200000),
                         rng_periodic, s);
}

static int rng_run(nl_node *n, const nl_args *a)
{
    rng_state *s = nl_mod_get(n, &ripng_mod);
    int fresh = !s;

    if (!s) {
        if (!nl_ip6_enabled()) {
            nl_log(n, NL_L_NET, 1, 0, T_ERR "RIPng" T_RESET " %s",
                   _("this lab does not use IPv6 (add `ipv6 on` or IPv6 addresses to the scenario)"));
            return -1;
        }
        s = calloc(1, sizeof(*s));
        if (!s)
            return -1;
        s->n = n;
        s->update = NL_SEC(30);
        s->split = s->triggered = s->jitter = 1;
        s->phase = -1;
        nl_mod_set(n, &ripng_mod, s);
        nl_udp6_bind(n, RIPNG_PORT, rng_rx, s);
        nl_hook6_add(n, NL_HOOK_PRE, rng_hook, s);
        n->forward = 1;
    }
    s->update = nl_arg_time(a, "update", s->update);
    if (s->update < NL_MS(100))
        s->update = NL_MS(100);
    s->timeout = nl_arg_time(a, "timeout", s->update * 6);
    s->gc = nl_arg_time(a, "gc", s->update * 4);
    s->split = (int)nl_arg_int(a, "split", s->split);
    s->poison = (int)nl_arg_int(a, "poison", s->poison);
    s->triggered = (int)nl_arg_int(a, "triggered", s->triggered);
    s->redist = (int)nl_arg_int(a, "redistribute", s->redist);
    s->jitter = (int)nl_arg_int(a, "jitter", s->jitter);
    s->phase = nl_arg_time(a, "phase", s->phase);
    if (fresh) {
        nl_log(n, NL_L_NET, 2, 0, T_OK "RIPng" T_RESET " %s %s, split horizon %s, poisoned reverse %s, triggered %s",
               _("started: update every"), nl_time_str(s->update), s->split ? "on" : "off",
               s->poison ? "on" : "off", s->triggered ? "on" : "off");
        rng_arm(s);
    }
    return 0;
}

static void rng_cancel(rng_state *s)
{
    nl_cancel(&s->tick_ev);
    nl_cancel(&s->upd_ev);
    nl_cancel(&s->trig_ev);
}

static void rng_stop(nl_node *n)
{
    rng_state *s = nl_mod_get(n, &ripng_mod);

    if (!s)
        return;
    rng_cancel(s);
    nl_udp6_unbind(n, RIPNG_PORT);
    nl_hook6_del(n, NL_HOOK_PRE, rng_hook, s);
    nl_route6_flush(n, NL_RT_RIP);
    nl_mod_set(n, &ripng_mod, NULL);
    free(s);
}

static void rng_crash(nl_node *n)
{
    rng_state *s = nl_mod_get(n, &ripng_mod);

    rng_cancel(s);
    memset(s->r, 0, sizeof(s->r));
    memset(s->if_up, 0, sizeof(s->if_up));
    nl_route6_flush(n, NL_RT_RIP);
}

static void rng_recover(nl_node *n)
{
    rng_arm(nl_mod_get(n, &ripng_mod));
}

static int rng_panel(nl_node *n, int x, int y, int w, int h, int detail)
{
    rng_state *s = nl_mod_get(n, &ripng_mod);
    int i, row = 0;

    row += nl_ui_kv(x, y + row, w, _("update / timeout / gc"), "%s / %s / %s", nl_time_str(s->update),
                    nl_time_str(s->timeout), nl_time_str(s->gc));
    row += nl_ui_kv(x, y + row, w, _("protections"), "split horizon %s · poisoned reverse %s · triggered %s",
                    s->split ? "on" : "off", s->poison ? "on" : "off", s->triggered ? "on" : "off");
    if (detail >= 2)
        row += nl_ui_kv(x, y + row, w, _("updates sent / heard"), "%u / %u  (%u triggered, %u metric climbs)",
                        s->sent, s->rcvd, s->ntrig, s->counting);
    if (row < h)
        tui_print(x, y + row++, w, TS(C_DIM, C_DEFAULT, 0), "%-24s %-3s %-5s %s", _("prefix"), _("m"), _("dev"),
                  _("next hop"));
    for (i = 0; i < RIPNG_MAX && row < h; i++) {
        rng_route *r = &s->r[i];

        if (r->state == RS_FREE)
            continue;
        tui_print(x, y + row++, w, TS(r->state == RS_VALID ? C_TEXT : C_ERR, C_DEFAULT, 0), "%-24s %-3d %-5s %s",
                  pfx_str(&r->dst, r->plen), r->metric, n->ifc[r->ifx].name,
                  r->local ? _("attached") : r->state == RS_GARBAGE ? _("unreachable") : nl_ip6_str(&r->nh));
    }
    return row;
}

static const nl_module ripng_mod = {
    .name = "ripng", .title = N_("RIPng — distance-vector routing for IPv6 (RFC 2080)"), .layer = NL_L_NET,
    .usage = N_("run ROUTER ripng [update=30s] [timeout=6×update] [gc=4×update]\n"
                "                 [split=1] [poison=0] [triggered=1] [redistribute=0]\n"
                "                 [phase=TIME] [jitter=1]\n"
                "  The options are those of `rip`. The lab must use IPv6 (`ipv6 on`, IPv6\n"
                "  addresses or `auto addr6`). A router may run rip and ripng side by side:\n"
                "  the two protocols share nothing but the algorithm."),
    .theory = N_("RIPng is RIP for IPv6: the same distance-vector algorithm, the same 30-second updates, "
                 "the same infinity of 16 hops, the same count-to-infinity problem and the same remedies "
                 "(split horizon, poisoned reverse, triggered updates). What changes is what IPv6 "
                 "changes. An entry carries a 128-bit prefix and its length instead of an address and a "
                 "mask. Messages travel on UDP port 521 to the link-scope multicast group ff02::9, sent "
                 "from the router's link-local address with hop limit 255, so that a receiver can tell "
                 "they come from a neighbor on the same link and were not forwarded. The next hop "
                 "installed in the routing table is that link-local address: routers on a link find each "
                 "other without any global addressing plan, and the route survives renumbering.\n\n"
                 "RIPng is a separate protocol instance, not an extension of RIPv2: a dual-stack router "
                 "runs both, with two tables that may disagree while one of them is still converging.\n\n"
                 "Here: the real message format (command, version 1, 20-byte entries, the next-hop entry "
                 "with metric 0xFF is understood on input). Authentication is left to IPsec by the RFC "
                 "and is not present; route tags are sent as zero."),
    .run = rng_run, .stop = rng_stop, .crash = rng_crash, .recover = rng_recover, .panel = rng_panel,
};
NL_MODULE(ripng_mod)

static void dis_ripng(nl_dissect *o, const uint8_t *d, int len, int off)
{
    int ne = (len - 4) / 20, i, inf = 0;

    if (len < 4)
        return;
    nl_dis_field(o, "RIPng", "Command", off, 1, "%d (%s)", d[0], d[0] == 1 ? "request" : "response");
    nl_dis_field(o, "RIPng", "Version", off + 1, 1, "%d", d[1]);
    for (i = 0; i < ne; i++) {
        const uint8_t *e = d + 4 + i * 20;
        nl_ip6 a;

        memcpy(a.b, e, 16);
        if (e[19] == 0xff) {
            nl_dis_field(o, "RIPng", "Next hop", off + 4 + i * 20, 20, "%s", nl_ip6_str(&a));
            continue;
        }
        if (e[19] >= RIPNG_INF)
            inf++;
        nl_dis_field(o, "RIPng", "Route", off + 4 + i * 20, 20, "%s/%d metric %d%s", nl_ip6_str(&a), e[18], e[19],
                     e[19] >= RIPNG_INF ? " (unreachable)" : "");
    }
    if (d[0] == 1)
        nl_dis_summary(o, "RIPng", RIPNG_COLOR, "Request: send me your table");
    else
        nl_dis_summary(o, "RIPng", RIPNG_COLOR, "Response: %d routes%s", ne, inf ? ", some unreachable (16)" : "");
}

NL_INIT(ripng)
{
    nl_dissector_udp(RIPNG_PORT, dis_ripng);
}
